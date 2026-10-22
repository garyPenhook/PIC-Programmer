/*
 * Unity tests for the serial command layer (src/cmd.c) against the simulated PIC16F17146
 * (sim_p16_8bit.c). The UART is a stub that records the reply text (stub_uart.c).
 */
#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "pic.h"
#include "pic_proto.h"
#include "bridge.h"
#include "sim_p16_8bit.h"
#include "stub_board.h"
#include "stub_icsp_lines.h"
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

/* Append the expected READB frame for cells[0..count) at offset to buf; returns new length. */
static size_t expected_frame(uint8_t *buf, size_t used, uint32_t offset, const uint16_t *cells,
                             size_t count)
{
    size_t start = used;

    buf[used++] = 0x42u;
    for (unsigned i = 0; i < 4u; i++) {
        buf[used++] = (uint8_t)(offset >> (8u * i));
    }
    buf[used++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        buf[used++] = (uint8_t)(cells[i] & 0xFFu);
        buf[used++] = (uint8_t)(cells[i] >> 8);
    }
    uint16_t crc = reference_crc((const char *)buf + start, used - start);
    buf[used++] = (uint8_t)(crc & 0xFFu);
    buf[used++] = (uint8_t)(crc >> 8);
    return used;
}

static size_t append_text(uint8_t *buf, size_t used, const char *text)
{
    size_t n = strlen(text);
    memcpy(buf + used, text, n);
    return used + n;
}

static void assert_output(const uint8_t *expected, size_t length)
{
    TEST_ASSERT_EQUAL_UINT(length, stub_uart_length());
    TEST_ASSERT_EQUAL_MEMORY(expected, stub_uart_output(), length);
}

