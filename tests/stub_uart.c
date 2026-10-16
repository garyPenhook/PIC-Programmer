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

static char input[8192];
static unsigned input_len;
static unsigned input_pos;

void stub_uart_inject(const char *bytes, unsigned n)
{
    for (unsigned i = 0; i < n && input_len < sizeof input; i++) {
        input[input_len++] = bytes[i];
    }
}

int uart_read_byte(uint8_t *byte)
{
    if (input_pos == input_len) {
        input_pos = input_len = 0;
        return 0;
    }
    *byte = (uint8_t)input[input_pos++];
    return 1;
}

int uart_try_write_byte(uint8_t byte)
{
    uart_write_byte(byte);
    return 1;
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
