/*
 * Host-side tests for the PIC16F171XX driver (src/pic16f171.c) against the simulated
 * device in sim_pic16f171.c. Every test runs once for each first-target device:
 * PIC16F17124 and PIC16F17146. They check protocol behaviour and that every wait the
 * spec requires is respected. They do not replace a test on real silicon: the simulator
 * only knows what the driver's author understood from the specification.
 */
#include <stdio.h>
#include <string.h>

#include "icsp.h"
#include "pic16f171.h"
#include "sim_pic16f171.h"

static int failures;
static const char *current;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("  FAIL [%s] %s:%d: %s\n", current, __FILE__, __LINE__, #cond);   \
            failures++;                                                             \
        }                                                                           \
    } while (0)

typedef struct {
    const char *name;
    uint16_t id;
    uint32_t flash_words;
    uint32_t eeprom_bytes;
} device_t;

static const device_t devices[] = {
    { "PIC16F17124", 0x30DCu, 4096u, 128u },
    { "PIC16F17146", 0x30E1u, 16384u, 256u },
};

static device_t dev;

static void fresh(void)
{
    sim_set_lvp(1);
    sim_select_device(dev.id);
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
    pic16f171_dci_t dci;
    const char *name;

    fresh();
    pic16f171_enter();
    CHECK(sim_in_program_mode());
    CHECK(pic16f171_read_device_id() == dev.id);
    name = pic16f171_device_name(pic16f171_read_device_id());
    CHECK(name != NULL && strcmp(name, dev.name) == 0);
    CHECK(pic16f171_read_revision_id() == 0x2000u);

    pic16f171_read_dci(&dci);
    CHECK(dci.erase_row_words == 32u);
    CHECK(dci.write_latches == 32u);
    CHECK(pic16f171_flash_words(&dci) == dev.flash_words);
    CHECK(dci.eeprom_bytes == dev.eeprom_bytes);

    pic16f171_exit();
    CHECK(!sim_in_program_mode());
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_lvp_disabled_target_is_not_reachable(void)
{
    fresh();
    sim_set_lvp(0);
    pic16f171_enter();
    CHECK(!sim_in_program_mode());
    CHECK(pic16f171_read_device_id() == 0u);        /* nothing answers */
    CHECK(pic16f171_device_name(0u) == NULL);
    pic16f171_exit();
    sim_set_lvp(1);
}

static void test_flash_erase_write_verify(void)
{
    uint16_t data[70];                              /* two full rows and a partial third */
    uint16_t back[70];

    fresh();
    for (uint32_t i = 0; i < 70u; i++) {
        data[i] = (uint16_t)((0x0100u + 11u * i) & 0x3FFFu);
        sim_set_flash_word(0x20u + i, 0x1555u);     /* old, non-blank contents */
    }

    pic16f171_enter();
    CHECK(pic16f171_bulk_erase(PIC16F171_ERASE_FLASH) == PIC16F171_OK);
    CHECK(sim_flash_word(0x20) == 0x3FFFu);

    CHECK(pic16f171_write_words(0x20u, data, 70u) == PIC16F171_OK);
    CHECK(pic16f171_verify_words(0x20u, data, 70u) == PIC16F171_OK);
    CHECK(pic16f171_read_words(0x20u, back, 70u) == PIC16F171_OK);
    CHECK(memcmp(data, back, sizeof data) == 0);
    for (uint32_t i = 0; i < 70u; i++) {
        CHECK(sim_flash_word(0x20u + i) == data[i]);
    }
    CHECK(sim_flash_word(0x1F) == 0x3FFFu);         /* neighbours untouched */
    CHECK(sim_flash_word(0x20u + 70u) == 0x3FFFu);  /* the padded tail of the last row stays blank */
    CHECK(sim_flash_word(0x20u + 96u) == 0x3FFFu);
    pic16f171_exit();

    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_last_row_of_flash(void)
{
    uint16_t data[32];
    uint32_t last_row = dev.flash_words - 32u;

    fresh();
    for (uint32_t i = 0; i < 32u; i++) data[i] = (uint16_t)(0x2000u + i);

    pic16f171_enter();
    CHECK(pic16f171_write_words((uint16_t)last_row, data, 32u) == PIC16F171_OK);
    CHECK(pic16f171_verify_words((uint16_t)last_row, data, 32u) == PIC16F171_OK);
    CHECK(sim_flash_word(dev.flash_words - 1u) == 0x201Fu);
    pic16f171_exit();
}

static void test_programming_only_clears_bits(void)
{
    uint16_t first[32] = { 0x0F0Fu };
    uint16_t second[32] = { 0x30F0u };

    fresh();
    for (uint32_t i = 1; i < 32u; i++) { first[i] = 0x3FFFu; second[i] = 0x3FFFu; }

    pic16f171_enter();
    CHECK(pic16f171_write_words(0x0u, first, 32u) == PIC16F171_OK);
    CHECK(pic16f171_write_words(0x0u, second, 32u) == PIC16F171_OK);   /* no erase in between */
    CHECK(sim_flash_word(0) == (0x0F0Fu & 0x30F0u));                   /* old AND new */
    CHECK(pic16f171_verify_words(0x0u, second, 32u) == PIC16F171_ERR_VERIFY);
    pic16f171_exit();
}

static void test_row_erase_affects_one_row(void)
{
    uint16_t row[32];

    fresh();
    for (uint32_t i = 0; i < 32u; i++) row[i] = 0x1234u;

    pic16f171_enter();
    CHECK(pic16f171_write_words(0x00u, row, 32u) == PIC16F171_OK);
    CHECK(pic16f171_write_words(0x20u, row, 32u) == PIC16F171_OK);
    CHECK(pic16f171_row_erase(0x05u) == PIC16F171_OK);        /* any address inside row 0 */
    CHECK(sim_flash_word(0) == 0x3FFFu);
    CHECK(sim_flash_word(31) == 0x3FFFu);
    CHECK(sim_flash_word(32) == 0x1234u);                     /* row 1 survives */
    pic16f171_exit();
    CHECK(sim_violations() == 0u);
}

static void test_userid_and_config_words(void)
{
    uint16_t id[4] = { 0x1111u, 0x2222u, 0x0333u, 0x3444u };
    uint16_t cfg[5] = { 0x3FF0u, 0x3FF1u, 0x3FF2u, 0x3FF3u, 0x3FF4u };
    uint16_t back[5];
    uint16_t flash_word = 0x0123u;

    fresh();
    pic16f171_enter();

    CHECK(pic16f171_write_words(PIC16F171_ADDR_USERID, id, 4u) == PIC16F171_OK);
    CHECK(pic16f171_verify_words(PIC16F171_ADDR_USERID, id, 4u) == PIC16F171_OK);
    CHECK(sim_userid_word(3) == 0x3444u);

    CHECK(pic16f171_write_words(PIC16F171_ADDR_CONFIG, cfg, 5u) == PIC16F171_OK);
    CHECK(pic16f171_read_words(PIC16F171_ADDR_CONFIG, back, 5u) == PIC16F171_OK);
    CHECK(memcmp(cfg, back, sizeof cfg) == 0);
    CHECK(sim_config_word(0) == 0x3FF0u && sim_config_word(4) == 0x3FF4u);

    /* Row erase at the User ID address erases only the User IDs (spec 3.2.3). */
    CHECK(pic16f171_write_words(0x0u, &flash_word, 1u) == PIC16F171_OK);
    CHECK(pic16f171_row_erase(PIC16F171_ADDR_USERID) == PIC16F171_OK);
    CHECK(sim_userid_word(0) == 0x3FFFu);
    CHECK(sim_flash_word(0) == 0x0123u);                      /* flash untouched */
    CHECK(sim_config_word(0) == 0x3FF0u);                     /* config untouched */

    CHECK(pic16f171_bulk_erase(PIC16F171_ERASE_CONFIG) == PIC16F171_OK);
    CHECK(sim_config_word(0) == 0x3FFFu);

    pic16f171_exit();
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_eeprom(void)
{
    uint8_t ee[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    uint8_t back[6];

    fresh();
    pic16f171_enter();

    CHECK(pic16f171_write_eeprom(PIC16F171_ADDR_EEPROM + 0x08u, ee, 6u) == PIC16F171_OK);
    CHECK(pic16f171_verify_eeprom(PIC16F171_ADDR_EEPROM + 0x08u, ee, 6u) == PIC16F171_OK);
    CHECK(pic16f171_read_eeprom(PIC16F171_ADDR_EEPROM + 0x08u, back, 6u) == PIC16F171_OK);
    CHECK(memcmp(ee, back, 6u) == 0);
    CHECK(sim_eeprom_byte(8) == 0x11u && sim_eeprom_byte(13) == 0x66u);

    /* Last byte of this device's EEPROM. */
    CHECK(pic16f171_write_eeprom((uint16_t)(PIC16F171_ADDR_EEPROM + dev.eeprom_bytes - 1u), ee, 1u) == PIC16F171_OK);
    CHECK(sim_eeprom_byte(dev.eeprom_bytes - 1u) == 0x11u);

    CHECK(pic16f171_bulk_erase(PIC16F171_ERASE_EEPROM) == PIC16F171_OK);
    CHECK(sim_eeprom_byte(8) == 0xFFu);

    pic16f171_exit();
    CHECK(sim_violations() == 0u);
    CHECK(sim_bad_commands() == 0u);
}

static void test_flash_beyond_device_reads_zero(void)
{
    /* Spec Table 2-1 note 2: flash that is not present reads as 0. The driver reads the
     * size from the DCI, which is how a host tool knows where the device really ends. */
    uint16_t w = 0xFFFFu;

    fresh();
    pic16f171_enter();
    if (dev.flash_words < 0x4000u) {
        CHECK(pic16f171_read_words((uint16_t)dev.flash_words, &w, 1u) == PIC16F171_OK);
        CHECK(w == 0u);
    } else {
        CHECK(pic16f171_read_words(0x3FFFu, &w, 1u) == PIC16F171_OK);
        CHECK(w == 0x3FFFu);
    }
    pic16f171_exit();
}

static void test_bad_arguments_send_nothing(void)
{
    uint16_t w[33] = { 0 };
    uint8_t b[300] = { 0 };
    uint64_t t0;

    fresh();
    pic16f171_enter();
    t0 = sim_time_us();

    CHECK(pic16f171_write_words(0x0001u, w, 1u) == PIC16F171_ERR_ARG);                      /* flash not row aligned */
    CHECK(pic16f171_write_words(0x7FE0u, w, 33u) == PIC16F171_ERR_ARG);                     /* runs off the end of flash */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_DEVID, w, 1u) == PIC16F171_ERR_ARG);         /* read-only */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_REVID, w, 1u) == PIC16F171_ERR_ARG);         /* read-only */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_DCI, w, 1u) == PIC16F171_ERR_ARG);           /* read-only */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_USERID, w, 5u) == PIC16F171_ERR_ARG);        /* past the 4 User IDs */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_CONFIG, w, 6u) == PIC16F171_ERR_ARG);        /* past the 5 config words */
    CHECK(pic16f171_write_words(PIC16F171_ADDR_EEPROM, w, 1u) == PIC16F171_ERR_ARG);        /* EEPROM uses the bytes call */
    CHECK(pic16f171_write_eeprom(0x0000u, b, 1u) == PIC16F171_ERR_ARG);                     /* flash is not EEPROM */
    CHECK(pic16f171_write_eeprom(PIC16F171_ADDR_EEPROM, b, 257u) == PIC16F171_ERR_ARG);     /* past 256 bytes */
    CHECK(pic16f171_read_words(0x7FFFu, w, 2u) == PIC16F171_ERR_ARG);                       /* crosses into config area */
    CHECK(pic16f171_read_words(0x9000u, w, 1u) == PIC16F171_ERR_ARG);                       /* unmapped */
    CHECK(pic16f171_row_erase(PIC16F171_ADDR_CONFIG) == PIC16F171_ERR_ARG);                 /* config: bulk erase only */
    CHECK(pic16f171_row_erase(PIC16F171_ADDR_EEPROM) == PIC16F171_ERR_ARG);                 /* EEPROM: bulk erase only */
    CHECK(pic16f171_bulk_erase(0u) == PIC16F171_ERR_ARG);
    CHECK(pic16f171_bulk_erase(0x10u) == PIC16F171_ERR_ARG);                                /* unknown region bit */

    CHECK(sim_time_us() == t0);                  /* none of that touched the wires */
    pic16f171_exit();
}

static void test_exit_releases_target(void)
{
    fresh();
    pic16f171_enter();
    pic16f171_exit();
    CHECK(!sim_in_program_mode());
    pic16f171_enter();                           /* can be entered again */
    CHECK(pic16f171_read_device_id() == dev.id);
    pic16f171_exit();
}

int main(void)
{
    for (size_t d = 0; d < sizeof devices / sizeof devices[0]; d++) {
        dev = devices[d];
        current = dev.name;

        test_checker_catches_short_waits();
        test_identify();
        test_lvp_disabled_target_is_not_reachable();
        test_flash_erase_write_verify();
        test_last_row_of_flash();
        test_programming_only_clears_bits();
        test_row_erase_affects_one_row();
        test_userid_and_config_words();
        test_eeprom();
        test_flash_beyond_device_reads_zero();
        test_bad_arguments_send_nothing();
        test_exit_releases_target();
    }

    if (failures == 0) {
        printf("all PIC16F171XX driver tests passed (PIC16F17124, PIC16F17146)\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
