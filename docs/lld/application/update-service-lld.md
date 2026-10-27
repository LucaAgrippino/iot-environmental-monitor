# LLD Companion — UpdateService

**Board:** Gateway (GW) only
**Layer:** Application

This artefact specifies the software design of `UpdateService`: the OTA
update policy of the Gateway. It covers command intake, chunked download
through CloudPublisher, the use of FirmwareStore to stage, verify, install
and activate the image, the post-update self-check, and the reporting of
every outcome to the cloud.

**Version:** 1.0
**Date:** October 2026
**Status:** Released — implemented and host-tested (70 cases). The bench harness is
written and compile-checked but has not been run on the board (US-O9). The system test is
gated by the items in §4.3, and the HLD amendments B1–B4 and changes LCC-2..4 / CPC-1..6 are
not part of this release (US-O8)

**HLD anchor:** UpdateService in `components.md` (GW application layer);
`state-machines.md` Machine 3 (owner) and Machine 1 (UpdatingFirmware,
I18, T19); `sequence-diagrams.md` SD-00c, SD-06a–d.

**Changes from v0.1:** full rewrite against the as-built FirmwareStore v1.0,
LifecycleController, CloudPublisher and MqttClient. v0.1 could not be
implemented as written:
- it called `IMqttClient` directly, but MqttClient runs only in
  CloudPublisherTask and is not thread-safe;
- it expected the image digest and signature in the command, but they are
  in the signed image header (FS-D4);
- it relied on a FirmwareStore API and pending flags that no longer exist
  (`apply`, `mark_inactive`, `set_pending_flag`), and the ~500 ms apply
  estimate was wrong (the measured install is 11.96 s, FS-O1);
- it added an UpdateService NVM region, which is not needed: FirmwareStore
  holds all non-volatile OTA state;
- it gave a priority (4) that contradicts `task-breakdown.md` (1);
- it had no way to leave UpdatingFirmware after a failed download;
- it made false principle claims and listed SD-trace functions that did
  not exist.

Decisions are logged in §24.

---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `IUpdateService` (consumer: CloudPublisher) · `IUpdateControl` (consumer: LifecycleController, through its `lc_update_service_if_t` adapter) |
| **Owns (implemented by others)** | `IOtaTransport`, implemented by CloudPublisher (§7.4) |
| **Uses** | `IFirmwareImageStore`, `IFirmwareBootControl` (FirmwareStore v1.0) · `ILifecycle` (`post_event`, `get_state`) · `IOtaTransport` · self-check probes (`us_probe_if_t`: SensorService and ModbusPoller readiness) · Logger (`LOG_*` macros, as FS-D15) · FreeRTOS |
| **Does not use** | MqttClient, ResetDriver, IHealthReport (see US-D2, US-D4, US-D10) |
| **Hosted in task** | `UpdateServiceTask`, priority 1, 1024 words / 4 KB, static (`task-breakdown.md` D23) |
| **Activation** | Task notifications only (§9.3). Does **not** wait on the LifecycleController start gate (§17) |
| **Instances** | One (GW ADT convention, static pool of size 1) |

---

## 2. Traceability

| Concern | SRS / UC | Where |
|---|---|---|
| Download the image | REQ-DM-050, UC-18 step 3 | §10 |
| Resume after interruption | REQ-DM-051, UC-18 E2 | §10.3, §10.5 |
| Delete the partial download after three retries | REQ-DM-052 | §10.4 |
| Log download failure | REQ-DM-053 | §19 |
| Reject a new command while one is in progress | REQ-DM-054 | §8.2 |
| Report every result to the cloud | REQ-DM-055, UC-18 steps 9 / rainy 7 | §7.4, §14 |
| Accept commands only from an authenticated source | REQ-DM-056 | §15 |
| Validate the image, then discard it on failure; log | REQ-DM-060, -061, -062, UC-20 | §11 |
| Set the new slot as boot slot | REQ-DM-070, UC-18 step 6 | §12 |
| Self-check on the new firmware | REQ-DM-071, REQ-DM-040, UC-18 step 8 | §13 |
| Roll back if the self-check fails | REQ-DM-072, UC-18 E3 | §13.3 |
| Keep the current firmware until the self-check passes | REQ-DM-073 | §12, §13 (FirmwareStore trial states) |
| Verify the signature before applying | REQ-DM-080, REQ-NF-304 | §11 |
| Roll back within 10 s | REQ-NF-204 | §18 |
| Begin a remote command within 500 ms | REQ-NF-107 | §18 |
| Discard failure: log and keep the current firmware | UC-18 E4 | §19 |
| No dynamic allocation after init | REQ-NF-408 | §9 (FS-O4 inherited, US-O6) |

---

## 3. Responsibility and scope

`UpdateService` owns the **policy** of an OTA update (Machine 3): when to
start, how to fetch, when to retry or give up, when to activate, when to
confirm, and what to report. `FirmwareStore` owns the **mechanics**:
staging, authentication, installation and boot metadata.
`LifecycleController` owns the **system mode** (Machine 1) and the reset
(I18). `CloudPublisher` owns **MQTT and serialisation**.

**It does:**
- validate an OTA command and ask LifecycleController to enter UpdatingFirmware;
- fetch the image chunk by chunk (request/response) for the slot that is not running;
- drive FirmwareStore: `resume`/`begin`, `write_chunk`, `verify`, `install_begin`, the `install_step` loop, `activate`, `discard`;
- run the post-update self-check and confirm the new image;
- report accepted / rejected / failed / success / rolled-back to the cloud.

**It does not:**
- touch MQTT, JSON or topics (CloudPublisher, through `IOtaTransport`);
- reset the MCU (LifecycleController, Machine 1 I18);
- store anything in flash itself (FirmwareStore holds all non-volatile OTA state);
- authenticate the image (FirmwareStore, FS-D4).

**Non-goals** (not in the SRS, P6): cancelling an update in progress,
progress reports, version anti-rollback, delta updates, Field Device OTA
(D35), automatic resume after a reboot without a new command (§10.5).

---

## 4. Prerequisites and required changes

### 4.1 HLD amendments (model first)

| ID | Artefact | Change | Why |
|---|---|---|---|
| **B1** | `components.md` UpdateService | USES `IFirmwareImageStore`, `IFirmwareBootControl`, `IOtaTransport`, `ILifecycle`, `ISensorService` / `IModbusPoller` readiness, Logger. **Remove** MqttClient and ResetDriver. PROVIDES `IUpdateService` + `IUpdateControl` (P3) | MqttClient is CloudPublisherTask-only (mqtt_client.h); the reset is LifecycleController's (Machine 1 I18) |
| **B2** | `components.md` CloudPublisher | PROVIDES `IOtaTransport` (implementation of an interface owned by UpdateService); USES `IUpdateService` (closes F-03) | CloudPublisher is the only MQTT user |
| **B3** | `state-machines.md` Machine 1 | T18 replaced: UpdatingFirmware has no "success" exit. Success is decided after the reboot (Init.SelfChecking). I18 is triggered by `LC_EVENT_OTA_RESET_REQUESTED`. T19/T20 are triggered by `LC_EVENT_OTA_FINISHED`. The cloud report in T18–T20 moves to UpdateService | With trial boot (FS-D2), success is only known on the new image. SD-06a–d already have UpdateService as the reporter; Machine 1 and SD-06 disagreed |
| **B4** | `sequence-diagrams.md` SD-06a msg 4, SD-06b msgs 1–7, SD-06c msg 8, SD-06d msgs 1–12, 18'–19' | Command goes CloudPublisher → UpdateService → LifecycleController (`OTA_REQUESTED`) → UpdateService (`start`). Chunks are requested by UpdateService (§10). Reset after `activate` goes through LifecycleController (I18). Confirm comes before the success report | Matches the implementation and the existing LifecycleController code |

### 4.2 Changes in other modules (same release as UpdateService)

**LifecycleController (one PR, LC-O6 close-out):**

| ID | Change |
|---|---|
| LCC-1 | Add `LC_EVENT_OTA_RESET_REQUESTED = 11` and `LC_EVENT_OTA_FINISHED = 12` to `ilifecycle.h` (appended; existing values unchanged) |
| LCC-2 | In UPDATING_FW: `OTA_FINISHED` → Operational (Machine 1 T19). `OTA_RESET_REQUESTED` → `cloud->flush()`, wait `LC_OTA_RESET_GRACE_MS` (2000 ms, timer or delay), then `reset_driver->soft_reset()` (I18). Both events are ignored with a WARN in any other state |
| LCC-3 | Rollback boot path: remove the `cloud->report_rollback_result()` call; UpdateService reports (US-D9). Remove `report_rollback_result` from `lc_cloud_if_t` |
| LCC-4 | `lifecycle_wiring`: replace the UpdateService and FirmwareStore stand-ins with adapters (§7.5). Update IT-LC-012: a rejected image now ends in **Operational** (Machine 1 T19), not Faulted. LC-D10 is kept: a refused `start()` is still a programming error and faults |

