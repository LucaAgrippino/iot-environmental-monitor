# LLD Companion — UpdateService

**Board:** Gateway only.
**Layer:** Application.

Orchestrates the OTA firmware update lifecycle — initiation,
authentication, chunk download, signature verification, bank swap,
reboot, self-check, and commit-or-rollback — as specified by Firmware
Update state machine (Machine 3, `state-machines.md` §7.4). Spans up
to two MCU reboots via a flag-based resume protocol.

---

## 1. Component summary

| Field | Value |
|---|---|
| **Provides** | `IUpdateService` *(command intake + lock query)* |
| **Uses** | `IMqttClient`, `IFirmwareStore`, `IResetDriver`, `ILogger` |
| **Hosted in task** | `UpdateServiceTask` priority 4, 1024 words / 4 KB |
| **Activation** | Command queue from `CloudPublisher`; flag-based resume at boot |

**State machine reference.** This companion specifies the implementation
of Machine 3. All state transitions, guards, and internal actions are
normatively defined in `state-machines.md` §7.4 and are not duplicated
here. This document specifies only the *implementation* decisions:
data structures, algorithms, concurrency, and test plan.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Download firmware image | REQ-DM-050 | UC-18, UC-20 |
| Resume from interruption | REQ-DM-051 | UC-18 |
| Delete partial after 3 retries | REQ-DM-052, DM-053 | UC-18 |
| Reject concurrent update | REQ-DM-054 | UC-18 |
| Report result to cloud | REQ-DM-055 | UC-18 |
| Authenticate update command source | REQ-DM-056 | UC-19 |
| Validate firmware image | REQ-DM-060, DM-062 | UC-20 |
| Discard and restore on validation failure | REQ-DM-061 | UC-20 |
| Set inactive bank as boot partition | REQ-DM-070 | UC-18 |
| Trigger self-check on new firmware | REQ-DM-071 | UC-18 |
| Rollback if self-check fails | REQ-DM-072 | UC-18 |
| Retain current firmware until self-check passes | REQ-DM-073 | UC-18 |
| Dual-bank partition scheme | REQ-DM-074 | UC-18 |
| Cryptographic signature verification | REQ-DM-080 | UC-20 |

---

## 3. Interface — `IUpdateService`

```c
typedef struct IUpdateService IUpdateService;

struct IUpdateService {
    void *ctx;

    /* Called by CloudPublisher to initiate an update.
     * Returns immediately after enqueuing the command.
     * Returns US_ERR_BUSY if an update is already in progress (DM-054). */
    us_err_t (*handle_command)(void *ctx, const update_cmd_t *cmd);

    /* Query whether an update is in progress.
     * Used by CloudPublisher to gate restart commands (DM-054).         */
    bool     (*is_busy)       (void *ctx);
};
```

```c
typedef struct {
    char     image_url[256];     /* MQTT chunk-delivery topic or URL      */
    uint32_t expected_size;      /* declared image size in bytes           */
    uint8_t  expected_sha256[32];/* expected SHA-256 digest                */
    uint8_t  signature[64];      /* ECDSA-P256 signature over header      */
} update_cmd_t;
```

---

## 4. Internal state and persistence

### 4.1 Runtime context

```c
typedef enum {
    US_STATE_IDLE        = 0,
    US_STATE_DOWNLOADING = 1,
    US_STATE_VALIDATING  = 2,
    US_STATE_APPLYING    = 3,
    US_STATE_SELF_CHECK  = 4,
    US_STATE_ROLLING_BACK= 5,
    US_STATE_COMMITTED   = 6,
    US_STATE_FAILED      = 7,
} us_state_t;

typedef struct {
    us_state_t    state;
    uint32_t      download_offset;   /* bytes received so far (resumable) */
    uint8_t       retry_count;       /* download attempts, 0..3           */
    update_cmd_t  cmd;               /* copy of the initiating command    */
} us_persistent_t;   /* written to NVM before every reboot               */

typedef struct {
    us_persistent_t  p;             /* persisted fields                   */
    volatile bool    busy;          /* lock — read by CloudPublisher       */
    TaskHandle_t     task_handle;
    QueueHandle_t    cmd_queue;     /* depth 1; CloudPublisher enqueues    */
    IMqttClient     *mqtt;
    IFirmwareStore  *fw_store;
    IResetDriver    *reset;
    ILogger         *log;
    /* Result-report callback to CloudPublisher (DM-055) */
    void (*report_result)(void *cloud_pub_ctx,
                          bool success, const char *detail);
    void *cloud_pub_ctx;
} update_service_t;
```

### 4.2 Persistence region

