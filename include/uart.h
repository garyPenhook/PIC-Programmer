#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * Polled USART2 on PA2 (TX) and PA3 (RX), 921600 baud, 8 data bits, no parity, 1 stop bit.
 * On the Nucleo-G071RB these pins go to the ST-LINK virtual COM port, so the PC sees it as
 * a serial port. No interrupts and no buffering: a received byte must be read before the
 * next one arrives, so the host must not send while a command is running.
 *
 * PA2/PA3 reach the ST-LINK virtual COM port through solder bridges SB16/SB18, which are on
 * by default (UM2324 6.9, Table 8). The highest rate the ST-LINK accepts was not found in
 * UM2324. If 921600 does not run cleanly on the board, lower UART_BAUD (460800, 230400 or
 * 115200) and set the PC side to match.
 */
#define UART_BAUD 921600u

/*
 * Rate used instead when clock_init() fell back to 16 MHz. 921600 needs a divisor of 17.4
 * there, so the closest (17) runs 2.1 percent fast, and halving the oversampling does not
 * help (its divisor has to be even: 34 gives the same error). Half the rate needs 34.7,
 * so 35 is only 0.8 percent off, and a host that finds nothing at UART_BAUD can try this
 * one (`picprog status` does) and so learn about the fault.
 */
#define UART_BAUD_FALLBACK (UART_BAUD / 2u)

/*
 * Configure the pins and the USART. Call after clock_init(): the baud rate divisor is
 * computed from SystemCoreClock, and the rate is UART_BAUD_FALLBACK if SystemCoreClock is below CLOCK_SYSCLK_HZ.
 */
void uart_init(void);

/* Send one byte, waiting until the transmitter can take it. */
void uart_write_byte(uint8_t byte);

/* Send one byte only if the transmitter can take it now. Returns 1 if sent. */
int uart_try_write_byte(uint8_t byte);

/*
 * Fetch a received byte without waiting. Returns 1 and stores it in *byte, 0 if nothing
 * has arrived, or -1 if the receiver reported an overrun, framing or noise error (the
 * affected byte is discarded and the error flags are cleared).
 */
int uart_read_byte(uint8_t *byte);

#endif
