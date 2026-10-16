#include "icsp.h"
#include "pic.h"
#include "platform.h"

/*
 * Result of the boot-time probe of the target PIC, readable over SWD.
 *   g_pic_device: the matching device table row (print *g_pic_device in GDB for its name
 *                 and geometry), or NULL when nothing recognised
 * The IDs are 0 when nothing answers. g_pic_status says why nothing was recognised
 * (PIC_ERR_NO_TARGET, PIC_ERR_UNSUPPORTED or PIC_ERR_PROTOCOL). A PIC16F17146 reads
 * 0x30E1, a PIC16F17124 0x30DC.
 */
volatile uint16_t g_pic_device_id;
volatile uint16_t g_pic_revision_id;
const pic_device_t *volatile g_pic_device;
volatile int g_pic_status; /* pic_status_t of the probe: 0 = found, else PIC_ERR_* */

static void probe_target(void)
{
    uint16_t id = 0u;
    uint16_t rev = 0u;

    const pic_device_t *dev = NULL;

    g_pic_status = pic_probe(&dev, &id, &rev);
    g_pic_device = dev;
    g_pic_device_id = id;
    g_pic_revision_id = rev;
}

/*
 * Entry point, reached from Reset_Handler (startup_stm32g071rb.s) once RAM is
 * initialised.
 */
int main(void)
{
    platform_init(); /* clock, LED pin and the TIM16 interrupt that blinks it */
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