`us_persistent_t` (~300 bytes) is written to a reserved region in the
on-chip metadata partition (`flash-partition-layout.md` §7.1) before
each reboot. The write uses A/B sector rotation for power-loss safety,
consistent with the metadata partition strategy (D38).

On every boot, `LifecycleController` reads the flags region to detect
`pending_self_check` or `pending_rollback`. If either is set, it
creates `UpdateServiceTask` and enqueues a resume notification before
releasing the start gate.

---

## 5. Two-reboot protocol

### 5.1 First reboot — Applying → SelfChecking

```
Applying state (do activity):
    fw_store->apply()          /* ~500 ms; kicks watchdog per FS-O1      */
    persist(state=SELF_CHECK, pending_self_check=true)
    reset->system_reset()      /* NVIC_SystemReset() via ResetDriver     */
    /* execution does not return here */

On next boot:
    Bootloader reads pending_self_check=true → jumps to new firmware
    LifecycleController detects pending_self_check → resumes UpdateService
    UpdateService enters SelfChecking state
```

### 5.2 Second reboot — RollingBack → Failed (if needed)

```
RollingBack state (entry action):
    fw_store->mark_inactive()   /* invalidate new image metadata         */
    persist(state=FAILED, pending_rollback=true)
    /* Clear pending_self_check so bootloader does not re-enter check    */
    reset->system_reset()

On next boot:
    Bootloader reads pending_rollback=true → switches boot pointer to old bank
    LifecycleController detects pending_rollback → resumes UpdateService
    UpdateService enters Failed state → reports failure (DM-055)
```

The bootloader is the only component that physically switches the boot
bank pointer. `UpdateService` only writes the metadata flags; it never
calls memory-mapping registers directly.

---

## 6. Per-state implementation

### 6.1 Idle

Entry: clear `persistent.state`, clear flags, set `busy = false`.
Waits on `cmd_queue`. On command received: validate authentication
(§7), set `busy = true`, transition to Downloading.

### 6.2 Downloading (REQ-DM-050, -051, -052)

```
enter Downloading:
    if persistent.download_offset == 0:
        fw_store->begin_staging()       /* erase QSPI staging partition  */
    else:
        log_info("Resuming from offset %u", persistent.download_offset)

    subscribe to chunk topic (cmd.image_url)

do activity — chunk receive loop:
    for each MQTT chunk received:
        fw_store->write_chunk(persistent.download_offset,
                              chunk.data, chunk.len)
        persistent.download_offset += chunk.len
        persist_offset()               /* REQ-DM-051 resumability        */
        if download_offset == cmd.expected_size:
            unsubscribe; transition to Validating; break

    if chunk timeout or publish error:
        retry_count++
        if retry_count >= 3:
            fw_store->discard_staging()   /* REQ-DM-052                 */
            log_error("Download failed after 3 attempts")   /* DM-053   */
            transition to Failed
        else:
            reset download (resubscribe, reset offset)
```

`persist_offset()` writes only `persistent.download_offset` and
`persistent.retry_count` to NVM — not the full struct — to minimise
flash wear on every chunk.

### 6.3 Validating (REQ-DM-060, -080)

```
enter Validating:
    rc = fw_store->verify()
    /* Verification order (session decision):
       1. header CRC32
       2. declared image length vs actual bytes received
       3. SHA-256 digest over full image
       4. ECDSA-P256 signature                                            */
    if rc == FS_OK:
        transition to Applying
    else:
        fw_store->discard_staging()
        log_error("Signature/integrity check failed: %d", rc)   /* DM-062 */
        report_result(false, "validation_failed")               /* DM-055 */
        transition to Failed
```

### 6.4 Applying (REQ-DM-070, -071, -073)

```
enter Applying:
    /* fw_store->apply() copies staging → inactive bank on-chip.
     * ~500 ms. Watchdog kicked inside apply() per FS-O1.
     * Current firmware (active bank) is NOT overwritten (DM-073).       */
    rc = fw_store->apply()
    if rc != FS_OK:
        log_error("Flash apply failed: %d", rc)
        report_result(false, "apply_failed")
        transition to Failed; return

    /* Commit: write boot pointer to new bank; set pending_self_check.   */
    persist(state=SELF_CHECK, pending_self_check=true)
    reset->system_reset()
```

### 6.5 SelfChecking (REQ-DM-071, -072)

Entered after the first reboot with new firmware running. Driven by
`LifecycleController`'s SelfChecking sub-step during Init.

```
enter SelfChecking:
    /* LifecycleController already ran sensor init and comms probes.
     * UpdateService queries their results.                               */
    self_check_ok = lifecycle->get_self_check_result()

    if self_check_ok:
        fw_store->clear_pending_self_check()    /* clears NVM flag        */
        persist(state=COMMITTED)
        report_result(true, "committed")        /* DM-055                */
        transition to Committed
    else:
        transition to RollingBack
```

