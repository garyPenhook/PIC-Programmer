#ifndef PLATFORM_H
#define PLATFORM_H

/* Board bring-up: system clock, LED pin and the TIM16 interrupt that blinks it. */
void platform_init(void);

/*
 * Independent watchdog. Init starts it (it cannot be stopped again) with a timeout of
 * about 2 s; call it once start-up is done. kick must then be called at least that often or the chip resets. Frozen
 * while a debugger halts the core.
 */
void platform_watchdog_init(void);

/* 1 if the last reset was the watchdog's (cleared by the next power-up). */
int platform_watchdog_reset(void);

void platform_watchdog_kick(void);

#endif
