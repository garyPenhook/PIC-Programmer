/*
 * p16_8bit: the 8-bit command set of recent PIC16 families.
 *
 * Source: "PIC16F171XX Family Programming Specification" DS40002266B (a copy is in
 * ~/Documents/Microchip/PIC/16F17146/). Section numbers below refer to that document.
 * The PIC16F13145, PIC16F13276 and PIC16F17576 programming specifications (DS40002500B,
 * DS40002646A, DS40002540B) describe the same command set; each family's devices have their
 * own rows in the device table.
 *
 * Memory is word addressed, 14 bits per word. Flash is written one row at a time through
 * write latches; Configuration Words and User IDs are written one word at a time; the data
 * EEPROM is written one byte at a time.
 */
#include "pic_proto.h"

#include "icsp.h"
#include "icsp8.h"

/* Command bytes (spec Table 3-1). Bit 1 of the Load Data and Read Data commands is the
 * "J" flag: 1 means the program counter advances by one word afterwards. */
#define CMD_LOAD_PC        0x80u
#define CMD_BULK_ERASE     0x18u
#define CMD_ROW_ERASE      0xF0u
#define CMD_LOAD_DATA      0x00u /* PC unchanged */
#define CMD_LOAD_DATA_INC  0x02u /* PC advances */
#define CMD_READ           0xFCu /* PC unchanged */
#define CMD_READ_INC       0xFEu /* PC advances */
#define CMD_BEGIN_INTERNAL 0xE0u /* commit the latched data, self-timed */

/* Word addresses (spec Table 2-1). */
#define ADDR_FLASH  0x0000u
#define ADDR_USERID 0x8000u
#define ADDR_REVID  0x8005u
#define ADDR_DEVID  0x8006u
#define ADDR_CONFIG 0x8007u
#define ADDR_DIA    0x8100u /* read only; Table 2-2 defines 8100h to 811Fh (32 words) */
#define ADDR_DCI    0x8200u
#define ADDR_EEPROM 0xF000u

/*
 * NVM timing, in microseconds. The specs give maximums (Table 4-1); each value below is
 * beyond the largest of the four families' limits, so one set serves every p16_8bit row.
 *
 *                                 PIC16F171XX    PIC16F131XX    PIC16F132XX   used here
 *                                 DS40002266B    DS40002500B    DS40002646A
 *                                 (PIC16F175XX, DS40002540B Table 4-1, has the F132XX's limits)
 *   mode entry hold (TENTH)       >= 250 us      >= 250 us      >= 250 us     1 ms
 *   row erase (TERAR)             <= 2.8 ms      <= 9 ms        <= 10 ms      11 ms
 *   program flash (TPINT)         <= 2.8 ms      <= 7 ms        <= 8 ms       9 ms
 *   program Config Words          <= 5.6 ms      <= 12 ms       <= 13 ms      14 ms
 *   bulk erase (TERAB)            <= 8.4 ms      <= 20 ms       <= 40 ms      41 ms
 *
 * User ID and EEPROM writes have no figure of their own in the specs, so they use the
 * longer Configuration Word time to be safe.
 */
#define T_ENTH_US       1000u
#define T_ROW_ERASE_US  11000u
#define T_PROG_FLASH_US 9000u
#define T_PROG_OTHER_US 14000u
#define T_BULK_ERASE_US 41000u

#define WORD_MASK  0x3FFFu
#define BLANK_WORD 0x3FFFu /* an erased 14-bit word */

/* Payload data fields: 16 bits for Load PC (Figure 3-6), 14 for Load Data to flash,
 * config and User ID (Figure 3-9), 8 for EEPROM (Figure 3-10), 4 for Bulk Erase (3-7). */
static void load_pc(uint16_t addr)
{
    icsp8_command(CMD_LOAD_PC);
    icsp8_payload(addr);
}

static void load_data(uint16_t value, int inc)
{
    icsp8_command(inc ? CMD_LOAD_DATA_INC : CMD_LOAD_DATA);
    icsp8_payload(value);
}

/* Read the item at the PC into *out. `inc` advances the PC by one afterwards. */
static pic_status_t read_item(int inc, uint16_t mask, uint16_t *out)
{
    uint32_t raw;
    pic_status_t st = icsp8_read(inc ? CMD_READ_INC : CMD_READ, &raw);

    if (st == PIC_OK) {
        *out = (uint16_t)(raw & mask);
    }
    return st;
}

/* Commit the latched data at the PC and wait for the self-timed write to finish. */
static void begin_program(uint32_t wait_us)
{
    icsp8_command(CMD_BEGIN_INTERNAL);
    icsp_delay_us(wait_us);
}

static uint16_t base_of(pic_region_t region)
{
    switch (region) {
    case PIC_REGION_USERID:
        return ADDR_USERID;
    case PIC_REGION_CONFIG:
        return ADDR_CONFIG;
    case PIC_REGION_EEPROM:
        return ADDR_EEPROM;
    case PIC_REGION_DIA:
        return ADDR_DIA;
    default:
        return ADDR_FLASH;
    }
}

/* EEPROM cells are bytes; everything else is a 14-bit word. */
static uint16_t mask_of(pic_region_t region)
{
    return region == PIC_REGION_EEPROM ? 0xFFu : WORD_MASK;
}

