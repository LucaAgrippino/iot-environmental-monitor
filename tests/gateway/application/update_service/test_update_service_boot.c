/**
 * @file test_update_service_boot.c
 * @brief UpdateService unit tests: the post-update boot (self-check, confirm, rollback).
 *
 * Test IDs follow docs/lld/application/update-service-lld.md §22 (TC-US-060..069). The boot
 * state is what the FirmwareStore fake reports to create(); the order of the calls is read from
 * the shared trace.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_update_service_boot:).
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
#define ST_SELF_CHECKING (6)
#define ST_ROLLING_BACK (7)

#define NEW_VERSION (0x02000000UL)
#define OLD_VERSION (0x01000000UL)

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

/** @brief Boot into a given state of the running image and create the instance. */
static void boot_in(firmware_boot_state_t state, firmware_slot_t slot, uint32_t version,
                    uint32_t rollbacks)
{
    const firmware_boot_info_t info = {
        .ota_capable = true,
        .running_slot = slot,
        .state = state,
        .rollback_count = rollbacks,
        .running_version = version,
    };

    ust_reset_world();
    fake_fs_set_boot_info(&info);
    (void) ust_create();
}

static void boot_trial(void)
{
    boot_in(FIRMWARE_BOOT_TRIAL_RUNNING, FIRMWARE_SLOT_B, NEW_VERSION, 0U);
}

static void expect_order(const char *first, const char *second)
{
    const int32_t a = fake_trace_find(first, 0U);
    const int32_t b = fake_trace_find(second, 0U);

    TEST_ASSERT_TRUE_MESSAGE(a >= 0, first);
    TEST_ASSERT_TRUE_MESSAGE(b >= 0, second);
    TEST_ASSERT_TRUE_MESSAGE(a < b, "order");
}

/* ---------------------------------------------------------------------- */
/* Self-check                                                              */
/* ---------------------------------------------------------------------- */

void test_TC_US_060_a_trial_boot_starts_the_self_check_and_polls_the_probes(void)
{
    boot_trial();
    fake_probes_set(false, true); /* not ready yet */

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.resume_self_checking(ust_us));
    TEST_ASSERT_EQUAL_INT(ST_SELF_CHECKING, update_service_state_for_test());
    ust_step();

    TEST_ASSERT_TRUE(fake_transport_connected_calls() > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_CONFIRM));

    const uint32_t polls = fake_transport_connected_calls();

    ust_advance(US_SELF_CHECK_POLL_MS - 1U);
    TEST_ASSERT_EQUAL_UINT32(polls, fake_transport_connected_calls());
    ust_advance(1U);
    TEST_ASSERT_TRUE(fake_transport_connected_calls() > polls);
}

void test_TC_US_060b_probes_that_become_ready_pass_the_check_before_the_deadline(void)
{
    boot_trial();
    fake_probes_set(false, true);
    (void) update_control_ops.resume_self_checking(ust_us);
    ust_step();
    ust_advance(20000U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_CONFIRM));

    fake_probes_set(true, true);
    ust_advance(US_SELF_CHECK_POLL_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_CONFIRM));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));
}

void test_TC_US_061_resume_self_checking_in_any_other_state_is_refused(void)
{
    const firmware_boot_state_t states[3] = {FIRMWARE_BOOT_CONFIRMED, FIRMWARE_BOOT_TRIAL_PENDING,
                                             FIRMWARE_BOOT_ROLLED_BACK};

    for (uint32_t i = 0U; i < 3U; i++)
    {
        boot_in(states[i], FIRMWARE_SLOT_B, NEW_VERSION, 0U);
        TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE,
                              update_control_ops.resume_self_checking(ust_us));
        TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    }

    const firmware_boot_info_t dev = {.ota_capable = false, .running_slot = FIRMWARE_SLOT_NONE};

    ust_reset_world();
    fake_fs_set_boot_info(&dev);
    (void) ust_create();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE,
                          update_control_ops.resume_self_checking(ust_us));
}

