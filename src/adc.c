#include "adc.h"

#include "icsp.h"
#include "regs.h"

/* VREFINT_CAL: raw VREFINT reading at 30 degC with VDDA = VREF+ = 3.0 V (DS12232 Table 6). */
#define VREFINT_CAL    (*(const volatile uint16_t *)0x1FFF75AAu)
#define VREFINT_CAL_MV 3000u


#define ADC_ISR_ADRDY              (1u << 0)
#define ADC_ISR_EOC                (1u << 2)
#define ADC_ISR_EOCAL              (1u << 11)
#define ADC_ISR_CCRDY              (1u << 13)
#define ADC_CR_ADEN                (1u << 0)
#define ADC_CR_ADSTART             (1u << 2)
#define ADC_CR_ADSTP               (1u << 4)
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

int adc_ready(void)
{
    return ready;
}

int adc_read(uint32_t channel, uint16_t *value)
{
    if (!ready || channel > ADC_CHANNEL_MAX) {
        return 0;
    }
    /* After writing CHSELR, wait for CCRDY before converting (15.12.9). */
    ADC_ISR = ADC_ISR_CCRDY;
    ADC_CHSELR = 1u << channel;
    if (!wait_set(ADC_ISR_CCRDY)) {
        return 0;
    }
    ADC_ISR = ADC_ISR_EOC; /* a stale EOC must not pass for this conversion */
    ADC_CR |= ADC_CR_ADSTART;
    if (!wait_set(ADC_ISR_EOC)) {
        /* Stop and discard the conversion (ADSTP, 15.12.3): CHSELR may only be written
         * while ADSTART = 0, so the next read would otherwise be ignored. */
        ADC_CR |= ADC_CR_ADSTP;
        for (uint32_t i = 0; i < WAIT_LOOPS && (ADC_CR & ADC_CR_ADSTP) != 0u; i++) {
        }
        ADC_ISR = ADC_ISR_EOC;
        return 0;
    }
    *value = (uint16_t)(ADC_DR & 0xFFFu); /* reading DR clears EOC */
    return 1;
}

int adc_vdda_mv(uint32_t *mv)
{
    uint16_t data;

    if (!adc_read(ADC_CHANNEL_VREFINT, &data) || data == 0u) {
        return 0;
    }
    *mv = VREFINT_CAL_MV * VREFINT_CAL / data;
    return 1;
}
