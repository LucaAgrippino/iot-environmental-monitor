/**
 * @file time_service.c
 * @brief TimeService implementation (Gateway) — NTP sync policy, RTC write, FD time push.
 *
 * @see docs/lld/application/time-service-lld.md
 */

#include "time_service.h"
#include "time_service_internal.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "timers.h"

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "TS"

/* Notification bits for TimeServiceTask. */
#define TS_NOTIFY_BIT_PERIODIC (1UL << 0)
#define TS_NOTIFY_BIT_RETRY (1UL << 1)

/* Task sizing per task-breakdown.md §TimeServiceTask. */
#define TS_TASK_STACK_WORDS (512U) /* D-1 F: 337 of 768 words used */
#define TS_TASK_PRIORITY (2U)

#define TS_MS_PER_S (1000UL)
#define TS_TIMER_PLACEHOLDER_MS (1000U) /* real period is set when the task starts */
#define TS_FD_RESPONSE_TIMEOUT_MS (5000U)

/* Field Device time push (REQ-TS-030): one FC16 write of the two-word CMD_SET_TIME block,
 * docs/hld/modbus-register-map.md §6.5. The FD applies it when the low word lands (TS-O5). */
#define TS_FD_SLAVE_ADDR (1U)
#define TS_FD_FUNCTION_WRITE_MULTIPLE (0x10U)
#define TS_FD_REG_CMD_SET_TIME_HI (0x0210U)
#define TS_FD_TIME_REG_COUNT (2U)
#define TS_WORD_SHIFT (16U)
#define TS_WORD_MASK (0xFFFFU)

/* NTP servers until the Gateway has an IConfigProvider (TS-O4). IPv4 literals, because the
 * ISM43362 has no DNS resolver (NtpClient harness, TS-O3): Cloudflare, then Google. */
static const char *const s_ntp_servers[] = {"162.159.200.1", "216.239.35.0"};
#define TS_NTP_SERVER_COUNT ((uint8_t) (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0])))

/* Servers from the configuration (TS-O4); count 0 = use s_ntp_servers. Written by
 * time_service_reconfigure(), copied by the sync task under a critical section. */
static char s_cfg_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
static uint8_t s_cfg_server_count;
static uint32_t s_cfg_interval_s = TIME_SERVICE_NTP_INTERVAL_S_DEFAULT;

static struct
{
    bool initialised;
    volatile time_service_sync_t sync_state;
    ntp_client_handle_t ntp;
    TaskHandle_t task_handle;
    TimerHandle_t ntp_timer;
    uint32_t interval_s;
    bool timer_armed; /**< The periodic timer was started by prv_arm_timer(). */
} s_ts;

static StackType_t s_task_stack[TS_TASK_STACK_WORDS];
static StaticTask_t s_task_tcb;
static StaticTimer_t s_timer_storage;
static StaticQueue_t s_fd_resp_queue_storage;
static uint8_t s_fd_resp_buf[sizeof(modbus_command_response_t)];
static QueueHandle_t s_fd_resp_queue;
static itime_service_t s_vtable;

static void prv_task(void *params);
static void prv_timer_cb(TimerHandle_t timer);
static void prv_arm_timer(void);
static void prv_handle_notification(uint32_t bits);
static time_service_err_t prv_push_time_to_fd(uint32_t utc);
static time_service_sync_t prv_get_sync_state(void *ctx);
static void prv_trigger_retry(void *ctx);
static time_service_err_t prv_do_ntp_sync(void);

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

time_service_err_t time_service_init(ntp_client_handle_t ntp)
{
    if (s_ts.initialised)
    {
        return TIME_SERVICE_OK; /* idempotent */
    }
    if (ntp == NULL)
    {
        return TIME_SERVICE_ERR_NULL_ARG;
    }

    s_ts.ntp = ntp;
    s_ts.sync_state = TIME_SERVICE_SYNC_NEVER;
    s_ts.interval_s = s_cfg_interval_s;

    s_fd_resp_queue = xQueueCreateStatic(1U, (UBaseType_t) sizeof(modbus_command_response_t),
                                         s_fd_resp_buf, &s_fd_resp_queue_storage);

    s_ts.ntp_timer = xTimerCreateStatic("ntp", pdMS_TO_TICKS(TS_TIMER_PLACEHOLDER_MS), pdTRUE, NULL,
                                        prv_timer_cb, &s_timer_storage);

    s_ts.task_handle = xTaskCreateStatic(prv_task, "TS", TS_TASK_STACK_WORDS, NULL,
                                         TS_TASK_PRIORITY, s_task_stack, &s_task_tcb);

    s_vtable.ctx = &s_ts;
    s_vtable.get_sync_state = prv_get_sync_state;
    s_vtable.trigger_retry = prv_trigger_retry;

    s_ts.initialised = true;
    return TIME_SERVICE_OK;
}

