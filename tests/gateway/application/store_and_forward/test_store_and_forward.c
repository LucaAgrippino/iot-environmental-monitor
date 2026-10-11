/**
 * @file test_store_and_forward.c
 * @brief Unit tests for StoreAndForward (Gateway application layer).
 *
 * CircularFlashLog is replaced by a small in-test FIFO that mirrors the real
 * module's observable behaviour — CFL_ERR_FULL at capacity, rejection of NULL
 * out-parameters, a CRC-corrupt flag per record — with per-call fault
 * injection. No CMock is needed. The test never includes circular_flash_log.h
 * directly, so Ceedling does not link the real circular_flash_log.c.
 *
 * Test IDs follow docs/lld/application/store-and-forward-lld.md §12.2. IDs
 * ending in a letter beyond the LLD's own (T26 onward) cover guards added
 * during implementation.
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "logger_stub.h"
#include "store_and_forward.h"

/* ---------------------------------------------------------------------- */
/* Logger and IHealthReport spies                                          */
/* ---------------------------------------------------------------------- */

void logger_log(log_level_t level, const char *module, const char *msg)
{
    (void) level;
    (void) module;
    (void) msg;
}

static uint32_t s_occ_calls;
static uint32_t s_occ_last_count;
static uint32_t s_occ_last_capacity;
static uint8_t s_occ_last_percent;

void health_update_buffer_occupancy(health_monitor_handle_t handle, uint32_t count,
                                    uint32_t capacity, uint8_t percent)
{
    (void) handle;
    s_occ_calls++;
    s_occ_last_count = count;
    s_occ_last_capacity = capacity;
    s_occ_last_percent = percent;
}

/* ---------------------------------------------------------------------- */
/* Fake CircularFlashLog                                                   */
/* ---------------------------------------------------------------------- */

#define FAKE_MAX_RECORDS (16U)

typedef struct
{
    uint8_t data[CFL_MAX_RECORD_BYTES];
    uint32_t len;
    bool corrupt;
} fake_rec_t;

static fake_rec_t s_ring[FAKE_MAX_RECORDS];
static uint32_t s_head;
static uint32_t s_count;
static uint32_t s_ring_capacity;

static cfl_err_t s_append_err;
static cfl_err_t s_drop_err;
static cfl_err_t s_peek_err;
static cfl_err_t s_consume_err;
static cfl_err_t s_occupancy_err;

static uint32_t s_append_calls;
static uint32_t s_peek_calls;
static uint32_t s_consume_calls;
static uint32_t s_drop_calls;
static uint32_t s_occupancy_calls;
static uint32_t s_last_append_len;
static uint8_t s_last_append[CFL_MAX_RECORD_BYTES];

static void fake_reset(void)
{
    (void) memset(s_ring, 0, sizeof(s_ring));
    s_head = 0U;
    s_count = 0U;
    s_ring_capacity = FAKE_MAX_RECORDS;
    s_append_err = CFL_ERR_OK;
    s_drop_err = CFL_ERR_OK;
    s_peek_err = CFL_ERR_OK;
    s_consume_err = CFL_ERR_OK;
    s_occupancy_err = CFL_ERR_OK;
    s_append_calls = 0U;
    s_peek_calls = 0U;
    s_consume_calls = 0U;
    s_drop_calls = 0U;
    s_occupancy_calls = 0U;
    s_last_append_len = 0U;
    (void) memset(s_last_append, 0, sizeof(s_last_append));
    s_occ_calls = 0U;
    s_occ_last_count = 0U;
    s_occ_last_capacity = 0U;
    s_occ_last_percent = 0U;
}

static void fake_pop(void)
{
    s_head = (s_head + 1U) % FAKE_MAX_RECORDS;
    s_count--;
}

cfl_err_t cfl_get_occupancy(cfl_handle_t handle, uint32_t *entry_count, uint32_t *byte_count)
{
    (void) handle;
    s_occupancy_calls++;

    if (s_occupancy_err != CFL_ERR_OK)
    {
        return s_occupancy_err;
    }

    /* The real module rejects a NULL out-parameter; so does the fake. */
    if ((entry_count == NULL) || (byte_count == NULL))
    {
        return CFL_ERR_NULL_PTR;
    }

    *entry_count = s_count;
    *byte_count = 0U;

    return CFL_ERR_OK;
}

