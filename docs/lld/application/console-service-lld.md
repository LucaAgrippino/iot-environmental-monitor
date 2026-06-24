# LLD Companion — ConsoleService

**Boards:** Field Device (FD) + Gateway (GW).
**Layer:** Application.

This artefact specifies the software design of the `ConsoleService`
component — command parsing, dispatch, provisioning workflow,
operational configuration workflow, self-test execution, and
board-specific command sets.

**Version:** 0.2
**Date:** June 2026
**Status:** Ready for Pass H review

**HLD anchor:** ConsoleService in `components.md` (FD + GW application layer)

---

## 1. Sources

| Field | FD | GW |
|---|---|---|
| **Provides** | `IConsoleService` (init, run loop entry) | `IConsoleService` (init, run loop entry) |
| **Uses** | `IDebugUart`, `ISensorService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `ILogger` | `IDebugUart`, `ISensorService`, `IConfigProvider`, `IConfigManager`, `IDeviceProfileManager`, `IHealthSnapshot`, `ILogger` |
| **Hosted in task** | `ConsoleTask` prio 1, 512 words / 2 KB | `ConsoleTask` prio 1, 512 words / 2 KB |
| **Activation** | Event — UART RX byte notify | Event — UART RX byte notify |

**Note — GW `IConfigManager` gap.** The `components.md` entry for the GW
`ConsoleService` omits `IConfigManager`. This is a component-spec
oversight: provisioning writes (WiFi credentials, MQTT endpoint,
Modbus address) are applied via `IConfigManager`. Tracked as
**CS-O4** for a follow-up `components.md` correction.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Serial console parameters | REQ-LI-000 | UC-04, UC-16 |
| Diagnostic command execution | REQ-LI-010, LI-020, LI-0E1 | UC-04 |
| Provisioning menu | REQ-LI-030 | UC-16 |
| WiFi credentials *(GW only)* | REQ-LI-040 | UC-16 |
| Cloud endpoint *(GW only)* | REQ-LI-050 | UC-16 |
| Cloud certificates *(GW only)* | REQ-LI-060 | UC-16 |
| Modbus address | REQ-LI-070 | UC-16 |
| Serial port parameters | REQ-LI-080 | UC-16 |
| Input validation | REQ-LI-090, LI-0E2 | UC-16 |
| Confirmation flow | REQ-LI-100, LI-110, LI-120, LI-0E3 | UC-16 |
| Self-test command | REQ-LI-130, LI-140, LI-160 | UC-04 |
| Device serial number | REQ-LI-150 | UC-04 |
| Config persistence | REQ-DM-090 | UC-16 |

---

## 3. Responsibility

`ConsoleService` owns the operator-facing serial console. It reads
bytes from `IDebugUart`, assembles them into lines, dispatches each
line to a handler from a static command table, and writes responses
back through `IDebugUart`. It does not own any peripheral state — all
diagnostic data, config reads/writes, and self-test evidence are
obtained through injected interfaces.

The component runs in a dedicated FreeRTOS task (`ConsoleTask`) at
priority 1, activated by a direct-to-task notify from the
`IDebugUart` RX ISR. Between notifications the task is blocked,
consuming no CPU.

---

## 4. Provided interface

### 4.1 Singleton accessor

```c
extern const iconsole_service_t * const console_service;
```

### 4.2 Vtable

```c
typedef struct iconsole_service {
    console_service_err_t (*run_once)(void);
} iconsole_service_t;
```

`run_once()` performs one drain-parse-dispatch cycle. `ConsoleTask`
calls it in an infinite loop after waiting on the RX notify.

### 4.3 Initialisation function

Not part of the vtable. Called once by `LifecycleController` during
boot, before the task that hosts the service is created.

```c
console_service_err_t
console_service_init(const idebug_uart_t          *uart,
                     const isensor_service_t      *sensors,
                     const iconfig_provider_t     *cfg_read,
                     const iconfig_manager_t      *cfg_write,
                     const ihealth_snapshot_t     *health,
                     const idevice_profile_mgr_t  *profiles, /* GW only; NULL on FD */
                     const ilogger_t              *log);
```

No `self` parameter — follows the project's singleton vtable pattern.

### 4.4 Task entry

```c
void console_task_body(void *arg);
```

`xTaskCreateStatic` references this. Internally waits on the RX
notify and calls `console_service->run_once()` per iteration.

---

## 5. Data types

### 5.1 Public error enum

```c
typedef enum {
    CONSOLE_SERVICE_ERR_OK              = 0,
    CONSOLE_SERVICE_ERR_NULL_ARG        = 1,
    CONSOLE_SERVICE_ERR_NOT_INIT        = 2,
    CONSOLE_SERVICE_ERR_VALIDATION      = 3,
    CONSOLE_SERVICE_ERR_UNKNOWN_KEY     = 4,
    CONSOLE_SERVICE_ERR_APPLY_FAILED    = 5,
    CONSOLE_SERVICE_ERR_TIMEOUT         = 6,
    CONSOLE_SERVICE_ERR_LINE_OVERFLOW   = 7,
} console_service_err_t;
```

### 5.2 Command table entry

```c
typedef console_service_err_t (*cmd_handler_t)(int argc, const char *argv[]);

