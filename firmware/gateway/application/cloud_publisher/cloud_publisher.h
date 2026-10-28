/**
 * @file cloud_publisher.h
 * @brief CloudPublisher (Gateway) — public API.
 *
 * Serialises and publishes telemetry, alarm, and health payloads to AWS
 * IoT Core via MqttClient. Routes inbound MQTT commands to the
 * appropriate handler. Polls MqttClient stats and reports connectivity
 * metrics via IHealthReport. Buffers to StoreAndForward when the cloud
 * connection is unavailable.
 *
 * CloudPublisher PROVIDES nothing upward — it is the top of the stack.
 * The only public entry point is cloud_publisher_create(); all other
 * behaviour is internal to CloudPublisherTask (timers, queues,
 * callbacks).
 *
 * @note See docs/lld/application/cloud-publisher-lld.md for the full
 *       design specification, including open items CP-O1..CP-O6.
 */

#ifndef CLOUD_PUBLISHER_H
#define CLOUD_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>

#include "mqtt_client.h"
#include "alarm_service/alarm_service.h"
#include "sensor_service/sensor_service.h"

/** @brief Opaque handle to a CloudPublisher instance. */
typedef struct cloud_publisher_inst *cloud_publisher_handle_t;

typedef enum
{
    CP_ERR_OK = 0,
    CP_ERR_NOT_INIT = 1,
    CP_ERR_NULL_PTR = 2,
    CP_ERR_NO_RESOURCE = 3,
    CP_ERR_SERIALISE = 4,      /**< JSON payload exceeded buffer.       */
    CP_ERR_PUBLISH_FAILED = 5, /**< MQTT publish failed; routed to SAF. */
    CP_ERR_SAF_FULL = 6,       /**< Store-and-forward buffer full.      */
    CP_ERR_BUFFERED = 7,       /**< Not published now; queued in SAF.   */
} cloud_publisher_err_t;

/* ===================================================================== */
/* Placeholder dependency types (CP-O1, CP-O2, F-03)                     */
/*                                                                        */
/* The opaque handles, data structures and function prototypes below     */
/* stand in for GW peer modules that do not yet have a firmware/gateway   */
/* implementation (ISensorService, IAlarmService, IModbusPoller,         */
/* IStoreAndForward, IHealthSnapshot/IHealthReport, IConfigProvider/      */
/* IConfigManager, IUpdateService, ILifecycle). Each type is guarded so   */
/* the real header can supersede it without a duplicate-definition error  */
/* once that module's own LLD is implemented for GW. Only the symbols    */
/* CloudPublisher actually calls are declared here — see the reuse       */
/* policy in the session prompt for the same convention applied to test  */
/* stubs.                                                                */
/* ===================================================================== */

/* --- IModbusPoller (placeholder; see docs/lld/middleware/modbus-master-poller.md) */

#ifndef MODBUS_POLLER_HANDLE_T_DEFINED
#define MODBUS_POLLER_HANDLE_T_DEFINED
typedef struct modbus_poller_inst *modbus_poller_handle_t;
#endif /* MODBUS_POLLER_HANDLE_T_DEFINED */

#ifndef MODBUS_POLLER_FD_READING_T_DEFINED
#define MODBUS_POLLER_FD_READING_T_DEFINED
/** @brief Latest Field Device reading relayed over Modbus (§7.1). */
typedef struct
{
    int16_t temp_deci_c;
    uint8_t humidity_pct;
    uint16_t pressure_hpa;
    bool valid; /**< False if the Field Device is offline (REQ-SA-160). */
} modbus_poller_fd_reading_t;
#endif /* MODBUS_POLLER_FD_READING_T_DEFINED */

/** @brief Placeholder — returns the latest cached Field Device reading. */
modbus_poller_fd_reading_t modbus_poller_get_latest_fd(modbus_poller_handle_t handle);

/* --- Alarm payload (CloudPublisher's own wire model) ------------------ */

#ifndef CP_ALARM_TYPE_T_DEFINED
#define CP_ALARM_TYPE_T_DEFINED
typedef enum
{
    CP_ALARM_TYPE_HIGH = 0,
    CP_ALARM_TYPE_LOW = 1,
    CP_ALARM_TYPE_CLEAR = 2,
} cp_alarm_type_t;
#endif /* CP_ALARM_TYPE_T_DEFINED */

