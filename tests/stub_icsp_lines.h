#ifndef STUB_ICSP_LINES_H
#define STUB_ICSP_LINES_H

#include "icsp.h"

/*
 * A model of the three ICSP wires for the self-test (src/selftest.c), implementing the
 * icsp_line_* functions of icsp.h. Each line can be held high or low from outside (a
 * short to a supply, a pull-up on the target, the target driving it), and lines can be
 * shorted together. A driven line wins over pulls; an outside hold wins over pulls.
 *
 * It also counts contention: every time the self-test drives a line while something on
 * its net (an outside hold, or another driven line) holds the other level. Returning to
 * the idle state is not counted: the firmware keeps that state between commands anyway.
 */
typedef enum { STUB_LINE_FREE = 0, STUB_LINE_HELD_HIGH, STUB_LINE_HELD_LOW } stub_line_hold_t;

void stub_lines_reset(void);
void stub_lines_hold(icsp_line_t line, stub_line_hold_t hold);
void stub_lines_short(icsp_line_t a, icsp_line_t b);
unsigned stub_lines_contention(void);
int stub_lines_idle(void); /* 1 if the lines are in the icsp_lines_idle() state */

#endif
