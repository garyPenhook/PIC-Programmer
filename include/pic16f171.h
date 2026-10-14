#ifndef PIC16F171_H
#define PIC16F171_H

#include <stddef.h>
#include <stdint.h>

/*
 * ICSP programming driver for the PIC16F171XX family: PIC16F17114/15/24/25/26,
 * 17144/45/46, 17154/55/56 and 17174/75/76. First targets: PIC16F17146 and PIC16F17124.
 *
 * Source: "PIC16F171XX Family Programming Specification" DS40002266B (a copy is in
 * ~/Documents/Microchip/PIC/16F17146/). Section numbers below refer to that document.
 *
 * Memory is word addressed, 14 bits per word. Flash is written one 32-word row at a time
 * through write latches; Configuration Words and User IDs are written one word at a time;
 * the data EEPROM is written one byte at a time.
 *
 * Call order: pic16f171_enter(), any number of operations, pic16f171_exit().
 *
 * (The PIC18-Q families use similar commands with different meanings, so they have their
 * own driver, pic18q.h. Never run one driver's writes against the other family.)
 */

typedef enum {
    PIC16F171_OK = 0,
    PIC16F171_ERR_ARG = -1,      /* address or size not allowed for this operation */
    PIC16F171_ERR_VERIFY = -2    /* data read back differs from what was written */
} pic16f171_status_t;

/* Word addresses (spec Table 2-1). */
#define PIC16F171_ADDR_FLASH    0x0000u   /* program flash, up to 0x7FFF depending on device */
#define PIC16F171_FLASH_LIMIT   0x8000u   /* first address that is not program flash */
#define PIC16F171_ADDR_USERID   0x8000u   /* 4 words */
#define PIC16F171_USERID_WORDS  4u
#define PIC16F171_ADDR_REVID    0x8005u
#define PIC16F171_ADDR_DEVID    0x8006u
#define PIC16F171_ADDR_CONFIG   0x8007u   /* 5 Configuration Words, 0x8007 to 0x800B */
#define PIC16F171_CONFIG_WORDS  5u
#define PIC16F171_ADDR_DIA      0x8100u   /* device information area, read-only */
#define PIC16F171_ADDR_DCI      0x8200u   /* device configuration information, read-only */
#define PIC16F171_ADDR_EEPROM   0xF000u   /* data EEPROM, one byte per address */
#define PIC16F171_EEPROM_MAX    0x100u    /* 256 bytes, the largest in the family */

#define PIC16F171_ROW_WORDS     32u       /* erase row size and write latch count (DCI) */
#define PIC16F171_BLANK_WORD    0x3FFFu   /* an erased 14-bit word */

/* Bulk Erase region bits (spec 3.2.2). */
#define PIC16F171_ERASE_EEPROM  (1u << 0)
#define PIC16F171_ERASE_FLASH   (1u << 1)
#define PIC16F171_ERASE_USERID  (1u << 2)
#define PIC16F171_ERASE_CONFIG  (1u << 3)
#define PIC16F171_ERASE_ALL     (PIC16F171_ERASE_EEPROM | PIC16F171_ERASE_FLASH | \
                                 PIC16F171_ERASE_USERID | PIC16F171_ERASE_CONFIG)

/* Device Configuration Information (spec Table 2-3), read from 0x8200 to 0x8204. */
typedef struct {
    uint16_t erase_row_words;    /* ERSIZ: 32 */
    uint16_t write_latches;      /* WLSIZ: 32 */
    uint16_t user_rows;          /* URSIZ: rows of flash (128 = 4 KW, 256 = 8 KW, 512 = 16 KW) */
    uint16_t eeprom_bytes;       /* EESIZ: 128 or 256 */
    uint16_t pin_count;          /* PCNT */
} pic16f171_dci_t;

/* Flash size in words from the DCI. */
static inline uint32_t pic16f171_flash_words(const pic16f171_dci_t *dci)
{
    return (uint32_t)dci->user_rows * dci->erase_row_words;
}

/* Enter Program/Verify mode with the low-voltage key. The target must have LVP = 1. */
void pic16f171_enter(void);

/* Leave Program/Verify mode and let the target run. */
void pic16f171_exit(void);

/* Device ID (spec 2.6; reserved bits 13:12 read as 1, so a PIC16F17146 reads 0x30E1). */
uint16_t pic16f171_read_device_id(void);
uint16_t pic16f171_read_revision_id(void);
void pic16f171_read_dci(pic16f171_dci_t *dci);

/* Name for a device ID, or NULL if it is not in the spec's table. */
const char *pic16f171_device_name(uint16_t device_id);

/*
 * Reads. Words are 14 bits. The EEPROM is read with the bytes call.
 * Code protection (CP = 0) makes flash read back as zeros (spec 3.4).
 */
pic16f171_status_t pic16f171_read_words(uint16_t addr, uint16_t *buf, size_t count);
pic16f171_status_t pic16f171_read_eeprom(uint16_t addr, uint8_t *buf, size_t count);

/* Erase. Row erase works on flash (and User ID at 0x8000 to 0x8004); bulk takes ERASE_*. */
pic16f171_status_t pic16f171_bulk_erase(uint8_t regions);
pic16f171_status_t pic16f171_row_erase(uint16_t addr);

/*
 * Writes. Programming can only turn 1s into 0s, so erase first.
 *   flash:               `addr` must be row aligned; a short final row is padded with
 *                        blank words, which leave the rest of the row unchanged
 *   User ID / Config:    written one word at a time
 *   EEPROM:              one byte at a time (write_eeprom)
 */
pic16f171_status_t pic16f171_write_words(uint16_t addr, const uint16_t *words, size_t count);
pic16f171_status_t pic16f171_write_eeprom(uint16_t addr, const uint8_t *bytes, size_t count);

/* Read back and compare. Return PIC16F171_ERR_VERIFY on the first difference. */
pic16f171_status_t pic16f171_verify_words(uint16_t addr, const uint16_t *words, size_t count);
pic16f171_status_t pic16f171_verify_eeprom(uint16_t addr, const uint8_t *bytes, size_t count);

#endif
