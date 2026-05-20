# LLD Companion — ConfigService

**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `IConfigManager` *(write side)*, `IConfigProvider` *(read side)*  
**Consumes:** `IConfigStore`, `ILogger`  
**SRS traces:** REQ-DM-000, REQ-DM-001, REQ-DM-002, REQ-DM-090; REQ-SA-000, REQ-SA-010, REQ-SA-020, REQ-SA-050; REQ-LI-030–LI-130; REQ-MB-100  
**HLD ref:** `components.md` §Application — ConfigService; §"Interface Segregation (ISP)"; `hld.md` §5.6; `sequence-diagrams.md` SD-07 (GW remote config), SD-08 (FD CLI config)
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** ConfigService in `components.md` (FD + GW application layer)

---

## 1. Sources

ConfigService maintains the live in-memory configuration for the system
and owns the validate → apply → persist pipeline for every parameter
change, regardless of its source (CLI, Modbus register write, or remote
MQTT command).

The ISP split is architectural: `IConfigManager` (write operations,
consumed by administrative components: ConsoleService, ModbusRegisterMap,
CloudPublisher) and `IConfigProvider` (read-only access, consumed by
functional components: SensorService, AlarmService, ModbusPoller,
LcdUi, ConsoleService).

ConfigService does **not** own the config snapshot/restore lifecycle —
LifecycleController drives that during EditingConfig transitions. ConfigService
provides the mechanism; LifecycleController decides when to invoke it.

---

## 2. Parameter schema

The config is held in a single flat struct. Board-specific fields are
conditionally compiled. Field names define the canonical parameter
identifiers used by CLI and Modbus register map.

```c
/* config_params.h — shared between boards */

#define CONFIG_MAX_NTP_SERVERS    4U
#define CONFIG_MAX_BROKER_LEN   128U
#define CONFIG_MAX_NTP_HOST_LEN  64U

typedef struct {
    /* ── Sensor acquisition (REQ-SA-070, SA-140) ── */
    uint32_t polling_interval_ms;        /* default 1000; range [100, 60000] */
    float    filter_alpha;               /* default 0.1f; range (0.0, 1.0) exclusive */

    /* ── Sensor physical ranges (REQ-SA-020, SA-120) ── */
    float    temp_range_min;             /* °C; default -40.0 */
    float    temp_range_max;             /* °C; default 85.0  */
    float    humidity_range_min;         /* %RH; default 0.0  */
    float    humidity_range_max;         /* %RH; default 100.0 */
    float    pressure_range_min;         /* hPa; default 300.0 */
    float    pressure_range_max;         /* hPa; default 1100.0 */

    /* ── Alarm thresholds (REQ-AM-000, AM-011) ── */
    float    temp_alarm_high;            /* default 40.0 */
    float    temp_alarm_low;             /* default 0.0  */
    float    temp_hysteresis;            /* default 1.0  */
    float    humidity_alarm_high;        /* default 80.0 */
    float    humidity_alarm_low;         /* default 20.0 */
    float    humidity_hysteresis;        /* default 2.0  */
    float    pressure_alarm_high;        /* default 1050.0 */
    float    pressure_alarm_low;         /* default 950.0  */
    float    pressure_hysteresis;        /* default 5.0    */

    /* ── Modbus ── */
    uint8_t  modbus_slave_addr;          /* FD: own address [1..247]; default 1 */
    uint32_t modbus_poll_period_ms;      /* GW: poll period; default 1000 */

#if defined(BOARD_GATEWAY)
    /* ── Cloud connectivity (GW only) ── */
    char     mqtt_broker[CONFIG_MAX_BROKER_LEN];
    uint16_t mqtt_port;                  /* default 8883 */
    uint32_t telemetry_interval_ms;      /* default 60000 */
    uint32_t health_interval_ms;         /* default 600000 */

    /* ── Time sync (GW only) ── */
    char     ntp_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    uint8_t  ntp_server_count;
#endif
} config_params_t;
```

Defaults are applied by `config_service_apply_defaults()` and used
whenever ConfigStore returns no valid data (REQ-SA-010, SA-020, SA-050).

---

## 3. Data types

