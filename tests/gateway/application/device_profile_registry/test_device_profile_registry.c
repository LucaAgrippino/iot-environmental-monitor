/**
 * @file test_device_profile_registry.c
 * @brief Unit tests for DeviceProfileRegistry (Gateway).
 *
 * The storage callbacks are spies defined here; FreeRTOS comes from freertos_mock.
 * Test IDs follow the companion's test plan (device-profile-registry-lld.md §11).
 */

#include <string.h>

#include "device_profile_registry.h"
#include "freertos_mock.h"
#include "unity.h"

/* ---------------------------------------------------------------------- */
/* Storage spy                                                             */
/* ---------------------------------------------------------------------- */

static device_profile_t s_stored[DEVICE_PROFILE_MAX];
static uint8_t s_stored_count;
static bool s_load_ok;
static bool s_save_ok;
static uint32_t s_load_calls;
static uint32_t s_save_calls;
static void (*s_during_save)(void);

static bool spy_load(device_profile_t *out, uint8_t max, uint8_t *count_out)
{
    s_load_calls++;
    if (!s_load_ok)
    {
        return false;
    }
    (void) memcpy(out, s_stored, (size_t) max * sizeof(out[0]));
    *count_out = s_stored_count;
    return true;
}

static bool spy_save(const device_profile_t *in, uint8_t count)
{
    s_save_calls++;
    if (s_during_save != NULL)
    {
        s_during_save();
    }
    if (!s_save_ok)
    {
        return false;
    }
    (void) memset(s_stored, 0, sizeof(s_stored));
    (void) memcpy(s_stored, in, (size_t) count * sizeof(in[0]));
    s_stored_count = count;
    return true;
}

static const device_profile_storage_t k_storage = {.load = spy_load, .save = spy_save};

static device_profile_t make(uint8_t addr, const char *desc)
{
    device_profile_t p;

    (void) memset(&p, 0, sizeof(p));
    p.slave_addr = addr;
    p.map_version = 1U;
    p.vendor_code = 0x1A45U;
    (void) strncpy(p.description, desc, DEVICE_PROFILE_DESC_MAX - 1U);
    return p;
}

void setUp(void)
{
    mock_freertos_reset();
    device_profile_registry_reset_for_test();
    (void) memset(s_stored, 0, sizeof(s_stored));
    s_stored_count = 0U;
    s_load_ok = true;
    s_save_ok = true;
    s_load_calls = 0U;
    s_save_calls = 0U;
    s_during_save = NULL;
}

void tearDown(void)
{
}

static void start(void)
{
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_registry_init(&k_storage));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_registry_load());
}

/* ---------------------------------------------------------------------- */
/* Init and load                                                           */
/* ---------------------------------------------------------------------- */

void test_TC_DPR_001_init_rejects_missing_storage_and_is_idempotent(void)
{
    device_profile_storage_t no_save = {.load = spy_load, .save = NULL};
    device_profile_storage_t no_load = {.load = NULL, .save = spy_save};

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG, device_profile_registry_init(NULL));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
                          device_profile_registry_init(&no_save));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
                          device_profile_registry_init(&no_load));

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_registry_init(&k_storage));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_registry_init(&k_storage));
}

void test_TC_DPR_001b_init_reports_a_mutex_that_could_not_be_created(void)
{
    g_mock_xSemaphoreCreateMutexStatic_return = NULL;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NO_RESOURCE,
                          device_profile_registry_init(&k_storage));
}

void test_TC_DPR_002_everything_before_init_is_not_init(void)
{
    device_profile_t p = make(2U, "x");
    device_profile_t out;
    device_profile_t ptr;
    uint8_t index = 0U;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT, device_profile_registry_load());
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT,
                          device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT, device_profile_manager->remove(1U));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT,
                          device_profile_provider->get_by_addr(1U, &out));
    TEST_ASSERT_FALSE(device_profile_provider->get_next(&ptr, &index));
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(1U));
    TEST_ASSERT_EQUAL_UINT8(0U, device_profile_registry_count());
}

void test_TC_DPR_003_nothing_stored_seeds_the_default_without_saving(void)
{
    s_stored_count = 0U;
    start();

    device_profile_t out;

    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(1U, &out));
    TEST_ASSERT_EQUAL_UINT16(1U, out.map_version);
    TEST_ASSERT_EQUAL_UINT16(0x1A45U, out.vendor_code);
    TEST_ASSERT_EQUAL_STRING("Field Device 1 (default)", out.description);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
}

void test_TC_DPR_004_two_stored_profiles_are_loaded(void)
{
    s_stored[0] = make(3U, "alpha");
    s_stored[1] = make(9U, "beta");
    s_stored_count = 2U;
    start();

    device_profile_t out;

    TEST_ASSERT_EQUAL_UINT8(2U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(9U, &out));
    TEST_ASSERT_EQUAL_STRING("beta", out.description);
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(1U));
}

void test_TC_DPR_005_unreadable_storage_seeds_the_default(void)
{
    s_load_ok = false;
    start();
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(1U));
}

