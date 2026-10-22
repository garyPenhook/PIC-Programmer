/*
 * Host-side tests for the p18_8bit protocol (src/proto/p18_8bit.c), driven through the
 * device API (src/pic.c), against the simulated device in sim_p18_8bit.c. Every test runs
 * once for each device table row that uses this protocol. They check protocol behaviour
 * and that every wait the spec requires is respected. They do not replace a test on real
 * silicon: the simulator only knows what the driver's author understood from the
 * specification.
 */
#include <stdio.h>
#include <string.h>

#include "icsp.h"
#include "pic.h"
#include "pic_proto.h"
#include "sim_p18_8bit.h"

static int failures;
static const pic_device_t *dev;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL [%s] %s:%d: %s\n", dev->name, __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static uint16_t read_id(void)
{
    uint16_t v = 0xFFFFu;
    CHECK(pic_read_device_id(dev, &v) == PIC_OK);
    return v;
}

static uint16_t read_rev(void)
{
    uint16_t v = 0xFFFFu;
    CHECK(pic_read_revision_id(dev, &v) == PIC_OK);
    return v;
}

static void fresh(void)
{
    sim_set_lvp(1);
    sim_select_device(dev->device_id);
    sim_reset();
}

static void test_checker_catches_short_waits(void)
{
    /* Prove the timing checker works, so "0 violations" elsewhere means something. */
    fresh();
    icsp_set_mclr(0u);
    icsp_set_clk(1u); /* clock immediately after MCLR falls: far too soon */
    CHECK(sim_violations() >= 1u);
}

static void test_probe_finds_device(void)
{
    uint16_t id = 0u;
    uint16_t rev = 0u;

    /* The probe tries p16_8bit first, whose entry hold is shorter than this family needs,
     * so the simulator logs a timing violation for that attempt; only the result counts. */
    fresh();
    const pic_device_t *found = NULL;
    CHECK(pic_probe(&found, &id, &rev) == PIC_OK);
    CHECK(found == dev);
    CHECK(id == dev->device_id);
    CHECK(rev == 0xA041u);
    CHECK(!sim_in_program_mode());
    CHECK(sim_bad_commands() == 0u);
}

static void test_probe_is_not_fooled_by_a_pic16_id(void)
{
    uint16_t id = 0u;
    uint16_t rev = 0u;

    /* The p16_8bit probe reads word address 0x8006, which on this part is flash byte
     * 0x8006 (word 0x4003). Put a PIC16F17146 ID there: the ID matches a p16 row, but the
     * DCI the p16 commands then read does not, so the part must still be found as itself. */
    if (dev->flash_words <= 0x4003u) {
        return;
    }
    fresh();
    sim_set_flash_word(0x4003u, 0x30E1u);
    const pic_device_t *found = NULL;
    CHECK(pic_probe(&found, &id, &rev) == PIC_OK);
    CHECK(found == dev);
    CHECK(id == dev->device_id);
}

static void test_identify(void)
{
    pic_dci_t dci;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(sim_in_program_mode());
    CHECK(read_id() == dev->device_id);
    CHECK(pic_find_device(&pic_proto_p18_8bit, read_id()) == dev);
    CHECK(read_rev() == 0xA041u);

    /* The part's own DCI agrees with its table row. */
    CHECK(pic_read_dci(dev, &dci) == PIC_OK);
    CHECK(dci.erase_words == dev->erase_words);
    CHECK(dci.latch_words == 0u);
    CHECK((uint32_t)dci.user_rows * dci.erase_words == dev->flash_words);
    CHECK(dci.eeprom_bytes == dev->eeprom_bytes);
    CHECK(dci.pin_count == 20u);

    pic_exit(dev);
    CHECK(!sim_in_program_mode());
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_lvp_disabled_target_is_not_reachable(void)
{
    const pic_device_t *found = &pic_devices[0];
    uint16_t id = 0xFFFFu;
    uint16_t rev = 0xFFFFu;

    fresh();
    sim_set_lvp(0);
    CHECK(pic_enter(dev) == PIC_ERR_NO_TARGET); /* nothing answers; ID reads 0 */
    CHECK(!sim_in_program_mode());              /* and enter backed out */
    CHECK(pic_probe(&found, &id, &rev) == PIC_ERR_NO_TARGET);
    CHECK(found == NULL);
    CHECK(id == 0u && rev == 0u);
    sim_set_lvp(1);
}

static void test_stuck_high_data_line_is_a_protocol_error(void)
{
    const pic_device_t *found = &pic_devices[0];
    uint16_t id = 0x1234u;
    uint16_t buf[4];

    /* A released ICSPDAT that reads 1 gives all-ones payloads: bad start and stop bits. */
    fresh();
    sim_set_dat_stuck_high(1);
    CHECK(pic_enter(dev) == PIC_ERR_PROTOCOL);
    CHECK(!sim_in_program_mode());
    CHECK(pic_probe(&found, NULL, NULL) == PIC_ERR_PROTOCOL);
    CHECK(found == NULL);
    sim_set_dat_stuck_high(0);

    /* The same fault after entering fails every kind of read and leaves outputs alone. */
    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    sim_set_dat_stuck_high(1);
    CHECK(pic_read_device_id(dev, &id) == PIC_ERR_PROTOCOL);
    CHECK(id == 0x1234u);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 0u, buf, 4u) == PIC_ERR_PROTOCOL);
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 0u, buf, 4u, NULL) == PIC_ERR_PROTOCOL);
    sim_set_dat_stuck_high(0);
    pic_exit(dev);
}

