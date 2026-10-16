#ifndef ADC_H
#define ADC_H

#include <stdint.h>

/*
 * Polled single conversions on the STM32G071 ADC, 12-bit, right aligned.
 * Clock PCLK/2 (32 MHz at 64 MHz, 8 MHz on the 16 MHz fallback; the limit is 35 MHz,
 * DS12232 Table 58), sampling time 160.5 cycles for every channel (about 5 us at 32 MHz:
 * enough for VREFINT's 4 us minimum, DS12232 Table 24, and for a source impedance up to
 * 50 kOhm, Table 59).
 */
#define ADC_CHANNEL_VREFINT 13u /* RM0444 15.9 */

/* Enable the regulator, calibrate and switch the ADC on. Call after icsp_init() (the
 * regulator start-up wait uses icsp_delay_us()). */
void adc_init(void);

/* One conversion of `channel` (0 to 18); returns 0 to 4095. */
uint16_t adc_read(uint32_t channel);

/*
 * VDDA (= VREF+ on this package) in millivolts, from the factory VREFINT calibration:
 * VREF+ = 3.0 V x VREFINT_CAL / VREFINT_DATA (RM0444 15.9; VREFINT_CAL measured at
 * VDDA = 3.0 V, DS12232 Table 6).
 */
uint32_t adc_vdda_mv(void);

#endif
