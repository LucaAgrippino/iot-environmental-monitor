/**
 * @file mqtt_client.h
 * @brief MQTT client (Gateway) — MQTT 3.1.1 over TLS 1.2 to AWS IoT Core.
 *
 * Implements IMqttClient and IMqttStats (per components.md). Owns the
 * MQTT protocol (coreMQTT) and the TLS 1.2 session beneath it
 * (mbedTLS). Does not own the Cloud Connectivity state machine or the
 * reconnect timer — those belong to CloudPublisher.
 *
 * MqttClient runs exclusively in CloudPublisherTask context; it has no
 * thread of its own. mqtt_client_process() must be called regularly
 * (recommended: once per 100 ms) to drive the receive path and
 * service keep-alive.
 *
 * @note See docs/lld/middleware/mqtt-client.md for the full design
 *       specification.
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_driver/wifi_driver.h"

/** @brief Opaque handle to an MqttClient instance. */
typedef struct mqtt_client_inst *mqtt_client_handle_t;

typedef enum
{
    MQTT_CLIENT_ERR_OK = 0,
    MQTT_CLIENT_ERR_NOT_INIT = 1,
    MQTT_CLIENT_ERR_NULL_PTR = 2,
    MQTT_CLIENT_ERR_NO_RESOURCE = 3,
    MQTT_CLIENT_ERR_CONNECT_FAIL = 4, /**< TLS or MQTT CONNECT rejected.       */
    MQTT_CLIENT_ERR_PUBLISH_FAIL = 5, /**< Send error or QoS 1 PUBACK timeout. */
    MQTT_CLIENT_ERR_NOT_CONNECTED = 6,
    MQTT_CLIENT_ERR_TLS_FAIL = 7,
    MQTT_CLIENT_ERR_SUBSCRIBE_FAIL = 8, /**< SUBACK with failure code.          */
} mqtt_client_err_t;

typedef enum
{
    MQTT_QOS_0 = 0, /**< Fire-and-forget (telemetry, health).              */
    MQTT_QOS_1 = 1, /**< At-least-once (OTA result, config ACK, alarms).   */
} mqtt_qos_t;

/** @brief MQTT connectivity statistics (Metric Producer Pattern — poll). */
typedef struct
{
    uint32_t publishes_sent;     /**< Total PUBLISH frames transmitted. */
    uint32_t publishes_acked;    /**< QoS 1 PUBACKs received.           */
    uint32_t publish_failures;   /**< Send errors or PUBACK timeouts.   */
    uint32_t connect_attempts;   /**< Total mqtt_client_connect() calls.*/
    uint32_t connect_ok;         /**< Successful connections.           */
    uint32_t reconnect_count;    /**< Connections after the first.      */
    uint32_t subscribe_failures; /**< SUBACK with failure code.         */
} mqtt_stats_t;

/**
 * @brief Callback invoked when an inbound PUBLISH arrives on a subscribed
 *        topic.
 *
 * Called from CloudPublisherTask context (inside mqtt_client_process()).
 * The payload pointer is valid only for the duration of the callback.
 */
typedef void (*mqtt_message_cb_t)(const char *topic, uint16_t topic_len, const uint8_t *payload,
                                  uint32_t payload_len);

/**
 * @brief Callback invoked when the MQTT connection is lost (keep-alive
 *        timeout, TCP error, or MQTT-level error).
 *
 * Called from CloudPublisherTask context. CloudPublisher transitions
 * Machine 3 to Disconnected on receipt.
 */
typedef void (*mqtt_disconnect_cb_t)(void);

/**
 * @brief MQTT connection parameters.
 *
 * Credentials are loaded by LifecycleController from the cert flash
 * partition at boot and passed as pointers. MqttClient stores pointers
 * for the session duration — it does not copy cert material
 * (REQ-NF-302).
 */
typedef struct
{
    const char *broker_endpoint;    /**< e.g. "xxxxx.iot.eu-west-1.amazonaws.com" */
    uint16_t broker_port;           /**< 8883 (MQTT over TLS).                   */
    const char *client_id;          /**< Device serial number.                   */
    const uint8_t *client_cert_der; /**< DER-encoded client certificate.      */
    uint32_t client_cert_len;
    const uint8_t *client_key_der; /**< DER-encoded private key.              */
    uint32_t client_key_len;
    const uint8_t *ca_cert_der; /**< DER-encoded AWS root CA certificate.     */
    uint32_t ca_cert_len;
    uint16_t keep_alive_s; /**< MQTT keep-alive interval (seconds).           */
} mqtt_connect_cfg_t;

/**
 * @brief MqttClient creation configuration.
 */
typedef struct
{
    wifi_handle_t wifi;                 /**< WifiDriver handle (injected). */
    mqtt_message_cb_t msg_cb;           /**< Inbound message callback.     */
    mqtt_disconnect_cb_t disconnect_cb; /**< Connection-loss callback.    */
} mqtt_client_config_t;

