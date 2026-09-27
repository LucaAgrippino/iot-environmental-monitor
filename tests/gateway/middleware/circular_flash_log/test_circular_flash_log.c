/**
 * @file test_circular_flash_log.c
 * @brief Unit tests for CircularFlashLog (Gateway middleware).
 *
 * Companion: docs/lld/middleware/circular-flash-log.md §12 — CFL-T01..T16.
 *
 * Test isolation: the QspiFlashDriver dependency is injected as an
 * iqspi_flash_t vtable, so this TU supplies a RAM-backed fake and the real
 * qspi_flash.c is never linked — no CMSIS mock is needed. IHealthReport is a
 * counting spy standing in for the GW HealthMonitor, which does not exist yet.
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "circular_flash_log.h"

/* ====================================================================== */
/* Flash simulation                                                       */
/* ====================================================================== */

/** Device byte offset of the CircularFlashLog partition (mapped 0x9002_0000). */
#define SIM_BASE_ADDR (0x00020000UL)
#define SIM_SECTOR_SIZE (4096U)
#define SIM_SECTOR_COUNT (256U)
#define SIM_SIZE_BYTES (SIM_SECTOR_COUNT * SIM_SECTOR_SIZE)
#define SIM_PAGE_SIZE (256U)
#define SIM_ERASED_BYTE (0xFFU)

/** Ring geometry mirrored from the implementation for assertions. */
#define DATA_SECTOR_BASE (2U)
#define DATA_SECTOR_COUNT (254U)

static uint8_t s_flash_sim[SIM_SIZE_BYTES];

/* Failure injection */
static bool s_fail_read;
static bool s_fail_erase;
static uint32_t s_writes_until_fail; /**< UINT32_MAX = never fail. */

/* Call counters */
static uint32_t s_erase_count;

/* IHealthReport spy */
static uint32_t s_overflow_events;
static uint32_t s_flash_err_events;

#define NEVER_FAIL (0xFFFFFFFFUL)

static qspi_flash_err_t sim_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (s_fail_read)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SIM_BASE_ADDR, addr);
    TEST_ASSERT_TRUE(((addr - SIM_BASE_ADDR) + len) <= SIM_SIZE_BYTES);

    (void) memcpy(buf, &s_flash_sim[addr - SIM_BASE_ADDR], len);

    return QSPI_FLASH_OK;
}

/**
 * @brief Page program with the real driver's constraints enforced.
 *
 * The MX25R6435F can only clear bits, so the fake ANDs rather than assigns.
 * That is what makes the dirty-region tests meaningful.
 */
static qspi_flash_err_t sim_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    if (s_writes_until_fail == 0U)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    if (s_writes_until_fail != NEVER_FAIL)
    {
        s_writes_until_fail--;
    }

    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(len > 0U);
    TEST_ASSERT_TRUE(len <= SIM_PAGE_SIZE);
    /* A page program may not cross a page boundary — QSPI_FLASH_ERR_LEN. */
    TEST_ASSERT_EQUAL_UINT32(addr / SIM_PAGE_SIZE, (addr + len - 1U) / SIM_PAGE_SIZE);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SIM_BASE_ADDR, addr);
    TEST_ASSERT_TRUE(((addr - SIM_BASE_ADDR) + len) <= SIM_SIZE_BYTES);

    for (uint16_t i = 0U; i < len; i++)
    {
        s_flash_sim[(addr - SIM_BASE_ADDR) + i] &= data[i];
    }

    return QSPI_FLASH_OK;
}

static qspi_flash_err_t sim_erase_sector(uint32_t addr)
{
    if (s_fail_erase)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SIM_BASE_ADDR, addr);
    TEST_ASSERT_EQUAL_UINT32(0U, (addr - SIM_BASE_ADDR) % SIM_SECTOR_SIZE);

    (void) memset(&s_flash_sim[addr - SIM_BASE_ADDR], SIM_ERASED_BYTE, SIM_SECTOR_SIZE);
    s_erase_count++;

    return QSPI_FLASH_OK;
}

static const iqspi_flash_t s_fake_qspi = {
    .read = sim_read,
    .write_page = sim_write_page,
    .erase_sector = sim_erase_sector,
};

/* IHealthReport placeholder implementation — the spy. */
void health_report_push_buffer_event(health_monitor_handle_t handle, cfl_health_event_t event)
{
    (void) handle;

    if (event == CFL_HEALTH_EVENT_BUFFER_OVERFLOW)
    {
        s_overflow_events++;
    }
    else
    {
        s_flash_err_events++;
    }
}

