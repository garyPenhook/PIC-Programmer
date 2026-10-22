/*
 * p18_q10: the 8-bit command set of the PIC18F2X/4XQ10 family.
 *
 * Source: "PIC18F2X/4XQ10 Memory Programming Specification" DS40001874H (command set,
 * entry sequence, memory map, Bulk Erase table, timing) and the PIC18F27/47Q10 data sheet
 * DS40002043 (geometry, device IDs).
 *
 * The command bytes match p18_8bit, but these parts differ in ways that make them a
 * protocol of their own:
 *   - the data EEPROM is at 31 0000h and there is no DCI block to read back;
 *   - Configuration Words are 16 bits wide (six words at 30 0000h to 30 000Bh), written
 *     one word per Program Data command like flash, so a config cell here is a word;
 *   - Bulk Erase takes no payload: what it erases depends on the PC (spec Table 3-2);
 *   - the erase and write times are different (spec Table 4-1).
 *
 * Programming and reading both use byte addresses: flash, User ID and Configuration Words
 * are 16-bit words at even addresses (the PC advances by 2), the EEPROM is bytes (by 1).
 */
#include "pic_proto.h"

#include "icsp.h"
#include "icsp8.h"

/* Command bytes (spec Table 3-1). Bit 1 (reads) and bit 5 (programs) are the "J" flag:
 * 1 means the program counter advances after the operation. */
#define CMD_LOAD_PC     0x80u
#define CMD_BULK_ERASE  0x18u
#define CMD_SECT_ERASE  0xF0u
#define CMD_READ        0xFCu /* PC unchanged */
#define CMD_READ_INC    0xFEu /* PC advances */
#define CMD_PROGRAM     0xC0u /* PC unchanged */
#define CMD_PROGRAM_INC 0xE0u /* PC advances */

/* Byte addresses (spec Figure 2-1). */
#define ADDR_FLASH  0x000000u
#define ADDR_USERID 0x200000u
#define ADDR_CONFIG 0x300000u
#define ADDR_EEPROM 0x310000u
#define ADDR_REVID  0x3FFFFCu
#define ADDR_DEVID  0x3FFFFEu

/* Revision ID bits 15:12 read as 1010 on every device in the specification (Register 2-2). */
#define REVID_FIXED_NIBBLE 0xAu

/*
 * Timing, in microseconds. Every value below is at or beyond the limit.
 *
 *   spec Table 4-1                         limit          used here
 *   mode entry hold (TENTH)                >= 1 ms        2 ms
 *   word program, flash/User ID (TPINT)    <= 65 us       100 us
 *   word program, EEPROM (TPDFM)           <= 11 ms       12 ms
 *   sector erase (TERAS)                   <= 11 ms       12 ms
 *   bulk erase (TERAB)                     <= 75 ms       80 ms
 *
 * Configuration Words use the flash figure (spec Figure 3-16 shows one program cycle for
 * every region), but are given the longer EEPROM time since they are written rarely.
 */
#define T_ENTH_US       2000u
#define T_PROG_FLASH_US 100u
#define T_PROG_SLOW_US  12000u
#define T_SECT_ERASE_US 12000u
#define T_BULK_ERASE_US 80000u

static void load_pc(uint32_t addr)
{
    icsp8_command(CMD_LOAD_PC);
    icsp8_payload(addr & 0x3FFFFFu); /* addresses are 22 bits */
}

static void program_item(uint32_t value, int inc, uint32_t wait_us)
{
    icsp8_command(inc ? CMD_PROGRAM_INC : CMD_PROGRAM);
    icsp8_payload(value);
    icsp_delay_us(wait_us);
}