```c
/* config_service.h */

typedef enum {
    CONFIG_SERVICE_ERR_OK          = 0,
    CONFIG_SERVICE_ERR_NOT_INIT    = 1,
    CONFIG_SERVICE_ERR_NULL_ARG    = 2,
    CONFIG_SERVICE_ERR_INVALID     = 3,   /* validation failed — REQ-DM-001 */
    CONFIG_SERVICE_ERR_PERSIST     = 4,   /* ConfigStore write failed      */
} config_service_err_t;

/* Identifies a single configurable parameter for targeted validation */
typedef enum {
    CONFIG_PARAM_POLL_INTERVAL    = 0,
    CONFIG_PARAM_FILTER_ALPHA     = 1,
    CONFIG_PARAM_TEMP_RANGE_MIN   = 2,
    /* ... one entry per field in config_params_t ... */
    CONFIG_PARAM_COUNT,
} config_param_id_t;
```

---

## 2. Public API

### 4.1 `IConfigProvider` — read side

```c
/**
 * @brief  Get a const pointer to the live config parameters.
 *
 * Returns a pointer to the internal config struct. The pointer is valid
 * for the lifetime of the system; the struct contents may change on a
 * config commit. Callers must NOT cache values across yield points if
 * they depend on the latest committed values.
 *
 * Thread-safe — returned pointer is to a mutex-protected copy or to a
 * struct whose fields are read atomically. See §7 thread safety.
 *
 * This is the primary read interface. Most consumers call this once
 * per acquisition cycle, so the cost of a mutex acquire is negligible.
 */
const config_params_t *config_service_get_params(void);
```

Providing a const pointer rather than individual getters avoids a
getter-per-parameter proliferation (12+ functions for FD, 20+ for GW).
The consumer reads only the fields it needs; the compiler eliminates
unused reads. The interface remains stable even as new parameters are added.

### 4.2 `IConfigManager` — write side

```c
/**
 * @brief  Initialise ConfigService.
 *
 * Applies defaults, then attempts to load from ConfigStore. If
 * ConfigStore returns valid data, overwrites defaults with stored values.
 * Always leaves ConfigService in a fully initialised state regardless of
 * ConfigStore outcome (REQ-SA-010, SA-020, SA-050).
 *
 * @param  store  IConfigStore handle.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
config_service_err_t config_service_init(IConfigStore *store);

/**
 * @brief  Apply stored config blob loaded by LifecycleController.
 *
 * Deserialises the blob from ConfigStore into config_params_t.
 * Validates each field; applies defaults for any field that fails
 * validation (corrupt stored value).
 *
 * @param  blob  Raw bytes from config_store_load().
 * @param  len   Blob byte count.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
config_service_err_t config_service_apply_loaded(const void *blob,
                                                  uint32_t    len);

/**
 * @brief  Validate and apply a single parameter change.
 *
 * Validates the new value against the parameter's rules (§5).
 * On failure: returns CONFIG_SERVICE_ERR_INVALID; config unchanged.
 * On success: applies to in-memory config; persists via ConfigStore
 * (REQ-DM-090). On ConfigStore failure: logs warning; returns
 * CONFIG_SERVICE_ERR_PERSIST; in-memory config already updated.
 *
 * Thread-safe. Acquires mutex for the duration of the operation.
 *
 * Called by: ConsoleService (SD-08), ModbusRegisterMap (FD),
 *             CloudPublisher (GW, SD-07).
 *
 * @param  id     Parameter identifier.
 * @param  value  Pointer to new value (type matches the param's type).
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 */
config_service_err_t config_service_set_param(config_param_id_t id,
                                               const void       *value);

/**
 * @brief  Validate a parameter without applying it.
 *
 * Used by ConsoleService to give immediate feedback before asking the
 * Field Technician to confirm. Does not modify state.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
config_service_err_t config_service_validate_param(config_param_id_t id,
                                                    const void       *value);

/**
 * @brief  Save a snapshot of the current config for rollback.
 *
 * Called by LifecycleController on entering EditingConfig.
 * Stores a copy of config_params_t in s_cs.snapshot.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
config_service_err_t config_service_snapshot(void);

/**
 * @brief  Restore config from the last snapshot.
 *
 * Called by LifecycleController on EditingConfig cancel or timeout.
 * Overwrites in-memory config with snapshot; persists the restored
 * config via ConfigStore.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
config_service_err_t config_service_restore_snapshot(void);

/**
 * @brief  Persist the current in-memory config to ConfigStore.
 *
 * Explicit flush — used by LifecycleController in the Restarting
 * state to ensure config is persisted before a soft reset.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
config_service_err_t config_service_flush(void);
```

---

## 5. Validation rules

Each parameter has a named validation rule applied by
`config_service_validate_param()`. The full table is defined in
`config_validation_rules.c` as a static array indexed by
`config_param_id_t`.

Representative rules:

