/*
 * Unity tests for the serial bridge (src/bridge.c): the exit-sequence matcher on its own,
 * and the bridge loop against a stub PC link (stub_uart.c) and target port (stub_board.c).
 */
#include <stdio.h>
#include <string.h>

#include "bridge.h"
#include "stub_board.h"
#include "stub_uart.h"
#include "unity.h"

static const char exit_seq[] = BRIDGE_EXIT;
static char emitted[256];
static size_t emitted_len;

static void collect(uint8_t byte)
{
    emitted[emitted_len++] = (char)byte;
}

/* Feed a string to a fresh matcher; returns the index just after the match, or -1. */
static int feed(const char *text, size_t n)
{
    bridge_matcher_t m = {0u};

    emitted_len = 0u;
    for (size_t i = 0; i < n; i++) {
        if (bridge_match(&m, (uint8_t)text[i], collect)) {
            return (int)(i + 1u);
        }
    }
    return -1;
}

void setUp(void)
{
    stub_board_reset();
    stub_uart_clear();
}

void tearDown(void)
{
}

/* platform_watchdog_kick() is provided by stub_uart.c. */

static void test_plain_bytes_pass_straight_through(void)
{
    TEST_ASSERT_EQUAL_INT(-1, feed("hello\r\n", 7u));
    TEST_ASSERT_EQUAL_UINT(7u, emitted_len);
    TEST_ASSERT_EQUAL_MEMORY("hello\r\n", emitted, 7u);
}

static void test_the_exit_sequence_is_caught_and_never_passed_on(void)
{
    char text[64];
    size_t n = (size_t)snprintf(text, sizeof text, "ab%sc", exit_seq);

    TEST_ASSERT_EQUAL_INT((int)(2u + strlen(exit_seq)), feed(text, n));
    TEST_ASSERT_EQUAL_UINT(2u, emitted_len);
    TEST_ASSERT_EQUAL_MEMORY("ab", emitted, 2u);
}

static void test_a_partial_match_that_is_data_is_passed_on_in_order(void)
{
    /* The first five bytes of the sequence, then something else. */
    char text[16];
    memcpy(text, exit_seq, 5u);
    text[5] = 'Z';
    TEST_ASSERT_EQUAL_INT(-1, feed(text, 6u));
    TEST_ASSERT_EQUAL_UINT(6u, emitted_len);
    TEST_ASSERT_EQUAL_MEMORY(text, emitted, 6u);
}

static void test_a_repeated_first_byte_still_finds_the_sequence(void)
{
    /* ESC ESC [picprog-...: the first ESC is data, the second starts the sequence. */
    char text[64];
    size_t n = (size_t)snprintf(text, sizeof text, "\x1b%s", exit_seq);

    TEST_ASSERT_EQUAL_INT((int)n, feed(text, n));
    TEST_ASSERT_EQUAL_UINT(1u, emitted_len);
    TEST_ASSERT_EQUAL_HEX8(0x1B, (uint8_t)emitted[0]);
}

static void test_bridge_passes_bytes_both_ways_until_the_exit(void)
{
    size_t sent_len;
    const char *sent;

    stub_target_uart_inject("from target", 11u);
    stub_uart_inject("to target", 9u);
    stub_uart_inject(exit_seq, (unsigned)strlen(exit_seq));
    TEST_ASSERT_EQUAL_UINT32(0u, bridge_run());
    sent = stub_target_uart_sent(&sent_len);
    TEST_ASSERT_EQUAL_UINT(9u, sent_len);
    TEST_ASSERT_EQUAL_MEMORY("to target", sent, 9u);
    TEST_ASSERT_EQUAL_STRING("from target", stub_uart_output());
}

static void test_bytes_beyond_the_buffer_are_dropped_and_counted(void)
{
    static char lots[3000];

    memset(lots, 'x', sizeof lots);
    stub_target_uart_block_tx(1); /* the target never takes a byte */
    stub_uart_inject(lots, sizeof lots);
    stub_uart_inject(exit_seq, (unsigned)strlen(exit_seq));
    /* 2047 fit in the buffer, the rest are dropped at once; the buffered ones are dropped
     * when the final drain gives up. */
    TEST_ASSERT_EQUAL_UINT32(3000u, bridge_run());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_bytes_pass_straight_through);
    RUN_TEST(test_the_exit_sequence_is_caught_and_never_passed_on);
    RUN_TEST(test_a_partial_match_that_is_data_is_passed_on_in_order);
    RUN_TEST(test_a_repeated_first_byte_still_finds_the_sequence);
    RUN_TEST(test_bridge_passes_bytes_both_ways_until_the_exit);
    RUN_TEST(test_bytes_beyond_the_buffer_are_dropped_and_counted);
    return UNITY_END();
}
