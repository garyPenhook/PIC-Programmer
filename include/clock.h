#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/*
 * System clock configuration.
 *
 * The core runs from HSISYS, which is the internal 16 MHz RC oscillator (HSI16)
 * passed through a divider. We divide by 16 to get 1 MHz.
 *
 * SYSCLK, HCLK (AHB bus, CPU) and PCLK (APB bus, peripherals) all run at the same
 * frequency, because the AHB and APB prescalers are left at their reset value of /1.
 *
 * 1 MHz is deliberately under 2 MHz: RM0444 section 4.3.2 requires the system clock
 * to be below 2 MHz before the regulator is put into Low-power run mode, so that
 * mode can be enabled later without touching the clock.
 */
#define CLOCK_SYSCLK_HZ 1000000u

/*
 * The current core clock in Hz. Defined in system_stm32g0xx.c (it starts at the
 * 16 MHz reset value) and updated by clock_init(). Any code that converts time to
 * ticks should use this or CLOCK_SYSCLK_HZ rather than hard-coding a frequency.
 */
extern uint32_t SystemCoreClock;

/*
 * Slow the core to CLOCK_SYSCLK_HZ and update SystemCoreClock.
 * Call once, first thing in platform_init(), before any timer is configured,
 * because timer prescalers are computed from the clock frequency.
 */
void clock_init(void);

#endif