| Parameter | Type | Valid range / constraint |
|-----------|------|--------------------------|
| `polling_interval_ms` | `uint32_t` | [100, 60 000] |
| `filter_alpha` | `float` | (0.0, 1.0) exclusive |
| `temp_range_min` | `float` | [−55.0, temp_range_max − 1.0] |
| `temp_range_max` | `float` | [temp_range_min + 1.0, 125.0] |
| `temp_alarm_high` | `float` | [temp_range_min, temp_range_max] |
| `temp_alarm_low` | `float` | [temp_range_min, temp_alarm_high − temp_hysteresis] |
| `temp_hysteresis` | `float` | (0.0, (temp_alarm_high − temp_alarm_low) / 2.0) |
| `modbus_slave_addr` | `uint8_t` | [1, 247] |
| `mqtt_port` | `uint16_t` | [1, 65535] |
| `mqtt_broker` | `char[]` | non-empty, ≤ CONFIG_MAX_BROKER_LEN − 1 |
| `ntp_server_count` | `uint8_t` | [1, CONFIG_MAX_NTP_SERVERS] |

**Cross-parameter validation:** alarm thresholds and hysteresis form a
dependency chain (alarm_low < alarm_low + hysteresis < alarm_high −
hysteresis < alarm_high). When any one of these is changed, the others
must remain consistent. Validation checks the proposed change against the
current values of related parameters.

---

## 6. Serialisation

`config_service_flush()` and `config_service_restore_snapshot()` need to
convert between `config_params_t` and the raw byte blob used by ConfigStore.

**Format:** direct `memcpy` of `config_params_t` — no additional
serialisation. The struct is packed (`__attribute__((packed))`) to
eliminate padding differences between compiler versions.

**Version field:** prepend a 4-byte schema version number to the blob:

```c
typedef struct __attribute__((packed)) {
    uint32_t schema_version;   /* incremented when config_params_t changes */
    config_params_t params;
} config_blob_t;
```

On `config_service_apply_loaded()`: if `schema_version` does not match
the compiled-in version, discard the stored blob and apply defaults. This
handles firmware upgrades that add or remove config fields. See CS-O1.

---

## 3. Internal design

```c
/* config_service.c */

typedef struct {
    bool              initialised;
    config_params_t   params;        /* live in-memory config */
    config_params_t   snapshot;      /* rollback copy (set by snapshot()) */
    bool              snapshot_valid;
    IConfigStore     *store;
    SemaphoreHandle_t mutex;         /* priority-inheritance mutex */
} ConfigServiceState;

static ConfigServiceState s_cs;
```

`config_service_get_params()` acquires the mutex, copies `s_cs.params`
into a local struct, releases the mutex, and returns a pointer to the
local copy — except the local copy must outlive the call, so it is placed
in a static per-caller buffer.

**Alternative: single-write/multiple-read model.** Since writes are rare
and readers are frequent, a simpler approach is: writes acquire the mutex
and update the struct; readers take a snapshot copy under the mutex when
they need it. Most readers (SensorService, AlarmService) call
`config_service_get_params()` once per 100 ms cycle — the mutex contention
is negligible at this rate.

**Decision:** return `const config_params_t *` to the live struct, not a
copy. The mutex protects the struct during writes only. Readers access
the struct without the mutex on the assumption that reads of individual
float/uint fields on Cortex-M4 are atomic (natural alignment, 32-bit bus).
This is documented and intentional — not a race condition. See CS-O2.

---

### Principles applied

- **P1 (Strict directional layering).** Depends on IConfigStore (middleware layer) and Logger; no lower-layer protocol-level dependencies.
- **P2 (Dependency Inversion).** Exposes two vtable interfaces (`iconfig_provider_t` and `iconfig_manager_t`); all consumers depend on the interface, not the concrete module.
- **P3 (Interface Segregation).** `IConfigProvider` (read-only) and `IConfigManager` (write) are separate interfaces because distinct consumer sets have non-overlapping access: SensorService / AlarmService read via IConfigProvider; CLI / remote config write via IConfigManager. Split documented in `components.md` ISP section.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Config cache in a static struct; no heap; ConfigStore handles flash serialisation.
- **P6 (Responsibility traces to requirements).** Get / set / commit functions trace to REQ-DM-000-002 / REQ-SA-* / REQ-LI-* configuration requirements.
- **P8 (Total error propagation, no silent failures).** All operations return `config_service_err_t`; ConfigStore errors and validation failures propagated with distinct codes.
- **P9 (BARR-C coding standard).** Config keys as `uint8_t` enums; values as fixed-width integers or bounded strings; no floating-point.
- **P10 (Naming conventions).** Prefix `config_service_`; interfaces `IConfigProvider` -> `iconfig_provider_t`, `IConfigManager` -> `iconfig_manager_t`; errors `CONFIG_SERVICE_ERR_*`.