**CloudPublisher (one PR, together with CP-O5 and the OTA part of LC-O13):**

| ID | Change |
|---|---|
| CPC-1 | Subscribe to `cmd/iotmonitor/<id>/ota` (QoS 1) and `cmd/iotmonitor/<id>/ota/data` (QoS 0: a lost chunk is re-requested, and QoS 1 redelivery after a reconnect would only produce stale chunks) after every successful connect (part of CP-O5) |
| CPC-2 | Route by **exact suffix**, `/ota/data` before `/ota`. Today `strstr("/ota")` would also match `/ota/data` |
| CPC-3 | `/ota/data` is **not** queued in the 128-byte command queue: from `msg_cb`, parse the 8-byte header and call `on_chunk()` directly (it copies and returns in microseconds, §9.4) |
| CPC-4 | `/ota`: parse the JSON command (§6.1) into `us_ota_command_t` and call `handle_command()`. A malformed command is rejected by CloudPublisher with `reason:"bad_command"` (UC-19 E2) |
| CPC-5 | Implement `IOtaTransport` (§7.4): a 4-entry static outbound queue plus notify bit 8 into CloudPublisherTask. Requests go out at QoS 0 **only if connected, never through StoreAndForward**. Reports go out at QoS 1 on `/ota/result` **through the normal StoreAndForward gate**. `is_connected` reads a volatile mirror of `mqtt_client_is_connected()` that CloudPublisherTask updates |
| CPC-6 | Remove `cloud_publisher_report_rollback_result()` and notify bit 7 (superseded by CPC-5; CP-D16 amended). Delete the `update_service_handle_command` placeholder |

**FirmwareStore:** no change. **MqttClient:** no change. MQTT-O3 closes:
the largest inbound OTA message is 1032 B of payload plus a topic of about
50 B, well inside `MQTT_PKT_BUF_SIZE` (4096 B).

### 4.3 What gates which test level

| Level | Needs | Status |
|---|---|---|
| Host unit tests | This LLD, FirmwareStore headers | Ready |
| Bench harness (§23.1) | FirmwareStore on target (done), Slot-A build and boot stub (done, FS-O12) | Ready |
| System test over MQTT (§23.2) | CPC-1..6, LCC-1..4, Gateway `main()` (LC-O15), bootloader (FS-O7), `scripts/ota_server.py` | Open |

---

## 5. Machine 3 mapping

| Machine 3 state | Implementation | Notes |
|---|---|---|
| Idle | `US_ST_IDLE` | FirmwareStore boot state CONFIRMED |
| *(Machine 1 T14, waiting for LifecycleController)* | `US_ST_AWAITING_START` | Command accepted locally; `OTA_REQUESTED` posted |
| Downloading | `US_ST_DOWNLOADING` | §10 |
| Validating | `US_ST_VALIDATING` | §11 |
| Applying | `US_ST_INSTALLING`, then `US_ST_ACTIVATED` | `install_*` loop, then `activate`; T7 action = post `OTA_RESET_REQUESTED` |
| SelfChecking | `US_ST_SELF_CHECKING` | Entered by `resume_self_checking()` in Init (T8) |
| RollingBack | `US_ST_ROLLING_BACK` | Entered by `resume_rollback()`; LifecycleController writes the metadata and resets (T11) |
| Committed | transient → `US_ST_IDLE` | Confirm, report success, discard staging, post `SELF_CHECK_PASS` |
| Failed | transient → `US_ST_IDLE` | Discard staging, report failure, post `OTA_FINISHED` (before the reboot), or report rolled-back and acknowledge (after the reboot, T12) |

Idle entry "clear update flags" (Machine 3) needs no action: the trial
states replace the flags (FS-D2).

---

## 6. Wire contract (cloud ↔ device)

CloudPublisher serialises and parses (`components.md` CloudPublisher
responsibility); the formats below are the contract between the cloud tool
and CloudPublisher. UpdateService sees only the C structs of §8.
`<id>` is the device serial (client id). All integers are decimal in JSON
and little-endian in binary.

### 6.1 Command — `cmd/iotmonitor/<id>/ota` (JSON, ≤ 128 B)

```json
{"job":42,"size_a":491648,"size_b":491600}
```

| Field | Rule |
|---|---|
| `job` | 1 … 0xFFFFFFFE. It is also the FirmwareStore session id (`image_id`) |
| `size_a`, `size_b` | Length in bytes of the signed image file (header + body) linked for Slot A and Slot B. Both are needed: the device chooses (FS-D3) |

### 6.2 Chunk request — `dt/iotmonitor/<id>/ota/request` (JSON, QoS 0, never buffered)

```json
{"job":42,"slot":"B","offset":4096,"len":1024}
```

### 6.3 Chunk data — `cmd/iotmonitor/<id>/ota/data` (binary)

| Bytes | Content |
|---|---|
| 0–3 | `job` (u32 LE) |
| 4–7 | `offset` (u32 LE) |
| 8… | 1 … 1024 bytes of the image file for the requested slot, starting at `offset` |

### 6.4 Result — `dt/iotmonitor/<id>/ota/result` (JSON, QoS 1, buffered when offline)

```json
{"event":"ota","job":42,"result":"failed","reason":"hash_mismatch","version":"1.2.0","rollbacks":0}
```

| Field | Rule |
|---|---|
| `job` | Omitted when not known (reports after a reboot, §13.4) |
| `result` | `accepted`, `rejected`, `failed`, `success`, `rolled_back` |
| `reason` | §8.4 string; omitted for `accepted` and `success` |
| `version` | Running firmware version `major.minor.patch` (from `get_boot_info`) |
| `rollbacks` | `rollback_count` from FirmwareStore |

The cloud correlates a report without `job` with the device's single
open job (REQ-DM-054 allows only one), and can always reconcile from the
firmware version in the health messages (REQ-NF-503).

---

## 7. Provided and owned interfaces

Interface-segregated headers contain only the vtable and its types. The
module follows the FirmwareStore shape: an opaque instance passed as
`self`, plus `const` ops tables.

### 7.1 Creation (wiring only)

```c
typedef struct update_service update_service_t;   /* opaque */

typedef struct {
    const ifirmware_image_store_t  *fs_image;
    const ifirmware_boot_control_t *fs_boot;
    firmware_store_t               *fs;          /* FirmwareStore handle */
    const iota_transport_t         *transport;
    const us_probe_if_t            *probes;
    const ilifecycle_t             *lifecycle;   /* normally &lifecycle_controller vtable */
} update_service_config_t;

/**
 * @brief Create the instance, its mutex and UpdateServiceTask; cache the boot info.
 * @return UPDATE_SERVICE_ERR_OK; ERR_NULL_POINTER if cfg, out or any member
 *         (or any function pointer used) is NULL; ERR_NO_INSTANCE if already
 *         created or a FreeRTOS object cannot be created.
 * @note Threading: once, before the scheduler starts, after firmware_store_create().
 */
update_service_err_t update_service_create(const update_service_config_t *cfg,
                                           update_service_t **out);

extern const iupdate_service_t update_service_ops;    /* CloudPublisher */
extern const iupdate_control_t update_control_ops;    /* LifecycleController */

#ifdef TEST
void update_service_reset_for_test(void);
/** Run the task body until no notification is pending (single-step). */
void update_service_task_step_for_test(void);
#endif
```

### 7.2 `IUpdateService` — CloudPublisher (CloudPublisherTask context)

```c
typedef struct {
    /** Copy the command into the 1-deep inbox and notify the task. Never blocks. */
    update_service_err_t (*handle_command)(update_service_t *self,
                                           const us_ota_command_t *cmd);
    /** Copy one chunk into the slot if it is the one requested; else drop. Never blocks. */
    update_service_err_t (*on_chunk)(update_service_t *self, uint32_t job, uint32_t offset,
                                     const uint8_t *data, uint32_t len);
} iupdate_service_t;
```

| Function | Returns | Note |
|---|---|---|
| `handle_command` | `OK` (queued); `BUSY` if the inbox is full (the command is dropped and logged; the cloud job times out); `NULL_POINTER`, `NOT_INITIALISED` | All semantic checks and the report run in the task (§8.2) |
| `on_chunk` | `OK` (accepted); `STALE` (not the outstanding request: wrong job, offset or length, or the slot is full); `BAD_PARAM` (`len == 0` or `len > US_CHUNK_SIZE`); `NULL_POINTER`, `NOT_INITIALISED` | CloudPublisher ignores `STALE`. It is normal after a retry |

