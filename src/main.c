#include "boot_policy.h"
#include "platform.h"
#include "security.h"

int main(void)
{
    platform_init();
    platform_watchdog_init();
    security_init();

    if (!boot_policy_ok()) {
        boot_policy_fail_closed();
    }

    for (;;) {
        platform_watchdog_kick();
        /* Application work goes here; the LED is driven by the TIM16 interrupt. */
    }
}