cfl_err_t cfl_append(cfl_handle_t handle, const uint8_t *data, uint32_t len)
{
    (void) handle;
    s_append_calls++;

    if (s_append_err != CFL_ERR_OK)
    {
        return s_append_err;
    }

    if (s_count >= s_ring_capacity)
    {
        return CFL_ERR_FULL;
    }

    uint32_t slot = (s_head + s_count) % FAKE_MAX_RECORDS;

    (void) memcpy(s_ring[slot].data, data, len);
    s_ring[slot].len = len;
    s_ring[slot].corrupt = false;
    s_count++;

    s_last_append_len = len;
    (void) memcpy(s_last_append, data, len);

    return CFL_ERR_OK;
}

cfl_err_t cfl_peek_oldest(cfl_handle_t handle, uint8_t *data_out, uint32_t *len_out,
                          uint32_t max_len)
{
    (void) handle;
    (void) max_len;
    s_peek_calls++;

    if (s_peek_err != CFL_ERR_OK)
    {
        return s_peek_err;
    }

    if (s_count == 0U)
    {
        return CFL_ERR_EMPTY;
    }

    if (s_ring[s_head].corrupt)
    {
        return CFL_ERR_CORRUPT;
    }

    (void) memcpy(data_out, s_ring[s_head].data, s_ring[s_head].len);
    *len_out = s_ring[s_head].len;

    return CFL_ERR_OK;
}

cfl_err_t cfl_consume_oldest(cfl_handle_t handle)
{
    (void) handle;
    s_consume_calls++;

    if (s_consume_err != CFL_ERR_OK)
    {
        return s_consume_err;
    }

    if (s_count == 0U)
    {
        return CFL_ERR_EMPTY;
    }

    fake_pop();

    return CFL_ERR_OK;
}

cfl_err_t cfl_drop_oldest(cfl_handle_t handle)
{
    (void) handle;
    s_drop_calls++;

    if (s_drop_err != CFL_ERR_OK)
    {
        return s_drop_err;
    }

    if (s_count == 0U)
    {
        return CFL_ERR_EMPTY;
    }

    fake_pop();

    return CFL_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* Fixture                                                                 */
/* ---------------------------------------------------------------------- */

#define TEST_CAPACITY (100U)

static saf_handle_t s_h;
static saf_config_t s_cfg;
static uint8_t s_data[SAF_PAYLOAD_MAX];

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) i);
    }
}

void setUp(void)
{
    saf_reset_for_test();
    fake_reset();

    s_h = NULL;
    s_cfg.cfl = (cfl_handle_t) 1;
    s_cfg.health = (health_monitor_handle_t) 1;
    s_cfg.capacity = TEST_CAPACITY;
    fill_pattern(s_data, sizeof(s_data), 0x11U);
}

void tearDown(void)
{
}

/** @brief Create the instance under test, failing the test if it cannot. */
static void create_ok(void)
{
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_create(&s_cfg, &s_h));
    TEST_ASSERT_NOT_NULL(s_h);
}

/** @brief Dequeue the oldest entry and check it against a pattern. */
static void dequeue_expect(const char *topic, uint8_t qos, uint16_t len, uint8_t seed)
{
    const saf_entry_t *e = NULL;
    uint8_t expected[SAF_PAYLOAD_MAX];

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING(topic, e->topic);
    TEST_ASSERT_EQUAL_UINT8(qos, e->qos);
    TEST_ASSERT_EQUAL_UINT16(len, e->payload_len);

    fill_pattern(expected, len, seed);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, e->payload, len);
}

/* ---------------------------------------------------------------------- */
/* Create / lifecycle                                                      */
/* ---------------------------------------------------------------------- */

void test_SAF_T01_create_reads_occupancy_and_pushes_it(void)
{
    create_ok();

    TEST_ASSERT_EQUAL_UINT32(1U, s_occupancy_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_occ_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_occ_last_count);
    TEST_ASSERT_EQUAL_UINT32(TEST_CAPACITY, s_occ_last_capacity);
}

