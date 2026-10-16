#include "stub_uart.h"

#include "platform.h"
#include "uart.h"

static char output[16384];
static unsigned length;

void uart_init(void)
{
}

void uart_write_byte(uint8_t byte)
{
    if (length < sizeof output - 1u) {
        output[length++] = (char)byte;
        output[length] = '\0';
    }
}

int uart_read_byte(uint8_t *byte)
{
    (void)byte;
    return 0;
}

void stub_uart_clear(void)
{
    length = 0;
    output[0] = '\0';
}

const char *stub_uart_output(void)
{
    return output;
}

unsigned stub_uart_length(void)
{
    return length;
}

/* The command layer kicks the watchdog during long reads; there is none on the PC. */
void platform_watchdog_kick(void)
{
}