void test_TC_US_062_a_passed_self_check_confirms_then_reports_then_tidies_then_tells_lifecycle(void)
{
    boot_trial();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.resume_self_checking(ust_us));
    ust_step();

    expect_order("confirm", "report_success");
    expect_order("report_success", "discard");
    expect_order("discard", "ev_pass");

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_SUCCESS, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_NONE, last_report()->reason);
    TEST_ASSERT_FALSE(last_report()->job_known);
    TEST_ASSERT_EQUAL_HEX32(NEW_VERSION, last_report()->version);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_fs_calls(FFS_GET_BOOT_INFO)); /* create, then the refresh */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());

    /* It does not run again. */
    ust_advance(60000U);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_CONFIRM));
}

void test_TC_US_063_a_cloud_that_stays_away_for_the_whole_budget_fails_the_check(void)
{
    boot_trial();
    fake_transport_set_connected(false);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.resume_self_checking(ust_us));
    ust_step();
    ust_advance(US_SELF_CHECK_TIMEOUT_MS - 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));

    ust_advance(1U);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_log_count()); /* create's get_boot_info only: no write */
    TEST_ASSERT_EQUAL_INT(ST_SELF_CHECKING, update_service_state_for_test());

    /* The verdict is posted once. */
    ust_advance(US_SELF_CHECK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
}

void test_TC_US_064_the_log_names_the_probe_that_failed(void)
{
    const struct
    {
        bool sensors;
        bool modbus;
        bool cloud;
        const char *named;
        const char *not_named;
    } cases[] = {
        {false, true, true, "sensors", "modbus"},
        {true, false, true, "modbus", "sensors"},
        {true, true, false, "cloud", "sensors"},
    };

    for (uint32_t i = 0U; i < 3U; i++)
    {
        boot_trial();
        fake_probes_set(cases[i].sensors, cases[i].modbus);
        fake_transport_set_connected(cases[i].cloud);

        (void) update_control_ops.resume_self_checking(ust_us);
        ust_step();
        ust_advance(US_SELF_CHECK_TIMEOUT_MS);

        TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
        TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U));
        TEST_ASSERT_TRUE(fake_log_contains(cases[i].named));
        TEST_ASSERT_FALSE(fake_log_contains(cases[i].not_named));
    }
}

void test_TC_US_065_a_confirm_that_fails_fails_the_check_and_reports_no_success(void)
{
    boot_trial();
    fake_fs_always(FFS_CONFIRM, FIRMWARE_STORE_ERR_FLASH_WRITE);

    (void) update_control_ops.resume_self_checking(ust_us);
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_fs_calls(FFS_DISCARD));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U));
}

void test_TC_US_066_resume_rollback_enters_rolling_back_without_touching_flash(void)
{
    boot_trial();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE, update_control_ops.resume_rollback(ust_us));

    fake_transport_set_connected(false);
    (void) update_control_ops.resume_self_checking(ust_us);
    ust_step();
    ust_advance(US_SELF_CHECK_TIMEOUT_MS);

    const uint32_t fs_calls = fake_fs_log_count();

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.resume_rollback(ust_us));
    TEST_ASSERT_EQUAL_INT(ST_ROLLING_BACK, update_service_state_for_test());
    TEST_ASSERT_EQUAL_UINT32(fs_calls, fake_fs_log_count());
    TEST_ASSERT_TRUE(fake_log_count(0U) >= 2U); /* the probe failure and the rollback */

    /* Nothing more happens while the rollback is carried out elsewhere. */
    ust_advance(US_SELF_CHECK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_FAIL));
}

/* ---------------------------------------------------------------------- */
/* After a rollback                                                        */
/* ---------------------------------------------------------------------- */

