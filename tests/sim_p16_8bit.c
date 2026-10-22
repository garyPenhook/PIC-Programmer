#include "sim_p16_8bit.h"

#include <string.h>

#include "icsp.h"

#define MAX_FLASH_WORDS 0x4000u
#define MAX_EEPROM      256u
#define ROW_WORDS       32u
#define BLANK           0x3FFFu
#define REVISION_ID     0x2000u /* reserved bits 13:12 = 10, revision fields 0 */

#define KEY 0x4D434850u

#define DIA_WORDS 32u

/* Device Information Area, 8100h to 811Fh, laid out as DS40002266B Table 2-2: MUI0 to MUI8,
 * one reserved word, EUI0 to EUI7 (not programmed: blank), TSLR1 to 3, TSHR1 to 3, FVRA1X to
 * FVRA4X, FVRC1X to FVRC4X (mV), two reserved words. The values are the model's own. */
static const uint16_t dia[DIA_WORDS] = {
    0x0A01u, 0x0B02u, 0x0C03u, 0x0D04u, 0x0E05u, 0x0F06u, 0x1007u, 0x1108u, 0x1209u, /* MUI */
    0x0000u,                                                                         /* MUI9 */
    0x3FFFu, 0x3FFFu, 0x3FFFu, 0x3FFFu, 0x3FFFu, 0x3FFFu, 0x3FFFu, 0x3FFFu,         /* EUI */
    0x0150u, 0x0A20u, 0x0004u,                                                       /* TSLR */
    0x0148u, 0x0A60u, 0x0008u,                                                       /* TSHR */
    1024u, 2048u, 4096u,                                                             /* FVRA */
    1024u, 2048u, 4096u,                                                             /* FVRC */
};

/* Waits the spec requires (microseconds) between the end of one item and the next clock
 * (Table 4-1). */
#define W_DLY       1u
#define W_ENTER     250u
#define W_ROW_ERASE 2800u
#define W_PROG      2800u
#define W_PROG_CFG  5600u
#define W_BULK      8400u

/* LVP is on by default (the factory state). It lives outside the device state so that
 * sim_reset() does not change it. */
static int g_lvp = 1;
static int g_dat_high; /* 1 = ICSPDAT reads high whatever the device sends (fault) */
static uint16_t g_device_id = 0x30E1u;

typedef enum { ST_IDLE, ST_KEY, ST_CMD, ST_PAYLOAD_IN, ST_PAYLOAD_OUT } state_t;
typedef enum { OP_NONE, OP_LOAD_PC, OP_BULK, OP_LOAD_DATA } op_t;

static struct {
    int mclr, clk;
    int dat_driven, dat_level;
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
    uint32_t flash_words, eeprom_bytes, user_rows;
    uint16_t flash[MAX_FLASH_WORDS];
    uint16_t userid[4];
    uint16_t config[7];
    uint8_t eeprom[MAX_EEPROM];
    uint16_t latch[ROW_WORDS];
} s;

void sim_select_device(uint16_t device_id)
{
    g_device_id = device_id;
}

/*
 * Geometry by device ID, written out from the spec independently of the device table so
 * the tests cross-check the table: Table 2-1 (flash) and Table 2-3 (URSIZ, EESIZ).
 */
