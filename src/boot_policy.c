#include "boot_policy.h"

#include "platform.h"
#include "security.h"

bool boot_policy_ok(void)
{
    return security_boot_policy_ok();
}

void boot_policy_fail_closed(void)
{
    platform_fault_lockdown();

    for (;;) {
        platform_watchdog_kick();
    }
}

