/**
 * @file us_test_util.c
 * @brief Shared helpers for the UpdateService unit tests (see us_test_util.h).
 */

#include "us_test_util.h"

#include "FreeRTOS.h"
#include "fake_fs_ops.h"
#include "fake_lifecycle.h"
#include "fake_log.h"
#include "fake_transport.h"
#include "freertos_mock.h"
#include "unity.h"

update_service_t *ust_us;

static const update_service_config_t k_cfg = {
    .fs_image = &fake_fs_image_ops,
    .fs_boot = &fake_fs_boot_ops,
    .fs = (firmware_store_t *) 0x1000, /* opaque: the fakes never dereference it */
    .transport = &fake_transport,
    .probes = &fake_probes,
    .lifecycle = &fake_lifecycle_ops,
};

void ust_reset_world(void)
{
    mock_freertos_reset();
    fake_log_reset();
    fake_fs_reset();
    fake_transport_reset();
    fake_lifecycle_reset();
    update_service_reset_for_test();
    ust_us = NULL;
}

update_service_t *ust_create(void)
{
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_service_create(&k_cfg, &ust_us));
    TEST_ASSERT_NOT_NULL(ust_us);

    return ust_us;
}

void ust_step(void)
{
    update_service_task_step_for_test();
}

void ust_advance(uint32_t ms)
{
    g_mock_tick_count += ms;
    ust_step();
}

update_service_err_t ust_send_command_with(uint32_t job, uint32_t size_a, uint32_t size_b)
{
    const us_ota_command_t cmd = {.job = job, .size_a = size_a, .size_b = size_b};

    return update_service_ops.handle_command(ust_us, &cmd);
}

update_service_err_t ust_send_command(void)
{
    return ust_send_command_with(UST_JOB, UST_SIZE_A, UST_SIZE_B);
}

void ust_to_awaiting_start(void)
{
    ust_reset_world();
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
}

void ust_to_downloading(void)
{
    ust_to_awaiting_start();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();
}

uint8_t ust_pattern(uint32_t index)
{
    return (uint8_t) ((index * 7U) + 3U);
}

update_service_err_t ust_deliver_requested(void)
{
    const uint32_t n = fake_transport_request_count();

    TEST_ASSERT_TRUE(n > 0U);

    const us_chunk_request_t *req = fake_transport_request_at(n - 1U);
    uint8_t data[US_CHUNK_SIZE];

    for (uint32_t i = 0U; i < req->len; i++)
    {
        data[i] = ust_pattern(req->offset + i);
    }

    const update_service_err_t rc =
        update_service_ops.on_chunk(ust_us, req->job, req->offset, data, req->len);

    ust_step();

    return rc;
}

void ust_download_all(void)
{
    /* UST_SIZE_B bytes: at most four requests; stop when no new request is made. */
    for (uint32_t guard = 0U; guard < 8U; guard++)
    {
        const uint32_t before = fake_transport_request_count();

        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

        if (fake_transport_request_count() == before)
        {
            return;
        }
    }
}
