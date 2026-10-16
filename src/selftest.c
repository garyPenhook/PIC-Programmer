#include "selftest.h"

/*
 * Time for a line to settle after a pull or a drive changes. The weak pulls are 25 to
 * 55 kOhm (DS12232 Table 51); with 55 kOhm and a generous 200 pF of cable and target pin,
 * the time constant is 11 us, so 200 us is over 18 time constants.
 */
#define T_SETTLE_US 200u

static void settle(void)
{
    icsp_delay_us(T_SETTLE_US);
}

/* Every line on the same pull; returns the levels read, bit n for line n. All lines get the
 * same pull so that shorted lines agree instead of fighting at mid-supply. */
static uint32_t read_all_with(uint8_t up)
{
    uint32_t levels = 0u;

    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        icsp_line_pull((icsp_line_t)i, up);
    }
    settle();
    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        levels |= (uint32_t)icsp_line_read((icsp_line_t)i) << i;
    }
    return levels;
}

/* Drive `from` to each level with `to` on the opposite pull: does `to` follow? */
static int follows(icsp_line_t from, icsp_line_t to)
{
    uint8_t high;
    uint8_t low;

    icsp_line_pull(to, 0u);
    icsp_line_drive(from, 1u);
    settle();
    high = icsp_line_read(to);

    icsp_line_pull(to, 1u);
    icsp_line_drive(from, 0u);
    settle();
    low = icsp_line_read(to);

    icsp_line_pull(from, 0u);
    icsp_line_pull(to, 0u);
    return high != 0u && low == 0u;
}

void selftest_run(selftest_result_t *r)
{
    uint32_t with_up = read_all_with(1u);
    uint32_t with_down = read_all_with(0u); /* leaves every line on its pull-down */

    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        if ((with_down >> i) & 1u) {
            r->state[i] = SELFTEST_HIGH; /* high against the pull-down */
        } else {
            r->state[i] = ((with_up >> i) & 1u) ? SELFTEST_FLOAT : SELFTEST_LOW;
        }
        for (uint32_t j = 0; j < ICSP_LINES; j++) {
            r->shorted[i][j] = 0u;
        }
    }

    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        /* Only a floating line is driven: a held one may be a hard short to VDD or ground,
         * and driving it against that would push current into the short. */
        for (uint32_t j = 0; j < ICSP_LINES; j++) {
            if (i == j || r->state[i] != SELFTEST_FLOAT || r->state[j] != SELFTEST_FLOAT) {
                continue;
            }
            if (follows((icsp_line_t)i, (icsp_line_t)j)) {
                r->shorted[i][j] = 1u;
                r->shorted[j][i] = 1u;
            }
        }
    }
    icsp_lines_idle();

    r->fault = r->state[ICSP_LINE_CLK] != SELFTEST_FLOAT ||
               r->state[ICSP_LINE_DAT] != SELFTEST_FLOAT ||
               r->state[ICSP_LINE_MCLR] == SELFTEST_LOW;
    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        for (uint32_t j = i + 1u; j < ICSP_LINES; j++) {
            r->fault = r->fault || r->shorted[i][j] != 0u;
        }
    }
}

const char *selftest_line_name(icsp_line_t line)
{
    static const char *const names[ICSP_LINES] = {"CLK", "DAT", "MCLR"};
    return names[line];
}

const char *selftest_state_name(selftest_state_t state)
{
    static const char *const names[] = {"float", "high", "low"};
    return names[state];
}
