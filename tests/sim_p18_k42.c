#include "sim_p18_k42.h"

#include <string.h>

#include "icsp.h"

/*
 * K42 model, written out from 40001886B (memory map Table 2-1, Table 3-1 commands, Table 3-2
 * Bulk Erase, Table 3-3 timing, Table B-1 device IDs, Table C-1 DCI), independently of the
 * device table so the tests cross-check it.
 *
 * Flash is written through 64 write latches (128 bytes): Load Data for NVM fills the latch
 * the PC selects, Begin Internally Timed Programming writes the whole row the PC is in and
 * resets every latch to FFFFh (section 3.1.3.5). User ID and Configuration Words take the
 * one latch their address selects.
 */
#define MAX_FLASH_WORDS  65536u
#define ERASE_WORDS      64u
#define LATCH_WORDS      64u
#define USERID_WORDS     8u
#define CONFIG_WORDS     5u
#define MAX_EEPROM_BYTES 1024u

#define KEY 0x4D434850u

/* Waits the spec requires (microseconds) between the end of one item and the next clock. */
#define W_DLY   1u
#define W_ENTER 250u
#define W_FLASH 2800u
#define W_OTHER 5600u
#define W_ROW   2800u
#define W_BULK  25200u

/* LVP is on by default (the factory state). It lives outside the device state so that
 * sim_reset() does not change it. */
static int g_lvp = 1;
static int g_bad_dci; /* 1 = the DCI claims one row too many */
static int g_dat_high; /* 1 = ICSPDAT reads high whatever the device sends (fault) */
static uint16_t g_device_id = 0x6C60u;

/*
 * Sizes by device ID, from Table 2-1 and Table C-1 (16, 32 and 64 KW; 256 or 1024 bytes of
 * EEPROM) and written out independently of the device table. IDs are from Table B-1; the
 * LF parts differ from the F parts in the ID alone.
 */
static uint32_t flash_words(void)
{
    switch (g_device_id) {
    case 0x6C20u: /* PIC18F45K42 */
    case 0x6BC0u: /* PIC18F55K42 */
    case 0x6D60u: /* PIC18LF45K42 */
    case 0x6D00u: /* PIC18LF55K42 */
        return 16384u;
    case 0x6C60u: /* PIC18F26K42 */
    case 0x6C00u: /* PIC18F46K42 */
    case 0x6BA0u: /* PIC18F56K42 */
    case 0x6DA0u: /* PIC18LF26K42 */
    case 0x6D40u: /* PIC18LF46K42 */
    case 0x6CE0u: /* PIC18LF56K42 */
        return 32768u;
    default: /* x27 x47 x57, F and LF */
        return 65536u;
    }
}

static uint32_t eeprom_bytes(void)
{
    return flash_words() == 16384u ? 256u : 1024u;
}

static uint16_t g_revision_id = 0xA000u;
static int g_cp; /* 1 = CP/CPD enabled: Bulk Erase also takes the EEPROM */

typedef enum { ST_IDLE, ST_KEY, ST_CMD, ST_PAYLOAD_IN, ST_PAYLOAD_OUT } state_t;
typedef enum { OP_NONE, OP_LOAD_PC, OP_LOAD_DATA } op_t;

static struct {
    int mclr, clk;
    int dat_driven, dat_level; /* what the host is driving on ICSPDAT */
    state_t state;
    uint32_t key, bit_count;
    uint32_t cmd, payload;
    op_t op;
    int op_inc;
    uint32_t out_raw, out_clocks;
    uint32_t pc;
    uint64_t now;
    uint64_t wait_start;
    uint32_t wait_us;
    uint32_t violations, bad_commands, bulk_erases, row_programs;
    uint16_t latch[LATCH_WORDS]; /* flash, User ID and Configuration latches */
    uint8_t ee_latch;            /* the EEPROM latch */
    uint16_t flash[MAX_FLASH_WORDS];
    uint16_t userid[USERID_WORDS];
    uint16_t config[CONFIG_WORDS];
    uint8_t eeprom[MAX_EEPROM_BYTES];
} s;