/* ====================================================================== */
/* Fixture                                                                */
/* ====================================================================== */

static uint8_t s_payload[CFL_MAX_RECORD_BYTES + 1U];
static uint8_t s_readback[CFL_MAX_RECORD_BYTES + 1U];

static uint32_t sim_data_sector_offset(uint32_t ring_index)
{
    return (DATA_SECTOR_BASE + ring_index) * SIM_SECTOR_SIZE;
}

/** @brief Fill a buffer with a deterministic, position-dependent pattern. */
static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) i);
    }
}

static cfl_handle_t create_ok(void)
{
    cfl_handle_t handle = NULL;
    const cfl_config_t config = {
        .qspi = &s_fake_qspi,
        .health = NULL,
    };

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_create(&config, &handle));
    TEST_ASSERT_NOT_NULL(handle);

    return handle;
}

/** @brief Append a pattern record and assert it was accepted. */
static void append_ok(cfl_handle_t handle, uint32_t len, uint8_t seed)
{
    fill_pattern(s_payload, len, seed);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_append(handle, s_payload, len));
}

/** @brief Peek the oldest record and assert it matches the expected pattern. */
static void peek_expect(cfl_handle_t handle, uint32_t len, uint8_t seed)
{
    uint32_t got_len = 0U;

    (void) memset(s_readback, 0, sizeof(s_readback));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK,
                          cfl_peek_oldest(handle, s_readback, &got_len, sizeof(s_readback)));
    TEST_ASSERT_EQUAL_UINT32(len, got_len);

    fill_pattern(s_payload, len, seed);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_payload, s_readback, len);
}

void setUp(void)
{
    (void) memset(s_flash_sim, SIM_ERASED_BYTE, sizeof(s_flash_sim));
    s_fail_read = false;
    s_fail_erase = false;
    s_writes_until_fail = NEVER_FAIL;
    s_erase_count = 0U;
    s_overflow_events = 0U;
    s_flash_err_events = 0U;
    cfl_reset_for_test();
}

void tearDown(void)
{
}

/* ====================================================================== */
/* CFL-T01 — create on fresh flash                                        */
/* ====================================================================== */

void test_cfl_create_fresh_flash_returns_empty_ring(void)
{
    cfl_handle_t handle = create_ok();
    bool empty = false;

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_is_empty(handle, &empty));
    TEST_ASSERT_TRUE(empty);

    uint32_t entries = 1U;
    uint32_t bytes = 1U;

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(0U, entries);
    TEST_ASSERT_EQUAL_UINT32(0U, bytes);

    /* A boot scan must not erase anything — it is read-only. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_erase_count);
}

/* ====================================================================== */
/* CFL-T02 — create argument validation                                   */
/* ====================================================================== */

void test_cfl_create_null_arguments_return_null_ptr(void)
{
    cfl_handle_t handle = NULL;
    const cfl_config_t config = {
        .qspi = &s_fake_qspi,
        .health = NULL,
    };
    const cfl_config_t no_qspi = {
        .qspi = NULL,
        .health = NULL,
    };

    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_create(NULL, &handle));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_create(&config, NULL));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_create(&no_qspi, &handle));
}

/* ====================================================================== */
/* CFL-T03 — pool exhaustion                                              */
/* ====================================================================== */

void test_cfl_create_second_instance_exhausts_pool(void)
{
    cfl_handle_t second = NULL;
    const cfl_config_t config = {
        .qspi = &s_fake_qspi,
        .health = NULL,
    };

    (void) create_ok();
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NO_RESOURCE, cfl_create(&config, &second));
    TEST_ASSERT_NULL(second);
}

/* ====================================================================== */
/* CFL-T04 — peek / consume / drop on an empty ring                       */
/* ====================================================================== */

void test_cfl_peek_on_empty_ring_returns_empty(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t len = 0U;

    TEST_ASSERT_EQUAL_INT(CFL_ERR_EMPTY,
                          cfl_peek_oldest(handle, s_readback, &len, sizeof(s_readback)));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_EMPTY, cfl_consume_oldest(handle));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_EMPTY, cfl_drop_oldest(handle));
    TEST_ASSERT_EQUAL_UINT32(0U, s_overflow_events);
}

/* ====================================================================== */
/* CFL-T05 — append then peek returns the same bytes                      */
/* ====================================================================== */

void test_cfl_append_then_peek_returns_payload(void)
{
    cfl_handle_t handle = create_ok();
    bool empty = true;

    append_ok(handle, 64U, 0x10U);
    peek_expect(handle, 64U, 0x10U);

    /* Peek must not remove the record. */
    peek_expect(handle, 64U, 0x10U);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_is_empty(handle, &empty));
    TEST_ASSERT_FALSE(empty);
}