## 8. Board differences

| Aspect | FD | GW |
|--------|----|----|
| MQTT / NTP params | — | Present in config_params_t |
| `modbus_slave_addr` | Own address | Not used (GW is master) |
| `modbus_poll_period_ms` | Not used | Poll scheduler period |
| Remote config path (SD-07) | — | CloudPublisher → ConfigService |
| Local config path (SD-08) | ConsoleService + LcdUi | ConsoleService |

The board difference is handled by `#if defined(BOARD_GATEWAY)` in
`config_params.h` and the corresponding validation rules. No runtime
branching needed — the board variant is fixed at compile time.

---

## 9. Change notification

ConfigService uses **pull-based** notification (P6: pull-based data access).
Consumers that need to react immediately to a parameter change (not just
on their next cycle) register a notification callback:

```c
typedef void (*config_change_cb_t)(config_param_id_t param_id);

config_service_err_t config_service_register_change_callback(
    config_change_cb_t cb);
```

Registered callbacks are called from within `config_service_set_param()`
after a successful write, while the mutex is **not** held — to avoid
deadlock if the callback calls `config_service_get_params()`.

Consumers that react on their next periodic cycle (SensorService reads
`polling_interval_ms` each cycle) do not need to register — they always
see the latest value via `config_service_get_params()`.

Current registered consumers:
- `ModbusSlave.set_address()` — on `modbus_slave_addr` change (FD)
- `ModbusPoller` — on `modbus_poll_period_ms` change (GW)
- `CloudPublisher` — on `telemetry_interval_ms`, `health_interval_ms` (GW)
- `TimeService` — on `ntp_server_*` change (GW)

---

## 10. Init ordering

```
config_store_init(health)          ← middleware ready
config_service_init(&config_store) ← applies defaults

[LifecycleController.CheckingIntegrity]
  config_store_check_integrity()

[LifecycleController.LoadingConfig]
  config_store_load(&blob, &len, sizeof(blob))
  config_service_apply_loaded(blob, len)
  ← from this point, config_service_get_params() returns valid data
```

All other application components initialise after `LoadingConfig` completes,
so they always see a fully loaded config on their first `get_params()` call.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* Replace ConfigStore with a RAM-backed stub */
static uint8_t s_flash_sim[CONFIG_STORE_MAX_DATA_BYTES];
config_store_err_t stub_config_store_load(void *out, uint32_t *len, uint32_t max);
config_store_err_t stub_config_store_save(const void *data, uint32_t len);
#endif
```

Minimum test cases:
- `config_service_init()` with no stored data → defaults applied; all params valid.
- `config_service_apply_loaded()` with valid blob → params match stored values.
- `config_service_apply_loaded()` with wrong schema_version → defaults applied.
- `config_service_set_param(POLL_INTERVAL, 500)` → params.polling_interval_ms == 500; ConfigStore write called.
- `config_service_set_param(POLL_INTERVAL, 50)` → ERR_INVALID; params unchanged.
- Cross-param: set alarm_high below alarm_low + hysteresis → ERR_INVALID.
- `config_service_snapshot()` + change + `config_service_restore_snapshot()` → original values restored; ConfigStore write called with original values.
- `config_service_validate_param()` does not modify state.
- Change callback fires after successful `set_param()`; does not fire on validation failure.
- Thread safety: concurrent `get_params()` and `set_param()` — no torn reads (verify with stress test on host).

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| CS-O1 | Schema version migration — when `schema_version` mismatch is detected, the current behaviour is "discard and apply defaults." A future requirement may need partial migration (preserve valid fields, default only new ones). Design the migration path when the first firmware upgrade requiring it is planned. | Design migration path when first firmware upgrade requiring it is planned | Open |
| CS-O2 | Mutex-free read safety — the decision to allow mutex-free reads relies on natural-alignment atomic reads on Cortex-M4. Strings (`mqtt_broker`, `ntp_servers`) are not atomically readable. Reads of string fields must acquire the mutex. Document this per-field in the code; add an assertion that string fields are only read under the mutex. | Document mutex requirement per string field in code; add assertion | Open |
| CS-O3 | `config_params_t` size — must be verified to fit within `CONFIG_STORE_MAX_DATA_BYTES` (32 712 bytes). At current estimated size (~500 bytes), this is not a concern. Record the actual struct size in a build-time `static_assert`. | Add static_assert for struct size at implementation; verify against MAX_DATA_BYTES | Open |
