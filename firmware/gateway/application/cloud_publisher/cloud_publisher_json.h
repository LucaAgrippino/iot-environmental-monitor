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

/**
 * @brief Telemetry (SD-03a): onboard channels, and the Field Device when @p fd is not NULL.
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param snap Onboard sensor readings.
 * @param fd Field Device reading, or NULL to leave it out.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_snapshot_t *snap,
                                const modbus_fd_reading_t *fd, const time_provider_ts_t *ts,
                                const char *device_serial);

/**
 * @brief Health 1.1 (SD-03b): the snapshot, the firmware version and the stack headroom.
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param snap Health snapshot.
 * @param fw_version Firmware version, packed as major in bits 24 to 31, minor in 16 to 23, patch in
 * 0 to 15.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const health_snapshot_t *snap,
                             uint32_t fw_version, const time_provider_ts_t *ts,
                             const char *device_serial);

/**
 * @brief Alarm (SD-05): the common block carries the time of detection (event->timestamp).
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param event Alarm event.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
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
 *
 * @param payload JSON payload of the command.
 * @param len Length of payload, in bytes.
 * @param out Receives the parsed keys.
 * @return CP_CFG_OK, or the reason the command is rejected.
 */
cp_cfg_status_t cp_parse_config_command(const uint8_t *payload, uint32_t len, cp_cfg_cmd_t *out);

/**
 * @brief Wire reason string of a status, or "bad_json" for CP_CFG_OK.
 *
 * @param status Result of cp_parse_config_command().
 * @return Static string, never NULL.
 */
const char *cp_cfg_reason_string(cp_cfg_status_t status);

/**
 * @brief Result of a /config command (SD-07): applied, or rejected with a reason and a key.
 * @param[in] req  Echoed when @p req_known.
 * @param[in] key  Included when not NULL.
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param req_known True when req holds a request id.
 * @param status Outcome of the command.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
uint32_t cp_serialise_config_result(char *buf, uint32_t buf_size, bool req_known, uint32_t req,
                                    cp_cfg_status_t status, const char *key,
                                    const time_provider_ts_t *ts, const char *device_serial);

/** The /control commands (SRS REQ-DM-010, REQ-SA-170, REQ-NF-107/308). */
typedef enum
{
    CP_CTRL_BAD_FORMAT = 0, /**< Not a JSON object with a string "command". */
    CP_CTRL_UNKNOWN,        /**< Well formed, but the command name is not known. */
    CP_CTRL_READ,           /**< "read": run one more acquisition cycle and publish it. */
    CP_CTRL_RESTART,        /**< "restart": needs a "restart_confirm" within the timeout. */
    CP_CTRL_RESTART_CONFIRM,
    CP_CTRL_RESET_METRICS,
    CP_CTRL_FD_ACK_ALARM, /**< "fd_ack_alarm": CMD_ACK_ALARM on the Field Device (REQ-MB-080). */
    CP_CTRL_FD_RESET_METRICS, /**< "fd_reset_metrics": CMD_RESET_METRICS on the Field Device. */
    CP_CTRL_PROFILE_ADD,      /**< "profile_add": add or replace a device profile (REQ-DM-100). */
    CP_CTRL_PROFILE_REMOVE,   /**< "profile_remove": remove the profile of a slave (REQ-DM-100). */
} cp_control_cmd_t;

/** Members of a "profile_add" / "profile_remove" command; a flag says whether the member was
 * present. */
typedef struct
{
    bool has_slave;  /**< "slave": Modbus address, 1..247. */
    bool has_map;    /**< "map": register map version, not 0. */
    bool has_vendor; /**< "vendor": vendor code, decimal, not 0. */
    uint8_t slave;   /**< Valid when has_slave. */
    uint16_t map;    /**< Valid when has_map. */
    uint16_t vendor; /**< Valid when has_vendor. */
    char desc[33];   /**< "desc": label, up to 32 characters, "" when absent. */
} cp_profile_args_t;

/**
 * @brief Read the members of a profile command:
 * {"command":"profile_add","slave":17,"map":1,"vendor":4660,"desc":"x"}.
 *
 * Presence is reported per member; the range rules are the caller's (they depend on add or remove).
 *
 * @param payload JSON payload of the command.
 * @param len Length of payload, in bytes.
 * @param[out] out The members found.
 * @return false when a member is present but malformed (not a number, over 65535, a label that is
 *         too long or holds a quote or backslash), or the payload is not a JSON object.
 */
bool cp_parse_profile_args(const uint8_t *payload, uint32_t len, cp_profile_args_t *out);

/**
 * @brief Classify a /control payload: {"command":"<name>"[,"req":\<n\>]}.
 * @param[out] req_known  Set when a numeric "req" member is present (may be NULL).
 * @param[out] req        The request id, echoed in the result (may be NULL).
 *
 * @param payload JSON payload of the command.
 * @param len Length of payload, in bytes.
 * @return The command, CP_CTRL_UNKNOWN for an unknown name, or CP_CTRL_BAD_FORMAT.
 */
cp_control_cmd_t cp_parse_control_command(const uint8_t *payload, uint32_t len, bool *req_known,
                                          uint32_t *req);

/**
 * @brief Wire name of a command, "unknown" for CP_CTRL_UNKNOWN and CP_CTRL_BAD_FORMAT.
 *
 * @param cmd Command.
 * @return Static string, never NULL.
 */
const char *cp_control_name(cp_control_cmd_t cmd);

/**
 * @brief Result of a /control command (CP-O8): {"event":"control","command":...,"result":...}.
 * @param[in] accepted  true: "result":"accepted" (executed or queued); false: "rejected".
 * @param[in] reason    Included when not NULL (rejected: "bad_format", "unknown_command",
 *                      "failed").
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param cmd Command the result is about.
 * @param req_known True when req holds a request id.
 * @param req Request id echoed in the result.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
uint32_t cp_serialise_control_result(char *buf, uint32_t buf_size, cp_control_cmd_t cmd,
                                     bool req_known, uint32_t req, bool accepted,
                                     const char *reason, const time_provider_ts_t *ts,
                                     const char *device_serial);

/**
 * @brief Serialise a chunk request (update-service-lld.md 6.2) with the common block.
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param req Chunk request.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
 */
uint32_t cp_serialise_ota_request(char *buf, uint32_t buf_size, const us_chunk_request_t *req,
                                  const time_provider_ts_t *ts, const char *device_serial);

/**
 * @brief Serialise a result report (6.4) with the common block.
 *
 * "job" is omitted when the report carries none, "reason" for accepted and success. A report whose
 * version is 0 comes from CloudPublisher itself (a command it rejected before UpdateService saw
 * it) and carries neither "version" nor "rollbacks".
 *
 * @param buf Output buffer.
 * @param buf_size Size of buf, in bytes.
 * @param rep Result report.
 * @param ts Time of the message.
 * @param device_serial Device serial number (the client ID).
 * @return Number of characters written, or 0 if buf is too small.
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
 *
 * @param payload JSON payload of the command.
 * @param len Length of payload, in bytes.
 * @param out Receives the command when the three keys are valid.
 * @param job_known_out Set when job_out holds a job number.
 * @return true if the payload is a valid OTA command.
 */
bool cp_parse_ota_command(const uint8_t *payload, uint32_t len, us_ota_command_t *out,
                          uint32_t *job_out, bool *job_known_out);

#endif /* CLOUD_PUBLISHER_JSON_H */