static void geometry(uint32_t *user_rows, uint32_t *eeprom_bytes)
{
    switch (g_device_id) {
    case 0x3121u:
    case 0x3122u:
    case 0x3123u: /* PIC16F131x3: 2 KW, no EEPROM (DS40002500B Tables 2-1, 2-3) */
        *user_rows = 64u;
        *eeprom_bytes = 0u;
        break;
    case 0x3124u:
    case 0x3125u:
    case 0x3126u: /* PIC16F131x4: 4 KW */
        *user_rows = 128u;
        *eeprom_bytes = 0u;
        break;
    case 0x3127u:
    case 0x3128u:
    case 0x3129u: /* PIC16F131x5: 8 KW */
        *user_rows = 256u;
        *eeprom_bytes = 0u;
        break;
    case 0x3107u:
    case 0x3108u:
    case 0x3109u:
    case 0x310Au:
    case 0x310Bu: /* PIC16F181x4: 4 KW, 128-byte EEPROM (DS40002276A Tables 2-1, 2-3) */
        *user_rows = 128u;
        *eeprom_bytes = 128u;
        break;
    case 0x310Cu:
    case 0x310Du:
    case 0x310Eu:
    case 0x310Fu:
    case 0x3110u: /* PIC16F181x5: 8 KW, 128-byte EEPROM */
        *user_rows = 256u;
        *eeprom_bytes = 128u;
        break;
    case 0x30E3u:
    case 0x30E4u:
    case 0x30E5u: /* PIC16F152x3: 2 KW, no EEPROM (DS40002149B Tables 2-1, 2-3) */
        *user_rows = 64u;
        *eeprom_bytes = 0u;
        break;
    case 0x30E6u:
    case 0x30E7u:
    case 0x30E8u:
    case 0x30F0u:
    case 0x30EEu: /* PIC16F152x4: 4 KW */
        *user_rows = 128u;
        *eeprom_bytes = 0u;
        break;
    case 0x30E9u:
    case 0x30EAu:
    case 0x30EFu:
    case 0x30EDu: /* PIC16F152x5: 8 KW */
        *user_rows = 256u;
        *eeprom_bytes = 0u;
        break;
    case 0x30EBu:
    case 0x30ECu: /* PIC16F152x6: 16 KW */
        *user_rows = 512u;
        *eeprom_bytes = 0u;
        break;
    case 0x30DBu:
    case 0x30DCu:
    case 0x30DDu:
    case 0x3101u:
    case 0x3102u: /* x14 x24 x44 x54 x74 */
        *user_rows = 128u;
        *eeprom_bytes = 128u;
        break;
    case 0x30E2u:
    case 0x30DEu:
    case 0x30DFu:
    case 0x3103u:
    case 0x3104u: /* x15 x25 x45 x55 x75 */
        *user_rows = 256u;
        *eeprom_bytes = 128u;
        break;
    case 0x3130u:
    case 0x3131u:
    case 0x3132u:
    case 0x3133u:
    case 0x3134u: /* PIC16F132x4: 4 KW, 256-byte EEPROM (DS40002646A Tables 2-1, 2-3) */
        *user_rows = 128u;
        *eeprom_bytes = 256u;
        break;
    case 0x3135u:
    case 0x3136u:
    case 0x3137u:
    case 0x3138u:
    case 0x3139u: /* PIC16F132x5: 8 KW */
        *user_rows = 256u;
        *eeprom_bytes = 256u;
        break;
    case 0x3115u:
    case 0x3116u:
    case 0x3117u:
    case 0x3118u: /* PIC16F175x4: 4 KW, 128-byte EEPROM (DS40002540B Tables 2-1, 2-3) */
        *user_rows = 128u;
        *eeprom_bytes = 128u;
        break;
    case 0x3119u:
    case 0x311Au:
    case 0x311Bu:
    case 0x311Cu: /* PIC16F175x5: 8 KW, 128-byte EEPROM */
        *user_rows = 256u;
        *eeprom_bytes = 128u;
        break;
    default: /* x26 x46 x56 x76 of the F131XX/F132XX/F171XX, PIC16F181x6 (IDs 3111h to 3114h) and PIC16F175x6 (IDs 311Dh to
              * 3120h): 16 KW, 256-byte EEPROM */
        *user_rows = 512u;
        *eeprom_bytes = 256u;
        break;
    }
}

/* PIC16F132XX parts (device IDs 3130h to 3144h) have seven Configuration Words, the rest
 * five (DS40002646A 2.5; DS40002500B 2.6). */
static uint32_t config_words(void)
{
    return (g_device_id >= 0x3130u && g_device_id <= 0x3144u) ? 7u : 5u;
}

