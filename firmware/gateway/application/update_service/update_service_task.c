/**
 * @file update_service_task.c
 * @brief UpdateService task side: Machine 3 (download, validate, install, self-check, report).
 *
 * Runs only in UpdateServiceTask, the sole caller of FirmwareStore. The instance mutex is taken
 * for field copies and is never held across a FirmwareStore, transport or lifecycle call, so
 * there is no lock nesting (§16).
 *
 * @see docs/lld/application/update-service-lld.md §5, §8.2, §9.2, §10 to §14
 */

#include <stddef.h>
#include <string.h>

#include "logger/logger.h"
#include "update_service_task.h"

#define US_ALL_BITS (0xFFFFFFFFUL)
#define US_VERIFY_ATTEMPTS (2U)
#define US_INSTALL_ATTEMPTS (2U)

#define MS(x) (pdMS_TO_TICKS(x))

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

/** @brief Wrap-safe "deadline has passed". */
static bool expired(TickType_t now, TickType_t deadline)
{
    return ((int32_t) (now - deadline)) >= 0;
}

static void set_state(us_state_t state)
{
    us_lock();
    g_us.state = state;
    us_unlock();
}

static void free_slot(void)
{
    us_lock();
    g_us.slot_state = US_SLOT_FREE;
    us_unlock();
}

/** @brief Refresh the cached boot info from FirmwareStore (task only). */
static void refresh_boot(void)
{
    firmware_boot_info_t info;

    if (g_us.cfg.fs_boot->get_boot_info(g_us.cfg.fs, &info) == FIRMWARE_STORE_ERR_OK)
    {
        us_lock();
        g_us.boot = info;
        us_unlock();
    }
}

static bool post_event(lifecycle_event_type_t type, uint32_t param)
{
    const lifecycle_event_t ev = {.type = type, .param = param};

    return g_us.cfg.lifecycle->post_event(ev);
}

/** @brief Post a lifecycle event; if refused, retry from the deadline pass every second. */
static void post_event_retrying(lifecycle_event_type_t type, uint32_t param)
{
    if (post_event(type, param))
    {
        g_us.event_retry_armed = false;
        return;
    }

    LOG_WARN(US_LOG_MODULE, "lifecycle event %u refused: retrying", (unsigned) type);
    g_us.event_retry_armed = true;
    g_us.event_retry_type = type;
    g_us.event_retry_at = xTaskGetTickCount() + MS(US_EVENT_RETRY_MS);
}

/** @brief Hand a report to the transport; a full queue is retried, then given up (§14). */
static void report(us_result_t result, us_reason_t reason, bool job_known, uint32_t job)
{
    const us_ota_report_t rep = {
        .job_known = job_known,
        .job = job,
        .result = result,
        .reason = reason,
        .version = g_us.boot.running_version,
        .rollback_count = g_us.boot.rollback_count,
    };

    for (uint32_t attempt = 0U; attempt < US_REPORT_RETRY_MAX; attempt++)
    {
        if (g_us.cfg.transport->report(g_us.cfg.transport->ctx, &rep))
        {
            return;
        }

        vTaskDelay(MS(US_REPORT_RETRY_MS));
    }

    LOG_ERROR(US_LOG_MODULE, "report %u dropped: transport queue stayed full", (unsigned) result);
}

/** @brief End of an update that got past the start: discard, log, report, release (§10.4). */
static void fail(us_reason_t reason, uint32_t code)
{
    LOG_ERROR(US_LOG_MODULE, "OTA job %lu failed: %s (code %lu)", (unsigned long) g_us.cmd.job,
              update_service_reason_string(reason), (unsigned long) code);

    const firmware_store_err_t rc = g_us.cfg.fs_image->discard(g_us.cfg.fs);

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        LOG_ERROR(US_LOG_MODULE, "discard failed (code %u): current firmware untouched",
                  (unsigned) rc);
    }

    report(US_RESULT_FAILED, reason, true, g_us.cmd.job);
    post_event_retrying(LC_EVENT_OTA_FINISHED, g_us.cmd.job);

    g_us.start_armed = false;
    g_us.req_armed = false;
    g_us.paused = false;
    free_slot();
    set_state(US_ST_IDLE);
}