void test_SAF_T02_create_rejects_null_arguments(void)
{
    saf_handle_t untouched = (saf_handle_t) 0x1234;
    saf_config_t no_cfl = s_cfg;

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_create(NULL, &untouched));
    TEST_ASSERT_EQUAL_PTR(0x1234, untouched);

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_create(&s_cfg, NULL));

    no_cfl.cfl = NULL;
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_create(&no_cfl, &s_h));
    TEST_ASSERT_EQUAL_UINT32(0U, s_occupancy_calls);
}

void test_SAF_T03_second_create_exhausts_the_pool(void)
{
    saf_handle_t second = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_NO_RESOURCE, saf_create(&s_cfg, &second));
    TEST_ASSERT_NULL(second);
}

void test_SAF_T04_every_operation_rejects_a_stale_handle(void)
{
    const saf_entry_t *e = NULL;
    uint32_t a = 0U;
    uint32_t b = 0U;
    bool empty = false;

    create_ok();
    saf_handle_t stale = s_h;

    saf_reset_for_test();

    TEST_ASSERT_EQUAL(SAF_ERR_NOT_INIT, saf_enqueue(stale, "t", s_data, 4U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_NOT_INIT, saf_dequeue(stale, &e));
    TEST_ASSERT_EQUAL(SAF_ERR_NOT_INIT, saf_confirm(stale));
    TEST_ASSERT_EQUAL(SAF_ERR_NOT_INIT, saf_is_empty(stale, &empty));
    TEST_ASSERT_EQUAL(SAF_ERR_NOT_INIT, saf_get_occupancy(stale, &a, &b));
}

void test_SAF_T26_create_failure_does_not_consume_the_pool(void)
{
    saf_handle_t first = NULL;

    s_occupancy_err = CFL_ERR_FLASH_READ;
    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_create(&s_cfg, &first));
    TEST_ASSERT_NULL(first);

    s_occupancy_err = CFL_ERR_OK;
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_create(&s_cfg, &first));
}

/* ---------------------------------------------------------------------- */
/* Enqueue — happy path                                                    */
/* ---------------------------------------------------------------------- */

void test_SAF_T05_enqueue_serialises_per_the_flash_format(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "sensors/temp", s_data, 64U, 1U));

    /* topic_len(1) + topic(12) + qos(1) + payload_len(2) + payload(64) */
    TEST_ASSERT_EQUAL_UINT32(1U + 12U + 1U + 2U + 64U, s_last_append_len);
    TEST_ASSERT_EQUAL_UINT8(12U, s_last_append[0]);
    TEST_ASSERT_EQUAL_MEMORY("sensors/temp", &s_last_append[1], 12U);
    TEST_ASSERT_EQUAL_UINT8(1U, s_last_append[13]);
    TEST_ASSERT_EQUAL_UINT8(64U, s_last_append[14]);
    TEST_ASSERT_EQUAL_UINT8(0U, s_last_append[15]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_data, &s_last_append[16], 64U);

    TEST_ASSERT_EQUAL_UINT32(2U, s_occ_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_occ_last_count);
}

void test_SAF_T06_entries_drain_in_fifo_order(void)
{
    uint8_t buf[8];

    create_ok();

    for (uint8_t i = 0U; i < 3U; i++)
    {
        fill_pattern(buf, sizeof(buf), (uint8_t) (0x10U * (i + 1U)));
        TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/fifo", buf, sizeof(buf), 0U));
    }

    for (uint8_t i = 0U; i < 3U; i++)
    {
        dequeue_expect("t/fifo", 0U, 8U, (uint8_t) (0x10U * (i + 1U)));
        TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    }

    const saf_entry_t *e = NULL;

    TEST_ASSERT_EQUAL(SAF_ERR_EMPTY, saf_dequeue(s_h, &e));
}