void test_TC_DPR_005b_a_stored_profile_that_fails_validation_seeds_the_default(void)
{
    s_stored[0] = make(3U, "alpha");
    s_stored[1] = make(0U, "bad address");
    s_stored_count = 2U;
    start();
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(1U));
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(3U));
}

void test_TC_DPR_005c_duplicate_stored_addresses_seed_the_default(void)
{
    s_stored[0] = make(3U, "alpha");
    s_stored[1] = make(3U, "again");
    s_stored_count = 2U;
    start();
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(1U));
}

/* ---------------------------------------------------------------------- */
/* add_or_update                                                           */
/* ---------------------------------------------------------------------- */

void test_TC_DPR_006_a_new_profile_is_added_and_saved(void)
{
    start();

    const device_profile_t p = make(5U, "second");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_UINT8(2U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT8(2U,
                            s_stored_count); /* the default is persisted with the first change */
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(5U));
}

void test_TC_DPR_007_an_existing_address_is_overwritten(void)
{
    start();

    device_profile_t p = make(1U, "renamed");

    p.map_version = 2U;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);

    device_profile_t out;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(1U, &out));
    TEST_ASSERT_EQUAL_STRING("renamed", out.description);
    TEST_ASSERT_EQUAL_UINT16(2U, out.map_version);
}

void test_TC_DPR_008_a_ninth_profile_is_refused_and_not_saved(void)
{
    start();
    for (uint8_t a = 2U; a <= DEVICE_PROFILE_MAX; a++)
    {
        const device_profile_t p = make(a, "fill");

        TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                              device_profile_manager->add_or_update(&p));
    }
    TEST_ASSERT_EQUAL_UINT8(DEVICE_PROFILE_MAX, device_profile_registry_count());

    const uint32_t saves = s_save_calls;
    const device_profile_t ninth = make(100U, "one too many");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_FULL,
                          device_profile_manager->add_or_update(&ninth));
    TEST_ASSERT_EQUAL_UINT8(DEVICE_PROFILE_MAX, device_profile_registry_count());
    TEST_ASSERT_EQUAL_UINT32(saves, s_save_calls);

    /* A full registry still accepts an update of an existing address. */
    const device_profile_t again = make(3U, "changed");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_manager->add_or_update(&again));
}

void test_TC_DPR_009_invalid_profiles_are_refused_and_not_saved(void)
{
    start();

    device_profile_t p = make(5U, "ok");
    device_profile_t bad;

    bad = p;
    bad.slave_addr = 0U;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    bad.slave_addr = 248U;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    bad.map_version = 0U;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    bad.vendor_code = 0U;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    bad.description[0] = '\0';
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    (void) memset(bad.description, 'x', sizeof(bad.description)); /* no terminator */
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    bad = p;
    bad.description[0] = '\n'; /* not printable */
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_INVALID,
                          device_profile_manager->add_or_update(&bad));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
                          device_profile_manager->add_or_update(NULL));

    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
}

void test_TC_DPR_010_a_failed_save_on_add_leaves_the_table_unchanged(void)
{
    start();
    s_save_ok = false;

    const device_profile_t p = make(5U, "never");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_PERSIST,
                          device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(5U));
}

void test_TC_DPR_011_a_failed_save_on_update_keeps_the_old_contents(void)
{
    start();
    s_save_ok = false;

    const device_profile_t p = make(1U, "changed");
    device_profile_t out;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_PERSIST,
                          device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(1U, &out));
    TEST_ASSERT_EQUAL_STRING("Field Device 1 (default)", out.description);
}

/* ---------------------------------------------------------------------- */
/* remove                                                                  */
/* ---------------------------------------------------------------------- */

void test_TC_DPR_012_a_failed_save_on_remove_keeps_the_entry(void)
{
    start();
    s_save_ok = false;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_PERSIST, device_profile_manager->remove(1U));
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(1U));
}

void test_TC_DPR_013_remove_compacts_and_saves_and_unknown_is_not_found(void)
{
    start();

    const device_profile_t a = make(5U, "a");
    const device_profile_t b = make(6U, "b");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&a));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&b));

    const uint32_t saves = s_save_calls;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->remove(1U));
    TEST_ASSERT_EQUAL_UINT8(2U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_UINT32(saves + 1U, s_save_calls);
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(1U));
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(5U));
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(6U));
    TEST_ASSERT_EQUAL_UINT8(2U, s_stored_count);

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND,
                          device_profile_manager->remove(77U));
    TEST_ASSERT_EQUAL_UINT32(saves + 1U, s_save_calls);
}

void test_TC_DPR_013b_removing_the_last_profile_leaves_an_empty_registry(void)
{
    start();
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->remove(1U));
    TEST_ASSERT_EQUAL_UINT8(0U, device_profile_registry_count());
    TEST_ASSERT_EQUAL_UINT8(0U, s_stored_count);

    /* An empty stored list means "nothing stored": the next boot seeds the default again. */
    device_profile_registry_reset_for_test();
    start();
    TEST_ASSERT_EQUAL_UINT8(1U, device_profile_registry_count());
}