/* ---------------------------------------------------------------------- */
/* Command intake (§8.2)                                                   */
/* ---------------------------------------------------------------------- */

static void reject(const us_ota_command_t *cmd, us_reason_t reason)
{
    LOG_WARN(US_LOG_MODULE, "OTA job %lu rejected: %s", (unsigned long) cmd->job,
             update_service_reason_string(reason));
    report(US_RESULT_REJECTED, reason, true, cmd->job);
}

static void process_inbox(void)
{
    us_ota_command_t cmd;

    us_lock();
    cmd = g_us.inbox;
    g_us.inbox_full = false;
    const us_state_t state = g_us.state;
    us_unlock();

    if (state != US_ST_IDLE) /* rule 1: REQ-DM-054, the update in progress is not touched */
    {
        reject(&cmd, US_REASON_BUSY);
        return;
    }

    if (!g_us.boot.ota_capable || (g_us.boot.state != FIRMWARE_BOOT_CONFIRMED) ||
        ((g_us.boot.running_slot != FIRMWARE_SLOT_A) &&
         (g_us.boot.running_slot != FIRMWARE_SLOT_B))) /* rule 2 */
    {
        reject(&cmd, US_REASON_NOT_OTA_CAPABLE);
        return;
    }

    const lifecycle_state_t lc = g_us.cfg.lifecycle->get_state();

    if ((lc != LIFECYCLE_STATE_OPERATIONAL) && (lc != LIFECYCLE_STATE_EDITING_CONFIG)) /* rule 3 */
    {
        reject(&cmd, US_REASON_NOT_READY);
        return;
    }

    const firmware_slot_t target =
        (g_us.boot.running_slot == FIRMWARE_SLOT_A) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
    const uint32_t size = (target == FIRMWARE_SLOT_B) ? cmd.size_b : cmd.size_a;

    if ((cmd.job == US_JOB_INVALID_LOW) || (cmd.job == US_JOB_INVALID_ERASED) ||
        (size < US_FILE_MIN) || (size > US_FILE_MAX)) /* rule 4 */
    {
        reject(&cmd, US_REASON_BAD_COMMAND);
        return;
    }

    const lifecycle_event_t ev = {.type = LC_EVENT_OTA_REQUESTED, .param = cmd.job};

    if (!g_us.cfg.lifecycle->post_event(ev)) /* rule 5 */
    {
        reject(&cmd, US_REASON_BUSY);
        return;
    }

    g_us.cmd = cmd;
    g_us.target_slot = target;
    g_us.file_len = size;
    g_us.start_armed = true;
    g_us.start_deadline = xTaskGetTickCount() + MS(US_START_TIMEOUT_MS);

    us_lock();
    g_us.active_job = cmd.job;
    g_us.state = US_ST_AWAITING_START;
    us_unlock();
}

/* ---------------------------------------------------------------------- */
/* Download (§10)                                                          */
/* ---------------------------------------------------------------------- */

static void validate(void);

/** @brief The link is down: stop requesting and wait, without counting a failure (§10.4). */
static void pause_download(void)
{
    g_us.paused = true;
    g_us.req_armed = false;
    g_us.next_poll = xTaskGetTickCount() + MS(US_SELF_CHECK_POLL_MS);
    free_slot();
}

static void request(void)
{
    if (!g_us.cfg.transport->is_connected(g_us.cfg.transport->ctx))
    {
        pause_download();
        return;
    }

    const uint32_t left = g_us.file_len - g_us.offset;
    const uint32_t len = (left < US_CHUNK_SIZE) ? left : US_CHUNK_SIZE;
    const us_chunk_request_t req = {
        .job = g_us.cmd.job,
        .slot = g_us.target_slot,
        .offset = g_us.offset,
        .len = len,
    };

    us_lock();
    g_us.slot_state = US_SLOT_EXPECTING;
    g_us.exp_offset = g_us.offset;
    g_us.exp_len = len;
    us_unlock();

    /* A refused request is treated as sent: the deadline turns it into a retry (§10.2). */
    (void) g_us.cfg.transport->request_chunk(g_us.cfg.transport->ctx, &req);

    g_us.req_deadline = xTaskGetTickCount() + MS(US_CHUNK_TIMEOUT_MS);
    g_us.req_armed = true;
}

