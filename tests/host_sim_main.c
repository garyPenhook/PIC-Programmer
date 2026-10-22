/*
 * The firmware's serial command layer (src/cmd.c) running on the PC, in front of the
 * simulated PIC16F171XX (sim_p16_8bit.c). stdin and stdout stand in for the UART, so the
 * host tool's tests (host/tests) can drive the real command parser end to end.
 *
 * Usage: host_sim [device id in hex, default 30E1]
 *
 * Built with -DSIM_HEADER='"sim_<protocol>.h"' and the matching tests/sim_<protocol>.c to
 * run another protocol's model (host/tests/conftest.py builds one binary per protocol).
 */
#include <stdio.h>
#include <stdlib.h>

#include "cmd.h"
#include "platform.h"
#ifndef SIM_HEADER
#define SIM_HEADER "sim_p16_8bit.h"
#endif
#include SIM_HEADER
#include "uart.h"

void uart_init(void)
{
}

void uart_write_byte(uint8_t byte)
{
    putchar(byte);
}

int uart_try_write_byte(uint8_t byte)
{
    putchar(byte);
    return 1;
}

int uart_read_byte(uint8_t *byte)
{
    (void)byte;
    return 0;
}

void platform_watchdog_kick(void)
{
}

int main(int argc, char **argv)
{
    int c;

    sim_select_device(argc > 1 ? (uint16_t)strtoul(argv[1], NULL, 16) : 0x30E1u);
    sim_reset();
    while ((c = getchar()) != EOF) {
        cmd_feed((uint8_t)c);
        fflush(stdout); /* a command's whole reply is out before the next byte is read */
    }
    return 0;
}
