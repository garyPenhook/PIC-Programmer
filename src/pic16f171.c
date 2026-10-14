#include "pic16f171.h"

#include "icsp.h"

/*
 * Low-voltage entry key: 32'h4D434850, "MCHP" in ASCII, most significant bit first.
 * The last bit is a don't-care but its clock is still required (spec 3.1.2).
 */
#define LVP_KEY 0x4D434850u

/* Command bytes (spec Table 3-1). Bit 1 of the Load Data and Read Data commands is the
 * "J" flag: 1 means the program counter advances by one word afterwards. */
#define CMD_LOAD_PC        0x80u
#define CMD_BULK_ERASE     0x18u
#define CMD_ROW_ERASE      0xF0u
#define CMD_LOAD_DATA      0x00u    /* PC unchanged */
#define CMD_LOAD_DATA_INC  0x02u    /* PC advances */
#define CMD_READ           0xFCu    /* PC unchanged */
#define CMD_READ_INC       0xFEu    /* PC advances */
#define CMD_BEGIN_INTERNAL 0xE0u    /* commit the latched data, self-timed */

/*
 * Timing, in microseconds. The spec gives minimums for clock and entry times and maximums
 * for the NVM operations (Table 4-1); each value below is at or beyond the stated limit.
 *
 *   limit (spec Table 4-1)                       used here
 *   clock high / low pulse      >= 100 ns        1 us each
 *   delay between command/data  >= 1.0 us        2 us
 *   mode entry hold (TENTH)     >= 250 us        1 ms
 *   exit delay (TEXIT)          >= 1 us          1 ms
 *   row erase (TERAR)           <= 2.8 ms        3 ms
 *   program flash (TPINT)       <= 2.8 ms        3 ms
 *   program Config Words        <= 5.6 ms        6 ms
 *   bulk erase (TERAB)          <= 8.4 ms        9 ms
 *
 * User ID and EEPROM writes have no figure of their own in the spec, so they use the
 * longer Configuration Word time to be safe.
 */
#define T_CK_US         1u
#define T_DLY_US        2u
#define T_ENTH_US       1000u
#define T_EXIT_US       1000u
#define T_ROW_ERASE_US  3000u
#define T_PROG_FLASH_US 3000u
#define T_PROG_OTHER_US 6000u
#define T_BULK_ERASE_US 9000u

/*
 * Every command is 8 bits, optionally followed by a 24-clock payload:
 *   bit 23: start bit (0), then pad zeros, then the data MSb first, then bit 0: stop bit (0)
 * so the data value sits shifted left by one. The data field is 16 bits for Load PC
 * (Figure 3-6), 14 for Load Data to flash/config/User ID (Figure 3-9), 8 for Load Data to
 * the EEPROM (Figure 3-10) and 4 for Bulk Erase (Figure 3-7). The same layout applies when
 * the target sends the data back.
 */
#define PAYLOAD_BITS 24u

#define WORD_MASK 0x3FFFu

/* ---- bit level ---- */

/* One clock with the data line set up first. Data is latched on the falling edge. */
static void send_bit(uint8_t bit)
{
    icsp_set_dat(bit);
    icsp_set_clk(1u);
    icsp_delay_us(T_CK_US);
    icsp_set_clk(0u);
    icsp_delay_us(T_CK_US);
}

static void send_bits(uint32_t value, uint32_t count)
{
    for (uint32_t i = count; i > 0u; i--) {
        send_bit((uint8_t)((value >> (i - 1u)) & 1u));
    }
}

/* Clock in a payload from the target, sampling just before each falling edge. */
static uint32_t receive_payload(void)
{
    uint32_t raw = 0u;

    /* The target takes over ICSPDAT on the first falling edge, so let go of it now. */
    icsp_release_dat();
    icsp_delay_us(T_DLY_US);

    for (uint32_t i = 0; i < PAYLOAD_BITS; i++) {
        icsp_set_clk(1u);
        icsp_delay_us(T_CK_US);
        raw = (raw << 1) | icsp_get_dat();
        icsp_set_clk(0u);
        icsp_delay_us(T_CK_US);
    }
    return raw;
}