/** @brief Return the write position to FirmwareStore's last checkpoint (§10.3). */
static bool resync(void)
{
    uint32_t offset = 0U;
    const firmware_store_err_t rc =
        g_us.cfg.fs_image->resume(g_us.cfg.fs, g_us.cmd.job, g_us.file_len, &offset);

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        fail(US_REASON_STAGING_ERROR, (uint32_t) rc);
        return false;
    }

    g_us.offset = offset;

    return true;
}

static void begin_download(void)
{
    if (g_us.state != US_ST_AWAITING_START)
    {
        return;
    }

    g_us.start_armed = false;

    uint32_t offset = 0U;
    firmware_store_err_t rc =
        g_us.cfg.fs_image->resume(g_us.cfg.fs, g_us.cmd.job, g_us.file_len, &offset);

    if (rc == FIRMWARE_STORE_ERR_NO_SESSION)
    {
        offset = 0U;
        rc = g_us.cfg.fs_image->begin(g_us.cfg.fs, g_us.cmd.job, g_us.file_len);
    }

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        fail(US_REASON_STAGING_ERROR, (uint32_t) rc);
        return;
    }

    g_us.offset = offset;
    g_us.failures = 0U;
    g_us.paused = false;
    g_us.last_progress = xTaskGetTickCount();

    report(US_RESULT_ACCEPTED, US_REASON_NONE, true, g_us.cmd.job);
    set_state(US_ST_DOWNLOADING);

    if (g_us.offset >= g_us.file_len)
    {
        validate();
    }
    else
    {
        request();
    }
}

static void on_chunk_ready(void)
{
    us_lock();
    const bool full = (g_us.slot_state == US_SLOT_FULL);
    const uint32_t len = g_us.got_len;
    us_unlock();

    if (!full)
    {
        return;
    }

    g_us.req_armed = false;

    if (g_us.state != US_ST_DOWNLOADING)
    {
        free_slot();
        return;
    }

    const firmware_store_err_t rc =
        g_us.cfg.fs_image->write_chunk(g_us.cfg.fs, g_us.offset, g_us_chunk, len);

    free_slot();

    if (rc == FIRMWARE_STORE_ERR_OK)
    {
        g_us.offset += len;
        g_us.failures = 0U;
        g_us.last_progress = xTaskGetTickCount();

        if (g_us.offset >= g_us.file_len)
        {
            validate();
        }
        else
        {
            request();
        }

        return;
    }

    if (rc == FIRMWARE_STORE_ERR_BAD_PARAM) /* out of order: defensive */
    {
        if (resync())
        {
            request();
        }

        return;
    }

    if ((rc == FIRMWARE_STORE_ERR_FLASH_READ) || (rc == FIRMWARE_STORE_ERR_FLASH_ERASE) ||
        (rc == FIRMWARE_STORE_ERR_FLASH_WRITE))
    {
        g_us.failures++;
        LOG_WARN(US_LOG_MODULE, "write_chunk failed (code %u), failure %u of %u", (unsigned) rc,
                 (unsigned) g_us.failures, (unsigned) US_MAX_RETRIES);

        if (g_us.failures >= US_MAX_RETRIES)
        {
            fail(US_REASON_DOWNLOAD_FAILED, (uint32_t) rc);
        }
        else if (resync())
        {
            request();
        }

        return;
    }

    fail(US_REASON_STAGING_ERROR, (uint32_t) rc);
}

/* ---------------------------------------------------------------------- */
/* Validation, installation, activation (§11, §12)                         */
/* ---------------------------------------------------------------------- */

