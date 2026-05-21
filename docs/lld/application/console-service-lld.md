# LLD Companion — ConsoleService

**Boards:** Field Device (FD) + Gateway (GW).
**Layer:** Application.

This artefact specifies the software design of the `ConsoleService`
component — command parsing, dispatch, provisioning workflow,
operational configuration workflow, self-test execution, and
board-specific command sets.

**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** ConsoleService in `components.md` (FD + GW application layer)
---

## 1. Sources

| Field | FD | GW |
|---|---|---|
| **Provides** | *(none)* | *(none)* |
| **Uses** | `IDebugUart`, `ISensorService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `ILogger` | `IDebugUart`, `ISensorService`, `IConfigProvider`, `IConfigManager`, `IDeviceProfileManager`, `IHealthSnapshot`, `ILogger` |
| **Hosted in task** | `ConsoleTask` prio 1, 512 words / 2 KB | `ConsoleTask` prio 1, 512 words / 2 KB |
| **Activation** | Event — UART RX byte notify | Event — UART RX byte notify |

**Note — GW IConfigManager gap.** The `components.md` entry for the GW
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

## 3. UART framing and task loop

```
DebugUartDriver ISR:
    place byte in ring_buf (lock-free, power-of-2 size)
    notify ConsoleTask (direct-to-task, bit 0)

ConsoleTask loop:
    wait on notify (no timeout — fully event-driven)
    drain ring_buf into line_buf until '\n'
    if '\n' received:
        parse_and_dispatch(line_buf)
        reset line_buf
    output prompt (fd> / gw>)
```

**Ring buffer** — static, 256 bytes. If overflow: discard oldest byte,
set `ring_overflow_flag`. Next prompt prints `[WARN] RX overflow` and
clears the flag.

**Max line length** — 256 bytes. Lines exceeding this are discarded with
`[ERR] line too long`.

**Serial parameters** — 115200 baud, 8N1, no flow control (REQ-LI-000).
Configured at init; `DebugUartDriver` owns the register-level setup.

---

## 2. Public API — command table

A `static const` array of `cmd_entry_t`. Lookup is a linear scan on the
first token (~20 entries maximum; worst-case scan is negligible at human
typing speeds).

```c
typedef console_err_t (*cmd_handler_t)(console_service_t *self,
                                       int argc,
                                       const char *argv[]);

typedef struct {
    const char    *token;
    uint8_t        min_argc;
    uint8_t        max_argc;
    cmd_handler_t  handler;
    const char    *help;
} cmd_entry_t;
```

**Parse algorithm:**

```
parse_and_dispatch(line):
    tokenise line by whitespace → argv[], argc
    if argc == 0: return (empty line)
    for each entry in cmd_table:
        if strcmp(argv[0], entry.token) == 0:
            if argc-1 < entry.min_argc or argc-1 > entry.max_argc:
                print "[ERR] usage: ..." (entry.help)
                return
            rc = entry.handler(self, argc, argv)
            if rc != CS_OK:
                print "[ERR] " + error_string(rc)
            return
    print "[ERR] unknown command '" argv[0] "'. Type 'help'."
```

---

## 5. Command catalogue

### 5.1 Shared commands (FD + GW)

| Token | Args | Handler | Traces to |
|---|---|---|---|
| `help` | 0 | Print all command tokens and one-line descriptions | — |
| `version` | 0 | Print firmware version string | — |
| `serial` | 0 | Print MCU UID (12 bytes, hex-formatted) | REQ-LI-150 |
| `sensors` | 0 | Print latest sensor reading (value, unit, timestamp, valid) | REQ-LI-010 |
| `status` | 0 | Print health snapshot (all fields from `IHealthSnapshot`) | REQ-LI-010 |
| `alarms` | 0 | Print active alarms list, or "No active alarms" | REQ-LI-010 |
| `selftest` | 0 | Run board self-test; print pass/fail per subsystem; persist result | REQ-LI-130, LI-140, LI-160 |
| `selftest-result` | 0 | Print most recently stored self-test result | REQ-LI-160 |
| `config list` | 0 | Print all current operational parameters | REQ-LI-010 |
| `config set` | 2 | Stage an operational parameter change | REQ-LI-010 |
| `config commit` | 0 | Confirm and apply staged operational changes | REQ-LI-100..120 |
| `config discard` | 0 | Discard staged operational changes | REQ-LI-0E3 |
| `prov set` | 2 | Stage a provisioning parameter | REQ-LI-030..080 |
| `prov commit` | 0 | Confirm and apply staged provisioning changes | REQ-LI-100..120 |
| `prov discard` | 0 | Discard staged provisioning changes | REQ-LI-0E3 |

### 5.2 Board-specific commands

Commands compiled in only for the relevant board using
`#if defined(BOARD_GATEWAY)` / `#if defined(BOARD_FIELD_DEVICE)`.

