#include "sim_p18_8bit.h"

#include <string.h>

#include "icsp.h"

/*
 * Q41 model, written out from the data sheet DS40002214 independently of the device table
 * so the tests cross-check the table: section 8.3 (32 User ID words), 8.7.1 (device IDs),
 * Table 9-3 (DCI) and the memory map (config bytes 30 0000h to 30 0009h).
 */
#define MAX_FLASH_WORDS  65536u
#define ERASE_WORDS      128u
#define ERASE_PAGE_BYTES (2u * ERASE_WORDS)
#define USERID_WORDS     32u
#define CONFIG_SPAN      0x1Du /* Q24: 30 0000h-30 001Ch */
#define MAX_EEPROM_BYTES 1024u
#define REVISION_ID      0xA041u /* revision B1, data sheet 8.7.2 tip */

#define FLASH_WORDS (s.user_pages * ERASE_WORDS)

#define KEY 0x4D434850u

/* Waits the spec requires (microseconds) between the end of one item and the next clock. */
#define W_DLY   1u
#define W_ENTER 1500u
#define W_FLASH 75u
#define W_NVM   11000u
#define W_ERASE 11000u

/* LVP is on by default (the factory state). It lives outside the device state so that
 * sim_reset() does not change it. */
static int g_lvp = 1;
static int g_dat_high; /* 1 = ICSPDAT reads high whatever the device sends (fault) */
static uint16_t g_device_id = 0x7560u;

typedef enum { ST_IDLE, ST_KEY, ST_CMD, ST_PAYLOAD_IN, ST_PAYLOAD_OUT } state_t;
typedef enum { OP_NONE, OP_LOAD_PC, OP_BULK, OP_PROGRAM } op_t;

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
    uint32_t violations, bad_commands;
    uint32_t user_pages;
    uint32_t eeprom_bytes;
    uint16_t flash[MAX_FLASH_WORDS];
    uint16_t userid[USERID_WORDS];
    uint32_t config_count; /* Q41/Q43: 10 bytes in a row; Q71: 11; Q20 and Q24: 13, with gaps */
    int config_q24;        /* Q24 address map (DS40002503 8.6) instead of Q20's (DS40002327D 6.14) */
    uint8_t config[CONFIG_SPAN];
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
    s.eeprom_bytes = 512u; /* Q41: EESIZ, Table 9-3 */
    s.config_count = 10u;
    switch (g_device_id) { /* URSIZ, Table 9-3 (Q41); Table 2-1 of DS40002079E (Q43) */
    case 0x7540u:
    case 0x7520u:
        s.user_pages = 64u;
        break; /* PIC18F04/14Q41 */
    case 0x7500u:
    case 0x74E0u:
        s.user_pages = 128u;
        break; /* PIC18F05/15Q41 */
    case 0x73C0u:
    case 0x73E0u:
    case 0x7400u:
        s.user_pages = 128u;
        s.eeprom_bytes = 1024u;
        break; /* PIC18F25/45/55Q43 */
    case 0x7420u:
    case 0x7440u:
    case 0x7460u:
        s.user_pages = 256u;
        s.eeprom_bytes = 1024u;
        break; /* PIC18F26/46/56Q43 */
    case 0x7480u:
    case 0x74A0u:
    case 0x74C0u:
        s.user_pages = 512u;
        s.eeprom_bytes = 1024u;
        break; /* PIC18F27/47/57Q43 */
    case 0x7AE0u:
    case 0x7AC0u:
        s.user_pages = 64u;
        s.eeprom_bytes = 256u;
        s.config_count = 13u;
        break; /* PIC18F04/14Q20, DS40002327D Table 2-1 */
    case 0x7AA0u:
    case 0x7A80u:
        s.user_pages = 128u;
        s.eeprom_bytes = 256u;
        s.config_count = 13u;
        break; /* PIC18F05/15Q20 */
    case 0x7A60u:
    case 0x7A40u:
        s.user_pages = 256u;
        s.eeprom_bytes = 256u;
        s.config_count = 13u;
        break; /* PIC18F06/16Q20 */
    case 0x78A0u:
        s.user_pages = 64u;
        s.config_count = 13u;
        s.config_q24 = 1;
        break; /* PIC18F24Q24, DS40002414C Table 2-1 */
    case 0x78C0u:
    case 0x7940u:
    case 0x7960u:
        s.user_pages = 128u;
        s.config_count = 13u;
        s.config_q24 = 1;
        break; /* PIC18F25/45/55Q24 */
    case 0x78E0u:
    case 0x7900u:
    case 0x7920u:
        s.user_pages = 256u;
        s.config_count = 13u;
        s.config_q24 = 1;
        break; /* PIC18F26/46/56Q24 */
    case 0x7660u:
    case 0x77A0u:
    case 0x7820u:
        s.user_pages = 64u;
        s.eeprom_bytes = 256u;
        s.config_count = 11u;
        break; /* PIC18F24/44/54Q71, DS40002306D Table 2-1 */
    case 0x76A0u:
    case 0x77E0u:
    case 0x7860u:
        s.user_pages = 128u;
        s.eeprom_bytes = 256u;
        s.config_count = 11u;
        break; /* PIC18F25/45/55Q71 */
    case 0x76E0u:
    case 0x7720u:
    case 0x7760u:
        s.user_pages = 256u;
        s.eeprom_bytes = 256u;
        s.config_count = 11u;
        break; /* PIC18F26/46/56Q71 */
    default:
        s.user_pages = 256u;
        break; /* PIC18F06/16Q41 */
    }
    for (size_t i = 0; i < FLASH_WORDS; i++)
        s.flash[i] = 0xFFFFu;
    for (size_t i = 0; i < USERID_WORDS; i++)
        s.userid[i] = 0xFFFFu;
    memset(s.config, 0xFF, sizeof s.config);
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
    return i < s.eeprom_bytes ? s.eeprom[i] : 0u;
}
void sim_set_eeprom_byte(uint32_t i, uint8_t v)
{
    s.eeprom[i] = v;
}
/* Address offset from 30 0000h of Configuration Byte i, as the part's address map has it. */
static uint32_t config_offset(uint32_t i)
{
    static const uint8_t q24[13] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                    0x08, 0x09, 0x0A, 0x18, 0x19, 0x1C};

    if (s.config_q24) {
        return q24[i];
    }
    return s.config_count > 11u && i >= 11u ? 0x18u + (i - 11u) : i;
}