static us_reason_t verify_reason(firmware_store_err_t rc)
{
    switch (rc)
    {
    case FIRMWARE_STORE_ERR_TOO_LARGE:
        return US_REASON_TOO_LARGE;
    case FIRMWARE_STORE_ERR_BAD_HEADER:
        return US_REASON_BAD_HEADER;
    case FIRMWARE_STORE_ERR_WRONG_SLOT:
        return US_REASON_WRONG_SLOT;
    case FIRMWARE_STORE_ERR_SIG_INVALID:
        return US_REASON_SIG_INVALID;
    case FIRMWARE_STORE_ERR_HASH_MISMATCH:
        return US_REASON_HASH_MISMATCH;
    case FIRMWARE_STORE_ERR_BAD_IMAGE:
        return US_REASON_BAD_IMAGE;
    case FIRMWARE_STORE_ERR_CRYPTO:
        return US_REASON_CRYPTO;
    case FIRMWARE_STORE_ERR_FLASH_READ:
        return US_REASON_VERIFY_IO;
    default:
        return US_REASON_STAGING_ERROR; /* INCOMPLETE, BAD_STATE, ...: a programming error */
    }
}

static void install_and_activate(void)
{
    set_state(US_ST_INSTALLING);

    firmware_store_err_t rc = FIRMWARE_STORE_ERR_INSTALL_VERIFY;

    for (uint32_t attempt = 0U; (attempt < US_INSTALL_ATTEMPTS) && (rc != FIRMWARE_STORE_ERR_OK);
         attempt++)
    {
        bool done = false;

        rc = g_us.cfg.fs_image->install_begin(g_us.cfg.fs);

        while ((rc == FIRMWARE_STORE_ERR_OK) && !done)
        {
            rc = g_us.cfg.fs_image->install_step(g_us.cfg.fs, &done);
            vTaskDelay(1U); /* yield between 2 KB pages (FS-D7) */
        }

        if (rc != FIRMWARE_STORE_ERR_OK)
        {
            LOG_ERROR(US_LOG_MODULE, "install attempt %u failed (code %u)",
                      (unsigned) (attempt + 1U), (unsigned) rc);
        }
    }

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        fail(US_REASON_INSTALL_FAILED, (uint32_t) rc);
        return;
    }

    rc = g_us.cfg.fs_image->activate(g_us.cfg.fs);

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        fail(US_REASON_ACTIVATE_FAILED, (uint32_t) rc);
        return;
    }

    /* No report before the reset: success is known only on the new image (US-D8). */
    set_state(US_ST_ACTIVATED);
    post_event_retrying(LC_EVENT_OTA_RESET_REQUESTED, g_us.cmd.job);
}

static void validate(void)
{
    set_state(US_ST_VALIDATING);

    firmware_image_info_t info = {
        .version = 0U, .image_len = 0U, .target_slot = FIRMWARE_SLOT_NONE};
    firmware_store_err_t rc = FIRMWARE_STORE_ERR_CRYPTO;

    for (uint32_t attempt = 0U; attempt < US_VERIFY_ATTEMPTS; attempt++)
    {
        rc = g_us.cfg.fs_image->verify(g_us.cfg.fs, &info);

        /* CRYPTO and FLASH_READ are worth one more try (UC-20 E1); the rest are final. */
        if ((rc != FIRMWARE_STORE_ERR_CRYPTO) && (rc != FIRMWARE_STORE_ERR_FLASH_READ))
        {
            break;
        }
    }

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        fail(verify_reason(rc), (uint32_t) rc);
        return;
    }

    /* The running slot is never written (REQ-DM-073): refuse anything but the inactive one. */
    if (info.target_slot != g_us.target_slot)
    {
        LOG_ERROR(US_LOG_MODULE, "verify named slot %u, expected %u", (unsigned) info.target_slot,
                  (unsigned) g_us.target_slot);
        fail(US_REASON_STAGING_ERROR, (uint32_t) FIRMWARE_STORE_ERR_WRONG_SLOT);
        return;
    }

    install_and_activate();
}

