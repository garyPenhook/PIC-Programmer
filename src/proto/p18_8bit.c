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
#define CMD_LOAD_PC      0x80u
#define CMD_BULK_ERASE   0x18u
#define CMD_PAGE_ERASE   0xF0u
#define CMD_READ         0xFCu    /* PC unchanged */
#define CMD_READ_INC     0xFEu    /* PC advances */
#define CMD_PROGRAM      0xC0u    /* PC unchanged */
#define CMD_PROGRAM_INC  0xE0u    /* PC advances */

/* Byte addresses (Q41 data sheet memory map). */
#define ADDR_FLASH  0x000000u
#define ADDR_USERID 0x200000u
#define ADDR_CONFIG 0x300000u
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

/*
 * The 24-clock payload layout (value << 1) is taken from the PIC16F171XX programming spec
 * figures; the PIC18-Q figures are images that could not be read, so it is assumed here
 * and should be confirmed on real silicon.
 */
static void load_pc(uint32_t addr)
{
    icsp8_command(CMD_LOAD_PC);
    icsp8_payload(addr & 0x3FFFFFu);       /* addresses are 22 bits */
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

static uint32_t address_of(pic_region_t region, uint32_t offset)
{
    switch (region) {
    case PIC_REGION_USERID: return ADDR_USERID + 2u * offset;
    case PIC_REGION_CONFIG: return ADDR_CONFIG + offset;
    case PIC_REGION_EEPROM: return ADDR_EEPROM + offset;
    default:                return ADDR_FLASH + 2u * offset;
    }
}

static uint16_t mask_of(pic_region_t region)
{
    return is_word_region(region) ? 0xFFFFu : 0xFFu;
}

static void p18_enter(void) { icsp8_enter(T_ENTH_US); }
static void p18_exit(void) { icsp8_exit(); }

static uint16_t p18_read_device_id(void)
{
    load_pc(ADDR_DEVID);
    return (uint16_t)icsp8_read(CMD_READ);
}

static uint16_t p18_read_revision_id(void)
{
    load_pc(ADDR_REVID);
    return (uint16_t)icsp8_read(CMD_READ);
}

static void p18_read_dci(pic_dci_t *dci)
{
    /* Five 16-bit values at 3C 0000h, 3C 0002h, ... (ERSIZ, WLSIZ, URSIZ, EESIZ, PCNT). */
    load_pc(ADDR_DCI);
    dci->erase_words  = (uint16_t)icsp8_read(CMD_READ_INC);
    dci->latch_words  = (uint16_t)icsp8_read(CMD_READ_INC);
    dci->user_rows    = (uint16_t)icsp8_read(CMD_READ_INC);
    dci->eeprom_bytes = (uint16_t)icsp8_read(CMD_READ_INC);
    dci->pin_count    = (uint16_t)icsp8_read(CMD_READ);
}

/* The PC advances by 2 after a word and by 1 after a byte (spec Table 3-1 note). */
static pic_status_t p18_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                             uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);
    (void)dev;
    load_pc(address_of(region, offset));
    for (size_t i = 0; i < count; i++) {
        buf[i] = (uint16_t)(icsp8_read(i + 1u < count ? CMD_READ_INC : CMD_READ) & mask);
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
        return PIC_ERR_ARG;
    }
    load_pc(address_of(region, offset));
    for (size_t i = 0; i < count; i++) {
        program_item(buf[i] & mask, i + 1u < count, wait);
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
    (void)dev;
    if (!is_word_region(region)) {         /* spec 3.1.3.6: program memory and User ID only */
        return PIC_ERR_ARG;
    }
    load_pc(address_of(region, offset));   /* the low bits of the address are ignored */
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