Both take only the UpdateService mutex, for at most one 1 KB copy. They
never call FirmwareStore, so CloudPublisherTask cannot be held by a 1.3 s
`verify` (alarm path, REQ-NF-113).

### 7.3 `IUpdateControl` — LifecycleController (LifecycleTask context)

```c
typedef struct {
    bool                 (*is_created)(const update_service_t *self);
    update_service_err_t (*start)(update_service_t *self, uint32_t job);
    update_service_err_t (*resume_self_checking)(update_service_t *self);
    update_service_err_t (*resume_rollback)(update_service_t *self);
    update_service_err_t (*resume_after_rollback)(update_service_t *self);
} iupdate_control_t;
```

| Function | Pre-condition | Effect | Error |
|---|---|---|---|
| `start(job)` | `US_ST_AWAITING_START` with the same `job` | Notify the task to begin downloading | `BAD_STATE` (programming error: LifecycleController faults, LC-D10) |
| `resume_self_checking` | Cached boot state TRIAL_RUNNING | `US_ST_SELF_CHECKING`; notify the task | `BAD_STATE` |
| `resume_rollback` | `US_ST_SELF_CHECKING` | `US_ST_ROLLING_BACK`; log ERROR. Synchronous, no flash access | `BAD_STATE` (LifecycleController logs and still rolls back) |
| `resume_after_rollback` | Cached boot state ROLLED_BACK | Notify the task to report and acknowledge (§13.4) | `BAD_STATE` |

All five return quickly; no FirmwareStore call is made in LifecycleTask
context.

### 7.4 `IOtaTransport` — owned by UpdateService, implemented by CloudPublisher

```c
typedef struct {
    void *ctx;
    /** Volatile mirror of the MQTT connection state. Any task. */
    bool (*is_connected)(void *ctx);
    /** Queue a chunk request. false = not queued (queue full). Dropped by
     *  CloudPublisher if the link is down when it is dequeued. Never buffered. */
    bool (*request_chunk)(void *ctx, const us_chunk_request_t *req);
    /** Queue a result report. false = not queued. QoS 1; StoreAndForward when offline. */
    bool (*report)(void *ctx, const us_ota_report_t *rep);
} iota_transport_t;
```

The interface lives in UpdateService's folder (`iota_transport.h`). The
`ctx` idiom matches `lifecycle_deps.h`, because the implementation is an
adapter in wiring code. CloudPublisher includes only the two vtable
headers, so there is no include cycle.

### 7.5 Adapters in `lifecycle_wiring` (LCC-4)

| `lc_*_if_t` member | Bound to |
|---|---|
| `update_service.init` | `update_control_ops.is_created(us)` → `LC_DEP_OK` / `FAIL` (the instance is created by `main()` before the scheduler) |
| `update_service.start(param)` | `update_control_ops.start(us, param)`; `param` is the `job` |
| `update_service.resume_self_checking` / `resume_rollback` / `resume_after_rollback` | Same-name `update_control_ops` functions |
| `firmware_store.get_pending_flags` | `get_boot_info`: `self_check = ota_capable && state == TRIAL_RUNNING`; `rollback = ota_capable && state == ROLLED_BACK` (FS §7.3) |
| `firmware_store.confirm_self_check` | `firmware_store_boot_ops.confirm` (idempotent: OK and no write when already CONFIRMED, TC-FS-074) |
| `firmware_store.rollback` | `firmware_store_boot_ops.rollback` |

---

## 8. Data types

```c
typedef enum {
    UPDATE_SERVICE_ERR_OK              = 0,
    UPDATE_SERVICE_ERR_NULL_POINTER    = 1,
    UPDATE_SERVICE_ERR_NOT_INITIALISED = 2,
    UPDATE_SERVICE_ERR_NO_INSTANCE     = 3,
    UPDATE_SERVICE_ERR_BAD_PARAM       = 4,
    UPDATE_SERVICE_ERR_BAD_STATE       = 5,
    UPDATE_SERVICE_ERR_BUSY            = 6,
    UPDATE_SERVICE_ERR_STALE           = 7,
} update_service_err_t;
```

### 8.1 Command, request, report

```c
typedef struct {
    uint32_t job;
    uint32_t size_a;   /* signed file length for Slot A */
    uint32_t size_b;   /* signed file length for Slot B */
} us_ota_command_t;

typedef struct {
    uint32_t        job;
    firmware_slot_t slot;     /* FIRMWARE_SLOT_A or _B */
    uint32_t        offset;
    uint32_t        len;      /* 1 … US_CHUNK_SIZE */
} us_chunk_request_t;

typedef enum {
    US_RESULT_ACCEPTED, US_RESULT_REJECTED, US_RESULT_FAILED,
    US_RESULT_SUCCESS,  US_RESULT_ROLLED_BACK,
} us_result_t;

typedef struct {
    bool        job_known;
    uint32_t    job;
    us_result_t result;
    us_reason_t reason;          /* US_REASON_NONE for accepted / success */
    uint32_t    version;         /* running version, major<<24 | minor<<16 | patch */
    uint32_t    rollback_count;
} us_ota_report_t;

typedef struct {
    void *ctx;
    bool (*sensors_ready)(void *ctx);   /* SensorService initialised and producing */
    bool (*modbus_ready)(void *ctx);    /* modbus_poller_is_ready() (MBP-D7)       */
} us_probe_if_t;
```

### 8.2 Command acceptance rules (task context)

A command is accepted only if **all** of these hold; otherwise it is
rejected with the first reason that applies, and the current update (if
any) is not touched:

| # | Rule | Reason |
|---|---|---|
| 1 | State `US_ST_IDLE` (REQ-DM-054) | `busy` |
| 2 | Cached boot info: `ota_capable` and state CONFIRMED | `not_ota_capable` |
| 3 | LifecycleController state is OPERATIONAL or EDITING_CONFIG (an OTA event in INIT is dropped by LifecycleController) | `not_ready` |
| 4 | `job` ∉ {0, 0xFFFFFFFF}; file size for the target slot in 129 … 491 648 | `bad_command` |
| 5 | `lifecycle->post_event(OTA_REQUESTED, job)` returns true (false while UPDATING_FW or when the queue is full) | `busy` |

On acceptance: `US_ST_AWAITING_START`, start timer `US_START_TIMEOUT_MS`.
`accepted` is reported in `start()` handling (§10.1), not here: until
LifecycleController agrees, the device has not committed to the update.

### 8.3 Constants

| Name | Value | Basis |
|---|---|---|
| `US_CHUNK_SIZE` | 1024 B | ≤ 1/4 of `MQTT_PKT_BUF_SIZE`; a quarter of a QSPI sector. US-O1 measures throughput |
| `US_CHUNK_TIMEOUT_MS` | 5000 | Covers the 100 ms MQTT process tick plus WiFi/TLS round trip with margin |
| `US_MAX_RETRIES` | 3 | REQ-DM-052, Machine 3 T4 |
| `US_STALL_LIMIT_MS` | 600 000 | No progress for 10 min, connected or not → fail (US-D7, US-O3) |
| `US_START_TIMEOUT_MS` | 360 000 | Above the 5-minute EditingConfig deferral (REQ-NF-214) |
| `US_SELF_CHECK_TIMEOUT_MS` | 60 000 | Includes MQTT reconnect (HLD state-machines open question 5); US-O2 |
| `US_SELF_CHECK_POLL_MS` | 500 | |
| `US_REPORT_RETRY_MS` / `_MAX` | 100 / 20 | Transport queue full → retry, then log ERROR |

### 8.4 Reasons (wire strings)

| `us_reason_t` | Wire | From |
|---|---|---|
| `US_REASON_BUSY` | `busy` | §8.2 rules 1, 5 |
| `US_REASON_NOT_OTA_CAPABLE` | `not_ota_capable` | §8.2 rule 2; FS `NOT_OTA_CAPABLE` |
| `US_REASON_NOT_READY` | `not_ready` | §8.2 rule 3 |
| `US_REASON_BAD_COMMAND` | `bad_command` | §8.2 rule 4; CPC-4 |
| `US_REASON_START_TIMEOUT` | `start_timeout` | AWAITING_START expired |
| `US_REASON_DOWNLOAD_FAILED` | `download_failed` | 3 consecutive failures (REQ-DM-052) |
| `US_REASON_STALLED` | `stalled` | No progress for `US_STALL_LIMIT_MS` |
| `US_REASON_STAGING_ERROR` | `staging_error` | FS flash error in `begin`/`resume` that does not recover |
| `US_REASON_TOO_LARGE` … `US_REASON_BAD_IMAGE` | `too_large`, `bad_header`, `wrong_slot`, `sig_invalid`, `hash_mismatch`, `bad_image` | FS `verify` terminal codes, one-to-one |
| `US_REASON_CRYPTO` | `crypto` | FS `CRYPTO` twice |
| `US_REASON_VERIFY_IO` | `verify_io` | FS `FLASH_READ` twice in verify |
| `US_REASON_INSTALL_FAILED` | `install_failed` | Install failed twice (§12) |
| `US_REASON_ACTIVATE_FAILED` | `activate_failed` | `activate` failed |
| `US_REASON_ROLLBACK` | `rollback` | After a reboot: the self-check failed, or the image crashed during its trial (indistinguishable, §13.4) |

