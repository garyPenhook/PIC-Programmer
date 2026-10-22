#ifndef PIC_PROTO_H
#define PIC_PROTO_H

#include "pic.h"

/*
 * A programming protocol: the code for one programming specification command set. Each
 * lives in src/proto/ and is selected by the device table, never by callers directly.
 *
 * src/pic.c checks pointers, regions and ranges against the device geometry before calling
 * read, write or row_erase (verify is built from read and cell_mask), so a protocol only
 * sees non-NULL buffers, offsets inside the region and count > 0. It also rejects bulk
 * erase masks that name a region the device does not have.
 * The protocol still checks rules of its own (row alignment: PIC_ERR_ALIGN; capabilities
 * such as which regions it can erase by row: PIC_ERR_UNSUPPORTED) and must send nothing
 * on the wires when it rejects a call. Reads return PIC_ERR_PROTOCOL if a payload has bad
 * framing (see icsp8_read).
 *
 * The same command byte can mean different things in different protocols (0xC0 and 0xE0
 * are "Begin Programming" in p16_8bit but "Program Data" in p18_8bit), so never run one
 * protocol against a part from another.
 */
struct pic_proto {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    pic_status_t (*read_device_id)(uint16_t *id);
    pic_status_t (*read_revision_id)(uint16_t *id);
    pic_status_t (*read_dci)(pic_dci_t *dci); /* NULL if the family has no DCI */
    /* Used when read_dci is NULL: a read-only check, beyond the device ID, that the part
     * really speaks this protocol (PIC_ERR_UNSUPPORTED if not). May be NULL only if
     * read_dci is not. */
    pic_status_t (*confirm)(const pic_device_t *dev);
    uint16_t (*cell_mask)(pic_region_t region); /* bits a cell of this region stores */
    pic_status_t (*read)(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                         uint16_t *buf, size_t count);
    pic_status_t (*write)(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                          const uint16_t *buf, size_t count);
    pic_status_t (*bulk_erase)(const pic_device_t *dev, uint8_t regions);
    pic_status_t (*row_erase)(const pic_device_t *dev, pic_region_t region, uint32_t offset);
};

/*
 * 8-bit command set used by recent PIC16 families (PIC16F171XX and others with the same
 * programming specification layout): 14-bit words, word addressing, row write latches.
 */
extern const pic_proto_t pic_proto_p16_8bit;

/*
 * 8-bit command set of the PIC18-Q families: 16-bit words, byte addressing, word-at-a-time
 * programming, page erase.
 */
extern const pic_proto_t pic_proto_p18_8bit;

/*
 * 8-bit command set of the PIC18F2X/4XQ10 (DS40001874H): the same command bytes as
 * p18_8bit, but a different memory map (EEPROM at 31 0000h, no DCI), 16-bit Configuration
 * Words, and a Bulk Erase that takes no payload and picks what it erases from the PC.
 */
extern const pic_proto_t pic_proto_p18_q10;

/*
 * 8-bit command set of the PIC18(L)F26/27/45/46/47/55/56/57K42 (40001886B): the p16_8bit
 * Load Data / Begin Programming commands on the PIC18's byte-addressed memory, with a DCI,
 * row-latch flash writes, 16-bit Configuration Words and a Bulk Erase that picks what it
 * erases from the PC.
 */
extern const pic_proto_t pic_proto_p18_k42;

#endif
