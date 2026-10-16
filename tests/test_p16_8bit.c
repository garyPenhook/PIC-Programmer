/*
 * Host-side tests for the p16_8bit protocol (src/proto/p16_8bit.c), driven through the
 * device API (src/pic.c), against the simulated device in sim_p16_8bit.c. Every test runs
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
#include "sim_p16_8bit.h"

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

    fresh();
    const pic_device_t *found = NULL;
    CHECK(pic_probe(&found, &id, &rev) == PIC_OK);
    CHECK(found == dev);
    CHECK(id == dev->device_id);
    CHECK(rev == 0x2000u);
    CHECK(!sim_in_program_mode());
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_identify(void)
{
    pic_dci_t dci;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(sim_in_program_mode());
    CHECK(read_id() == dev->device_id);
    CHECK(pic_find_device(&pic_proto_p16_8bit, read_id()) == dev);
    CHECK(read_rev() == 0x2000u);

    /* The part's own DCI agrees with its table row. */
    CHECK(pic_read_dci(dev, &dci) == PIC_OK);
    CHECK(dci.erase_words == dev->erase_words);
    CHECK(dci.latch_words == dev->latch_words);
    CHECK((uint32_t)dci.user_rows * dci.erase_words == dev->flash_words);
    CHECK(dci.eeprom_bytes == dev->eeprom_bytes);

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

static void test_unknown_part_is_unsupported(void)
{
    const pic_device_t *found = dev;
    uint16_t id = 0u;

    fresh();
    sim_select_device(0x1234u); /* answers, but is not in the table */
    sim_reset();
    CHECK(pic_probe(&found, &id, NULL) == PIC_ERR_UNSUPPORTED);
    CHECK(found == NULL);
    CHECK(id == 0x1234u);
    CHECK(pic_enter(dev) == PIC_ERR_MISMATCH);
    sim_select_device(dev->device_id);
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

static void test_flash_erase_write_verify(void)
{
    uint16_t data[70]; /* two full rows and a partial third */
    uint16_t back[70];

    fresh();
    for (uint32_t i = 0; i < 70u; i++) {
        data[i] = (uint16_t)((0x0100u + 11u * i) & 0x3FFFu);
        sim_set_flash_word(0x20u + i, 0x1555u); /* old, non-blank contents */
    }

    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_bulk_erase(dev, PIC_ERASE_FLASH) == PIC_OK);
    CHECK(sim_flash_word(0x20) == 0x3FFFu);

    CHECK(pic_write(dev, PIC_REGION_FLASH, 0x20u, data, 70u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 0x20u, data, 70u, NULL) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_FLASH, 0x20u, back, 70u) == PIC_OK);
    CHECK(memcmp(data, back, sizeof data) == 0);
    for (uint32_t i = 0; i < 70u; i++) {
        CHECK(sim_flash_word(0x20u + i) == data[i]);
    }
    CHECK(sim_flash_word(0x1F) == 0x3FFFu);        /* neighbours untouched */
    CHECK(sim_flash_word(0x20u + 70u) == 0x3FFFu); /* the padded tail of the last row stays blank */
    CHECK(sim_flash_word(0x20u + 96u) == 0x3FFFu);
    pic_exit(dev);

    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_last_row_of_flash(void)
{
    uint16_t data[32];
    uint32_t last_row = dev->flash_words - 32u;

    fresh();
    for (uint32_t i = 0; i < 32u; i++)
        data[i] = (uint16_t)(0x2000u + i);

    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, last_row, data, 32u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_FLASH, last_row, data, 32u, NULL) == PIC_OK);
    CHECK(sim_flash_word(dev->flash_words - 1u) == 0x201Fu);
    pic_exit(dev);
}

static void test_programming_only_clears_bits(void)
{
    uint16_t first[32] = {0x0F0Fu};
    uint16_t second[32] = {0x30F0u};

    fresh();
    for (uint32_t i = 1; i < 32u; i++) {
        first[i] = 0x3FFFu;
        second[i] = 0x3FFFu;
    }

    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, first, 32u) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, second, 32u) == PIC_OK); /* no erase in between */
    CHECK(sim_flash_word(0) == (0x0F0Fu & 0x30F0u));                    /* old AND new */
    CHECK(pic_verify(dev, PIC_REGION_FLASH, 0u, second, 32u, NULL) == PIC_ERR_VERIFY);
    pic_exit(dev);
}

static void test_row_erase_affects_one_row(void)
{
    uint16_t row[32];

    fresh();
    for (uint32_t i = 0; i < 32u; i++)
        row[i] = 0x1234u;

    CHECK(pic_enter(dev) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0x00u, row, 32u) == PIC_OK);
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0x20u, row, 32u) == PIC_OK);
    CHECK(pic_row_erase(dev, PIC_REGION_FLASH, 0x05u) == PIC_OK); /* any address inside row 0 */
    CHECK(sim_flash_word(0) == 0x3FFFu);
    CHECK(sim_flash_word(31) == 0x3FFFu);
    CHECK(sim_flash_word(32) == 0x1234u); /* row 1 survives */
    pic_exit(dev);
    CHECK(sim_violations() == 0u);
}