typedef struct {
    const char    *token;
    uint8_t        min_argc;
    uint8_t        max_argc;
    cmd_handler_t  handler;
    const char    *help;
} cmd_entry_t;
```

The command table is a `static const cmd_entry_t s_cmd_table[]` — read
from `.rodata`, no RAM cost.

### 5.3 Pending-change staging structs

```c
typedef struct {
    bool     dirty;                /* at least one field staged    */
    char     wifi_ssid[64];        /* GW only                      */
    char     wifi_pass[64];        /* GW only                      */
    char     mqtt_endpoint[128];   /* GW only                      */
    uint8_t  modbus_slave_addr;
    uint32_t modbus_baud;
    char     modbus_parity;        /* 'N', 'E', 'O'                */
} prov_pending_t;

typedef struct {
    bool     dirty;
    uint32_t polling_interval_ms;
    int16_t  temp_alarm_hi_deci_c;
    /* ... all operational fields, mirroring lcd_ui_editable_params_t */
} cfg_pending_t;
```

### 5.4 Self-test result

```c
typedef struct {
    uint64_t timestamp_unix;
    bool     sensor_pass;
    bool     comms_pass;
    bool     flash_pass;
    bool     overall_pass;
} selftest_result_t;
```

Persisted via `IConfigManager.store_selftest_result()` and retrieved
via `IConfigManager.get_selftest_result()`.

---

## 6. Internal state

All state is file-scope static — no heap, no per-instance context.

```c
static const idebug_uart_t          *s_uart;
static const isensor_service_t      *s_sensors;
static const iconfig_provider_t     *s_cfg_read;
static const iconfig_manager_t      *s_cfg_write;
static const ihealth_snapshot_t     *s_health;
static const idevice_profile_mgr_t  *s_profiles;  /* NULL on FD */
static const ilogger_t              *s_log;

static char     s_line_buf[CONSOLE_LINE_MAX_LEN];   /* 256 B */
static uint16_t s_line_len;
static uint8_t  s_ring_buf[CONSOLE_RING_BUF_SIZE];  /* 256 B, power of 2 */
static uint16_t s_ring_head;
static uint16_t s_ring_tail;
static bool     s_ring_overflow_flag;

static prov_pending_t s_prov_pending;
static cfg_pending_t  s_cfg_pending;

static bool s_initialised;
```

`CONSOLE_LINE_MAX_LEN = 256`, `CONSOLE_RING_BUF_SIZE = 256` (must be
power of 2 for masked-index arithmetic).

---

## 7. UART framing and task loop

```
DebugUartDriver ISR:
    place byte in s_ring_buf (lock-free; single producer, single consumer)
    notify ConsoleTask (direct-to-task, bit 0)

ConsoleTask loop:
    wait on notify (no timeout — fully event-driven)
    drain s_ring_buf into s_line_buf until '\n'
    if '\n' received:
        parse_and_dispatch(s_line_buf)
        s_line_len = 0
        print prompt (fd> / gw>)
```

**Ring buffer overflow.** If the ISR cannot enqueue, it discards the
incoming byte and sets `s_ring_overflow_flag`. Next prompt prints
`[WARN] RX overflow` and clears the flag.

**Line overflow.** Lines exceeding `CONSOLE_LINE_MAX_LEN` are
discarded with `[ERR] line overflow`.

**Serial parameters.** 115200 baud, 8N1, no flow control (REQ-LI-000).
Configured at init; `DebugUartDriver` owns the register-level setup.

---

## 8. Command catalogue

### 8.1 Shared commands (FD + GW)

| Token | Args | Handler behaviour | Traces to |
|---|---|---|---|
| `help` | 0 | Print all command tokens and one-line descriptions | — |
| `version` | 0 | Print firmware version string from `shared/firmware_version.h` | — |
| `serial` | 0 | Print MCU UID (12 bytes, hex-formatted) | REQ-LI-150 |
| `sensors` | 0 | Print latest sensor reading (value, unit, timestamp, valid) via `ISensorService` | REQ-LI-010 |
| `status` | 0 | Print health snapshot via `IHealthSnapshot` | REQ-LI-010 |
| `alarms` | 0 | Print active alarms list, or `No active alarms` | REQ-LI-010 |
| `selftest` | 0 | Run board self-test; print pass/fail per subsystem; persist result | REQ-LI-130, LI-140, LI-160 |
| `selftest-result` | 0 | Print most recently stored self-test result | REQ-LI-160 |
| `config list` | 0 | Print all current operational parameters from `IConfigProvider` | REQ-LI-010 |
| `config set <k> <v>` | 2 | Stage an operational parameter change | REQ-LI-010 |
| `config commit` | 0 | Confirm and apply staged operational changes | REQ-LI-100..120 |
| `config discard` | 0 | Discard staged operational changes | REQ-LI-0E3 |
| `prov set <k> <v>` | 2 | Stage a provisioning parameter | REQ-LI-030..080 |
| `prov commit` | 0 | Confirm and apply staged provisioning changes | REQ-LI-100..120 |
| `prov discard` | 0 | Discard staged provisioning changes | REQ-LI-0E3 |

### 8.2 Board-specific commands

Compiled in only for the relevant board using
`#if defined(BOARD_GATEWAY)` / `#if defined(BOARD_FIELD_DEVICE)`.

