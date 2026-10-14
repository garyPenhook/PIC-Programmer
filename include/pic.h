#ifndef PIC_H
#define PIC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Device-independent API for programming a target PIC over ICSP.
 *
 * Supported parts are rows in a device table (data/pic_devices.csv, turned into C at build
 * time by tools/gen_devices.py). Each row names the programming protocol the part speaks
 * and its memory geometry. A protocol is the code: one per programming specification
 * command set (see pic_proto.h and src/proto/), shared by every family that uses it.
 *
 * Memory is addressed as (region, offset). The offset counts cells from the start of the
 * region, and every cell is passed as a uint16_t:
 *   flash, User ID   one program word per cell (14 bits on PIC16, 16 bits on PIC18)
 *   config           one Configuration Word (PIC16) or configuration byte (PIC18) per cell
 *   EEPROM           one byte per cell, in the low 8 bits
 * Each protocol maps this onto its own native addresses, so callers never need to know
 * whether a family is word or byte addressed.
 *
 * Call order: pic_probe() (or pic_enter() for a known device), any number of operations,
 * pic_exit().
 */

typedef enum {
    PIC_OK = 0,
    PIC_ERR_ARG = -1,      /* region, offset, size or alignment not allowed */
    PIC_ERR_VERIFY = -2    /* data read back differs from what was written */
} pic_status_t;

typedef enum {
    PIC_REGION_FLASH = 0,
    PIC_REGION_USERID,
    PIC_REGION_CONFIG,
    PIC_REGION_EEPROM
} pic_region_t;

/* Bulk Erase region bits. Both 8-bit-command protocols use this payload layout. */
#define PIC_ERASE_EEPROM (1u << 0)
#define PIC_ERASE_FLASH  (1u << 1)
#define PIC_ERASE_USERID (1u << 2)
#define PIC_ERASE_CONFIG (1u << 3)
#define PIC_ERASE_ALL    (PIC_ERASE_EEPROM | PIC_ERASE_FLASH | PIC_ERASE_USERID | PIC_ERASE_CONFIG)

typedef struct pic_proto pic_proto_t;

/* One supported part: a row of the device table. */
typedef struct {
    const char *name;
    const pic_proto_t *proto;
    uint16_t device_id;
    uint16_t erase_words;     /* ERSIZ: flash words per row or page erase */
    uint16_t latch_words;     /* WLSIZ: write latches per row; 0 = one word at a time */
    uint16_t userid_words;
    uint16_t config_count;    /* Configuration Words (PIC16) or bytes (PIC18) */
    uint16_t eeprom_bytes;    /* 0 = no EEPROM */
    uint32_t flash_words;
} pic_device_t;

/* Device Configuration Information, as read from the part itself. */
typedef struct {
    uint16_t erase_words;     /* ERSIZ */
    uint16_t latch_words;     /* WLSIZ */
    uint16_t user_rows;       /* URSIZ: flash = user_rows * erase_words */
    uint16_t eeprom_bytes;    /* EESIZ */
    uint16_t pin_count;       /* PCNT */
} pic_dci_t;

extern const pic_device_t pic_devices[];
extern const size_t pic_device_count;

/* The table row for this protocol and device ID, or NULL. */
const pic_device_t *pic_find_device(const pic_proto_t *proto, uint16_t device_id);

/*
 * Try each protocol with read-only commands until one returns a device ID in the table
 * and the part's DCI agrees with that row. Leaves the target out of programming mode. Returns the device or NULL. The IDs are
 * those of the matching protocol, or of the first protocol tried if nothing matched
 * (0 when nothing answers).
 */
const pic_device_t *pic_probe(uint16_t *device_id, uint16_t *revision_id);

/* Number of cells in a region of this device (0 if it has none). */
uint32_t pic_region_size(const pic_device_t *dev, pic_region_t region);

/* Enter Program/Verify mode with the low-voltage key. The target must have LVP = 1. */
void pic_enter(const pic_device_t *dev);

/* Leave Program/Verify mode and let the target run. */
void pic_exit(const pic_device_t *dev);

uint16_t pic_read_device_id(const pic_device_t *dev);
uint16_t pic_read_revision_id(const pic_device_t *dev);
void pic_read_dci(const pic_device_t *dev, pic_dci_t *dci);

/*
 * Reads and writes. The whole range must lie inside the region. Programming can only
 * turn 1s into 0s, so erase first. Flash writes on a device with write latches must start
 * on a row boundary; a short final row is padded with blank cells, which leave the rest of
 * the row unchanged. Code protection makes protected memory read back as zeros.
 */
pic_status_t pic_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                      uint16_t *buf, size_t count);
pic_status_t pic_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                       const uint16_t *buf, size_t count);

/* Read back and compare. Return PIC_ERR_VERIFY on the first difference. */
pic_status_t pic_verify(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                        const uint16_t *buf, size_t count);

/* Erase. Bulk takes PIC_ERASE_*; row erase works on flash and User ID only. */
pic_status_t pic_bulk_erase(const pic_device_t *dev, uint8_t regions);
pic_status_t pic_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset);

#endif