| Token | Board | Purpose |
|---|---|---|
| `profiles list` | GW | Print all registered device profiles |
| `profiles add` | GW | Add or update a device profile (key=value pairs) |
| `profiles remove` | GW | Remove a device profile by slave address |
| `wifi status` | GW | Print WiFi link state and RSSI |
| `mqtt status` | GW | Print MQTT connection state and counters |
| `modbus status` | FD | Print Modbus slave counters (CRC, timeout, success) |

---

## 6. Provisioning workflow (UC-16, REQ-LI-030..-120)

### 6.1 Pending buffer

`ConsoleService` holds a static `prov_pending_t` struct (Tier 1 params)
and a `cfg_pending_t` struct (Tier 2 operational params). Both are
zero-initialised at boot and reset to "no staged changes" after each
`commit` or `discard`.

```c
typedef struct {
    bool     dirty;                        /* at least one field staged */
    char     wifi_ssid[64];                /* GW only                   */
    char     wifi_pass[64];                /* GW only                   */
    char     mqtt_endpoint[128];           /* GW only                   */
    uint8_t  modbus_slave_addr;
    uint32_t modbus_baud;
    char     modbus_parity;               /* 'N', 'E', 'O'             */
} prov_pending_t;

typedef struct {
    bool    dirty;
    /* Mirrors lcd_ui_editable_params_t — polling rate, alarm thresholds */
    uint32_t polling_interval_ms;
    int16_t  temp_alarm_hi_deci_c;
    /* ... all operational fields */
} cfg_pending_t;
```

### 6.2 `prov set <key> <value>`

```
cmd_prov_set(key, value):
    validate format and range of value (REQ-LI-090)
    if invalid:
        print "[ERR] " + validation_message   (REQ-LI-0E2)
        return CS_ERR_VALIDATION
    update pending[key] = value
    pending.dirty = true
    print "[OK] staged: " key " = " value
```

Validation is per-field:
- `wifi-ssid` — 1..63 printable chars
- `mqtt-endpoint` — valid hostname or IPv4, ≤128 chars
- `modbus-addr` — integer 1..247
- `modbus-baud` — one of {1200, 2400, 4800, 9600, 19200}
- `modbus-parity` — one of {N, E, O}

Unknown keys return `CS_ERR_UNKNOWN_KEY` immediately (REQ-LI-0E2).

### 6.3 `prov commit`

```
cmd_prov_commit():
    if not pending.dirty:
        print "[INFO] Nothing staged."
        return CS_OK
    print staged changes summary
    print "Apply? [y/N]: "
    wait for single char response (blocking read with 30 s timeout)
    if response != 'y' and 'Y':
        discard pending (REQ-LI-0E3)
        print "[INFO] Discarded."
        return CS_OK
    rc = config_manager->apply_prov_block(config_manager, &pending)
    if rc != CFG_OK:
        print "[ERR] Apply failed: " + cfg_error_string(rc)
        /* pending retained until next discard (REQ-LI-120) */
        return CS_ERR_APPLY_FAILED
    pending.dirty = false
    print "[OK] Provisioning applied and persisted."
```

The `config_manager->apply_prov_block()` validates internally and writes
to `ConfigStore`. On success, the change is immediately live (REQ-LI-110,
REQ-DM-090). On failure, `pending` is not cleared — the operator can
correct the staged value and `commit` again (REQ-LI-120).

### 6.4 `prov discard`

Clears `pending`, sets `dirty = false`, prints `[INFO] Discarded.`
(REQ-LI-0E3).

### 6.5 Operational config workflow

`config set / commit / discard` follows the same algorithm as §6.2..-6.4
with `cfg_pending_t` as the staging struct and
`config_manager->apply_cfg_block()` as the apply call. Applies Tier 2
operational parameters (polling interval, alarm thresholds, display
settings).

---

## 7. Self-test (REQ-LI-130..-160)

ConsoleService does not invoke drivers directly. It relies on three
sources for self-test evidence:

| Subsystem | Method | Evidence |
|---|---|---|
| Sensors | `sensor_service->force_read()` | Returns a `sensor_reading_t`; `valid == true` = pass |
| Comms links | `health_snapshot->get()` | `modbus_slave_ok`, `wifi_ok`, `mqtt_ok` flags |
| Flash | `config_manager->round_trip_test()` | Writes a known token, reads it back; match = pass |

