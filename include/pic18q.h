#ifndef PIC18Q_H
#define PIC18Q_H

#include <stddef.h>
#include <stdint.h>

/*
 * ICSP programming driver for the PIC18-Q families (PIC18-Q20, Q24, Q41, Q43, ...).
 *
 * Sources: the "PIC18-Q24/Q41/Q43 Family Programming Specification" documents (command
 * set, entry sequence, timing) and the PIC18F06/16Q41 data sheet DS40002214 (memory map,
 * device IDs). The Q41 programming specification is DS40002143.
 *
 * These devices have a 16-bit program word, byte addressing (a flash word occupies two
 * addresses), no row write latches (word-at-a-time programming), and page erase.
 *
 * Call order: pic18q_enter(), any number of operations, pic18q_exit().
 * Nothing here touches the SAFLOCK or ICSPDIS bits: both need the Program Access Enable
 * command, which this driver deliberately never sends, so they cannot be set by accident.
 */

typedef enum {
    PIC18Q_OK = 0,
    PIC18Q_ERR_ARG = -1,      /* address or size not allowed for this operation */
    PIC18Q_ERR_VERIFY = -2    /* data read back differs from what was written */
} pic18q_status_t;

/* Address map (byte addresses). */
#define PIC18Q_ADDR_FLASH   0x000000u   /* program flash, 16-bit words, even addresses */
#define PIC18Q_ADDR_USERID  0x200000u   /* 32 words, 0x200000 to 0x20003F */
#define PIC18Q_ADDR_CONFIG  0x300000u   /* configuration bytes (10 on Q41, 13 on Q24) */
#define PIC18Q_ADDR_EEPROM  0x380000u   /* data EEPROM, bytes */
#define PIC18Q_ADDR_DCI     0x3C0000u   /* device configuration information, read-only */
#define PIC18Q_ADDR_REVID   0x3FFFFCu   /* 16-bit revision ID */
#define PIC18Q_ADDR_DEVID   0x3FFFFEu   /* 16-bit device ID */

#define PIC18Q_USERID_WORDS 32u
#define PIC18Q_CONFIG_MAX   0x20u       /* guard: config writes below ADDR_CONFIG + this */

/* Bulk Erase region bits (payload of the Bulk Erase command). */
#define PIC18Q_ERASE_EEPROM (1u << 0)
#define PIC18Q_ERASE_FLASH  (1u << 1)
#define PIC18Q_ERASE_USERID (1u << 2)
#define PIC18Q_ERASE_CONFIG (1u << 3)
#define PIC18Q_ERASE_ALL    (PIC18Q_ERASE_EEPROM | PIC18Q_ERASE_FLASH | \
                             PIC18Q_ERASE_USERID | PIC18Q_ERASE_CONFIG)

/* Values read from the Device Configuration Information region (3C 0000h ...). */
typedef struct {
    uint16_t erase_page_words;   /* ERSIZ: flash words per erase page */
    uint16_t write_latches;      /* WLSIZ: 0 means word-at-a-time programming */
    uint16_t user_pages;         /* URSIZ: number of user-erasable pages */
    uint16_t eeprom_bytes;       /* EESIZ */
    uint16_t pin_count;          /* PCNT */
} pic18q_dci_t;

/* Enter Program/Verify mode with the low-voltage key. The target must have LVP = 1. */
void pic18q_enter(void);

/* Leave Program/Verify mode and let the target run. */
void pic18q_exit(void);

uint16_t pic18q_read_device_id(void);
uint16_t pic18q_read_revision_id(void);
void pic18q_read_dci(pic18q_dci_t *dci);

/* Name for a device ID, or NULL if it is not one we have a verified ID for. */
const char *pic18q_device_name(uint16_t device_id);

/* Reads. `addr` is a byte address; words must be on even addresses. */
pic18q_status_t pic18q_read_words(uint32_t addr, uint16_t *buf, size_t count);
pic18q_status_t pic18q_read_bytes(uint32_t addr, uint8_t *buf, size_t count);

/* Erase. Page erase works on flash and User ID only; bulk erase takes PIC18Q_ERASE_*. */
pic18q_status_t pic18q_bulk_erase(uint8_t regions);
pic18q_status_t pic18q_page_erase(uint32_t addr);

/*
 * Writes. Programming can only turn 1s into 0s, so erase first. Words go to flash or
 * the User ID area; bytes go to the EEPROM or the configuration bytes.
 */
pic18q_status_t pic18q_write_words(uint32_t addr, const uint16_t *words, size_t count);
pic18q_status_t pic18q_write_bytes(uint32_t addr, const uint8_t *bytes, size_t count);

/* Read back and compare. Return PIC18Q_ERR_VERIFY on the first difference. */
pic18q_status_t pic18q_verify_words(uint32_t addr, const uint16_t *words, size_t count);
pic18q_status_t pic18q_verify_bytes(uint32_t addr, const uint8_t *bytes, size_t count);

#endif
