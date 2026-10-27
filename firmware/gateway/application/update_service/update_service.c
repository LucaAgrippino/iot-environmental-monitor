/**
 * @file update_service.c
 * @brief UpdateService intake side: create, IUpdateService, IUpdateControl, reason strings.
 *
 * Everything here runs in the caller's task and takes only the instance mutex, for field copies
 * of at most one chunk. No FirmwareStore, transport or lifecycle call is made from this file
 * (§16): the work happens in update_service_task.c.
 *
 * @see docs/lld/application/update-service-lld.md §7, §9.4, §16
 */

#include "update_service.h"

#include <stddef.h>
#include <string.h>

#include "logger/logger.h"
#include "update_service_task.h"

struct update_service g_us;
uint8_t g_us_chunk[US_CHUNK_SIZE];

static StaticTask_t s_tcb;
static StackType_t s_stack[US_TASK_STACK_WORDS];
static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;

void us_lock(void)
{
    if (s_mutex != NULL)
    {
        (void) xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

void us_unlock(void)
{
    if (s_mutex != NULL)
    {
        (void) xSemaphoreGive(s_mutex);
    }
}

/** @brief Set pending bits under the lock, then wake the task. */
static void notify(struct update_service *us, uint32_t bits)
{
    us_lock();
    us->pending |= bits;
    us_unlock();

    if (us->task != NULL)
    {
        (void) xTaskNotify(us->task, bits, eSetBits);
    }
}

static update_service_err_t check_self(const update_service_t *self)
{
    if (self == NULL)
    {
        return UPDATE_SERVICE_ERR_NULL_POINTER;
    }

    if ((self != &g_us) || !g_us.created)
    {
        return UPDATE_SERVICE_ERR_NOT_INITIALISED;
    }

    return UPDATE_SERVICE_ERR_OK;
}

static bool config_is_complete(const update_service_config_t *c)
{
    return (c->fs_image != NULL) && (c->fs_boot != NULL) && (c->fs != NULL) &&
           (c->transport != NULL) && (c->probes != NULL) && (c->lifecycle != NULL) &&
           (c->fs_image->begin != NULL) && (c->fs_image->resume != NULL) &&
           (c->fs_image->write_chunk != NULL) && (c->fs_image->verify != NULL) &&
           (c->fs_image->install_begin != NULL) && (c->fs_image->install_step != NULL) &&
           (c->fs_image->activate != NULL) && (c->fs_image->discard != NULL) &&
           (c->fs_boot->get_boot_info != NULL) && (c->fs_boot->confirm != NULL) &&
           (c->fs_boot->acknowledge_rollback != NULL) && (c->transport->is_connected != NULL) &&
           (c->transport->request_chunk != NULL) && (c->transport->report != NULL) &&
           (c->probes->sensors_ready != NULL) && (c->probes->modbus_ready != NULL) &&
           (c->lifecycle->post_event != NULL) && (c->lifecycle->get_state != NULL);
}

update_service_err_t update_service_create(const update_service_config_t *cfg,
                                           update_service_t **out)
{
    if ((cfg == NULL) || (out == NULL) || !config_is_complete(cfg))
    {
        return UPDATE_SERVICE_ERR_NULL_POINTER;
    }

    if (g_us.created)
    {
        return UPDATE_SERVICE_ERR_NO_INSTANCE;
    }

    (void) memset(&g_us, 0, sizeof(g_us));
    g_us.cfg = *cfg;
    g_us.state = US_ST_IDLE;
    g_us.target_slot = FIRMWARE_SLOT_NONE;

    /* The one FirmwareStore call create makes: cache the boot state. A failure means "not OTA
     * capable", which is also what get_boot_info reports for a development image. */
    if (cfg->fs_boot->get_boot_info(cfg->fs, &g_us.boot) != FIRMWARE_STORE_ERR_OK)
    {
        (void) memset(&g_us.boot, 0, sizeof(g_us.boot));
        g_us.boot.running_slot = FIRMWARE_SLOT_NONE;
        LOG_ERROR(US_LOG_MODULE, "get_boot_info failed at create: not OTA capable");
    }

    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);

    if (s_mutex == NULL)
    {
        return UPDATE_SERVICE_ERR_NO_INSTANCE;
    }

    g_us.task = xTaskCreateStatic(us_task_body, "UpdateServiceTask", US_TASK_STACK_WORDS, NULL,
                                  US_TASK_PRIORITY, s_stack, &s_tcb);

    if (g_us.task == NULL)
    {
        s_mutex = NULL;
        return UPDATE_SERVICE_ERR_NO_INSTANCE;
    }

    g_us.created = true;
    *out = &g_us;

    return UPDATE_SERVICE_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* IUpdateService (CloudPublisherTask)                                      */
/* ---------------------------------------------------------------------- */

static update_service_err_t us_handle_command(update_service_t *self, const us_ota_command_t *cmd)
{
    if ((self == NULL) || (cmd == NULL))
    {
        return UPDATE_SERVICE_ERR_NULL_POINTER;
    }

    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    us_lock();
    const bool full = us->inbox_full;

    if (!full)
    {
        us->inbox = *cmd;
        us->inbox_full = true;
    }

    us_unlock();

    if (full)
    {
        LOG_WARN(US_LOG_MODULE, "OTA command dropped: inbox full (job %lu)",
                 (unsigned long) cmd->job);
        return UPDATE_SERVICE_ERR_BUSY;
    }

    notify(us, US_N_COMMAND);

    return UPDATE_SERVICE_ERR_OK;
}

static update_service_err_t us_on_chunk(update_service_t *self, uint32_t job, uint32_t offset,
                                        const uint8_t *data, uint32_t len)
{
    if ((self == NULL) || (data == NULL))
    {
        return UPDATE_SERVICE_ERR_NULL_POINTER;
    }

    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    if ((len == 0U) || (len > US_CHUNK_SIZE))
    {
        return UPDATE_SERVICE_ERR_BAD_PARAM;
    }

    us_lock();
    const bool wanted = (us->slot_state == US_SLOT_EXPECTING) && (job == us->active_job) &&
                        (offset == us->exp_offset) && (len == us->exp_len);

    if (wanted)
    {
        (void) memcpy(g_us_chunk, data, len);
        us->got_len = len;
        us->slot_state = US_SLOT_FULL;
    }

    us_unlock();

    if (!wanted)
    {
        return UPDATE_SERVICE_ERR_STALE;
    }

    notify(us, US_N_CHUNK);

    return UPDATE_SERVICE_ERR_OK;
}

const iupdate_service_t update_service_ops = {
    .handle_command = us_handle_command,
    .on_chunk = us_on_chunk,
};

/* ---------------------------------------------------------------------- */
/* IUpdateControl (LifecycleTask)                                           */
/* ---------------------------------------------------------------------- */

static bool us_is_created(const update_service_t *self)
{
    return (self == &g_us) && self->created;
}

static update_service_err_t us_start(update_service_t *self, uint32_t job)
{
    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    us_lock();
    const bool ok = (us->state == US_ST_AWAITING_START) && (job == us->active_job);
    us_unlock();

    if (!ok)
    {
        return UPDATE_SERVICE_ERR_BAD_STATE;
    }

    notify(us, US_N_START);

    return UPDATE_SERVICE_ERR_OK;
}

static update_service_err_t us_resume_self_checking(update_service_t *self)
{
    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    us_lock();
    const bool ok = us->boot.ota_capable && (us->boot.state == FIRMWARE_BOOT_TRIAL_RUNNING);

    if (ok)
    {
        us->state = US_ST_SELF_CHECKING;
    }

    us_unlock();

    if (!ok)
    {
        return UPDATE_SERVICE_ERR_BAD_STATE;
    }

    notify(us, US_N_SELF_CHECK);

    return UPDATE_SERVICE_ERR_OK;
}

static update_service_err_t us_resume_rollback(update_service_t *self)
{
    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    us_lock();
    const bool ok = (us->state == US_ST_SELF_CHECKING);

    if (ok)
    {
        us->state = US_ST_ROLLING_BACK;
        us->check_armed = false;
    }

    us_unlock();

    if (!ok)
    {
        return UPDATE_SERVICE_ERR_BAD_STATE;
    }

    LOG_ERROR(US_LOG_MODULE, "self-check failed: rolling back");

    return UPDATE_SERVICE_ERR_OK;
}

static update_service_err_t us_resume_after_rollback(update_service_t *self)
{
    const update_service_err_t rc = check_self(self);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        return rc;
    }

    struct update_service *const us = self;

    us_lock();
    const bool ok = us->boot.ota_capable && (us->boot.state == FIRMWARE_BOOT_ROLLED_BACK);
    us_unlock();

    if (!ok)
    {
        return UPDATE_SERVICE_ERR_BAD_STATE;
    }

    notify(us, US_N_AFTER_ROLLBACK);

    return UPDATE_SERVICE_ERR_OK;
}

const iupdate_control_t update_control_ops = {
    .is_created = us_is_created,
    .start = us_start,
    .resume_self_checking = us_resume_self_checking,
    .resume_rollback = us_resume_rollback,
    .resume_after_rollback = us_resume_after_rollback,
};

/* ---------------------------------------------------------------------- */
/* Reasons                                                                  */
/* ---------------------------------------------------------------------- */

const char *update_service_reason_string(us_reason_t reason)
{
    static const char *const k_names[] = {
        [US_REASON_NONE] = "",
        [US_REASON_BUSY] = "busy",
        [US_REASON_NOT_OTA_CAPABLE] = "not_ota_capable",
        [US_REASON_NOT_READY] = "not_ready",
        [US_REASON_BAD_COMMAND] = "bad_command",
        [US_REASON_START_TIMEOUT] = "start_timeout",
        [US_REASON_DOWNLOAD_FAILED] = "download_failed",
        [US_REASON_STALLED] = "stalled",
        [US_REASON_STAGING_ERROR] = "staging_error",
        [US_REASON_TOO_LARGE] = "too_large",
        [US_REASON_BAD_HEADER] = "bad_header",
        [US_REASON_WRONG_SLOT] = "wrong_slot",
        [US_REASON_SIG_INVALID] = "sig_invalid",
        [US_REASON_HASH_MISMATCH] = "hash_mismatch",
        [US_REASON_BAD_IMAGE] = "bad_image",
        [US_REASON_CRYPTO] = "crypto",
        [US_REASON_VERIFY_IO] = "verify_io",
        [US_REASON_INSTALL_FAILED] = "install_failed",
        [US_REASON_ACTIVATE_FAILED] = "activate_failed",
        [US_REASON_ROLLBACK] = "rollback",
    };

    if ((uint32_t) reason >= (sizeof(k_names) / sizeof(k_names[0])))
    {
        return "";
    }

    return k_names[reason];
}

#ifdef TEST
void update_service_reset_for_test(void)
{
    (void) memset(&g_us, 0, sizeof(g_us));
    (void) memset(g_us_chunk, 0, sizeof(g_us_chunk));
    s_mutex = NULL;
}

void update_service_task_step_for_test(void)
{
    us_task_pass();
}

int update_service_state_for_test(void)
{
    return (int) g_us.state;
}
#endif
