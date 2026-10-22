/*
 * p18_k42: the 8-bit command set of the PIC18(L)F26/27/45/46/47/55/56/57K42.
 *
 * Source: "PIC18(L)F26/27/45/46/47/55/56/57K42 Memory Programming Specification" 40001886B
 * (command set, memory map, Bulk Erase table, DCI, timing) and its Appendix B (device IDs).
 * Section and table numbers below refer to that document.
 *
 * The command bytes are the p16_8bit ones (Load Data for NVM, then Begin Internally Timed
 * Programming), but the memory is the PIC18's:
 *   - addresses are bytes: flash, User ID and Configuration Words are 16-bit words at even
 *     addresses (the PC advances by 2), the data EEPROM is bytes at 31 0000h (advance by 1);
 *   - flash is written one row at a time through write latches, User ID, Configuration Words
 *     and EEPROM one cell at a time (section 3.2);
 *   - the five Configuration Words are 16 bits wide (30 0000h to 30 0009h);
 *   - the DCI at 3F FF00h holds ERSIZ, WLSIZ (in bytes here), URSIZ, EESIZ and PCNT as
 *     16-bit words (Table C-1);
 *   - Bulk Erase takes no payload: what it erases depends on the PC (Table 3-2).
 */
#include "pic_proto.h"

#include "icsp.h"
#include "icsp8.h"

/* Command bytes (Table 3-1). Bit 1 of Load Data and Read Data is the "J" flag: 1 means the
 * program counter advances after the operation. */
#define CMD_LOAD_PC        0x80u
#define CMD_BULK_ERASE     0x18u
#define CMD_ROW_ERASE      0xF0u
#define CMD_LOAD_DATA      0x00u /* PC unchanged */
#define CMD_LOAD_DATA_INC  0x02u /* PC advances */
#define CMD_READ           0xFCu /* PC unchanged */
#define CMD_READ_INC       0xFEu /* PC advances */
#define CMD_BEGIN_INTERNAL 0xE0u /* commit the latched data, self-timed */

/* Byte addresses (Table 2-1, Table C-1). */
#define ADDR_FLASH  0x000000u
#define ADDR_USERID 0x200000u
#define ADDR_CONFIG 0x300000u
#define ADDR_EEPROM 0x310000u
#define ADDR_DCI    0x3FFF00u
#define ADDR_REVID  0x3FFFFCu
#define ADDR_DEVID  0x3FFFFEu

/*
 * Timing, in microseconds. Every value is beyond the limit in Table 3-3.
 *
 *   mode entry hold (TENTH)               >= 250 us    1 ms
 *   row erase (TERAR)                     <= 2.8 ms    3 ms
 *   program flash (TPINT)                 <= 2.8 ms    3 ms
 *   program Config Words, EEPROM (TPINT)  <= 5.6 ms    6 ms
 *   bulk erase (TERAB)                    <= 25.2 ms   26 ms
 *
 * The User ID has no figure of its own, so it uses the longer time.
 */
#define T_ENTH_US       1000u
#define T_ROW_ERASE_US  3000u
#define T_PROG_FLASH_US 3000u
#define T_PROG_OTHER_US 6000u
#define T_BULK_ERASE_US 26000u

#define BLANK_WORD 0xFFFFu /* an erased 16-bit word */

/* Payloads: the 22-bit address for Load PC (Figure 3-11), 16 bits of data for flash,
 * User ID and Configuration Words, 8 for EEPROM (Figures 3-6, 3-7). */
static void load_pc(uint32_t addr)
{
    icsp8_command(CMD_LOAD_PC);
    icsp8_payload(addr & 0x3FFFFFu);
}

static void load_data(uint16_t value, int inc)
{
    icsp8_command(inc ? CMD_LOAD_DATA_INC : CMD_LOAD_DATA);
    icsp8_payload(value);
}

/* Commit the latched data at the PC and wait for the self-timed write to finish. */
static void begin_program(uint32_t wait_us)
{
    icsp8_command(CMD_BEGIN_INTERNAL);
    icsp_delay_us(wait_us);
}

static pic_status_t read_item(int inc, uint16_t mask, uint16_t *out)
{
    uint32_t raw;
    pic_status_t st = icsp8_read(inc ? CMD_READ_INC : CMD_READ, &raw);

    if (st == PIC_OK) {
        *out = (uint16_t)(raw & mask);
    }
    return st;
}

/* Flash, User ID and Configuration cells are 16-bit words; EEPROM cells are bytes. */
static uint16_t mask_of(pic_region_t region)
{
    return region == PIC_REGION_EEPROM ? 0xFFu : 0xFFFFu;
}

static uint32_t address_of(pic_region_t region, uint32_t offset)
{
    switch (region) {
    case PIC_REGION_USERID:
        return ADDR_USERID + 2u * offset;
    case PIC_REGION_CONFIG:
        return ADDR_CONFIG + 2u * offset;
    case PIC_REGION_EEPROM:
        return ADDR_EEPROM + offset;
    default:
        return ADDR_FLASH + 2u * offset;
    }
}

static void k42_enter(void)
{
    icsp8_enter(T_ENTH_US);
}

static void k42_exit(void)
{
    icsp8_exit();
}

static pic_status_t k42_read_device_id(uint16_t *id)
{
    load_pc(ADDR_DEVID);
    return read_item(0, 0xFFFFu, id);
}

