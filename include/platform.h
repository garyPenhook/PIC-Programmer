#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

void platform_init(void);
void platform_watchdog_init(void);
void platform_watchdog_kick(void);
void platform_fault_lockdown(void);
uint32_t platform_monotonic_version(void);

#endif

