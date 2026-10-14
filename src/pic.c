#include "pic.h"

#include "pic_proto.h"

/* Cells read per protocol call while verifying; bounds the stack used. */
#define VERIFY_CHUNK 32u

/* Tried in this order by pic_probe(): the first targets (PIC16F171XX) first. */
static const pic_proto_t *const protocols[] = {
    &pic_proto_p16_8bit,
    &pic_proto_p18_8bit,
};

const pic_device_t *pic_find_device(const pic_proto_t *proto, uint16_t device_id)
{
    for (size_t i = 0; i < pic_device_count; i++) {
        if (pic_devices[i].proto == proto && pic_devices[i].device_id == device_id) {
            return &pic_devices[i];
        }
    }
    return NULL;
}

/*
 * A device ID alone is not proof: a part from another protocol answers these read
 * commands too, with whatever its memory holds at that address. Its own DCI must also
 * agree with the table row before any write command of this protocol is allowed near it.
 */
static int dci_matches(const pic_device_t *dev)
{
    pic_dci_t dci;

    dev->proto->read_dci(&dci);
    return dci.erase_words == dev->erase_words &&
           dci.latch_words == dev->latch_words &&
           (uint32_t)dci.user_rows * dci.erase_words == dev->flash_words &&
           dci.eeprom_bytes == dev->eeprom_bytes;
}

const pic_device_t *pic_probe(uint16_t *device_id, uint16_t *revision_id)
{
    for (size_t i = 0; i < sizeof protocols / sizeof protocols[0]; i++) {
        const pic_proto_t *proto = protocols[i];

        proto->enter();
        uint16_t id = proto->read_device_id();
        const pic_device_t *dev = pic_find_device(proto, id);
        if (dev != NULL && !dci_matches(dev)) {
            dev = NULL;
        }
        if (dev != NULL || i == 0u) {
            *device_id = id;
            *revision_id = proto->read_revision_id();
        }
        proto->exit();
        if (dev != NULL) {
            return dev;
        }
    }
    return NULL;
}

uint32_t pic_region_size(const pic_device_t *dev, pic_region_t region)
{
    switch (region) {
    case PIC_REGION_FLASH:  return dev->flash_words;
    case PIC_REGION_USERID: return dev->userid_words;
    case PIC_REGION_CONFIG: return dev->config_count;
    case PIC_REGION_EEPROM: return dev->eeprom_bytes;
    default:                return 0u;
    }
}

/* The whole range must lie inside the region; written so that it cannot overflow. */
static int range_ok(const pic_device_t *dev, pic_region_t region, uint32_t offset, size_t count)
{
    uint32_t size = pic_region_size(dev, region);
    return offset < size && count <= size - offset;
}

void pic_enter(const pic_device_t *dev) { dev->proto->enter(); }
void pic_exit(const pic_device_t *dev) { dev->proto->exit(); }
uint16_t pic_read_device_id(const pic_device_t *dev) { return dev->proto->read_device_id(); }
uint16_t pic_read_revision_id(const pic_device_t *dev) { return dev->proto->read_revision_id(); }
void pic_read_dci(const pic_device_t *dev, pic_dci_t *dci) { dev->proto->read_dci(dci); }

pic_status_t pic_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                      uint16_t *buf, size_t count)
{
    if (count == 0u) {
        return PIC_OK;
    }
    if (!range_ok(dev, region, offset, count)) {
        return PIC_ERR_ARG;
    }
    return dev->proto->read(dev, region, offset, buf, count);
}

pic_status_t pic_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                       const uint16_t *buf, size_t count)
{
    if (count == 0u) {
        return PIC_OK;
    }
    if (!range_ok(dev, region, offset, count)) {
        return PIC_ERR_ARG;
    }
    return dev->proto->write(dev, region, offset, buf, count);
}

pic_status_t pic_verify(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                        const uint16_t *buf, size_t count)
{
    if (count == 0u) {
        return PIC_OK;
    }
    if (!range_ok(dev, region, offset, count)) {
        return PIC_ERR_ARG;
    }
    uint16_t mask = dev->proto->cell_mask(region);   /* compare only the bits the part stores */
    uint16_t back[VERIFY_CHUNK];
    for (size_t done = 0; done < count; done += VERIFY_CHUNK) {
        size_t n = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
        pic_status_t st = dev->proto->read(dev, region, offset + (uint32_t)done, back, n);
        if (st != PIC_OK) {
            return st;
        }
        for (size_t i = 0; i < n; i++) {
            if (back[i] != (buf[done + i] & mask)) {
                return PIC_ERR_VERIFY;
            }
        }
    }
    return PIC_OK;
}

pic_status_t pic_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    if (regions == 0u || (regions & ~PIC_ERASE_ALL) != 0u) {
        return PIC_ERR_ARG;
    }
    return dev->proto->bulk_erase(dev, regions);
}

pic_status_t pic_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    if (!range_ok(dev, region, offset, 1u)) {
        return PIC_ERR_ARG;
    }
    return dev->proto->row_erase(dev, region, offset);
}