#ifndef CP_ALARM_SOURCE_T_DEFINED
#define CP_ALARM_SOURCE_T_DEFINED
typedef enum
{
    CP_ALARM_SOURCE_GATEWAY = 0,
    CP_ALARM_SOURCE_FIELD_DEVICE = 1,
} cp_alarm_source_t;
#endif /* CP_ALARM_SOURCE_T_DEFINED */

#define CP_ALARM_SENSOR_NAME_MAX 16u

/**
 * @brief One alarm transition as published to the cloud (companion §7.3).
 *
 * Built in the AlarmService subscriber callback from the real sensor_id_t /
 * alarm_event_t / sensor_reading_t triple. Values are in SensorService fixed-point
 * units (0.01 degC, 0.01 %RH, 0.1 hPa), the same units the thresholds use.
 */
typedef struct
{
    char sensor_name[CP_ALARM_SENSOR_NAME_MAX];
    cp_alarm_type_t alarm_type;
    int32_t measured_value_raw;
    int32_t threshold_value_raw;
    cp_alarm_source_t source;
} cp_alarm_event_t;

/* --- IStoreAndForward (placeholder; see store-and-forward-lld.md) ----- */

#ifndef STORE_AND_FORWARD_HANDLE_T_DEFINED
#define STORE_AND_FORWARD_HANDLE_T_DEFINED
typedef struct store_and_forward_inst *store_and_forward_handle_t;
#endif /* STORE_AND_FORWARD_HANDLE_T_DEFINED */

#ifndef SAF_ERR_T_DEFINED
#define SAF_ERR_T_DEFINED
typedef enum
{
    SAF_ERR_OK = 0,
    SAF_ERR_FULL = 1,
    SAF_ERR_EMPTY = 2,
    SAF_ERR_NULL_PTR = 3,
} saf_err_t;
#endif /* SAF_ERR_T_DEFINED */

#define SAF_TOPIC_MAX_LEN 64u
#define SAF_ENTRY_MAX_LEN 512u

#ifndef SAF_ENTRY_T_DEFINED
#define SAF_ENTRY_T_DEFINED
typedef struct
{
    char topic[SAF_TOPIC_MAX_LEN];
    uint8_t payload[SAF_ENTRY_MAX_LEN];
    uint32_t len;
    mqtt_qos_t qos;
} saf_entry_t;
#endif /* SAF_ENTRY_T_DEFINED */

saf_err_t store_and_forward_enqueue(store_and_forward_handle_t handle, const char *topic,
                                    const uint8_t *buf, uint32_t len, mqtt_qos_t qos);
saf_err_t store_and_forward_dequeue(store_and_forward_handle_t handle, saf_entry_t *entry_out);
saf_err_t store_and_forward_confirm(store_and_forward_handle_t handle);

/* --- IHealthSnapshot / IHealthReport (placeholder; see health-monitor.md) */

#ifndef HEALTH_MONITOR_HANDLE_T_DEFINED
#define HEALTH_MONITOR_HANDLE_T_DEFINED
typedef struct health_monitor_inst *health_monitor_handle_t;
#endif /* HEALTH_MONITOR_HANDLE_T_DEFINED */

#ifndef CP_HEALTH_SNAPSHOT_T_DEFINED
#define CP_HEALTH_SNAPSHOT_T_DEFINED
/**
 * @brief Minimal placeholder subset of IHealthSnapshot's
 * device_health_snapshot_t. Full schema per health-monitor.md (CP-O_health);
 * only the fields CloudPublisher's health payload needs are modelled here.
 */
typedef struct
{
    uint32_t uptime_s;
    bool cloud_connected;
    uint32_t mqtt_reconnect_count;
    uint32_t buffer_entry_count;
} cp_health_snapshot_t;
#endif /* CP_HEALTH_SNAPSHOT_T_DEFINED */

cp_health_snapshot_t health_snapshot_get(health_monitor_handle_t handle);
void health_report_update_mqtt(health_monitor_handle_t handle, const mqtt_stats_t *stats,
                               const mqtt_stats_t *last_stats);

