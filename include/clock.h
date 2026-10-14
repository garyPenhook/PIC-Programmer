#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/*
 * System clock configuration: the fastest the STM32G071RB allows.
 *
 * SYSCLK is 64 MHz, taken from the PLL's R output with the PLL fed by the internal
 * 16 MHz oscillator (HSI16):
 *
 *     HSI16 (16 MHz) -> /M (1) -> 16 MHz -> x N (8) -> VCO 128 MHz -> /R (2) -> 64 MHz
 *
 * Limits this respects (RM0444 5.4.4 and datasheet Table 43, voltage Range 1):
 *   - PLL input after /M: 2.66 to 16 MHz   (16 MHz)
 *   - VCO output:         96 to 344 MHz    (128 MHz)
 *   - PLLRCLK output:     64 MHz maximum   (64 MHz)
 * Range 1 is the reset default, which is the range 64 MHz needs (RM0444 Table 13).
 *
 * HCLK (CPU, AHB bus) and PCLK (APB bus, peripherals) are both left at /1, so all
 * three run at 64 MHz. The APB limit is also 64 MHz. Because the APB prescaler is /1,
 * the timers run at PCLK too (RM0444 5.2.13).
 *
 * At this speed the flash needs 2 wait states (RM0444 Table 13), which clock_init()
 * sets before it raises the frequency.
 */
#define CLOCK_SYSCLK_HZ 64000000u

/*
 * The clock the core is actually running at, in Hz.
 *
 * Defined in system_stm32g0xx.c, where it starts at the 16 MHz reset value, and
 * updated by clock_init(). Use this, not CLOCK_SYSCLK_HZ, to turn times into ticks:
 * if the PLL ever fails to lock, clock_init() leaves the chip on 16 MHz and this
 * variable says so, so timers and delays stay correct.
 */
extern uint32_t SystemCoreClock;

/*
 * Switch the core to CLOCK_SYSCLK_HZ and update SystemCoreClock.
 * Call once, first thing in platform_init(), before any timer or delay is set up,
 * because their timing is computed from the clock frequency.
 */
void clock_init(void);

#endif
