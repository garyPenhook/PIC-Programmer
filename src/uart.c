#include "uart.h"

#include "clock.h"
#define REGS_USART2
#include "regs.h"


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
     * oversampling by 16, baud = fck / USARTDIV (33.5.7) and BRR holds USARTDIV itself;
     * round to nearest. At 921600 baud: 64 MHz / 921600 = 69.4 gives 69, which runs
     * 0.6 percent fast. On the 16 MHz fallback it gives 17, 2.1 percent fast, which may be
     * too far off for the receiver at the other end. BRR can only be written while
     * UE = 0, which it is after reset.
     */
    /* The rate follows the clock actually running, not the order of the calls. */
    uint32_t baud = SystemCoreClock >= CLOCK_SYSCLK_HZ ? UART_BAUD : UART_BAUD_FALLBACK;

    USART2_BRR = (SystemCoreClock + baud / 2u) / baud;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void uart_write_byte(uint8_t byte)
{
    while ((USART2_ISR & USART_ISR_TXE) == 0u) {
    }
    USART2_TDR = byte;
}

int uart_try_write_byte(uint8_t byte)
{
    if ((USART2_ISR & USART_ISR_TXE) == 0u) {
        return 0;
    }
    USART2_TDR = byte;
    return 1;
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
