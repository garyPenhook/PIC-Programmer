#ifndef ICSP8_H
#define ICSP8_H

#include <stdint.h>

/*
 * Bit layer shared by the 8-bit-command protocols (p16_8bit, p18_8bit). Both
 * specifications describe the same framing; only the meaning of the commands differs.
 *
 *   - Low-voltage entry: MCLR low, then the 32-bit key 0x4D434850 ("MCHP"), MSb first.
 *     The last bit is a don't-care but its clock is required.
 *   - MSb first; data changes on the rising clock edge and is latched on the falling edge.
 *   - Commands are 8 bits. A payload is 24 clocks: start bit (0), pad zeros, data MSb
 *     first, stop bit (0), so the value sits shifted left by one. Reads use the same layout.
 *
 * Timing (identical limits in both specifications' Table 4-1, values used here are at or
 * beyond them): clock high/low >= 100 ns, 1 us used; delay between command and data
 * >= 1.0 us, 2 us used; exit >= 1 us, 1 ms used. The entry hold differs per family, so the
 * caller passes it.
 */

void icsp8_enter(uint32_t hold_us);
void icsp8_exit(void);

/* Send an 8-bit command, then the inter-item delay. */
void icsp8_command(uint8_t cmd);

/* Send a 24-clock payload carrying `value`, then the inter-item delay. */
void icsp8_payload(uint32_t value);

/* Send a read command and clock in its 24-clock payload; returns the data field. */
uint32_t icsp8_read(uint8_t cmd);

#endif
