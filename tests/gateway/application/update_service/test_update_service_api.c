/**
 * @file test_update_service_api.c
 * @brief UpdateService unit tests: creation, command intake, chunk intake, control functions.
 *
 * Test IDs follow docs/lld/application/update-service-lld.md §22 (TC-US-001..012, 080, 081).
 * FirmwareStore, the transport, the probes and the lifecycle are fakes; time is the FreeRTOS
 * tick mock (1 tick = 1 ms). The task is driven with update_service_task_step_for_test().
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_update_service_api:).
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

/* ---------------------------------------------------------------------- */
/* Create                                                                  */
/* ---------------------------------------------------------------------- */

static update_service_config_t good_config(void)
{
    const update_service_config_t c = {
        .fs_image = &fake_fs_image_ops,
        .fs_boot = &fake_fs_boot_ops,
        .fs = (firmware_store_t *) 0x1000,
        .transport = &fake_transport,
        .probes = &fake_probes,
        .lifecycle = &fake_lifecycle_ops,
    };

    return c;
}

void test_TC_US_001_create_refuses_null_arguments(void)
{
    update_service_t *out = NULL;
    update_service_config_t c = good_config();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(NULL, &out));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, NULL));

    c = good_config();
    c.fs_image = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    c = good_config();
    c.fs_boot = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    c = good_config();
    c.fs = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    c = good_config();
    c.transport = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    c = good_config();
    c.probes = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    c = good_config();
    c.lifecycle = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTaskCreateStatic_call_count);
}

void test_TC_US_001b_create_refuses_a_missing_function_pointer(void)
{
    update_service_t *out = NULL;
    update_service_config_t c = good_config();

    ifirmware_image_store_t image = fake_fs_image_ops;
    ifirmware_boot_control_t boot = fake_fs_boot_ops;
    iota_transport_t transport = fake_transport;
    ilifecycle_t lifecycle = fake_lifecycle_ops;

    image.verify = NULL;
    c.fs_image = &image;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    boot.confirm = NULL;
    c = good_config();
    c.fs_boot = &boot;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    transport.report = NULL;
    c = good_config();
    c.transport = &transport;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));

    lifecycle.post_event = NULL;
    c = good_config();
    c.lifecycle = &lifecycle;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER, update_service_create(&c, &out));
}

void test_TC_US_002_a_second_create_is_refused(void)
{
    update_service_t *second = NULL;
    const update_service_config_t c = good_config();

    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NO_INSTANCE, update_service_create(&c, &second));
    TEST_ASSERT_NULL(second);
}

void test_TC_US_002b_create_fails_when_a_freertos_object_cannot_be_made(void)
{
    update_service_t *out = NULL;
    const update_service_config_t c = good_config();

    g_mock_xSemaphoreCreateMutexStatic_return = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NO_INSTANCE, update_service_create(&c, &out));

    ust_reset_world();
    g_mock_xTaskCreateStatic_return = NULL;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NO_INSTANCE, update_service_create(&c, &out));
    TEST_ASSERT_FALSE(update_control_ops.is_created(&(update_service_t){0}));

    /* A later create succeeds once the object can be made. */
    g_mock_xTaskCreateStatic_return = (TaskHandle_t) 1;
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_service_create(&c, &out));
    TEST_ASSERT_TRUE(update_control_ops.is_created(out));
}

void test_TC_US_003_operations_on_a_foreign_handle_are_refused(void)
{
    update_service_t *foreign = (update_service_t *) 0x2000;
    const us_ota_command_t cmd = {.job = 1U, .size_a = 1000U, .size_b = 1000U};
    const uint8_t data[4] = {0};

    /* Before create. */
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_service_ops.handle_command(foreign, &cmd));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER,
                          update_service_ops.handle_command(NULL, &cmd));

    (void) ust_create();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_service_ops.handle_command(foreign, &cmd));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER,
                          update_service_ops.handle_command(ust_us, NULL));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_service_ops.on_chunk(foreign, 1U, 0U, data, 4U));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NULL_POINTER,
                          update_service_ops.on_chunk(ust_us, 1U, 0U, NULL, 4U));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_control_ops.start(foreign, 1U));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_control_ops.resume_self_checking(foreign));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_control_ops.resume_rollback(foreign));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_NOT_INITIALISED,
                          update_control_ops.resume_after_rollback(foreign));
    TEST_ASSERT_FALSE(update_control_ops.is_created(foreign));
}