/* The Configuration Byte at address pc, or NULL if the part has none there. */
static uint8_t *config_at(uint32_t pc)
{
    if (pc < 0x300000u || pc >= 0x300000u + CONFIG_SPAN) {
        return NULL;
    }
    for (uint32_t i = 0; i < s.config_count; i++) {
        if (config_offset(i) == pc - 0x300000u) {
            return &s.config[pc - 0x300000u];
        }
    }
    return NULL;
}

uint8_t sim_config_byte(uint32_t i)
{
    return i < s.config_count ? s.config[config_offset(i)] : 0u;
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
    if (pc < 2u * FLASH_WORDS)
        return s.flash[pc / 2u];
    if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS)
        return s.userid[(pc - 0x200000u) / 2u];
    if (config_at(pc) != NULL) {
        *step = 1u;
        return *config_at(pc);
    }
    if (pc >= 0x380000u && pc < 0x380000u + s.eeprom_bytes) {
        *step = 1u;
        return s.eeprom[pc - 0x380000u];
    }
    switch (pc) { /* Device Configuration Information */
    case 0x3C0000u:
        return ERASE_WORDS; /* ERSIZ */
    case 0x3C0002u:
        return 0u; /* WLSIZ */
    case 0x3C0004u:
        return s.user_pages; /* URSIZ */
    case 0x3C0006u:
        return s.eeprom_bytes; /* EESIZ */
    case 0x3C0008u:
        return 20u; /* PCNT */
    case 0x3FFFFCu:
        return REVISION_ID;
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
    if (pc < 2u * FLASH_WORDS) {
        s.flash[pc / 2u] &= (uint16_t)value;
        return;
    }
    if (pc >= 0x200000u && pc < 0x200000u + 2u * USERID_WORDS) {
        s.userid[(pc - 0x200000u) / 2u] &= (uint16_t)value;
        return;
    }
    *step = 1u;
    *wait = W_NVM;
    if (config_at(pc) != NULL) {
        *config_at(pc) &= (uint8_t)value;
        return;
    }
    if (pc >= 0x380000u && pc < 0x380000u + s.eeprom_bytes) {
        s.eeprom[pc - 0x380000u] &= (uint8_t)value;
        return;
    }
}

static void do_bulk_erase(uint32_t regions)
{
    if (regions & 1u)
        memset(s.eeprom, 0xFF, sizeof s.eeprom);
    if (regions & 2u)
        for (size_t i = 0; i < FLASH_WORDS; i++)
            s.flash[i] = 0xFFFFu;
    if (regions & 4u)
        for (size_t i = 0; i < USERID_WORDS; i++)
            s.userid[i] = 0xFFFFu;
    if (regions & 8u)
        memset(s.config, 0xFF, sizeof s.config);
}

static void do_page_erase(uint32_t pc)
{
    if (pc < 2u * FLASH_WORDS) {
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
    case OP_BULK:
        do_bulk_erase(value & 0xFu);
        require_wait(W_ERASE);
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
    case 0x18:
        s.op = OP_BULK;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0xC0:
    case 0xE0:
        s.op = OP_PROGRAM;
        s.op_inc = (s.cmd & 0x20u) != 0u;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0xF0:
        do_page_erase(s.pc);
        require_wait(W_ERASE);
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