| Token | Board | Purpose |
|---|---|---|
| `profiles list` | GW | Print all registered device profiles via `IDeviceProfileManager` |
| `profiles add` | GW | Add or update a device profile (key=value pairs) |
| `profiles remove` | GW | Remove a device profile by slave address |
| `wifi status` | GW | Print WiFi link state and RSSI from `IHealthSnapshot` |
| `mqtt status` | GW | Print MQTT connection state and counters from `IHealthSnapshot` |
| `modbus status` | FD | Print Modbus slave counters (CRC, timeout, success) from `IHealthSnapshot` |

### 8.3 Parse and dispatch algorithm

```
parse_and_dispatch(line):
    tokenise line by whitespace → argv[], argc
    if argc == 0: return (empty line, just print prompt)
    for each entry in s_cmd_table:
        if strcmp(argv[0], entry.token) == 0:
            if (argc - 1) < entry.min_argc or (argc - 1) > entry.max_argc:
                print "[ERR] usage: " + entry.help
                return CONSOLE_SERVICE_ERR_VALIDATION
            rc = entry.handler(argc, argv)
            if rc != CONSOLE_SERVICE_ERR_OK:
                print "[ERR] " + error_string(rc)
            return rc
    print "[ERR] unknown command '" + argv[0] + "'. Type 'help'."
    return CONSOLE_SERVICE_ERR_UNKNOWN_KEY
```

Lookup is a linear scan on the first token (~20 entries; worst-case
scan is negligible at human typing speeds — P5 micro-optimisation
not warranted).

---

## 9. Provisioning workflow (UC-16, REQ-LI-030..-120)

### 9.1 `prov set <key> <value>`

```
cmd_prov_set(argv):
    key, value = argv[1], argv[2]
    validate format and range of value per §9.4
    if invalid:
        print "[ERR] " + validation_message  (REQ-LI-0E2)
        return CONSOLE_SERVICE_ERR_VALIDATION
    update s_prov_pending[key] = value
    s_prov_pending.dirty = true
    print "[OK] staged: " + key + " = " + value
    return CONSOLE_SERVICE_ERR_OK
```

### 9.2 `prov commit`

```
cmd_prov_commit():
    if not s_prov_pending.dirty:
        print "[INFO] Nothing staged."
        return CONSOLE_SERVICE_ERR_OK
    print staged changes summary
    print "Apply? [y/N]: "
    response = read_char_with_timeout(30 seconds)
    if response != 'y' and response != 'Y':
        discard s_prov_pending  (REQ-LI-0E3)
        print "[INFO] Discarded."
        return CONSOLE_SERVICE_ERR_OK
    rc = s_cfg_write->apply_prov_block(&s_prov_pending)
    if rc != CONFIG_SERVICE_ERR_OK:
        print "[ERR] Apply failed: " + cfg_error_string(rc)
        /* pending retained until next discard (REQ-LI-120) */
        return CONSOLE_SERVICE_ERR_APPLY_FAILED
    s_prov_pending.dirty = false
    print "[OK] Provisioning applied and persisted."
    return CONSOLE_SERVICE_ERR_OK
```

`apply_prov_block()` validates internally and writes to `ConfigStore`.
On success, the change is immediately live (REQ-LI-110, REQ-DM-090).
On failure, `pending` is not cleared — the operator can correct the
staged value and commit again (REQ-LI-120).

### 9.3 `prov discard`

Clears `s_prov_pending`, sets `dirty = false`, prints
`[INFO] Discarded.` (REQ-LI-0E3).

### 9.4 Validation rules

| Key | Validation |
|---|---|
| `wifi-ssid` | 1..63 printable chars |
| `wifi-pass` | 8..63 printable chars |
| `mqtt-endpoint` | valid hostname or IPv4, ≤128 chars |
| `modbus-addr` | integer 1..247 |
| `modbus-baud` | one of {1200, 2400, 4800, 9600, 19200} |
| `modbus-parity` | one of {N, E, O} |