/**
 * @brief Create and initialise an MqttClient instance.
 *
 * Registers callbacks and prepares the coreMQTT context. Does NOT
 * connect — connect is a separate call so LifecycleController can gate
 * it on WiFi association.
 *
 * @param[in]  config  Injected dependencies and callbacks.
 * @param[out] handle  Receives the created handle on success.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_NULL_PTR if
 *         config, handle, msg_cb, or disconnect_cb is NULL;
 *         MQTT_CLIENT_ERR_NO_RESOURCE if pool exhausted.
 * @note Threading: task-context only. Call before scheduler starts.
 */
mqtt_client_err_t mqtt_client_create(const mqtt_client_config_t *config,
                                     mqtt_client_handle_t *handle);

/**
 * @brief Establish TLS + MQTT connection to AWS IoT Core.
 *
 * Sequence: open TCP socket -> TLS 1.2 handshake with X.509 mutual
 * auth -> MQTT CONNECT -> await CONNACK -> update stats. On failure at
 * any step: close socket, return error.
 *
 * Blocking. Timeout: MQTT_CONNECT_TIMEOUT_MS (10 s, see MQTT-O2).
 * Called by CloudPublisher when Machine 3 enters Connecting.
 *
 * @param[in] handle  MqttClient handle.
 * @param[in] cfg     Connection parameters (broker, certs, keep-alive).
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_CONNECT_FAIL
 *         on CONNACK rejection; MQTT_CLIENT_ERR_TLS_FAIL on TLS
 *         handshake failure.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_connect(mqtt_client_handle_t handle, const mqtt_connect_cfg_t *cfg);

/**
 * @brief Send MQTT DISCONNECT and close the TLS session.
 *
 * Graceful disconnect — does NOT invoke disconnect_cb. Called by
 * CloudPublisher on controlled shutdown (UC-17).
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_disconnect(mqtt_client_handle_t handle);

/**
 * @brief Publish a message to a topic.
 *
 * QoS 0: fire-and-forget; returns after the frame is handed to
 * WifiDriver. Non-blocking.
 * QoS 1: blocks until PUBACK received or MQTT_PUBACK_TIMEOUT_MS
 * expires (5 s, see MQTT-O4).
 *
 * Returns MQTT_CLIENT_ERR_NOT_CONNECTED immediately if not connected.
 *
 * @param[in] handle   MqttClient handle.
 * @param[in] topic    Null-terminated topic string.
 * @param[in] payload  Message payload.
 * @param[in] len      Payload byte count.
 * @param[in] qos      MQTT_QOS_0 or MQTT_QOS_1.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_PUBLISH_FAIL
 *         on send error or PUBACK timeout.
 * @note Threading: task-context only. QoS 0 non-blocking; QoS 1 blocks
 *       until PUBACK or timeout. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_publish(mqtt_client_handle_t handle, const char *topic,
                                      const uint8_t *payload, uint32_t len, mqtt_qos_t qos);

/**
 * @brief Subscribe to an MQTT topic.
 *
 * Issues MQTT SUBSCRIBE and waits for SUBACK. Inbound messages on the
 * subscribed topic are delivered via the msg_cb registered at creation
 * time.
 *
 * @param[in] handle  MqttClient handle.
 * @param[in] topic   Null-terminated topic filter string.
 * @param[in] qos     Maximum QoS for messages on this subscription.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_SUBSCRIBE_FAIL
 *         if SUBACK contains failure code.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_subscribe(mqtt_client_handle_t handle, const char *topic,
                                        mqtt_qos_t qos);

/**
 * @brief Process inbound MQTT frames and service keep-alive.
 *
 * Must be called regularly from CloudPublisherTask's main loop
 * (recommended: once per 100 ms). Internally calls coreMQTT's
 * MQTT_ProcessLoop(). Delivers inbound messages via msg_cb. Detects
 * keep-alive timeout and invokes disconnect_cb.
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_process(mqtt_client_handle_t handle);

/**
 * @brief Copy current stats snapshot.
 *
 * Polled by CloudPublisher on each health-report cycle (Metric
 * Producer Pattern — counters -> poll). Thread-safe: stats updated
 * only in CloudPublisherTask context. No mutex required.
 *
 * @param[in]  handle     MqttClient handle.
 * @param[out] stats_out  Receives the stats snapshot.
 * @return MQTT_CLIENT_ERR_OK on success.
 */
mqtt_client_err_t mqtt_client_get_stats(mqtt_client_handle_t handle, mqtt_stats_t *stats_out);

/**
 * @brief Reset all counters to zero.
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 */
mqtt_client_err_t mqtt_client_reset_stats(mqtt_client_handle_t handle);

#ifdef TEST
/** @brief Reset all internal static state for unit testing. */
void mqtt_client_reset_for_test(void);
#endif

#endif /* MQTT_CLIENT_H */