void test_SAF_T07_variable_length_topics_round_trip(void)
{
    const char *long_topic = "devices/gateway-01/telemetry/environment/temperature";

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "a/bcd", s_data, 10U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, long_topic, s_data, 20U, 1U));

    dequeue_expect("a/bcd", 0U, 10U, 0x11U);
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    dequeue_expect(long_topic, 1U, 20U, 0x11U);
}

void test_SAF_T07b_maximum_entry_fills_exactly_one_cfl_record(void)
{
    char topic[SAF_TOPIC_MAX + 1U];

    (void) memset(topic, 'x', SAF_TOPIC_MAX);
    topic[SAF_TOPIC_MAX] = '\0';

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, topic, s_data, SAF_PAYLOAD_MAX, 1U));
    TEST_ASSERT_EQUAL_UINT32(CFL_MAX_RECORD_BYTES, s_last_append_len);

    dequeue_expect(topic, 1U, SAF_PAYLOAD_MAX, 0x11U);
}

void test_SAF_T27_derived_limits_match_the_companion(void)
{
    TEST_ASSERT_EQUAL_UINT32(4010U, SAF_PAYLOAD_MAX);
    TEST_ASSERT_EQUAL_UINT32(68U, SAF_HEADER_OVERHEAD_MAX);
}

/* ---------------------------------------------------------------------- */
/* Enqueue — error / boundary                                              */
/* ---------------------------------------------------------------------- */

void test_SAF_T08_full_ring_drops_oldest_then_appends(void)
{
    create_ok();
    s_ring_capacity = 1U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/old", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/new", s_data, 8U, 0U));

    TEST_ASSERT_EQUAL_UINT32(1U, s_drop_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_occ_last_count);
    dequeue_expect("t/new", 0U, 8U, 0x11U);
}

void test_SAF_T09_oversized_payload_is_rejected_before_flash(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_TOO_LARGE, saf_enqueue(s_h, "t", s_data, SAF_PAYLOAD_MAX + 1U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_append_calls);
}

void test_SAF_T09b_zero_length_payload_is_rejected(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_TOO_LARGE, saf_enqueue(s_h, "t", s_data, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_append_calls);
}

void test_SAF_T10_append_failure_leaves_the_count_alone(void)
{
    create_ok();
    s_append_err = CFL_ERR_FLASH_WRITE;

    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_enqueue(s_h, "t", s_data, 8U, 0U));

    uint32_t count = 99U;
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_get_occupancy(s_h, &count, &cap));
    TEST_ASSERT_EQUAL_UINT32(0U, count);
}

void test_SAF_T11_drop_failure_aborts_the_enqueue(void)
{
    create_ok();
    s_ring_capacity = 1U;
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));

    s_drop_err = CFL_ERR_FLASH_READ;
    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_enqueue(s_h, "t", s_data, 8U, 0U));

    uint32_t count = 0U;
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_get_occupancy(s_h, &count, &cap));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
}

void test_SAF_T12_enqueue_rejects_null_arguments(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_enqueue(s_h, "t", NULL, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_enqueue(s_h, NULL, s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_enqueue(NULL, "t", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_append_calls);
}

void test_SAF_T28_empty_or_overlong_topic_is_rejected(void)
{
    char topic[SAF_TOPIC_MAX + 2U];

    (void) memset(topic, 'x', SAF_TOPIC_MAX + 1U);
    topic[SAF_TOPIC_MAX + 1U] = '\0';

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_TOO_LARGE, saf_enqueue(s_h, "", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_TOO_LARGE, saf_enqueue(s_h, topic, s_data, 8U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_append_calls);
}

void test_SAF_T29_drop_loop_ends_when_ring_empties(void)
{
    create_ok();
    s_ring_capacity = 0U;

    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_enqueue(s_h, "t", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_drop_calls);
}

/* ---------------------------------------------------------------------- */
/* Dequeue / confirm                                                       */
/* ---------------------------------------------------------------------- */

void test_SAF_T13_dequeue_on_empty_buffer(void)
{
    const saf_entry_t *e = NULL;

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_EMPTY, saf_dequeue(s_h, &e));
}

void test_SAF_T14_dequeue_returns_the_stored_entry(void)
{
    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "alarms/high", s_data, 33U, 1U));

    dequeue_expect("alarms/high", 1U, 33U, 0x11U);
}