Unknown keys return `CONSOLE_SERVICE_ERR_UNKNOWN_KEY` immediately
(REQ-LI-0E2).

---

## 10. Operational config workflow

`config set / commit / discard` follow the same algorithm as §9 with
`s_cfg_pending` as the staging struct and
`s_cfg_write->apply_cfg_block()` as the apply call. Applies Tier 2
operational parameters (polling interval, alarm thresholds, display
settings).

---

## 11. Self-test (REQ-LI-130..-160)

`ConsoleService` does not invoke drivers directly. Three sources for
self-test evidence:

| Subsystem | Method | Evidence |
|---|---|---|
| Sensors | `s_sensors->force_read()` | Returns a `sensor_reading_t`; `valid == true` = pass |
| Comms links | `s_health->get()` | `modbus_slave_ok`, `wifi_ok`, `mqtt_ok` flags |
| Flash | `s_cfg_write->round_trip_test()` | Writes a known token, reads it back; match = pass |

```
cmd_selftest():
    selftest_result_t r;
    r.timestamp_unix = time_now()
    r.sensor_pass    = (s_sensors->force_read().valid == true)
    snap = s_health->get()
    r.comms_pass     = board_comms_ok(snap)
    r.flash_pass     = (s_cfg_write->round_trip_test() == CONFIG_SERVICE_ERR_OK)
    r.overall_pass   = r.sensor_pass && r.comms_pass && r.flash_pass

    print formatted table (subsystem | PASS/FAIL)
    print "Overall: " + (r.overall_pass ? "PASS" : "FAIL")

    /* Persist (REQ-LI-160) */
    s_cfg_write->store_selftest_result(&r)
    return CONSOLE_SERVICE_ERR_OK
```

`board_comms_ok()` is a compile-time selected inline:

- FD: `return snap.modbus_slave_ok;`
- GW: `return snap.wifi_ok && snap.mqtt_ok && snap.modbus_master_ok;`

**Self-test depth note.** This implementation satisfies REQ-LI-130 at
HLD self-test depth. A more invasive test (e.g., UART loopback, sector
erase/write verify) would require direct driver access not currently
in `ConsoleService`'s interface list. Tracked as **CS-O5**.

---

## 12. Sequence integration

| SD | Component role | Key function |
|---|---|---|
| SD-10 | Field Technician enters provisioning commands; `ConsoleService` parses each and calls `s_cfg_write->apply_prov_block()` or `s_profiles->add_profile()` | `console_task_body()`, `cmd_prov_commit()` |

See `docs/sequence-diagrams.md` SD-08 (provisioning flow, UC-16).

---

## 13. Thread safety

`ConsoleService` state (`s_line_buf`, `s_line_len`, `s_prov_pending`,
`s_cfg_pending`) is accessed only within `ConsoleTask`. No mutex
needed for internal state.

`s_ring_buf` is a single-producer / single-consumer lock-free ring.
The `DebugUartDriver` RX ISR writes (`s_ring_head`); `ConsoleTask`
reads (`s_ring_tail`). Power-of-2 size enables masked-index
arithmetic without locks.

Provider calls (`s_sensors->force_read()`, `s_health->get()`,
`s_cfg_write->apply_*()`) are individually thread-safe inside each
provider. `ConsoleTask` does not hold locks while issuing these
calls.

---

## 14. Initialisation order

`LifecycleController` calls `console_service_init()` during the
post-driver, pre-task-creation phase:

```
drivers → middleware → application services:
    ConfigService → SensorService → AlarmService → HealthMonitor
                  → ConsoleService             ← here
→ task creation → scheduler start
```

On return from `console_service_init()`:

- All injected interface pointers stored in file-scope statics.
- `s_line_buf`, `s_ring_buf`, `s_prov_pending`, `s_cfg_pending`
  zero-initialised.
- `s_initialised = true`.
- Prompt printed via `s_uart`.

`console_task_body` is then registered via `xTaskCreateStatic` by
`LifecycleController`. The task begins running when the scheduler
starts.

---

## 15. Memory and sizing

| Item | Size |
|---|---|
| Injected interface pointers (×7) | 28 B |
| `s_line_buf` | 256 B |
| `s_ring_buf` | 256 B |
| `s_prov_pending` | ~320 B |
| `s_cfg_pending` | ~48 B |
| Scalar state (head, tail, len, flags, initialised) | ~16 B |
| **Total RAM** | **~924 B** |
| Command table (~20 × 24 B) `.rodata` | ~480 B |
| Task stack (FreeRTOS, static) | 2048 B |

No dynamic allocation post-init (P5).

---

## 16. Error and fault behaviour

