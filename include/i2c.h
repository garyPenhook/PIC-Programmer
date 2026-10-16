#ifndef I2C_H
#define I2C_H

#include <stddef.h>
#include <stdint.h>

/*
 * Polled I2C1 controller on PB8 (SCL) and PB9 (SDA), the Arduino D15 and D14 pins of the
 * Nucleo-G071RB (UM2324 Table 12). Write-only, 7-bit addresses, about 380 kHz.
 *
 * Every wait is bounded, so a missing device, missing pull-ups or a stuck bus returns an
 * error instead of hanging the programmer.
 *
 * Build with I2C_IGNORE_ACK=1 (CMake option of the same name) for a device that never
 * acknowledges, such as an SSD1309 module whose SDAOUT pin is not connected (SSD1309
 * datasheet 8.1.5: "When SDAOUT pin is disconnected, the acknowledgement signal will be
 * ignored"). The I2C1 peripheral cannot do that: on a NACK it always sends STOP and ends
 * the transfer (RM0444 32.4.8, controller transmitter). So that build drives the same two
 * pins in software instead, at about 250 kHz, clocks the ninth bit without reading it,
 * and every write returns I2C_OK: a missing device can then no longer be detected.
 */
#ifndef I2C_IGNORE_ACK
#define I2C_IGNORE_ACK 0
#endif

typedef enum {
    I2C_OK = 0,
    I2C_NACK = -1,    /* no device answered at that address, or it refused a byte */
    I2C_TIMEOUT = -2, /* the bus stayed busy or a flag never came; the peripheral was reset */
} i2c_status_t;

/* Configure the pins and the peripheral. Call after clock_init(), and with I2C_IGNORE_ACK
 * also after icsp_init() (the software bus is timed with icsp_delay_us()). */
void i2c_init(void);

/*
 * One write transfer: START, address, `first`, then `len` bytes of data, STOP. `first` is
 * separate so a caller can put a control byte in front of a buffer without copying it.
 * Any length is allowed (transfers over 255 bytes use NBYTES reload).
 */
i2c_status_t i2c_write(uint8_t addr7, uint8_t first, const uint8_t *data, size_t len);

#endif
