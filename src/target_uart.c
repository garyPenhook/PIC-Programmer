#include "target_uart.h"

#include "clock.h"
#define REGS_USART1
#include "regs.h"


#define PIN_TX     4u /* PC4 = USART1_TX, AF1 */
#define PIN_RX     5u /* PC5 = USART1_RX, AF1 */
#define AF_USART1  1u
#define MODE_INPUT 0u
#define MODE_AF    2u

/* Bits as in uart.c (USART_CR1 and USART_ISR with FIFO mode off, RM0444 33.8). */
#define USART_CR1_UE     (1u << 0)
#define USART_CR1_RE     (1u << 2)
#define USART_CR1_TE     (1u << 3)
#define USART_ISR_FE     (1u << 1)
#define USART_ISR_NE     (1u << 2)
#define USART_ISR_ORE    (1u << 3)
#define USART_ISR_RXNE   (1u << 5)
#define USART_ISR_TC     (1u << 6)
#define USART_ISR_TXE    (1u << 7)
#define USART_ISR_ERRORS (USART_ISR_FE | USART_ISR_NE | USART_ISR_ORE)

static void set_mode(uint32_t pin, uint32_t mode)
{
    GPIOC_MODER = (GPIOC_MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
}

static void set_af(uint32_t pin, uint32_t af)
{
    GPIOC_AFRL = (GPIOC_AFRL & ~(0xFu << (pin * 4u))) | (af << (pin * 4u));
}

int target_uart_open(uint32_t baud)
{
    if (baud < TARGET_UART_MIN_BAUD || baud > TARGET_UART_MAX_BAUD) {
        return 0;
    }
    RCC_IOPENR |= RCC_IOPENR_GPIOCEN;
    RCC_APBENR2 |= RCC_APBENR2_USART1EN;
    (void)RCC_APBENR2;

    USART1_CR1 = 0u; /* BRR may only be written while UE = 0 */
    /* USART1 runs from PCLK (RCC_CCIPR USART1SEL reset value, 5.4.21) = SystemCoreClock;
     * oversampling by 16, so BRR = fck / baud (33.5.7), rounded. */
    USART1_BRR = (SystemCoreClock + baud / 2u) / baud;
    set_af(PIN_TX, AF_USART1);
    set_af(PIN_RX, AF_USART1);
    set_mode(PIN_TX, MODE_AF);
    set_mode(PIN_RX, MODE_AF);
    USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    return 1;
}

void target_uart_close(void)
{
    /* Let the last byte leave the shift register (TC) before stopping; bounded in case
     * the clock is off. */
    for (uint32_t i = 0; i < 100000u && (USART1_ISR & USART_ISR_TC) == 0u; i++) {
    }
    USART1_CR1 = 0u;
    set_mode(PIN_TX, MODE_INPUT);
    set_mode(PIN_RX, MODE_INPUT);
}

int target_uart_try_write(uint8_t byte)
{
    if ((USART1_ISR & USART_ISR_TXE) == 0u) {
        return 0;
    }
    USART1_TDR = byte;
    return 1;
}

int target_uart_read(uint8_t *byte)
{
    uint32_t isr = USART1_ISR;

    if ((isr & USART_ISR_ERRORS) != 0u) {
        USART1_ICR = USART_ISR_ERRORS; /* same bit positions in ICR */
        if ((isr & USART_ISR_RXNE) != 0u) {
            (void)USART1_RDR;
        }
        return -1;
    }
    if ((isr & USART_ISR_RXNE) == 0u) {
        return 0;
    }
    *byte = (uint8_t)(USART1_RDR & 0xFFu);
    return 1;
}