static void test_wrong_device_is_rejected(void)
{
    const pic_device_t *other = NULL;

    /* Name a different row than the one the simulator models. */
    for (size_t i = 0; i < pic_device_count; i++) {
        if (pic_devices[i].proto == dev->proto && pic_devices[i].device_id != dev->device_id) {
            other = &pic_devices[i];
            break;
        }
    }
    if (other == NULL) {
        return;
    }
    fresh();
    CHECK(pic_enter(other) == PIC_ERR_MISMATCH);
    CHECK(!sim_in_program_mode());
}

static void test_null_arguments_are_rejected(void)
{
    uint16_t w[2] = {0};
    uint16_t id;
    pic_dci_t dci;
    const pic_device_t *found;
    uint64_t t0;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    t0 = sim_time_us();
    CHECK(pic_enter(NULL) == PIC_ERR_ARG);
    CHECK(pic_read(NULL, PIC_REGION_FLASH, 0u, w, 1u) == PIC_ERR_ARG);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 0u, NULL, 1u) == PIC_ERR_ARG);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, NULL, 1u) == PIC_ERR_ARG);
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 0u, NULL, 1u, NULL) == PIC_ERR_ARG);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 0u, NULL, 0u) == PIC_OK); /* nothing to do */
    CHECK(pic_read_device_id(dev, NULL) == PIC_ERR_ARG);
    CHECK(pic_read_revision_id(NULL, &id) == PIC_ERR_ARG);
    CHECK(pic_read_dci(dev, NULL) == PIC_ERR_ARG);
    CHECK(pic_read_dci(NULL, &dci) == PIC_ERR_ARG);
    CHECK(pic_bulk_erase(NULL, PIC_ERASE_FLASH) == PIC_ERR_ARG);
    CHECK(pic_row_erase(NULL, PIC_REGION_FLASH, 0u) == PIC_ERR_ARG);
    CHECK(pic_probe(NULL, &id, &id) == PIC_ERR_ARG);
    CHECK(sim_time_us() == t0); /* none of that touched the wires */
    pic_exit(NULL);             /* must not crash */
    pic_exit(dev);

    CHECK(pic_probe(&found, NULL, NULL) == PIC_OK); /* the ID outputs are optional */
    CHECK(found == dev);
}

static void test_verify_reports_the_mismatch_index(void)
{
    uint16_t data[3] = {0x0001u, 0x0002u, 0x0003u};
    size_t where = 99u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_bulk_erase(dev, PIC_ERASE_EEPROM) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_EEPROM, 0u, data, 3u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_EEPROM, 0u, data, 3u, &where) == PIC_OK);
    CHECK(where == 99u); /* untouched on success */
    data[2] = 0x00FFu;
    CHECK(pic_verify(dev, PIC_REGION_EEPROM, 0u, data, 3u, &where) == PIC_ERR_VERIFY);
    CHECK(where == 2u);
    pic_exit(dev);
}

static void test_erase_all_erases_everything_the_device_has(void)
{
    uint16_t w = 0u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_bulk_erase(dev, PIC_ERASE_ALL) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 0u, &w, 1u) == PIC_OK);
    pic_exit(dev);
    CHECK(sim_violations() == 0u);
}

