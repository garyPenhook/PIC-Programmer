#include "pic18q.h"

#include "icsp.h"

/*
 * Low-voltage entry key: 32'h4D434850, "MCHP" in ASCII, most significant bit first.
 * The last bit is a don't-care but its clock is still required (spec 3.1.2).
 */
#define LVP_KEY 0x4D434850u

/* Command bytes (spec Table 3-1). Both bit 1 (reads) and bit 5 (programs) are the "J"
 * flag: 1 means the program counter advances after the operation. */
#define CMD_LOAD_PC      0x80u
#define CMD_BULK_ERASE   0x18u
#define CMD_PAGE_ERASE   0xF0u
#define CMD_READ         0xFCu    /* PC unchanged */
#define CMD_READ_INC     0xFEu    /* PC advances */
#define CMD_PROGRAM      0xC0u    /* PC unchanged */
#define CMD_PROGRAM_INC  0xE0u    /* PC advances */

/*
 * Timing, in microseconds. The spec gives minimums for the clock and entry times and
 * maximums for the NVM operations; every value below is at or beyond the stated limit.
 *
 *   spec Table 4-1 (Q24/Q41/Q43)      limit          used here
 *   clock high / low pulse            >= 100 ns      1 us each
 *   delay between command/data        >= 1.0 us      2 us
 *   mode entry hold (TENTH)           >= 1.5 ms      2 ms (Q24 is the strictest)
 *   exit delay (TEXIT)                >= 1 us        1 ms
 *   word program, flash / User ID     <= 75 us       100 us
 *   word program, EEPROM / config     <= 11 ms       12 ms
 *   page erase, bulk erase            <= 11 ms       12 ms
 */
#define T_CK_US        1u
#define T_DLY_US       2u
#define T_ENTH_US      2000u
#define T_EXIT_US      1000u
#define T_PROG_FLASH_US 100u
#define T_PROG_NVM_US  12000u
#define T_ERASE_US     12000u

/*
 * Every command is 8 bits, optionally followed by a 24-clock payload. A payload is
 *   bit 23: start bit (0), then pad zeros, then the data MSb first, then bit 0: stop bit (0)
 * so the data value sits shifted left by one. This layout is taken from the PIC16F171XX
 * programming spec figures (Figure 3-6/3-9/3-10); the PIC18-Q figures are images that
 * could not be read, so it is assumed here and should be confirmed on real silicon.
 */
#define PAYLOAD_BITS 24u

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

/* Clock in a payload from the target. We sample just before the falling edge. The
 * target changes data after the rising edge (<= 80 ns), long before that. */
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

static void load_pc(uint32_t addr)
{
    send_command(CMD_LOAD_PC);
    send_payload(addr & 0x3FFFFFu);        /* addresses are 22 bits */
}

/* Read one item at the current PC. `inc` advances the PC afterwards. */
static uint32_t read_item(int inc)
{
    send_command(inc ? CMD_READ_INC : CMD_READ);
    uint32_t raw = receive_payload();
    icsp_delay_us(T_DLY_US);
    return (raw >> 1);                     /* drop the stop bit; start/pad are above the data */
}

/* Program one item at the current PC, then wait for the write to finish. */
static void program_item(uint32_t value, int inc, uint32_t wait_us)
{
    send_command(inc ? CMD_PROGRAM_INC : CMD_PROGRAM);
    send_payload(value);
    icsp_delay_us(wait_us);
}

/* ---- address classification ---- */

typedef enum {
    REGION_NONE = 0,
    REGION_FLASH,
    REGION_USERID,
    REGION_CONFIG,
    REGION_EEPROM,
    REGION_READONLY    /* DCI, revision ID, device ID: readable, never written */
} region_t;

static region_t classify(uint32_t addr)
{
    if (addr < PIC18Q_ADDR_USERID) {
        return REGION_FLASH;
    }
    if (addr < PIC18Q_ADDR_USERID + 2u * PIC18Q_USERID_WORDS) {
        return REGION_USERID;
    }
    if (addr >= PIC18Q_ADDR_CONFIG && addr < PIC18Q_ADDR_CONFIG + PIC18Q_CONFIG_MAX) {
        return REGION_CONFIG;
    }
    if (addr >= PIC18Q_ADDR_EEPROM && addr < PIC18Q_ADDR_EEPROM + 0x10000u) {
        return REGION_EEPROM;
    }
    if (addr >= PIC18Q_ADDR_DCI) {
        return REGION_READONLY;
    }
    return REGION_NONE;
}

/* Words live in flash and User ID; bytes live in EEPROM and the configuration bytes. */
static int is_word_region(region_t r)
{
    return r == REGION_FLASH || r == REGION_USERID;
}

static int is_byte_region(region_t r)
{
    return r == REGION_CONFIG || r == REGION_EEPROM;
}

/* The DCI, IDs and the like are 16-bit words too, but read-only. */
static int readable_as_words(region_t r)
{
    return is_word_region(r) || r == REGION_READONLY;
}

/* A whole range must stay inside one region. */
static int range_ok(uint32_t addr, size_t count, size_t item_size, region_t want)
{
    if (count == 0u) {
        return 1;
    }
    uint32_t last = addr + (uint32_t)((count - 1u) * item_size);
    return classify(addr) == want && classify(last) == want;
}