All public functions return `console_service_err_t`; callers must not
ignore non-OK returns. All handler errors are mapped to a
human-readable `[ERR]` line on the debug-UART console before
returning. Errors do not affect `ConsoleTask` stability — the prompt
is always reprinted after any command outcome.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `CONSOLE_SERVICE_ERR_NULL_ARG` | Null pointer argument to init or vtable call | Return error; no action | Non-OK return | No retry — programming error | Logged at ERROR via ILogger |
| `CONSOLE_SERVICE_ERR_NOT_INIT` | Function called before `console_service_init()` | Return error | Non-OK return | No retry — programming error | Logged at ERROR via ILogger |
| `CONSOLE_SERVICE_ERR_VALIDATION` | Input value out of range or wrong format | `[ERR] invalid value` printed; return error | Non-OK return | No retry — operator must re-enter valid value | Logged at DEBUG via ILogger |
| `CONSOLE_SERVICE_ERR_UNKNOWN_KEY` | Unknown command token or unknown prov/config key | `[ERR] unknown` printed; return error | Non-OK return | No retry — operator must correct | Logged at WARN via ILogger |
| `CONSOLE_SERVICE_ERR_APPLY_FAILED` | `IConfigManager.apply_*()` returned non-OK | `[ERR] apply failed` printed; pending retained | Non-OK return | Operator may re-enter the command | ConfigService logs underlying error; ConsoleService logs at WARN |
| `CONSOLE_SERVICE_ERR_TIMEOUT` | Confirm prompt timed out (no `y` within 30 s) | Command aborted; `[ERR] timed out` printed; pending discarded | Non-OK return | Operator must restart the command | Logged at DEBUG via ILogger |
| `CONSOLE_SERVICE_ERR_LINE_OVERFLOW` | Incoming line exceeded `CONSOLE_LINE_MAX_LEN` | Line buffer reset; `[ERR] line overflow` printed | Non-OK return | Operator must re-enter | Logged at WARN via ILogger |

---

## 17. Principles applied

- **P1 (Strict directional layering).** Depends on `DebugUartDriver`
  (driver layer) for byte I/O; middleware and application command
  targets consumed via injected vtable pointers. No upward
  dependencies.
- **P2 (Dependency Inversion).** Exposes `iconsole_service_t`
  vtable; `LifecycleController` depends on `IConsoleService`; command
  targets consumed via their respective interfaces — no concrete-type
  dependency.
- **P4 (Cross-cutting concern exception).** `Logger` and
  `TimeProvider` referenced concretely per the cross-cutting
  exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static
  input buffer, ring buffer, pending structs. Command dispatch table
  is a `const` array in `.rodata`. No heap.
- **P6 (Responsibility traces to requirements).** Every CLI command
  in §8 traces to a specific `REQ-LI-*` or `REQ-DM-*` requirement —
  see §2 table.
- **P7 (Pull-based access).** `ConsoleService` pulls data from
  injected providers (`ISensorService.force_read`,
  `IHealthSnapshot.get`, `IConfigProvider.get_params`) at command
  time. No subscriptions, no callbacks from providers.
- **P8 (Total error propagation, no silent failures).**
  `console_service_err_t` on init and `run_once`; command execution
  errors printed to console and returned to caller per §16.
- **P9 (BARR-C coding standard).** Command-code enums `uint8_t`;
  buffer length `uint16_t`; no floating-point; designated
  initialisers throughout.
- **P10 (Naming conventions).** Prefix `console_service_`; interface
  `IConsoleService` → `iconsole_service_t`; errors
  `CONSOLE_SERVICE_ERR_*`.

P3 (Interface Segregation) does not apply — `ConsoleService` exposes
a single-method vtable; no class of consumers requires only a subset.

---

## 18. Unit test plan (Pass H)

File: `tests/field-device/application/console_service/test_console_service.c`
(plus `tests/gateway/application/console_service/test_console_service_gw.c`
for board-specific commands).

All tests use CMock-generated mocks for `IDebugUart`, `ISensorService`,
`IConfigProvider`, `IConfigManager`, `IHealthSnapshot`,
`IDeviceProfileManager`, `ILogger`. UART output is captured via the
mocked `IDebugUart.write()` — assertions on output text use
`TEST_ASSERT_EQUAL_STRING` against the captured buffer.

Test isolation: `console_service_reset_for_test()` (TEST-only)
zeroes file-scope statics between cases.

### 18.1 Initialisation suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-001 | `console_service_init(NULL, …)` → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-002 | Null sensors pointer → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-003 | Null cfg_read pointer → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-004 | Null cfg_write pointer → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-005 | Null health pointer → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-006 | Null logger pointer → `CONSOLE_SERVICE_ERR_NULL_ARG` | P8 |
| TC-CS-007 | FD build accepts `profiles == NULL` → `CONSOLE_SERVICE_ERR_OK` | — |
| TC-CS-008 | GW build rejects `profiles == NULL` → `CONSOLE_SERVICE_ERR_NULL_ARG` | — |
| TC-CS-009 | After init: `s_line_buf` zeroed, `s_line_len == 0` | — |
| TC-CS-010 | After init: `s_prov_pending.dirty == false`, `s_cfg_pending.dirty == false` | — |
| TC-CS-011 | After init: prompt `fd> ` (or `gw> `) written to UART | REQ-LI-000 |
| TC-CS-012 | `run_once` before init → `CONSOLE_SERVICE_ERR_NOT_INIT` | P8 |

