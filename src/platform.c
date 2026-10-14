#include "platform.h"

#define STM32_FLASH_BASE 0x08000000u
#define STM32_FLASH_SIZE (128u * 1024u)

/* RM0444: RCC base 0x40021000, GPIOA base 0x50000000. */
#define RCC_CR      (*(volatile uint32_t *)0x40021000u)
#define RCC_IOPENR  (*(volatile uint32_t *)0x40021034u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define GPIOA_ODR   (*(volatile uint32_t *)0x50000014u)
#define GPIOA_BSRR  (*(volatile uint32_t *)0x50000018u)

/* RM0444: TIM16 base 0x40014400, RCC_APBENR2 offset 0x40, NVIC_ISER 0xE000E100 (PM0223). */
#define RCC_APBENR2     (*(volatile uint32_t *)0x40021040u)
#define TIM16_CR1       (*(volatile uint32_t *)0x40014400u)
#define TIM16_DIER      (*(volatile uint32_t *)0x4001440Cu)
#define TIM16_SR        (*(volatile uint32_t *)0x40014410u)
#define TIM16_EGR       (*(volatile uint32_t *)0x40014414u)
#define TIM16_PSC       (*(volatile uint32_t *)0x40014428u)
#define TIM16_ARR       (*(volatile uint32_t *)0x4001442Cu)
#define NVIC_ISER       (*(volatile uint32_t *)0xE000E100u)

#define RCC_CR_HSIDIV_MASK  (7u << 11)
#define RCC_CR_HSIDIV_16    (4u << 11)   /* HSISYS = HSI16 / 16 = 1 MHz */
#define RCC_IOPENR_GPIOAEN  (1u << 0)
#define RCC_APBENR2_TIM16EN (1u << 17)
#define TIM_CR1_CEN         (1u << 0)
#define TIM_DIER_UIE        (1u << 0)
#define TIM_SR_UIF          (1u << 0)
#define TIM_EGR_UG          (1u << 0)
#define TIM16_IRQ           21u          /* TIM16 / FDCAN_IT0, vector 0x94 */

#define LED_PIN 5u                       /* LD4 on PA5 (D13), active high */
#define SYSCLK_HZ 1000000u
#define LED_TOGGLE_MS 1000u              /* LED changes state every 1 s */

extern uint32_t SystemCoreClock;

void platform_init(void)
{
    /* SYSCLK = HSISYS = HSI16 / 16 = 1 MHz (HSI16 is on and selected after reset). */
    RCC_CR = (RCC_CR & ~RCC_CR_HSIDIV_MASK) | RCC_CR_HSIDIV_16;
    SystemCoreClock = SYSCLK_HZ;

    /* PA5 push-pull output (MODER = 01), LED off. */
    RCC_IOPENR |= RCC_IOPENR_GPIOAEN;
    (void)RCC_IOPENR;                    /* read-back: let the clock enable settle */
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (LED_PIN * 2u))) | (1u << (LED_PIN * 2u));
    GPIOA_BSRR = 1u << (LED_PIN + 16u);

    /* TIM16 update interrupt every LED_TOGGLE_MS. Timer clock = PCLK = 1 MHz,
     * PSC divides to 10 kHz (100 us tick), ARR counts LED_TOGGLE_MS * 10 ticks. */
    RCC_APBENR2 |= RCC_APBENR2_TIM16EN;
    (void)RCC_APBENR2;
    TIM16_PSC = (SYSCLK_HZ / 10000u) - 1u;
    TIM16_ARR = (LED_TOGGLE_MS * 10u) - 1u;
    TIM16_EGR = TIM_EGR_UG;              /* load PSC/ARR into the shadow registers */
    TIM16_SR = ~TIM_SR_UIF;              /* drop the UIF the UG just raised */
    TIM16_DIER = TIM_DIER_UIE;
    NVIC_ISER = 1u << TIM16_IRQ;
    TIM16_CR1 = TIM_CR1_CEN;
}

/* Runs from the vector table slot at 0x94. PA5 is only ever touched here. */
void TIM16_IRQHandler(void)
{
    if ((TIM16_SR & TIM_SR_UIF) != 0u) {
        TIM16_SR = ~TIM_SR_UIF;          /* rc_w0: clears UIF only */
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

void platform_fault_lockdown(void)
{
    /* Disable actuators/outputs, clear secrets from RAM, signal fault if desired. */
}

uint32_t platform_monotonic_version(void)
{
    /* Read anti-rollback counter from protected flash / option-byte-backed policy. */
    return 0u;
}

