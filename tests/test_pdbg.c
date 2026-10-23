/*
 * Unity tests for the debug agent core (target/pdbg/pdbg.c): framing, every command, the
 * halted loop and breakpoints, against the fake part of pdbg_fake_target.c.
 */
#include <string.h>

#include "pdbg.h"
#include "pdbg_fake_target.h"
#include "unity.h"

/* -- the fake UART ---------------------------------------------------------------------- */

static uint8_t rx_queue[1024];
static unsigned rx_head, rx_tail;
static uint8_t tx_log[2048];
static unsigned tx_len;
static unsigned service_calls;

int pdbg_port_rx(uint8_t *byte)
{
    if (rx_head == rx_tail) {
        return 0;
    }
    *byte = rx_queue[rx_head++];
    return 1;
}

void pdbg_port_tx(uint8_t byte)
{
    tx_log[tx_len++] = byte;
}

void pdbg_port_service(void)
{
    service_calls++;
}

/* -- test-side framing, written independently of the code under test --------------------- */

static unsigned cobs_encode(const uint8_t *in, unsigned len, uint8_t *out)
{
    unsigned w = 1, code_at = 0;
    uint8_t code = 1;

    for (unsigned i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = w++;
            code = 1;
        } else {
            out[w++] = in[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = w++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return w;
}

static unsigned cobs_decode(const uint8_t *in, unsigned len, uint8_t *out)
{
    unsigned r = 0, w = 0;

    while (r < len) {
        const unsigned code = in[r++];

        for (unsigned k = 1; k < code; k++) {
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) {
            out[w++] = 0;
        }
    }
    return w;
}

static void queue_bytes(const uint8_t *bytes, unsigned n)
{
    memcpy(&rx_queue[rx_tail], bytes, n);
    rx_tail += n;
}

/* A frame as the PC sends it: payload + CRC, COBS, delimiter. */
static unsigned make_frame(const uint8_t *payload, unsigned len, uint8_t *out)
{
    uint8_t raw[80];
    const uint16_t crc = pdbg_crc16(payload, (uint8_t)len);
    unsigned n;

    memcpy(raw, payload, len);
    raw[len] = (uint8_t)crc;
    raw[len + 1] = (uint8_t)(crc >> 8);
    n = cobs_encode(raw, len + 2, out);
    out[n++] = 0;
    return n;
}

static void feed_all(void)
{
    while (rx_head < rx_tail) {
        uint8_t b;

        (void)pdbg_port_rx(&b);
        if (pdbg_feed(b) == PDBG_HALT) {
            pdbg_enter(PDBG_REASON_REQUEST, 0);
        }
    }
}

/* Send a request while running, return the agent's output. */
static void request(uint8_t cmd, uint8_t seq, const uint8_t *args, unsigned alen)
{
    uint8_t payload[64], frame[100];

    payload[0] = cmd;
    payload[1] = seq;
    memcpy(&payload[2], args, alen);
    queue_bytes(frame, make_frame(payload, 2 + alen, frame));
    tx_len = 0;
    feed_all();
}

/* Frame number `index` of the transmit log, decoded and CRC-checked: returns the payload
 * length (0 if there is no such frame). */
static unsigned tx_frame(unsigned index, uint8_t *payload)
{
    unsigned start = 0;

    for (unsigned i = 0; i < tx_len; i++) {
        if (tx_log[i] == 0) {
            if (index-- == 0) {
                uint8_t raw[80];
                const unsigned n = cobs_decode(&tx_log[start], i - start, raw);

                TEST_ASSERT_TRUE(n >= 2);
                TEST_ASSERT_EQUAL_HEX16(pdbg_crc16(raw, (uint8_t)(n - 2)),
                                        raw[n - 2] | (raw[n - 1] << 8));
                memcpy(payload, raw, n - 2);
                return n - 2;
            }
            start = i + 1;
        }
    }
    return 0;
}

static unsigned tx_frames(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < tx_len; i++) {
        n += tx_log[i] == 0;
    }
    return n;
}

/* The one reply to the last request: checks the header and returns the data length. */
static unsigned reply(uint8_t cmd, uint8_t seq, uint8_t status, uint8_t *data)
{
    uint8_t p[80];
    const unsigned n = tx_frame(0, p);

    TEST_ASSERT_TRUE(n >= 4);
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_REPLY, p[0]);
    TEST_ASSERT_EQUAL_UINT8(seq, p[1]);
    TEST_ASSERT_EQUAL_UINT8(cmd, p[2]);
    TEST_ASSERT_EQUAL_UINT8(status, p[3]);
    memcpy(data, &p[4], n - 4);
    return n - 4;
}

