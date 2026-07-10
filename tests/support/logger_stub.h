/**
 * @file logger_stub.h
 * @brief Narrow stub declaration for MqttClient test build.
 *
 * Replaces #include "logger.h" in test_mqtt_client.c. Reason: that
 * include causes Ceedling to auto-link the real logger.c, pulling in
 * its FreeRTOS queue/task plumbing. By including a header with no
 * corresponding .c anywhere in the source paths, Ceedling has nothing
 * to auto-link, and the real Logger stays out of the MqttClient test
 * executable.
 *
 * Keep log_level_t and logger_log() IDENTICAL in signature to
 * logger.h — the linker matches by name but the compiler must see
 * consistent types in both test_mqtt_client.c (this file) and
 * mqtt_client.c (the real header).
 */

#ifndef LOGGER_STUB_H
#define LOGGER_STUB_H

typedef enum
{
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

void logger_log(log_level_t level, const char *module, const char *msg);

#endif /* LOGGER_STUB_H */