void test_TC_US_004_create_caches_the_boot_info_and_makes_no_other_flash_call(void)
{
    (void) ust_create();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_GET_BOOT_INFO));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_log_count());
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskCreateStatic_call_count);
    TEST_ASSERT_EQUAL_INT(0, update_service_state_for_test()); /* IDLE */
}

void test_TC_US_004b_a_failing_get_boot_info_leaves_the_module_not_ota_capable(void)
{
    fake_fs_always(FFS_GET_BOOT_INFO, FIRMWARE_STORE_ERR_FLASH_READ);
    (void) ust_create();
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U));

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(US_REASON_NOT_OTA_CAPABLE, last_report()->reason);
}

/* ---------------------------------------------------------------------- */
/* Command intake                                                          */
/* ---------------------------------------------------------------------- */

void test_TC_US_005_handle_command_copies_and_notifies_and_calls_nothing_else(void)
{
    (void) ust_create();

    const uint32_t fs_before = fake_fs_log_count();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(fs_before, fake_fs_log_count());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_post_calls());
}

void test_TC_US_006_a_second_command_before_the_task_runs_is_busy_and_the_first_is_kept(void)
{
    (void) ust_create();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(7U, 5000U, 3000U));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BUSY, ust_send_command_with(8U, 5000U, 3000U));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(1U));

    ust_step();
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_event_count());
    TEST_ASSERT_EQUAL_UINT32(7U, fake_lifecycle_event_at(0U).param);

    /* The inbox is free again. */
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(9U, 5000U, 3000U));
}

void test_TC_US_007_a_valid_command_asks_lifecycle_and_waits_without_reporting(void)
{
    ust_to_awaiting_start();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_event_count());
    TEST_ASSERT_EQUAL_INT(LC_EVENT_OTA_REQUESTED, fake_lifecycle_event_at(0U).type);
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, fake_lifecycle_event_at(0U).param);
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test()); /* AWAITING_START */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_log_count()); /* only create's get_boot_info */
}

static void expect_rejected(us_reason_t reason)
{
    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_REJECTED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(reason, last_report()->reason);
    TEST_ASSERT_TRUE(last_report()->job_known);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_log_count()); /* no FirmwareStore call */
    TEST_ASSERT_EQUAL_INT(0, update_service_state_for_test());
}

void test_TC_US_008a_rule1_a_command_while_not_idle_is_busy(void)
{
    ust_to_awaiting_start();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(50U, 5000U, 3000U));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_REASON_BUSY, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(50U, last_report()->job);
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test()); /* the first is untouched */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_event_count());
}

void test_TC_US_008b_rule2_a_device_that_is_not_ota_capable_rejects(void)
{
    const firmware_boot_info_t dev = {.ota_capable = false, .running_slot = FIRMWARE_SLOT_NONE};

    ust_reset_world();
    fake_fs_set_boot_info(&dev);
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    expect_rejected(US_REASON_NOT_OTA_CAPABLE);
}

void test_TC_US_008c_rule2_a_state_other_than_confirmed_rejects(void)
{
    const firmware_boot_state_t states[3] = {
        FIRMWARE_BOOT_TRIAL_PENDING, FIRMWARE_BOOT_TRIAL_RUNNING, FIRMWARE_BOOT_ROLLED_BACK};

    for (uint32_t i = 0U; i < 3U; i++)
    {
        const firmware_boot_info_t info = {.ota_capable = true,
                                           .running_slot = FIRMWARE_SLOT_A,
                                           .state = states[i],
                                           .running_version = 0x01000000UL};

        ust_reset_world();
        fake_fs_set_boot_info(&info);
        (void) ust_create();
        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
        ust_step();
        expect_rejected(US_REASON_NOT_OTA_CAPABLE);
    }
}

void test_TC_US_008d_rule3_lifecycle_states_other_than_operational_and_editing_reject(void)
{
    const lifecycle_state_t refused[4] = {LIFECYCLE_STATE_INIT, LIFECYCLE_STATE_RESTARTING,
                                          LIFECYCLE_STATE_UPDATING_FW, LIFECYCLE_STATE_FAULTED};

    for (uint32_t i = 0U; i < 4U; i++)
    {
        ust_reset_world();
        (void) ust_create();
        fake_lifecycle_set_state(refused[i]);
        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
        ust_step();
        expect_rejected(US_REASON_NOT_READY);
        TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_post_calls());
    }

    ust_reset_world();
    (void) ust_create();
    fake_lifecycle_set_state(LIFECYCLE_STATE_EDITING_CONFIG);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test()); /* accepted */
}

