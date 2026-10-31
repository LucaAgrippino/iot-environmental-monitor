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

#include "alarm_service/alarm_service.h"
#include "health_monitor/health_monitor.h"
#include "config_service/iconfig_manager.h"
#include "config_service/iconfig_provider.h"
#include "lifecycle_controller/ilifecycle.h"
#include "modbus_poller/modbus_poller.h"
#include "mqtt_client.h"
#include "sensor_service/sensor_service.h"
#include "store_and_forward/store_and_forward.h"
#include "time_provider/time_provider.h"
#include "time_service/itime_service.h"

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
    CP_ERR_SAF_FULL = 6,       /**< StoreAndForward refused the entry (flash error or too
                                    large): the message is lost (CP-D29).   */
    CP_ERR_BUFFERED = 7,       /**< Not published now; queued in SAF.   */
} cloud_publisher_err_t;

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
    time_provider_ts_t timestamp; /**< Time of detection, from the reading (REQ-AM-040).   */
} cp_alarm_event_t;

/* --- IUpdateService / IOtaTransport (update-service-lld.md CPC-1..6) -- */

#include "update_service/iota_transport.h"
#include "update_service/iupdate_service.h"

/* ===================================================================== */
/* Configuration and public API                                          */
/* ===================================================================== */

/** @brief Handler for the /control command payload (LifecycleController's remote dispatch). */
typedef void (*cp_control_handler_t)(const uint8_t *payload, uint32_t len);

/**
 * @brief CloudPublisher creation configuration (CP-D25).
 *
 * All dependencies are injected. The mandatory ones are mqtt, saf, health, cfg_read, cfg_write
 * and lifecycle; the others may be NULL or 0 (see each member).
 */
typedef struct
{
    mqtt_client_handle_t mqtt;            /**< MqttClient, created but not connected.        */
    saf_handle_t saf;                     /**< StoreAndForward (offline buffer).             */
    health_monitor_handle_t health;       /**< HealthMonitor: snapshot read and reports.     */
    const iconfig_provider_t *cfg_read;   /**< config_service_get_provider().                */
    const iconfig_manager_t *cfg_write;   /**< config_service_get_manager().                 */
    const itime_service_t *time;          /**< NULL: no NTP retry on reconnect.              */
    const ilifecycle_t *lifecycle;        /**< get_state only (CP-D27).                      */
    cp_control_handler_t control_handler; /**< NULL: /control is not subscribed.             */
    uint8_t fd_slave_addr;                /**< 0: no Field Device in the telemetry.          */
    uint32_t fw_version;                  /**< major<<24 | minor<<16 | patch, in the health. */
    const iupdate_service_t *update_ops;  /**< NULL: no OTA; the OTA topics are not subscribed. */
    update_service_t *update;             /**< UpdateService instance (both or neither); may be
                                               update_service_get_instance(), valid before
                                               update_service_create() (US-D15).               */
    mqtt_connect_cfg_t mqtt_connect_cfg;  /**< Broker and certificates; stored by value, the
                                               pointers inside stay caller-owned (CP-D7/D9).  */
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
 * @brief MqttClient's connection-lost callback (CP-D20, CP-O5).
 *
 * The twin of cloud_publisher_on_message(): pass it as mqtt_client_config_t::disconnect_cb. It runs
 * in CloudPublisherTask (inside mqtt_client_process()), records the transition and pushes
 * HEALTH_EVENT_CLOUD_DISCONNECTED. Reconnecting stays with the 1 Hz stats tick (CP-D9, CP-D13).
 * A call before cloud_publisher_create() does nothing.
 */
void cloud_publisher_on_disconnect(void);

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

/** @brief Alarms the REQ-NF-113 seam saw published now / buffered since the reset (CP-D19). */
uint32_t cloud_publisher_alarm_published_for_test(void);
uint32_t cloud_publisher_alarm_buffered_for_test(void);

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
