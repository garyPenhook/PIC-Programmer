#include "stub_board.h"

#include <string.h>

#include "adc.h"
#include "target_power.h"
#include "target_uart.h"

static int powered = 1;
static uint32_t vdd = 3300u;
static char rx[4096];
static size_t rx_len;
static size_t rx_pos;
static char tx[8192];
static size_t tx_len;
static int tx_blocked;
static uint32_t baud;

void stub_board_reset(void)
{
    powered = 1;
    vdd = 3300u;
    rx_len = rx_pos = tx_len = 0u;
    tx_blocked = 0;
    baud = 0u;
}

void stub_board_set_vdd(uint32_t mv)
{
    vdd = mv;
}

void stub_target_uart_inject(const char *bytes, size_t n)
{
    memcpy(rx + rx_len, bytes, n);
    rx_len += n;
}

void stub_target_uart_block_tx(int blocked)
{
    tx_blocked = blocked;
}

const char *stub_target_uart_sent(size_t *n)
{
    *n = tx_len;
    return tx;
}

uint32_t stub_target_uart_baud(void)
{
    return baud;
}

void target_power_init(void)
{
}

void target_power_set(int on)
{
    powered = on;
}

int target_power_on(void)
{
    return powered;
}

uint32_t target_vdd_mv(void)
{
    return powered ? vdd : 0u;
}

void adc_init(void)
{
}

uint16_t adc_read(uint32_t channel)
{
    (void)channel;
    return 0u;
}

uint32_t adc_vdda_mv(void)
{
    return 3300u;
}

int target_uart_open(uint32_t b)
{
    if (b < TARGET_UART_MIN_BAUD || b > TARGET_UART_MAX_BAUD) {
        return 0;
    }
    baud = b;
    return 1;
}

void target_uart_close(void)
{
    baud = 0u;
}

int target_uart_try_write(uint8_t byte)
{
    if (tx_blocked || tx_len == sizeof tx) {
        return 0;
    }
    tx[tx_len++] = (char)byte;
    return 1;
}

int target_uart_read(uint8_t *byte)
{
    if (rx_pos == rx_len) {
        return 0;
    }
    *byte = (uint8_t)rx[rx_pos++];
    return 1;
}
