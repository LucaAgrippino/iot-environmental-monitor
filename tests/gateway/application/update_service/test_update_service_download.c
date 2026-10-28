/**
 * @file test_update_service_download.c
 * @brief UpdateService unit tests: the download (start, request loop, retries, pause, stall).
 *
 * Test IDs follow docs/lld/application/update-service-lld.md §22 (TC-US-020..035). Time is the
 * FreeRTOS tick mock (1 tick = 1 ms); each ust_advance() runs one task pass.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_update_service_download:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "fake_fs_ops.h"
#include "fake_lifecycle.h"
#include "fake_log.h"
#include "fake_transport.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */
#include "update_service.h"
#include "us_reason.h"           /* causes Ceedling to auto-link us_reason.c */
#include "update_service_task.h" /* causes Ceedling to auto-link update_service_task.c */
#include "us_test_util.h"

#define ST_DOWNLOADING (2)
#define ST_IDLE (0)

void setUp(void)
{
    ust_reset_world();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, fake_audit_violations());
}

static const us_ota_report_t *last_report(void)
{
    TEST_ASSERT_TRUE(fake_transport_report_count() > 0U);

    return fake_transport_report_at(fake_transport_report_count() - 1U);
}

static const us_chunk_request_t *last_request(void)
{
    TEST_ASSERT_TRUE(fake_transport_request_count() > 0U);

    return fake_transport_request_at(fake_transport_request_count() - 1U);
}

/** @brief Run the three timeouts of a connected, silent link. */
static void time_out_once(void)
{
    ust_advance(US_CHUNK_TIMEOUT_MS);
}

/* ---------------------------------------------------------------------- */
/* Start                                                                   */
/* ---------------------------------------------------------------------- */

void test_TC_US_020_a_fresh_start_begins_a_session_reports_accepted_and_requests_offset_0(void)
{
    ust_to_awaiting_start();
    const firmware_store_err_t no_session = FIRMWARE_STORE_ERR_NO_SESSION;

    fake_fs_script(FFS_RESUME, &no_session, 1U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, fake_fs_last_job());
    TEST_ASSERT_EQUAL_UINT32(UST_SIZE_B, fake_fs_last_file_len());

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_ACCEPTED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_NONE, last_report()->reason);
    TEST_ASSERT_TRUE(last_report()->job_known);
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, last_report()->job);
    TEST_ASSERT_EQUAL_HEX32(0x01000000UL, last_report()->version);

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, last_request()->job);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, last_request()->slot);
    TEST_ASSERT_EQUAL_UINT32(0U, last_request()->offset);
    TEST_ASSERT_EQUAL_UINT32(US_CHUNK_SIZE, last_request()->len);
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
}

void test_TC_US_021_a_session_that_exists_is_resumed_without_begin(void)
{
    ust_reset_world();
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(UST_JOB, 5000U, 20000U));
    ust_step();

    fake_fs_set_resume_offset(8192U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(8192U, last_request()->offset);
    TEST_ASSERT_EQUAL_UINT32(US_CHUNK_SIZE, last_request()->len);
}

void test_TC_US_022_a_session_that_is_already_complete_goes_straight_to_verify(void)
{
    ust_to_awaiting_start();
    fake_fs_set_resume_offset(UST_SIZE_B);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_VERIFY));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_WRITE_CHUNK));
}

void test_TC_US_023_the_last_chunk_is_the_remainder(void)
{
    ust_to_downloading();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_UINT32(1024U, last_request()->offset);
    TEST_ASSERT_EQUAL_UINT32(1024U, last_request()->len);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_UINT32(2048U, last_request()->offset);
    TEST_ASSERT_EQUAL_UINT32(UST_LAST_CHUNK, last_request()->len);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_UINT32(3U, fake_transport_request_count()); /* no fourth request */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_VERIFY));
}

/* ---------------------------------------------------------------------- */
/* Chunks                                                                  */
/* ---------------------------------------------------------------------- */

