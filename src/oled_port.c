#include "oled_port.h"

#include <stdint.h>

#include "icsp.h"

/*
 * RES on PA8, Arduino D7, a plain I/O on the header (UM2324 Table 12) that nothing else in
 * this firmware uses. GPIOA addresses from RM0444 (base 0x50000000, MODER 0x00, BSRR 0x18).
 *
 * BSRR writes are single stores, so they cannot disturb PA5: TIM16_IRQHandler toggles PA5
 * with a read-modify-write of ODR, but on this single core it runs to completion before
 * main() continues, so it never interleaves with these writes.
 */
#define RCC_IOPENR  (*(volatile uint32_t *)0x40021034u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define GPIOA_BSRR  (*(volatile uint32_t *)0x50000018u)

#define RCC_IOPENR_GPIOAEN (1u << 0)
#define PIN_RES            8u
#define MODE_OUTPUT        1u

/* The datasheet minimum is 3 us low and gives no wait after release; these leave margin. */
#define T_RES_LOW_US   10u
#define T_RES_AFTER_US 100u

void oled_port_reset(void)
{
    RCC_IOPENR |= RCC_IOPENR_GPIOAEN;
    (void)RCC_IOPENR;

    /* Set the level low before making it an output, so the pin never drives high first. */
    GPIOA_BSRR = 1u << (PIN_RES + 16u);
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (PIN_RES * 2u))) | (MODE_OUTPUT << (PIN_RES * 2u));
    icsp_delay_us(T_RES_LOW_US);
    GPIOA_BSRR = 1u << PIN_RES;
    icsp_delay_us(T_RES_AFTER_US);
}
