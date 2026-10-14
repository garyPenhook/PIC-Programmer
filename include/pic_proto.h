#ifndef PIC_PROTO_H
#define PIC_PROTO_H

#include "pic.h"

/*
 * A programming protocol: the code for one programming specification command set. Each
 * lives in src/proto/ and is selected by the device table, never by callers directly.
 *
 * src/pic.c checks every range against the device geometry before calling read, write or
 * row_erase (verify is built from read and cell_mask), so a protocol only sees offsets
 * inside the region and count > 0.
 * The protocol still checks rules of its own (row alignment, which regions it can erase
 * by row) and must send nothing on the wires when it rejects a call.
 *
 * The same command byte can mean different things in different protocols (0xC0 and 0xE0
 * are "Begin Programming" in p16_8bit but "Program Data" in p18_8bit), so never run one
 * protocol against a part from another.
 */
struct pic_proto {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    uint16_t (*read_device_id)(void);
    uint16_t (*read_revision_id)(void);
    void (*read_dci)(pic_dci_t *dci);
    uint16_t (*cell_mask)(pic_region_t region);   /* bits a cell of this region stores */
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

#endif