---

## 9. Internal design

### 9.1 State

```c
typedef enum {
    US_ST_IDLE, US_ST_AWAITING_START, US_ST_DOWNLOADING, US_ST_VALIDATING,
    US_ST_INSTALLING, US_ST_ACTIVATED, US_ST_SELF_CHECKING, US_ST_ROLLING_BACK,
} us_state_t;

typedef enum { US_SLOT_FREE, US_SLOT_EXPECTING, US_SLOT_FULL } us_slot_state_t;

struct update_service {
    bool                    created;
    update_service_config_t cfg;
    /* --- shared with other tasks: under s_mutex --- */
    us_state_t              state;
    firmware_boot_info_t    boot;            /* cached; refreshed only by the task */
    bool                    inbox_full;
    us_ota_command_t        inbox;
    uint32_t                active_job;
    us_slot_state_t         slot_state;
    uint32_t                exp_offset, exp_len, got_len;
    /* --- task only --- */
    us_ota_command_t        cmd;
    firmware_slot_t         target_slot;
    uint32_t                file_len, offset;
    uint8_t                 failures;        /* consecutive, REQ-DM-052 */
    TickType_t              req_deadline, last_progress, start_deadline, check_deadline;
    bool                    paused;          /* link down while downloading */
    TaskHandle_t            task;
};

static struct update_service s_pool[1];
static uint8_t s_chunk[US_CHUNK_SIZE];         /* the single chunk slot */
static StaticTask_t s_tcb;  static StackType_t s_stack[1024];
static StaticSemaphore_t s_mutex_buf;  static SemaphoreHandle_t s_mutex;
```

No UpdateService state survives a reset. Everything that must survive
(staged bytes, resume checkpoint, slot descriptors, boot state, rollback
count) is in FirmwareStore (US-D3).

### 9.2 Task loop

```
create(): cache boot = fs_boot->get_boot_info(); create mutex, task
task:
  loop:
    bits = xTaskNotifyWait(all, timeout = nearest active deadline or portMAX_DELAY)
    if bits & US_N_COMMAND        → process inbox (§8.2)
    if bits & US_N_START          → begin download (§10.1)
    if bits & US_N_CHUNK          → write chunk (§10.2)
    if bits & US_N_SELF_CHECK     → run self-check (§13.2)
    if bits & US_N_AFTER_ROLLBACK → report and acknowledge (§13.4)
    check deadlines: start (AWAITING_START), request and stall (DOWNLOADING),
                     self-check (SELF_CHECKING)
```

### 9.3 Notification bits

| Bit | Name | Set by |
|---|---|---|
| 0 | `US_N_COMMAND` | `handle_command` (CloudPublisherTask) |
| 1 | `US_N_START` | `start` (LifecycleTask) |
| 2 | `US_N_CHUNK` | `on_chunk` (CloudPublisherTask) |
| 3 | `US_N_SELF_CHECK` | `resume_self_checking` (LifecycleTask) |
| 4 | `US_N_AFTER_ROLLBACK` | `resume_after_rollback` (LifecycleTask) |

### 9.4 Chunk slot (single-producer, single-consumer)

There is at most one outstanding request, so one 1 KB slot is enough and
no queue is needed.

```
task, before requesting:  lock; slot = EXPECTING; exp = (offset, len); active_job; unlock
on_chunk (CP task):       lock;
                          if slot == EXPECTING && job == active_job &&
                             offset == exp_offset && len == exp_len:
                               memcpy(s_chunk, data, len); got_len = len; slot = FULL
                               unlock; notify US_N_CHUNK; return OK
                          unlock; return STALE
task, on US_N_CHUNK:      lock; if slot == FULL → take (no copy: s_chunk is now owned by the task); unlock
                          fs_image->write_chunk(...); lock; slot = FREE; unlock
```

The task reads `s_chunk` outside the lock. That is safe because
`on_chunk` writes only while the state is EXPECTING, and the state is FULL
until the task frees it.

---

## 10. Download

### 10.1 Start (`US_N_START`, Machine 3 T2)

1. `target_slot` = the slot that is not `boot.running_slot`; `file_len` = `size_a` or `size_b` for it.
2. `rc = fs_image->resume(job, file_len, &offset)`:
   - `OK` → continue from `offset` (DM-051, also across a reboot, §10.5);
   - `NO_SESSION` → `fs_image->begin(job, file_len)`, `offset = 0`;
   - any other error → fail `staging_error` (§14).
3. Report `accepted` (job, running version). State `US_ST_DOWNLOADING`; `failures = 0`; `last_progress = now`.
4. If `offset == file_len` (resume found a complete file) → Validating (§11). Otherwise request (§10.2).

### 10.2 Request and write loop (Machine 3 I1)

```
request():
  if !transport->is_connected(): paused = true; return        /* do not count */
  len = min(US_CHUNK_SIZE, file_len − offset)
  arm slot (§9.4); transport->request_chunk({job, target_slot, offset, len})
  req_deadline = now + US_CHUNK_TIMEOUT_MS

on US_N_CHUNK:
  rc = fs_image->write_chunk(offset, s_chunk, got_len); free slot
  OK          → offset += got_len; failures = 0; last_progress = now
                offset == file_len ? → Validating : request()
  BAD_PARAM   → resync (§10.3)                 /* should not happen; defensive */
  FLASH_*     → failures++; resync; failures ≥ 3 ? fail(download_failed) : request()
```

If `request_chunk` returns false (transport queue full), the request is
treated as sent. The request deadline then produces a retry.

### 10.3 Resync

`fs_image->resume(job, file_len, &offset)` resets the write position to
the last FirmwareStore checkpoint (≤ 4 KB back) after a failed or
out-of-order write. `resume` failing → fail `staging_error`.

### 10.4 Timeouts, pause and failure (REQ-DM-051, -052)

| Event | Action |
|---|---|
| Request deadline passes, link up | `failures++`; `failures ≥ US_MAX_RETRIES` → fail `download_failed`; else re-request the same offset (Machine 3 I2) |
| Request deadline passes, link down | `paused = true`; not counted (UC-18 E2) |
| Paused and link up again (checked every `US_SELF_CHECK_POLL_MS` while paused) | `paused = false`; re-request (DM-051) |
| `now − last_progress > US_STALL_LIMIT_MS` | Fail `stalled` |

**Interpretation (US-D6):** REQ-DM-052's "three retries" counts
*consecutive* failed attempts **while connected**. A success resets the
counter. Losing the connection is REQ-DM-051 (resume), not a failure.
Machine 3 T4 (`retries == 3`) is unchanged.

**Fail (Machine 3 T4):** `fs_image->discard()` (REQ-DM-052; error →
LOG_ERROR, UC-18 E4, continue) → LOG_ERROR with the reason (REQ-DM-053) →
report `failed` → post `LC_EVENT_OTA_FINISHED` → `US_ST_IDLE`.

### 10.5 Reboot during download

UpdateService keeps nothing in RAM across a reset, and it does not
restart an update by itself (non-goal). The cloud job times out and sends
the command again with the **same** `job`. §10.1 step 2 then finds the
FirmwareStore session and continues from its checkpoint. A different
`job` starts afresh (`begin` erases the old session). This meets
REQ-DM-051 across a reboot without any UpdateService NVM.

---

## 11. Validation (Machine 3 Validating, T5, T6)

`US_ST_VALIDATING`: `rc = fs_image->verify(&info)` (measured 1.32 s worst,
FS-O5; runs in UpdateServiceTask only).

| `rc` | Action |
|---|---|
| `OK` | Installing (§12) |
| `TOO_LARGE`, `BAD_HEADER`, `WRONG_SLOT`, `SIG_INVALID`, `HASH_MISMATCH`, `BAD_IMAGE` | Terminal. Fail with the matching reason (REQ-DM-061, -062; UC-18 rainy steps 6–7) |
| `CRYPTO`, `FLASH_READ` | Retry `verify` once; second failure → fail `crypto` / `verify_io` (UC-20 E1) |
| `INCOMPLETE`, `BAD_STATE`, others | Programming error: LOG_ERROR, fail `staging_error` |

Fail = the procedure of §10.4 (discard, log, report, `OTA_FINISHED`).

---

