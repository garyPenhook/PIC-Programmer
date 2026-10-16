/*
 * Unity tests for the SSD1309 driver (src/oled.c) and the status screen (src/ui.c), against
 * a stub I2C bus that records each transfer (stub_i2c.c). The expected bytes come from the
 * SSD1309 datasheet: control byte 0x00 before commands and 0x40 before RAM data (8.1.5.2),
 * and the commands in Tables 9-1, 9-3 and 9-4.
 */
#include <string.h>

#include "i2c.h"
#include "oled.h"
#include "pic.h"
#include "pic_proto.h"
#include "stub_i2c.h"
#include "ui.h"
#include "unity.h"

static const uint8_t init_cmds[] = {0x00, 0xFD, 0x12, 0xAE, 0x20, 0x00};

void setUp(void)
{
    stub_i2c_reset();
}

void tearDown(void)
{
}

static void assert_transfer(size_t index, uint8_t addr, const uint8_t *bytes, size_t len)
{
    const stub_i2c_transfer_t *t = stub_i2c_get(index);

    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_HEX8(addr, t->addr);
    TEST_ASSERT_EQUAL_UINT(len, t->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, t->bytes, len);
}

/* The two transfers that write one page: the window commands, then 0x40 and 128 bytes. */
static void assert_page(size_t index, uint8_t addr, uint8_t page, const uint8_t *columns)
{
    const uint8_t window[] = {0x00, 0x21, 0x00, 0x7F, 0x22, page, page};
    uint8_t data[129];

    data[0] = 0x40;
    memcpy(data + 1, columns, 128);
    assert_transfer(index, addr, window, sizeof window);
    assert_transfer(index + 1u, addr, data, sizeof data);
}

static void init_at(uint8_t addr)
{
    stub_i2c_attach(addr);
    TEST_ASSERT_EQUAL_INT(1, oled_init());
    stub_i2c_reset();
    stub_i2c_attach(addr);
}

#if !I2C_IGNORE_ACK
static void test_no_display_means_no_traffic_after_the_search(void)
{
    TEST_ASSERT_EQUAL_INT(0, oled_init());
    TEST_ASSERT_EQUAL_UINT(2u, stub_i2c_count()); /* 0x3C, then 0x3D */
    TEST_ASSERT_EQUAL_HEX8(0x3C, stub_i2c_get(0)->addr);
    TEST_ASSERT_EQUAL_HEX8(0x3D, stub_i2c_get(1)->addr);
    TEST_ASSERT_EQUAL_INT(0, oled_present());

    oled_line(0, "hello");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    ui_probe(PIC_ERR_NO_TARGET, NULL, 0u, 0u);
    ui_result("READ", PIC_OK, 4u);
    TEST_ASSERT_EQUAL_UINT(2u, stub_i2c_count());
}

#else
static void test_ignore_ack_build_uses_only_the_configured_address(void)
{
    /* With acknowledges ignored, the search cannot tell addresses apart, so only
     * OLED_I2C_ADDR (0x3C by default) is ever used. */
    stub_i2c_attach(0x3C);
    TEST_ASSERT_EQUAL_INT(1, oled_init());
    for (size_t i = 0; i < stub_i2c_count(); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x3C, stub_i2c_get(i)->addr);
    }
    assert_transfer(0, 0x3C, init_cmds, sizeof init_cmds);
}
#endif

static void test_init_sequence_clears_ram_then_turns_the_panel_on(void)
{
    const uint8_t zeros[128] = {0};
    const uint8_t on[] = {0x00, 0xAF};

    stub_i2c_attach(0x3C);
    TEST_ASSERT_EQUAL_INT(1, oled_init());
    TEST_ASSERT_EQUAL_UINT(1u + 16u + 1u, stub_i2c_count());
    assert_transfer(0, 0x3C, init_cmds, sizeof init_cmds);
    for (uint8_t p = 0; p < 8u; p++) {
        assert_page(1u + 2u * p, 0x3C, p, zeros);
    }
    assert_transfer(17, 0x3C, on, sizeof on);
}

static void test_reset_is_pulsed_before_any_transfer(void)
{
    stub_i2c_attach(0x3C);
    TEST_ASSERT_EQUAL_INT(1, oled_init());
    TEST_ASSERT_EQUAL_INT(1, stub_oled_resets());
    TEST_ASSERT_EQUAL_UINT(0u, stub_oled_reset_at());
}

#if !I2C_IGNORE_ACK
static void test_second_address_is_used_when_sa0_is_high(void)
{
    stub_i2c_attach(0x3D);
    TEST_ASSERT_EQUAL_INT(1, oled_init());
    TEST_ASSERT_EQUAL_HEX8(0x3C, stub_i2c_get(0)->addr); /* NACKed */
    assert_transfer(1, 0x3D, init_cmds, sizeof init_cmds);
    TEST_ASSERT_EQUAL_HEX8(0x3D, stub_i2c_get(2)->addr);
}

#endif

