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
 * pic_exit(). Every operation that can fail returns a pic_status_t. Writes and erases are
 * open loop (the part is given its worst-case time, never polled), so use pic_verify()
 * to learn whether they worked.
 */

/* Makes the compiler warn when a caller ignores the returned status. */
#if defined(__GNUC__)
#define PIC_NODISCARD __attribute__((warn_unused_result))
#else
#define PIC_NODISCARD
#endif

/*
 * Result of an operation: PIC_OK (0) or a negative PIC_ERR_* code. Except for
 * PIC_ERR_PROTOCOL, an argument or capability error is reported before anything is sent
 * to the target.
 */
typedef enum {
    PIC_OK = 0,
    PIC_ERR_ARG = -1,         /* NULL pointer, unknown region, or bad erase mask */
    PIC_ERR_RANGE = -2,       /* offset and count do not lie inside the region */
    PIC_ERR_ALIGN = -3,       /* flash write does not start on a row boundary */
    PIC_ERR_UNSUPPORTED = -4, /* this device or protocol cannot do that, or does not have
                                 the region (also: a part answered that is not in the table) */
    PIC_ERR_NO_TARGET = -5,   /* nothing answered: ID reads back 0 (no target, no power, or
                                 the part does not have LVP enabled) */
    PIC_ERR_MISMATCH = -6,    /* a different device answered than the one named */
    PIC_ERR_PROTOCOL = -7,    /* a read payload had a bad start or stop bit */
    PIC_ERR_VERIFY = -8       /* data read back differs from what was written */
} pic_status_t;

/* A short fixed string for a status ("ok" for PIC_OK, "unknown error" for any other value). */
const char *pic_strerror(pic_status_t status);

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
    uint16_t erase_words; /* ERSIZ: flash words per row or page erase */
    uint16_t latch_words; /* WLSIZ: write latches per row; 0 = one word at a time */
    uint16_t userid_words;
    uint16_t config_count; /* Configuration Words (PIC16) or bytes (PIC18) */
    uint16_t eeprom_bytes; /* 0 = no EEPROM */
    uint8_t config_map;    /* PIC18 Configuration Byte placement; see p18_8bit.c (0 = contiguous) */
    uint32_t flash_words;
} pic_device_t;

/* Device Configuration Information, as read from the part itself. */
typedef struct {
    uint16_t erase_words;  /* ERSIZ */
    uint16_t latch_words;  /* WLSIZ */
    uint16_t user_rows;    /* URSIZ: flash = user_rows * erase_words */
    uint16_t eeprom_bytes; /* EESIZ */
    uint16_t pin_count;    /* PCNT */
} pic_dci_t;

extern const pic_device_t pic_devices[];
extern const size_t pic_device_count;

/* The table row for this protocol and device ID, or NULL. */
const pic_device_t *pic_find_device(const pic_proto_t *proto, uint16_t device_id);

/*
 * Try each protocol with read-only commands until one returns a device ID in the table
 * and the part's DCI agrees with that row. Leaves the target out of programming mode.
 * Returns PIC_OK and sets *dev to the row, or sets *dev to NULL and returns:
 *   PIC_ERR_ARG          dev is NULL (nothing is sent)
 *   PIC_ERR_UNSUPPORTED  a part answered but is not in the table (or its DCI disagrees)
 *   PIC_ERR_PROTOCOL     nothing sensible answered and a read had bad framing
 *   PIC_ERR_NO_TARGET    every ID read back 0
 * device_id and revision_id may be NULL. They get the IDs of the matching protocol, or of
 * the first protocol tried if nothing matched (0 when nothing answers).
 */
pic_status_t pic_probe(const pic_device_t **dev, uint16_t *device_id, uint16_t *revision_id);

/* Number of cells in a region of this device (0 if it has none). */
uint32_t pic_region_size(const pic_device_t *dev, pic_region_t region);

/*
 * The bits a cell of this region stores (0x3FFF for a PIC16 word, 0xFF for an EEPROM byte).
 * An erased cell reads back as this value: erasing sets every bit to 1 (DS40002266B
 * Table 1-2). 0 for a NULL device or an unknown region.
 */
uint16_t pic_cell_mask(const pic_device_t *dev, pic_region_t region);

/*
 * Enter Program/Verify mode with the low-voltage key. The target must have LVP = 1. Then
 * read the device ID and compare it with dev, so a missing or different part is caught
 * before any write: PIC_ERR_NO_TARGET (ID 0), PIC_ERR_MISMATCH (a different ID, or the
 * part's DCI disagrees with the row) or PIC_ERR_PROTOCOL. A NULL or invalid dev returns
 * PIC_ERR_ARG without touching the pins. On any error the target has
 * been taken back out of programming mode, and the caller must not go on to write: the
 * read, write and erase calls do not check that the target is in programming mode.
 */
PIC_NODISCARD pic_status_t pic_enter(const pic_device_t *dev);

/* Leave Program/Verify mode and let the target run. */
void pic_exit(const pic_device_t *dev);

/* These can return PIC_ERR_PROTOCOL. The output is untouched on error. */
pic_status_t pic_read_device_id(const pic_device_t *dev, uint16_t *id);
pic_status_t pic_read_revision_id(const pic_device_t *dev, uint16_t *id);
pic_status_t pic_read_dci(const pic_device_t *dev, pic_dci_t *dci);

/*
 * Reads and writes. The whole range must lie inside the region. Programming can only
 * turn 1s into 0s, so erase first. Flash writes on a device with write latches must start
 * on a row boundary; a short final row is padded with blank cells, which leave the rest of
 * the row unchanged. Code protection makes protected memory read back as zeros.
 *
 * Errors: PIC_ERR_ARG (NULL dev or buf, unknown region), PIC_ERR_RANGE, PIC_ERR_ALIGN,
 * PIC_ERR_UNSUPPORTED, PIC_ERR_PROTOCOL (reads). A count of 0 succeeds without checks
 * beyond dev and region. After PIC_ERR_PROTOCOL from pic_read, buf is partly filled and
 * must be discarded.
 */
pic_status_t pic_read(const pic_device_t *dev, pic_region_t region, uint32_t offset, uint16_t *buf,
                      size_t count);
pic_status_t pic_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                       const uint16_t *buf, size_t count);

/*
 * Read back and compare. Returns PIC_ERR_VERIFY on the first difference and, if mismatch
 * is not NULL, stores its index in buf (the cell is offset + *mismatch). *mismatch is
 * written only on PIC_ERR_VERIFY.
 */
pic_status_t pic_verify(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                        const uint16_t *buf, size_t count, size_t *mismatch);

/*
 * Erase. Bulk takes PIC_ERASE_*; PIC_ERR_UNSUPPORTED if it names a region the device does
 * not have (for example EEPROM on a part with none). PIC_ERASE_ALL is the exception: it
 * erases every region the device has. Row erase works on flash and User ID
 * only (PIC_ERR_UNSUPPORTED for other regions).
 */
pic_status_t pic_bulk_erase(const pic_device_t *dev, uint8_t regions);
pic_status_t pic_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset);

#endif