/* Flash, User ID and Configuration cells are 16-bit words; EEPROM cells are bytes. */
static int is_word_region(pic_region_t region)
{
    return region != PIC_REGION_EEPROM;
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

static uint16_t mask_of(pic_region_t region)
{
    return is_word_region(region) ? 0xFFFFu : 0xFFu;
}

static void q10_enter(void)
{
    icsp8_enter(T_ENTH_US);
}

static void q10_exit(void)
{
    icsp8_exit();
}

static pic_status_t read_item(uint8_t cmd, uint16_t mask, uint16_t *out)
{
    uint32_t raw;
    pic_status_t st = icsp8_read(cmd, &raw);

    if (st == PIC_OK) {
        *out = (uint16_t)(raw & mask);
    }
    return st;
}

static pic_status_t q10_read_device_id(uint16_t *id)
{
    load_pc(ADDR_DEVID);
    return read_item(CMD_READ, 0xFFFFu, id);
}

static pic_status_t q10_read_revision_id(uint16_t *id)
{
    load_pc(ADDR_REVID);
    return read_item(CMD_READ, 0xFFFFu, id);
}

/*
 * There is no DCI on these parts, so the device ID alone has to be backed by something
 * else before a write is allowed: the revision ID's top nibble is fixed at 1010. This is
 * weaker than a DCI cross-check (a one-in-sixteen chance for an arbitrary word), so the
 * ID and the table row carry most of the weight.
 */
static pic_status_t q10_confirm(const pic_device_t *dev)
{
    uint16_t rev;
    pic_status_t st = q10_read_revision_id(&rev);

    (void)dev;
    if (st != PIC_OK) {
        return st;
    }
    return (rev >> 12) == REVID_FIXED_NIBBLE ? PIC_OK : PIC_ERR_UNSUPPORTED;
}

static pic_status_t q10_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                             uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);

    (void)dev;
    load_pc(address_of(region, offset));
    for (size_t i = 0; i < count; i++) {
        pic_status_t st = read_item(i + 1u < count ? CMD_READ_INC : CMD_READ, mask, &buf[i]);
        if (st != PIC_OK) {
            return st;
        }
    }
    return PIC_OK;
}

static pic_status_t q10_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                              const uint16_t *buf, size_t count)
{
    uint16_t mask = mask_of(region);
    uint32_t wait = T_PROG_FLASH_US;

    if (region == PIC_REGION_EEPROM || region == PIC_REGION_CONFIG) {
        wait = T_PROG_SLOW_US;
    }
    if (dev->latch_words != 0u) { /* these parts program one word at a time */
        return PIC_ERR_UNSUPPORTED;
    }
    load_pc(address_of(region, offset));
    for (size_t i = 0; i < count; i++) {
        program_item(buf[i] & mask, i + 1u < count, wait);
    }
    return PIC_OK;
}

/* Send one Bulk Erase with the PC at `addr` and wait it out (spec Table 3-2, Figure 3-12). */
static void bulk_erase_at(uint32_t addr)
{
    load_pc(addr);
    icsp8_command(CMD_BULK_ERASE);
    icsp_delay_us(T_BULK_ERASE_US);
}

/*
 * Bulk Erase has no region payload; the PC picks the set (spec Table 3-2):
 *   PC in 00 0000h-01 FFFFh   flash + Configuration Words
 *   PC in 20 0000h-30 00FFh   flash + User ID + Configuration Words
 *   PC in 31 0000h and up     data EEPROM only
 * If CP or CPD is set the first two also erase the EEPROM, which cannot be avoided.
 * Anything that is not one of those sets (config alone, User ID alone, flash alone) has no
 * command and is rejected before anything is sent: use a sector erase for User ID and
 * flash.
 */
static pic_status_t q10_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    uint8_t rest = (uint8_t)(regions & ~PIC_ERASE_EEPROM);
    uint32_t addr = ADDR_FLASH;

    (void)dev;
    if (rest == (PIC_ERASE_FLASH | PIC_ERASE_CONFIG)) {
        addr = ADDR_FLASH;
    } else if (rest == (PIC_ERASE_FLASH | PIC_ERASE_USERID | PIC_ERASE_CONFIG)) {
        addr = ADDR_USERID;
    } else if (rest != 0u) {
        return PIC_ERR_UNSUPPORTED;
    }
    if (rest != 0u) {
        bulk_erase_at(addr);
    }
    if ((regions & PIC_ERASE_EEPROM) != 0u) {
        bulk_erase_at(ADDR_EEPROM);
    }
    return PIC_OK;
}

static pic_status_t q10_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    (void)dev;
    /* spec 3.1.3.6: sector erase is for program memory and User ID only */
    if (region != PIC_REGION_FLASH && region != PIC_REGION_USERID) {
        return PIC_ERR_UNSUPPORTED;
    }
    load_pc(address_of(region, offset)); /* the low bits of the address are ignored */
    icsp8_command(CMD_SECT_ERASE);
    icsp_delay_us(T_SECT_ERASE_US);
    return PIC_OK;
}

const pic_proto_t pic_proto_p18_q10 = {
    .name = "p18_q10",
    .enter = q10_enter,
    .exit = q10_exit,
    .read_device_id = q10_read_device_id,
    .read_revision_id = q10_read_revision_id,
    .read_dci = NULL,
    .confirm = q10_confirm,
    .cell_mask = mask_of,
    .dia_words = 0u, /* DS40001874H and the Q10 data sheets describe no DIA */
    .read = q10_read,
    .write = q10_write,
    .bulk_erase = q10_bulk_erase,
    .row_erase = q10_row_erase,
};
