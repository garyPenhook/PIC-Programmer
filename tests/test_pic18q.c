/*
 * Host-side tests for the PIC18-Q driver (src/pic18q.c) against the simulated device in
 * sim_pic18q.c. They check protocol behaviour and that every wait the spec requires is
 * respected. They do not replace a test on real silicon: the simulator only knows what
 * the driver's author understood from the specification.
 */
#include <stdio.h>
#include <string.h>

#include "icsp.h"
#include "pic18q.h"
#include "sim_pic18q.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void fresh(void)
{
    sim_set_lvp(1);
    sim_reset();
}

static void test_checker_catches_short_waits(void)
{
    /* Prove the timing checker works, so "0 violations" elsewhere means something. */
    fresh();
    icsp_set_mclr(0u);
    icsp_set_clk(1u);               /* clock immediately after MCLR falls: far too soon */
    CHECK(sim_violations() >= 1u);
}

static void test_identify(void)
{
    pic18q_dci_t dci;

    fresh();
    pic18q_enter();
    CHECK(sim_in_program_mode());
    CHECK(pic18q_read_device_id() == 0x7560u);
    {
        const char *name = pic18q_device_name(pic18q_read_device_id());
        CHECK(name != NULL && strcmp(name, "PIC18F16Q41") == 0);
    }
    CHECK(pic18q_read_revision_id() == 0xA041u);

    pic18q_read_dci(&dci);
    CHECK(dci.erase_page_words == 128u);
    CHECK(dci.write_latches == 0u);
    CHECK(dci.user_pages == 256u);
    CHECK(dci.eeprom_bytes == 512u);
    CHECK(dci.pin_count == 20u);

    pic18q_exit();
    CHECK(!sim_in_program_mode());
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_lvp_disabled_target_is_not_reachable(void)
{
    fresh();
    sim_set_lvp(0);
    pic18q_enter();
    CHECK(!sim_in_program_mode());
    CHECK(pic18q_read_device_id() == 0u);        /* nothing answers */
    CHECK(pic18q_device_name(0u) == NULL);
    pic18q_exit();
    sim_set_lvp(1);
}

static void test_erase_write_verify_flash(void)
{
    uint16_t data[40];
    uint16_t back[40];

    fresh();
    for (uint32_t i = 0; i < 40u; i++) {
        data[i] = (uint16_t)(0x1000u + 7u * i);
        sim_set_flash_word(128u + i, 0x5A5Au);   /* old, non-blank contents */
    }

    pic18q_enter();
    CHECK(pic18q_bulk_erase(PIC18Q_ERASE_FLASH) == PIC18Q_OK);
    CHECK(sim_flash_word(128) == 0xFFFFu);       /* byte address 0x100 is word 128 */

    CHECK(pic18q_write_words(0x100u, data, 40u) == PIC18Q_OK);
    CHECK(pic18q_verify_words(0x100u, data, 40u) == PIC18Q_OK);
    CHECK(pic18q_read_words(0x100u, back, 40u) == PIC18Q_OK);
    CHECK(memcmp(data, back, sizeof data) == 0);
    for (uint32_t i = 0; i < 40u; i++) {
        CHECK(sim_flash_word(128u + i) == data[i]);
    }
    CHECK(sim_flash_word(127) == 0xFFFFu);       /* neighbours untouched */
    CHECK(sim_flash_word(168) == 0xFFFFu);
    pic18q_exit();

    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_programming_only_clears_bits(void)
{
    uint16_t first = 0x0F0Fu;
    uint16_t second = 0xF0F0u;

    fresh();
    pic18q_enter();
    CHECK(pic18q_write_words(0x0u, &first, 1u) == PIC18Q_OK);
    CHECK(pic18q_write_words(0x0u, &second, 1u) == PIC18Q_OK);   /* no erase in between */
    CHECK(sim_flash_word(0) == 0x0000u);                          /* old AND new */
    CHECK(pic18q_verify_words(0x0u, &second, 1u) == PIC18Q_ERR_VERIFY);
    pic18q_exit();
}

static void test_page_erase_only_affects_one_page(void)
{
    uint16_t v = 0x1234u;

    fresh();
    pic18q_enter();
    CHECK(pic18q_write_words(0x000u, &v, 1u) == PIC18Q_OK);   /* page 0 */
    CHECK(pic18q_write_words(0x0FEu, &v, 1u) == PIC18Q_OK);   /* last word of page 0 */
    CHECK(pic18q_write_words(0x100u, &v, 1u) == PIC18Q_OK);   /* page 1 */
    CHECK(pic18q_page_erase(0x010u) == PIC18Q_OK);            /* any address inside page 0 */
    CHECK(sim_flash_word(0) == 0xFFFFu);
    CHECK(sim_flash_word(127) == 0xFFFFu);
    CHECK(sim_flash_word(128) == 0x1234u);                    /* page 1 survives */
    pic18q_exit();
    CHECK(sim_violations() == 0u);
}

static void test_eeprom_config_and_userid(void)
{
    uint8_t ee[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t ee_back[5];
    uint8_t cfg = 0xA7u;
    uint16_t id[4] = { 0xBEEFu, 0xCAFEu, 0x0123u, 0x4567u };

    fresh();
    pic18q_enter();

    CHECK(pic18q_write_bytes(PIC18Q_ADDR_EEPROM + 0x10u, ee, 5u) == PIC18Q_OK);
    CHECK(pic18q_verify_bytes(PIC18Q_ADDR_EEPROM + 0x10u, ee, 5u) == PIC18Q_OK);
    CHECK(pic18q_read_bytes(PIC18Q_ADDR_EEPROM + 0x10u, ee_back, 5u) == PIC18Q_OK);
    CHECK(memcmp(ee, ee_back, 5u) == 0);
    CHECK(sim_eeprom_byte(0x10) == 0x11u && sim_eeprom_byte(0x14) == 0x55u);

    CHECK(pic18q_write_bytes(PIC18Q_ADDR_CONFIG + 2u, &cfg, 1u) == PIC18Q_OK);
    CHECK(sim_config_byte(2) == 0xA7u);

    CHECK(pic18q_write_words(PIC18Q_ADDR_USERID, id, 4u) == PIC18Q_OK);
    CHECK(pic18q_verify_words(PIC18Q_ADDR_USERID, id, 4u) == PIC18Q_OK);
    CHECK(sim_userid_word(3) == 0x4567u);

    CHECK(pic18q_bulk_erase(PIC18Q_ERASE_EEPROM | PIC18Q_ERASE_USERID | PIC18Q_ERASE_CONFIG) == PIC18Q_OK);
    CHECK(sim_eeprom_byte(0x10) == 0xFFu);
    CHECK(sim_userid_word(0) == 0xFFFFu);
    CHECK(sim_config_byte(2) == 0xFFu);

    pic18q_exit();
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_bad_arguments_send_nothing(void)
{
    uint16_t w = 0u;
    uint8_t b = 0u;
    uint16_t many[33] = { 0 };
    uint64_t t0;

    fresh();
    pic18q_enter();
    t0 = sim_time_us();

    CHECK(pic18q_write_words(0x001u, &w, 1u) == PIC18Q_ERR_ARG);                     /* odd address */
    CHECK(pic18q_write_words(PIC18Q_ADDR_EEPROM, &w, 1u) == PIC18Q_ERR_ARG);        /* wrong region */
    CHECK(pic18q_write_words(PIC18Q_ADDR_DEVID, &w, 1u) == PIC18Q_ERR_ARG);         /* read-only */
    CHECK(pic18q_write_words(PIC18Q_ADDR_USERID, many, 33u) == PIC18Q_ERR_ARG);     /* runs past User ID */
    CHECK(pic18q_write_bytes(0x0u, &b, 1u) == PIC18Q_ERR_ARG);                      /* flash is words */
    CHECK(pic18q_write_bytes(PIC18Q_ADDR_CONFIG + PIC18Q_CONFIG_MAX, &b, 1u) == PIC18Q_ERR_ARG);
    CHECK(pic18q_read_words(0x3u, many, 1u) == PIC18Q_ERR_ARG);
    CHECK(pic18q_page_erase(PIC18Q_ADDR_EEPROM) == PIC18Q_ERR_ARG);                 /* page erase: flash/ID only */
    CHECK(pic18q_bulk_erase(0u) == PIC18Q_ERR_ARG);
    CHECK(pic18q_bulk_erase(0x10u) == PIC18Q_ERR_ARG);                              /* unknown region bit */

    CHECK(sim_time_us() == t0);                  /* none of that touched the wires */
    pic18q_exit();
}

static void test_exit_releases_target(void)
{
    fresh();
    pic18q_enter();
    pic18q_exit();
    CHECK(!sim_in_program_mode());
    pic18q_enter();                              /* can be entered again */
    CHECK(pic18q_read_device_id() == 0x7560u);
    pic18q_exit();
}

int main(void)
{
    test_checker_catches_short_waits();
    test_identify();
    test_lvp_disabled_target_is_not_reachable();
    test_erase_write_verify_flash();
    test_programming_only_clears_bits();
    test_page_erase_only_affects_one_page();
    test_eeprom_config_and_userid();
    test_bad_arguments_send_nothing();
    test_exit_releases_target();

    if (failures == 0) {
        printf("all PIC18-Q driver tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