#ifndef CP_HEALTH_EVENT_T_DEFINED
#define CP_HEALTH_EVENT_T_DEFINED
/** @brief Placeholder subset of health_event_t — only the event CloudPublisher raises. */
typedef enum
{
    CP_HEALTH_EVENT_SAF_FULL = 0,
    CP_HEALTH_EVENT_ALARM_PUBLISHED = 1, /**< Alarm went out on the wire now.   */
    CP_HEALTH_EVENT_ALARM_BUFFERED = 2,  /**< Alarm queued in SAF (not connected
                                              or publish failed); REQ-NF-113's
                                              clock keeps running until drain. */
} cp_health_event_t;
#endif /* CP_HEALTH_EVENT_T_DEFINED */

void health_report_push_event(health_monitor_handle_t handle, cp_health_event_t event);

/* --- IConfigProvider / IConfigManager (placeholder; see config-service.md) */

#ifndef CONFIG_SERVICE_HANDLE_T_DEFINED
#define CONFIG_SERVICE_HANDLE_T_DEFINED
typedef struct config_service_inst *config_service_handle_t;
#endif /* CONFIG_SERVICE_HANDLE_T_DEFINED */

uint32_t config_provider_get_telemetry_interval_s(config_service_handle_t handle);
uint32_t config_provider_get_health_interval_s(config_service_handle_t handle);
bool config_manager_apply(config_service_handle_t handle, const uint8_t *payload, uint32_t len);

/* --- IUpdateService / IOtaTransport (update-service-lld.md CPC-1..6) -- */

#include "update_service/iota_transport.h"
#include "update_service/iupdate_service.h"

/* --- ILifecycle (placeholder; see lifecycle-controller.md) ------------ */

#ifndef LIFECYCLE_HANDLE_T_DEFINED
#define LIFECYCLE_HANDLE_T_DEFINED
typedef struct lifecycle_inst *lifecycle_handle_t;
#endif /* LIFECYCLE_HANDLE_T_DEFINED */

void lifecycle_handle_remote(lifecycle_handle_t handle, const uint8_t *payload, uint32_t len);

/* ===================================================================== */
/* Configuration and public API                                          */
/* ===================================================================== */

/**
 * @brief CloudPublisher creation configuration.
 *
 * All dependencies injected via opaque handles (Gateway ADT default).
 */
typedef struct
{
    mqtt_client_handle_t mqtt;
    modbus_poller_handle_t poller;        /**< IModbusPoller — FD readings.         */
    store_and_forward_handle_t saf;       /**< IStoreAndForward — offline buffer.   */
    health_monitor_handle_t health_read;  /**< IHealthSnapshot — health payload.    */
    health_monitor_handle_t health_write; /**< IHealthReport — MQTT stats push.     */
    config_service_handle_t cfg_read;     /**< IConfigProvider — publish intervals. */
    config_service_handle_t cfg_write;    /**< IConfigManager — remote config cmds. */
    const iupdate_service_t *update_ops;  /**< IUpdateService; NULL for a Gateway without OTA (the
                                               OTA topics are then not subscribed). */
    update_service_t *update;             /**< UpdateService instance, already created.      */
    lifecycle_handle_t lifecycle;         /**< ILifecycle — restart command.        */
    mqtt_connect_cfg_t mqtt_connect_cfg; /**< Broker/certs — CloudPublisher owns connect/reconnect
                                               (CP-D7/CP-D9); stored by value, pointers inside
                                               remain caller-owned (matches mqtt_client_connect()'s
                                               own convention). */
} cloud_publisher_config_t;

/**
 * @brief Create and initialise a CloudPublisher instance.
 *
 * Stores all dependency handles, creates the alarm queue (8 entries),
 * command queue (4 entries), FreeRTOS software timers (telemetry 60 s,
 * health 600 s, stats 1 Hz), registers the alarm callback, reads the MCU
 * unique ID as the device serial, and creates CloudPublisherTask. Does
 * NOT attempt the initial MQTT connect itself (that would block this
 * pre-scheduler call for up to MQTT_CONNECT_TIMEOUT_MS) — the task's own
 * 1 Hz stats tick attempts it on its first iteration, using the same
 * connect/reconnect logic as any later drop (CP-D9).
 *
 * @param[in]  config  Injected dependencies.
 * @param[out] handle  Receives the created handle on success.
 * @return CP_ERR_OK on success; CP_ERR_NULL_PTR if config or handle is
 *         NULL; CP_ERR_NO_RESOURCE if pool exhausted.
 * @note Threading: call before scheduler starts.
 */