/* ====================================================================== */
/* CFL-T06 — FIFO order across N appends and N consumes                   */
/* ====================================================================== */

void test_cfl_fifo_order_preserved_across_appends_and_consumes(void)
{
    cfl_handle_t handle = create_ok();
    const uint32_t record_count = 20U;
    bool empty = false;

    for (uint32_t i = 0U; i < record_count; i++)
    {
        append_ok(handle, 32U + i, (uint8_t) (0x40U + i));
    }

    for (uint32_t i = 0U; i < record_count; i++)
    {
        peek_expect(handle, 32U + i, (uint8_t) (0x40U + i));
        TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    }

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_is_empty(handle, &empty));
    TEST_ASSERT_TRUE(empty);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_EMPTY, cfl_consume_oldest(handle));
}

/* ====================================================================== */
/* CFL-T07 — a record that will not fit advances the tail sector           */
/* ====================================================================== */

void test_cfl_append_beyond_sector_advances_tail(void)
{
    cfl_handle_t handle = create_ok();

    /* Two max-size records cannot share a 4 KB sector, so the second append
     * must erase and move to the next ring sector. */
    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x01U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_erase_count);

    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x02U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_erase_count);

    /* The second record landed in ring sector 1. */
    TEST_ASSERT_EQUAL_UINT8(0xBEU, s_flash_sim[sim_data_sector_offset(1U) + 3U]);

    /* Both are still readable, oldest first. */
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x01U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x02U);
}

/* ====================================================================== */
/* CFL-T08 — append until the ring is full                                */
/* ====================================================================== */

void test_cfl_append_until_full_returns_full(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    /* One max-size record per sector fills the ring in DATA_SECTOR_COUNT
     * appends; the next one has nowhere to go. */
    for (uint32_t i = 0U; i < DATA_SECTOR_COUNT; i++)
    {
        append_ok(handle, CFL_MAX_RECORD_BYTES, (uint8_t) i);
    }

    fill_pattern(s_payload, CFL_MAX_RECORD_BYTES, 0xAAU);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_FULL, cfl_append(handle, s_payload, CFL_MAX_RECORD_BYTES));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(DATA_SECTOR_COUNT, entries);
    TEST_ASSERT_EQUAL_UINT32(DATA_SECTOR_COUNT * CFL_MAX_RECORD_BYTES, bytes);

    /* A rejected append must not have raised a flash-error event. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_flash_err_events);
}

/* ====================================================================== */
/* CFL-T09 — drop_oldest frees space and reports the overflow             */
/* ====================================================================== */

void test_cfl_drop_oldest_on_full_ring_frees_space_and_reports(void)
{
    cfl_handle_t handle = create_ok();

    for (uint32_t i = 0U; i < DATA_SECTOR_COUNT; i++)
    {
        append_ok(handle, CFL_MAX_RECORD_BYTES, (uint8_t) i);
    }

    fill_pattern(s_payload, CFL_MAX_RECORD_BYTES, 0xAAU);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_FULL, cfl_append(handle, s_payload, CFL_MAX_RECORD_BYTES));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_drop_oldest(handle));
    TEST_ASSERT_EQUAL_UINT32(1U, s_overflow_events);

    /* Space is now available. */
    append_ok(handle, CFL_MAX_RECORD_BYTES, 0xAAU);

    /* The oldest surviving record is the second one appended, not the first. */
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x01U);
}

/* ====================================================================== */
/* CFL-T10 — simulated write failure leaves consistent state              */
/* ====================================================================== */