void test_TC_US_024_a_matching_chunk_is_written_with_its_bytes_and_the_next_is_requested(void)
{
    ust_to_downloading();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_WRITE_CHUNK));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_last_write_offset());
    TEST_ASSERT_EQUAL_UINT32(US_CHUNK_SIZE, fake_fs_last_write_len());

    for (uint32_t i = 0U; i < US_CHUNK_SIZE; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(ust_pattern(i), fake_fs_last_write_data()[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(1024U, last_request()->offset);
}

void test_TC_US_025_chunks_that_were_not_asked_for_are_stale_and_not_written(void)
{
    ust_to_downloading();

    const us_chunk_request_t req = *last_request();
    uint8_t data[US_CHUNK_SIZE] = {0};

    TEST_ASSERT_EQUAL_INT(
        UPDATE_SERVICE_ERR_STALE,
        update_service_ops.on_chunk(ust_us, req.job + 1U, req.offset, data, req.len));
    TEST_ASSERT_EQUAL_INT(
        UPDATE_SERVICE_ERR_STALE,
        update_service_ops.on_chunk(ust_us, req.job, req.offset + 1024U, data, req.len));
    TEST_ASSERT_EQUAL_INT(
        UPDATE_SERVICE_ERR_STALE,
        update_service_ops.on_chunk(ust_us, req.job, req.offset, data, req.len - 1U));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_WRITE_CHUNK));

    /* And with no request outstanding at all: after the download completed. */
    ust_download_all();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_STALE,
                          update_service_ops.on_chunk(ust_us, req.job, 0U, data, 1024U));
}

void test_TC_US_026_a_chunk_of_no_bytes_or_too_many_is_a_bad_parameter(void)
{
    ust_to_downloading();

    const us_chunk_request_t req = *last_request();
    uint8_t data[US_CHUNK_SIZE + 1U] = {0};

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_PARAM,
                          update_service_ops.on_chunk(ust_us, req.job, req.offset, data, 0U));
    TEST_ASSERT_EQUAL_INT(
        UPDATE_SERVICE_ERR_BAD_PARAM,
        update_service_ops.on_chunk(ust_us, req.job, req.offset, data, US_CHUNK_SIZE + 1U));
}

/* ---------------------------------------------------------------------- */
/* Timeouts and retries                                                    */
/* ---------------------------------------------------------------------- */

void test_TC_US_027_a_silent_link_gets_the_same_chunk_requested_again(void)
{
    ust_to_downloading();

    ust_advance(US_CHUNK_TIMEOUT_MS - 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_request_count());

    ust_advance(1U);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(0U, last_request()->offset);

    time_out_once();
    TEST_ASSERT_EQUAL_UINT32(3U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(0U, last_request()->offset);
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_DISCARD));
}

void test_TC_US_028_the_third_consecutive_timeout_ends_the_update(void)
{
    ust_to_downloading();
    time_out_once();
    time_out_once();
    time_out_once();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_DISCARD));
    TEST_ASSERT_EQUAL_INT(US_RESULT_FAILED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_DOWNLOAD_FAILED, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, last_report()->job);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());

    /* Exactly one ERROR, naming the reason (REQ-DM-053). */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U));
    TEST_ASSERT_TRUE(fake_log_contains("download_failed"));

    /* The update lock is released: a new command is accepted. */
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(43U, 5000U, 3000U));
    ust_step();
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test());
}

void test_TC_US_029_a_success_between_failures_resets_the_count(void)
{
    ust_to_downloading();
    time_out_once();
    time_out_once();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

    time_out_once();
    time_out_once();
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_DISCARD));

    time_out_once(); /* the third in a row since the success */
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_EQUAL_INT(US_REASON_DOWNLOAD_FAILED, last_report()->reason);
}

void test_TC_US_030_a_link_that_is_down_pauses_without_counting_and_resumes_at_the_same_offset(void)
{
    ust_to_downloading();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested()); /* offset 1024 asked */

    fake_transport_set_connected(false);
    ust_advance(US_CHUNK_TIMEOUT_MS);
    ust_advance(US_CHUNK_TIMEOUT_MS);
    ust_advance(US_CHUNK_TIMEOUT_MS);
    ust_advance(US_CHUNK_TIMEOUT_MS);

    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count()); /* nothing new while down */
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_DISCARD));

    fake_transport_set_connected(true);
    ust_advance(US_SELF_CHECK_POLL_MS);

    TEST_ASSERT_EQUAL_UINT32(3U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(1024U, last_request()->offset);

    /* The failure counter was not touched: two more timeouts are still allowed. */
    time_out_once();
    time_out_once();
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
}

void test_TC_US_031_a_download_that_makes_no_progress_for_ten_minutes_stalls(void)
{
    ust_to_downloading();
    fake_transport_set_connected(false);

    ust_advance(US_CHUNK_TIMEOUT_MS);
    ust_advance(US_STALL_LIMIT_MS - US_CHUNK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());

    ust_advance(1U);
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_EQUAL_INT(US_RESULT_FAILED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_STALLED, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_DISCARD));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));
}

void test_TC_US_032_a_flash_error_on_write_resyncs_to_the_checkpoint_and_counts_a_failure(void)
{
    ust_to_downloading();
    const firmware_store_err_t flash = FIRMWARE_STORE_ERR_FLASH_WRITE;

    fake_fs_script(FFS_WRITE_CHUNK, &flash, 1U);
    fake_fs_set_resume_offset(0U); /* the checkpoint */

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_RESUME)); /* start, then the resync */
    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(0U, last_request()->offset); /* from the checkpoint */

    /* That was failure 1: one more timeout is failure 2, the next is the third. */
    time_out_once();
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());
    time_out_once();
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_EQUAL_INT(US_REASON_DOWNLOAD_FAILED, last_report()->reason);
}

