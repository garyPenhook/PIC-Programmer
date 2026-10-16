#include "icsp.h"

#include "clock.h"

/*
 * GPIOB (base 0x50000400, RM0444 memory map) carries all three ICSP signals.
 * Offsets verified in RM0444 7.5: MODER 0x00, OTYPER 0x04, OSPEEDR 0x08, PUPDR 0x0C,
 * IDR 0x10, BSRR 0x18.
 */
#define RCC_IOPENR    (*(volatile uint32_t *)0x40021034u)
#define GPIOB_MODER   (*(volatile uint32_t *)0x50000400u)
#define GPIOB_OSPEEDR (*(volatile uint32_t *)0x50000408u)
#define GPIOB_PUPDR   (*(volatile uint32_t *)0x5000040Cu)
#define GPIOB_IDR     (*(volatile uint32_t *)0x50000410u)
#define GPIOB_BSRR    (*(volatile uint32_t *)0x50000418u)

#define RCC_IOPENR_GPIOBEN (1u << 1)

#define PIN_CLK  3u /* PB3 = ICSPCLK, Arduino D3 */
#define PIN_DAT  4u /* PB4 = ICSPDAT, Arduino D5 */
#define PIN_MCLR 5u /* PB5 = MCLR,    Arduino D4 */

/* MODER holds 2 bits per pin: 00 input, 01 general-purpose output. */
#define MODE_INPUT  0u
#define MODE_OUTPUT 1u

/* OSPEEDR: 2 bits per pin, 01 = low speed (RM0444 7.5.3). PUPDR: 2 bits per pin,
 * 00 = none, 01 = pull-up, 10 = pull-down (7.5.4). */
#define SPEED_LOW 1u
#define PULL_NONE 0u
#define PULL_UP   1u
#define PULL_DOWN 2u

/* SysTick (PM0223): free-running 24-bit down-counter on the processor clock. */
#define STK_CSR           (*(volatile uint32_t *)0xE000E010u)
#define STK_RVR           (*(volatile uint32_t *)0xE000E014u)
#define STK_CVR           (*(volatile uint32_t *)0xE000E018u)
#define STK_CSR_ENABLE    (1u << 0)
#define STK_CSR_CLKSOURCE (1u << 2) /* 1 = processor clock */
#define STK_MASK          0x00FFFFFFu

/* SysTick ticks per microsecond (= core clock in MHz), set by icsp_init(). Cached
 * because the M0+ has no divide instruction: dividing in icsp_delay_us() would call
 * the software divide on every ICSP clock edge. Starts at the 16 MHz reset value. */
static uint32_t ticks_per_us = 16u;

/* Replace the 2-bit field for `pin` in a MODER/OSPEEDR/PUPDR style register. */
static void set_field(volatile uint32_t *reg, uint32_t pin, uint32_t value)
{
    *reg = (*reg & ~(3u << (pin * 2u))) | (value << (pin * 2u));
}

static void drive(uint32_t pin, uint8_t level)
{
    /* BSRR bits 15:0 set a pin, bits 31:16 reset it. One write, so it is atomic. */
    GPIOB_BSRR = level ? (1u << pin) : (1u << (pin + 16u));
}

