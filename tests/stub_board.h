#ifndef STUB_BOARD_H
#define STUB_BOARD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Stand-ins for the board-level drivers on the PC: target power and VDD (target_power.h),
 * VDDA (adc.h) and the target serial port (target_uart.h).
 */
void stub_board_reset(void); /* power on, VDD 3300 mV, empty target UART */
void stub_board_set_vdd(uint32_t mv);
void stub_board_set_faults(int clock_fell_back, int adc_dead); /* start-up faults */
void stub_target_uart_inject(const char *bytes, size_t n); /* bytes the target sends */
void stub_target_uart_block_tx(int blocked);  /* 1 = the transmitter never takes a byte */
const char *stub_target_uart_sent(size_t *n); /* bytes sent to the target */
uint32_t stub_target_uart_baud(void);         /* baud of the last open, 0 if never/closed */

#endif