## 12. Installation and activation (Machine 3 Applying, T7)

```
US_ST_INSTALLING:
  for attempt in 1..2:
    rc = fs_image->install_begin()
    while rc == OK and !done:
      rc = fs_image->install_step(&done)
      vTaskDelay(1)                           /* yield between 2 KB pages, FS-D7 */
    if rc == OK: break
    LOG_ERROR(rc)                             /* INSTALL_VERIFY or FLASH_*: retry once (FS §19) */
  rc != OK → fail(install_failed)

  rc = fs_image->activate()                   /* TRIAL_PENDING */
  rc != OK → fail(activate_failed)            /* op INSTALLED; metadata unchanged */

US_ST_ACTIVATED:
  post LC_EVENT_OTA_RESET_REQUESTED           /* Machine 1 I18 */
  if post fails: retry every 1 s, log WARN    /* image boots at the next reset anyway */
```

The running slot is never written (REQ-DM-073; FirmwareStore write guard).
No result is reported before the reset: success is known only after the
self-check on the new image (US-D8).

---

## 13. Post-update boot

### 13.1 Who does what

| Step | LifecycleController (LifecycleTask) | UpdateService (UpdateServiceTask) |
|---|---|---|
| Boot, TRIAL_RUNNING | `get_pending_flags` → self_check; `resume_self_checking()`; waits for the verdict (LC-D4) | Self-check (§13.2) |
| Pass | `SELF_CHECK_PASS` → `confirm_self_check` (no-op, already confirmed) → Operational | `confirm`, report `success`, `discard`, post `SELF_CHECK_PASS` |
| Fail | `SELF_CHECK_FAIL` → `resume_rollback()` → `firmware_store.rollback` → reset | Post `SELF_CHECK_FAIL`; in `resume_rollback`, state ROLLING_BACK and log |
| Boot, ROLLED_BACK | `resume_after_rollback()`; → Operational | Report `rolled_back`, `acknowledge_rollback`, `discard` (§13.4) |

UpdateService confirms **before** it reports success and posts PASS. The
report is therefore never sent for an image that is not yet committed.
LifecycleController's own confirm becomes an idempotent re-assert
(TC-FS-074).

### 13.2 Self-check (REQ-DM-071, REQ-DM-040)

```
on US_N_SELF_CHECK: check_deadline = now + US_SELF_CHECK_TIMEOUT_MS
every US_SELF_CHECK_POLL_MS:
  ok = probes->sensors_ready() && probes->modbus_ready() && transport->is_connected()
  ok   → pass (§13.3)
  now ≥ check_deadline → LOG_ERROR(which probe failed); post SELF_CHECK_FAIL
```

**Probe coverage (US-O4):** `modbus_ready` is "ModbusPoller initialised"
(MBP-D7). The poller task waits on the start gate, so the Field Device
link cannot be proven before Operational without deadlocking Init. The
cloud probe needs the broker: a good image on a site with no internet is
rolled back. This is the HLD decision (state-machines open question 5:
include MQTT reconnect) and is accepted.

### 13.3 Pass and fail

**Pass (Machine 3 T9):**
1. `fs_boot->confirm()`. On error: LOG_ERROR, post `SELF_CHECK_FAIL`. LifecycleController attempts the rollback; if that also fails, the image is still TRIAL_RUNNING and the bootloader rolls it back at the next reset. Either way the system converges (FS §14).
2. Refresh the cached boot info; report `success` (version = new running version).
3. `fs_image->discard()` (a duplicate job must not find a complete staged file).
4. Post `SELF_CHECK_PASS`; `US_ST_IDLE`.

**Fail (Machine 3 T10):** post `SELF_CHECK_FAIL`. Nothing is reported:
the reset follows, and the report is made by the rolled-back image
(§13.4). The 10 s budget of REQ-NF-204 starts here (§18).

### 13.4 After a rollback (Machine 3 T12)

On `US_N_AFTER_ROLLBACK`:
1. Report `rolled_back`, reason `rollback`, no `job`, version = running (old) version, `rollbacks` = count.
2. `fs_boot->acknowledge_rollback()`. On error: LOG_ERROR; the state stays ROLLED_BACK and the next boot reports again (idempotent for the cloud).
3. `fs_image->discard()`; refresh the cached boot info; `US_ST_IDLE`.

The same path covers a rollback made by the bootloader (crash or
watchdog during the trial, FS §13.2). UpdateService cannot tell the two
cases apart, hence the single reason `rollback`. The detail is in the
persistent log of the failed image.

Window: a power loss between step 1 being queued and its delivery, with
step 2 already done, loses the report. The cloud then reconciles from the
version in the health message (REQ-NF-503). Accepted (US-D9).

---

## 14. Reporting summary (REQ-DM-055)

| Moment | Result | Reason | `job` |
|---|---|---|---|
| Command refused (§8.2) | `rejected` | `busy` / `not_ota_capable` / `not_ready` / `bad_command` | yes |
| Start timeout | `rejected` | `start_timeout` | yes |
| LifecycleController started the update | `accepted` | — | yes |
| Download, validation, install or activate failed | `failed` | §8.4 | yes |
| Self-check passed and image confirmed | `success` | — | no |
| Rolled-back image booted | `rolled_back` | `rollback` | no |
| Malformed command (CloudPublisher, CPC-4) | `rejected` | `bad_command` | if parsable |

Every report goes through `transport->report()`, at QoS 1, buffered in
StoreAndForward when offline (UC-19 E4). A full transport queue is retried
`US_REPORT_RETRY_MAX` times at `US_REPORT_RETRY_MS`, then LOG_ERROR.

---

## 15. Authentication (REQ-DM-056)

The device cannot see who published an MQTT message, so authentication
is layered. UpdateService adds no check of its own (US-D5):

1. **Transport:** TLS 1.2 with server certificate validation and X.509
   client authentication (REQ-NF-300, -301). The device only receives
   messages from the genuine broker.
2. **Broker authorisation (deployment):** the AWS IoT policy (or the
   Mosquitto ACL on the test broker) allows publishing to
   `cmd/iotmonitor/<id>/ota` and `…/ota/data` only to the operator identity.
   This is documented in `tools/ota/README.md` (US-O5).
3. **End to end:** whatever arrives, only an image signed with the release
   key is installed (FirmwareStore, FS-D4, REQ-NF-304). A forged command
   can at worst start a download that fails `sig_invalid`.

v0.1's "CloudPublisher checks the allowed-publisher list" was not
implementable: MQTT 3.1.1 does not carry the publisher identity to
subscribers.

---

## 16. Thread safety

| Data | Writers | Readers | Protection |
|---|---|---|---|
| `inbox`, `inbox_full` | CloudPublisherTask (`handle_command`), UpdateServiceTask | UpdateServiceTask | `s_mutex` |
| `slot_state`, `exp_*`, `got_len`, `active_job`, `s_chunk` (§9.4) | CloudPublisherTask (`on_chunk`), UpdateServiceTask | both | `s_mutex` + slot ownership |
| `state`, `boot` | UpdateServiceTask, LifecycleTask (`start`, `resume_*` set state) | all three | `s_mutex` |
| Everything else | UpdateServiceTask only | — | none |

- One static mutex (priority inheritance, D26). It is held only for field copies (≤ 1 KB memcpy, a few µs at 80 MHz), **never** across a FirmwareStore, transport or lifecycle call. There is therefore no lock nesting and no deadlock path with the FirmwareStore or QSPI mutexes.
- FirmwareStore is called only from UpdateServiceTask (and from LifecycleTask by LifecycleController itself, FS §16).
- No ISR access.

---

## 17. Initialisation order

```
firmware_store_create(&fs_cfg, &fs)            /* boot info resolved */
cloud_publisher_create(...)                    /* implements IOtaTransport (CPC-5) */
update_service_create(&us_cfg, &us)            /* caches boot info, creates task */
cloud_publisher binds update_service_ops + us  /* CPC-6 wiring */
lifecycle_wiring_bind(...)                     /* adapters §7.5 */
lifecycle_controller_init(...)
vTaskStartScheduler()
```

UpdateServiceTask does **not** block on the start gate. It must run the
self-check while LifecycleController is still in Init (LC-D4), so it only
waits for its notifications.

---

## 18. Memory and timing

| RAM | Size |
|---|---|
| Instance | ~120 B |
| Chunk slot | 1024 B |
| Task stack (static) | 4096 B (D23; US-O6 high-water mark) |
| `StaticTask_t` + `StaticSemaphore_t` | ~170 B |
| **UpdateService total** | **~5.4 KB** |
| CloudPublisher side (CPC-5): 4 × outbound entry + queue control | ~200 B |

