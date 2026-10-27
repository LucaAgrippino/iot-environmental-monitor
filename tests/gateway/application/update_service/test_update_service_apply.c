/**
 * @file test_update_service_apply.c
 * @brief UpdateService unit tests: validation, installation and activation.
 *
 * Test IDs follow docs/lld/application/update-service-lld.md §22 (TC-US-040..049). Every case
 * starts from a completed download of the standard Slot-B file; the fakes script what
 * FirmwareStore answers to verify, install_* and activate.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_update_service_apply:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

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

#define ST_IDLE (0)
#define ST_ACTIVATED (5)

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

/**
 * @brief Run the standard update to the end of the download.
 *
 * The world is NOT reset here: the test has already scripted the fakes it wants (setUp() resets
 * once per case), and ust_to_downloading() would wipe those scripts.
 */
static void run_download_with_scripts(void)
{
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();
    ust_download_all();
}

static void expect_failed(us_reason_t reason)
{
    TEST_ASSERT_EQUAL_INT(US_RESULT_FAILED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(reason, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, last_report()->job);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_DISCARD));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
}

/* ---------------------------------------------------------------------- */
/* Verify and install                                                      */
/* ---------------------------------------------------------------------- */

void test_TC_US_040_a_verified_image_is_installed_step_by_step_with_a_yield_between_steps(void)
{
    fake_fs_set_install_steps(5U);
    run_download_with_scripts();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_VERIFY));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_INSTALL_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(5U, fake_fs_calls(FFS_INSTALL_STEP));
    TEST_ASSERT_EQUAL_UINT32(5U, g_mock_vTaskDelay_call_count); /* FS-D7: a yield per step */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_ACTIVATE));

    const int32_t verify = fake_trace_find("verify", 0U);
    const int32_t begin = fake_trace_find("install_begin", 0U);
    const int32_t step = fake_trace_find("install_step", 0U);
    const int32_t activate = fake_trace_find("activate", 0U);

    TEST_ASSERT_TRUE((verify >= 0) && (verify < begin) && (begin < step) && (step < activate));
}

void test_TC_US_041_every_terminal_verify_code_discards_and_reports_its_reason(void)
{
    const struct
    {
        firmware_store_err_t rc;
        us_reason_t reason;
    } cases[] = {
        {FIRMWARE_STORE_ERR_TOO_LARGE, US_REASON_TOO_LARGE},
        {FIRMWARE_STORE_ERR_BAD_HEADER, US_REASON_BAD_HEADER},
        {FIRMWARE_STORE_ERR_WRONG_SLOT, US_REASON_WRONG_SLOT},
        {FIRMWARE_STORE_ERR_SIG_INVALID, US_REASON_SIG_INVALID},
        {FIRMWARE_STORE_ERR_HASH_MISMATCH, US_REASON_HASH_MISMATCH},
        {FIRMWARE_STORE_ERR_BAD_IMAGE, US_REASON_BAD_IMAGE},
        {FIRMWARE_STORE_ERR_INCOMPLETE, US_REASON_STAGING_ERROR},
        {FIRMWARE_STORE_ERR_BAD_STATE, US_REASON_STAGING_ERROR},
    };

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        ust_reset_world();
        ust_to_awaiting_start();
        fake_fs_always(FFS_VERIFY, cases[i].rc);
        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
        ust_step();
        ust_download_all();

        expect_failed(cases[i].reason);
        TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_VERIFY)); /* terminal: no retry */
        TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_INSTALL_BEGIN));
        TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_ACTIVATE));
        TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));
    }
}

void test_TC_US_042_a_crypto_error_is_tried_once_more(void)
{
    const firmware_store_err_t once = FIRMWARE_STORE_ERR_CRYPTO;

    fake_fs_script(FFS_VERIFY, &once, 1U);
    run_download_with_scripts();

    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_VERIFY));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_ACTIVATE));
    TEST_ASSERT_EQUAL_INT(ST_ACTIVATED, update_service_state_for_test());

    ust_reset_world();
    fake_fs_always(FFS_VERIFY, FIRMWARE_STORE_ERR_CRYPTO);
    run_download_with_scripts();
    expect_failed(US_REASON_CRYPTO);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_VERIFY));
}

void test_TC_US_043_a_flash_read_error_in_verify_is_tried_once_more_then_fails(void)
{
    fake_fs_always(FFS_VERIFY, FIRMWARE_STORE_ERR_FLASH_READ);
    run_download_with_scripts();

    expect_failed(US_REASON_VERIFY_IO);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_VERIFY));
}