void test_SAF_T15_second_dequeue_returns_the_pending_entry(void)
{
    const saf_entry_t *first = NULL;
    const saf_entry_t *second = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &first));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &second));

    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_UINT32(1U, s_peek_calls);
}

void test_SAF_T16_confirm_consumes_and_reports(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));

    uint32_t calls_before = s_occ_calls;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));

    TEST_ASSERT_EQUAL_UINT32(1U, s_consume_calls);
    TEST_ASSERT_EQUAL_UINT32(calls_before + 1U, s_occ_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_occ_last_count);
}

void test_SAF_T17_confirm_without_a_dequeue_is_a_no_op(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    TEST_ASSERT_EQUAL_UINT32(0U, s_consume_calls);
}

void test_SAF_T18_peek_failure_is_reported(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));
    s_peek_err = CFL_ERR_FLASH_READ;

    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_dequeue(s_h, &e));
    TEST_ASSERT_EQUAL_UINT32(0U, s_consume_calls);
}

void test_SAF_T18b_corrupt_record_is_skipped(void)
{
    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/bad", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/good", s_data, 9U, 0U));
    s_ring[s_head].corrupt = true;

    dequeue_expect("t/good", 0U, 9U, 0x11U);

    uint32_t count = 0U;
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL_UINT32(1U, s_consume_calls);
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_get_occupancy(s_h, &count, &cap));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
}

void test_SAF_T19_confirm_failure_keeps_the_entry_pending(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));

    s_consume_err = CFL_ERR_FLASH_WRITE;
    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_confirm(s_h));

    /* Still pending: a retry consumes it. */
    s_consume_err = CFL_ERR_OK;
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    TEST_ASSERT_EQUAL_UINT32(2U, s_consume_calls);
}

void test_SAF_T30_skip_that_cannot_consume_does_not_loop(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 8U, 0U));
    s_ring[s_head].corrupt = true;
    s_consume_err = CFL_ERR_FLASH_ERASE;

    TEST_ASSERT_EQUAL(SAF_ERR_FLASH, saf_dequeue(s_h, &e));
    TEST_ASSERT_EQUAL_UINT32(1U, s_consume_calls);
}

void test_SAF_T31_well_formed_crc_but_malformed_blob_is_skipped(void)
{
    create_ok();

    /* A blob whose topic_len claims more bytes than the record holds. */
    s_ring[0].data[0] = 60U;
    s_ring[0].len = 6U;
    s_ring[0].corrupt = false;
    s_count = 1U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/ok", s_data, 5U, 0U));

    dequeue_expect("t/ok", 0U, 5U, 0x11U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_consume_calls);
}

void test_SAF_T32_dequeue_rejects_null_arguments(void)
{
    const saf_entry_t *e = NULL;

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_dequeue(NULL, &e));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_dequeue(s_h, NULL));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_confirm(NULL));
}

void test_SAF_T33_dropping_the_pending_entry_forgets_it(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    s_ring_capacity = 2U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/one", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/two", s_data, 8U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));
    TEST_ASSERT_EQUAL_STRING("t/one", e->topic);

    /* Full: enqueueing drops t/one, the entry the caller is holding. */
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/three", s_data, 8U, 0U));

    /* A confirm now must not consume t/two, which was never published. */
    uint32_t consumed_before = s_consume_calls;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    TEST_ASSERT_EQUAL_UINT32(consumed_before, s_consume_calls);

    dequeue_expect("t/two", 0U, 8U, 0x11U);
}

/* ---------------------------------------------------------------------- */
/* Power-loss / boot recovery                                              */
/* ---------------------------------------------------------------------- */

void test_SAF_T20_create_recovers_the_entry_count(void)
{
    for (uint32_t i = 0U; i < 5U; i++)
    {
        s_count++;
    }

    create_ok();

    uint32_t count = 0U;
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_get_occupancy(s_h, &count, &cap));
    TEST_ASSERT_EQUAL_UINT32(5U, count);
    TEST_ASSERT_EQUAL_UINT32(TEST_CAPACITY, cap);
    TEST_ASSERT_EQUAL_UINT32(5U, s_occ_last_count);
}

