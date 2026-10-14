#include "clock.h"

/*
 * Core clock in Hz. The reset value is 16 MHz because the chip starts on HSI16 with
 * the divider at /1. clock_init() overwrites it once the divider is changed.
 */
uint32_t SystemCoreClock = 16000000u;

/*
 * Called from Reset_Handler after .data/.bss are set up and before main().
 * Intentionally empty: clock setup is done in clock_init(), called from
 * platform_init(), so all board bring-up is in one place that main() controls.
 */
void SystemInit(void)
{
}
