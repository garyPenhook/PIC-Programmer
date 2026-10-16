#include "target_power.h"

#include "adc.h"
#include "icsp.h"

/* GPIOA (RM0444 Table 6, 7.5): base 0x50000000, MODER 0x00, BSRR 0x18. */
#define RCC_IOPENR  (*(volatile uint32_t *)0x40021034u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define GPIOA_BSRR  (*(volatile uint32_t *)0x50000018u)

#define RCC_IOPENR_GPIOAEN (1u << 0)
#define PIN_ENABLE         10u /* PA10, Arduino D2 (UM2324 Table 12) */
#define PIN_SENSE          0u  /* PA0, Arduino A0, ADC_IN0 */
#define ADC_CHANNEL        0u
#define MODE_OUTPUT        1u
#define MODE_ANALOG        3u

/*
 * Time for the target VDD and the sense divider to settle after switching on, before the
 * ICSP lines drive the target again. The divider's filter capacitor (100 nF on 5 kOhm,
 * docs/hookup.md) has a 0.5 ms time constant; 5 ms leaves ten. The switch's own rise time
 * must be shorter than this; check the part chosen.
 */
#define T_SETTLE_US 5000u

static int powered;

static void set_mode(uint32_t pin, uint32_t mode)
{
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
}

static void drive_enable(int on)
{
    /* BSRR stores are single writes, so they cannot disturb PA5, which TIM16_IRQHandler
     * toggles with a read-modify-write of ODR. */
    int level = (on != 0) == (TARGET_POWER_ON_LEVEL != 0u); /* pin high? */
    GPIOA_BSRR = level ? (1u << PIN_ENABLE) : (1u << (PIN_ENABLE + 16u));
}

void target_power_init(void)
{
    RCC_IOPENR |= RCC_IOPENR_GPIOAEN;
    (void)RCC_IOPENR;
    set_mode(PIN_SENSE, MODE_ANALOG);
    drive_enable(1); /* level first, then output: on from the first edge */
    set_mode(PIN_ENABLE, MODE_OUTPUT);
    powered = 1;
    icsp_delay_us(T_SETTLE_US);
}

void target_power_set(int on)
{
    if (on) {
        drive_enable(1);
        icsp_delay_us(T_SETTLE_US);
        icsp_lines_idle();
    } else {
        icsp_lines_release(); /* stop driving the pins before the supply goes */
        drive_enable(0);
    }
    powered = on;
}

int target_power_on(void)
{
    return powered;
}

int target_vdd_mv(uint32_t *mv)
{
    uint32_t vdda;
    uint16_t raw;

    if (!adc_vdda_mv(&vdda) || !adc_read(ADC_CHANNEL, &raw)) {
        return 0;
    }
    *mv = vdda * raw * TARGET_VDD_DIVIDER / 4095u;
    return 1;
}