static void test_userid_and_config_words(void)
{
    uint16_t id[4] = {0x1111u, 0x2222u, 0x0333u, 0x3444u};
    uint16_t cfg[5] = {0x3FF0u, 0x3FF1u, 0x3FF2u, 0x3FF3u, 0x3FF4u};
    uint16_t back[5];
    uint16_t flash_word = 0x0123u;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);

    CHECK(pic_write(dev, PIC_REGION_USERID, 0u, id, 4u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_USERID, 0u, id, 4u, NULL) == PIC_OK);
    CHECK(sim_userid_word(3) == 0x3444u);

    CHECK(pic_write(dev, PIC_REGION_CONFIG, 0u, cfg, 5u) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_CONFIG, 0u, back, 5u) == PIC_OK);
    CHECK(memcmp(cfg, back, sizeof cfg) == 0);
    CHECK(sim_config_word(0) == 0x3FF0u && sim_config_word(4) == 0x3FF4u);

    /* Row erase at the User ID address erases only the User IDs (spec 3.2.3). */
    CHECK(pic_write(dev, PIC_REGION_FLASH, 0u, &flash_word, 1u) == PIC_OK);
    CHECK(pic_row_erase(dev, PIC_REGION_USERID, 0u) == PIC_OK);
    CHECK(sim_userid_word(0) == 0x3FFFu);
    CHECK(sim_flash_word(0) == 0x0123u);  /* flash untouched */
    CHECK(sim_config_word(0) == 0x3FF0u); /* config untouched */

    CHECK(pic_bulk_erase(dev, PIC_ERASE_CONFIG) == PIC_OK);
    CHECK(sim_config_word(0) == 0x3FFFu);

    pic_exit(dev);
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_eeprom(void)
{
    uint16_t ee[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint16_t back[6];

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);

    CHECK(pic_write(dev, PIC_REGION_EEPROM, 0x08u, ee, 6u) == PIC_OK);
    CHECK(pic_verify(dev, PIC_REGION_EEPROM, 0x08u, ee, 6u, NULL) == PIC_OK);
    CHECK(pic_read(dev, PIC_REGION_EEPROM, 0x08u, back, 6u) == PIC_OK);
    CHECK(memcmp(ee, back, sizeof ee) == 0);
    CHECK(sim_eeprom_byte(8) == 0x11u && sim_eeprom_byte(13) == 0x66u);

    /* Last byte of this device's EEPROM. */
    CHECK(pic_write(dev, PIC_REGION_EEPROM, dev->eeprom_bytes - 1u, ee, 1u) == PIC_OK);
    CHECK(sim_eeprom_byte(dev->eeprom_bytes - 1u) == 0x11u);

    CHECK(pic_bulk_erase(dev, PIC_ERASE_EEPROM) == PIC_OK);
    CHECK(sim_eeprom_byte(8) == 0xFFu);

    pic_exit(dev);
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_bad_arguments_send_nothing(void)
{
    uint16_t w[33] = {0};
    uint64_t t0;

    fresh();
    CHECK(pic_enter(dev) == PIC_OK);
    t0 = sim_time_us();

    CHECK(pic_write(dev, PIC_REGION_FLASH, 1u, w, 1u) == PIC_ERR_ALIGN); /* not row aligned */
    CHECK(pic_write(dev, PIC_REGION_FLASH, dev->flash_words - 32u, w, 33u) ==
          PIC_ERR_RANGE); /* runs off the end */
    CHECK(pic_read(dev, PIC_REGION_FLASH, dev->flash_words, w, 1u) ==
          PIC_ERR_RANGE); /* past this device's flash */
    CHECK(pic_write(dev, PIC_REGION_USERID, 0u, w, 5u) == PIC_ERR_RANGE); /* past the 4 User IDs */
    CHECK(pic_write(dev, PIC_REGION_CONFIG, 0u, w, 6u) ==
          PIC_ERR_RANGE); /* past the 5 config words */
    CHECK(pic_write(dev, PIC_REGION_CONFIG, 5u, w, 1u) == PIC_ERR_RANGE);
    CHECK(pic_write(dev, PIC_REGION_EEPROM, dev->eeprom_bytes, w, 1u) ==
          PIC_ERR_RANGE); /* past the EEPROM */
    CHECK(pic_read(dev, PIC_REGION_EEPROM, 1u, w, 0xFFFFFFFFu) ==
          PIC_ERR_RANGE);                                            /* size overflow */
    CHECK(pic_read(dev, (pic_region_t)7, 0u, w, 1u) == PIC_ERR_ARG); /* no such region */
    CHECK(pic_row_erase(dev, PIC_REGION_CONFIG, 0u) ==
          PIC_ERR_UNSUPPORTED); /* config: bulk erase only */
    CHECK(pic_row_erase(dev, PIC_REGION_EEPROM, 0u) ==
          PIC_ERR_UNSUPPORTED); /* EEPROM: bulk erase only */
    CHECK(pic_row_erase(dev, PIC_REGION_FLASH, dev->flash_words) == PIC_ERR_RANGE);
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
        if (dev->proto != &pic_proto_p16_8bit) {
            continue;
        }
        devices++;

        test_checker_catches_short_waits();
        test_probe_finds_device();
        test_identify();
        test_lvp_disabled_target_is_not_reachable();
        test_stuck_high_data_line_is_a_protocol_error();
        test_unknown_part_is_unsupported();
        test_wrong_device_is_rejected();
        test_null_arguments_are_rejected();
        test_verify_reports_the_mismatch_index();
        test_erase_all_erases_everything_the_device_has();
        test_strerror_covers_every_status();
        test_flash_erase_write_verify();
        test_last_row_of_flash();
        test_programming_only_clears_bits();
        test_row_erase_affects_one_row();
        test_userid_and_config_words();
        test_eeprom();
        test_bad_arguments_send_nothing();
        test_exit_releases_target();
    }

    if (devices == 0) {
        printf("no p16_8bit devices in the table\n");
        return 1;
    }
    if (failures == 0) {
        printf("all p16_8bit tests passed (%d devices)\n", devices);
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