void setUp(void)
{
    stub_lines_reset();
    stub_board_reset();
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

static void test_read_dia_returns_the_factory_words(void)
{
    uint16_t dia[64];
    char expected[1024] = "";

    for (uint32_t i = 0; i < 64u; i++) {
        dia[i] = sim_dia_word(i);
    }
    send_line("PROBE");
    send_line("READ dia 8 18");
    expected_chunk(expected, sizeof expected, 8u, dia + 8, 16u);
    expected_chunk(expected, sizeof expected, 24u, dia + 24, 2u);
    strcat(expected, "OK 18\n");
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
    send_line("READ dia 60 5"); /* the window is 64 words */
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_dia_is_read_only_and_never_reaches_the_target(void)
{
    uint64_t t0;

    send_line("PROBE");
    t0 = sim_time_us();
    send_line("WRITE dia 0 4");
    TEST_ASSERT_EQUAL_STRING("ERR -4 not supported by this device\n", stub_uart_output());
    send_line("ERASE dia");
    TEST_ASSERT_EQUAL_STRING("ERR -4 not supported by this device\n", stub_uart_output());
    send_line("ERASE ROW dia 0");
    TEST_ASSERT_EQUAL_STRING("ERR -4 not supported by this device\n", stub_uart_output());
    TEST_ASSERT_EQUAL_UINT64(t0, sim_time_us()); /* nothing was sent */
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    send_line("ERASE ALL"); /* and a bulk erase of everything leaves it readable */
    send_line("CRC dia 0 2");
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "OK ", 3u));
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

static void test_readb_sends_binary_frame_then_text_ok(void)
{
    uint8_t expected[64];
    size_t n = 0;

    send_line("PROBE");
    send_line("READB flash 0 4");
    n = expected_frame(expected, n, 0u, pattern, 4u);
    n = append_text(expected, n, "OK 4\n");
    assert_output(expected, n);
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_readb_splits_into_chunks_and_encodes_offset_little_endian(void)
{
    uint8_t expected[256];
    size_t n = 0;

    send_line("PROBE");
    send_line("READB flash 0x14 40");
    n = expected_frame(expected, n, 0x14u, pattern + 0x14, 16u);
    n = expected_frame(expected, n, 0x24u, pattern + 0x24, 16u);
    n = expected_frame(expected, n, 0x34u, pattern + 0x34, 8u);
    n = append_text(expected, n, "OK 40\n");
    assert_output(expected, n);
}

static void test_readb_frame_with_high_offset_bytes(void)
{
    /* An offset above 0xFF exercises every byte of the little-endian field. */
    uint16_t blank[2] = {0x3FFFu, 0x3FFFu};
    uint8_t expected[64];
    size_t n = 0;

    send_line("PROBE");
    send_line("READB flash 0x1234 2");
    n = expected_frame(expected, n, 0x1234u, blank, 2u);
    n = append_text(expected, n, "OK 2\n");
    assert_output(expected, n);
}

static void test_readb_errors_are_text(void)
{
    send_line("READB flash 0 1");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
    send_line("PROBE");
    send_line("READB flash 0x4000 1");
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    send_line("READB flash 0");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
}

/* Answer NEXT/RESEND requests with lines taken from cells[] (indexed from `base`) until the
 * command ends. Returns the number of data lines sent. */
static int upload(uint32_t base, const uint16_t *cells)
{
    int lines = 0;

    for (;;) {
        const char *out = stub_uart_output();
        unsigned offset;
        unsigned n;
        char body[96];
        char text[112];

        if (sscanf(out, "NEXT %8X %u", &offset, &n) != 2 &&
            sscanf(out, "RESEND %8X %u", &offset, &n) != 2) {
            return lines;
        }
        int len = snprintf(body, sizeof body, "D %08X", offset);
        for (unsigned i = 0; i < n; i++) {
            len += snprintf(body + len, sizeof body - (size_t)len, " %04X",
                            (unsigned)cells[offset - base + i]);
        }
        snprintf(text, sizeof text, "%s *%04X", body, (unsigned)reference_crc(body, (size_t)len));
        send_line(text);
        lines++;
    }
}

static void blank_target(void)
{
    send_line("PROBE");
    send_line("ERASE ALL");
    TEST_ASSERT_EQUAL_STRING("OK\n", stub_uart_output());
}

static void test_erase_all_blanks_every_region(void)
{
    blank_target();
    for (unsigned i = 0; i < PATTERN_WORDS; i++) {
        TEST_ASSERT_EQUAL_HEX16(0x3FFF, sim_flash_word(i));
    }
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_erase_row_erases_only_that_row(void)
{
    send_line("PROBE");
    send_line("ERASE ROW flash 33"); /* any cell in the second 32-word row */
    TEST_ASSERT_EQUAL_STRING("OK\n", stub_uart_output());
    TEST_ASSERT_EQUAL_HEX16(pattern[31], sim_flash_word(31));
    for (unsigned i = 32; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX16(0x3FFF, sim_flash_word(i));
    }
}

static void test_erase_one_region(void)
{
    const uint16_t bytes[2] = {0x12u, 0x34u};

    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_enter(dev));
    TEST_ASSERT_EQUAL_INT(PIC_OK, pic_write(dev, PIC_REGION_EEPROM, 0u, bytes, 2u));
    pic_exit(dev);
    send_line("PROBE");
    send_line("ERASE eeprom");
    TEST_ASSERT_EQUAL_STRING("OK\n", stub_uart_output());
    TEST_ASSERT_EQUAL_HEX8(0xFF, sim_eeprom_byte(0));
    TEST_ASSERT_EQUAL_HEX16(pattern[0], sim_flash_word(0)); /* flash left alone */
}

static void test_erase_errors(void)
{
    send_line("ERASE ALL");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
    send_line("PROBE");
    send_line("ERASE nowhere");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    send_line("ERASE ROW flash");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
    send_line("ERASE ROW flash 0x4000");
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    send_line("ERASE ROW config 0");
    TEST_ASSERT_EQUAL_STRING("ERR -4 not supported by this device\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_write_flash_whole_rows_and_a_padded_last_row(void)
{
    uint16_t data[40];

    for (unsigned i = 0; i < 40u; i++) {
        data[i] = (uint16_t)((0x2A5u + i * 77u) & 0x3FFFu);
    }
    blank_target();
    send_line("WRITE flash 0 40");
    TEST_ASSERT_EQUAL_STRING("NEXT 00000000 8\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(5, upload(0u, data));
    TEST_ASSERT_EQUAL_STRING("OK 40\n", stub_uart_output());
    for (unsigned i = 0; i < 40u; i++) {
        TEST_ASSERT_EQUAL_HEX16(data[i], sim_flash_word(i));
    }
    for (unsigned i = 40; i < 64u; i++) {
        TEST_ASSERT_EQUAL_HEX16(0x3FFF, sim_flash_word(i)); /* padding programs nothing */
    }
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_write_asks_for_the_remainder_last(void)
{
    char line[96];
    char body[64];
    int len;

    blank_target();
    send_line("WRITE flash 32 12");
    TEST_ASSERT_EQUAL_STRING("NEXT 00000020 8\n", stub_uart_output());
    len = snprintf(body, sizeof body, "D 00000020");
    for (unsigned i = 0; i < 8u; i++) {
        len += snprintf(body + len, sizeof body - (size_t)len, " %04X", (unsigned)pattern[i]);
    }
    snprintf(line, sizeof line, "%s *%04X", body, (unsigned)reference_crc(body, (size_t)len));
    send_line(line);
    TEST_ASSERT_EQUAL_STRING("NEXT 00000028 4\n", stub_uart_output()); /* 12 - 8 left */
    TEST_ASSERT_EQUAL_HEX16(0x3FFF, sim_flash_word(32)); /* row not complete: nothing yet */
    upload(32u, pattern);                                /* cells 32.. are pattern[0..] */
    TEST_ASSERT_EQUAL_STRING("OK 12\n", stub_uart_output());
    for (unsigned i = 0; i < 12u; i++) {
        TEST_ASSERT_EQUAL_HEX16(pattern[i], sim_flash_word(32u + i));
    }
}

static void test_write_must_start_on_a_row_boundary(void)
{
    uint64_t before;

    send_line("PROBE");
    before = sim_time_us();
    send_line("WRITE flash 5 4");
    TEST_ASSERT_EQUAL_STRING("ERR -3 flash write not on a row boundary\n", stub_uart_output());
    TEST_ASSERT_EQUAL_UINT64(before, sim_time_us());
}

static void test_damaged_lines_are_asked_for_again(void)
{
    const uint16_t data[1] = {0x0123u};
    char good[64];
    char body[32];
    int len;

    blank_target();
    send_line("WRITE eeprom 0 1");
    TEST_ASSERT_EQUAL_STRING("NEXT 00000000 1\n", stub_uart_output());

    send_line("D 00000000 0123 *0000"); /* bad crc */
    TEST_ASSERT_EQUAL_STRING("RESEND 00000000 1\n", stub_uart_output());

    len = snprintf(body, sizeof body, "D 00000004 0123"); /* wrong offset */
    snprintf(good, sizeof good, "%s *%04X", body, (unsigned)reference_crc(body, (size_t)len));
    send_line(good);
    TEST_ASSERT_EQUAL_STRING("RESEND 00000000 1\n", stub_uart_output());

    len = snprintf(body, sizeof body, "D 00000000 0123 0456"); /* too many cells */
    snprintf(good, sizeof good, "%s *%04X", body, (unsigned)reference_crc(body, (size_t)len));
    send_line(good);
    TEST_ASSERT_EQUAL_STRING("RESEND 00000000 1\n", stub_uart_output());

    cmd_rx_error();
    send_line("D 00000000 0123");
    TEST_ASSERT_EQUAL_STRING("RESEND 00000000 1\n", stub_uart_output());

    upload(0u, data);
    TEST_ASSERT_EQUAL_STRING("OK 1\n", stub_uart_output());
    TEST_ASSERT_EQUAL_HEX8(0x23, sim_eeprom_byte(0)); /* EEPROM cells keep the low byte */
}

static void test_any_other_line_aborts_and_releases_the_target(void)
{
    blank_target();
    send_line("WRITE flash 0 32");
    TEST_ASSERT_EQUAL_INT(1, sim_in_program_mode());
    send_line("PROBE");
    TEST_ASSERT_EQUAL_STRING("ERR aborted\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_HEX16(0x3FFF, sim_flash_word(0)); /* nothing was written */
    send_line("PROBE");                                 /* normal commands work again */
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "OK PIC16F17146", 14u));
}

static void test_verify_matches_and_reports_the_first_difference(void)
{
    uint16_t data[PATTERN_WORDS];

    memcpy(data, pattern, sizeof data);
    send_line("PROBE");
    send_line("VERIFY flash 0 20");
    upload(0u, data);
    TEST_ASSERT_EQUAL_STRING("OK 20\n", stub_uart_output());

    data[13] ^= 0x0001u;
    send_line("VERIFY flash 0 20");
    upload(0u, data);
    TEST_ASSERT_EQUAL_STRING("ERR -8 verify mismatch @0000000D\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_write_then_read_back(void)
{
    const uint16_t ids[4] = {0x0001u, 0x0002u, 0x0003u, 0x0004u};

    blank_target();
    send_line("WRITE userid 0 4");
    upload(0u, ids);
    TEST_ASSERT_EQUAL_STRING("OK 4\n", stub_uart_output());
    send_line("VERIFY userid 0 4");
    upload(0u, ids);
    TEST_ASSERT_EQUAL_STRING("OK 4\n", stub_uart_output());
    TEST_ASSERT_EQUAL_HEX16(0x0003, sim_userid_word(2));
}

static void test_verify_dia_compares_the_read_only_words(void)
{
    uint16_t good[2] = {sim_dia_word(0), sim_dia_word(1)};
    uint16_t bad[2] = {sim_dia_word(0), (uint16_t)(sim_dia_word(1) ^ 1u)};

    send_line("PROBE");
    send_line("VERIFY dia 0 2");
    upload(0u, good);
    TEST_ASSERT_EQUAL_STRING("OK 2\n", stub_uart_output());
    send_line("VERIFY dia 0 2");
    upload(0u, bad);
    TEST_ASSERT_EQUAL_INT(0, strncmp(stub_uart_output(), "ERR -8 verify mismatch @00000001", 32u));
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_id_reads_device_and_revision(void)
{
    send_line("ID");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
    send_line("PROBE");
    send_line("ID");
    TEST_ASSERT_EQUAL_STRING("OK 30E1 2000\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    send_line("ID now");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
}

static void test_dci_reads_the_five_fields(void)
{
    send_line("PROBE");
    send_line("DCI");
    /* PIC16F17146: 32-word rows and latches, 512 rows (16 KW), 256 EEPROM bytes; the
     * simulator reports 20 pins. */
    TEST_ASSERT_EQUAL_STRING("OK 32 32 512 256 20\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
    TEST_ASSERT_EQUAL_UINT32(0u, sim_violations());
}

static void test_id_and_dci_refuse_a_different_part(void)
{
    send_line("PROBE");
    sim_select_device(0x30DCu); /* a PIC16F17124 is now on the wires */
    sim_reset();
    send_line("ID");
    TEST_ASSERT_EQUAL_STRING("ERR -6 a different device answered\n", stub_uart_output());
    send_line("DCI");
    TEST_ASSERT_EQUAL_STRING("ERR -6 a different device answered\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

/* Standard CRC-32 (zlib's crc32), written bit by bit independently of src/cmd.c. */
static uint32_t reference_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

static void test_crc32_reference_matches_published_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, reference_crc32((const uint8_t *)"123456789", 9u));
}

static void test_crc_of_cells_matches_their_hex_file_bytes(void)
{
    uint8_t bytes[2 * PATTERN_WORDS];
    char expected[32];

    for (unsigned i = 0; i < PATTERN_WORDS; i++) {
        bytes[2 * i] = (uint8_t)(pattern[i] & 0xFFu); /* low byte first, as in a hex file */
        bytes[2 * i + 1] = (uint8_t)(pattern[i] >> 8);
    }
    send_line("PROBE");
    send_line("CRC flash 0 64");
    snprintf(expected, sizeof expected, "OK %08X\n", (unsigned)reference_crc32(bytes, 128u));
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
    send_line("CRC flash 3 1");
    snprintf(expected, sizeof expected, "OK %08X\n", (unsigned)reference_crc32(bytes + 6, 2u));
    TEST_ASSERT_EQUAL_STRING(expected, stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_crc_errors(void)
{
    send_line("CRC flash");
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
    send_line("PROBE");
    send_line("CRC nowhere");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    send_line("CRC flash 0x3FFF 2");
    TEST_ASSERT_EQUAL_STRING("ERR -2 range outside region\n", stub_uart_output());
    send_line("CRC flash 0");
    TEST_ASSERT_EQUAL_STRING("ERR bad-command\n", stub_uart_output());
}

static void test_blank_check(void)
{
    send_line("PROBE");
    send_line("BLANK flash");
    TEST_ASSERT_EQUAL_STRING("ERR not-blank @00000000 0007\n", stub_uart_output());
    send_line("BLANK flash 64 100"); /* past the written pattern */
    TEST_ASSERT_EQUAL_STRING("OK 100\n", stub_uart_output());
    send_line("ERASE ALL");
    send_line("BLANK flash");
    TEST_ASSERT_EQUAL_STRING("OK 16384\n", stub_uart_output());
    send_line("BLANK eeprom");
    TEST_ASSERT_EQUAL_STRING("OK 256\n", stub_uart_output());
    TEST_ASSERT_EQUAL_INT(0, sim_in_program_mode());
}

static void test_selftest_reports_each_line_then_ok(void)
{
    send_line("SELFTEST"); /* needs no PROBE */
    TEST_ASSERT_EQUAL_STRING("LINE CLK float\nLINE DAT float\nLINE MCLR float\nOK\n",
                             stub_uart_output());
}

static void test_selftest_reports_a_short(void)
{
    stub_lines_short(ICSP_LINE_CLK, ICSP_LINE_DAT);
    stub_lines_hold(ICSP_LINE_MCLR, STUB_LINE_HELD_HIGH);
    send_line("SELFTEST");
    TEST_ASSERT_EQUAL_STRING(
        "LINE CLK float\nLINE DAT float\nLINE MCLR high\nSHORT CLK DAT\nERR wiring\n",
        stub_uart_output());
}

static void test_power_and_vdd(void)
{
    send_line("POWER");
    TEST_ASSERT_EQUAL_STRING("OK on 3300\n", stub_uart_output());
    send_line("VDD");
    TEST_ASSERT_EQUAL_STRING("OK 3300 3300\n", stub_uart_output());
    send_line("POWER sideways");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
}

static void test_status_reports_start_up_faults(void)
{
    send_line("STATUS");
    TEST_ASSERT_EQUAL_STRING("OK clock=pll adc=ok reset=normal\n", stub_uart_output());
    stub_board_set_faults(1, 1);
    stub_board_set_watchdog_reset(1);
    send_line("STATUS");
    TEST_ASSERT_EQUAL_STRING("OK clock=hsi16 adc=fail reset=watchdog\n", stub_uart_output());
}

static void test_a_failed_adc_is_an_error_not_zero_millivolts(void)
{
    stub_board_set_faults(0, 1);
    send_line("VDD");
    TEST_ASSERT_EQUAL_STRING("ERR adc\n", stub_uart_output());
    send_line("POWER");
    TEST_ASSERT_EQUAL_STRING("OK on -1\n", stub_uart_output());
}

static void test_power_off_refuses_target_commands_and_forgets_the_device(void)
{
    send_line("PROBE");
    send_line("POWER OFF");
    TEST_ASSERT_EQUAL_STRING("OK off 0\n", stub_uart_output());
    const char *const commands[] = {"PROBE",     "READ flash 0 1",  "ID",
                                    "ERASE ALL", "SELFTEST",        "BRIDGE 9600",
                                    "CRC flash", "WRITE flash 0 1", "BLANK flash"};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        send_line(commands[i]);
        TEST_ASSERT_EQUAL_STRING("ERR power-off\n", stub_uart_output());
    }
    send_line("POWER ON");
    TEST_ASSERT_EQUAL_STRING("OK on 3300\n", stub_uart_output());
    send_line("READ flash 0 1"); /* a part that was off must be probed again */
    TEST_ASSERT_EQUAL_STRING("ERR no-device\n", stub_uart_output());
}

static void test_bridge_opens_relays_and_closes(void)
{
    static const char exit_seq[] = BRIDGE_EXIT;
    size_t n;

    send_line("BRIDGE 300");
    TEST_ASSERT_EQUAL_STRING("ERR bad-argument\n", stub_uart_output());
    stub_target_uart_inject("hi", 2u);
    stub_uart_inject("abc", 3u);
    stub_uart_inject(exit_seq, (unsigned)strlen(exit_seq));
    send_line("BRIDGE 115200");
    TEST_ASSERT_EQUAL_STRING("OK bridge\nhi\r\nOK bridge closed 0\n", stub_uart_output());
    TEST_ASSERT_EQUAL_MEMORY("abc", stub_target_uart_sent(&n), 3u);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    TEST_ASSERT_EQUAL_UINT32(0u, stub_target_uart_baud()); /* closed again */
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
    RUN_TEST(test_readb_sends_binary_frame_then_text_ok);
    RUN_TEST(test_readb_splits_into_chunks_and_encodes_offset_little_endian);
    RUN_TEST(test_readb_frame_with_high_offset_bytes);
    RUN_TEST(test_readb_errors_are_text);
    RUN_TEST(test_erase_all_blanks_every_region);
    RUN_TEST(test_erase_row_erases_only_that_row);
    RUN_TEST(test_erase_one_region);
    RUN_TEST(test_erase_errors);
    RUN_TEST(test_read_dia_returns_the_factory_words);
    RUN_TEST(test_dia_is_read_only_and_never_reaches_the_target);
    RUN_TEST(test_verify_dia_compares_the_read_only_words);
    RUN_TEST(test_write_flash_whole_rows_and_a_padded_last_row);
    RUN_TEST(test_write_asks_for_the_remainder_last);
    RUN_TEST(test_write_must_start_on_a_row_boundary);
    RUN_TEST(test_damaged_lines_are_asked_for_again);
    RUN_TEST(test_any_other_line_aborts_and_releases_the_target);
    RUN_TEST(test_verify_matches_and_reports_the_first_difference);
    RUN_TEST(test_write_then_read_back);
    RUN_TEST(test_id_reads_device_and_revision);
    RUN_TEST(test_dci_reads_the_five_fields);
    RUN_TEST(test_id_and_dci_refuse_a_different_part);
    RUN_TEST(test_crc32_reference_matches_published_check_value);
    RUN_TEST(test_crc_of_cells_matches_their_hex_file_bytes);
    RUN_TEST(test_crc_errors);
    RUN_TEST(test_blank_check);
    RUN_TEST(test_selftest_reports_each_line_then_ok);
    RUN_TEST(test_selftest_reports_a_short);
    RUN_TEST(test_power_and_vdd);
    RUN_TEST(test_status_reports_start_up_faults);
    RUN_TEST(test_a_failed_adc_is_an_error_not_zero_millivolts);
    RUN_TEST(test_power_off_refuses_target_commands_and_forgets_the_device);
    RUN_TEST(test_bridge_opens_relays_and_closes);
    return UNITY_END();
}