### 6.6 RollingBack (REQ-DM-072)

```
enter RollingBack:
    log_error("Self-check failed — rolling back")
    fw_store->mark_inactive()      /* mark new image as invalid in metadata */
    persist(state=FAILED, pending_rollback=true)
    reset->system_reset()          /* bootloader will switch back to old bank */
```

### 6.7 Committed / Failed (terminal)

Both are terminal states. `UpdateService` sets `busy = false`, persists
the final state, and the task blocks indefinitely. `LifecycleController`
transitions the gateway lifecycle out of `UpdatingFirmware` on the
`update_done` event emitted from these states.

---

## 7. Authentication (REQ-DM-056)

Authentication is two-layer:

1. **TLS X.509** — the MQTT connection itself is mutual TLS; the broker
   verifies the gateway's certificate. Commands cannot arrive from
   unauthenticated sources at the transport level.
2. **Command-level check** in `CloudPublisher` — verifies the command
   arrives on the authorised OTA topic and that the MQTT client
   identity matches the allowed publisher list.

`UpdateService` does not perform additional authentication — it trusts
the command delivered by `CloudPublisher`. This is documented explicitly
so the trust boundary is clear: `CloudPublisher` is responsible for
REQ-DM-056 at the application layer.

---

## 8. Concurrent update rejection (REQ-DM-054)

```c
us_err_t handle_command(void *ctx, const update_cmd_t *cmd)
{
    update_service_t *self = ctx;
    if (self->busy) {
        log_warn("OTA command rejected — update in progress");
        return US_ERR_BUSY;
    }
    xQueueSend(self->cmd_queue, cmd, 0);
    return US_OK;
}
```

`busy` is set to `true` inside `UpdateServiceTask` before the state
machine leaves Idle. `handle_command` is called from
`CloudPublisherTask`; `busy` is declared `volatile` to ensure the read
in `handle_command` sees the latest value written by `UpdateServiceTask`.

---

## 9. Concurrency

| State | Access | Task | Protection |
|---|---|---|---|
| `busy` | Read: `CloudPublisherTask`; Write: `UpdateServiceTask` | Cross-task | `volatile` — single boolean, safe on Cortex-M4 |
| `persistent.*` | `UpdateServiceTask` only | Single-task | None |
| `cmd_queue` | Write: `CloudPublisherTask`; Read: `UpdateServiceTask` | Cross-task | FreeRTOS queue (depth 1) |
| All other fields | `UpdateServiceTask` only | Single-task | None |

`UpdateServiceTask` is the sole executor of the state machine. All
blocking I/O (`fw_store->apply()`, `mqtt->subscribe()`, chunk receives)
runs within it.

---

## 10. Error handling

```c
typedef enum {
    US_OK = 0,
    US_ERR_BUSY,              /* DM-054: concurrent update rejected      */
    US_ERR_AUTH_FAILED,       /* DM-056: handled by CloudPublisher       */
    US_ERR_DOWNLOAD_FAILED,   /* DM-053: 3 retries exhausted             */
    US_ERR_VALIDATION_FAILED, /* DM-062: signature/integrity check       */
    US_ERR_APPLY_FAILED,      /* flash write to inactive bank failed     */
    US_ERR_SELF_CHECK_FAILED, /* DM-072: triggers rollback               */
    US_ERR_NULL_ARG,
    US_ERR_NOT_INIT,
} us_err_t;
```

All terminal errors result in the machine reaching `Failed`, a cloud
failure report (REQ-DM-055), and `busy = false`. The gateway lifecycle
returns to `Operational` running on the previous firmware.

---

## 11. Watchdog (FS-O1)

`fw_store->apply()` takes ~500 ms to erase and program the inactive
on-chip flash bank. The hardware watchdog must be refreshed during this
window. Two options:

- **Option A** — `FirmwareStore.apply()` receives a watchdog-kick
  callback and calls it after each sector write (~2 ms per sector).
- **Option B** — `FirmwareStore.apply()` calls `IWatchdog.kick()`
  directly (Middleware-to-driver call, P1 compliant).

Option A is preferred: `FirmwareStore` remains decoupled from the
watchdog driver; the kick callback is provided by `UpdateService` at
the call site. Tracked as **US-O1** for resolution at the
`FirmwareStore` LLD phase.

---

## 12. Memory and sizing

| Item | Size |
|---|---|
| `update_service_t` context | ~380 B |
| `cmd_queue` (depth 1 × ~300 B) | ~300 B |
| Chunk receive buffer (stack, per MQTT callback) | ~4 KB |
| **Total RAM** | **~4.7 KB** |