void test_TC_US_044_an_install_that_fails_its_check_once_is_begun_again(void)
{
    const firmware_store_err_t once = FIRMWARE_STORE_ERR_INSTALL_VERIFY;

    fake_fs_script(FFS_INSTALL_STEP, &once, 1U);
    run_download_with_scripts();

    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_INSTALL_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_ACTIVATE));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U)); /* the failed attempt is logged */
    TEST_ASSERT_EQUAL_INT(ST_ACTIVATED, update_service_state_for_test());
}

void test_TC_US_045_an_install_that_fails_twice_ends_the_update(void)
{
    fake_fs_always(FFS_INSTALL_STEP, FIRMWARE_STORE_ERR_FLASH_WRITE);
    run_download_with_scripts();

    expect_failed(US_REASON_INSTALL_FAILED);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_INSTALL_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_ACTIVATE));
}

void test_TC_US_045b_an_install_begin_that_fails_twice_ends_the_update(void)
{
    fake_fs_always(FFS_INSTALL_BEGIN, FIRMWARE_STORE_ERR_FLASH_ERASE);
    run_download_with_scripts();

    expect_failed(US_REASON_INSTALL_FAILED);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_INSTALL_STEP));
}

/* ---------------------------------------------------------------------- */
/* Activate                                                                */
/* ---------------------------------------------------------------------- */

void test_TC_US_046_activate_asks_for_the_reset_and_reports_nothing_more(void)
{
    run_download_with_scripts();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));
    TEST_ASSERT_EQUAL_INT(ST_ACTIVATED, update_service_state_for_test());

    /* Only "accepted" has been reported: success is known after the reboot (US-D8). */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_ACCEPTED, last_report()->result);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_OTA_FINISHED));

    /* Nothing more happens while the reset is awaited. */
    ust_advance(60000U);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));
}

void test_TC_US_047_an_activate_error_ends_the_update(void)
{
    fake_fs_always(FFS_ACTIVATE, FIRMWARE_STORE_ERR_FLASH_WRITE);
    run_download_with_scripts();

    expect_failed(US_REASON_ACTIVATE_FAILED);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));
}

void test_TC_US_048_a_refused_reset_request_is_retried_every_second(void)
{
    ust_to_awaiting_start();
    fake_lifecycle_refuse_type(LC_EVENT_OTA_RESET_REQUESTED);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();
    ust_download_all();

    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));
    TEST_ASSERT_EQUAL_INT(ST_ACTIVATED, update_service_state_for_test());
    TEST_ASSERT_TRUE(fake_log_count(1U) >= 1U); /* a WARN was logged */
    TEST_ASSERT_TRUE(fake_log_contains("refused"));

    const uint32_t calls = fake_lifecycle_post_calls();

    ust_advance(US_EVENT_RETRY_MS - 1U);
    TEST_ASSERT_EQUAL_UINT32(calls, fake_lifecycle_post_calls());
    ust_advance(1U);
    TEST_ASSERT_EQUAL_UINT32(calls + 1U, fake_lifecycle_post_calls());
    ust_advance(US_EVENT_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT32(calls + 2U, fake_lifecycle_post_calls());

    fake_lifecycle_refuse_none();
    ust_advance(US_EVENT_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_OTA_RESET_REQUESTED));

    /* Delivered: the retry stops. */
    const uint32_t after = fake_lifecycle_post_calls();

    ust_advance(US_EVENT_RETRY_MS * 3U);
    TEST_ASSERT_EQUAL_UINT32(after, fake_lifecycle_post_calls());
}

void test_TC_US_049_nothing_is_installed_unless_verify_named_the_inactive_slot(void)
{
    /* The running slot is A: a verify that says "A" must never lead to an install. */
    fake_fs_set_verify_info(0x02000000UL, 4096U, FIRMWARE_SLOT_A);
    run_download_with_scripts();

    expect_failed(US_REASON_STAGING_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_INSTALL_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_INSTALL_STEP));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_ACTIVATE));
}

void test_a_failed_update_leaves_the_service_ready_for_the_next_command(void)
{
    fake_fs_always(FFS_VERIFY, FIRMWARE_STORE_ERR_SIG_INVALID);
    run_download_with_scripts();
    expect_failed(US_REASON_SIG_INVALID);

    fake_fs_always(FFS_VERIFY, FIRMWARE_STORE_ERR_OK);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(44U, 5000U, 3000U));
    ust_step();
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test()); /* AWAITING_START */
    TEST_ASSERT_EQUAL_UINT32(44U, fake_lifecycle_event_at(fake_lifecycle_event_count() - 1U).param);
}
