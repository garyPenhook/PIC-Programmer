#include "clock.h"

/*
 * RCC_CR - Clock control register (RM0444 5.4.1).
 * RCC is at 0x40021000 and RCC_CR is at offset 0x00.
 * Reset value is 0x0500: HSION (bit 8) and HSIRDY (bit 10) are set, so the
 * 16 MHz oscillator is already running and stable at reset.
 */
#define RCC_CR              (*(volatile uint32_t *)0x40021000u)

/*
 * HSIDIV, RCC_CR bits 13:11, divides HSI16 to produce HSISYS (the system clock).
 * Encoding is a power of two: 000 = /1, 001 = /2, 010 = /4, 011 = /8, 100 = /16,
 * 101 = /32, 110 = /64, 111 = /128.
 * 16 MHz / 16 = 1 MHz, so we write 100b.
 */
#define RCC_CR_HSIDIV_MASK  (7u << 11)
#define RCC_CR_HSIDIV_16    (4u << 11)

void clock_init(void)
{
    /*
     * After reset SYSCLK already comes from HSISYS (RCC_CFGR.SW = 000), so there is
     * no clock-source switch to perform or wait for. We only change the divider.
     *
     * Read-modify-write: clear the three HSIDIV bits, then set the new value. This
     * leaves HSION and the other RCC_CR bits (HSE, PLL, ...) as they were.
     *
     * The core slows down as soon as this write lands. That is safe here because
     * nothing time-dependent has been started yet.
     */
    RCC_CR = (RCC_CR & ~RCC_CR_HSIDIV_MASK) | RCC_CR_HSIDIV_16;

    /*
     * Keep the CMSIS-style global in step with the hardware. Nothing reads the clock
     * back from the registers, so this variable is the only record of the frequency.
     */
    SystemCoreClock = CLOCK_SYSCLK_HZ;
}
