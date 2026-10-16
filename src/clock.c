#include "clock.h"

/*
 * Register addresses (RM0444). RCC base is 0x40021000, FLASH interface base is
 * 0x40022000.
 */
#define RCC_CR      (*(volatile uint32_t *)0x40021000u) /* clock control */
#define RCC_CFGR    (*(volatile uint32_t *)0x40021008u) /* clock configuration */
#define RCC_PLLCFGR (*(volatile uint32_t *)0x4002100Cu) /* PLL configuration */
#define FLASH_ACR   (*(volatile uint32_t *)0x40022000u) /* flash access control */

/* RCC_CR: HSIDIV divides HSI16 to make HSISYS (000 = /1). PLLON starts the PLL. */
#define RCC_CR_HSIDIV_MASK (7u << 11)
#define RCC_CR_PLLON       (1u << 24) /* PLL enable */
#define RCC_CR_PLLRDY      (1u << 25) /* PLL locked (set by hardware) */

/*
 * RCC_PLLCFGR. Output frequencies: fVCO = fIN x N / M, fPLLR = fVCO / R.
 * Fields can only be written while the PLL is off (it is off after reset).
 */
#define PLLCFGR_PLLSRC_HSI16 (2u << 0)  /* PLLSRC 10 = HSI16 as the PLL input */
#define PLLCFGR_PLLM_DIV1    (0u << 4)  /* PLLM 000 = /1 */
#define PLLCFGR_PLLN_8       (8u << 8)  /* PLLN = 8: VCO = 16 MHz x 8 = 128 MHz */
#define PLLCFGR_PLLREN       (1u << 28) /* enable the PLLRCLK output */
#define PLLCFGR_PLLR_DIV2    (1u << 29) /* PLLR 001 = /2: 128 MHz / 2 = 64 MHz */

/* RCC_CFGR: SW selects the system clock source, SWS reports the one in use. */
#define RCC_CFGR_SW_MASK     (7u << 0)
#define RCC_CFGR_SW_PLLRCLK  (2u << 0) /* SW 010 = PLLRCLK */
#define RCC_CFGR_SWS_MASK    (7u << 3)
#define RCC_CFGR_SWS_PLLRCLK (2u << 3) /* SWS 010 = PLLRCLK is the system clock */

/* FLASH_ACR: LATENCY is the number of flash wait states; PRFTEN enables prefetch. */
#define FLASH_ACR_LATENCY_MASK (7u << 0)
#define FLASH_ACR_LATENCY_2WS  (2u << 0) /* 2 wait states: required above 48 MHz */
#define FLASH_ACR_PRFTEN       (1u << 8)

/* Upper bound on any hardware wait so a dead oscillator cannot hang the boot. */
#define WAIT_LOOPS 100000u

/* Poll (reg & mask) == value. Returns 1 if it happened, 0 if we gave up. */
static int wait_for(const volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
        if ((*reg & mask) == value) {
            return 1;
        }
    }
    return 0;
}

void clock_init(void)
{
    /*
     * HSI16 is on and is the system clock after reset (RCC_CR.HSION, RCC_CFGR.SW = 000).
     * Force HSIDIV to /1 so the fallback clock is a known 16 MHz even if an earlier run
     * changed it.
     */
    RCC_CR &= ~RCC_CR_HSIDIV_MASK;

    /*
     * Step 1: flash wait states. RM0444 3.3.4: when INCREASING the frequency, program
     * the new latency first, and read it back until it takes effect, before changing
     * the clock. At 64 MHz the flash is too slow for 0 wait states.
     * Prefetch (PRFTEN) helps hide the wait states for sequential code.
     */
    FLASH_ACR = (FLASH_ACR & ~FLASH_ACR_LATENCY_MASK) | FLASH_ACR_LATENCY_2WS | FLASH_ACR_PRFTEN;
    if (!wait_for(&FLASH_ACR, FLASH_ACR_LATENCY_MASK, FLASH_ACR_LATENCY_2WS)) {
        SystemCoreClock = 16000000u;
        return;
    }

    /*
     * Step 2: configure and start the PLL, then wait for it to lock.
     * PLLCFGR is written whole: source HSI16, /1, x8, R = /2 enabled. The PLL is off
     * here, so the fields are writable.
     */
    RCC_PLLCFGR = PLLCFGR_PLLSRC_HSI16 | PLLCFGR_PLLM_DIV1 | PLLCFGR_PLLN_8 | PLLCFGR_PLLREN |
                  PLLCFGR_PLLR_DIV2;
    RCC_CR |= RCC_CR_PLLON;
    if (!wait_for(&RCC_CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        /* PLL did not lock: stay on HSI16 (16 MHz) so the rest of the code still works. */
        RCC_CR &= ~RCC_CR_PLLON;
        SystemCoreClock = 16000000u;
        return;
    }

    /*
     * Step 3: switch SYSCLK to the PLL and wait until the hardware confirms (SWS).
     * HPRE and PPRE stay at their reset value /1, so HCLK = PCLK = SYSCLK = 64 MHz.
     */
    RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLLRCLK;
    if (!wait_for(&RCC_CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_PLLRCLK)) {
        SystemCoreClock = 16000000u;
        return;
    }

    /*
     * Keep the CMSIS-style global in step with the hardware. Nothing reads the clock
     * back from the registers, so this variable is the only record of the frequency.
     */
    SystemCoreClock = CLOCK_SYSCLK_HZ;
}