void test_TC_US_008e_rule4_a_bad_job_or_size_rejects(void)
{
    const struct
    {
        uint32_t job;
        uint32_t size_b;
    } bad[] = {
        {0U, 3000U}, {0xFFFFFFFFUL, 3000U}, {1U, 128U},
        {1U, 0U},    {1U, 491649UL},        {1U, 0xFFFFFFFFUL},
    };

    for (uint32_t i = 0U; i < (sizeof(bad) / sizeof(bad[0])); i++)
    {
        ust_reset_world();
        (void) ust_create();
        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK,
                              ust_send_command_with(bad[i].job, 5000U, bad[i].size_b));
        ust_step();
        expect_rejected(US_REASON_BAD_COMMAND);
        TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_post_calls());
    }

    /* The limits themselves are valid. */
    ust_reset_world();
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(1U, 5000U, 129U));
    ust_step();
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test());

    ust_reset_world();
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(1U, 5000U, 491648UL));
    ust_step();
    TEST_ASSERT_EQUAL_INT(1, update_service_state_for_test());
}

void test_TC_US_008f_rule5_a_refused_lifecycle_event_is_busy(void)
{
    (void) ust_create();
    fake_lifecycle_refuse_posts(1U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    expect_rejected(US_REASON_BUSY);
}

void test_TC_US_009_a_command_while_downloading_is_busy_and_the_download_is_untouched(void)
{
    ust_to_downloading();

    const uint32_t requests = fake_transport_request_count();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command_with(99U, 5000U, 3000U));
    ust_step();

    TEST_ASSERT_EQUAL_INT(US_RESULT_REJECTED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_BUSY, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(99U, last_report()->job);
    TEST_ASSERT_EQUAL_INT(2, update_service_state_for_test()); /* DOWNLOADING */
    TEST_ASSERT_EQUAL_UINT32(requests, fake_transport_request_count());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_DISCARD));
}

void test_TC_US_010_the_size_of_the_target_slot_is_the_one_used(void)
{
    /* Running Slot A: the target is B and size_b is the file length. */
    ust_to_downloading();
    TEST_ASSERT_EQUAL_UINT32(UST_SIZE_B, fake_fs_last_file_len());
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, fake_transport_request_at(0U)->slot);

    /* Running Slot B: the target is A and size_a is the file length. */
    const firmware_boot_info_t on_b = {.ota_capable = true,
                                       .running_slot = FIRMWARE_SLOT_B,
                                       .state = FIRMWARE_BOOT_CONFIRMED,
                                       .running_version = 0x02000000UL};

    ust_reset_world();
    fake_fs_set_boot_info(&on_b);
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();
    TEST_ASSERT_EQUAL_UINT32(UST_SIZE_A, fake_fs_last_file_len());
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, fake_transport_request_at(0U)->slot);
}

void test_TC_US_011_an_unstarted_command_times_out(void)
{
    ust_to_awaiting_start();

    ust_advance(US_START_TIMEOUT_MS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_count());

    ust_advance(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_REJECTED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_START_TIMEOUT, last_report()->reason);
    TEST_ASSERT_EQUAL_UINT32(UST_JOB, last_report()->job);
    TEST_ASSERT_EQUAL_INT(0, update_service_state_for_test());

    /* A start after the timeout is a state error. */
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE, update_control_ops.start(ust_us, UST_JOB));
}

void test_TC_US_012_start_in_the_wrong_state_or_for_another_job_is_refused(void)
{
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE, update_control_ops.start(ust_us, UST_JOB));

    ust_to_awaiting_start();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE,
                          update_control_ops.start(ust_us, UST_JOB + 1U));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
}

/* ---------------------------------------------------------------------- */
/* Chunk intake and concurrency                                            */
/* ---------------------------------------------------------------------- */

