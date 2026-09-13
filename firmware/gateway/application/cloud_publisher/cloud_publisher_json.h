/**
 * @file cloud_publisher_json.h
 * @brief Prototypes for cloud_publisher_json.c.
 *
 * Not listed in the companion's §16 file layout (which shows only
 * cloud_publisher.h/.c and cloud_publisher_json.c) — added so Ceedling's
 * basename-based auto-link can find cloud_publisher_json.c (it has no
 * other header to match against). Purely a build-mechanics addition; no
 * behaviour or API decision beyond what §5.2 already specifies.
 */

#ifndef CLOUD_PUBLISHER_JSON_H
#define CLOUD_PUBLISHER_JSON_H

#include "cloud_publisher.h"

uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_reading_t *reading,
                                const modbus_poller_fd_reading_t *fd, const char *device_serial);
uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const cp_health_snapshot_t *snap,
                             const char *device_serial);
uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const alarm_event_t *event,
                            const char *device_serial);

#endif /* CLOUD_PUBLISHER_JSON_H */