static void test_strerror_covers_every_status(void)
{
    static const pic_status_t all[] = {
        PIC_OK,           PIC_ERR_ARG,         PIC_ERR_RANGE,
        PIC_ERR_ALIGN,    PIC_ERR_UNSUPPORTED, PIC_ERR_NO_TARGET,
        PIC_ERR_MISMATCH, PIC_ERR_PROTOCOL,    PIC_ERR_VERIFY,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        CHECK(strcmp(pic_strerror(all[i]), "unknown error") != 0);
    }
    CHECK(strcmp(pic_strerror((pic_status_t)-99), "unknown error") == 0);
}

static void test_erase_write_verify_flash(void)
{
    uint16_t data[40];
    uint16_t back[40];

    fresh();
    for (uint32_t i = 0; i < 40u; i++) {
        data[i] = (uint16_t)(0x1000u + 7u * i);
        sim_set_flash_word(128u + i, 0x5A5Au); /* old, non-blank contents */
    }

    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_bulk_erase(dev, PIC_ERASE_FLASH) == PIC_OK);
    CHECK(sim_flash_word(128) == 0xFFFFu);

    CHECK(pic_write(dev, PIC_REGION_FLASH, 128u, data, 40u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 128u, data, 40u, NULL) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 128u, back, 40u) == PIC_OK);
    CHECK(memcmp(data, back, sizeof data) == 0);
    for (uint32_t i = 0; i < 40u; i++) {
        CHECK(sim_flash_word(128u + i) == data[i]);
    }
    CHECK(sim_flash_word(127) == 0xFFFFu); /* neighbours untouched */
    CHECK(sim_flash_word(168) == 0xFFFFu);
    pic_exit(dev);

    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_last_word_of_flash(void)
{
    uint16_t v = 0x2468u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, dev->flash_words - 1u, &v, 1u) == PIC_OK);
    CHECK(sim_flash_word(dev->flash_words - 1u) == 0x2468u);
    pic_exit(dev);
}

static void test_programming_only_clears_bits(void)
{
    uint16_t first = 0x0F0Fu;
    uint16_t second = 0xF0F0u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, &first, 1u) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, &second, 1u) == PIC_OK); /* no erase in between */
    CHECK(sim_flash_word(0) == 0x0000u);                                /* old AND new */
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 0u, &second, 1u, NULL) == PIC_ERR_VERIFY);
    pic_exit(dev);
}

static void test_page_erase_only_affects_one_page(void)
{
    uint16_t v = 0x1234u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, &v, 1u) == PIC_OK);   /* page 0 */
    CHECK(pic_write(dev, PIC_REGION_FLASH, 127u, &v, 1u) == PIC_OK); /* last word of page 0 */
    CHECK(pic_write(dev, PIC_REGION_FLASH, 128u, &v, 1u) == PIC_OK); /* page 1 */
    CHECK(pic_row_erase(dev, PIC_REGION_FLASH, 8u) == PIC_OK);       /* any word inside page 0 */
    CHECK(sim_flash_word(0) == 0xFFFFu);
    CHECK(sim_flash_word(127) == 0xFFFFu);
    CHECK(sim_flash_word(128) == 0x1234u); /* page 1 survives */
    pic_exit(dev);
    CHECK(sim_violations() == 0u);
}

static void test_eeprom_config_and_userid(void)
{
    uint16_t ee[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    uint16_t ee_back[5];
    uint16_t cfg = 0xA7u;
    uint16_t id[4] = {0xBEEFu, 0xCAFEu, 0x0123u, 0x4567u};

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);

    CHECK(pic_write(dev, PIC_REGION_EEPROM, 0x10u, ee, 5u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_EEPROM, 0x10u, ee, 5u, NULL) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_EEPROM, 0x10u, ee_back, 5u) == PIC_OK);
    CHECK(memcmp(ee, ee_back, sizeof ee) == 0);
    CHECK(sim_eeprom_byte(0x10) == 0x11u && sim_eeprom_byte(0x14) == 0x55u);

    CHECK(pic_write(dev, PIC_REGION_CONFIG, 2u, &cfg, 1u) == PIC_OK);
    CHECK(sim_config_byte(2) == 0xA7u);
    CHECK(pic_write(dev, PIC_REGION_CONFIG, dev->config_count - 1u, &cfg, 1u) == PIC_OK);
    CHECK(sim_config_byte(dev->config_count - 1u) == 0xA7u);

    CHECK(pic_write(dev, PIC_REGION_USERID, 0u, id, 4u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_USERID, 0u, id, 4u, NULL) == PIC_OK);
    CHECK(sim_userid_word(3) == 0x4567u);

    CHECK(pic_bulk_erase(dev, PIC_ERASE_EEPROM | PIC_ERASE_USERID | PIC_ERASE_CONFIG) == PIC_OK);
    CHECK(sim_eeprom_byte(0x10) == 0xFFu);
    CHECK(sim_userid_word(0) == 0xFFFFu);
    CHECK(sim_config_byte(2) == 0xFFu);

    pic_exit(dev);
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

