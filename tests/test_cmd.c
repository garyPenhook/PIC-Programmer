/*
 * Unity tests for the serial command layer (src/cmd.c) against the simulated PIC16F17146
 * (sim_p16_8bit.c). The UART is a stub that records the reply text (stub_uart.c).
 */
#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "pic.h"
#include "pic_proto.h"
#include "sim_p16_8bit.h"
#include "stub_uart.h"
#include "unity.h"

#define DEVICE_ID     0x30E1u /* PIC16F17146 */
#define PATTERN_WORDS 64u

static const pic_device_t *dev;
static uint16_t pattern[PATTERN_WORDS];

/* CRC-16/CCITT-FALSE, written bit by bit and independently of src/cmd.c. */
static uint16_t reference_crc(const char *text, size_t length)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)((unsigned)(unsigned char)text[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void send_line(const char *text)
{
    stub_uart_clear();
    for (; *text != '\0'; text++) {
        cmd_feed((uint8_t)*text);
    }
    cmd_feed((uint8_t)'\n');
}

/* Append the expected data line for cells[0..count) at offset to out. */
static void expected_chunk(char *out, size_t size, uint32_t offset, const uint16_t *cells,
                           size_t count)
{
    char body[128];
    int n = snprintf(body, sizeof body, "D %08X", (unsigned)offset);

    for (size_t i = 0; i < count; i++) {
        n += snprintf(body + n, sizeof body - (size_t)n, " %04X", (unsigned)cells[i]);
    }
    size_t used = strlen(out);
    snprintf(out + used, size - used, "%s *%04X\n", body, (unsigned)reference_crc(body, (size_t)n));
}

void setUp(void)
{
    sim_set_lvp(1);
    sim_set_dat_stuck_high(0);
    sim_select_device(DEVICE_ID);
    sim_reset();
    cmd_set_device(NULL);
    stub_uart_clear();

    dev = pic_find_device(&pic_proto_p16_8bit, DEVICE_ID);
    TEST_ASSERT_NOT_NULL(dev);
    for (unsigned i = 0; i < PATTERN_WORDS; i++) {
        pattern[i] = (uint16_t)((i * 0x123u + 7u) & 0x3FFFu);
    }
    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_enter(dev));
    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_write(dev, PIC_REGION_FLASH, 0u, pattern, PATTERN_WORDS));
    pic_exit(dev);
    stub_uart_clear();
}

void tearDown(void)
{
}

static void test_crc_reference_matches_published_check_value(void)
{
    /* The standard check value for CRC-16/CCITT-FALSE, so the tests below mean something. */
    TEST_ASSERT_EQUAL_HEX16(0x29B1, reference_crc("123456789", 9u));
}

static void test_probe_reports_device(void)
{
    send_line("PROBE");
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "OK PIC16F17146 30E1 ", 20u));
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_probe_failure_forgets_the_device(void)
{
    send_line("PROBE");
    sim_set_dat_stuck_high(1);
    send_line("PROBE");
    TEST_ASSERT_EQUAL_STRING("ERR -7 bad framing on a read\n", stub_uart_output());
    sim_set_dat_stuck_high(0);
    send_line("READ flash 0 1");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
}

static void test_read_before_probe_is_refused(void)
{
    send_line("READ flash 0 4");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
}

static void test_read_returns_cells_with_valid_crc(void)
{
    char expected[256] = "";

    send_line("PROBE");
    send_line("READ flash 0 4");
    expected_chunk(expected, sizeof expected, 0u, pattern, 4u);
    strcat(expected, "OK 4\n");
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_read_splits_into_chunks_and_honours_offset(void)
{
    char expected[1024] = "";

    send_line("PROBE");
    send_line("READ flash 0x4 40");
    expected_chunk(expected, sizeof expected, 4u, pattern + 4, 16u);
    expected_chunk(expected, sizeof expected, 20u, pattern + 20, 16u);
    expected_chunk(expected, sizeof expected, 36u, pattern + 36, 8u);
    strcat(expected, "OK 40\n");
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
}

static void test_read_eeprom_cells_are_bytes(void)
{
    const uint16_t bytes[2] = {0xABu, 0xCDu};
    char expected[128] = "";

    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_enter(dev));
    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_write(dev, PIC_REGION_EEPROM, 0u, bytes, 2u));
    pic_exit(dev);

    send_line("PROBE");
    send_line("READ eeprom 0 2");
    expected_chunk(expected, sizeof expected, 0u, bytes, 2u);
    strcat(expected, "OK 2\n");
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
}

