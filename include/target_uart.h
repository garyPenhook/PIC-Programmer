#ifndef TARGET_UART_H
#define TARGET_UART_H

#include <stdint.h>

/*
 * USART1 to the target's serial port, for BRIDGE (bridge.h): Arduino D1 (PC4, USART1_TX,
 * AF1) to the target's RX, D0 (PC5, USART1_RX, AF1) from the target's TX (UM2324 Table 12,
 * DS12232 Table 15). 8 data bits, no parity, 1 stop bit.
 */
#define TARGET_UART_MIN_BAUD 1200u
#define TARGET_UART_MAX_BAUD 1000000u

/* Configure the pins and USART1. Returns 0 if baud is out of range. */
int target_uart_open(uint32_t baud);

/* Back to inputs, so the TX pin no longer drives the target. */
void target_uart_close(void);

/* Send a byte if the transmitter can take one now. Returns 1 if sent. */
int target_uart_try_write(uint8_t byte);

/* Fetch a received byte: 1 = got one, 0 = nothing, -1 = receive error (byte dropped). */
int target_uart_read(uint8_t *byte);

#endif