void time_service_reconfigure(const config_params_t *params)
{
    if (params == NULL)
    {
        return;
    }

    const uint32_t interval_s = (uint32_t) params->ntp_interval_min * 60U;

    taskENTER_CRITICAL();
    s_cfg_server_count = (params->ntp_server_count <= CONFIG_MAX_NTP_SERVERS)
                             ? params->ntp_server_count
                             : (uint8_t) CONFIG_MAX_NTP_SERVERS;
    (void) memcpy(s_cfg_servers, params->ntp_servers, sizeof(s_cfg_servers));
    s_cfg_interval_s = (interval_s > 0U) ? interval_s : TIME_SERVICE_NTP_INTERVAL_S_DEFAULT;
    taskEXIT_CRITICAL();

    if (s_ts.initialised)
    {
        s_ts.interval_s = s_cfg_interval_s;
        if (s_ts.timer_armed)
        {
            prv_arm_timer(); /* the new period applies from now */
        }
    }
}

const itime_service_t *time_service_get_interface(void)
{
    return s_ts.initialised ? &s_vtable : NULL;
}

/* ======================================================================= */
/* Task                                                                     */
/* ======================================================================= */

static void prv_arm_timer(void)
{
    const TickType_t period = pdMS_TO_TICKS(s_ts.interval_s * TS_MS_PER_S);
    (void) xTimerChangePeriod(s_ts.ntp_timer, period, portMAX_DELAY);
    s_ts.timer_armed = true;
}

/* Boot sequence (TS-D3): attempt a sync at once, then start the periodic timer. */
static void prv_boot(void)
{
    (void) prv_do_ntp_sync();
    prv_arm_timer();
    (void) xTimerStart(s_ts.ntp_timer, portMAX_DELAY);
}

static void prv_handle_notification(uint32_t bits)
{
    if ((bits & (TS_NOTIFY_BIT_PERIODIC | TS_NOTIFY_BIT_RETRY)) == 0U)
    {
        return;
    }

    (void) prv_do_ntp_sync();

    /* The interval is applied from the next cycle onwards (TS-D6). */
    if ((bits & TS_NOTIFY_BIT_PERIODIC) != 0U)
    {
        prv_arm_timer();
    }
}

static void prv_task(void *params)
{
    (void) params;

    prv_boot();

    for (;;)
    {
        uint32_t bits = 0U;
        (void) xTaskNotifyWait(0U, UINT32_MAX, &bits, portMAX_DELAY);
        prv_handle_notification(bits);
    }
}

static void prv_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    (void) xTaskNotify(s_ts.task_handle, TS_NOTIFY_BIT_PERIODIC, eSetBits);
}

/* ======================================================================= */
/* Sync algorithm                                                           */
/* ======================================================================= */

/* The RTC is a usable reference only after an earlier sync and while TimeProvider still
 * reports itself synchronised; otherwise it holds an arbitrary post-reset value (TS-D5). */
static bool prv_rtc_is_reference(uint32_t *epoch_out)
{
    if (s_ts.sync_state == TIME_SERVICE_SYNC_NEVER)
    {
        return false;
    }

    time_provider_ts_t now;
    if ((time_provider_get(&now) != TIME_PROVIDER_ERR_OK) ||
        (now.sync_state != TIME_SYNC_SYNCHRONISED))
    {
        return false;
    }

    *epoch_out = now.epoch;
    return true;
}

