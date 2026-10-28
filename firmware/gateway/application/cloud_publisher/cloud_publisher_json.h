/**
 * @file cloud_publisher_json.h
 * @brief Prototypes for cloud_publisher_json.c: the wire schema 1.1 (CP-D22) and the /config
 * parser.
 *
 * Not listed in the companion's file layout; added so Ceedling's basename-based auto-link can find
 * cloud_publisher_json.c (it has no other header to match against).
 *
 * Every serialiser returns the bytes written (without the terminator) or 0 if the buffer is too
 * small. Every payload starts with the common block (schema_version, device_id, timestamp,
 * sync_state); timestamp and sync_state come from the time_provider_ts_t passed in.
 */

#ifndef CLOUD_PUBLISHER_JSON_H
#define CLOUD_PUBLISHER_JSON_H

#include <stdbool.h>
#include <stdint.h>

#include "cloud_publisher.h"
#include "update_service/update_service_types.h"

/** Largest number of parameters one /config command may carry (CP-D21). */
#define CP_CFG_MAX_KEYS (8U)

/** Longest key accepted in a /config command, with the terminator. */
#define CP_CFG_KEY_LEN (33U)

/** @brief Telemetry (SD-03a): onboard channels, and the Field Device when @p fd is not NULL. */
uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_snapshot_t *snap,
                                const modbus_fd_reading_t *fd, const time_provider_ts_t *ts,
                                const char *device_serial);

/** @brief Health 1.1 (SD-03b): the snapshot, the firmware version and the stack headroom. */
uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const health_snapshot_t *snap,
                             uint32_t fw_version, const time_provider_ts_t *ts,
                             const char *device_serial);

/** @brief Alarm (SD-05): the common block carries the time of detection (event->timestamp). */
uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const cp_alarm_event_t *event,
                            const char *device_serial);

/** @brief Why a /config command was rejected (CP-D21); the strings are the wire reasons. */
typedef enum
{
    CP_CFG_OK = 0,
    CP_CFG_BAD_JSON,
    CP_CFG_UNKNOWN_KEY,
    CP_CFG_NOT_REMOTE,
    CP_CFG_TOO_MANY_KEYS,
    CP_CFG_INVALID_VALUE,
    CP_CFG_BUSY, /**< Not parsed: the Gateway is not Operational (CP-D27). */
} cp_cfg_status_t;

/** @brief One parameter of a /config command, with the C type of its field. */
typedef struct
{
    const char *key;      /**< Wire name (a string literal of the key table).       */
    config_param_id_t id; /**< ConfigService parameter id.                          */
    uint16_t offset;      /**< offsetof() the field in config_params_t.             */
    uint8_t size;         /**< 2 or 4 bytes.                                        */
    bool is_signed;       /**< int16_t when size is 2 and this is set.              */
    int64_t value;        /**< Parsed value, already range-checked for the type.    */
} cp_cfg_pair_t;

/** @brief A parsed /config command. */
typedef struct
{
    bool req_known;                       /**< "req" was present.                     */
    uint32_t req;                         /**< Echoed in the result.                  */
    uint8_t count;                        /**< Parameters in @c pairs.                */
    cp_cfg_pair_t pairs[CP_CFG_MAX_KEYS]; /**< In message order.                      */
    char bad_key[CP_CFG_KEY_LEN];         /**< Key of the failure, or empty.          */
} cp_cfg_cmd_t;

/**
 * @brief Parse a /config command: one flat JSON object of integers (CP-D21).
 *
 * Only the Tier 2 keys are accepted (REQ-NF-306); the other config_params_t fields are recognised
 * and refused with CP_CFG_NOT_REMOTE. Nested objects, strings, floats, duplicates, trailing data
 * and an empty object are CP_CFG_BAD_JSON. Nothing is applied here.
 */
cp_cfg_status_t cp_parse_config_command(const uint8_t *payload, uint32_t len, cp_cfg_cmd_t *out);

/** @brief Wire reason string of a status, or "bad_json" for CP_CFG_OK. */
const char *cp_cfg_reason_string(cp_cfg_status_t status);

/**
 * @brief Result of a /config command (SD-07): applied, or rejected with a reason and a key.
 * @param[in] req  Echoed when @p req_known.
 * @param[in] key  Included when not NULL.
 */
uint32_t cp_serialise_config_result(char *buf, uint32_t buf_size, bool req_known, uint32_t req,
                                    cp_cfg_status_t status, const char *key,
                                    const time_provider_ts_t *ts, const char *device_serial);

/** @brief Serialise a chunk request (update-service-lld.md 6.2) with the common block. */
uint32_t cp_serialise_ota_request(char *buf, uint32_t buf_size, const us_chunk_request_t *req,
                                  const time_provider_ts_t *ts, const char *device_serial);

/**
 * @brief Serialise a result report (6.4) with the common block.
 *
 * "job" is omitted when the report carries none, "reason" for accepted and success. A report whose
 * version is 0 comes from CloudPublisher itself (a command it rejected before UpdateService saw
 * it) and carries neither "version" nor "rollbacks".
 */
uint32_t cp_serialise_ota_report(char *buf, uint32_t buf_size, const us_ota_report_t *rep,
                                 const time_provider_ts_t *ts, const char *device_serial);

/**
 * @brief Parse an OTA command {"job":N,"size_a":N,"size_b":N} (6.1).
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