/* ---------------------------------------------------------------------- */
/* Post-update boot (§13)                                                  */
/* ---------------------------------------------------------------------- */

/** @brief The self-check passed: commit, report, tidy up, tell LifecycleController (§13.3). */
static void self_check_pass(void)
{
    g_us.check_armed = false;

    const firmware_store_err_t rc = g_us.cfg.fs_boot->confirm(g_us.cfg.fs);

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        LOG_ERROR(US_LOG_MODULE, "confirm failed (code %u): treating the self-check as failed",
                  (unsigned) rc);
        post_event_retrying(LC_EVENT_SELF_CHECK_FAIL, (uint32_t) rc);
        return;
    }

    refresh_boot();
    report(US_RESULT_SUCCESS, US_REASON_NONE, false, 0U);

    if (g_us.cfg.fs_image->discard(g_us.cfg.fs) != FIRMWARE_STORE_ERR_OK)
    {
        LOG_ERROR(US_LOG_MODULE, "discard after success failed");
    }

    post_event_retrying(LC_EVENT_SELF_CHECK_PASS, 0U);
    set_state(US_ST_IDLE);
}

static void self_check_poll(TickType_t now)
{
    const bool sensors = g_us.cfg.probes->sensors_ready(g_us.cfg.probes->ctx);
    const bool modbus = g_us.cfg.probes->modbus_ready(g_us.cfg.probes->ctx);
    const bool cloud = g_us.cfg.transport->is_connected(g_us.cfg.transport->ctx);

    if (sensors && modbus && cloud)
    {
        self_check_pass();
    }
    else if (expired(now, g_us.check_deadline))
    {
        LOG_ERROR(US_LOG_MODULE, "self-check failed, not ready:%s%s%s", sensors ? "" : " sensors",
                  modbus ? "" : " modbus", cloud ? "" : " cloud");
        g_us.check_armed = false;
        post_event_retrying(LC_EVENT_SELF_CHECK_FAIL, 0U);
    }
    else
    {
        g_us.next_poll = now + MS(US_SELF_CHECK_POLL_MS);
    }
}

static void start_self_check(void)
{
    const TickType_t now = xTaskGetTickCount();

    g_us.check_armed = true;
    g_us.check_deadline = now + MS(US_SELF_CHECK_TIMEOUT_MS);
    self_check_poll(now);
}

/** @brief The rolled-back image booted: report, acknowledge, tidy up (§13.4). */
static void after_rollback(void)
{
    report(US_RESULT_ROLLED_BACK, US_REASON_ROLLBACK, false, 0U);

    const firmware_store_err_t rc = g_us.cfg.fs_boot->acknowledge_rollback(g_us.cfg.fs);

    if (rc != FIRMWARE_STORE_ERR_OK)
    {
        LOG_ERROR(US_LOG_MODULE, "acknowledge_rollback failed (code %u): reported again next boot",
                  (unsigned) rc);
    }

    if (g_us.cfg.fs_image->discard(g_us.cfg.fs) != FIRMWARE_STORE_ERR_OK)
    {
        LOG_ERROR(US_LOG_MODULE, "discard after rollback failed");
    }

    refresh_boot();
    set_state(US_ST_IDLE);
}

/* ---------------------------------------------------------------------- */
/* Deadlines and the loop (§9.2)                                           */
/* ---------------------------------------------------------------------- */