cloud_publisher_err_t cloud_publisher_create(const cloud_publisher_config_t *config,
                                             cloud_publisher_handle_t *handle);

/**
 * @brief Report whether the instance exists and its task is running.
 *
 * LifecycleController's SelfChecking probe (REQ-DM-040). "Ready" means able to accept messages, not
 * connected: while the broker is unreachable CloudPublisher buffers in StoreAndForward, so an
 * offline start is a normal state and must not fault the boot (LC-O7).
 *
 * @param[in] handle  Handle from cloud_publisher_create().
 * @return true if @p handle is a created instance; false for NULL or an unused handle.
 * @note Threading: any task.
 */
bool cloud_publisher_is_ready(cloud_publisher_handle_t handle);

/**
 * @brief Ask CloudPublisherTask to hand over everything that is waiting (CP-D16).
 *
 * Asynchronous: it posts a request and returns. The task publishes the pending alarms and, if the
 * broker is connected, drains the offline buffer. Anything it cannot send stays in the flash-backed
 * buffer. Called before a controlled restart, which waits for a confirmation, so the task has time.
 *
 * @param[in] handle  Handle from cloud_publisher_create().
 * @return CP_ERR_OK if the request was posted; CP_ERR_NULL_PTR; CP_ERR_NOT_INIT if the instance is
 *         not created; CP_ERR_NO_RESOURCE if the notification could not be posted.
 * @note Threading: task context. Not ISR-safe.
 */
cloud_publisher_err_t cloud_publisher_flush(cloud_publisher_handle_t handle);

/**
 * @brief MqttClient's inbound-message callback (CPC-3, CP-O5).
 *
 * mqtt_client_create() takes its msg_cb before CloudPublisher exists, and the callback carries no
 * context, so this function reaches the instance through the module's single pool entry. Pass it
 * as mqtt_client_config_t::msg_cb. Called from CloudPublisherTask inside mqtt_client_process().
 * Messages that arrive before cloud_publisher_create() are dropped.
 *
 * The OTA chunk topic is handled here and now (a 1 KB payload does not fit the 128-byte command
 * queue, and UpdateService copies it in microseconds); every other command is queued for the task.
 */
void cloud_publisher_on_message(const char *topic, uint16_t topic_len, const uint8_t *payload,
                                uint32_t payload_len);

/**
 * @brief Fill the IOtaTransport that carries UpdateService's chunk requests and reports (CPC-5).
 *
 * Requests go out at QoS 0 and only while the broker is connected; they are never buffered. Reports
 * go out at QoS 1 on /ota/result through the normal StoreAndForward gate. Both are queued (4
 * entries) and sent by CloudPublisherTask, so the calls never block.
 *
 * @param[in]  handle  Handle from cloud_publisher_create().
 * @param[out] out     Receives the transport; its ctx is @p handle.
 * @return CP_ERR_OK; CP_ERR_NULL_PTR; CP_ERR_NOT_INIT if the instance is not created.
 */
cloud_publisher_err_t cloud_publisher_get_ota_transport(cloud_publisher_handle_t handle,
                                                        iota_transport_t *out);

#ifdef TEST

/** @brief Reset all internal static state for unit testing. */
void cloud_publisher_reset_for_test(void);

/** @brief Runs exactly one iteration of the task body (single-step testing). */
void cloud_publisher_task_step_for_test(cloud_publisher_handle_t handle);

/**
 * @brief Test-only injection of an alarm event, as if delivered by the
 * IAlarmService subscriber callback (CP-O5: real wiring pending
 * AlarmService(GW) implementation).
 */
void cloud_publisher_inject_alarm_for_test(cloud_publisher_handle_t handle,
                                           const cp_alarm_event_t *event);

/**
 * @brief Test-only injection of an inbound MQTT command, as if delivered
 * by MqttClient's msg_cb (CP-O5: real wiring pending — MqttClient's
 * msg_cb/disconnect_cb are registered at mqtt_client_create() time,
 * upstream of CloudPublisher's config injection).
 */
void cloud_publisher_inject_command_for_test(cloud_publisher_handle_t handle, const char *topic,
                                             const uint8_t *payload, uint32_t len);

/** @brief Size of one outbound OTA queue entry, so a test can enable the mock's item capture. */
uint32_t cloud_publisher_ota_entry_size_for_test(void);

#endif /* TEST */

#endif /* CLOUD_PUBLISHER_H */