void sim_reset(void)
{
    memset(&s, 0, sizeof s);
    s.mclr = 1;

    geometry(&s.user_rows, &s.eeprom_bytes);
    s.flash_words = s.user_rows * ROW_WORDS;

    for (size_t i = 0; i < MAX_FLASH_WORDS; i++)
        s.flash[i] = BLANK;
    for (size_t i = 0; i < 4; i++)
        s.userid[i] = BLANK;
    for (size_t i = 0; i < 7; i++)
        s.config[i] = BLANK;
    memset(s.eeprom, 0xFF, sizeof s.eeprom);
    for (size_t i = 0; i < ROW_WORDS; i++)
        s.latch[i] = BLANK;
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
uint16_t sim_config_word(uint32_t i)
{
    return s.config[i];
}
uint8_t sim_eeprom_byte(uint32_t i)
{
    return s.eeprom[i];
}
uint16_t sim_dia_word(uint32_t i)
{
    return dia[i];
}

static void require_wait(uint32_t us)
{
    s.wait_start = s.now;
    s.wait_us = us;
}

/* ---- memory model ---- */

static uint32_t mem_read(uint32_t pc)
{
    if (pc < s.flash_words)
        return s.flash[pc];
    if (pc >= 0x8000u && pc < 0x8004u)
        return s.userid[pc - 0x8000u];
    if (pc == 0x8005u)
        return REVISION_ID;
    if (pc == 0x8006u)
        return g_device_id;
    if (pc >= 0x8007u && pc < 0x8007u + config_words())
        return s.config[pc - 0x8007u];
    if (pc >= 0xF000u && pc < 0xF000u + s.eeprom_bytes)
        return s.eeprom[pc - 0xF000u];
    if (pc >= 0x8100u && pc < 0x8100u + DIA_WORDS)
        return dia[pc - 0x8100u];
    switch (pc) { /* Device Configuration Information */
    case 0x8200u:
        return ROW_WORDS; /* ERSIZ */
    case 0x8201u:
        return ROW_WORDS; /* WLSIZ */
    case 0x8202u:
        return s.user_rows; /* URSIZ */
    case 0x8203u:
        return s.eeprom_bytes; /* EESIZ */
    case 0x8204u:
        return 20u; /* PCNT */
    default:
        return 0u; /* not present reads as 0 */
    }
}

/* Commit the latched data at the PC. Programming can only clear bits (old AND new). */
static void do_begin_program(void)
{
    uint32_t pc = s.pc;
    uint32_t wait = W_PROG;

    if (pc < s.flash_words) {
        uint32_t row = pc & ~(ROW_WORDS - 1u);
        for (uint32_t i = 0; i < ROW_WORDS; i++)
            s.flash[row + i] &= s.latch[i];
    } else if (pc >= 0x8000u && pc < 0x8004u) {
        s.userid[pc - 0x8000u] &= s.latch[pc & (ROW_WORDS - 1u)];
    } else if (pc >= 0x8007u && pc < 0x8007u + config_words()) {
        s.config[pc - 0x8007u] &= s.latch[pc & (ROW_WORDS - 1u)];
        wait = W_PROG_CFG;
    } else if (pc >= 0xF000u && pc < 0xF000u + s.eeprom_bytes) {
        s.eeprom[pc - 0xF000u] &= (uint8_t)s.latch[pc & (ROW_WORDS - 1u)];
    }
    for (uint32_t i = 0; i < ROW_WORDS; i++)
        s.latch[i] = BLANK; /* latches reset to 1s */
    require_wait(wait);
}

static void do_bulk_erase(uint32_t regions)
{
    if (regions & 1u)
        memset(s.eeprom, 0xFF, sizeof s.eeprom);
    if (regions & 2u)
        for (size_t i = 0; i < MAX_FLASH_WORDS; i++)
            s.flash[i] = BLANK;
    if (regions & 4u)
        for (size_t i = 0; i < 4; i++)
            s.userid[i] = BLANK;
    if (regions & 8u)
        for (size_t i = 0; i < 7; i++)
            s.config[i] = BLANK;
}

static void do_row_erase(uint32_t pc)
{
    if (pc >= 0x8000u && pc <= 0x8004u) { /* spec 3.2.3: only the User IDs */
        for (size_t i = 0; i < 4; i++)
            s.userid[i] = BLANK;
    } else if (pc < s.flash_words) {
        uint32_t row = pc & ~(ROW_WORDS - 1u);
        for (uint32_t i = 0; i < ROW_WORDS; i++)
            s.flash[row + i] = BLANK;
    }
}

/* ---- command decoding ---- */

static void finish_payload_in(void)
{
    switch (s.op) {
    case OP_LOAD_PC:
        s.pc = (s.payload >> 1) & 0xFFFFu; /* 16-bit address between start and stop */
        break;
    case OP_BULK:
        do_bulk_erase((s.payload >> 1) & 0xFu);
        require_wait(W_BULK);
        s.state = ST_CMD;
        s.bit_count = 0;
        s.cmd = 0;
        return;
    case OP_LOAD_DATA:
        s.latch[s.pc & (ROW_WORDS - 1u)] = (uint16_t)((s.payload >> 1) & BLANK);
        if (s.op_inc)
            s.pc++;
        break;
    default:
        break;
    }
    require_wait(W_DLY);
    s.state = ST_CMD;
    s.bit_count = 0;
    s.cmd = 0;
}

static void decode_command(void)
{
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
    case 0x00:
    case 0x02:
        s.op = OP_LOAD_DATA;
        s.op_inc = (s.cmd & 0x02u) != 0u;
        s.state = ST_PAYLOAD_IN;
        require_wait(W_DLY);
        break;
    case 0xF0:
        do_row_erase(s.pc);
        require_wait(W_ROW_ERASE);
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    case 0xE0:
        do_begin_program();
        s.state = ST_CMD;
        s.cmd = 0;
        break;
    case 0xFC:
    case 0xFE:
        s.out_raw = mem_read(s.pc) << 1; /* data between start and stop bits */
        if (s.cmd == 0xFEu)
            s.pc++;
        s.out_clocks = 0;
        s.state = ST_PAYLOAD_OUT;
        require_wait(W_DLY);
        break;
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
