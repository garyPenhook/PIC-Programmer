#include "icsp.h"
#include "pic16f171.h"
#include "pic18q.h"
#include "platform.h"

/*
 * Result of the boot-time probe of the target PIC, readable over SWD.
 *   g_pic_family: 0 = nothing recognised, 16 = PIC16F171XX, 18 = PIC18-Q
 * The IDs are 0 when nothing answers. A PIC16F17146 reads 0x30E1, a PIC16F17124 0x30DC.
 */
volatile uint16_t g_pic_device_id;
volatile uint16_t g_pic_revision_id;
volatile uint8_t g_pic_family;

/* Try each supported interface with read-only commands until one recognises the device. */
static void probe_target(void)
{
    uint16_t id;

    /* First targets: the PIC16F171XX family (PIC16F17146, PIC16F17124, ...). */
    pic16f171_enter();
    id = pic16f171_read_device_id();
    g_pic_device_id = id;
    g_pic_revision_id = pic16f171_read_revision_id();
    pic16f171_exit();
    if (pic16f171_device_name(id) != NULL) {
        g_pic_family = 16u;
        return;
    }

    /* Otherwise try the PIC18-Q interface. */
    pic18q_enter();
    id = pic18q_read_device_id();
    if (pic18q_device_name(id) != NULL) {
        g_pic_device_id = id;
        g_pic_revision_id = pic18q_read_revision_id();
        g_pic_family = 18u;
    }
    pic18q_exit();
}

/*
 * Entry point, reached from Reset_Handler (startup_stm32g071rb.s) once RAM is
 * initialised.
 */
int main(void)
{
    platform_init();          /* clock, LED pin and the TIM16 interrupt that blinks it */
    platform_watchdog_init();

    /*
     * Look for a target PIC on the ICSP pins once at boot. A real command interface
     * (over the virtual COM port) will replace this; for now it proves the wiring.
     * The LED keeps blinking meanwhile because TIM16_IRQHandler runs independently.
     */
    icsp_init();
    probe_target();

    for (;;) {
        platform_watchdog_kick();
        /*
         * Application work goes here. The LED is not handled in this loop: it is
         * toggled by TIM16_IRQHandler, so a slow loop iteration does not affect it.
         */
    }
}
