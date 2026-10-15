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
static pic_status_t dci_matches(const pic_device_t *dev)
{
    pic_dci_t dci;
    pic_status_t st = dev->proto->read_dci(&dci);

    if (st != PIC_OK) {
        return st;
    }
    if (dci.erase_words != dev->erase_words || dci.latch_words != dev->latch_words ||
        (uint32_t)dci.user_rows * dci.erase_words != dev->flash_words ||
        dci.eeprom_bytes != dev->eeprom_bytes) {
        return PIC_ERR_UNSUPPORTED;
    }
    return PIC_OK;
}

pic_status_t pic_probe(const pic_device_t **dev_out, uint16_t *device_id, uint16_t *revision_id)
{
    int unknown = 0;      /* a part answered that is not in the table, or disagrees with its row */
    int bad_framing = 0;  /* some read had a bad start or stop bit */

    if (dev_out == NULL) {
        return PIC_ERR_ARG;
    }
    *dev_out = NULL;
    if (device_id != NULL) {
        *device_id = 0u;
    }
    if (revision_id != NULL) {
        *revision_id = 0u;
    }

    for (size_t i = 0; i < sizeof protocols / sizeof protocols[0]; i++) {
        const pic_proto_t *proto = protocols[i];
        uint16_t id = 0u;
        uint16_t rev = 0u;
        const pic_device_t *dev = NULL;

        proto->enter();
        pic_status_t st = proto->read_device_id(&id);
        if (st == PIC_OK) {
            dev = pic_find_device(proto, id);
            if (dev != NULL) {
                st = dci_matches(dev);
                if (st != PIC_OK) {
                    dev = NULL;
                }
            }
            /* An ID that is not in the table, or whose DCI disagrees, is an unknown part;
             * a framing error while checking is a bad read, not an unknown part. */
            unknown |= (dev == NULL && st != PIC_ERR_PROTOCOL && id != 0u) ||
                       st == PIC_ERR_UNSUPPORTED;
        } else {
            id = 0u;
        }
        bad_framing |= st == PIC_ERR_PROTOCOL;
        if (dev != NULL || i == 0u) {
            if (proto->read_revision_id(&rev) != PIC_OK) {
                rev = 0u;
            }
            if (device_id != NULL) {
                *device_id = id;
            }
            if (revision_id != NULL) {
                *revision_id = rev;
            }
        }
        proto->exit();
        if (dev != NULL) {
            *dev_out = dev;
            return PIC_OK;
        }
    }
    if (unknown) {
        return PIC_ERR_UNSUPPORTED;
    }
    return bad_framing ? PIC_ERR_PROTOCOL : PIC_ERR_NO_TARGET;
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

static int region_valid(pic_region_t region)
{
    return region == PIC_REGION_FLASH || region == PIC_REGION_USERID ||
           region == PIC_REGION_CONFIG || region == PIC_REGION_EEPROM;
}

static int dev_ok(const pic_device_t *dev) { return dev != NULL && dev->proto != NULL; }

/* The whole range must lie inside the region; written so that it cannot overflow. */
static int range_ok(const pic_device_t *dev, pic_region_t region, uint32_t offset, size_t count)
{
    uint32_t size = pic_region_size(dev, region);
    return offset < size && count <= size - offset;
}

/*
 * Checks common to read, write and verify. Returns PIC_OK with *nothing_to_do set when the
 * call is valid but count is 0, and PIC_OK with it clear when the transfer should go ahead.
 */
static pic_status_t check_transfer(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                                   const void *buf, size_t count, int *nothing_to_do)
{
    *nothing_to_do = 0;
    if (!dev_ok(dev) || !region_valid(region) || (count != 0u && buf == NULL)) {
        return PIC_ERR_ARG;
    }
    if (count == 0u) {
        *nothing_to_do = 1;
        return PIC_OK;
    }
    return range_ok(dev, region, offset, count) ? PIC_OK : PIC_ERR_RANGE;
}

const char *pic_strerror(pic_status_t status)
{
    switch (status) {
    case PIC_OK:              return "ok";
    case PIC_ERR_ARG:         return "bad argument";
    case PIC_ERR_RANGE:       return "range outside region";
    case PIC_ERR_ALIGN:       return "flash write not on a row boundary";
    case PIC_ERR_UNSUPPORTED: return "not supported by this device";
    case PIC_ERR_NO_TARGET:   return "no target answered";
    case PIC_ERR_MISMATCH:    return "a different device answered";
    case PIC_ERR_PROTOCOL:    return "bad framing on a read";
    case PIC_ERR_VERIFY:      return "verify mismatch";
    default:                  return "unknown error";
    }
}

pic_status_t pic_enter(const pic_device_t *dev)
{
    uint16_t id = 0u;

    if (!dev_ok(dev)) {
        return PIC_ERR_ARG;
    }
    dev->proto->enter();
    pic_status_t st = dev->proto->read_device_id(&id);
    if (st == PIC_OK && id != dev->device_id) {
        st = id == 0u ? PIC_ERR_NO_TARGET : PIC_ERR_MISMATCH;
    } else if (st == PIC_OK) {
        /* Same guard as pic_probe: the ID alone is not proof, so the part's own geometry
         * must agree with the row before any write is allowed near it. */
        st = dci_matches(dev);
        if (st == PIC_ERR_UNSUPPORTED) {
            st = PIC_ERR_MISMATCH;
        }
    }
    if (st != PIC_OK) {
        dev->proto->exit();
    }
    return st;
}

void pic_exit(const pic_device_t *dev)
{
    if (dev_ok(dev)) {
        dev->proto->exit();
    }
}

pic_status_t pic_read_device_id(const pic_device_t *dev, uint16_t *id)
{
    return dev_ok(dev) && id != NULL ? dev->proto->read_device_id(id) : PIC_ERR_ARG;
}

pic_status_t pic_read_revision_id(const pic_device_t *dev, uint16_t *id)
{
    return dev_ok(dev) && id != NULL ? dev->proto->read_revision_id(id) : PIC_ERR_ARG;
}

pic_status_t pic_read_dci(const pic_device_t *dev, pic_dci_t *dci)
{
    return dev_ok(dev) && dci != NULL ? dev->proto->read_dci(dci) : PIC_ERR_ARG;
}

pic_status_t pic_read(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                      uint16_t *buf, size_t count)
{
    int nothing;
    pic_status_t st = check_transfer(dev, region, offset, buf, count, &nothing);

    return st != PIC_OK || nothing ? st : dev->proto->read(dev, region, offset, buf, count);
}

pic_status_t pic_write(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                       const uint16_t *buf, size_t count)
{
    int nothing;
    pic_status_t st = check_transfer(dev, region, offset, buf, count, &nothing);

    return st != PIC_OK || nothing ? st : dev->proto->write(dev, region, offset, buf, count);
}

pic_status_t pic_verify(const pic_device_t *dev, pic_region_t region, uint32_t offset,
                        const uint16_t *buf, size_t count, size_t *mismatch)
{
    int nothing;
    pic_status_t st = check_transfer(dev, region, offset, buf, count, &nothing);

    if (st != PIC_OK || nothing) {
        return st;
    }
    uint16_t mask = dev->proto->cell_mask(region);   /* compare only the bits the part stores */
    uint16_t back[VERIFY_CHUNK];
    for (size_t done = 0; done < count; done += VERIFY_CHUNK) {
        size_t n = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
        st = dev->proto->read(dev, region, offset + (uint32_t)done, back, n);
        if (st != PIC_OK) {
            return st;
        }
        for (size_t i = 0; i < n; i++) {
            if (back[i] != (buf[done + i] & mask)) {
                if (mismatch != NULL) {
                    *mismatch = done + i;
                }
                return PIC_ERR_VERIFY;
            }
        }
    }
    return PIC_OK;
}

pic_status_t pic_bulk_erase(const pic_device_t *dev, uint8_t regions)
{
    if (!dev_ok(dev) || regions == 0u || (regions & ~PIC_ERASE_ALL) != 0u) {
        return PIC_ERR_ARG;
    }
    if (regions == PIC_ERASE_ALL) {   /* "everything" means everything this device has */
        regions = 0u;
        regions |= dev->eeprom_bytes != 0u ? PIC_ERASE_EEPROM : 0u;
        regions |= dev->flash_words != 0u ? PIC_ERASE_FLASH : 0u;
        regions |= dev->userid_words != 0u ? PIC_ERASE_USERID : 0u;
        regions |= dev->config_count != 0u ? PIC_ERASE_CONFIG : 0u;
    }
    if (((regions & PIC_ERASE_EEPROM) != 0u && dev->eeprom_bytes == 0u) ||
        ((regions & PIC_ERASE_FLASH) != 0u && dev->flash_words == 0u) ||
        ((regions & PIC_ERASE_USERID) != 0u && dev->userid_words == 0u) ||
        ((regions & PIC_ERASE_CONFIG) != 0u && dev->config_count == 0u)) {
        return PIC_ERR_UNSUPPORTED;
    }
    return dev->proto->bulk_erase(dev, regions);
}

pic_status_t pic_row_erase(const pic_device_t *dev, pic_region_t region, uint32_t offset)
{
    if (!dev_ok(dev) || !region_valid(region)) {
        return PIC_ERR_ARG;
    }
    if (!range_ok(dev, region, offset, 1u)) {
        return PIC_ERR_RANGE;
    }
    return dev->proto->row_erase(dev, region, offset);
}
