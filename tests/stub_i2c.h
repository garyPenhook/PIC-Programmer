#ifndef STUB_I2C_H
#define STUB_I2C_H

#include <stddef.h>
#include <stdint.h>

/*
 * A stand-in for i2c.c that records every write transfer. Only the device at the address
 * set with stub_i2c_attach() acknowledges; with none attached every write is a NACK, as
 * on a board without a display.
 */
#define STUB_I2C_MAX_TRANSFERS 64u
#define STUB_I2C_MAX_BYTES     260u

typedef struct {
    uint8_t addr;
    size_t len; /* bytes after the address, including `first` */
    uint8_t bytes[STUB_I2C_MAX_BYTES];
} stub_i2c_transfer_t;

void stub_i2c_reset(void);
void stub_i2c_attach(uint8_t addr7);     /* 0 for no device */
void stub_i2c_fail_after(int transfers); /* NACK every write after this many; -1 = never */
size_t stub_i2c_count(void);
int stub_oled_resets(void); /* oled_port_reset() calls since stub_i2c_reset() */
size_t stub_oled_reset_at(void);
    /* transfer count when the last reset happened */ /* transfers recorded, including NACKed ones
                                                       */
const stub_i2c_transfer_t *stub_i2c_get(size_t index);

#endif