void test_TC_US_067_a_rolled_back_boot_reports_acknowledges_and_tidies_in_that_order(void)
{
    boot_in(FIRMWARE_BOOT_ROLLED_BACK, FIRMWARE_SLOT_A, OLD_VERSION, 2U);

    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.resume_after_rollback(ust_us));
    ust_step();

    expect_order("report_rolled_back", "ack_rollback");
    expect_order("ack_rollback", "discard");

    TEST_ASSERT_EQUAL_UINT32(1U, fake_transport_report_count());
    TEST_ASSERT_EQUAL_INT(US_RESULT_ROLLED_BACK, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_ROLLBACK, last_report()->reason);
    TEST_ASSERT_FALSE(last_report()->job_known);
    TEST_ASSERT_EQUAL_HEX32(OLD_VERSION, last_report()->version);
    TEST_ASSERT_EQUAL_UINT32(2U, last_report()->rollback_count);
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_post_calls());
}

void test_TC_US_067b_resume_after_rollback_in_any_other_state_is_refused(void)
{
    boot_trial();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE,
                          update_control_ops.resume_after_rollback(ust_us));

    boot_in(FIRMWARE_BOOT_CONFIRMED, FIRMWARE_SLOT_A, OLD_VERSION, 0U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_BAD_STATE,
                          update_control_ops.resume_after_rollback(ust_us));
}

void test_TC_US_068_an_acknowledge_that_fails_keeps_the_state_and_blocks_new_commands(void)
{
    boot_in(FIRMWARE_BOOT_ROLLED_BACK, FIRMWARE_SLOT_A, OLD_VERSION, 1U);
    fake_fs_always(FFS_ACK_ROLLBACK, FIRMWARE_STORE_ERR_FLASH_WRITE);

    (void) update_control_ops.resume_after_rollback(ust_us);
    ust_step();

    TEST_ASSERT_EQUAL_INT(US_RESULT_ROLLED_BACK, last_report()->result);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_log_count(0U));
    TEST_ASSERT_EQUAL_INT(ST_IDLE, update_service_state_for_test());

    /* FirmwareStore still says ROLLED_BACK, so a new command is refused. */
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command());
    ust_step();
    TEST_ASSERT_EQUAL_INT(US_RESULT_REJECTED, last_report()->result);
    TEST_ASSERT_EQUAL_INT(US_REASON_NOT_OTA_CAPABLE, last_report()->reason);
}

void test_TC_US_069_after_a_success_the_same_job_starts_a_fresh_download(void)
{
    boot_trial();
    (void) update_control_ops.resume_self_checking(ust_us);

    /* FirmwareStore now says CONFIRMED on the new image, running Slot B. */
    const firmware_boot_info_t confirmed = {.ota_capable = true,
                                            .running_slot = FIRMWARE_SLOT_B,
                                            .state = FIRMWARE_BOOT_CONFIRMED,
                                            .running_version = NEW_VERSION};

    fake_fs_set_boot_info(&confirmed);
    ust_step();
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));

    const firmware_store_err_t no_session = FIRMWARE_STORE_ERR_NO_SESSION;

    fake_fs_script(FFS_RESUME, &no_session, 1U);
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, ust_send_command()); /* same job 42 */
    ust_step();
    TEST_ASSERT_EQUAL_INT(UPDATE_SERVICE_ERR_OK, update_control_ops.start(ust_us, UST_JOB));
    ust_step();

    TEST_ASSERT_EQUAL_UINT32(1U, fake_fs_calls(FFS_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(UST_SIZE_A, fake_fs_last_file_len()); /* running B: the target is A */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_transport_request_at(0U)->offset);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, fake_transport_request_at(0U)->slot);
}

void test_a_refused_pass_event_is_retried(void)
{
    boot_trial();
    fake_lifecycle_refuse_type(LC_EVENT_SELF_CHECK_PASS);

    (void) update_control_ops.resume_self_checking(ust_us);
    ust_step();
    TEST_ASSERT_EQUAL_UINT32(0U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));

    fake_lifecycle_refuse_none();
    ust_advance(US_EVENT_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, fake_lifecycle_events_of(LC_EVENT_SELF_CHECK_PASS));
}