```
cmd_selftest():
    result.timestamp = time_now()
    result.sensor_pass  = (sensor_service->force_read().valid == true)
    snap = health_snapshot->get()
    result.comms_pass   = board_comms_ok(snap)   /* FD: modbus_ok; GW: wifi+mqtt+modbus */
    result.flash_pass   = (config_manager->round_trip_test() == CFG_OK)
    result.overall_pass = result.sensor_pass
                       && result.comms_pass
                       && result.flash_pass

    print formatted table (subsystem | PASS/FAIL)
    print "Overall: PASS" or "Overall: FAIL"

    /* Persist (REQ-LI-160) */
    config_manager->store_selftest_result(&result)
```

`board_comms_ok()` is a compile-time–selected inline:
- FD: returns `snap.modbus_slave_ok`
- GW: returns `snap.wifi_ok && snap.mqtt_ok && snap.modbus_master_ok`

**Self-test depth note.** This implementation satisfies REQ-LI-130 at
HLD self-test depth. A more invasive test (e.g., UART loopback, sector
erase/write verify) would require direct driver access not currently in
`ConsoleService`'s interface list. Tracked as **CS-O5** for consideration
in a later engineering pass.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-10 | Field Technician enters provisioning commands via the CLI; `ConsoleService` parses each command and calls `config_service_set()` or `device_profile_registry_add_profile()` | `console_service_run()`, `console_service_process_line()` |

---

## 6. Error and fault behaviour

```c
typedef enum {
    CS_OK = 0,
    CS_ERR_NULL_ARG,
    CS_ERR_NOT_INIT,
    CS_ERR_VALIDATION,       /* input out of range or wrong format */
    CS_ERR_UNKNOWN_KEY,      /* unknown provisioning/config key    */
    CS_ERR_APPLY_FAILED,     /* IConfigManager returned error      */
    CS_ERR_TIMEOUT,          /* confirm prompt timed out           */
    CS_ERR_LINE_OVERFLOW,    /* incoming line > 256 bytes          */
} console_err_t;
```

All handler errors are mapped to a human-readable `[ERR]` line on the
console before returning. Errors do not affect `ConsoleTask` stability —
the prompt is always reprinted after any command outcome.

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    char     line_buf[CONSOLE_LINE_MAX_LEN]; /**< Current input line being assembled. */
    uint16_t line_len;                       /**< Bytes in line_buf so far. */
    bool     prov_pending;                   /**< Provisioning transaction in progress. */
    bool     cfg_pending;                    /**< Config-store update pending flush. */
} console_service_t;

static console_service_t s_console;
```


`ConsoleService` state (`line_buf`, `ring_buf`, `prov_pending`,
`cfg_pending`) is accessed only within `ConsoleTask`. No mutex needed
for internal state.

Provider calls (`ISensorService.force_read()`, `IHealthSnapshot.get()`,
`IConfigManager.apply_*()`) are individually thread-safe inside each
provider. `ConsoleTask` does not hold locks while issuing these calls.

`DebugUartDriver` ISR writes to `ring_buf` (lock-free; assumes single
producer). `ConsoleTask` drains it (single consumer). No additional
synchronisation.

---

### Principles applied

- **P1 (Strict directional layering).** Depends on DebugUartDriver (driver layer) for byte I/O; middleware command targets consumed via injected vtable pointers.
- **P2 (Dependency Inversion).** Exposes `iconsole_service_t` vtable; LifecycleController depends on `IConsoleService`; injected command targets consumed via their respective interfaces.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static input buffer (bounded by max command length); command dispatch table is a `const` array; no heap.
- **P6 (Responsibility traces to requirements).** Every CLI command entry traces to a specific REQ-LI-* or REQ-DM-* provisioning/configuration requirement.
- **P8 (Total error propagation, no silent failures).** `console_service_err_t` on init and dispatch; command execution errors printed to console and returned to caller.
- **P9 (BARR-C coding standard).** Command-code enums `uint8_t`; buffer length `uint16_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `console_service_`; interface `IConsoleService` -> `iconsole_service_t`; errors `CONSOLE_SERVICE_ERR_*`.


## 10. Initialisation

```c
console_err_t
console_service_init(console_service_t       *self,
                     IDebugUart              *uart,
                     const ISensorService    *sensors,
                     const IConfigProvider   *cfg_read,
                     IConfigManager          *cfg_write,
                     const IHealthSnapshot   *health,
                     IDeviceProfileManager   *profiles,  /* GW only, NULL on FD */
                     ILogger                 *log);
```