static void test_read_count_zero_succeeds_without_touching_the_target(void)
{
    send_line("PROBE");
    send_line("READ flash 0 0");
    TEST_ASSERT_EQUAL_STRING("OK 0\n", stub_uart_output());
}

static void test_read_out_of_range_is_rejected_before_entering_the_target(void)
{
    uint64_t before;

    send_line("PROBE");
    before = sim_time_us();
    send_line("READ flash 0x3FFF 2");
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    send_line("READ flash 0x10000 1");
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    TEST_ASSERT_EQUAL_UINT64(before, sim_time_us());
}

static void test_bad_commands_and_arguments(void)
{
    send_line("PROBE");
    send_line("READ nowhere 0 1");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    send_line("READ flash x 1");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    send_line("READ flash 0 99999999999");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    send_line("READ flash 0");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
    send_line("read flash 0 1");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
    send_line("FOO");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
    send_line("READ flash 0 1 extra tokens here");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
    send_line("");
    TEST_ASSERT_EQUAL_STRING("", stub_uart_output());
}

static void test_line_endings_and_spacing(void)
{
    char expected[128] = "";

    send_line("PROBE");
    stub_uart_clear();
    for (const char *p = "  READ \t flash   0 1\r"; *p != '\0'; p++) {
        cmd_feed((uint8_t)*p);
    }
    cmd_feed((uint8_t)'\n');
    expected_chunk(expected, sizeof expected, 0u, pattern, 1u);
    strcat(expected, "OK 1\n");
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
}

static void test_overlong_line_is_rejected_and_the_next_one_still_works(void)
{
    char longline[200];

    memset(longline, 'A', sizeof longline - 1u);
    longline[sizeof longline - 1u] = '\0';
    send_line(longline);
    TEST_ASSERT_EQUAL_STRING("ERR line-too-long\n", stub_uart_output());
    send_line("PROBE");
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "OK PIC16F17146", 14u));
}

static void test_receiver_error_rejects_the_line(void)
{
    cmd_rx_error();
    send_line("PROBE");
    TEST_ASSERT_EQUAL_STRING("ERR rx-error\n", stub_uart_output());
    send_line("PROBE");
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "OK PIC16F17146", 14u));
}

static void test_wrong_device_on_the_wires_is_reported_and_leaves_programming_mode(void)
{
    send_line("PROBE");
    sim_select_device(0x30DCu); /* a PIC16F17124 answers instead */
    sim_reset();
    send_line("READ flash 0 1");
    TEST_ASSERT_EQUAL_STRING("ERR -6 a different device answered\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_reference_matches_published_check_value);
    RUN_TEST(test_probe_reports_device);
    RUN_TEST(test_probe_failure_forgets_the_device);
    RUN_TEST(test_read_before_probe_is_refused);
    RUN_TEST(test_read_returns_cells_with_valid_crc);
    RUN_TEST(test_read_splits_into_chunks_and_honours_offset);
    RUN_TEST(test_read_eeprom_cells_are_bytes);
    RUN_TEST(test_read_count_zero_succeeds_without_touching_the_target);
    RUN_TEST(test_read_out_of_range_is_rejected_before_entering_the_target);
    RUN_TEST(test_bad_commands_and_arguments);
    RUN_TEST(test_line_endings_and_spacing);
    RUN_TEST(test_overlong_line_is_rejected_and_the_next_one_still_works);
    RUN_TEST(test_receiver_error_rejects_the_line);
    RUN_TEST(test_wrong_device_on_the_wires_is_reported_and_leaves_programming_mode);
    return UNITY_END();
}
