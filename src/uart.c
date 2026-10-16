#include "uart.h"

#include "clock.h"

/*
 * Register addresses (RM0444). USART2 base is 0x40004400 (Table 6); offsets from 33.8:
 * CR1 0x00, BRR 0x0C, ISR 0x1C, ICR 0x20, RDR 0x24, TDR 0x28. RCC_APBENR1 is at RCC base
 * + 0x3C (5.4.15) and GPIOA_AFRL at GPIOA base + 0x20 (7.5.9).
 */
#define USART2_CR1  (*(volatile uint32_t *)0x40004400u)
#define USART2_BRR  (*(volatile uint32_t *)0x4000440Cu)
#define USART2_ISR  (*(volatile uint32_t *)0x4000441Cu)
#define USART2_ICR  (*(volatile uint32_t *)0x40004420u)
#define USART2_RDR  (*(volatile uint32_t *)0x40004424u)
#define USART2_TDR  (*(volatile uint32_t *)0x40004428u)
#define RCC_IOPENR  (*(volatile uint32_t *)0x40021034u)
#define RCC_APBENR1 (*(volatile uint32_t *)0x4002103Cu)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define GPIOA_AFRL  (*(volatile uint32_t *)0x50000020u)

#define RCC_IOPENR_GPIOAEN   (1u << 0)
#define RCC_APBENR1_USART2EN (1u << 17)

#define PIN_TX    2u /* PA2 = USART2_TX, AF1 (datasheet Table 13) */
#define PIN_RX    3u /* PA3 = USART2_RX, AF1 */
#define AF_USART2 1u
#define MODE_AF   2u

/* USART_CR1 with FIFO mode off (the reset state): UE enable, RE and TE receiver and
 * transmitter. Word length, parity and oversampling stay at their reset values: 8 data
 * bits, no parity, oversampling by 16. */
#define USART_CR1_UE (1u << 0)
#define USART_CR1_RE (1u << 2)
#define USART_CR1_TE (1u << 3)

/* USART_ISR with FIFO mode off. */
#define USART_ISR_FE     (1u << 1)
#define USART_ISR_NE     (1u << 2)
#define USART_ISR_ORE    (1u << 3)
#define USART_ISR_RXNE   (1u << 5)
#define USART_ISR_TXE    (1u << 7)
#define USART_ISR_ERRORS (USART_ISR_FE | USART_ISR_NE | USART_ISR_ORE)

/* USART_ICR: writing 1 clears the matching ISR flag (same bit positions for these three). */
#define USART_ICR_ERRORS (USART_ISR_ERRORS)

/* Replace the 2-bit field (MODER) or 4-bit field (AFRL) for `pin`. */
static void set_mode(uint32_t pin, uint32_t mode)
{
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
}

static void set_af(uint32_t pin, uint32_t af)
{
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFu << (pin * 4u))) | (af << (pin * 4u));
}

void uart_init(void)
{
    RCC_IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC_APBENR1 |= RCC_APBENR1_USART2EN;
    (void)RCC_APBENR1;

    /* Alternate function first, then switch the pins over, so they never drive a
     * wrong level in between. */
    set_af(PIN_TX, AF_USART2);
    set_af(PIN_RX, AF_USART2);
    set_mode(PIN_TX, MODE_AF);
    set_mode(PIN_RX, MODE_AF);

    /*
     * USART2 runs from PCLK (the RCC_CCIPR USART2SEL reset value, 5.4.21), and PCLK equals
     * SystemCoreClock because clock_init() leaves the APB prescaler at /1. With
     * oversampling by 16, BRR is the divisor fck / baud itself; round to nearest
     * (64 MHz / 115200 = 555.6 gives 556, an error of 0.08 percent). BRR can only be
     * written while UE = 0, which it is after reset.
     */
    USART2_BRR = (SystemCoreClock + UART_BAUD / 2u) / UART_BAUD;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void uart_write_byte(uint8_t byte)
{
    while ((USART2_ISR & USART_ISR_TXE) == 0u) {
    }
    USART2_TDR = byte;
}

int uart_read_byte(uint8_t *byte)
{
    uint32_t isr = USART2_ISR;

    if ((isr & USART_ISR_ERRORS) != 0u) {
        /* Drop the affected byte (if one is waiting) and clear the flags, otherwise an
         * overrun would keep the receiver stuck. */
        USART2_ICR = USART_ICR_ERRORS;
        if ((isr & USART_ISR_RXNE) != 0u) {
            (void)USART2_RDR;
        }
        return -1;
    }
    if ((isr & USART_ISR_RXNE) == 0u) {
        return 0;
    }
    *byte = (uint8_t)(USART2_RDR & 0xFFu);
    return 1;
}