/* ---- commands ---- */

static void send_command(uint8_t cmd)
{
    send_bits(cmd, 8u);
    icsp_delay_us(T_DLY_US);
}

static void send_payload(uint32_t value)
{
    send_bits(value << 1, PAYLOAD_BITS);   /* value sits between the start and stop bit */
    icsp_delay_us(T_DLY_US);
}

static void load_pc(uint16_t addr)
{
    send_command(CMD_LOAD_PC);
    send_payload(addr);
}

static void load_data(uint16_t value, int inc)
{
    send_command(inc ? CMD_LOAD_DATA_INC : CMD_LOAD_DATA);
    send_payload(value);
}

/* Read the word at the PC. `inc` advances the PC by one afterwards. */
static uint32_t read_item(int inc)
{
    send_command(inc ? CMD_READ_INC : CMD_READ);
    uint32_t raw = receive_payload();
    icsp_delay_us(T_DLY_US);
    return raw >> 1;                       /* drop the stop bit */
}

/* Commit the latched data at the PC and wait for the self-timed write to finish. */
static void begin_program(uint32_t wait_us)
{
    send_command(CMD_BEGIN_INTERNAL);
    icsp_delay_us(wait_us);
}

/* ---- address classification ---- */

typedef enum {
    REGION_NONE = 0,
    REGION_FLASH,
    REGION_USERID,
    REGION_CONFIG,
    REGION_EEPROM,
    REGION_READONLY    /* revision/device ID, DIA, DCI: readable, never written */
} region_t;

static region_t classify(uint16_t addr)
{
    if (addr < PIC16F171_FLASH_LIMIT) {
        return REGION_FLASH;
    }
    if (addr >= PIC16F171_ADDR_USERID && addr < PIC16F171_ADDR_USERID + PIC16F171_USERID_WORDS) {
        return REGION_USERID;
    }
    if (addr >= PIC16F171_ADDR_CONFIG && addr < PIC16F171_ADDR_CONFIG + PIC16F171_CONFIG_WORDS) {
        return REGION_CONFIG;
    }
    if (addr >= PIC16F171_ADDR_EEPROM && addr < PIC16F171_ADDR_EEPROM + PIC16F171_EEPROM_MAX) {
        return REGION_EEPROM;
    }
    if (addr == PIC16F171_ADDR_REVID || addr == PIC16F171_ADDR_DEVID ||
        (addr >= PIC16F171_ADDR_DIA && addr < PIC16F171_ADDR_DIA + 0x40u) ||
        (addr >= PIC16F171_ADDR_DCI && addr < PIC16F171_ADDR_DCI + 0x100u)) {
        return REGION_READONLY;
    }
    return REGION_NONE;
}

/* A whole range must stay inside one region (and not wrap past 0xFFFF). */
static int range_ok(uint16_t addr, size_t count, region_t want)
{
    if (count == 0u) {
        return 1;
    }
    uint32_t last = (uint32_t)addr + (uint32_t)(count - 1u);
    return last <= 0xFFFFu && classify(addr) == want && classify((uint16_t)last) == want;
}

static int is_word_readable(region_t r)
{
    return r == REGION_FLASH || r == REGION_USERID || r == REGION_CONFIG || r == REGION_READONLY;
}

/* ---- public API ---- */

void pic16f171_enter(void)
{
    /* Both lines low before MCLR falls, so the target sees a clean start. */
    icsp_set_clk(0u);
    icsp_set_dat(0u);
    icsp_delay_us(T_DLY_US);

    icsp_set_mclr(0u);
    icsp_delay_us(T_ENTH_US);

    send_bits(LVP_KEY, 32u);               /* MSb first */
    icsp_delay_us(T_ENTH_US);
}