static void check_download_deadlines(TickType_t now)
{
    if ((now - g_us.last_progress) > MS(US_STALL_LIMIT_MS))
    {
        fail(US_REASON_STALLED, 0U);
        return;
    }

    if (g_us.paused)
    {
        if (expired(now, g_us.next_poll))
        {
            if (g_us.cfg.transport->is_connected(g_us.cfg.transport->ctx))
            {
                g_us.paused = false;
                request();
            }
            else
            {
                g_us.next_poll = now + MS(US_SELF_CHECK_POLL_MS);
            }
        }

        return;
    }

    if (g_us.req_armed && expired(now, g_us.req_deadline))
    {
        if (!g_us.cfg.transport->is_connected(g_us.cfg.transport->ctx))
        {
            pause_download(); /* UC-18 E2: not a failure */
            return;
        }

        g_us.failures++;
        LOG_WARN(US_LOG_MODULE, "chunk at %lu timed out, failure %u of %u",
                 (unsigned long) g_us.offset, (unsigned) g_us.failures, (unsigned) US_MAX_RETRIES);

        if (g_us.failures >= US_MAX_RETRIES)
        {
            fail(US_REASON_DOWNLOAD_FAILED, 0U);
        }
        else
        {
            request();
        }
    }
}

static void check_deadlines(void)
{
    const TickType_t now = xTaskGetTickCount();

    us_lock();
    const us_state_t state = g_us.state;
    us_unlock();

    if ((state == US_ST_AWAITING_START) && g_us.start_armed && expired(now, g_us.start_deadline))
    {
        g_us.start_armed = false;
        report(US_RESULT_REJECTED, US_REASON_START_TIMEOUT, true, g_us.cmd.job);
        set_state(US_ST_IDLE);
    }
    else if (state == US_ST_DOWNLOADING)
    {
        check_download_deadlines(now);
    }
    else if ((state == US_ST_SELF_CHECKING) && g_us.check_armed &&
             (expired(now, g_us.next_poll) || expired(now, g_us.check_deadline)))
    {
        self_check_poll(now);
    }
    else
    {
        /* Nothing is timed in the other states. */
    }

    if (g_us.event_retry_armed && expired(now, g_us.event_retry_at))
    {
        if (post_event(g_us.event_retry_type, g_us.cmd.job))
        {
            g_us.event_retry_armed = false;
        }
        else
        {
            g_us.event_retry_at = now + MS(US_EVENT_RETRY_MS);
        }
    }
}

void us_task_pass(void)
{
    us_lock();
    const uint32_t bits = g_us.pending;
    g_us.pending = 0U;
    us_unlock();

    if ((bits & US_N_COMMAND) != 0U)
    {
        process_inbox();
    }

    if ((bits & US_N_START) != 0U)
    {
        begin_download();
    }

    if ((bits & US_N_CHUNK) != 0U)
    {
        on_chunk_ready();
    }

    if ((bits & US_N_SELF_CHECK) != 0U)
    {
        start_self_check();
    }

    if ((bits & US_N_AFTER_ROLLBACK) != 0U)
    {
        after_rollback();
    }

    check_deadlines();
}

TickType_t us_task_wait_ticks(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t soonest = 0U;
    bool any = false;

    const struct
    {
        bool armed;
        TickType_t at;
    } due[] = {
        {g_us.start_armed, g_us.start_deadline},
        {(g_us.state == US_ST_DOWNLOADING) && g_us.req_armed, g_us.req_deadline},
        {(g_us.state == US_ST_DOWNLOADING) && g_us.paused, g_us.next_poll},
        {g_us.state == US_ST_DOWNLOADING, g_us.last_progress + MS(US_STALL_LIMIT_MS)},
        {g_us.check_armed, g_us.next_poll},
        {g_us.check_armed, g_us.check_deadline},
        {g_us.event_retry_armed, g_us.event_retry_at},
    };

    for (uint32_t i = 0U; i < (sizeof(due) / sizeof(due[0])); i++)
    {
        if (!due[i].armed)
        {
            continue;
        }

        const TickType_t left = expired(now, due[i].at) ? 0U : (due[i].at - now);

        if (!any || (left < soonest))
        {
            soonest = left;
            any = true;
        }
    }

    return any ? soonest : portMAX_DELAY;
}

void us_task_body(void *arg)
{
    (void) arg;

    for (;;)
    {
        uint32_t bits = 0U;

        (void) xTaskNotifyWait(0U, US_ALL_BITS, &bits, us_task_wait_ticks());
        us_task_pass();
    }
}
