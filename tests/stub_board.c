#include "stub_board.h"

#include <string.h>

#include "adc.h"
#include "clock.h"
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
static int adc_failed;
static int clock_fallback;
static int watchdog_reset;

void stub_board_reset(void)
{
    powered = 1;
    vdd = 3300u;
    rx_len = rx_pos = tx_len = 0u;
    tx_blocked = 0;
    baud = 0u;
    adc_failed = clock_fallback = watchdog_reset = 0;
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

int target_vdd_mv(uint32_t *mv)
{
    if (adc_failed) {
        return 0;
    }
    *mv = powered ? vdd : 0u;
    return 1;
}

void adc_init(void)
{
}

int adc_ready(void)
{
    return !adc_failed;
}

int adc_read(uint32_t channel, uint16_t *value)
{
    (void)channel;
    if (adc_failed) {
        return 0;
    }
    *value = 0u;
    return 1;
}

int adc_vdda_mv(uint32_t *mv)
{
    *mv = 3300u;
    return !adc_failed;
}

int platform_watchdog_reset(void)
{
    return watchdog_reset;
}

void stub_board_set_watchdog_reset(int reset)
{
    watchdog_reset = reset;
}

clock_status_t clock_status(void)
{
    return clock_fallback ? CLOCK_ERR_PLL : CLOCK_OK;
}

void stub_board_set_faults(int clock_fell_back, int adc_dead)
{
    clock_fallback = clock_fell_back;
    adc_failed = adc_dead;
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