void test_cfl_append_write_failure_reports_and_keeps_state_consistent(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    append_ok(handle, 48U, 0x70U);

    /* Fail part-way through the next record's payload write. */
    s_writes_until_fail = 1U;
    fill_pattern(s_payload, 600U, 0x80U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_FLASH_WRITE, cfl_append(handle, s_payload, 600U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_flash_err_events);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(1U, entries);
    TEST_ASSERT_EQUAL_UINT32(48U, bytes);

    /* The ring recovers: the next append lands in a clean sector, and both the
     * surviving record and the new one read back correctly. */
    s_writes_until_fail = NEVER_FAIL;
    append_ok(handle, 100U, 0x90U);

    peek_expect(handle, 48U, 0x70U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    peek_expect(handle, 100U, 0x90U);
}

/* ====================================================================== */
/* CFL-T11 — CRC mismatch on read                                         */
/* ====================================================================== */

void test_cfl_peek_detects_crc_corruption(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t len = 0U;

    append_ok(handle, 128U, 0x30U);

    /* Clear a bit in the payload — a 1->0 change is what real flash decay and
     * an interrupted write both look like. */
    s_flash_sim[sim_data_sector_offset(0U) + CFL_RECORD_HEADER_SZ + 7U] &= (uint8_t) ~0x01U;

    TEST_ASSERT_EQUAL_INT(CFL_ERR_CORRUPT,
                          cfl_peek_oldest(handle, s_readback, &len, sizeof(s_readback)));

    /* StoreAndForward advances past a corrupt entry (companion §10). */
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_EMPTY,
                          cfl_peek_oldest(handle, s_readback, &len, sizeof(s_readback)));
}

/* ====================================================================== */
/* CFL-T12 — boot scan rejects a record interrupted by power loss          */
/* ====================================================================== */

void test_cfl_boot_scan_rejects_partial_tail_record(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    append_ok(handle, 40U, 0xA0U);
    append_ok(handle, 40U, 0xB0U);

    /* Simulate power loss mid-append: the payload and CRC reached flash but the
     * header — written last, so that the magic is the commit point — did not. */
    /* Each record occupies align4(12 + 40 + 2) = 56 bytes, so the third slot
     * begins at offset 112. */
    const uint32_t third = 2U * 56U;
    for (uint32_t i = 0U; i < 40U; i++)
    {
        s_flash_sim[sim_data_sector_offset(0U) + third + CFL_RECORD_HEADER_SZ + i] = 0x5AU;
    }

    /* Reboot. */
    cfl_reset_for_test();
    handle = create_ok();

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(2U, entries);
    TEST_ASSERT_EQUAL_UINT32(80U, bytes);

    /* The two committed records survived, in order. */
    peek_expect(handle, 40U, 0xA0U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    peek_expect(handle, 40U, 0xB0U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));

    /* A new append must avoid the dirty region: it goes to a fresh sector and
     * reads back intact. */
    append_ok(handle, 40U, 0xC0U);
    peek_expect(handle, 40U, 0xC0U);
}

/* ====================================================================== */
/* CFL-T13 — head pointer A/B recovery after consume                      */
/* ====================================================================== */

void test_cfl_head_pointer_recovered_from_metadata_after_reboot(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    /* One max-size record per sector, so consuming one advances the head
     * sector and triggers a metadata write. */
    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x11U);
    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x22U);
    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x33U);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));

    /* Reboot: the head must come back from the metadata zone, not from zero. */
    cfl_reset_for_test();
    handle = create_ok();

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(2U, entries);
    TEST_ASSERT_EQUAL_UINT32(2U * CFL_MAX_RECORD_BYTES, bytes);

    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x22U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x33U);
}

/* ====================================================================== */
/* CFL-T14 — occupancy tracks append and consume history                  */
/* ====================================================================== */

void test_cfl_get_occupancy_matches_append_consume_history(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    append_ok(handle, 10U, 0x01U);
    append_ok(handle, 20U, 0x02U);
    append_ok(handle, 30U, 0x03U);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(3U, entries);
    TEST_ASSERT_EQUAL_UINT32(60U, bytes);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(2U, entries);
    TEST_ASSERT_EQUAL_UINT32(50U, bytes);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_drop_oldest(handle));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(1U, entries);
    TEST_ASSERT_EQUAL_UINT32(30U, bytes);
    TEST_ASSERT_EQUAL_UINT32(1U, s_overflow_events);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(0U, entries);
    TEST_ASSERT_EQUAL_UINT32(0U, bytes);
}

/* ====================================================================== */
/* CFL-T15 — payload exactly CFL_MAX_RECORD_BYTES is accepted             */
/* ====================================================================== */

void test_cfl_append_max_payload_is_accepted(void)
{
    cfl_handle_t handle = create_ok();

    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x5AU);
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x5AU);
}

/* ====================================================================== */
/* CFL-T16 — one byte over the limit is rejected                          */
/* ====================================================================== */

void test_cfl_append_oversized_payload_returns_too_large(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t entries = 1U;
    uint32_t bytes = 1U;

    fill_pattern(s_payload, CFL_MAX_RECORD_BYTES + 1U, 0x77U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_TOO_LARGE,
                          cfl_append(handle, s_payload, CFL_MAX_RECORD_BYTES + 1U));

    /* A zero-length record carries no information and is rejected too. */
    TEST_ASSERT_EQUAL_INT(CFL_ERR_TOO_LARGE, cfl_append(handle, s_payload, 0U));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_get_occupancy(handle, &entries, &bytes));
    TEST_ASSERT_EQUAL_UINT32(0U, entries);
    TEST_ASSERT_EQUAL_UINT32(0U, bytes);
}