On return: ring buffer cleared, pending structs zeroed, prompt printed.
`ConsoleTask` is created after init completes and after the start-gate
event group is released by `LifecycleController`.

---

## 11. Memory and sizing

| Item | Size |
|---|---|
| `console_service_t` context | ~80 B |
| `line_buf` (static) | 256 B |
| `ring_buf` (static) | 256 B |
| `prov_pending_t` | ~320 B |
| `cfg_pending_t` | ~48 B |
| Command table (~20 entries × 24 B) | ~480 B `.rodata` |
| **Total RAM** | **~960 B** |

No dynamic allocation post-init (P8).

---

## 7. Unit-test plan

### 12.1 Unit tests — `tests/application/test_console_service.c`

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; ring buffer cleared; prompt emitted |
| Line parser | Empty line (no dispatch); single valid token; unknown token → `[ERR]`; line overflow → discard |
| `help` | All registered tokens appear in output |
| `sensors` | Calls `ISensorService.get_latest()`; formats reading correctly |
| `status` | Calls `IHealthSnapshot.get()`; formats snapshot fields |
| `prov set` — valid | Stages value; `dirty = true`; `[OK]` printed |
| `prov set` — invalid value | `dirty` unchanged; `[ERR]` + validation message printed |
| `prov set` — unknown key | `dirty` unchanged; `[ERR]` printed |
| `prov commit` — confirm | `IConfigManager.apply_prov_block` called; `dirty` cleared; `[OK]` printed |
| `prov commit` — decline | `IConfigManager` not called; `dirty` cleared; `[INFO] Discarded` printed |
| `prov commit` — timeout | Same as decline |
| `prov commit` — apply failure | `IConfigManager` called but returns error; `dirty` NOT cleared; `[ERR]` printed |
| `prov discard` | `dirty` cleared; `[INFO] Discarded` printed |
| `config set / commit / discard` | Mirror of `prov` suite on `cfg_pending_t` |
| `selftest` — all pass | All three probe calls succeed; `[OK]` per subsystem; result persisted |
| `selftest` — sensor fail | Sensor probe returns `valid = false`; `FAIL` printed for sensors; overall `FAIL`; result persisted |
| `serial` | Prints 12-byte UID in hex format |
| `selftest-result` | Returns last stored result from `IConfigManager.get_selftest_result()` |
| Board-specific commands | GW: `profiles list/add/remove` call `IDeviceProfileManager`; FD: not compiled in |

### 12.2 Integration tests — on target

| Test | Setup |
|---|---|
| Full provisioning round-trip | Issue `prov set` commands for all Tier 1 params; `prov commit`; reboot; verify params survive |
| Validation rejection | Issue `prov set modbus-addr 300` → `[ERR]`; verify no config change |
| Confirm abort | `prov commit` then type `n` → verify old config unchanged after reboot |
| Self-test all pass | System in normal operational state; `selftest` → all pass |
| Diagnostic sensors command | Force a sensor error; `sensors` → invalid flag shown |

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| **CS-O4** | GW `components.md` entry omits `IConfigManager` — correct in a follow-up PR alongside this companion. |
| **CS-O5** | Self-test depth: driver-level loopback and destructive flash verify not implementable without adding driver interfaces to `ConsoleService`. Evaluate at integration whether the current depth is acceptable for REQ-LI-130. |
| **CS-O6** | Certificate provisioning (REQ-LI-060): certificates may be hundreds of bytes. The `prov set mqtt-cert` workflow needs a multi-line paste mode (e.g., `prov cert-begin` / `prov cert-end`). Define exact UX at implementation time. |
| **CS-O7** | Confirm prompt blocking read timeout (30 s, matching LC-O4): `IDebugUart` must expose a timed read or `ConsoleTask` must use a FreeRTOS timer. Confirm mechanism at implementation. |
| **CS-O8** | GW `profiles add` command input format: key=value pairs vs JSON snippet. Recommend key=value for CLI simplicity; confirm before implementing. |

---

## 14. References

- `docs/components.md` (FD + GW ConsoleService entries).
- `docs/task-breakdown.md` §4.2 (`ConsoleTask` definition).
- `docs/sequence-diagrams.md` SD-08 (provisioning flow, UC-16).
- `docs/lld/config-service.md` (defines `IConfigProvider` / `IConfigManager`).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot`).
- `docs/architecture-principles.md` P7 (pull-based), P8 (no dynamic alloc).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
