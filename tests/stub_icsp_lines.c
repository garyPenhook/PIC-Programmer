#include "stub_icsp_lines.h"

static int driven[ICSP_LINES];
static uint8_t level[ICSP_LINES];
static uint8_t pull_up[ICSP_LINES];
static stub_line_hold_t hold[ICSP_LINES];
static uint8_t joined[ICSP_LINES][ICSP_LINES];
static unsigned contention;
static int idle;

/* Lines on the same net: every line is on its own, plus any shorts. Correct from the
 * zero-initialised state too, so a program that never calls stub_lines_reset() gets
 * healthy wiring. */
static int on_net(icsp_line_t line, unsigned other)
{
    return (unsigned)line == other || joined[line][other];
}

void stub_lines_reset(void)
{
    for (unsigned i = 0; i < ICSP_LINES; i++) {
        driven[i] = 0;
        level[i] = 0;
        pull_up[i] = 0;
        hold[i] = STUB_LINE_FREE;
        for (unsigned j = 0; j < ICSP_LINES; j++) {
            joined[i][j] = 0;
        }
    }
    contention = 0;
    idle = 0;
}

void stub_lines_hold(icsp_line_t line, stub_line_hold_t h)
{
    hold[line] = h;
}

void stub_lines_short(icsp_line_t a, icsp_line_t b)
{
    joined[a][b] = joined[b][a] = 1;
}

unsigned stub_lines_contention(void)
{
    return contention;
}

int stub_lines_idle(void)
{
    return idle;
}

/* Level on the net of `line`: a drive first, then an outside hold, then the pulls (all
 * pull-ups gives 1; any pull-down gives 0, as a mid-supply level would be undefined). */
static uint8_t net_level(icsp_line_t line)
{
    int any_down = 0;

    for (unsigned j = 0; j < ICSP_LINES; j++) {
        if (on_net(line, j) && driven[j]) {
            return level[j];
        }
    }
    for (unsigned j = 0; j < ICSP_LINES; j++) {
        if (on_net(line, j) && hold[j] != STUB_LINE_FREE) {
            return (uint8_t)(hold[j] == STUB_LINE_HELD_HIGH);
        }
    }
    for (unsigned j = 0; j < ICSP_LINES; j++) {
        if (on_net(line, j) && !driven[j] && !pull_up[j]) {
            any_down = 1;
        }
    }
    return (uint8_t)(any_down ? 0u : 1u);
}

static void check_contention(icsp_line_t line)
{
    for (unsigned j = 0; j < ICSP_LINES; j++) {
        if (!on_net(line, j)) {
            continue;
        }
        if ((hold[j] == STUB_LINE_HELD_HIGH && level[line] == 0) ||
            (hold[j] == STUB_LINE_HELD_LOW && level[line] == 1) ||
            (j != (unsigned)line && driven[j] && level[j] != level[line])) {
            contention++;
        }
    }
}

void icsp_line_pull(icsp_line_t line, uint8_t up)
{
    driven[line] = 0;
    pull_up[line] = up != 0u;
    idle = 0;
}

void icsp_line_drive(icsp_line_t line, uint8_t value)
{
    driven[line] = 1;
    level[line] = value != 0u;
    idle = 0;
    check_contention(line);
}

uint8_t icsp_line_read(icsp_line_t line)
{
    return net_level(line);
}

/* The normal idle state the firmware keeps between commands anyway, so it is not counted
 * as contention caused by the self-test. */
void icsp_lines_idle(void)
{
    driven[ICSP_LINE_MCLR] = 1;
    level[ICSP_LINE_MCLR] = 1;
    driven[ICSP_LINE_CLK] = 1;
    level[ICSP_LINE_CLK] = 0;
    driven[ICSP_LINE_DAT] = 0;
    pull_up[ICSP_LINE_DAT] = 0;
    idle = 1;
}
