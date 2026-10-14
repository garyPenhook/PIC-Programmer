#include "platform.h"

#include "clock.h"

/*
 * Register access: each register is a fixed address, cast to a volatile pointer
 * and dereferenced. "volatile" stops the compiler from caching, merging or
 * dropping reads and writes, which it must not do for hardware registers.
 * Addresses and offsets are from RM0444; NVIC from PM0223.
 */

/* RCC (base 0x40021000) and GPIOA (base 0x50000000). */
#define RCC_IOPENR  (*(volatile uint32_t *)0x40021034u)  /* I/O port clock enable */
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)  /* pin mode, 2 bits per pin */
#define GPIOA_ODR   (*(volatile uint32_t *)0x50000014u)  /* output data, 1 bit per pin */
#define GPIOA_BSRR  (*(volatile uint32_t *)0x50000018u)  /* atomic set (15:0) / reset (31:16) */

/* TIM16 (base 0x40014400), its clock enable in RCC, and the NVIC enable register. */
#define RCC_APBENR2     (*(volatile uint32_t *)0x40021040u)  /* APB peripheral clock enable 2 */
#define TIM16_CR1       (*(volatile uint32_t *)0x40014400u)  /* control 1: counter enable */
#define TIM16_DIER      (*(volatile uint32_t *)0x4001440Cu)  /* interrupt enable */
#define TIM16_SR        (*(volatile uint32_t *)0x40014410u)  /* status / interrupt flags */
#define TIM16_EGR       (*(volatile uint32_t *)0x40014414u)  /* event generation */
#define TIM16_PSC       (*(volatile uint32_t *)0x40014428u)  /* prescaler */
#define TIM16_ARR       (*(volatile uint32_t *)0x4001442Cu)  /* auto-reload (period) */
#define NVIC_ISER       (*(volatile uint32_t *)0xE000E100u)  /* interrupt set-enable */

/* Bit definitions. */
#define RCC_IOPENR_GPIOAEN  (1u << 0)    /* enable the GPIOA peripheral clock */
#define RCC_APBENR2_TIM16EN (1u << 17)   /* enable the TIM16 peripheral clock */
#define TIM_CR1_CEN         (1u << 0)    /* counter enable */
#define TIM_DIER_UIE        (1u << 0)    /* update interrupt enable */
#define TIM_SR_UIF          (1u << 0)    /* update interrupt flag */
#define TIM_EGR_UG          (1u << 0)    /* update generation: force an update event */

/*
 * TIM16 shares one interrupt line with FDCAN_IT0: IRQ position 21, vector table
 * address 0x94. NVIC_ISER bit n enables IRQ n. FDCAN is not used here, so the
 * handler only has to deal with the timer.
 */
#define TIM16_IRQ           21u

#define LED_PIN 5u                       /* LD4 user LED on PA5 (Arduino D13), active high */
#define LED_TOGGLE_MS 1000u              /* the LED changes state every 1 s */

/*
 * Bring up the board: clock, LED pin, and the timer interrupt that drives the LED.
 * Order matters: the clock is set first because the timer prescaler below is
 * calculated from SystemCoreClock.
 */
void platform_init(void)
{
    clock_init();

    /*
     * LED pin: PA5 as a push-pull output.
     * A peripheral's registers are not usable until its bus clock is enabled.
     * The dummy read-back after the enable is a precaution (not a manual requirement):
     * it forces the enable write to complete before the first access to the peripheral.
     */
    RCC_IOPENR |= RCC_IOPENR_GPIOAEN;
    (void)RCC_IOPENR;

    /*
     * MODER holds 2 bits per pin: 00 input, 01 general-purpose output, 10 alternate
     * function, 11 analog. PA5 sits at bits 11:10. Port A resets to 0xEBFFFFFF, so
     * PA5 starts as 11 (analog): both bits must be cleared before setting 01, or
     * a plain OR would leave it analog.
     */
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (LED_PIN * 2u))) | (1u << (LED_PIN * 2u));

    /* BSRR bits 31:16 reset (drive low) the matching pin: start with the LED off. */
    GPIOA_BSRR = 1u << (LED_PIN + 16u);

    /*
     * Timer: TIM16 raises an update interrupt once per LED_TOGGLE_MS.
     *
     * The timer is clocked by PCLK (the APB prescaler is /1, so PCLK = SystemCoreClock,
     * which is 64 MHz when the PLL is up). The counter ticks at PCLK / (PSC + 1).
     * We pick PSC so one tick is 100 us (10 kHz): 64 MHz / 10 kHz = 6400, so
     * PSC = 6400 - 1 = 6399. PSC is 16 bits, so this still fits.
     * SystemCoreClock is used (not the constant) so the LED stays at 1 s even if the
     * PLL failed to lock and clock_init() fell back to 16 MHz (PSC = 1599).
     * The counter counts 0..ARR and then wraps, which is ARR + 1 ticks per period.
     * For LED_TOGGLE_MS: ticks = LED_TOGGLE_MS * 10 (10 ticks per ms), so ARR = ticks - 1.
     * At 1000 ms that is ARR = 9999. PSC and ARR are 16-bit, so the longest period
     * with this prescaler is 65536 ticks = about 6.5 s.
     */
    RCC_APBENR2 |= RCC_APBENR2_TIM16EN;
    (void)RCC_APBENR2;
    TIM16_PSC = (SystemCoreClock / 10000u) - 1u;
    TIM16_ARR = (LED_TOGGLE_MS * 10u) - 1u;

    /*
     * PSC and ARR are buffered: a write only takes effect at the next update event.
     * Force one now with UG so the new values load immediately. That also sets UIF,
     * which would fire a bogus interrupt the moment we enable it, so clear it first.
     * SR flag bits are "rc_w0": writing 0 clears a flag and writing 1 leaves it alone,
     * so writing ~UIF clears only UIF.
     */
    TIM16_EGR = TIM_EGR_UG;
    TIM16_SR = ~TIM_SR_UIF;

    /*
     * Enable the interrupt at both levels, then start the counter last:
     *   - TIM16_DIER.UIE lets the timer request an interrupt on update,
     *   - NVIC_ISER lets the core accept that IRQ.
     * Both are needed. Starting the counter after everything else is armed means no
     * update event can arrive half-configured.
     */
    TIM16_DIER = TIM_DIER_UIE;
    NVIC_ISER = 1u << TIM16_IRQ;
    TIM16_CR1 = TIM_CR1_CEN;
}

/*
 * TIM16 update interrupt, once per LED_TOGGLE_MS.
 * The name must match the entry in the vector table (startup_stm32g071rb.s); the
 * CPU jumps here directly, and the hardware saves and restores the registers, so
 * this is an ordinary C function with no special attribute on Cortex-M.
 *
 * This is the only code that writes PA5, so it needs no locking against main().
 * Keep it short: it runs with the main loop paused.
 */
void TIM16_IRQHandler(void)
{
    /* The line is shared with FDCAN_IT0, so confirm it was the timer update. */
    if ((TIM16_SR & TIM_SR_UIF) != 0u) {
        /*
         * Clear the flag first or the interrupt would fire again as soon as we
         * return. Writing ~UIF clears only that bit (rc_w0, see platform_init).
         */
        TIM16_SR = ~TIM_SR_UIF;

        /* XOR flips the output bit: on -> off -> on. */
        GPIOA_ODR ^= 1u << LED_PIN;
    }
}

void platform_watchdog_init(void)
{
    /* Enable IWDG in production. */
}

void platform_watchdog_kick(void)
{
    /* Refresh IWDG. */
}
