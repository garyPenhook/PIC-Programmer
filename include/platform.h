#ifndef PLATFORM_H
#define PLATFORM_H

/* Board bring-up: system clock, LED pin and the TIM16 interrupt that blinks it. */
void platform_init(void);

/* Independent watchdog hooks. Stubs for now: enable and refresh the IWDG here. */
void platform_watchdog_init(void);
void platform_watchdog_kick(void);

#endif