void pic16f171_exit(void)
{
    icsp_set_clk(0u);
    icsp_release_dat();
    icsp_set_mclr(1u);                     /* raise MCLR: the target leaves programming mode */
    icsp_delay_us(T_EXIT_US);
}

uint16_t pic16f171_read_device_id(void)
{
    load_pc(PIC16F171_ADDR_DEVID);
    return (uint16_t)(read_item(0) & WORD_MASK);
}

uint16_t pic16f171_read_revision_id(void)
{
    load_pc(PIC16F171_ADDR_REVID);
    return (uint16_t)(read_item(0) & WORD_MASK);
}

void pic16f171_read_dci(pic16f171_dci_t *dci)
{
    /* ERSIZ, WLSIZ, URSIZ, EESIZ, PCNT at 0x8200 to 0x8204 (spec Table 2-3). */
    load_pc(PIC16F171_ADDR_DCI);
    dci->erase_row_words = (uint16_t)(read_item(1) & WORD_MASK);
    dci->write_latches   = (uint16_t)(read_item(1) & WORD_MASK);
    dci->user_rows       = (uint16_t)(read_item(1) & WORD_MASK);
    dci->eeprom_bytes    = (uint16_t)(read_item(1) & WORD_MASK);
    dci->pin_count       = (uint16_t)(read_item(0) & WORD_MASK);
}

const char *pic16f171_device_name(uint16_t device_id)
{
    /* Spec section 2.6. */
    switch (device_id) {
    case 0x30DBu: return "PIC16F17114";
    case 0x30E2u: return "PIC16F17115";
    case 0x30DCu: return "PIC16F17124";
    case 0x30DEu: return "PIC16F17125";
    case 0x30E0u: return "PIC16F17126";
    case 0x30DDu: return "PIC16F17144";
    case 0x30DFu: return "PIC16F17145";
    case 0x30E1u: return "PIC16F17146";
    case 0x3101u: return "PIC16F17154";
    case 0x3103u: return "PIC16F17155";
    case 0x3105u: return "PIC16F17156";
    case 0x3102u: return "PIC16F17174";
    case 0x3104u: return "PIC16F17175";
    case 0x3106u: return "PIC16F17176";
    default:      return NULL;
    }
}