/* ---------------------------------------------------------------------- */
/* Read side                                                               */
/* ---------------------------------------------------------------------- */

void test_TC_DPR_014_get_by_addr_found_and_not_found(void)
{
    start();

    device_profile_t out;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(1U, &out));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND,
                          device_profile_provider->get_by_addr(2U, &out));
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
                          device_profile_provider->get_by_addr(1U, NULL));
}

void test_TC_DPR_015_get_next_iterates_all_then_stops(void)
{
    start();
    for (uint8_t a = 2U; a <= 3U; a++)
    {
        const device_profile_t p = make(a, "n");

        TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                              device_profile_manager->add_or_update(&p));
    }

    device_profile_t p;
    uint8_t index = 0U;
    uint8_t seen = 0U;

    while (device_profile_provider->get_next(&p, &index))
    {
        TEST_ASSERT_NOT_EQUAL_UINT8(0U, p.slave_addr);
        seen++;
    }
    TEST_ASSERT_EQUAL_UINT8(3U, seen);
    TEST_ASSERT_EQUAL_UINT8(3U, index);

    TEST_ASSERT_FALSE(device_profile_provider->get_next(NULL, &index));
    TEST_ASSERT_FALSE(device_profile_provider->get_next(&p, NULL));
}

void test_TC_DPR_015b_the_free_functions_match_the_vtable(void)
{
    start();

    device_profile_t p;
    uint8_t index = 0U;

    TEST_ASSERT_TRUE(device_profile_get_next(&p, &index));
    TEST_ASSERT_EQUAL_UINT8(1U, p.slave_addr);
    TEST_ASSERT_FALSE(device_profile_get_next(&p, &index));
    TEST_ASSERT_TRUE(device_profile_is_registered(1U));
    TEST_ASSERT_FALSE(device_profile_is_registered(2U));
}

void test_TC_DPR_016_is_registered_true_and_false(void)
{
    start();
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(1U));
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(0U));
    TEST_ASSERT_FALSE(device_profile_provider->is_registered(247U));
}

/* ---------------------------------------------------------------------- */
/* Concurrency-visible behaviour                                           */
/* ---------------------------------------------------------------------- */

static bool s_reader_saw_old_table;

static void reader_during_save(void)
{
    device_profile_t out;

    s_reader_saw_old_table =
        (device_profile_provider->get_by_addr(1U, &out) == DEVICE_PROFILE_REGISTRY_OK) &&
        (strcmp(out.description, "Field Device 1 (default)") == 0) &&
        !device_profile_provider->is_registered(5U) && (device_profile_registry_count() == 1U);
}

void test_TC_DPR_017_a_reader_during_a_save_sees_the_old_table_even_when_it_fails(void)
{
    start();
    s_during_save = reader_during_save;
    s_reader_saw_old_table = false;
    s_save_ok = false;

    const device_profile_t p = make(5U, "x");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_ERR_PERSIST,
                          device_profile_manager->add_or_update(&p));
    TEST_ASSERT_TRUE(s_reader_saw_old_table);

    /* And during a save that succeeds, the reader still sees the old table; the new one appears
     * after. */
    s_reader_saw_old_table = false;
    s_save_ok = true;
    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    TEST_ASSERT_TRUE(s_reader_saw_old_table);
    TEST_ASSERT_TRUE(device_profile_provider->is_registered(5U));
}

void test_TC_DPR_018_the_description_is_copied_not_referenced(void)
{
    start();

    device_profile_t p = make(5U, "before");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    (void) strcpy(p.description, "after");

    device_profile_t out;

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK,
                          device_profile_provider->get_by_addr(5U, &out));
    TEST_ASSERT_EQUAL_STRING("before", out.description);
}

void test_TC_DPR_019_the_writer_mutex_is_taken_and_released_around_every_write(void)
{
    start();

    const uint32_t takes = g_mock_xSemaphoreTake_call_count;
    const uint32_t gives = g_mock_xSemaphoreGive_call_count;
    const device_profile_t p = make(5U, "m");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_UINT32(g_mock_xSemaphoreTake_call_count - takes,
                             g_mock_xSemaphoreGive_call_count - gives);
    TEST_ASSERT_TRUE((g_mock_xSemaphoreTake_call_count - takes) >= 3U);
}

void test_TC_DPR_020_get_next_returns_a_copy_that_later_writes_do_not_change(void)
{
    start();

    device_profile_t first;
    uint8_t index = 0U;

    TEST_ASSERT_TRUE(device_profile_provider->get_next(&first, &index));

    const device_profile_t p = make(1U, "replaced");

    TEST_ASSERT_EQUAL_INT(DEVICE_PROFILE_REGISTRY_OK, device_profile_manager->add_or_update(&p));
    TEST_ASSERT_EQUAL_STRING("Field Device 1 (default)", first.description);
}