void test_SAF_T21_unconfirmed_entry_is_republished_after_reset(void)
{
    const saf_entry_t *e = NULL;

    create_ok();
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t/replay", s_data, 12U, 1U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));

    /* Power loss: RAM state is gone, the fake ring (flash) survives. */
    saf_reset_for_test();
    create_ok();

    uint32_t count = 0U;
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_get_occupancy(s_h, &count, &cap));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    dequeue_expect("t/replay", 1U, 12U, 0x11U);
}

/* ---------------------------------------------------------------------- */
/* Occupancy                                                               */
/* ---------------------------------------------------------------------- */

void test_SAF_T22_initial_occupancy_push_is_zero_percent(void)
{
    create_ok();

    TEST_ASSERT_EQUAL_UINT32(0U, s_occ_last_count);
    TEST_ASSERT_EQUAL_UINT32(TEST_CAPACITY, s_occ_last_capacity);
    TEST_ASSERT_EQUAL_UINT8(0U, s_occ_last_percent);
}

void test_SAF_T23_percentage_tracks_the_fill(void)
{
    s_cfg.capacity = 10U;
    create_ok();

    for (uint32_t i = 0U; i < 5U; i++)
    {
        TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 4U, 0U));
    }

    TEST_ASSERT_EQUAL_UINT32(5U, s_occ_last_count);
    TEST_ASSERT_EQUAL_UINT8(50U, s_occ_last_percent);
}

void test_SAF_T24_drop_and_append_keeps_the_count_at_capacity(void)
{
    s_cfg.capacity = 4U;
    create_ok();
    s_ring_capacity = 4U;

    for (uint32_t i = 0U; i < 5U; i++)
    {
        TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 4U, 0U));
    }

    TEST_ASSERT_EQUAL_UINT32(4U, s_occ_last_count);
    TEST_ASSERT_EQUAL_UINT8(100U, s_occ_last_percent);
    TEST_ASSERT_EQUAL_UINT32(1U, s_drop_calls);
}

void test_SAF_T25_get_occupancy_rejects_null_arguments(void)
{
    uint32_t value = 0U;

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_get_occupancy(s_h, NULL, &value));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_get_occupancy(s_h, &value, NULL));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_get_occupancy(NULL, &value, &value));
}

void test_SAF_T34_is_empty_follows_enqueue_and_confirm(void)
{
    const saf_entry_t *e = NULL;
    bool empty = false;

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_is_empty(s_h, &empty));
    TEST_ASSERT_TRUE(empty);

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 4U, 0U));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_is_empty(s_h, &empty));
    TEST_ASSERT_FALSE(empty);

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_dequeue(s_h, &e));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_is_empty(s_h, &empty));
    TEST_ASSERT_FALSE(empty);

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_confirm(s_h));
    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_is_empty(s_h, &empty));
    TEST_ASSERT_TRUE(empty);
}

void test_SAF_T35_is_empty_rejects_null_arguments(void)
{
    bool empty = false;

    create_ok();

    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_is_empty(s_h, NULL));
    TEST_ASSERT_EQUAL(SAF_ERR_NULL_PTR, saf_is_empty(NULL, &empty));
}

void test_SAF_T36_zero_capacity_and_overfull_clamp_the_percentage(void)
{
    s_cfg.capacity = 0U;
    create_ok();
    TEST_ASSERT_EQUAL_UINT8(0U, s_occ_last_percent);

    TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 4U, 0U));
    TEST_ASSERT_EQUAL_UINT8(100U, s_occ_last_percent);

    saf_reset_for_test();
    s_cfg.capacity = 1U;
    create_ok();

    for (uint32_t i = 0U; i < 4U; i++)
    {
        TEST_ASSERT_EQUAL(SAF_ERR_OK, saf_enqueue(s_h, "t", s_data, 4U, 0U));
    }

    TEST_ASSERT_EQUAL_UINT8(100U, s_occ_last_percent);
}