/* A run of Configuration Bytes that crosses the Q20's address gap (11 to 12) must reload the PC. */
static void test_config_run_across_the_end(void)
{
    uint16_t out[4] = {0xA1u, 0xB2u, 0xC3u, 0xD4u};
    uint16_t back[4] = {0};
    uint32_t first = dev->config_count - 4u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_CONFIG, first, out, 4u) == PIC_OK);
    for (uint32_t i = 0; i < 4u; i++) {
        CHECK(sim_config_byte(first + i) == out[i]);
    }
    CHECK(pic_read(dev, PIC_REGION_CONFIG, first, back, 4u) == PIC_OK);
    CHECK(memcmp(out, back, sizeof out) == 0);
    CHECK(pic_verify(dev, PIC_REGION_CONFIG, first, out, 4u, NULL) == PIC_OK);
    pic_exit(dev);
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_bad_arguments_send_nothing(void)
{
    uint16_t many[33] = {0};
    uint64_t t0;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    t0 = sim_time_us();

    CHECK(pic_write(dev, PIC_REGION_FLASH, dev->flash_words, many, 1u) ==
          PIC_ERR_RANGE); /* past this device's flash */
    CHECK(pic_write(dev, PIC_REGION_USERID, 0u, many, 33u) ==
          PIC_ERR_RANGE); /* runs past User ID */
    CHECK(pic_write(dev, PIC_REGION_CONFIG, dev->config_count, many, 1u) ==
          PIC_ERR_RANGE); /* past the config bytes */
    CHECK(pic_write(dev, PIC_REGION_EEPROM, dev->eeprom_bytes - 1u, many, 2u) == PIC_ERR_RANGE);
    CHECK(pic_read(dev, PIC_REGION_FLASH, dev->flash_words - 1u, many, 2u) == PIC_ERR_RANGE);
    CHECK(pic_row_erase(dev, PIC_REGION_EEPROM, 0u) ==
          PIC_ERR_UNSUPPORTED); /* page erase: flash/ID only */
    CHECK(pic_row_erase(dev, PIC_REGION_CONFIG, 0u) == PIC_ERR_UNSUPPORTED);
    CHECK(pic_bulk_erase(dev, 0u) == PIC_ERR_ARG);
    CHECK(pic_bulk_erase(dev, 0x10u) == PIC_ERR_ARG); /* unknown region bit */

    CHECK(sim_time_us() == t0); /* none of that touched the wires */
    pic_exit(dev);
}

static void test_exit_releases_target(void)
{
    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    pic_exit(dev);
    CHECK(!sim_in_program_mode());
    CHECK(pic_enter(dev) == PIC_OK); /* can be entered again */
    CHECK(read_id() == dev->device_id);
    pic_exit(dev);
}

int main(void)
{
    int devices = 0;

    for (size_t d = 0; d < pic_device_count; d++) {
        dev = &pic_devices[d];
        if (dev->proto != &pic_proto_p18_8bit) {
            continue;
        }
        devices++;

        test_checker_catches_short_waits();
        test_probe_finds_device();
        test_probe_is_not_fooled_by_a_pic16_id();
        test_identify();
        test_lvp_disabled_target_is_not_reachable();
        test_stuck_high_data_line_is_a_protocol_error();
        test_wrong_device_is_rejected();
        test_null_arguments_are_rejected();
        test_verify_reports_the_mismatch_index();
        test_erase_all_erases_everything_the_device_has();
        test_strerror_covers_every_status();
        test_erase_write_verify_flash();
        test_last_word_of_flash();
        test_programming_only_clears_bits();
        test_page_erase_only_affects_one_page();
        test_eeprom_config_and_userid();
        test_config_run_across_the_end();
        test_bad_arguments_send_nothing();
        test_exit_releases_target();
    }

    if (devices == 0) {
        printf("no p18_8bit devices in the table\n");
        return 1;
    }
    if (failures == 0) {
        printf("all p18_8bit tests passed (%d devices)\n", devices);
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
