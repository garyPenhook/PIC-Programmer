#include "stub_i2c.h"

#include <string.h>

#include "i2c.h"

static stub_i2c_transfer_t log_[STUB_I2C_MAX_TRANSFERS];
static size_t count;
static uint8_t attached;
static int fail_after = -1;
static int acked;

void stub_i2c_reset(void)
{
    count = 0;
    attached = 0;
    fail_after = -1;
    acked = 0;
}

void stub_i2c_attach(uint8_t addr7)
{
    attached = addr7;
}

void stub_i2c_fail_after(int transfers)
{
    fail_after = transfers;
    acked = 0;
}

size_t stub_i2c_count(void)
{
    return count;
}

const stub_i2c_transfer_t *stub_i2c_get(size_t index)
{
    return index < count ? &log_[index] : NULL;
}

void i2c_init(void)
{
}

i2c_status_t i2c_write(uint8_t addr7, uint8_t first, const uint8_t *data, size_t len)
{
    if (count < STUB_I2C_MAX_TRANSFERS) {
        stub_i2c_transfer_t *t = &log_[count++];
        t->addr = addr7;
        t->len = len + 1u;
        t->bytes[0] = first;
        memcpy(t->bytes + 1, data, len < STUB_I2C_MAX_BYTES - 1u ? len : STUB_I2C_MAX_BYTES - 1u);
    }
    if (attached == 0u || addr7 != attached) {
        return I2C_NACK;
    }
    if (fail_after >= 0 && acked >= fail_after) {
        return I2C_NACK;
    }
    acked++;
    return I2C_OK;
}