static time_service_err_t prv_do_ntp_sync(void)
{
    uint32_t new_utc = 0U;
    char local[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    const char *list[CONFIG_MAX_NTP_SERVERS];
    uint8_t count = 0U;

    /* A copy, so a reconfigure during the (seconds-long) query cannot change a name under it. */
    taskENTER_CRITICAL();
    count = s_cfg_server_count;
    (void) memcpy(local, s_cfg_servers, sizeof(local));
    taskEXIT_CRITICAL();

    if (count > 0U)
    {
        for (uint8_t i = 0U; i < count; i++)
        {
            local[i][CONFIG_MAX_NTP_HOST_LEN - 1U] = '\0';
            list[i] = local[i];
        }
    }
    else
    {
        count = TS_NTP_SERVER_COUNT;
        for (uint8_t i = 0U; i < count; i++)
        {
            list[i] = s_ntp_servers[i];
        }
    }

    const ntp_client_err_t ntp_rc = ntp_client_query(s_ts.ntp, list, count, &new_utc);
    if (ntp_rc != NTP_CLIENT_ERR_OK)
    {
        if (s_ts.sync_state != TIME_SERVICE_SYNC_NEVER)
        {
            s_ts.sync_state = TIME_SERVICE_SYNC_STALE;
        }
        LOG_WARN(MODULE_TAG, "NTP sync failed: %d", (int) ntp_rc);
        return TIME_SERVICE_ERR_NTP_FAILED;
    }

    uint32_t current_utc = 0U;
    if (prv_rtc_is_reference(&current_utc))
    {
        const uint32_t delta =
            (new_utc > current_utc) ? (new_utc - current_utc) : (current_utc - new_utc);
        if (delta > TIME_SERVICE_NTP_MAX_DELTA_S)
        {
            s_ts.sync_state = TIME_SERVICE_SYNC_STALE;
            LOG_WARN(MODULE_TAG, "NTP delta %u s rejected", (unsigned) delta);
            return TIME_SERVICE_ERR_DELTA_REJECTED;
        }
    }

    /* A failed RTC write is not fatal (TS-D7): the correct time is known, so the state
     * is OK and the RTC keeps its last value. */
    const time_provider_err_t tp_rc = time_provider_set_time(new_utc);
    if (tp_rc != TIME_PROVIDER_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "RTC write failed: %d", (int) tp_rc);
    }

    s_ts.sync_state = TIME_SERVICE_SYNC_OK;
    LOG_INFO(MODULE_TAG, "NTP sync ok: UTC %u", (unsigned) new_utc);

    (void) prv_push_time_to_fd(new_utc); /* logged inside; does not affect the result (TS-D9) */

    return (tp_rc != TIME_PROVIDER_ERR_OK) ? TIME_SERVICE_ERR_RTC_WRITE : TIME_SERVICE_OK;
}

/* ======================================================================= */
/* Field Device time push                                                   */
/* ======================================================================= */

static time_service_err_t prv_push_time_to_fd(uint32_t utc)
{
    modbus_command_t cmd;
    (void) memset(&cmd, 0, sizeof(cmd));
    cmd.response_queue = s_fd_resp_queue;
    cmd.slave_addr = TS_FD_SLAVE_ADDR;
    cmd.function_code = TS_FD_FUNCTION_WRITE_MULTIPLE;
    cmd.start_reg = TS_FD_REG_CMD_SET_TIME_HI;
    cmd.reg_count = TS_FD_TIME_REG_COUNT;
    cmd.data[0] = (uint16_t) (utc >> TS_WORD_SHIFT);
    cmd.data[1] = (uint16_t) (utc & TS_WORD_MASK);

    const modbus_poller_err_t rc = modbus_poller_send_command(&cmd);
    if (rc != MODBUS_POLLER_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "FD time push rejected: %d", (int) rc);
        return TIME_SERVICE_ERR_FD_PUSH;
    }

    modbus_command_response_t resp;
    (void) memset(&resp, 0, sizeof(resp));
    if (xQueueReceive(s_fd_resp_queue, &resp, pdMS_TO_TICKS(TS_FD_RESPONSE_TIMEOUT_MS)) != pdTRUE)
    {
        LOG_WARN(MODULE_TAG, "FD time push: response timeout");
        return TIME_SERVICE_ERR_FD_PUSH;
    }
    if (resp.status != MODBUS_MASTER_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "FD time push failed: Modbus error %d", (int) resp.status);
        return TIME_SERVICE_ERR_FD_PUSH;
    }
    return TIME_SERVICE_OK;
}

/* ======================================================================= */
/* Vtable                                                                   */
/* ======================================================================= */

static time_service_sync_t prv_get_sync_state(void *ctx)
{
    (void) ctx;
    return s_ts.sync_state;
}

static void prv_trigger_retry(void *ctx)
{
    (void) ctx;
    if (s_ts.task_handle != NULL)
    {
        (void) xTaskNotify(s_ts.task_handle, TS_NOTIFY_BIT_RETRY, eSetBits);
    }
}

#ifdef TEST
void time_service_reset_for_test(void)
{
    s_cfg_server_count = 0U;
    s_cfg_interval_s = TIME_SERVICE_NTP_INTERVAL_S_DEFAULT;
    (void) memset(&s_ts, 0, sizeof(s_ts));
    (void) memset(&s_vtable, 0, sizeof(s_vtable));
    s_fd_resp_queue = NULL;
}

time_service_err_t time_service_sync_for_test(void)
{
    return prv_do_ntp_sync();
}

void time_service_boot_for_test(void)
{
    prv_boot();
}

void time_service_notify_for_test(uint32_t bits)
{
    prv_handle_notification(bits);
}

void time_service_set_interval_for_test(uint32_t seconds)
{
    s_ts.interval_s = seconds;
}
#endif /* TEST */