void sim_select_device(uint16_t device_id)
{
    g_device_id = device_id;
}

static void reset_latches(void)
{
    for (size_t i = 0; i < LATCH_WORDS; i++)
        s.latch[i] = 0xFFFFu;
    s.ee_latch = 0xFFu;
}

void sim_reset(void)
{
    memset(&s, 0, sizeof s);
    s.mclr = 1;
    for (size_t i = 0; i < flash_words(); i++)
        s.flash[i] = 0xFFFFu;
    for (size_t i = 0; i < USERID_WORDS; i++)
        s.userid[i] = 0xFFFFu;
    for (size_t i = 0; i < CONFIG_WORDS; i++)
        s.config[i] = 0xFFFFu;
    memset(s.eeprom, 0xFF, sizeof s.eeprom);
    reset_latches();
}

void sim_set_lvp(int enabled)
{
    g_lvp = enabled;
}
void sim_set_dat_stuck_high(int enabled)
{
    g_dat_high = enabled;
}
int sim_in_program_mode(void)
{
    return s.state >= ST_CMD;
}
uint32_t sim_violations(void)
{
    return s.violations;
}
uint32_t sim_bad_commands(void)
{
    return s.bad_commands;
}
uint64_t sim_time_us(void)
{
    return s.now;
}
uint16_t sim_flash_word(uint32_t i)
{
    return s.flash[i];
}
void sim_set_flash_word(uint32_t i, uint16_t v)
{
    s.flash[i] = v;
}
uint16_t sim_userid_word(uint32_t i)
{
    return s.userid[i];
}
uint8_t sim_eeprom_byte(uint32_t i)
{
    return i < eeprom_bytes() ? s.eeprom[i] : 0u;
}
void sim_set_eeprom_byte(uint32_t i, uint8_t v)
{
    s.eeprom[i] = v;
}
uint16_t sim_config_word(uint32_t i)
{
    return i < CONFIG_WORDS ? s.config[i] : 0u;
}
uint32_t sim_bulk_erases(void)
{
    return s.bulk_erases;
}
uint32_t sim_row_programs(void)
{
    return s.row_programs;
}
int sim_latches_blank(void)
{
    for (size_t i = 0; i < LATCH_WORDS; i++)
        if (s.latch[i] != 0xFFFFu)
            return 0;
    return s.ee_latch == 0xFFu;
}
void sim_set_revision_id(uint16_t value)
{
    g_revision_id = value;
}
void sim_corrupt_dci(int enabled)
{
    g_bad_dci = enabled;
}
void sim_set_code_protect(int enabled)
{
    g_cp = enabled;
}

/* The device must not see clocks before this much virtual time has passed. */
static void require_wait(uint32_t us)
{
    s.wait_start = s.now;
    s.wait_us = us;
}

/* ---- memory model ---- */

/* Read the item at the PC. Returns the value; *step is how far the PC moves (1 or 2). */
static uint32_t mem_read(uint32_t pc, uint32_t *step)
{
    *step = 2u;
    if (pc < 2u * flash_words())
        return s.flash[pc / 2u];
    if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS)
        return s.userid[(pc - 0x200000u) / 2u];
    if (pc >= 0x300000u && pc < 0x300000u + 2u * CONFIG_WORDS)
        return s.config[(pc - 0x300000u) / 2u];
    if (pc >= 0x310000u && pc < 0x310000u + eeprom_bytes()) {
        *step = 1u;
        return s.eeprom[pc - 0x310000u];
    }
    switch (pc) {
    case 0x3FFF00u: /* DCI, Table C-1: ERSIZ, WLSIZ (bytes), URSIZ, EESIZ, PCNT */
        return ERASE_WORDS;
    case 0x3FFF02u:
        return 2u * LATCH_WORDS;
    case 0x3FFF04u:
        return flash_words() / ERASE_WORDS + (g_bad_dci ? 1u : 0u);
    case 0x3FFF06u:
        return eeprom_bytes();
    case 0x3FFF08u:
        return 28u;
    case 0x3FFFFCu:
        return g_revision_id;
    case 0x3FFFFEu:
        return g_device_id;
    default:
        return 0u; /* not present reads as 0 */
    }
}

