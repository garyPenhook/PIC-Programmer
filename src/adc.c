#include "adc.h"

#include "icsp.h"

/*
 * Register addresses (RM0444): ADC base 0x40012400 (Table 6); offsets from the register map
 * (Table 81): ISR 0x00, CR 0x08, CFGR1 0x0C, CFGR2 0x10, SMPR 0x14, CHSELR 0x28, DR 0x40,
 * CCR 0x308. RCC_APBENR2 at RCC base + 0x40 (5.4.16).
 */
#define ADC_ISR     (*(volatile uint32_t *)0x40012400u)
#define ADC_CR      (*(volatile uint32_t *)0x40012408u)
#define ADC_CFGR2   (*(volatile uint32_t *)0x40012410u)
#define ADC_SMPR    (*(volatile uint32_t *)0x40012414u)
#define ADC_CHSELR  (*(volatile uint32_t *)0x40012428u)
#define ADC_DR      (*(volatile uint32_t *)0x40012440u)
#define ADC_CCR     (*(volatile uint32_t *)0x40012708u)
#define RCC_APBENR2 (*(volatile uint32_t *)0x40021040u)

/* VREFINT_CAL: raw VREFINT reading at 30 degC with VDDA = VREF+ = 3.0 V (DS12232 Table 6). */
#define VREFINT_CAL    (*(const volatile uint16_t *)0x1FFF75AAu)
#define VREFINT_CAL_MV 3000u

#define RCC_APBENR2_ADCEN (1u << 20)

#define ADC_ISR_ADRDY              (1u << 0)
#define ADC_ISR_EOC                (1u << 2)
#define ADC_ISR_EOCAL              (1u << 11)
#define ADC_ISR_CCRDY              (1u << 13)
#define ADC_CR_ADEN                (1u << 0)
#define ADC_CR_ADSTART             (1u << 2)
#define ADC_CR_ADVREGEN            (1u << 28)
#define ADC_CR_ADCAL               (1u << 31)
#define ADC_CFGR2_CKMODE_PCLK_DIV2 (1u << 30) /* CKMODE 01 (15.12.5) */
#define ADC_SMPR_SMP1_160_5        7u         /* SMP1 111: 160.5 cycles (15.12.6) */
#define ADC_CCR_VREFEN             (1u << 22)

/* Start-up waits: the regulator needs at most 20 us (tADCVREG_STUP, DS12232 Table 58), the
 * VREFINT buffer at most 12 us (tstart_vrefint, Table 24). */
#define T_REGULATOR_US 20u
#define T_VREFINT_US   12u

/* Bound on every flag wait, so a dead ADC cannot hang the programmer. */
#define WAIT_LOOPS 100000u

static int wait_set(uint32_t mask)
{
    for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
        if ((ADC_ISR & mask) != 0u) {
            return 1;
        }
    }
    return 0;
}

static int ready;

void adc_init(void)
{
    RCC_APBENR2 |= RCC_APBENR2_ADCEN;
    (void)RCC_APBENR2;

    /* CFGR2 may only be written while the ADC is disabled, which it is after reset. */
    ADC_CFGR2 = ADC_CFGR2_CKMODE_PCLK_DIV2;

    /* Regulator, then calibration with ADEN = 0 (RM0444 15.3.2, 15.3.3). */
    ADC_CR = ADC_CR_ADVREGEN;
    icsp_delay_us(T_REGULATOR_US);
    ADC_CR |= ADC_CR_ADCAL;
    for (uint32_t i = 0; (ADC_CR & ADC_CR_ADCAL) != 0u; i++) {
        if (i == WAIT_LOOPS) {
            return; /* calibration never finished: leave the ADC off */
        }
    }
    ADC_ISR = ADC_ISR_EOCAL;

    /* Enable: clear ADRDY, set ADEN, wait for ADRDY (15.3.4). */
    ADC_ISR = ADC_ISR_ADRDY;
    ADC_CR |= ADC_CR_ADEN;
    if (!wait_set(ADC_ISR_ADRDY)) {
        return;
    }
    ADC_SMPR = ADC_SMPR_SMP1_160_5; /* every channel uses SMP1 (SMPSELx = 0) */
    ADC_CCR |= ADC_CCR_VREFEN;
    icsp_delay_us(T_VREFINT_US);
    ready = 1;
}

uint16_t adc_read(uint32_t channel)
{
    if (!ready || channel > ADC_CHANNEL_MAX) {
        return 0u;
    }
    /* After writing CHSELR, wait for CCRDY before converting (15.12.9). */
    ADC_ISR = ADC_ISR_CCRDY;
    ADC_CHSELR = 1u << channel;
    if (!wait_set(ADC_ISR_CCRDY)) {
        return 0u;
    }
    ADC_CR |= ADC_CR_ADSTART;
    if (!wait_set(ADC_ISR_EOC)) {
        return 0u;
    }
    return (uint16_t)(ADC_DR & 0xFFFu); /* reading DR clears EOC */
}

uint32_t adc_vdda_mv(void)
{
    uint32_t data = adc_read(ADC_CHANNEL_VREFINT);

    return data != 0u ? VREFINT_CAL_MV * VREFINT_CAL / data : 0u;
}