static void p16_enter(void)
{
    icsp8_enter(T_ENTH_US);
}
static void p16_exit(void)
{
    icsp8_exit();
}

static pic_status_t p16_read_device_id(uint16_t *id)
{
    load_pc(ADDR_DEVID);
    return read_item(0, WORD_MASK, id);
}

static pic_status_t p16_read_revision_id(uint16_t *id)
{
    load_pc(ADDR_REVID);
    return read_item(0, WORD_MASK, id);
}

static pic_status_t p16_read_dci(pic_dci_t *dci)
{
    /* ERSIZ, WLSIZ, URSIZ, EESIZ, PCNT at 0x8200 to 0x8204 (spec Table 2-3). Fill a local
     * copy so that a failed read leaves *dci untouched. */
    pic_dci_t d;
    uint16_t *const fields[] = {&d.erase_words, &d.latch_words, &d.user_rows, &d.eeprom_bytes,
                                &d.pin_count};
    const size_t n = sizeof fields / sizeof fields[0];

    load_pc(ADDR_DCI);
    for (size_t i = 0; i < n; i++) {
        pic_status_t st = read_item(i + 1u < n, WORD_MASK, fields[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    *dci = d;
    return PIC_OK;
}

static pic_status_t p16_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                             uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);
    (void)dev;
    load_pc((uint16_t)(base_of(region) + offset));
    for (size_t i = 0; i < count; i++) {
        /* Advance the PC except after the last item. */
        pic_status_t st = read_item(i + 1u < count, mask, &buf[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    return PIC_OK;
}

static pic_status_t p16_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                              const uint16_t *buf, size_t count)
{
    uint16_t base = base_of(region);
    uint16_t mask = mask_of(region);

    if (region == PIC_REGION_DIA) { /* read only: the part ignores it, so send nothing */
        return PIC_ERR_UNSUPPORTED;
    }
    if (region == PIC_REGION_FLASH) {
        uint32_t row = dev->latch_words;
        if (row == 0u) {
            return PIC_ERR_UNSUPPORTED;
        }
        if ((offset % row) != 0u) {
            return PIC_ERR_ALIGN;
        }
        /* One row per cycle (Figure 3-19): fill the latches, then commit. */
        for (size_t done = 0; done < count; done += row) {
            load_pc((uint16_t)(offset + done));
            for (uint32_t i = 0; i < row; i++) {
                /* Past the end of the data, load a blank word: programming 1s changes nothing. */
                uint16_t w =
                    (done + i < count) ? (uint16_t)(buf[done + i] & WORD_MASK) : BLANK_WORD;
                /* Advance the PC after every latch EXCEPT the last, so that it still points
                 * inside this row when the commit command arrives (the row is chosen by the
                 * PC at that moment). */
                load_data(w, i + 1u < row);
            }
            begin_program(T_PROG_FLASH_US);
        }
        return PIC_OK;
    }

    /* User ID, Configuration Words and EEPROM: one cell per cycle (Figure 3-18). */
    for (size_t i = 0; i < count; i++) {
        load_pc((uint16_t)(base + offset + i));
        load_data((uint16_t)(buf[i] & mask), 0);
        begin_program(T_PROG_OTHER_US);
    }
    return PIC_OK;
}

static pic_status_t p16_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    (void)dev;
    /*
     * Spec 3.2.2 says the payload selects the regions to erase. The Configuration Memory
     * flowchart (Figure 3-20) also loads a PC address first ("selects Bulk Erase regions"),
     * so follow it: point the PC into the configuration area when User ID or Configuration
     * Words are being erased, and at the start of flash otherwise. This is harmless if only
     * the payload matters.
     */
    load_pc((regions & (PIC_ERASE_USERID | PIC_ERASE_CONFIG)) != 0u ? ADDR_USERID : ADDR_FLASH);
    icsp8_command(CMD_BULK_ERASE);
    icsp8_payload(regions);
    icsp_delay_us(T_BULK_ERASE_US);
    return PIC_OK;
}

static pic_status_t p16_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    (void)dev;
    /* Flash rows, or the User IDs (spec 3.2.3: 0x8000 to 0x8004 erase only the User IDs
     * whatever CP says). Configuration Words and EEPROM need a bulk erase. */
    if (region != PIC_REGION_FLASH && region != PIC_REGION_USERID) {
        return PIC_ERR_UNSUPPORTED;
    }
    load_pc((uint16_t)(base_of(region) + offset)); /* the low bits of the address are ignored */
    icsp8_command(CMD_ROW_ERASE);
    icsp_delay_us(T_ROW_ERASE_US);
    return PIC_OK;
}

const pic_proto_t pic_proto_p16_8bit = {
    .name = "p16_8bit",
    .enter = p16_enter,
    .exit = p16_exit,
    .read_device_id = p16_read_device_id,
    .read_revision_id = p16_read_revision_id,
    .read_dci = p16_read_dci,
    .cell_mask = mask_of,
    .dia_words = 32u, /* the mapped window runs to 813Fh; only these words are defined */
    .read = p16_read,
    .write = p16_write,
    .bulk_erase = p16_bulk_erase,
    .row_erase = p16_row_erase,
};