### 18.2 Line parser suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-020 | Empty line (only `\n`) → no handler invoked, fresh prompt printed | — |
| TC-CS-021 | Single valid token (`help`) → corresponding handler invoked | — |
| TC-CS-022 | Multi-token line with arguments → handler receives correct argc/argv | — |
| TC-CS-023 | Unknown token → `[ERR] unknown command` printed, `CONSOLE_SERVICE_ERR_UNKNOWN_KEY` returned | REQ-LI-0E2 |
| TC-CS-024 | argc below `min_argc` → `[ERR] usage:` printed, no handler invoked | — |
| TC-CS-025 | argc above `max_argc` → `[ERR] usage:` printed, no handler invoked | — |
| TC-CS-026 | Line of exactly `CONSOLE_LINE_MAX_LEN - 1` bytes + `\n` → dispatched normally | — |
| TC-CS-027 | Line of `CONSOLE_LINE_MAX_LEN` bytes without `\n` → discarded, `[ERR] line overflow` printed, `CONSOLE_SERVICE_ERR_LINE_OVERFLOW` returned | REQ-LI-0E2 |
| TC-CS-028 | Tokenisation collapses multiple spaces and tabs into one separator | — |
| TC-CS-029 | Leading/trailing whitespace ignored | — |
| TC-CS-030 | `\r\n` line ending behaves identically to `\n` | — |
| TC-CS-031 | Ring buffer overflow flag → next prompt prints `[WARN] RX overflow` and clears flag | — |

### 18.3 Shared diagnostic commands

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-040 | `help` prints every token registered in `s_cmd_table` | — |
| TC-CS-041 | `help` prints `entry.help` text per token | — |
| TC-CS-042 | `version` prints `FW_VERSION_STRING` | — |
| TC-CS-043 | `serial` prints 12-byte UID in lowercase hex (`xxxxxxxx-xxxxxxxx-xxxxxxxx`) | REQ-LI-150 |
| TC-CS-044 | `sensors` invokes `ISensorService.get_latest`; prints value, unit, timestamp when `valid == true` | REQ-LI-010 |
| TC-CS-045 | `sensors` prints `INVALID` marker when `valid == false` | REQ-LI-010 |
| TC-CS-046 | `status` invokes `IHealthSnapshot.get`; prints all health fields | REQ-LI-010 |
| TC-CS-047 | `alarms` with all states `CLEAR` → prints `No active alarms` | REQ-LI-010 |
| TC-CS-048 | `alarms` with one `ACTIVE_HIGH` → prints one entry showing sensor name and threshold | REQ-LI-010 |
| TC-CS-049 | `config list` invokes `IConfigProvider.get_params`; prints every parameter | REQ-LI-010 |

### 18.4 Operational `config` workflow

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-060 | `config set polling-interval-ms 1000` (valid) → `s_cfg_pending.polling_interval_ms == 1000`, `dirty == true`, `[OK] staged` printed | REQ-LI-010 |
| TC-CS-061 | `config set polling-interval-ms abc` (non-numeric) → `dirty` unchanged, `[ERR] invalid value` printed, `CONSOLE_SERVICE_ERR_VALIDATION` returned | REQ-LI-090, REQ-LI-0E2 |
| TC-CS-062 | `config set unknown-key 42` → `dirty` unchanged, `[ERR] unknown key` printed, `CONSOLE_SERVICE_ERR_UNKNOWN_KEY` returned | REQ-LI-0E2 |
| TC-CS-063 | `config commit` with `dirty == false` → `[INFO] Nothing staged` printed, no `apply_cfg_block` call | — |
| TC-CS-064 | `config commit` with `y` confirm → `IConfigManager.apply_cfg_block` called once with staged values, `dirty == false`, `[OK] applied` printed | REQ-LI-100, REQ-LI-110 |
| TC-CS-065 | `config commit` with `n` confirm → `apply_cfg_block` NOT called, pending zeroed, `dirty == false`, `[INFO] Discarded` printed | REQ-LI-0E3 |
| TC-CS-066 | `config commit` with no response within 30 s → same outcome as decline | REQ-LI-0E3, CS-O7 |
| TC-CS-067 | `config commit` with `apply_cfg_block` returning error → `dirty == true` retained, `[ERR] apply failed` printed, `CONSOLE_SERVICE_ERR_APPLY_FAILED` returned | REQ-LI-120 |
| TC-CS-068 | `config discard` → pending zeroed, `dirty == false`, `[INFO] Discarded` printed | REQ-LI-0E3 |

