#include "adc.h"
#include "clock.h"
#include "cmd.h"
#include "i2c.h"
#include "icsp.h"
#include "pic.h"
#include "platform.h"
#include "target_power.h"
#include "uart.h"
#include "ui.h"

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

/* clock_status_t from start-up: 0 = running at 64 MHz, else the step that failed. */
volatile int g_clock_status;

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
    g_clock_status = (int)clock_status();

    /*
     * Look for a target PIC on the ICSP pins once at boot, so the result can be read over
     * SWD and the serial commands (cmd.h) start out knowing the device. The PC can probe
     * again with PROBE. The LED keeps blinking meanwhile because TIM16_IRQHandler runs
     * independently.
     */
    icsp_init();
    adc_init();
    target_power_init(); /* switches the target on, then waits for VDD to settle */
    uart_init();
    i2c_init();
    ui_init(); /* optional OLED status screen; a no-op without one */
    probe_target();
    ui_probe((pic_status_t)g_pic_status, g_pic_device, g_pic_device_id, g_pic_revision_id);
    cmd_set_device(g_pic_device);

    /*
     * Start the watchdog only now. Boot work above can wait on a missing or stuck OLED for
     * a long time (each I2C wait is bounded, but there are many), and a watchdog that fired
     * during it would reset the chip in a loop. From here on every path through the loop
     * kicks it.
     */
    platform_watchdog_init();

    for (;;) {
        uint8_t byte;
        int rx;

        platform_watchdog_kick();

        /*
         * Serial commands. A command runs to completion before the next byte is read, so
         * the host waits for the final OK or ERR line before sending again (cmd.h). The
         * LED is not handled in this loop: it is toggled by TIM16_IRQHandler, so a long
         * command does not affect it.
         */
        rx = uart_read_byte(&byte);
        if (rx > 0) {
            cmd_feed(byte);
        } else if (rx < 0) {
            cmd_rx_error();
        }
    }
}