/* ====================================================================== */
/* Guard coverage — NULL and uninitialised handles                        */
/* ====================================================================== */

void test_cfl_operations_reject_null_and_uninitialised_handles(void)
{
    struct cfl_inst_probe
    {
        uint8_t raw[512];
    };
    static struct cfl_inst_probe s_zeroed;
    cfl_handle_t stale = (cfl_handle_t) &s_zeroed;
    cfl_handle_t handle = create_ok();
    uint32_t len = 0U;
    bool empty = false;

    (void) memset(&s_zeroed, 0, sizeof(s_zeroed));

    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_append(NULL, s_payload, 4U));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_peek_oldest(NULL, s_readback, &len, 4U));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_consume_oldest(NULL));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_drop_oldest(NULL));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_is_empty(NULL, &empty));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_get_occupancy(NULL, &len, &len));

    /* in_use is false in a zeroed instance, so the guards report NOT_INIT. */
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NOT_INIT, cfl_append(stale, s_payload, 4U));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NOT_INIT, cfl_consume_oldest(stale));

    /* NULL output arguments on a valid handle. */
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_append(handle, NULL, 4U));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_peek_oldest(handle, NULL, &len, 4U));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_is_empty(handle, NULL));
    TEST_ASSERT_EQUAL_INT(CFL_ERR_NULL_PTR, cfl_get_occupancy(handle, NULL, NULL));
}

/* ====================================================================== */
/* Peek buffer smaller than the record                                    */
/* ====================================================================== */

void test_cfl_peek_with_undersized_buffer_reports_required_length(void)
{
    cfl_handle_t handle = create_ok();
    uint32_t len = 0U;
    uint8_t small[16];

    append_ok(handle, 200U, 0x21U);

    TEST_ASSERT_EQUAL_INT(CFL_ERR_TOO_LARGE, cfl_peek_oldest(handle, small, &len, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(200U, len);

    /* The record is untouched and still readable with an adequate buffer. */
    peek_expect(handle, 200U, 0x21U);
}

/* ====================================================================== */
/* Flash read and erase failures propagate                                */
/* ====================================================================== */

void test_cfl_flash_failures_propagate(void)
{
    cfl_config_t config = {
        .qspi = &s_fake_qspi,
        .health = NULL,
    };
    cfl_handle_t handle = NULL;

    /* A metadata read failure aborts create. */
    s_fail_read = true;
    TEST_ASSERT_EQUAL_INT(CFL_ERR_FLASH_READ, cfl_create(&config, &handle));
    s_fail_read = false;

    /* The failed create must not have consumed the pool slot. */
    handle = create_ok();

    append_ok(handle, CFL_MAX_RECORD_BYTES, 0x01U);

    /* The next append needs a fresh sector, so an erase failure surfaces. */
    s_fail_erase = true;
    fill_pattern(s_payload, CFL_MAX_RECORD_BYTES, 0x02U);
    TEST_ASSERT_EQUAL_INT(CFL_ERR_FLASH_ERASE, cfl_append(handle, s_payload, CFL_MAX_RECORD_BYTES));
    TEST_ASSERT_EQUAL_UINT32(1U, s_flash_err_events);

    /* The first record is still intact after the failure. */
    s_fail_erase = false;
    peek_expect(handle, CFL_MAX_RECORD_BYTES, 0x01U);
}

/* ====================================================================== */
/* Ring wrap-around reuses sectors in order                               */
/* ====================================================================== */

void test_cfl_ring_wraps_and_reuses_sectors(void)
{
    cfl_handle_t handle = create_ok();
    bool empty = false;

    /* Walk the tail all the way round the ring and past the start, draining as
     * we go so the ring never fills. */
    for (uint32_t i = 0U; i < DATA_SECTOR_COUNT + 10U; i++)
    {
        append_ok(handle, CFL_MAX_RECORD_BYTES, (uint8_t) i);
        peek_expect(handle, CFL_MAX_RECORD_BYTES, (uint8_t) i);
        TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_consume_oldest(handle));
    }

    TEST_ASSERT_EQUAL_INT(CFL_ERR_OK, cfl_is_empty(handle, &empty));
    TEST_ASSERT_TRUE(empty);

    /* Records are still stored and read back correctly after the wrap. */
    append_ok(handle, 64U, 0xEEU);
    peek_expect(handle, 64U, 0xEEU);
}
