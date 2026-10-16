#ifndef STUB_UART_H
#define STUB_UART_H

/* A stand-in for uart.c that records what the command layer sends. */

void stub_uart_clear(void);
const char *stub_uart_output(void); /* NUL-terminated, for text replies */
unsigned stub_uart_length(void);
void stub_uart_inject(const char *bytes, unsigned n);
    /* bytes uart_read_byte() returns next */ /* byte count, for binary replies */

#endif