void icsp_init(void)
{
    RCC_IOPENR |= RCC_IOPENR_GPIOBEN;
    (void)RCC_IOPENR;

    /* Set the idle levels BEFORE making the pins outputs, so nothing glitches:
     * MCLR high releases the target from reset, ICSPCLK idles low. */
    drive(PIN_MCLR, 1u);
    drive(PIN_CLK, 0u);

    /* ICSPCLK runs at about 500 kHz, far below the low-speed limit (10 MHz at 50 pF,
     * DS12232 Table 53). Slower edges (~30 ns) ring less on a programming cable. */
    set_field(&GPIOB_OSPEEDR, PIN_CLK, SPEED_LOW);
    set_field(&GPIOB_OSPEEDR, PIN_DAT, SPEED_LOW);
    set_field(&GPIOB_OSPEEDR, PIN_MCLR, SPEED_LOW);

    /* Weak pull-down on ICSPDAT: with no target attached, reads return 0. */
    set_field(&GPIOB_PUPDR, PIN_DAT, PULL_DOWN);

    set_field(&GPIOB_MODER, PIN_MCLR, MODE_OUTPUT);
    set_field(&GPIOB_MODER, PIN_CLK, MODE_OUTPUT);
    set_field(&GPIOB_MODER, PIN_DAT, MODE_INPUT);

    /* Free-running SysTick for icsp_delay_us(). It counts down from the maximum and
     * wraps; the delay code only looks at differences, so wrapping is harmless. */
    STK_RVR = STK_MASK;
    STK_CVR = 0u;
    STK_CSR = STK_CSR_CLKSOURCE | STK_CSR_ENABLE;

    /* SystemCoreClock is the real clock (64 MHz normally, 16 MHz if the PLL failed).
     * clock_init() runs before this and nothing changes the clock afterwards; if that
     * ever changes, recompute ticks_per_us after the switch. Do not call icsp_init()
     * again while a target is in programming mode: it drives MCLR high. */
    ticks_per_us = SystemCoreClock / 1000000u;
}

void icsp_set_mclr(uint8_t level)
{
    drive(PIN_MCLR, level);
}
void icsp_set_clk(uint8_t level)
{
    drive(PIN_CLK, level);
}

void icsp_set_dat(uint8_t level)
{
    /* Level first, then switch to output, so the line never drives a stale value. */
    drive(PIN_DAT, level);
    set_field(&GPIOB_MODER, PIN_DAT, MODE_OUTPUT);
}

void icsp_release_dat(void)
{
    set_field(&GPIOB_MODER, PIN_DAT, MODE_INPUT);
}

uint8_t icsp_get_dat(void)
{
    return (uint8_t)((GPIOB_IDR >> PIN_DAT) & 1u);
}

void icsp_delay_us(uint32_t us)
{
    /* Work in chunks of at most 1 ms: that is 64000 ticks, well under the 24-bit
     * counter range, so one chunk can never wrap more than once. */
    while (us > 0u) {
        uint32_t chunk = (us > 1000u) ? 1000u : us;
        uint32_t ticks = chunk * ticks_per_us;
        uint32_t start = STK_CVR;

        /* The counter counts DOWN, so elapsed = start - now (modulo 2^24). */
        while (((start - STK_CVR) & STK_MASK) < ticks) {
        }
        us -= chunk;
    }
}

static const uint32_t line_pin[ICSP_LINES] = {PIN_CLK, PIN_DAT, PIN_MCLR};

void icsp_line_pull(icsp_line_t line, uint8_t up)
{
    uint32_t pin = line_pin[line];

    set_field(&GPIOB_PUPDR, pin, up ? PULL_UP : PULL_DOWN);
    set_field(&GPIOB_MODER, pin, MODE_INPUT);
}

void icsp_line_drive(icsp_line_t line, uint8_t level)
{
    uint32_t pin = line_pin[line];

    drive(pin, level); /* level first, then output, as in icsp_set_dat() */
    set_field(&GPIOB_MODER, pin, MODE_OUTPUT);
    set_field(&GPIOB_PUPDR, pin, PULL_NONE);
}

uint8_t icsp_line_read(icsp_line_t line)
{
    return (uint8_t)((GPIOB_IDR >> line_pin[line]) & 1u);
}

void icsp_lines_idle(void)
{
    /* The same pin state icsp_init() leaves: MCLR high, ICSPCLK low, ICSPDAT an input with
     * the weak pull-down. */
    icsp_line_drive(ICSP_LINE_MCLR, 1u);
    icsp_line_drive(ICSP_LINE_CLK, 0u);
    icsp_line_pull(ICSP_LINE_DAT, 0u);
}