Stack: 1024 words / 4 KB. Peak usage during `fw_store->verify()` which
runs SHA-256 + ECDSA-P256 in-place — estimated ~2 KB stack frame.

---

## 13. Initialisation

```c
us_err_t update_service_init(update_service_t     *self,
                             IMqttClient          *mqtt,
                             IFirmwareStore       *fw_store,
                             IResetDriver         *reset,
                             ILogger              *log,
                             void (*report_result)(void *, bool, const char *),
                             void                 *cloud_pub_ctx);
```

After init, `LifecycleController` checks the NVM flags region:
- If `pending_self_check` or `pending_rollback` is set: create
  `UpdateServiceTask` with `busy = true` (update is resuming mid-flight).
- Otherwise: create task with `busy = false` and leave it blocking on
  `cmd_queue`.

---

## 14. Test plan

### 14.1 Unit tests — `tests/application/test_update_service.c`

Mocks: `IMqttClient` (chunk injection), `IFirmwareStore`, `IResetDriver`,
`ILogger`. NVM persistence mocked as in-memory byte array.

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; `busy = false`; state = IDLE |
| Concurrent rejection | `handle_command` while `busy = true` → `US_ERR_BUSY`; second command not queued |
| Happy path — Downloading | Chunks arrive in order; `fw_store->write_chunk` called for each; offset advances; `download_complete` transitions to Validating |
| Resume — offset persistence | Simulate power cycle mid-download (restore from persisted offset); verify download resumes from correct offset, not from 0 |
| Retry — transient failure | One chunk failure; `retry_count++`; re-subscribe; next attempt succeeds |
| Retry exhausted | Three chunk failures; `fw_store->discard_staging` called; transition to Failed; `report_result(false, ...)` called |
| Validation — success | `fw_store->verify()` returns OK; transitions to Applying |
| Validation — failure | `fw_store->verify()` returns error; `discard_staging` called; `report_result(false, ...)` called; transition to Failed |
| Applying — success | `fw_store->apply()` OK; `pending_self_check` written to mock NVM; `reset->system_reset()` called |
| Applying — flash failure | `fw_store->apply()` fails; transition to Failed; reset NOT called |
| SelfChecking — pass | `lifecycle->get_self_check_result()` = true; NVM flag cleared; `report_result(true, ...)` called; transition to Committed |
| SelfChecking — fail | result = false; transition to RollingBack; `pending_rollback` written; reset called |
| RollingBack | `fw_store->mark_inactive()` called; `pending_rollback` set; reset called |
| Boot-resume — SelfChecking | Init with `pending_self_check=true`; verify task starts in SelfChecking, not Idle |
| Boot-resume — Failed | Init with `pending_rollback=true`; verify task starts in Failed; `report_result(false,...)` called |

### 14.2 Integration tests — on target

| Test | Setup |
|---|---|
| End-to-end happy path | Publish valid OTA command via MQTT; verify Committed state, new firmware version reported to cloud |
| Concurrent rejection | Send two simultaneous OTA commands; verify second is rejected with `US_ERR_BUSY` |
| Self-check failure + rollback | Inject a self-check failure; verify gateway reboots, returns to old firmware, reports failure |
| Download resume | Kill WiFi mid-download; restore; verify download continues from persisted offset, not restart |
| Signature rejection | Corrupt the image SHA-256; verify validation fails, staging discarded, failure reported |

---

## 15. Open items

| ID | Item |
|---|---|
| **US-O1** | Watchdog kick during `fw_store->apply()` — Option A (callback) vs Option B (direct IWatchdog). Resolve at `FirmwareStore` LLD phase. |
| **US-O2** | `ILifecycle.get_self_check_result()` — this method does not yet appear in `LifecycleController`'s interface. Add it in a follow-up `lifecycle-controller.md` update. |
| **US-O3** | NVM persistence region within the metadata partition — exact byte offsets for `us_persistent_t` to be defined in `flash-partition-layout.md` §10 (LLD handoff section). |
| **US-O4** | Maximum chunk size and chunk topic format — defined by the cloud-side OTA job infrastructure (AWS IoT Jobs or custom). Document in the cloud integration guide; `UpdateService` must be configured with matching sizes at init. |

---

## 16. References

- `docs/components.md` (GW UpdateService entry).
- `docs/state-machines.md` §7.4 (Machine 3 — normative state definition).
- `docs/sequence-diagrams.md` SD-06a–d (OTA lifecycle sequences).
- `docs/flash-partition-layout.md` §5, §7 (dual-bank layout, bootloader contract, metadata partition).
- `docs/lld/firmware-store.md` (defines `IFirmwareStore`).
- `docs/architecture-principles.md` P1 (no direct driver calls from Application), P8 (no dynamic alloc).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