void test_TC_US_080_the_mutex_is_never_held_across_a_call_out(void)
{
    ust_to_downloading();
    ust_download_all();

    TEST_ASSERT_TRUE(fake_fs_log_count() > 4U);
    TEST_ASSERT_TRUE(g_mock_xSemaphoreTake_call_count > 0U);
    TEST_ASSERT_EQUAL_UINT32(g_mock_xSemaphoreTake_call_count, g_mock_xSemaphoreGive_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_audit_violations()); /* also checked in tearDown() */
}

void test_the_stack_high_water_mark_is_read_from_the_task(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, update_service_stack_free_words(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, update_service_stack_free_words(ust_us)); /* not created */

    (void) ust_create();
    g_mock_uxTaskGetStackHighWaterMark_return = 321U;
    TEST_ASSERT_EQUAL_UINT32(321U, update_service_stack_free_words(ust_us));
    TEST_ASSERT_EQUAL_UINT32(0U, update_service_stack_free_words((update_service_t *) 0x2000));
}

void test_TC_US_081_a_chunk_that_arrives_while_the_slot_is_full_is_stale(void)
{
    ust_to_downloading();

    const us_chunk_request_t req = *fake_transport_request_at(0U);
    uint8_t data[US_CHUNK_SIZE];

    (void) memset(data, 0x5A, sizeof(data));

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK,
                          update_service_ops.on_chunk(ust_us, req.job, req.offset, data, req.len));

    /* The task has not run yet: the slot is full and a duplicate must not overwrite it. */
    (void) memset(data, 0xA5, sizeof(data));
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_STALE,
                          update_service_ops.on_chunk(ust_us, req.job, req.offset, data, req.len));

    ust_step();
    TEST_ASSERT_EQUAL_UINT8(0x5A, fake_fs_last_write_data()[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_WRITE_CHUNK));
}

void test_the_reason_strings_match_the_wire_contract(void)
{
    TEST_ASSERT_EQUAL_STRING("", update_service_reason_string(US_REASON_NONE));
    TEST_ASSERT_EQUAL_STRING("busy", update_service_reason_string(US_REASON_BUSY));
    TEST_ASSERT_EQUAL_STRING("not_ota_capable",
                             update_service_reason_string(US_REASON_NOT_OTA_CAPABLE));
    TEST_ASSERT_EQUAL_STRING("not_ready", update_service_reason_string(US_REASON_NOT_READY));
    TEST_ASSERT_EQUAL_STRING("bad_command", update_service_reason_string(US_REASON_BAD_COMMAND));
    TEST_ASSERT_EQUAL_STRING("start_timeout",
                             update_service_reason_string(US_REASON_START_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("download_failed",
                             update_service_reason_string(US_REASON_DOWNLOAD_FAILED));
    TEST_ASSERT_EQUAL_STRING("stalled", update_service_reason_string(US_REASON_STALLED));
    TEST_ASSERT_EQUAL_STRING("staging_error",
                             update_service_reason_string(US_REASON_STAGING_ERROR));
    TEST_ASSERT_EQUAL_STRING("too_large", update_service_reason_string(US_REASON_TOO_LARGE));
    TEST_ASSERT_EQUAL_STRING("bad_header", update_service_reason_string(US_REASON_BAD_HEADER));
    TEST_ASSERT_EQUAL_STRING("wrong_slot", update_service_reason_string(US_REASON_WRONG_SLOT));
    TEST_ASSERT_EQUAL_STRING("sig_invalid", update_service_reason_string(US_REASON_SIG_INVALID));
    TEST_ASSERT_EQUAL_STRING("hash_mismatch",
                             update_service_reason_string(US_REASON_HASH_MISMATCH));
    TEST_ASSERT_EQUAL_STRING("bad_image", update_service_reason_string(US_REASON_BAD_IMAGE));
    TEST_ASSERT_EQUAL_STRING("crypto", update_service_reason_string(US_REASON_CRYPTO));
    TEST_ASSERT_EQUAL_STRING("verify_io", update_service_reason_string(US_REASON_VERIFY_IO));
    TEST_ASSERT_EQUAL_STRING("install_failed",
                             update_service_reason_string(US_REASON_INSTALL_FAILED));
    TEST_ASSERT_EQUAL_STRING("activate_failed",
                             update_service_reason_string(US_REASON_ACTIVATE_FAILED));
    TEST_ASSERT_EQUAL_STRING("rollback", update_service_reason_string(US_REASON_ROLLBACK));
    TEST_ASSERT_EQUAL_STRING("", update_service_reason_string((us_reason_t) 99));
}