| Item | Value | Basis |
|---|---|---|
| Command intake (`handle_command`) | < 50 µs | Copy + notify; REQ-NF-107 (begin within 500 ms) met |
| Chunk write | ~33 ms average, ~94 ms worst per 1 KB | 4 × 3.2 ms page program; one 81 ms sector erase every fourth chunk (measured QSPI figures) |
| Download, max image (480 chunks) | ~2–3 min estimated | Dominated by the WiFi/TLS round trip; US-O1 |
| Verify | 1.32 s worst | FS-O5 measured |
| Install | 11.96 s | FS-O1 measured, 240 steps with a yield |
| Rollback decision → old image running (REQ-NF-204, 10 s) | ~25 ms metadata write + immediate reset (the rollback path has no grace delay) + boot ≤ 5 s (REQ-NF-203) | Within 10 s; measured only with the bootloader (US-O7) |

---

## 19. Error behaviour

UpdateService API errors are programming or flow-control errors. Update
failures are not API errors: they become cloud reports (§14).

| Error / condition | Cause | Local behaviour | Caller sees | Observability |
|---|---|---|---|---|
| `NULL_POINTER`, `NOT_INITIALISED`, `NO_INSTANCE` | Programming error | No state change | Error code | LOG_ERROR |
| `BAD_PARAM` (`on_chunk`) | `len` 0 or > 1024 | Dropped | Code; CloudPublisher logs WARN | LOG_WARN |
| `BUSY` (`handle_command`) | Inbox full (two commands within one task pass) | Second command dropped | Code; CloudPublisher logs | LOG_WARN |
| `STALE` (`on_chunk`) | Late or duplicate chunk after a retry | Dropped | Code; CloudPublisher ignores | LOG_DEBUG |
| `BAD_STATE` (`IUpdateControl`) | Called in the wrong state | No state change | LifecycleController: LC-D10 fault for `start`; WARN for the others | LOG_ERROR |
| FS `discard` fails | Flash error (UC-18 E4) | Log; continue the failure path; current firmware untouched | — | LOG_ERROR |
| FS `confirm` fails | Metadata write error | Post `SELF_CHECK_FAIL` (§13.3) | Rollback | LOG_ERROR |
| FS `acknowledge_rollback` fails | Metadata write error | Stays ROLLED_BACK; reported again next boot | — | LOG_ERROR |
| `post_event` fails for `OTA_RESET_REQUESTED` | LifecycleController queue full | Retry every 1 s | — | LOG_WARN |
| Transport queue full | CloudPublisher busy | Request: covered by the deadline. Report: retry ×20 | — | LOG_ERROR after the last retry |

Every terminal OTA failure produces exactly one LOG_ERROR with the reason
and the FirmwareStore code (REQ-DM-053, -062).

---

## 20. Principles applied

- **P1 — applied.** UpdateService (Application) uses FirmwareStore (Middleware) through its interfaces. It also uses LifecycleController and CloudPublisher, which are both in the same layer, through narrow interfaces. It never touches MqttClient or a driver.
- **P2 — not applicable.** No lower layer delivers data upward to UpdateService. `IOtaTransport` is owned by its consumer, UpdateService, to break a same-layer cycle at header level (interface-segregated headers, project rule). That is not an upward-flow inversion.
- **P3 — applied.** CloudPublisher (intake) and LifecycleController (lifecycle control) need disjoint operations, hence `IUpdateService` and `IUpdateControl`.
- **P4 — applied.** Logger macros used directly, as in FirmwareStore (FS-D15).
- **P5 — not applicable.** No counters and no pushed health events. Outcomes are cloud reports (REQ-DM-055). `components.md` has no `IHealthReport` for UpdateService.
- **P6 — applied.** Every behaviour traces to §2; non-goals are listed and not implemented.
- **P7 — applied.** Boot state is pulled from FirmwareStore and cached; probes and link state are pulled.
- **P8 — applied.** No helper infrastructure. A single-slot mailbox replaces a queue because only one request is ever outstanding.
- **P9 — not applicable** at module level (B4 updates the SD-06 views).
- **P10 — applied.** Component `UpdateService`; interfaces `IUpdateService` → `iupdate_service_t`, `IUpdateControl` → `iupdate_control_t`, `IOtaTransport` → `iota_transport_t`; prefix `update_service_` / `us_`; errors `UPDATE_SERVICE_ERR_*`.

Coding standard (BARR-C subset, not a P-principle): fixed-width types,
named constants (§8.3), static allocation only, no floating point, no JSON
in this module.

---

## 21. File layout

```
firmware/gateway/application/update_service/
├── update_service.h          create, config, ops externs, test hooks
├── update_service_types.h    error enum, command/request/report structs, reasons, constants
├── iupdate_service.h         vtable only (CloudPublisher)
├── iupdate_control.h         vtable only (LifecycleController)
├── iota_transport.h          vtable only (implemented by CloudPublisher)
├── update_service.c          public API, mutex, inbox, chunk slot
└── update_service_task.c     task loop and Machine 3 (§9.2–§13)

tests/gateway/application/update_service/
├── test_update_service_api.c        intake, slot, control functions
├── test_update_service_download.c   §10
├── test_update_service_apply.c      §11, §12
├── test_update_service_boot.c       §13
└── fakes/fake_fs_ops.c, fake_transport.c, fake_probes.c, fake_lifecycle.c
                                     scriptable return sequences + call logs

firmware/gateway/integration-tests/update_service/main_test_update_service.c   §23.1
scripts/ota_uart_server.py      answers chunk requests over the debug UART (§23.1)
scripts/ota_server.py           MQTT OTA server for the system test (§23.2)
tools/ota/README.md             + broker policy for /ota topics (US-O5)
```

FirmwareStore is used through **fakes of its two vtables**, not the real
module. The FirmwareStore contract is already proven by its own 15
power-loss and contract suites. The fakes let each FS error code be
injected at each call.

---

## 22. Unit test plan (host)

Each case starts from `update_service_reset_for_test()`. The task is
driven with `update_service_task_step_for_test()`, and time with the
FreeRTOS tick mock.

**Create and intake**

| TC | Behaviour | Traces |
|---|---|---|
| TC-US-001 | NULL `cfg`, any NULL member or used function pointer, NULL `out` → `NULL_POINTER` | §7.1 |
| TC-US-002 | Second `create` → `NO_INSTANCE` | — |
| TC-US-003 | Ops on a handle not from `create` → `NOT_INITIALISED` | — |
| TC-US-004 | `create` caches `get_boot_info`, makes no other FS call | §9.1 |
| TC-US-005 | `handle_command` copies and notifies; makes no FS, transport or lifecycle call | §7.2 |
| TC-US-006 | Second `handle_command` before the task runs → `BUSY`, first kept | §7.2 |
| TC-US-007 | Valid command → `post_event(OTA_REQUESTED, job)`; AWAITING_START; no report yet | §8.2 |
| TC-US-008 | Each rule 1–5 of §8.2 failing → one `rejected` report with that reason; no FS call | DM-054, DM-055 |
| TC-US-009 | Command while DOWNLOADING → `rejected/busy`; active download untouched | DM-054 |
| TC-US-010 | Size for the target slot selected (`size_b` when running A, `size_a` when running B) | FS-D3 |
| TC-US-011 | AWAITING_START + `US_START_TIMEOUT_MS` → `rejected/start_timeout`; IDLE | §8.3 |
| TC-US-012 | `start` in the wrong state or with another job → `BAD_STATE` | §7.3 |

**Download**

| TC | Behaviour | Traces |
|---|---|---|
| TC-US-020 | `start`: `resume` → `NO_SESSION` → `begin(job, len)`; `accepted` reported; first request offset 0, len 1024, target slot | DM-050 |
| TC-US-021 | `start`: `resume` → OK at 8192 → first request at 8192; no `begin` | DM-051 |
| TC-US-022 | `resume` reports the complete file → straight to `verify` | DM-051 |
| TC-US-023 | Last chunk length = remainder | — |
| TC-US-024 | `on_chunk` matching → OK, `write_chunk` with the same bytes, next request | DM-050 |
| TC-US-025 | `on_chunk` wrong job / offset / length / slot FULL → `STALE`, nothing written | §9.4 |
| TC-US-026 | `on_chunk` len 0 or > 1024 → `BAD_PARAM` | — |
| TC-US-027 | Timeout while connected → same offset re-requested; `failures` 1, 2 | DM-052, Machine 3 I2 |
| TC-US-028 | Third consecutive timeout → `discard`, `failed/download_failed`, `OTA_FINISHED`, IDLE | DM-052, DM-053 |
| TC-US-029 | Success between failures resets the counter (2 timeouts, ok, 2 timeouts → still downloading) | US-D6 |
| TC-US-030 | Link down: no requests, timeouts not counted; link up → re-request same offset | DM-051, UC-18 E2 |
| TC-US-031 | No progress for `US_STALL_LIMIT_MS` while paused → `failed/stalled` | US-D7 |
| TC-US-032 | `write_chunk` FLASH error → `resume` resync, failure counted, request from checkpoint | §10.3 |
| TC-US-033 | `begin` / `resume` hard error → `failed/staging_error` | §10.1 |
| TC-US-034 | `discard` failing on the failure path → still reports and finishes | UC-18 E4 |
| TC-US-035 | `request_chunk` returns false → retried at the deadline, not lost | §10.2 |