void setUp(void)
{
    fake_target_reset();
    pdbg_init();
    rx_head = rx_tail = 0;
    tx_len = 0;
    service_calls = 0;
}

void tearDown(void)
{
}

/* -- framing ---------------------------------------------------------------------------- */

static void test_crc_matches_the_ccitt_false_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1, pdbg_crc16((const uint8_t *)"123456789", 9));
}

static void test_hello_reports_version_device_and_reset_cause(void)
{
    uint8_t d[80];

    request(PDBG_CMD_HELLO, 7, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(9, reply(PDBG_CMD_HELLO, 7, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8(PDBG_VERSION, d[0]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_MAX_DATA, d[1]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_BP_SITES, d[2]);
    TEST_ASSERT_EQUAL_HEX16(0x7100, d[3] | (d[4] << 8));
    TEST_ASSERT_EQUAL_HEX16(0xA042, d[5] | (d[6] << 8));
    TEST_ASSERT_EQUAL_HEX8(0x3C, d[7]);
    TEST_ASSERT_EQUAL_UINT8(0, d[8]);
}

static void test_a_frame_with_a_bad_crc_gets_no_answer(void)
{
    uint8_t payload[2] = {PDBG_CMD_HELLO, 1}, frame[20];
    const unsigned n = make_frame(payload, 2, frame);

    frame[1] ^= 0x10;
    queue_bytes(frame, n);
    feed_all();
    TEST_ASSERT_EQUAL_UINT(0, tx_len);
}

static void test_garbage_before_a_frame_costs_only_that_frame(void)
{
    static const uint8_t junk[] = {0x55, 0x12, 0x77, 0x00};
    uint8_t d[80];

    queue_bytes(junk, sizeof junk);
    feed_all();
    TEST_ASSERT_EQUAL_UINT(0, tx_len);
    request(PDBG_CMD_HELLO, 2, NULL, 0);
    reply(PDBG_CMD_HELLO, 2, PDBG_OK, d);
}

static void test_an_oversize_frame_is_skipped_whole(void)
{
    uint8_t big[PDBG_RX_MAX + 20], d[80];

    memset(big, 0x41, sizeof big);
    queue_bytes(big, sizeof big);
    queue_bytes((const uint8_t *)"\0", 1);
    feed_all();
    TEST_ASSERT_EQUAL_UINT(0, tx_len);
    request(PDBG_CMD_HELLO, 3, NULL, 0);
    reply(PDBG_CMD_HELLO, 3, PDBG_OK, d);
}

static void test_zeros_in_a_reply_survive_the_encoding(void)
{
    static const uint8_t args[3] = {0x00, 0x00, 8};
    uint8_t d[80];

    memset(fake_ram, 0, 8);
    fake_ram[3] = 7;
    request(PDBG_CMD_RD, 2, args, 3);
    TEST_ASSERT_EQUAL_UINT(8, reply(PDBG_CMD_RD, 2, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fake_ram, d, 8);
}

static void test_an_unknown_command_is_refused(void)
{
    uint8_t d[80];

    request(0x7F, 9, NULL, 0);
    reply(0x7F, 9, PDBG_ERR_CMD, d);
}

/* -- memory ----------------------------------------------------------------------------- */

static void test_rd_reads_data_memory_while_running(void)
{
    static const uint8_t args[3] = {0x20, 0x01, 4};
    uint8_t d[80];

    fake_ram[0x120] = 1;
    fake_ram[0x121] = 2;
    fake_ram[0x122] = 3;
    fake_ram[0x123] = 0;
    request(PDBG_CMD_RD, 1, args, 3);
    TEST_ASSERT_EQUAL_UINT(4, reply(PDBG_CMD_RD, 1, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&fake_ram[0x120], d, 4);
}

static void test_rd_checks_its_length(void)
{
    static const uint8_t zero[3] = {0, 0, 0};
    static const uint8_t many[3] = {0, 0, PDBG_MAX_DATA + 1};
    uint8_t d[80];

    request(PDBG_CMD_RD, 1, zero, 3);
    reply(PDBG_CMD_RD, 1, PDBG_ERR_ARG, d);
    request(PDBG_CMD_RD, 2, many, 3);
    reply(PDBG_CMD_RD, 2, PDBG_ERR_ARG, d);
    request(PDBG_CMD_RD, 3, many, 2);
    reply(PDBG_CMD_RD, 3, PDBG_ERR_ARG, d);
}

static void test_rd_of_a_full_buffer_fits_in_a_reply(void)
{
    static const uint8_t args[3] = {0, 0, PDBG_MAX_DATA};
    uint8_t d[80];

    for (unsigned i = 0; i < PDBG_MAX_DATA; i++) {
        fake_ram[i] = (uint8_t)(i % 3);  /* plenty of zeros */
    }
    request(PDBG_CMD_RD, 1, args, 3);
    TEST_ASSERT_EQUAL_UINT(PDBG_MAX_DATA, reply(PDBG_CMD_RD, 1, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fake_ram, d, PDBG_MAX_DATA);
}

static void test_wr_needs_the_agent_halted(void)
{
    static const uint8_t args[4] = {0x30, 0x00, 0xAA, 0xBB};
    uint8_t d[80];

    request(PDBG_CMD_WR, 1, args, 4);
    reply(PDBG_CMD_WR, 1, PDBG_ERR_STATE, d);
    TEST_ASSERT_EQUAL_UINT8(0, fake_ram[0x30]);
}

static void test_rdpfm_reads_program_flash(void)
{
    static const uint8_t args[4] = {0x10, 0x00, 0x00, 5};
    uint8_t d[80];

    request(PDBG_CMD_RDPFM, 1, args, 4);
    TEST_ASSERT_EQUAL_UINT(5, reply(PDBG_CMD_RDPFM, 1, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&fake_pfm[0x10], d, 5);
}

/* -- halting ---------------------------------------------------------------------------- */

static void test_state_commands_refuse_while_running(void)
{
    static const uint8_t start[1] = {0};
    uint8_t d[80];

    request(PDBG_CMD_REGS, 1, NULL, 0);
    reply(PDBG_CMD_REGS, 1, PDBG_ERR_STATE, d);
    request(PDBG_CMD_STACK, 2, start, 1);
    reply(PDBG_CMD_STACK, 2, PDBG_ERR_STATE, d);
    request(PDBG_CMD_CONT, 3, NULL, 0);
    reply(PDBG_CMD_CONT, 3, PDBG_ERR_STATE, d);
}

static void test_halt_command_replies_then_announces_the_halt_and_cont_resumes(void)
{
    uint8_t frame[20], p[80];
    const uint8_t halt[2] = {PDBG_CMD_HALT, 1};
    const uint8_t cont[2] = {PDBG_CMD_CONT, 2};

    fake_regs.pc = 0x1234u;
    queue_bytes(frame, make_frame(halt, 2, frame));
    queue_bytes(frame, make_frame(cont, 2, frame));
    feed_all();
    TEST_ASSERT_EQUAL_UINT(3, tx_frames());  /* HALT reply, HALTED event, CONT reply */
    tx_frame(0, p);
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_REPLY, p[0]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_CMD_HALT, p[2]);
    TEST_ASSERT_EQUAL_UINT(7, tx_frame(1, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_EVENT, p[0]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_EV_HALTED, p[1]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_REASON_REQUEST, p[2]);
    TEST_ASSERT_EQUAL_HEX32(0x1234, p[4] | (p[5] << 8) | (p[6] << 16));
    tx_frame(2, p);
    TEST_ASSERT_EQUAL_UINT8(PDBG_CMD_CONT, p[2]);
    TEST_ASSERT_TRUE(service_calls > 0);
}

static void test_an_empty_frame_halts_without_a_reply(void)
{
    static const uint8_t two_zeros[2] = {0, 0};
    uint8_t cont[2] = {PDBG_CMD_CONT, 5}, frame[20], p[80];

    queue_bytes(two_zeros, 2);
    queue_bytes(frame, make_frame(cont, 2, frame));
    feed_all();
    /* The first zero ends an empty frame already, so it halts; the second repeats the
     * announcement. Then CONT. */
    TEST_ASSERT_EQUAL_UINT(3, tx_frames());
    TEST_ASSERT_EQUAL_UINT(7, tx_frame(0, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_EVENT, p[0]);
    TEST_ASSERT_EQUAL_UINT(7, tx_frame(1, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_EVENT, p[0]);
    TEST_ASSERT_EQUAL_UINT(4, tx_frame(2, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_REPLY, p[0]);
}

static void test_regs_stack_and_wr_while_halted(void)
{
    uint8_t cont[2] = {PDBG_CMD_CONT, 6}, regs[2] = {PDBG_CMD_REGS, 1};
    uint8_t stk[3] = {PDBG_CMD_STACK, 2, 1};
    uint8_t wr[7] = {PDBG_CMD_WR, 3, 0x40, 0x00, 0x00, 0xFF, 0x00};
    uint8_t frame[100], p[80];

    fake_regs.pc = 0x00ABCDu;
    fake_regs.wreg = 0x5A;
    fake_regs.status = 0x1F;
    fake_regs.bsr = 0x03;
    fake_regs.fsr0 = 0x0123;
    fake_regs.fsr1 = 0x0456;
    fake_regs.fsr2 = 0x0789;
    fake_regs.prod = 0xBEEF;
    fake_regs.tblptr = 0x1ABCDE;
    fake_regs.tablat = 0xC3;
    fake_regs.stkptr = 4;
    fake_stack_depth = 4;
    fake_stack[0] = 0x000100u;
    fake_stack[1] = 0x000200u;
    fake_stack[2] = 0x030000u;
    fake_stack[3] = 0x000004u;

    queue_bytes((const uint8_t *)"\0", 1);  /* halt */
    queue_bytes(frame, make_frame(regs, 2, frame));
    queue_bytes(frame, make_frame(stk, 3, frame));
    queue_bytes(frame, make_frame(wr, 7, frame));
    queue_bytes(frame, make_frame(cont, 2, frame));
    feed_all();
    TEST_ASSERT_EQUAL_UINT(5, tx_frames());

    TEST_ASSERT_EQUAL_UINT(4 + 19, tx_frame(1, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_OK, p[3]);
    TEST_ASSERT_EQUAL_HEX32(0xABCD, p[4] | (p[5] << 8) | (p[6] << 16));
    TEST_ASSERT_EQUAL_HEX8(0x5A, p[7]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, p[8]);
    TEST_ASSERT_EQUAL_HEX8(0x03, p[9]);
    TEST_ASSERT_EQUAL_HEX16(0x0123, p[10] | (p[11] << 8));
    TEST_ASSERT_EQUAL_HEX16(0x0456, p[12] | (p[13] << 8));
    TEST_ASSERT_EQUAL_HEX16(0x0789, p[14] | (p[15] << 8));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, p[16] | (p[17] << 8));
    TEST_ASSERT_EQUAL_HEX32(0x1ABCDE, p[18] | (p[19] << 8) | (p[20] << 16));
    TEST_ASSERT_EQUAL_HEX8(0xC3, p[21]);
    TEST_ASSERT_EQUAL_UINT8(4, p[22]);

    /* STACK from index 1: depth 4, 3 entries, top-first. */
    TEST_ASSERT_EQUAL_UINT(4 + 2 + 9, tx_frame(2, p));
    TEST_ASSERT_EQUAL_UINT8(4, p[4]);
    TEST_ASSERT_EQUAL_UINT8(3, p[5]);
    TEST_ASSERT_EQUAL_HEX32(0x000200, p[6] | (p[7] << 8) | (p[8] << 16));
    TEST_ASSERT_EQUAL_HEX32(0x030000, p[9] | (p[10] << 8) | (p[11] << 16));
    TEST_ASSERT_EQUAL_HEX32(0x000004, p[12] | (p[13] << 8) | (p[14] << 16));

    TEST_ASSERT_EQUAL_UINT8(PDBG_OK, (tx_frame(3, p), p[3]));
    TEST_ASSERT_EQUAL_UINT8(0x00, fake_ram[0x40]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, fake_ram[0x41]);
    TEST_ASSERT_EQUAL_UINT8(0x00, fake_ram[0x42]);
}

static void test_stack_start_past_the_depth_is_refused_and_a_deep_stack_is_chunked(void)
{
    uint8_t bad[3] = {PDBG_CMD_STACK, 1, 32}, ok[3] = {PDBG_CMD_STACK, 2, 0};
    uint8_t cont[2] = {PDBG_CMD_CONT, 3}, frame[100], p[80];

    fake_stack_depth = 31;
    queue_bytes((const uint8_t *)"\0", 1);
    queue_bytes(frame, make_frame(bad, 3, frame));
    queue_bytes(frame, make_frame(ok, 3, frame));
    queue_bytes(frame, make_frame(cont, 2, frame));
    feed_all();
    tx_frame(1, p);
    TEST_ASSERT_EQUAL_UINT8(PDBG_ERR_ARG, p[3]);
    TEST_ASSERT_EQUAL_UINT(4 + 2 + 3 * PDBG_MAX_STACK, tx_frame(2, p));
    TEST_ASSERT_EQUAL_UINT8(31, p[4]);
    TEST_ASSERT_EQUAL_UINT8(PDBG_MAX_STACK, p[5]);
}

/* -- breakpoints ------------------------------------------------------------------------ */

static void test_a_disabled_site_does_not_halt_and_an_enabled_one_does(void)
{
    uint8_t on[4] = {PDBG_CMD_BP, 1, 5, 1}, off[4] = {PDBG_CMD_BP, 2, 5, 0};
    uint8_t cont[2] = {PDBG_CMD_CONT, 3}, frame[100], p[80], d[80];

    pdbg_break(5);
    TEST_ASSERT_EQUAL_UINT(0, tx_len);

    request(PDBG_CMD_BP, 1, &on[2], 2);
    TEST_ASSERT_EQUAL_UINT(4, reply(PDBG_CMD_BP, 1, PDBG_OK, d));
    TEST_ASSERT_EQUAL_HEX32(1u << 5, (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                                         ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24));

    fake_regs.pc = 0x2000;
    queue_bytes(frame, make_frame(cont, 2, frame));
    tx_len = 0;
    pdbg_break(5);  /* halts, serves the queued CONT, comes back */
    TEST_ASSERT_EQUAL_UINT(2, tx_frames());
    tx_frame(0, p);
    TEST_ASSERT_EQUAL_UINT8(PDBG_REASON_BREAK, p[2]);
    TEST_ASSERT_EQUAL_UINT8(5, p[3]);
    TEST_ASSERT_EQUAL_UINT8(1, fake_irq_enabled);  /* restored after the halt */

    request(PDBG_CMD_BP, 2, &off[2], 2);
    TEST_ASSERT_EQUAL_UINT(4, reply(PDBG_CMD_BP, 2, PDBG_OK, d));
    TEST_ASSERT_EQUAL_UINT8(0, d[0]);
    tx_len = 0;
    pdbg_break(5);
    TEST_ASSERT_EQUAL_UINT(0, tx_len);
}

static void test_bp_queries_and_rejects_a_site_out_of_range(void)
{
    static const uint8_t bad[2] = {PDBG_BP_SITES, 1};
    uint8_t d[80];

    request(PDBG_CMD_BP, 1, bad, 2);
    reply(PDBG_CMD_BP, 1, PDBG_ERR_ARG, d);
    request(PDBG_CMD_BP, 2, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(4, reply(PDBG_CMD_BP, 2, PDBG_OK, d));
}

static void test_interrupts_are_off_while_halted_and_back_after(void)
{
    uint8_t cont[2] = {PDBG_CMD_CONT, 3}, frame[100];

    queue_bytes(frame, make_frame(cont, 2, frame));
    pdbg_halt(PDBG_REASON_ASSERT, 0);
    TEST_ASSERT_EQUAL_UINT8(1, fake_irq_enabled);
}

/* -- console and reset ------------------------------------------------------------------ */

static void test_console_text_goes_out_in_frames_of_at_most_max_data(void)
{
    uint8_t text[PDBG_MAX_DATA + 5], p[80];

    for (unsigned i = 0; i < sizeof text; i++) {
        text[i] = (uint8_t)(i == 3 ? 0 : 'a' + i % 20);
    }
    pdbg_console(text, sizeof text);
    TEST_ASSERT_EQUAL_UINT(2, tx_frames());
    TEST_ASSERT_EQUAL_UINT(1 + PDBG_MAX_DATA, tx_frame(0, p));
    TEST_ASSERT_EQUAL_UINT8(PDBG_CH_CONSOLE, p[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(text, &p[1], PDBG_MAX_DATA);
    TEST_ASSERT_EQUAL_UINT(1 + 5, tx_frame(1, p));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&text[PDBG_MAX_DATA], &p[1], 5);
    TEST_ASSERT_EQUAL_UINT8(1, fake_irq_enabled);
}

static void test_reset_answers_first_then_resets(void)
{
    uint8_t d[80];

    request(PDBG_CMD_RESET, 4, NULL, 0);
    reply(PDBG_CMD_RESET, 4, PDBG_OK, d);
    TEST_ASSERT_EQUAL_UINT(1, fake_resets);
}

static void test_cobs_handles_a_254_byte_run(void)
{
    uint8_t in[300], enc[320], out[320];
    unsigned n;

    memset(in, 0x33, sizeof in);
    in[260] = 0;
    n = cobs_encode(in, sizeof in, enc);
    TEST_ASSERT_EQUAL_UINT(sizeof in, cobs_decode(enc, n, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, sizeof in);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_the_ccitt_false_check_value);
    RUN_TEST(test_hello_reports_version_device_and_reset_cause);
    RUN_TEST(test_a_frame_with_a_bad_crc_gets_no_answer);
    RUN_TEST(test_garbage_before_a_frame_costs_only_that_frame);
    RUN_TEST(test_an_oversize_frame_is_skipped_whole);
    RUN_TEST(test_zeros_in_a_reply_survive_the_encoding);
    RUN_TEST(test_an_unknown_command_is_refused);
    RUN_TEST(test_rd_reads_data_memory_while_running);
    RUN_TEST(test_rd_checks_its_length);
    RUN_TEST(test_rd_of_a_full_buffer_fits_in_a_reply);
    RUN_TEST(test_wr_needs_the_agent_halted);
    RUN_TEST(test_rdpfm_reads_program_flash);
    RUN_TEST(test_state_commands_refuse_while_running);
    RUN_TEST(test_halt_command_replies_then_announces_the_halt_and_cont_resumes);
    RUN_TEST(test_an_empty_frame_halts_without_a_reply);
    RUN_TEST(test_regs_stack_and_wr_while_halted);
    RUN_TEST(test_stack_start_past_the_depth_is_refused_and_a_deep_stack_is_chunked);
    RUN_TEST(test_a_disabled_site_does_not_halt_and_an_enabled_one_does);
    RUN_TEST(test_bp_queries_and_rejects_a_site_out_of_range);
    RUN_TEST(test_interrupts_are_off_while_halted_and_back_after);
    RUN_TEST(test_console_text_goes_out_in_frames_of_at_most_max_data);
    RUN_TEST(test_reset_answers_first_then_resets);
    RUN_TEST(test_cobs_handles_a_254_byte_run);
    return UNITY_END();
}