static pic_status_t k42_read_revision_id(uint16_t *id)
{
    load_pc(ADDR_REVID);
    return read_item(0, 0xFFFFu, id);
}

static pic_status_t k42_read_dci(pic_dci_t *dci)
{
    /* Table C-1: five 16-bit values at 3F FF00h. WLSIZ is in bytes, two per latch word.
     * Fill a local copy so that a failed read leaves *dci untouched. */
    pic_dci_t d;
    uint16_t *const fields[] = {&d.erase_words, &d.latch_words, &d.user_rows, &d.eeprom_bytes,
                                &d.pin_count};
    const size_t n = sizeof fields / sizeof fields[0];

    load_pc(ADDR_DCI);
    for (size_t i = 0; i < n; i++) {
        pic_status_t st = read_item(i + 1u < n, 0xFFFFu, fields[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    d.latch_words = (uint16_t)(d.latch_words / 2u);
    *dci = d;
    return PIC_OK;
}

static pic_status_t k42_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                             uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);

    (void)dev;
    load_pc(address_of(region, offset));
    for (size_t i = 0; i < count; i++) {
        /* Advance the PC except after the last item. */
        pic_status_t st = read_item(i + 1u < count, mask, &buf[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    return PIC_OK;
}

static pic_status_t k42_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                              const uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);

    if (region == PIC_REGION_FLASH) {
        uint32_t row = dev->latch_words;
        if (row == 0u) {
            return PIC_ERR_UNSUPPORTED;
        }
        if ((offset % row) != 0u) {
            return PIC_ERR_ALIGN;
        }
        /* One row per cycle (Figure 3-20): fill the latches, then commit. */
        for (size_t done = 0; done < count; done += row) {
            load_pc(address_of(region, offset + (uint32_t)done));
            for (uint32_t i = 0; i < row; i++) {
                /* Past the end of the data, load a blank word: programming 1s changes nothing. */
                uint16_t w = (done + i < count) ? buf[done + i] : BLANK_WORD;
                /* Advance the PC after every latch EXCEPT the last, so that it still points
                 * inside this row when the commit command arrives (the row is chosen by the
                 * PC at that moment, section 3.2). */
                load_data(w, i + 1u < row);
            }
            begin_program(T_PROG_FLASH_US);
        }
        return PIC_OK;
    }

    /* User ID, Configuration Words and EEPROM: one cell per cycle (Figure 3-19). */
    for (size_t i = 0; i < count; i++) {
        load_pc(address_of(region, offset + (uint32_t)i));
        load_data((uint16_t)(buf[i] & mask), 0);
        begin_program(T_PROG_OTHER_US);
    }
    return PIC_OK;
}

/*
 * Bulk Erase has no payload; the PC picks the set (Table 3-2):
 *   PC in 00 0000h-01 FFFFh   flash + Configuration Words
 *   PC in 30 0000h-30 001Fh   flash + User ID + Configuration Words
 *   PC in 31 0000h-3E FFFFh   data EEPROM only
 * If CP is set the first two also erase the EEPROM, which cannot be avoided. Any other set
 * (config alone, User ID alone, flash alone) has no command and is rejected before anything
 * is sent: use a row erase for flash.
 */
static pic_status_t k42_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    uint8_t rest = (uint8_t)(regions & ~PIC_ERASE_EEPROM);
    uint32_t addr = ADDR_FLASH;

    (void)dev;
    if (rest == (PIC_ERASE_FLASH | PIC_ERASE_CONFIG)) {
        addr = ADDR_FLASH;
    } else if (rest == (PIC_ERASE_FLASH | PIC_ERASE_USERID | PIC_ERASE_CONFIG)) {
        addr = ADDR_CONFIG;
    } else if (rest != 0u) {
        return PIC_ERR_UNSUPPORTED;
    }
    if (rest != 0u) {
        load_pc(addr);
        icsp8_command(CMD_BULK_ERASE);
        icsp_delay_us(T_BULK_ERASE_US);
    }
    if ((regions & PIC_ERASE_EEPROM) != 0u) {
        load_pc(ADDR_EEPROM);
        icsp8_command(CMD_BULK_ERASE);
        icsp_delay_us(T_BULK_ERASE_US);
    }
    return PIC_OK;
}

static pic_status_t k42_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    (void)dev;
    /* Section 3.1.3.9 describes flash rows only; the User ID is erased by a bulk erase. */
    if (region != PIC_REGION_FLASH) {
        return PIC_ERR_UNSUPPORTED;
    }
    load_pc(address_of(region, offset)); /* the low bits of the address are ignored */
    icsp8_command(CMD_ROW_ERASE);
    icsp_delay_us(T_ROW_ERASE_US);
    return PIC_OK;
}

const pic_proto_t pic_proto_p18_k42 = {
    .name = "p18_k42",
    .enter = k42_enter,
    .exit = k42_exit,
    .read_device_id = k42_read_device_id,
    .read_revision_id = k42_read_revision_id,
    .read_dci = k42_read_dci,
    .confirm = NULL,
    .cell_mask = mask_of,
    .read = k42_read,
    .write = k42_write,
    .bulk_erase = k42_bulk_erase,
    .row_erase = k42_row_erase,
};
