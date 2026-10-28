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

#include <stdbool.h>

#include "cloud_publisher.h"
#include "update_service/update_service_types.h"

uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_snapshot_t *snap,
                                const modbus_poller_fd_reading_t *fd, const char *device_serial);
uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const cp_health_snapshot_t *snap,
                             const char *device_serial);
uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const cp_alarm_event_t *event,
                            const char *device_serial);

/**
 * @brief Serialise a chunk request (update-service-lld.md §6.2).
 * @return Bytes written, or 0 if the buffer is too small.
 */
uint32_t cp_serialise_ota_request(char *buf, uint32_t buf_size, const us_chunk_request_t *req);

/**
 * @brief Serialise a result report (§6.4).
 *
 * "job" is omitted when the report carries none, "reason" for accepted and success. A report whose
 * version is 0 comes from CloudPublisher itself (a command it rejected before UpdateService saw
 * it) and carries neither "version" nor "rollbacks".
 *
 * @return Bytes written, or 0 if the buffer is too small.
 */
uint32_t cp_serialise_ota_report(char *buf, uint32_t buf_size, const us_ota_report_t *rep);

/**
 * @brief Parse an OTA command {"job":N,"size_a":N,"size_b":N} (§6.1).
 *
 * All three keys must be present and be unsigned decimal integers that fit 32 bits; anything
 * else is a malformed command. Range rules (job, sizes) are UpdateService's.
 *
 * @param[out] job_out  Receives "job" when it alone could be read, even if the command is bad;
 *                      @p job_known_out says whether it did. May be NULL.
 */
bool cp_parse_ota_command(const uint8_t *payload, uint32_t len, us_ota_command_t *out,
                          uint32_t *job_out, bool *job_known_out);

#endif /* CLOUD_PUBLISHER_JSON_H */
