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

/* --- ISensorService (placeholder; see sensor-alarm-service.md) -------- */

#ifndef SENSOR_SERVICE_HANDLE_T_DEFINED
#define SENSOR_SERVICE_HANDLE_T_DEFINED
typedef struct sensor_service_inst *sensor_service_handle_t;
#endif /* SENSOR_SERVICE_HANDLE_T_DEFINED */

#ifndef SENSOR_READING_T_DEFINED
#define SENSOR_READING_T_DEFINED
/** @brief Latest onboard sensor reading (fields per companion §7.1). */
typedef struct
{
    int16_t temperature_deci_c;
    uint8_t humidity_pct;
    uint16_t pressure_hpa;
    int16_t accel_x_mg;
    int16_t accel_y_mg;
    int16_t accel_z_mg;
    int16_t gyro_x_mdps;
    int16_t gyro_y_mdps;
    int16_t gyro_z_mdps;
    int16_t mag_x_mgauss;
    int16_t mag_y_mgauss;
    int16_t mag_z_mgauss;
    bool time_synchronised; /**< Drives JSON "sync_state" (§7.1). */
} sensor_reading_t;
#endif /* SENSOR_READING_T_DEFINED */

/** @brief Placeholder — returns the latest cached onboard sensor reading. */
sensor_reading_t sensor_service_get_latest(sensor_service_handle_t handle);

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

/* --- IAlarmService (placeholder; see sensor-alarm-service.md) --------- */

#ifndef ALARM_SERVICE_HANDLE_T_DEFINED
#define ALARM_SERVICE_HANDLE_T_DEFINED
typedef struct alarm_service_inst *alarm_service_handle_t;
#endif /* ALARM_SERVICE_HANDLE_T_DEFINED */

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

#ifndef ALARM_EVENT_T_DEFINED
#define ALARM_EVENT_T_DEFINED
/** @brief Alarm event payload (fields per companion §7.3). */
typedef struct
{
    char sensor_name[CP_ALARM_SENSOR_NAME_MAX];
    cp_alarm_type_t alarm_type;
    int32_t measured_value_raw;
    int32_t threshold_value_raw;
    cp_alarm_source_t source;
} alarm_event_t;
#endif /* ALARM_EVENT_T_DEFINED */

/** @brief Alarm subscriber callback — runs in SensorTask context; must not block. */
typedef void (*alarm_event_cb_t)(const alarm_event_t *event, void *ctx);

/** @brief Placeholder — registers cb, invoked whenever an alarm changes state. */
void alarm_service_subscribe(alarm_service_handle_t handle, alarm_event_cb_t cb, void *ctx);

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

/* --- IUpdateService (placeholder; see update-service-lld.md, CP-O1) --- */

#ifndef UPDATE_SERVICE_HANDLE_T_DEFINED
#define UPDATE_SERVICE_HANDLE_T_DEFINED
typedef struct update_service_inst *update_service_handle_t;
#endif /* UPDATE_SERVICE_HANDLE_T_DEFINED */

void update_service_handle_command(update_service_handle_t handle, const uint8_t *payload,
                                   uint32_t len);

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
    sensor_service_handle_t sensors;      /**< ISensorService — latest readings.    */
    alarm_service_handle_t alarms;        /**< IAlarmService — alarm subscription.  */
    modbus_poller_handle_t poller;        /**< IModbusPoller — FD readings.         */
    store_and_forward_handle_t saf;       /**< IStoreAndForward — offline buffer.   */
    health_monitor_handle_t health_read;  /**< IHealthSnapshot — health payload.    */
    health_monitor_handle_t health_write; /**< IHealthReport — MQTT stats push.     */
    config_service_handle_t cfg_read;     /**< IConfigProvider — publish intervals. */
    config_service_handle_t cfg_write;    /**< IConfigManager — remote config cmds. */
    update_service_handle_t update_svc;   /**< May be NULL until UpdateService LLD (CP-O1). */
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
                                           const alarm_event_t *event);

/**
 * @brief Test-only injection of an inbound MQTT command, as if delivered
 * by MqttClient's msg_cb (CP-O5: real wiring pending — MqttClient's
 * msg_cb/disconnect_cb are registered at mqtt_client_create() time,
 * upstream of CloudPublisher's config injection; see deviations in
 * docs/dev-tools/cloud_publisher/session-report.md).
 */
void cloud_publisher_inject_command_for_test(cloud_publisher_handle_t handle, const char *topic,
                                             const uint8_t *payload, uint32_t len);

#endif /* TEST */

#endif /* CLOUD_PUBLISHER_H */
