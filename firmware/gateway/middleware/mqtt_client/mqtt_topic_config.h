/**
 * @file mqtt_topic_config.h
 * @brief MQTT topic string assembly for MqttClient (Gateway).
 *
 * Topics are assembled at connect time from the client ID (device serial
 * number) and a fixed per-message suffix, per companion §6:
 *
 *   Publish:
 *     dt/iotmonitor/<client_id>/telemetry
 *     dt/iotmonitor/<client_id>/health
 *     dt/iotmonitor/<client_id>/alarms
 *     dt/iotmonitor/<client_id>/ota/result
 *
 *   Subscribe:
 *     cmd/iotmonitor/<client_id>/config
 *     cmd/iotmonitor/<client_id>/ota
 *
 * "dt/" = device-to-cloud, "cmd/" = cloud-to-device (AWS IoT Core
 * convention). CloudPublisher owns topic assembly (it holds the
 * client_id and calls mqtt_client_publish()/mqtt_client_subscribe()
 * with the fully-built topic string); this header supplies only the
 * fixed prefix/suffix constants so the format stays in one place.
 */

#ifndef MQTT_TOPIC_CONFIG_H
#define MQTT_TOPIC_CONFIG_H

#define MQTT_TOPIC_PREFIX_PUBLISH "dt/iotmonitor/"
#define MQTT_TOPIC_PREFIX_SUBSCRIBE "cmd/iotmonitor/"

#define MQTT_TOPIC_SUFFIX_TELEMETRY "/telemetry"
#define MQTT_TOPIC_SUFFIX_HEALTH "/health"
#define MQTT_TOPIC_SUFFIX_ALARMS "/alarms"
#define MQTT_TOPIC_SUFFIX_OTA_RESULT "/ota/result"

#define MQTT_TOPIC_SUFFIX_CONFIG "/config"
#define MQTT_TOPIC_SUFFIX_OTA "/ota"

/** @brief Longest assembled topic string length, excluding the null
 *         terminator: prefix (15) + client_id (up to 32) + suffix (10). */
#define MQTT_TOPIC_MAX_LEN 64u

#endif /* MQTT_TOPIC_CONFIG_H */