pic16f171_status_t pic16f171_read_words(uint16_t addr, uint16_t *buf, size_t count)
{
    region_t r = classify(addr);
    if (!is_word_readable(r) || !range_ok(addr, count, r)) {
        return PIC16F171_ERR_ARG;
    }
    if (count == 0u) {
        return PIC16F171_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint16_t)(read_item(i + 1u < count) & WORD_MASK);   /* advance except after the last */
    }
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_read_eeprom(uint16_t addr, uint8_t *buf, size_t count)
{
    if (!range_ok(addr, count, REGION_EEPROM)) {
        return PIC16F171_ERR_ARG;
    }
    if (count == 0u) {
        return PIC16F171_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint8_t)read_item(i + 1u < count);
    }
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_bulk_erase(uint8_t regions)
{
    if (regions == 0u || (regions & ~PIC16F171_ERASE_ALL) != 0u) {
        return PIC16F171_ERR_ARG;
    }

    /*
     * Spec 3.2.2 says the payload selects the regions to erase. The Configuration Memory
     * flowchart (Figure 3-20) also loads a PC address first ("selects Bulk Erase regions"),
     * so follow it: point the PC into the configuration area when User ID or Configuration
     * Words are being erased, and at the start of flash otherwise. This is harmless if only
     * the payload matters.
     */
    load_pc((regions & (PIC16F171_ERASE_USERID | PIC16F171_ERASE_CONFIG)) != 0u
                ? PIC16F171_ADDR_USERID
                : PIC16F171_ADDR_FLASH);
    send_command(CMD_BULK_ERASE);
    send_payload(regions);
    icsp_delay_us(T_BULK_ERASE_US);
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_row_erase(uint16_t addr)
{
    region_t r = classify(addr);
    /* Flash rows, or the User IDs (0x8000 to 0x8003; spec 3.2.3 says 0x8000 to 0x8004
     * erase only the User IDs whatever CP says). */
    if (r != REGION_FLASH && r != REGION_USERID) {
        return PIC16F171_ERR_ARG;
    }
    load_pc(addr);                         /* the low bits of the address are ignored */
    send_command(CMD_ROW_ERASE);
    icsp_delay_us(T_ROW_ERASE_US);
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_write_words(uint16_t addr, const uint16_t *words, size_t count)
{
    region_t r = classify(addr);

    if (count == 0u) {
        return PIC16F171_OK;
    }

    if (r == REGION_FLASH) {
        if ((addr % PIC16F171_ROW_WORDS) != 0u || !range_ok(addr, count, REGION_FLASH)) {
            return PIC16F171_ERR_ARG;
        }
        /* One row per cycle (Figure 3-19): fill the 32 latches, then commit. */
        for (size_t done = 0; done < count; done += PIC16F171_ROW_WORDS) {
            load_pc((uint16_t)(addr + done));
            for (uint32_t i = 0; i < PIC16F171_ROW_WORDS; i++) {
                /* Past the end of the data, load a blank word: programming 1s changes nothing. */
                uint16_t w = (done + i < count) ? (uint16_t)(words[done + i] & WORD_MASK)
                                                : PIC16F171_BLANK_WORD;
                /* Advance the PC after every latch EXCEPT the 32nd, so that it still points
                 * inside this row when the commit command arrives (the row is chosen by the
                 * PC at that moment). */
                load_data(w, i + 1u < PIC16F171_ROW_WORDS);
            }
            begin_program(T_PROG_FLASH_US);
        }
        return PIC16F171_OK;
    }

    if ((r == REGION_USERID || r == REGION_CONFIG) && range_ok(addr, count, r)) {
        /* One word per cycle (Figure 3-18). */
        for (size_t i = 0; i < count; i++) {
            load_pc((uint16_t)(addr + i));
            load_data((uint16_t)(words[i] & WORD_MASK), 0);
            begin_program(T_PROG_OTHER_US);
        }
        return PIC16F171_OK;
    }

    return PIC16F171_ERR_ARG;
}

pic16f171_status_t pic16f171_write_eeprom(uint16_t addr, const uint8_t *bytes, size_t count)
{
    if (!range_ok(addr, count, REGION_EEPROM)) {
        return PIC16F171_ERR_ARG;
    }
    /* One byte per cycle (Figure 3-18). */
    for (size_t i = 0; i < count; i++) {
        load_pc((uint16_t)(addr + i));
        load_data(bytes[i], 0);
        begin_program(T_PROG_OTHER_US);
    }
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_verify_words(uint16_t addr, const uint16_t *words, size_t count)
{
    region_t r = classify(addr);
    if (!is_word_readable(r) || !range_ok(addr, count, r)) {
        return PIC16F171_ERR_ARG;
    }
    if (count == 0u) {
        return PIC16F171_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        if ((uint16_t)(read_item(i + 1u < count) & WORD_MASK) != (words[i] & WORD_MASK)) {
            return PIC16F171_ERR_VERIFY;
        }
    }
    return PIC16F171_OK;
}

pic16f171_status_t pic16f171_verify_eeprom(uint16_t addr, const uint8_t *bytes, size_t count)
{
    if (!range_ok(addr, count, REGION_EEPROM)) {
        return PIC16F171_ERR_ARG;
    }
    if (count == 0u) {
        return PIC16F171_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        if ((uint8_t)read_item(i + 1u < count) != bytes[i]) {
            return PIC16F171_ERR_VERIFY;
        }
    }
    return PIC16F171_OK;
}
