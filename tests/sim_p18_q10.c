#include "sim_p18_q10.h"

#include <string.h>

#include "icsp.h"

/*
 * Q10 model, written out from DS40001874H (memory map Figure 2-1, Table 3-1 commands,
 * Table 3-2 Bulk Erase, Table 4-1 timing, Register 2-2 revision ID) and DS40002043
 * (Table 2-1 sizes, device IDs), independently of the device table so the tests
 * cross-check it.
 */
#define MAX_FLASH_WORDS  65536u
#define ERASE_WORDS      128u
#define ERASE_PAGE_BYTES (2u * ERASE_WORDS)
#define USERID_WORDS     128u
#define CONFIG_WORDS     6u
#define MAX_EEPROM_BYTES 1024u

#define KEY 0x4D434850u

/* Waits the spec requires (microseconds) between the end of one item and the next clock. */
#define W_DLY   1u
#define W_ENTER 1000u
#define W_FLASH 65u
#define W_EE    11000u
#define W_SECT  11000u
#define W_BULK  75000u

/* LVP is on by default (the factory state). It lives outside the device state so that
 * sim_reset() does not change it. */
static int g_lvp = 1;
static int g_dat_high; /* 1 = ICSPDAT reads high whatever the device sends (fault) */
static uint16_t g_device_id = 0x7100u;

/*
 * Sizes by device ID, from the data sheets' Table 1 (flash in bytes: 16k, 32k, 64k, 128k;
 * words are half of that) and written out independently of the device table. IDs are from
 * DS40001874H Table B-1.
 */
static uint32_t flash_words(void)
{
    switch (g_device_id) {
    case 0x71C0u: /* PIC18F24Q10 */
        return 8192u;
    case 0x71A0u: /* PIC18F25Q10 */
    case 0x7140u: /* PIC18F45Q10 */
        return 16384u;
    case 0x7180u: /* PIC18F26Q10 */
    case 0x7120u: /* PIC18F46Q10 */
        return 32768u;
    default: /* PIC18F27Q10, PIC18F47Q10 */
        return 65536u;
    }
}

static uint32_t eeprom_bytes(void)
{
    switch (g_device_id) {
    case 0x71C0u:
    case 0x71A0u:
    case 0x7140u:
        return 256u;
    default:
        return 1024u;
    }
}

static uint16_t g_revision_id = 0xA000u;
static int g_cp; /* 1 = CP/CPD enabled: Bulk Erase also takes the EEPROM */

typedef enum { ST_IDLE, ST_KEY, ST_CMD, ST_PAYLOAD_IN, ST_PAYLOAD_OUT } state_t;
typedef enum { OP_NONE, OP_LOAD_PC, OP_PROGRAM } op_t;

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
    uint32_t violations, bad_commands, bulk_erases;
    uint16_t flash[MAX_FLASH_WORDS];
    uint16_t userid[USERID_WORDS];
    uint16_t config[CONFIG_WORDS];
    uint8_t eeprom[MAX_EEPROM_BYTES];
} s;

void sim_select_device(uint16_t device_id)
{
    g_device_id = device_id;
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
void sim_set_revision_id(uint16_t value)
{
    g_revision_id = value;
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
    case 0x3FFFFCu:
        return g_revision_id;
    case 0x3FFFFEu:
        return g_device_id;
    default:
        return 0u; /* not present reads as 0 */
    }
}

/* Programming can only clear bits, so the stored value is old AND new. */
static void mem_program(uint32_t pc, uint32_t value, uint32_t *step, uint32_t *wait)
{
    *step = 2u;
    *wait = W_FLASH;
    if (pc < 2u * flash_words()) {
        s.flash[pc / 2u] &= (uint16_t)value;
    } else if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS) {
        s.userid[(pc - 0x200000u) / 2u] &= (uint16_t)value;
    } else if (pc >= 0x300000u && pc < 0x300000u + 2u * CONFIG_WORDS) {
        s.config[(pc - 0x300000u) / 2u] &= (uint16_t)value;
    } else if (pc >= 0x310000u && pc < 0x310000u + eeprom_bytes()) {
        *step = 1u;
        *wait = W_EE;
        s.eeprom[pc - 0x310000u] &= (uint8_t)value;
    }
}

/* Spec Table 3-2: what Bulk Erase takes depends on where the PC points. */
static void do_bulk_erase(uint32_t pc)
{
    int flash = 0, userid = 0, config = 0, eeprom = 0;

    s.bulk_erases++;
    if (pc < 0x200000u) {
        flash = config = 1;
        eeprom = g_cp;
    } else if (pc < 0x310000u) { /* 20 0000h-20 00FFh and 30 0000h-30 00FFh */
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

static void do_sector_erase(uint32_t pc)
{
    if (pc < 2u * flash_words()) {
        uint32_t base = (pc & ~(ERASE_PAGE_BYTES - 1u)) / 2u;
        for (uint32_t i = 0; i < ERASE_PAGE_BYTES / 2u; i++)
            s.flash[base + i] = 0xFFFFu;
    } else if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS) {
        for (size_t i = 0; i < USERID_WORDS; i++)
            s.userid[i] = 0xFFFFu;
    }
}

/* ---- command decoding ---- */

static void finish_payload_in(void)
{
    uint32_t value = (s.payload >> 1) & 0x3FFFFFu; /* data sits between start and stop bits */
    uint32_t step, wait;

    switch (s.op) {
    case OP_LOAD_PC:
        s.pc = value;
        require_wait(W_DLY);
        break;
    case OP_PROGRAM:
        mem_program(s.pc, value, &step, &wait);
        if (s.op_inc)
            s.pc += step;
        require_wait(wait);
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
    case 0xC0:
    case 0xE0:
        s.op = OP_PROGRAM;
        s.op_inc = (s.cmd & 0x20u) != 0u;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0xF0:
        do_sector_erase(s.pc);
        require_wait(W_SECT);
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