static void test_a_glyph_lands_in_its_page_and_columns(void)
{
    uint8_t expected[128] = {0};
    const uint8_t a[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E};

    init_at(0x3C);
    memcpy(expected + 6, a, 5); /* second cell: columns 6 to 10, then a blank column */
    oled_line(1, " A");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    TEST_ASSERT_EQUAL_UINT(2u, stub_i2c_count());
    assert_page(0, 0x3C, 1, expected);
}

static void test_unchanged_text_sends_nothing(void)
{
    init_at(0x3C);
    oled_line(3, "same");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    stub_i2c_reset();
    stub_i2c_attach(0x3C);
    oled_line(3, "same");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    TEST_ASSERT_EQUAL_UINT(0u, stub_i2c_count());
}

static void test_only_changed_pages_are_sent_in_order(void)
{
    init_at(0x3C);
    oled_line(5, "x");
    oled_line(2, "y");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    TEST_ASSERT_EQUAL_UINT(4u, stub_i2c_count());
    TEST_ASSERT_EQUAL_HEX8(2, stub_i2c_get(0)->bytes[5]);
    TEST_ASSERT_EQUAL_HEX8(5, stub_i2c_get(2)->bytes[5]);
}

static void test_long_text_is_cut_and_unprintable_shows_question_mark(void)
{
    const uint8_t x[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
    const uint8_t question[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    const stub_i2c_transfer_t *data;

    init_at(0x3C);
    oled_line(0, "\001XXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    data = stub_i2c_get(1);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(question, data->bytes + 1, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x, data->bytes + 1 + 120, 5); /* 21st cell */
    TEST_ASSERT_EQUAL_HEX8(0, data->bytes[1 + 126]);           /* right edge stays blank */
    TEST_ASSERT_EQUAL_HEX8(0, data->bytes[1 + 127]);
}

static void test_display_that_stops_answering_is_dropped(void)
{
    init_at(0x3C);
    stub_i2c_fail_after(0);
    oled_line(4, "gone");
    TEST_ASSERT_EQUAL_INT(-1, oled_flush());
    TEST_ASSERT_EQUAL_INT(0, oled_present());
    stub_i2c_reset();
    oled_line(4, "still gone");
    TEST_ASSERT_EQUAL_INT(0, oled_flush());
    ui_result("READ", PIC_OK, 1u);
    TEST_ASSERT_EQUAL_UINT(0u, stub_i2c_count());
}

static void test_ui_probe_redraws_device_ids_and_result_rows(void)
{
    const pic_device_t *dev = pic_find_device(&pic_proto_p16_8bit, 0x30E1u);

    TEST_ASSERT_NOT_NULL(dev);
    stub_i2c_attach(0x3C);
    ui_init();
    stub_i2c_reset();
    stub_i2c_attach(0x3C);

    ui_probe(PIC_OK, dev, 0x30E1u, 0x2000u);
    /* Rows 2 (device), 3 (IDs) and 5 (last command) change; row 6 stays blank. */
    TEST_ASSERT_EQUAL_UINT(6u, stub_i2c_count());
    TEST_ASSERT_EQUAL_HEX8(2, stub_i2c_get(0)->bytes[5]);
    TEST_ASSERT_EQUAL_HEX8(3, stub_i2c_get(2)->bytes[5]);
    TEST_ASSERT_EQUAL_HEX8(5, stub_i2c_get(4)->bytes[5]);

    /* A failed read adds the error text on row 6. */
    stub_i2c_reset();
    stub_i2c_attach(0x3C);
    ui_result("READ", PIC_ERR_PROTOCOL, 0u);
    TEST_ASSERT_EQUAL_UINT(4u, stub_i2c_count());
    TEST_ASSERT_EQUAL_HEX8(5, stub_i2c_get(0)->bytes[5]);
    TEST_ASSERT_EQUAL_HEX8(6, stub_i2c_get(2)->bytes[5]);
}

int main(void)
{
    UNITY_BEGIN();
#if !I2C_IGNORE_ACK
    RUN_TEST(test_no_display_means_no_traffic_after_the_search);
#else
    RUN_TEST(test_ignore_ack_build_uses_only_the_configured_address);
#endif
    RUN_TEST(test_init_sequence_clears_ram_then_turns_the_panel_on);
    RUN_TEST(test_reset_is_pulsed_before_any_transfer);
#if !I2C_IGNORE_ACK
    RUN_TEST(test_second_address_is_used_when_sa0_is_high);
#endif
    RUN_TEST(test_a_glyph_lands_in_its_page_and_columns);
    RUN_TEST(test_unchanged_text_sends_nothing);
    RUN_TEST(test_only_changed_pages_are_sent_in_order);
    RUN_TEST(test_long_text_is_cut_and_unprintable_shows_question_mark);
    RUN_TEST(test_display_that_stops_answering_is_dropped);
    RUN_TEST(test_ui_probe_redraws_device_ids_and_result_rows);
    return UNITY_END();
}