void test_TC_US_032b_three_flash_errors_in_a_row_end_the_update(void)
{
    ust_to_downloading();
    fake_fs_always(FFS_WRITE_CHUNK, FIRMWARE_STORE_ERR_FLASH_ERASE);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_EQUAL_INT(US_REASON_DOWNLOAD_FAILED, last_report()->reason);
}

void test_TC_US_032c_an_out_of_order_write_resyncs_without_counting(void)
{
    ust_to_downloading();
    const firmware_store_err_t bad = FIRMWARE_STORE_ERR_BAD_PARAM;

    fake_fs_script(FFS_WRITE_CHUNK, &bad, 1U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());

    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_RESUME));
    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count());

    time_out_once();
    time_out_once();
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test()); /* only 2 counted */
}

void test_TC_US_032d_an_unexpected_write_error_is_a_staging_error(void)
{
    ust_to_downloading();
    fake_fs_always(FFS_WRITE_CHUNK, FIRMWARE_STORE_ERR_BAD_STATE);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_INT(US_REASON_STAGING_ERROR, last_report()->reason);
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
}

void test_TC_US_032e_a_failed_resync_is_a_staging_error(void)
{
    ust_to_downloading();
    const firmware_store_err_t flash = FIRMWARE_STORE_ERR_FLASH_WRITE;
    const firmware_store_err_t read_err = FIRMWARE_STORE_ERR_FLASH_READ;

    fake_fs_script(FFS_WRITE_CHUNK, &flash, 1U);
    fake_fs_script(FFS_RESUME, &read_err, 1U);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_INT(US_REASON_STAGING_ERROR, last_report()->reason);
}

/* ---------------------------------------------------------------------- */
/* Hard errors at the start, and the failure path                          */
/* ---------------------------------------------------------------------- */

void test_TC_US_033_a_resume_or_begin_that_cannot_work_is_a_staging_error(void)
{
    ust_to_awaiting_start();
    fake_fs_always(FFS_RESUME, FIRMWARE_STORE_ERR_FLASH_READ);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count()); /* no "accepted" */
    TEST_ASSERT_EQUAL_INT(US_RESULT_FAILED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_STAGING_ERROR, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());

    ust_to_awaiting_start();
    const firmware_store_err_t no_session = FIRMWARE_STORE_ERR_NO_SESSION;

    fake_fs_script(FFS_RESUME, &no_session, 1U);
    fake_fs_always(FFS_BEGIN, FIRMWARE_STORE_ERR_FLASH_ERASE);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();
    TEST_ASSERT_EQUAL_INT(US_REASON_STAGING_ERROR, last_report()->reason);
}

void test_TC_US_034_a_discard_that_fails_does_not_stop_the_failure_report(void)
{
    ust_to_downloading();
    fake_fs_always(FFS_DISCARD, FIRMWARE_STORE_ERR_FLASH_ERASE);

    time_out_once();
    time_out_once();
    time_out_once();

    TEST_ASSERT_EQUAL_INT(US_RESULT_FAILED, last_report()->result);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_TRUE(fake_log_contains("discard failed"));
}

void test_TC_US_035_a_refused_request_is_retried_at_the_deadline_not_lost(void)
{
    ust_reset_world();
    fake_transport_set_accept_requests(false);
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING, update_service_state_for_test());

    fake_transport_set_accept_requests(true);
    time_out_once();
    TEST_ASSERT_EQUAL_UINT32(2U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_deliver_requested());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_WRITE_CHUNK));
}

void test_a_report_that_the_transport_refuses_is_retried_then_given_up(void)
{
    ust_to_awaiting_start();
    fake_transport_refuse_reports(US_REPORT_RETRY_MAX); /* every attempt of the first report */

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(US_REPORT_RETRY_MAX, fake_transport_report_calls());
    TEST_ASSERT_EQUAL_UINT32(US_REPORT_RETRY_MAX, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_count());
    TEST_ASSERT_TRUE(fake_log_contains("dropped"));
    TEST_ASSERT_EQUAL_INT(ST_DOWNLOADING,
                          update_service_state_for_test()); /* the download goes on */
}

void test_a_report_that_is_accepted_late_is_delivered_once(void)
{
    ust_to_awaiting_start();
    fake_transport_refuse_reports(3U);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(4U, fake_transport_report_calls());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_ACCEPTED, last_report()->result);
}
