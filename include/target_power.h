#ifndef TARGET_POWER_H
#define TARGET_POWER_H

#include <stdint.h>

/*
 * Optional target power switch and target VDD measurement (docs/hookup.md, "Target power").
 *
 *   D2 (PA10)  push-pull output to the enable input of a high-side switch between the
 *              Nucleo 3V3 and the target's VDD. High = on (TARGET_POWER_ON_LEVEL).
 *   A0 (PA0)   ADC_IN0, the target VDD through a divider of TARGET_VDD_DIVIDER : 1, so a
 *              self-powered target up to 5.5 V stays inside the ADC range.
 *
 * Without the switch fitted, D2 drives nothing and the target stays on the 3V3 pin as
 * before; without the divider, VDD readings are meaningless (A0 floats).
 *
 * While the power is off the ICSP lines are released to inputs, so nothing back-powers
 * the target through its pins, and the target commands answer ERR power-off.
 */
#ifndef TARGET_POWER_ON_LEVEL
#define TARGET_POWER_ON_LEVEL 1u
#endif
#ifndef TARGET_VDD_DIVIDER
#define TARGET_VDD_DIVIDER 2u /* 10 kOhm over 10 kOhm */
#endif

/* Configure the pins and switch the target on. Call after icsp_init() and adc_init(). */
void target_power_init(void);

/* Switch the target on (ICSP lines back to idle once VDD has settled) or off (lines
 * released first). */
void target_power_set(int on);
int target_power_on(void);

/* Target VDD in millivolts, measured through the divider against the calibrated VDDA. */
uint32_t target_vdd_mv(void);

#endif
