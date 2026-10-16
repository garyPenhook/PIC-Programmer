/*
 * Unity tests for the ICSP wiring self-test (src/selftest.c) against a model of the three
 * wires (stub_icsp_lines.c): each fault the test is meant to find, and that it never
 * drives a line against something holding it.
 */
#include "selftest.h"
#include "stub_icsp_lines.h"
#include "unity.h"

static selftest_result_t r;

void setUp(void)
{
    stub_lines_reset();
}

void tearDown(void)
{
    /* Whatever it found, the test must not fight anything and must leave the lines idle. */
    TEST_ASSERT_EQUAL_UINT(0u, stub_lines_contention());
    TEST_ASSERT_EQUAL_INT(1, stub_lines_idle());
}

/* icsp_delay_us() from icsp.c; the model needs no time. */
void icsp_delay_us(uint32_t us)
{
    (void)us;
}

static void test_good_wiring_is_all_floating_and_passes(void)
{
    selftest_run(&r);
    for (unsigned i = 0; i < ICSP_LINES; i++) {
        TEST_ASSERT_EQUAL_INT(SELFTEST_FLOAT, r.state[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, r.fault);
}

static void test_mclr_pull_up_on_the_target_is_normal(void)
{
    stub_lines_hold(ICSP_LINE_MCLR, STUB_LINE_HELD_HIGH);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_HIGH, r.state[ICSP_LINE_MCLR]);
    TEST_ASSERT_EQUAL_INT(0, r.fault);
}

static void test_clk_dat_short_is_found(void)
{
    stub_lines_short(ICSP_LINE_CLK, ICSP_LINE_DAT);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_FLOAT, r.state[ICSP_LINE_CLK]);
    TEST_ASSERT_EQUAL_INT(SELFTEST_FLOAT, r.state[ICSP_LINE_DAT]);
    TEST_ASSERT_EQUAL_UINT8(1u, r.shorted[ICSP_LINE_CLK][ICSP_LINE_DAT]);
    TEST_ASSERT_EQUAL_UINT8(1u, r.shorted[ICSP_LINE_DAT][ICSP_LINE_CLK]);
    TEST_ASSERT_EQUAL_UINT8(0u, r.shorted[ICSP_LINE_CLK][ICSP_LINE_MCLR]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_dat_mclr_short_is_found(void)
{
    stub_lines_short(ICSP_LINE_DAT, ICSP_LINE_MCLR);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_UINT8(1u, r.shorted[ICSP_LINE_DAT][ICSP_LINE_MCLR]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_line_shorted_to_ground_is_low_and_never_driven(void)
{
    stub_lines_hold(ICSP_LINE_CLK, STUB_LINE_HELD_LOW);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_LOW, r.state[ICSP_LINE_CLK]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_line_shorted_to_vdd_is_high_and_never_driven(void)
{
    stub_lines_hold(ICSP_LINE_DAT, STUB_LINE_HELD_HIGH);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_HIGH, r.state[ICSP_LINE_DAT]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_mclr_held_low_is_a_fault(void)
{
    stub_lines_hold(ICSP_LINE_MCLR, STUB_LINE_HELD_LOW);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_LOW, r.state[ICSP_LINE_MCLR]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_short_to_a_held_line_is_not_driven_against(void)
{
    /* CLK shorted to an MCLR that a target pull-up holds high: CLK reads high too, and
     * neither is driven, so there is no contention even though the short is not named. */
    stub_lines_hold(ICSP_LINE_MCLR, STUB_LINE_HELD_HIGH);
    stub_lines_short(ICSP_LINE_CLK, ICSP_LINE_MCLR);
    selftest_run(&r);
    TEST_ASSERT_EQUAL_INT(SELFTEST_HIGH, r.state[ICSP_LINE_CLK]);
    TEST_ASSERT_EQUAL_INT(1, r.fault);
}

static void test_model_counts_contention(void)
{
    /* The no-contention check in tearDown only means something if the model can see it. */
    stub_lines_hold(ICSP_LINE_CLK, STUB_LINE_HELD_LOW);
    icsp_line_drive(ICSP_LINE_CLK, 1u);
    TEST_ASSERT_EQUAL_UINT(1u, stub_lines_contention());
    stub_lines_reset();
    icsp_lines_idle();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_model_counts_contention);
    RUN_TEST(test_good_wiring_is_all_floating_and_passes);
    RUN_TEST(test_mclr_pull_up_on_the_target_is_normal);
    RUN_TEST(test_clk_dat_short_is_found);
    RUN_TEST(test_dat_mclr_short_is_found);
    RUN_TEST(test_line_shorted_to_ground_is_low_and_never_driven);
    RUN_TEST(test_line_shorted_to_vdd_is_high_and_never_driven);
    RUN_TEST(test_mclr_held_low_is_a_fault);
    RUN_TEST(test_short_to_a_held_line_is_not_driven_against);
    return UNITY_END();
}