### 18.5 `prov` workflow (provisioning)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-080 | `prov set wifi-ssid MyNetwork` → `s_prov_pending.wifi_ssid == "MyNetwork"`, `dirty == true`, `[OK] staged` printed (GW only) | REQ-LI-040 |
| TC-CS-081 | `prov set wifi-ssid ""` → `[ERR] invalid value`, `dirty` unchanged | REQ-LI-090 |
| TC-CS-082 | `prov set wifi-ssid <64-char string>` → `[ERR] invalid value`, `dirty` unchanged | REQ-LI-090 |
| TC-CS-083 | `prov set mqtt-endpoint broker.example.com` → staged, `dirty == true` (GW only) | REQ-LI-050 |
| TC-CS-084 | `prov set mqtt-endpoint <129-char string>` → `[ERR] invalid value` | REQ-LI-090 |
| TC-CS-085 | `prov set modbus-addr 50` → staged, `dirty == true` | REQ-LI-070 |
| TC-CS-086 | `prov set modbus-addr 0` → `[ERR] invalid value` | REQ-LI-090 |
| TC-CS-087 | `prov set modbus-addr 248` → `[ERR] invalid value` | REQ-LI-090 |
| TC-CS-088 | `prov set modbus-baud 9600` → staged | REQ-LI-080 |
| TC-CS-089 | `prov set modbus-baud 7200` (not in allow-list) → `[ERR] invalid value` | REQ-LI-090 |
| TC-CS-090 | `prov set modbus-parity N` / `E` / `O` → staged | REQ-LI-080 |
| TC-CS-091 | `prov set modbus-parity X` → `[ERR] invalid value` | REQ-LI-090 |
| TC-CS-092 | `prov set unknown-key foo` → `[ERR] unknown key`, `CONSOLE_SERVICE_ERR_UNKNOWN_KEY` | REQ-LI-0E2 |
| TC-CS-093 | `prov commit` with `dirty == false` → `[INFO] Nothing staged`, no `apply_prov_block` call | — |
| TC-CS-094 | `prov commit` with `y` → `IConfigManager.apply_prov_block` called once with staged buffer, `dirty == false`, `[OK] applied and persisted` | REQ-LI-100, REQ-LI-110, REQ-DM-090 |
| TC-CS-095 | `prov commit` with `n` → `apply_prov_block` NOT called, pending zeroed | REQ-LI-0E3 |
| TC-CS-096 | `prov commit` with 30 s timeout → same as decline | REQ-LI-0E3 |
| TC-CS-097 | `prov commit` with `apply_prov_block` returning error → `dirty == true` retained, `[ERR] apply failed` | REQ-LI-120 |
| TC-CS-098 | `prov discard` → pending zeroed, `dirty == false`, `[INFO] Discarded` | REQ-LI-0E3 |

### 18.6 Self-test suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-110 | `selftest` all pass → `sensor_pass`, `comms_pass`, `flash_pass`, `overall_pass` all `true`; `store_selftest_result` called once | REQ-LI-130, REQ-LI-160 |
| TC-CS-111 | `selftest` sensor probe returns `valid == false` → `sensor_pass == false`, `overall_pass == false`, result still persisted | REQ-LI-140 |
| TC-CS-112 | `selftest` (FD) with `modbus_slave_ok == false` → `comms_pass == false`, `overall_pass == false` | REQ-LI-140 |
| TC-CS-113 | `selftest` (GW) with `wifi_ok == false` → `comms_pass == false`, `overall_pass == false` | REQ-LI-140 |
| TC-CS-114 | `selftest` (GW) with `mqtt_ok == false` → `comms_pass == false`, `overall_pass == false` | REQ-LI-140 |
| TC-CS-115 | `selftest` `round_trip_test` returning error → `flash_pass == false`, `overall_pass == false` | REQ-LI-140 |
| TC-CS-116 | `selftest` prints formatted table with one row per subsystem | REQ-LI-130 |
| TC-CS-117 | `selftest-result` calls `IConfigManager.get_selftest_result` and prints last stored result | REQ-LI-160 |
| TC-CS-118 | `selftest-result` with no stored result → prints `No self-test result stored` | REQ-LI-160 |

