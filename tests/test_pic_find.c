/* Unity tests for the pure device-table lookup in src/pic.c. */
#include "pic.h"
#include "pic_proto.h"
#include "unity.h"

void setUp(void)
{
}
void tearDown(void)
{
}

static void test_every_row_is_found_by_its_own_key(void)
{
    for (size_t i = 0; i < pic_device_count; i++) {
        const pic_device_t *d = &pic_devices[i];
        TEST_ASSERT_EQUAL_PTR(d, pic_find_device(d->proto, d->device_id));
    }
}

static void test_unknown_id_returns_null(void)
{
    TEST_ASSERT_NOT_NULL(pic_devices[0].proto);
    TEST_ASSERT_NULL(pic_find_device(pic_devices[0].proto, 0xFFFFu));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_row_is_found_by_its_own_key);
    RUN_TEST(test_unknown_id_returns_null);
    return UNITY_END();
}