/* Load Data for NVM: fill the latch the address selects (aligned with the address LSbs). */
static void load_latch(uint32_t pc, uint32_t value, uint32_t *step)
{
    *step = 2u;
    if (pc >= 0x310000u && pc < 0x310000u + eeprom_bytes()) {
        *step = 1u;
        s.ee_latch = (uint8_t)value;
    } else {
        s.latch[(pc / 2u) % LATCH_WORDS] = (uint16_t)value;
    }
}

/* Begin Internally Timed Programming. Programming can only clear bits, so each stored value
 * is old AND latch. Flash writes the whole row the PC is in; the other regions write the
 * one cell the PC points at. Every latch is reset afterwards (section 3.1.3.5). */
static void begin_program(uint32_t pc, uint32_t *wait)
{
    s.row_programs++;
    *wait = W_OTHER;
    if (pc < 2u * flash_words()) {
        uint32_t base = (pc / 2u) & ~(LATCH_WORDS - 1u);
        for (uint32_t i = 0; i < LATCH_WORDS; i++)
            s.flash[base + i] &= s.latch[i];
        *wait = W_FLASH;
    } else if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS) {
        s.userid[(pc - 0x200000u) / 2u] &= s.latch[(pc / 2u) % LATCH_WORDS];
    } else if (pc >= 0x300000u && pc < 0x300000u + 2u * CONFIG_WORDS) {
        s.config[(pc - 0x300000u) / 2u] &= s.latch[(pc / 2u) % LATCH_WORDS];
    } else if (pc >= 0x310000u && pc < 0x310000u + eeprom_bytes()) {
        s.eeprom[pc - 0x310000u] &= s.ee_latch;
    }
    reset_latches();
}

/* Spec Table 3-2: what Bulk Erase takes depends on where the PC points. */
static void do_bulk_erase(uint32_t pc)
{
    int flash = 0, userid = 0, config = 0, eeprom = 0;

    s.bulk_erases++;
    if (pc < 0x200000u) {
        flash = config = 1;
        eeprom = g_cp;
    } else if (pc < 0x310000u) { /* 30 0000h-30 001Fh */
        flash = userid = config = 1;
        eeprom = g_cp;
    } else {
        eeprom = 1;
    }
    if (flash)
        for (size_t i = 0; i < flash_words(); i++)
            s.flash[i] = 0xFFFFu;
    if (userid)
        for (size_t i = 0; i < USERID_WORDS; i++)
            s.userid[i] = 0xFFFFu;
    if (config)
        for (size_t i = 0; i < CONFIG_WORDS; i++)
            s.config[i] = 0xFFFFu;
    if (eeprom)
        memset(s.eeprom, 0xFF, sizeof s.eeprom);
}

static void do_row_erase(uint32_t pc)
{
    if (pc < 2u * flash_words()) {
        uint32_t base = (pc / 2u) & ~(ERASE_WORDS - 1u);
        for (uint32_t i = 0; i < ERASE_WORDS; i++)
            s.flash[base + i] = 0xFFFFu;
    }
}

/* ---- command decoding ---- */

static void finish_payload_in(void)
{
    uint32_t value = (s.payload >> 1) & 0x3FFFFFu; /* data sits between start and stop bits */
    uint32_t step;

    switch (s.op) {
    case OP_LOAD_PC:
        s.pc = value;
        require_wait(W_DLY);
        break;
    case OP_LOAD_DATA:
        load_latch(s.pc, value, &step);
        if (s.op_inc)
            s.pc += step;
        require_wait(W_DLY);
        break;
    default:
        break;
    }
    s.state = ST_CMD;
    s.bit_count = 0;
    s.cmd = 0;
}

