#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdint.h>

#include "icsp.h"

/*
 * Wiring check of the three ICSP lines (ICSPCLK, ICSPDAT, MCLR), run before programming.
 *
 * 1. Each line is read with the STM32's weak pull-down and then its weak pull-up, with
 *    nothing driving it:
 *      follows both pulls     "float"  nothing drives it (what a target's input pin
 *                                      does, and also what an unconnected wire does)
 *      high with the pull-down "high"  something holds it high: a short to VDD, a pull-up
 *                                      on the target board (normal on MCLR), or the
 *                                      target's firmware driving the pin
 *      low with the pull-up    "low"   something holds it low: a short to ground, an
 *                                      unpowered target, or the target driving the pin
 * 2. For each pair of floating lines, one is driven high and then low while the other
 *    is held by the opposite pull. If the other follows both times, the two are shorted.
 *    Only floating lines take part: nothing is driving a floating line, so driving it
 *    cannot fight anything, whereas a held line may be a hard short to VDD or ground.
 *    So pairs that include a held line (often MCLR, with a pull-up on the target board)
 *    are not checked for shorts.
 *
 * Faults: any short, ICSPCLK or ICSPDAT not floating, or MCLR held low. MCLR held high is
 * normal (a pull-up on the target board).
 *
 * It cannot see an open wire (it floats just like a connected input) or ICSPCLK and
 * ICSPDAT swapped. MCLR is not driven high during the test, so the target may sit in
 * reset; afterwards the lines go back to their idle state and it runs (icsp_lines_idle()).
 */
typedef enum { SELFTEST_FLOAT = 0, SELFTEST_HIGH, SELFTEST_LOW } selftest_state_t;

typedef struct {
    selftest_state_t state[ICSP_LINES];
    uint8_t shorted[ICSP_LINES][ICSP_LINES]; /* symmetric; the diagonal is unused */
    int fault;
} selftest_result_t;

void selftest_run(selftest_result_t *result);

const char *selftest_line_name(icsp_line_t line); /* "CLK", "DAT", "MCLR" */
const char *selftest_state_name(selftest_state_t state);

#endif