### 18.7 GW board-specific commands (`tests/gateway/…`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-130 | `profiles list` calls `IDeviceProfileManager.list_profiles`; prints each entry | — |
| TC-CS-131 | `profiles list` with empty registry → prints `No profiles registered` | — |
| TC-CS-132 | `profiles add addr=5 sensors=temp,hum` → parses key=value pairs, calls `IDeviceProfileManager.add_profile` | CS-O8 |
| TC-CS-133 | `profiles add` with malformed key=value → `[ERR] invalid format` | REQ-LI-0E2 |
| TC-CS-134 | `profiles remove 5` → calls `IDeviceProfileManager.remove_profile(5)` | — |
| TC-CS-135 | `profiles remove 0` → `[ERR] invalid value` (Modbus addr range) | REQ-LI-090 |
| TC-CS-136 | `wifi status` prints link state and RSSI from `IHealthSnapshot` | REQ-LI-010 |
| TC-CS-137 | `mqtt status` prints connection state and counters from `IHealthSnapshot` | REQ-LI-010 |

### 18.8 FD board-specific commands

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-150 | `modbus status` prints slave counters (CRC errors, timeouts, success count) from `IHealthSnapshot` | REQ-LI-010 |

### 18.9 Cross-cutting

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-CS-170 | Every command execution returns control to the prompt — `run_once` always emits a prompt on return | REQ-LI-000 |
| TC-CS-171 | A handler returning `CONSOLE_SERVICE_ERR_VALIDATION` logs at DEBUG via `ILogger` | P8 |
| TC-CS-172 | A handler returning `CONSOLE_SERVICE_ERR_APPLY_FAILED` logs at WARN via `ILogger` | P8 |
| TC-CS-173 | `console_service_init` failure logs at ERROR via `ILogger` | P8 |

---

## 19. Integration test plan

On-target tests (Field Device unless noted). Executed manually with a
serial terminal attached to the debug UART.

| TC | Setup | Pass criterion |
|---|---|---|
| IT-CS-001 | Boot, observe prompt within 2 s | `fd> ` (FD) or `gw> ` (GW) appears |
| IT-CS-002 | Issue `help` | All commands from §8 listed |
| IT-CS-003 | Issue `version` | Version matches `firmware_version.h` value |
| IT-CS-004 | Issue `sensors` | Latest sensor reading shown with valid timestamp |
| IT-CS-005 | Issue `status` | Health snapshot fields all populated |
| IT-CS-006 | Full provisioning round-trip: `prov set` for all Tier 1 params; `prov commit` → `y`; reboot | Params survive across reboot (verified via `prov` read commands or by behavioural test) |
| IT-CS-007 | Validation rejection: `prov set modbus-addr 300` | `[ERR] invalid value`; no config change |
| IT-CS-008 | Confirm abort: `prov commit` then type `n` | After reboot, old config unchanged |
| IT-CS-009 | Confirm timeout: `prov commit` and wait 30+ s without input | `[ERR] timed out`; pending discarded |
| IT-CS-010 | `selftest` with all subsystems healthy | All PASS; overall PASS; `selftest-result` returns same |
| IT-CS-011 | `selftest` with sensor I²C disconnected | Sensor FAIL; overall FAIL; persisted |
| IT-CS-012 | Issue 257-byte line without `\n` | `[ERR] line overflow`; prompt re-emitted |
| IT-CS-013 (GW) | `wifi status` after WiFi connect | Link state and RSSI shown |
| IT-CS-014 (GW) | `mqtt status` after MQTT connect | Connection state and counters shown |
| IT-CS-015 (GW) | `profiles add addr=5 sensors=temp,hum,pres`; `profiles list` | New profile appears in list |

---

## 20. Open items

Earlier IDs (CS-O1..CS-O3) assigned in passes A–G.

| ID | Item | Resolution path | Status |
|---|---|---|---|
| **CS-O4** | GW `components.md` entry omits `IConfigManager` | Follow-up PR to correct `components.md` alongside this companion | Open |
| **CS-O5** | Self-test depth: driver-level loopback and destructive flash verify not implementable without adding driver interfaces to `ConsoleService` | Evaluate at integration whether the current depth is acceptable for REQ-LI-130 | Open |
| **CS-O6** | Certificate provisioning (REQ-LI-060): certificates may be hundreds of bytes; `prov set mqtt-cert` workflow needs a multi-line paste mode | Define `prov cert-begin` / `prov cert-end` UX at implementation time | Open |
| **CS-O7** | Confirm-prompt blocking read timeout (30 s, matching LC-O4) | `IDebugUart` must expose a timed read, or `ConsoleTask` uses a FreeRTOS timer — confirm mechanism at implementation | Open |
| **CS-O8** | GW `profiles add` command input format: key=value pairs vs JSON snippet | Recommendation: key=value for CLI simplicity; confirm before implementing | Open |

---

## 21. References

- `docs/components.md` (FD + GW ConsoleService entries).
- `docs/task-breakdown.md` §4.2 (`ConsoleTask` definition).
- `docs/sequence-diagrams.md` SD-08 (provisioning flow, UC-16).
- `docs/lld/config-service.md` (defines `IConfigProvider` / `IConfigManager`).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot`).
- `docs/architecture-principles.md` P1, P2, P4, P5, P6, P7, P8, P9, P10.

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD Pass H.*
