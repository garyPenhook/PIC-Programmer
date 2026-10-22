/*
 * p18_8bit: the 8-bit command set of the PIC18-Q families.
 *
 * Sources: the "PIC18-Q24/Q41/Q43 Family Programming Specification" documents (command
 * set, entry sequence, timing) and the PIC18F06/16Q41 data sheet DS40002214 (memory map,
 * DCI, device IDs). The Q41 programming specification is DS40002143.
 *
 * These devices have a 16-bit program word, byte addressing (a flash word occupies two
 * addresses), no row write latches (word-at-a-time programming), and page erase.
 *
 * Nothing here touches the SAFLOCK or ICSPDIS bits: both need the Program Access Enable
 * command, which this protocol deliberately never sends, so they cannot be set by accident.
 */
#include "pic_proto.h"

#include "icsp.h"
#include "icsp8.h"

/* Command bytes (spec Table 3-1). Both bit 1 (reads) and bit 5 (programs) are the "J"
 * flag: 1 means the program counter advances after the operation. */
#define CMD_LOAD_PC     0x80u
#define CMD_BULK_ERASE  0x18u
#define CMD_PAGE_ERASE  0xF0u
#define CMD_READ        0xFCu /* PC unchanged */
#define CMD_READ_INC    0xFEu /* PC advances */
#define CMD_PROGRAM     0xC0u /* PC unchanged */
#define CMD_PROGRAM_INC 0xE0u /* PC advances */

/* Byte addresses (Q41 data sheet memory map). */
#define ADDR_FLASH  0x000000u
#define ADDR_USERID 0x200000u
#define ADDR_CONFIG 0x300000u
/* Configuration Bytes do not always increment sequentially. The device table's config_map picks
 * the layout; each entry is the first cell of a run and the low byte of its address from
 * 30 0000h. Cell numbers count in address order, so the last run ends at config_count.
 *   0: one run (Q41 and Q43, 10 bytes)
 *   1: PIC18-Q20 (DS40002327D 2.4, 6.14): 30 0000h-30 000Ah, 30 0018h, 30 0019h
 *   2: PIC18-Q24 (DS40002503 8.6): 30 0000h-30 0006h, 30 0008h-30 000Ah, 30 0018h, 30 0019h,
 *      30 001Ch
 * host/picprog/devices.py and tests/sim_p18_8bit.c carry the same tables. */
typedef struct {
    uint8_t first_cell;
    uint8_t address;
} config_run_t;

static const config_run_t config_map_q20[] = {{0u, 0x00u}, {11u, 0x18u}};
static const config_run_t config_map_q24[] = {{0u, 0x00u}, {7u, 0x08u}, {10u, 0x18u}, {12u, 0x1Cu}};
#define ADDR_EEPROM 0x380000u
#define ADDR_DCI    0x3C0000u
#define ADDR_REVID  0x3FFFFCu
#define ADDR_DEVID  0x3FFFFEu

/*
 * NVM timing, in microseconds. Every value below is at or beyond the stated limit.
 *
 *   spec Table 4-1 (Q24/Q41/Q43)      limit          used here
 *   mode entry hold (TENTH)           >= 1.5 ms      2 ms (Q24 is the strictest)
 *   word program, flash / User ID     <= 75 us       100 us
 *   word program, EEPROM / config     <= 11 ms       12 ms
 *   page erase, bulk erase            <= 11 ms       12 ms
 */
#define T_ENTH_US       2000u
#define T_PROG_FLASH_US 100u
#define T_PROG_NVM_US   12000u
#define T_ERASE_US      12000u

/* Note: the payload layout used by icsp8_payload() is assumed for PIC18-Q (see icsp8.h). */
static void load_pc(uint32_t addr)
{
    icsp8_command(CMD_LOAD_PC);
    icsp8_payload(addr & 0x3FFFFFu); /* addresses are 22 bits */
}

/* Program one item at the current PC, then wait for the write to finish. */
static void program_item(uint32_t value, int inc, uint32_t wait_us)
{
    icsp8_command(inc ? CMD_PROGRAM_INC : CMD_PROGRAM);
    icsp8_payload(value);
    icsp_delay_us(wait_us);
}

/* Flash and User ID hold 16-bit words at even addresses; config and EEPROM hold bytes. */
static int is_word_region(pic_region_t region)
{
    return region == PIC_REGION_FLASH || region == PIC_REGION_USERID;
}

/* The run of Configuration Bytes holding cell `offset`: its first address and how many cells
 * (from `offset`) follow it consecutively. */
static uint32_t config_run(const pic_device_t *dev, size_t offset, size_t *left)
{
    const config_run_t *runs = NULL;
    size_t n = 0u;
    size_t i = 0u;

    if (dev->config_map == 1u) {
        runs = config_map_q20;
        n = sizeof config_map_q20 / sizeof config_map_q20[0];
    } else if (dev->config_map == 2u) {
        runs = config_map_q24;
        n = sizeof config_map_q24 / sizeof config_map_q24[0];
    }
    if (n == 0u) {
        *left = dev->config_count - offset;
        return ADDR_CONFIG + (uint32_t)offset;
    }
    while (i + 1u < n && offset >= runs[i + 1u].first_cell) {
        i++;
    }
    *left = (i + 1u < n ? runs[i + 1u].first_cell : dev->config_count) - offset;
    return ADDR_CONFIG + runs[i].address + (uint32_t)(offset - runs[i].first_cell);
}