/* ---- public API ---- */

void pic18q_enter(void)
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

void pic18q_exit(void)
{
    icsp_set_clk(0u);
    icsp_release_dat();
    icsp_set_mclr(1u);                     /* raise MCLR: the target leaves programming mode */
    icsp_delay_us(T_EXIT_US);
}

uint16_t pic18q_read_device_id(void)
{
    load_pc(PIC18Q_ADDR_DEVID);
    return (uint16_t)read_item(0);
}

uint16_t pic18q_read_revision_id(void)
{
    load_pc(PIC18Q_ADDR_REVID);
    return (uint16_t)read_item(0);
}

void pic18q_read_dci(pic18q_dci_t *dci)
{
    /* Five 16-bit values at 3C 0000h, 3C 0002h, ... (ERSIZ, WLSIZ, URSIZ, EESIZ, PCNT). */
    load_pc(PIC18Q_ADDR_DCI);
    dci->erase_page_words = (uint16_t)read_item(1);
    dci->write_latches    = (uint16_t)read_item(1);
    dci->user_pages       = (uint16_t)read_item(1);
    dci->eeprom_bytes     = (uint16_t)read_item(1);
    dci->pin_count        = (uint16_t)read_item(0);
}

const char *pic18q_device_name(uint16_t device_id)
{
    /* Device IDs verified in the PIC18F06/16Q41 data sheet, section 8.7.1. Other
     * families are not listed until their IDs are read from their own documents. */
    switch (device_id) {
    case 0x7540u: return "PIC18F04Q41";
    case 0x7500u: return "PIC18F05Q41";
    case 0x7580u: return "PIC18F06Q41";
    case 0x7520u: return "PIC18F14Q41";
    case 0x74E0u: return "PIC18F15Q41";
    case 0x7560u: return "PIC18F16Q41";
    default:      return NULL;
    }
}

pic18q_status_t pic18q_read_words(uint32_t addr, uint16_t *buf, size_t count)
{
    region_t r = classify(addr);
    if ((addr & 1u) != 0u || !readable_as_words(r) || !range_ok(addr, count, 2u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint16_t)read_item(i + 1u < count);   /* advance except after the last */
    }
    return PIC18Q_OK;
}

pic18q_status_t pic18q_read_bytes(uint32_t addr, uint8_t *buf, size_t count)
{
    region_t r = classify(addr);
    if (!is_byte_region(r) || !range_ok(addr, count, 1u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint8_t)read_item(i + 1u < count);
    }
    return PIC18Q_OK;
}

pic18q_status_t pic18q_bulk_erase(uint8_t regions)
{
    if (regions == 0u || (regions & ~PIC18Q_ERASE_ALL) != 0u) {
        return PIC18Q_ERR_ARG;
    }
    send_command(CMD_BULK_ERASE);
    send_payload(regions);
    icsp_delay_us(T_ERASE_US);
    return PIC18Q_OK;
}

pic18q_status_t pic18q_page_erase(uint32_t addr)
{
    region_t r = classify(addr);
    if (!is_word_region(r)) {              /* spec 3.1.3.6: program memory and User ID only */
        return PIC18Q_ERR_ARG;
    }
    load_pc(addr);                         /* the low bits of the address are ignored */
    send_command(CMD_PAGE_ERASE);
    icsp_delay_us(T_ERASE_US);
    return PIC18Q_OK;
}

pic18q_status_t pic18q_write_words(uint32_t addr, const uint16_t *words, size_t count)
{
    region_t r = classify(addr);
    if ((addr & 1u) != 0u || !is_word_region(r) || !range_ok(addr, count, 2u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        /* PC advances by 2 after each word (n = 2 for program memory, spec Table 3-1 note). */
        program_item(words[i], i + 1u < count, T_PROG_FLASH_US);
    }
    return PIC18Q_OK;
}

pic18q_status_t pic18q_write_bytes(uint32_t addr, const uint8_t *bytes, size_t count)
{
    region_t r = classify(addr);
    if (!is_byte_region(r) || !range_ok(addr, count, 1u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        /* PC advances by 1 for EEPROM and configuration bytes; these take up to 11 ms. */
        program_item(bytes[i], i + 1u < count, T_PROG_NVM_US);
    }
    return PIC18Q_OK;
}

pic18q_status_t pic18q_verify_words(uint32_t addr, const uint16_t *words, size_t count)
{
    region_t r = classify(addr);
    if ((addr & 1u) != 0u || !readable_as_words(r) || !range_ok(addr, count, 2u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        if ((uint16_t)read_item(i + 1u < count) != words[i]) {
            return PIC18Q_ERR_VERIFY;
        }
    }
    return PIC18Q_OK;
}

pic18q_status_t pic18q_verify_bytes(uint32_t addr, const uint8_t *bytes, size_t count)
{
    region_t r = classify(addr);
    if (!is_byte_region(r) || !range_ok(addr, count, 1u, r)) {
        return PIC18Q_ERR_ARG;
    }
    if (count == 0u) {
        return PIC18Q_OK;
    }
    load_pc(addr);
    for (size_t i = 0; i < count; i++) {
        if ((uint8_t)read_item(i + 1u < count) != bytes[i]) {
            return PIC18Q_ERR_VERIFY;
        }
    }
    return PIC18Q_OK;
}