static void decode_command(void)
{
    uint32_t step;

    s.bit_count = 0;
    switch (s.cmd) {
    case 0x80:
        s.op = OP_LOAD_PC;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0x18: /* Bulk Erase: no payload */
        do_bulk_erase(s.pc);
        require_wait(W_BULK);
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    case 0x00:
    case 0x02:
        s.op = OP_LOAD_DATA;
        s.op_inc = (s.cmd & 0x02u) != 0u;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0xE0: { /* Begin Internally Timed Programming */
        uint32_t wait;
        begin_program(s.pc, &wait);
        require_wait(wait);
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    }
    case 0xF0:
        do_row_erase(s.pc);
        require_wait(W_ROW);
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    case 0xFC:
    case 0xFE: {
        uint32_t v = mem_read(s.pc, &step);
        if (s.cmd == 0xFEu)
            s.pc += step;
        s.out_raw = v << 1; /* data between start and stop bits */
        s.out_clocks = 0;
        s.state = ST_PAYLOAD_OUT;
        require_wait(W_DLY);
        break;
    }
    default:
        s.bad_commands++;
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    }
}

/* Called on every falling edge of ICSPCLK: the device latches ICSPDAT here. */
static void falling_edge(void)
{
    int bit = s.dat_driven ? s.dat_level : 0;

    switch (s.state) {
    case ST_KEY:
        s.key = (s.key << 1) | (uint32_t)bit;
        if (++s.bit_count == 32u) {
            /* Only the first 31 bits are checked (the last is a don't-care). */
            if (g_lvp && (s.key >> 1) == (KEY >> 1)) {
                s.state = ST_CMD;
                s.bit_count = 0;
                s.cmd = 0;
                require_wait(W_ENTER);
            } else {
                s.state = ST_IDLE;
            }
        }
        break;
    case ST_CMD:
        s.cmd = (s.cmd << 1) | (uint32_t)bit;
        if (++s.bit_count == 8u)
            decode_command();
        break;
    case ST_PAYLOAD_IN:
        s.payload = (s.payload << 1) | (uint32_t)bit;
        if (++s.bit_count == 24u)
            finish_payload_in();
        break;
    case ST_PAYLOAD_OUT:
        if (s.out_clocks == 24u) {
            s.state = ST_CMD;
            s.bit_count = 0;
            s.cmd = 0;
            require_wait(W_DLY);
        }
        break;
    default:
        break;
    }
}

static void rising_edge(void)
{
    /* Any clock activity before the required wait has elapsed is a driver bug. */
    if (s.state != ST_IDLE && s.wait_us != 0u) {
        if (s.now - s.wait_start < s.wait_us)
            s.violations++;
        s.wait_us = 0u;
    }
    if (s.state == ST_PAYLOAD_OUT) {
        s.out_clocks++;
        /* The device drives ICSPDAT for the whole read payload. If the host is still
         * driving it too, the two outputs fight on a real board. */
        if (s.dat_driven)
            s.violations++;
    }
}

/* ---- the icsp.h API, as seen by the driver ---- */

void icsp_init(void)
{
    sim_reset();
}

void icsp_set_mclr(uint8_t level)
{
    if (!level && s.mclr) {
        s.state = ST_KEY;
        s.bit_count = 0;
        s.key = 0;
        require_wait(W_ENTER); /* entry hold time before the first key clock */
    } else if (level && !s.mclr) {
        s.state = ST_IDLE;
        s.wait_us = 0;
    }
    s.mclr = level;
}

void icsp_set_clk(uint8_t level)
{
    if (level && !s.clk) {
        s.clk = 1;
        rising_edge();
    } else if (!level && s.clk) {
        s.clk = 0;
        falling_edge();
    }
}

void icsp_set_dat(uint8_t level)
{
    s.dat_driven = 1;
    s.dat_level = level;
}
void icsp_release_dat(void)
{
    s.dat_driven = 0;
}

uint8_t icsp_get_dat(void)
{
    if (g_dat_high)
        return 1u;
    /* During a read payload the device drives data after each rising edge. */
    if (s.state == ST_PAYLOAD_OUT && s.clk && s.out_clocks >= 1u && s.out_clocks <= 24u) {
        return (uint8_t)((s.out_raw >> (24u - s.out_clocks)) & 1u);
    }
    return 0u;
}

void icsp_delay_us(uint32_t us)
{
    s.now += us;
}