**Validation, install, activate**

| TC | Behaviour | Traces |
|---|---|---|
| TC-US-040 | `verify` OK → install loop; `install_step` called until done; a yield between steps | DM-070, FS-D7 |
| TC-US-041 | Each terminal `verify` code → `discard`, `failed` with the matching reason, `OTA_FINISHED`; no `install_*` | DM-060..062, DM-080 |
| TC-US-042 | `CRYPTO` then OK → installs; `CRYPTO` twice → `failed/crypto` | UC-20 E1 |
| TC-US-043 | `FLASH_READ` twice in `verify` → `failed/verify_io` | UC-20 E1 |
| TC-US-044 | `INSTALL_VERIFY` once → `install_begin` again and success | FS §19 |
| TC-US-045 | Install fails twice → `discard`, `failed/install_failed` | DM-061 |
| TC-US-046 | `activate` OK → `OTA_RESET_REQUESTED` posted; no report; state ACTIVATED | Machine 1 I18, US-D8 |
| TC-US-047 | `activate` error → `failed/activate_failed`, `OTA_FINISHED` | DM-061 |
| TC-US-048 | `post_event` false for `OTA_RESET_REQUESTED` → retried every 1 s | §12 |
| TC-US-049 | No FS call ever targets the running slot (fake asserts `install_*` only after a `verify` that returned the inactive slot) | DM-073 |

**Post-update boot**

| TC | Behaviour | Traces |
|---|---|---|
| TC-US-060 | `resume_self_checking` with TRIAL_RUNNING → SELF_CHECKING; probes polled | DM-071 |
| TC-US-061 | `resume_self_checking` with any other state → `BAD_STATE` | — |
| TC-US-062 | All probes OK → order: `confirm`, `success` report (new version), `discard`, `SELF_CHECK_PASS` | DM-071, DM-073, DM-055 |
| TC-US-063 | Cloud not connected for 60 s → `SELF_CHECK_FAIL`; no report; no FS write | DM-072 |
| TC-US-064 | Each probe false alone at the deadline → FAIL, and the log names it | DM-040 |
| TC-US-065 | `confirm` error → `SELF_CHECK_FAIL`, no success report | §13.3 |
| TC-US-066 | `resume_rollback` → ROLLING_BACK, synchronous, no FS call | DM-072 |
| TC-US-067 | `resume_after_rollback` → order: `rolled_back` report (no job, old version, count), `acknowledge_rollback`, `discard`; IDLE | DM-072, DM-055 |
| TC-US-068 | `acknowledge_rollback` error → state ROLLED_BACK kept; new command rejected `not_ota_capable` | §13.4 |
| TC-US-069 | After success, the same job again → new download from 0 (session discarded) | §13.3 |

**Concurrency**

| TC | Behaviour | Traces |
|---|---|---|
| TC-US-080 | Mutex taken and given around every shared-field access; never held during an FS, transport or lifecycle call (fakes assert that the mutex is free when called) | §16 |
| TC-US-081 | `on_chunk` during the task's `write_chunk` (slot FULL) → `STALE` | §9.4 |

---

## 23. Integration and system tests

### 23.1 Bench harness — `integration-tests/update_service` (ready now)

A Slot-A image behind the boot stub (FS-O12), with real FreeRTOS,
FirmwareStore, InternalFlash and QSPI. The transport is a harness-local
`IOtaTransport` over the debug UART, answered by
`scripts/ota_uart_server.py` (which serves the `make_image.py` output and
can drop, delay or corrupt chunks). Probes and the lifecycle are
harness fakes that print the posted events.

| TC | Setup | Pass criterion |
|---|---|---|
| TC-HW-US-001 | Command for a valid max-size Slot-B image | `accepted`; all chunks written; verify OK; 240 install steps; `activate`; `OTA_RESET_REQUESTED` printed. Download time, chunk round trip and stack high-water mark logged (US-O1, US-O6) |
| TC-HW-US-002 | Server stops answering for 30 s mid-download, link reported up | Three retries, `failed/download_failed`, staging discarded |
| TC-HW-US-003 | Link reported down for 2 min mid-download, then up | No failure counted; resumes at the same offset; completes |
| TC-HW-US-004 | Power cut mid-download, reboot, same job sent again | Resumes ≤ 4 KB before the cut; completes; verify OK |
| TC-HW-US-005 | Image with one flipped body byte | `failed/hash_mismatch`; slot B descriptor absent |
| TC-HW-US-006 | Slot-A-linked image | `failed/wrong_slot` |
| TC-HW-US-007 | Second command during the download | `rejected/busy`; first completes |
| TC-HW-US-008 | 1 kHz timer ISR gap meter during the whole run | Worst gap ≤ 30 ms (only `install_begin` / `activate`, FS-O10); CloudPublisher-like prio-2 task never delayed > 5 ms by UpdateService |
| TC-HW-US-009 | Harness seeds TRIAL_RUNNING metadata (no bootloader); fake probes pass | `confirm` → CONFIRMED read back; `success` printed |
| TC-HW-US-010 | Harness seeds ROLLED_BACK | `rolled_back` printed; `acknowledge_rollback` → CONFIRMED read back |

### 23.2 System test over MQTT (gated, §4.3)

| TC | Setup | Pass criterion |
|---|---|---|
| TC-SYS-OTA-001 | Mosquitto + `scripts/ota_server.py`; command from the operator identity | Device reboots into the new slot; `success` with the new version on `/ota/result` |
| TC-SYS-OTA-002 | New image whose self-check fails (probe forced false by a build flag) | Rollback; `rolled_back` with the old version; rollback ≤ 10 s from the FAIL decision (REQ-NF-204) |
| TC-SYS-OTA-003 | Broker stopped for 2 min mid-download | Resumes; completes (UC-18 E2) |
| TC-SYS-OTA-004 | Command published by a non-operator identity | Refused by the broker ACL; device never sees it (REQ-DM-056) |
| TC-SYS-OTA-005 | Image signed with another key | `failed/sig_invalid` (REQ-NF-304) |

---

## 24. Open items and decisions

