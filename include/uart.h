#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * Polled USART2 on PA2 (TX) and PA3 (RX), 115200 baud, 8 data bits, no parity, 1 stop bit.
 * On the Nucleo-G071RB these pins go to the ST-LINK virtual COM port, so the PC sees it as
 * a serial port. No interrupts and no buffering: a received byte must be read before the
 * next one arrives, so the host must not send while a command is running.
 */
#define UART_BAUD 115200u

/*
 * Configure the pins and the USART. Call after clock_init(): the baud rate divisor is
 * computed from SystemCoreClock.
 */
void uart_init(void);

/* Send one byte, waiting until the transmitter can take it. */
void uart_write_byte(uint8_t byte);

/*
 * Fetch a received byte without waiting. Returns 1 and stores it in *byte, 0 if nothing
 * has arrived, or -1 if the receiver reported an overrun, framing or noise error (the
 * affected byte is discarded and the error flags are cleared).
 */
int uart_read_byte(uint8_t *byte);

#endif