static uint32_t address_of(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    size_t left;

    switch (region) {
    case PIC_REGION_USERID:
        return ADDR_USERID + 2u * offset;
    case PIC_REGION_CONFIG:
        return config_run(dev, offset, &left);
    case PIC_REGION_EEPROM:
        return ADDR_EEPROM + offset;
    default:
        return ADDR_FLASH + 2u * offset;
    }
}

/* How many of `count` items from `offset` sit at consecutive addresses (the PC auto-increments
 * only within a run; a new run needs its own Load PC). */
static size_t run_length(const pic_device_t *dev, pic_region_t region, size_t offset,
                         size_t count)
{
    size_t left;

    if (region == PIC_REGION_CONFIG) {
        (void)config_run(dev, offset, &left);
        return count < left ? count : left;
    }
    return count;
}

static uint16_t mask_of(pic_region_t region)
{
    return is_word_region(region) ? 0xFFFFu : 0xFFu;
}

static void p18_enter(void)
{
    icsp8_enter(T_ENTH_US);
}
static void p18_exit(void)
{
    icsp8_exit();
}

/* Read one item into *out. `cmd` is CMD_READ or CMD_READ_INC. */
static pic_status_t read_item(uint8_t cmd, uint16_t mask, uint16_t *out)
{
    uint32_t raw;
    pic_status_t st = icsp8_read(cmd, &raw);

    if (st == PIC_OK) {
        *out = (uint16_t)(raw & mask);
    }
    return st;
}

static pic_status_t p18_read_device_id(uint16_t *id)
{
    load_pc(ADDR_DEVID);
    return read_item(CMD_READ, 0xFFFFu, id);
}

static pic_status_t p18_read_revision_id(uint16_t *id)
{
    load_pc(ADDR_REVID);
    return read_item(CMD_READ, 0xFFFFu, id);
}

static pic_status_t p18_read_dci(pic_dci_t *dci)
{
    /* Five 16-bit values at 3C 0000h, 3C 0002h, ... (ERSIZ, WLSIZ, URSIZ, EESIZ, PCNT).
     * Fill a local copy so that a failed read leaves *dci untouched. */
    pic_dci_t d;
    uint16_t *const fields[] = {&d.erase_words, &d.latch_words, &d.user_rows, &d.eeprom_bytes,
                                &d.pin_count};
    const size_t n = sizeof fields / sizeof fields[0];

    load_pc(ADDR_DCI);
    for (size_t i = 0; i < n; i++) {
        pic_status_t st = read_item(i + 1u < n ? CMD_READ_INC : CMD_READ, 0xFFFFu, fields[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    *dci = d;
    return PIC_OK;
}

/* The PC advances by 2 after a word and by 1 after a byte (spec Table 3-1 note). */
static pic_status_t p18_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                             uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);
    size_t done = 0;

    while (done < count) {
        size_t run = run_length(dev, region, offset + done, count - done);

        load_pc(address_of(dev, region, (uint32_t)(offset + done)));
        for (size_t i = 0; i < run; i++) {
            pic_status_t st = read_item(i + 1u < run ? CMD_READ_INC : CMD_READ, mask, &buf[done + i]);
            if (st != PIC_OK) {
                return st;
            }
        }
        done += run;
    }
    return PIC_OK;
}

static pic_status_t p18_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                              const uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);
    /* Words take up to 75 us; EEPROM and configuration bytes up to 11 ms. */
    uint32_t wait = is_word_region(region) ? T_PROG_FLASH_US : T_PROG_NVM_US;

    /* Only word-at-a-time parts (WLSIZ = 0) are handled. */
    if (dev->latch_words != 0u) {
        return PIC_ERR_UNSUPPORTED;
    }
    for (size_t done = 0; done < count;) {
        size_t run = run_length(dev, region, offset + done, count - done);

        load_pc(address_of(dev, region, (uint32_t)(offset + done)));
        for (size_t i = 0; i < run; i++) {
            program_item(buf[done + i] & mask, i + 1u < run, wait);
        }
        done += run;
    }
    return PIC_OK;
}

static pic_status_t p18_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    (void)dev;
    icsp8_command(CMD_BULK_ERASE);
    icsp8_payload(regions);
    icsp_delay_us(T_ERASE_US);
    return PIC_OK;
}

static pic_status_t p18_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    if (!is_word_region(region)) { /* spec 3.1.3.6: program memory and User ID only */
        return PIC_ERR_UNSUPPORTED;
    }
    load_pc(address_of(dev, region, offset)); /* the low bits of the address are ignored */
    icsp8_command(CMD_PAGE_ERASE);
    icsp_delay_us(T_ERASE_US);
    return PIC_OK;
}

const pic_proto_t pic_proto_p18_8bit = {
    .name = "p18_8bit",
    .enter = p18_enter,
    .exit = p18_exit,
    .read_device_id = p18_read_device_id,
    .read_revision_id = p18_read_revision_id,
    .read_dci = p18_read_dci,
    .cell_mask = mask_of,
    .read = p18_read,
    .write = p18_write,
    .bulk_erase = p18_bulk_erase,
    .row_erase = p18_row_erase,
};