### 24.1 Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| US-O1 | Chunk size and throughput: 1024 B chosen; real round trip over ISM43362 + TLS unknown | Measure in TC-HW-US-001 / TC-SYS-OTA-001; 2048 B is the next step (still inside `MQTT_PKT_BUF_SIZE`) | Open |
| US-O2 | Self-check cloud timeout of 60 s is provisional | Measure the reconnect time after boot (~12 s broker-drop recovery, ~4 s TLS measured for CloudPublisher) | Open |
| US-O3 | Stall limit of 10 min is a design value, not an SRS figure | Review at system test | Open |
| US-O4 | Self-check cannot prove the Field Device link (poller gated by the start gate); "no internet" rolls a good image back | Accepted per HLD; revisit if a field case appears | Open |
| US-O5 | Broker policy for `/ota` and `/ota/data` (AWS IoT policy, Mosquitto ACL) | **Done 2026-10-27:** `tools/ota/README.md` (Mosquitto ACL and AWS IoT policy), with `scripts/ota_server.py` | Closed |
| US-O6 | Stack: `verify` runs software ECDSA in this task; FS-O4 (mbedTLS heap, no static arena) is inherited, so REQ-NF-408 is not yet met for verify | Measure the high-water mark in TC-HW-US-001; FS-O4 resolution applies here | Open |
| US-O7 | Full swap, trial and rollback on target need the bootloader | Bootloader LLD (FS-O7) | Open |
| US-O8 | HLD amendments B1–B4 and the VP diagrams | **B1–B4 done 2026-10-27** (PR #104; `components.md`, `state-machines.md`, `sequence-diagrams.md`). CPC-1..6 done on `feature/phase-4-gw-cloud_publisher_ota` (cloud-publisher-lld.md CP-D17). The Visual Paradigm model and PNG diagrams are not updated | Partly closed |
| v0.1 US-O1 | Watchdog during apply | Step-wise install with a yield (FS-D7) | **Closed** |
| v0.1 US-O2 | `ILifecycle.get_self_check_result()` | UpdateService runs the probes itself (§13) | **Closed** |
| v0.1 US-O3 | NVM region for UpdateService | Not needed (US-D3) | **Closed** |
| v0.1 US-O4 | Chunk topic and size | Defined in §6 | **Closed** |

The v0.1 items were renumbered: IDs US-O1..US-O8 above are new.

### 24.2 Decisions

| ID | Decision | Rationale |
|---|---|---|
| US-D1 | Pull-based download: the device requests each chunk; the cloud answers | Flow control for free (QSPI writes are slower than the network); resume is just "request from the checkpoint"; only one outstanding message, so a single 1 KB slot instead of a queue. Same model as AWS IoT MQTT file delivery |
| US-D2 | No MqttClient dependency; CloudPublisher implements `IOtaTransport` | MqttClient is CloudPublisherTask-only; CloudPublisher owns topics and serialisation |
| US-D3 | UpdateService has no NVM; FirmwareStore is the single source of persistent OTA state | No second copy to keep consistent; power-loss analysis stays in one place (FS §14) |
| US-D4 | Reset through LifecycleController (`OTA_RESET_REQUESTED`, Machine 1 I18) | One canonical reset site; LifecycleController flushes CloudPublisher first |
| US-D5 | REQ-DM-056 is met by TLS, broker authorisation and image signature; no device-side publisher check | MQTT does not carry the publisher identity; the signature is the real gate |
| US-D6 | "Three retries" = three consecutive failures while connected | A disconnect is REQ-DM-051 (resume), not a failure; one bad patch must not fail a 480-chunk download |
| US-D7 | A stall limit bounds the pause | Otherwise a lost cloud job holds the REQ-DM-054 lock for ever |
| US-D8 | No report before the reset; success only after confirm | Success can only be known on the new image; a report queued in RAM would be lost at the reset |
| US-D9 | UpdateService owns every OTA report, including the rollback (CloudPublisher's `report_rollback_result` removed) | One owner, one topic (`/ota/result`), one payload format with version and count |
| US-D10 | No IHealthReport events | Not in `components.md`; outcomes go to the cloud directly |
| US-D11 | Both slot sizes in the command; the device chooses | XIP slot-specific images (FS-D3); the cloud does not need to know the running slot |
| US-D12 | CloudPublisher hands chunks over in `msg_cb` without its command queue | The 128 B command queue cannot hold 1 KB; the copy is microseconds and keeps the MQTT payload pointer valid |
| US-D13 | UpdateService confirms before reporting success; LifecycleController's confirm is an idempotent re-assert | The success report is never sent for an uncommitted image; the existing LifecycleController code is kept |
| US-D14 | Same `job` sent again = resume; a new `job` = fresh download | Resume across a reboot with no device-side memory (REQ-DM-051) |

---

## 25. Phase H checklist

| # | Check | Status |
|---|---|---|
| H-01 | Every public function specified with pre-conditions, effects and errors | Done (§7) |
| H-02 | Error table with cause, behaviour, caller result and observability | Done (§19) |
| H-03 | Init ordering with pre-conditions | Done (§17) |
| H-04 | Synchronisation strategy; no lock nesting | Done (§16) |
| H-05 | `_reset_for_test()` and single-step hooks under `#ifdef TEST` | Done (§7.1) |
| H-06 | File layout matches the project structure | Done (§21) |
| H-07 | Unit-test plan with TC identifiers covering every function and reason | Done (§22) |
| H-08 | Open items with structured IDs and resolution paths | Done (§24.1) |
| H-09 | Principles section without false claims; non-applicable ones stated | Done (§20) |
| H-10 | SD and state-machine mapping uses the §7 function names | Done (§5, §13.1) |
| H-11 | Machine 3 mapped to code states | Done (§5) |
| H-12 | RAM, ROM and timing estimates | Done (§18) |
| H-13 | SRS traces verified against the sections | Done (§2) |
| H-14 | Decisions logged with rationale | Done (§24.2) |
| H-15 | Wire contract defined for the cloud tool | Done (§6) |
| H-16 | Required changes in other modules and HLD listed | Done (§4) — **B1–B4 before the code PR; LCC and CPC in the same release** |

---

## 26. Implementation notes (as built, v1.0)

### 26.1 Result

| Item | Value |
|---|---|
| Code | `firmware/gateway/application/update_service/` (7 files; `update_service_task.h` is the private header the two `.c` files share) |
| Host tests | 70 cases in four suites: `api` 23, `download` 22, `apply` 12, `boot` 13 (TC-US-001..012, 020..035, 040..049, 060..069, 080, 081 plus the extra cases named in the files) |
| Full projects | Gateway 1271 tests, 0 failures; default project 773 unchanged |
| Hardware | Not run. `integration-tests/update_service/main_test_update_service.c` and `scripts/ota_uart_server.py` implement §23.1 except TC-HW-US-008 (gap meter, stack high-water mark), which stays with FS-O10 / US-O6 |

### 26.2 Deviations from the text above

| # | Companion said | As built | Why |
|---|---|---|---|
| 1 | `post_event(OTA_REQUESTED, job)` | `post_event(lifecycle_event_t)` with `{type, param}` | That is the shipped signature of `ilifecycle_t` |
| 2 | `LC_EVENT_OTA_RESET_REQUESTED` and `LC_EVENT_OTA_FINISHED` come with LCC-1 in the LifecycleController PR | Only these two enumerators (11 and 12) are added to `ilifecycle.h` here; LCC-2..4 stay in that PR | The module cannot compile without them. Existing values are unchanged |
| 3 | Task state and pending work in notification bits | A `pending` mask under the mutex, set before `xTaskNotify` | The notification value is not accumulated by the test mock, and the mask makes the single-step hook exact |
| 4 | The self-check checks the 60 s budget when it polls | The budget is checked on every pass, independent of the poll tick | Found by TC-US-063: a pass landing after the deadline but before the next poll tick delayed the verdict |
| 5 | Lifecycle event refused: retried only for `OTA_RESET_REQUESTED` (§12) | The same one-second retry covers `OTA_FINISHED`, `SELF_CHECK_PASS` and `SELF_CHECK_FAIL` | A lost `OTA_FINISHED` would leave LifecycleController in UPDATING_FW for ever |
| 6 | Transport queue full: "retried 20 times" | 20 attempts in total (the first included), 100 ms apart, then LOG_ERROR | The text was ambiguous; the test pins it |
| 7 | `verify` result used as is | A `verify` that names any slot other than the target is a `staging_error`, before any `install_*` | Defensive: REQ-DM-073 must hold even if FirmwareStore misbehaves (TC-US-049) |
| 8 | Public API as listed | Adds `update_service_reason_string()` (the §8.4 wire strings, for CloudPublisher) and `update_service_state_for_test()` | CloudPublisher serialises the reason; tests assert the state |
| 9 | Operations act on the instance | They act through `self` (checked against the single pool entry) | A static-analysis finding: `self` was only compared, never used |

### 26.3 Open items raised by the implementation

| ID | Item | Resolution path | Status |
|---|---|---|---|
| US-O9 | The bench harness has not been run on the board (TC-HW-US-001..007, 009, 010) | Needs a release key, both slot images, the bootloader and the un-excluded `firmware_store` in a bench-local `.cproject` (see the harness header) | Open |
| US-O10 | `LC_EVENT_OTA_RESET_REQUESTED` / `_FINISHED` exist but LifecycleController does not act on them yet (LCC-2) | **Done 2026-10-27** on `feature/phase-4-gw-lifecycle_ota_events` (LCC-2..4; lifecycle-controller-lld.md LC-D19..D22). Binding in `main()` is LC-O15 | Closed |
| US-O11 | A rejected command is reported but nothing tells LifecycleController when `start_timeout` fires after it accepted `OTA_REQUESTED` and never called `start()` | Check with LCC-2: LifecycleController never takes the event without calling `start()`, so this should not occur | Open |

---

## References

- `docs/components.md` (GW UpdateService, CloudPublisher, LifecycleController)
- `docs/state-machines.md` Machine 1 (UpdatingFirmware, I18, T14, T19), Machine 3
- `docs/sequence-diagrams.md` SD-00c, SD-06a–d
- `docs/task-breakdown.md` (`UpdateServiceTask`, D23, D26)
- `docs/lld/middleware/firmware-store.md` v1.0 (§7, §13, §14, §19, FS-D2..D19)
- `docs/lld/application/lifecycle-controller-lld.md` (LC-D1, LC-D4, LC-D10, LC-O6, LC-O13, LC-O15)
- `docs/lld/application/cloud-publisher-lld.md` (CP-O1, CP-O5, CP-D16)
- `docs/lld/middleware/mqtt-client.md` (MQTT-O3, threading contract)
- `docs/use-case-descriptions.md` UC-18, UC-19, UC-20
- `docs/architecture-principles.md` P1, P3, P4, P6, P7, P8, P10

---

*LLD companion v0.2, produced for Pass H review.*
