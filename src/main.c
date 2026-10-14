#include "platform.h"

/*
 * Entry point, reached from Reset_Handler (startup_stm32g071rb.s) once RAM is
 * initialised.
 */
int main(void)
{
    platform_init();          /* clock, LED pin and the TIM16 interrupt that blinks it */
    platform_watchdog_init();

    for (;;) {
        platform_watchdog_kick();
        /*
         * Application work goes here. The LED is not handled in this loop: it is
         * toggled by TIM16_IRQHandler, so a slow loop iteration does not affect it.
         */
    }
}
