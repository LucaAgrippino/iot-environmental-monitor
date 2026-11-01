# LLD Companion — ConfigService

**Module:** ConfigService  
**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `iconfig_provider_t` *(read side)*, `iconfig_manager_t` *(write side)*  
**Consumes:** ConfigStore (`config_store_load()`, `config_store_save()` — singleton free functions), Logger (`logger_log()` — P4 cross-cutting)  
**SRS traces:** REQ-DM-000, REQ-DM-001, REQ-DM-002, REQ-DM-090; REQ-SA-000, REQ-SA-010, REQ-SA-020, REQ-SA-050; REQ-LI-030–LI-130; REQ-MB-100  
**HLD ref:** `components.md` §Application — ConfigService; §"Interface Segregation (ISP)"; `hld.md` §5.6; `sequence-diagrams.md` SD-07 (GW remote config), SD-08 (FD CLI config), SD-10 (device provisioning)  
**Task-breakdown ref:** ConfigService is **reactive** — no dedicated task; runs in caller's task context (`task-breakdown.md` §5.1)  
**Version:** 1.1  
**Date:** October 2026  
**Status:** Pass H — Implementation ready · Gateway implemented (host-tested, bench-validated 2026-10-24)

---

## §0 Reconciliation with the repository (v1.1)

The Gateway implementation follows the repository, not the v1.0 listings in §1.1, §2, §4, §7,
§9 and §11. Where a v1.0 code block disagrees with this table, this table wins.

| v1.0 said | Repository / implementation | Decision |
|---|---|---|
| Sensor parameters are `float` (alpha `0.1f`, thresholds `40.0f`, ...) | The Field Device `config_params.h` already uses packed fixed-point integers (centi-°C, centi-%RH, deci-hPa, per-mille alpha; no floating point) and SensorService / AlarmService work in the same scales | **CFG-D10:** same field names, widths, order and units as the FD (T `int16` x100, H `uint16` x100, P `uint16` x10, alpha `uint16` x1000). Thresholds need no conversion. The Gateway struct is **not packed** and carries explicit reserved bytes (`reserved_a[3]`, `reserved_b[2]`, `reserved_c[3]`), so the stored blob is 4 + 444 = 448 B and is not byte-compatible with the FD blob (the boards never exchange blobs). All ranges in §9 are in these units. |
| `config_service_init(void)` creates the mutex, applies defaults, then loads from ConfigStore | Gateway ConfigStore is handle-based (`config_store_create` / `config_store_load(self, ...)`); §8 already has LifecycleController do the load | `config_service_init(config_store_handle_t store)`. Init applies the defaults only and never reads flash; the stored blob arrives through `config_service_apply_loaded()`. NULL `store` returns `CONFIG_SERVICE_ERR_NULL_ARG`. A second init is a no-op. |
| `config_store_load(&blob, &len, max)` / `config_store_save(&blob, len)` | `config_store_load(self, out, &len, max)` / `config_store_save(self, data, len)` | Handle passed as the first argument. |
| `#if defined(BOARD_GATEWAY)` around cloud and NTP fields | The Gateway copy lives under `firmware/gateway/`, so the fields are unconditional (CFG-D6: no runtime board branching still holds) | Per-board files, as for the other Gateway modules. |
| Shared sources under `firmware/common/`, rules in `config_validation_rules_{fd,gw}.c` | No `firmware/common` in this repo (SS-O5); one rule set is enough | Files are in `firmware/gateway/application/config_service/`. The validation rules are a table in `config_service.c`: five rules shared by the three sensor channels plus scalar ranges. |
| Rules given only for temperature; humidity and pressure "follow the same pattern" | The pattern needs absolute limits and a minimum range gap per channel | **CFG-D12 (gap fill):** temperature abs [-5500, 12500], gap 100; humidity [0, 10000], gap 100; pressure [2600, 12600] (LPS22HB span), gap 10. |
| Alarm constraint `alarm_low + hysteresis < alarm_high - hysteresis` | Equivalent: `high - low > 2 x hysteresis`; high and low must also lie inside the range | Implemented as written; the same check bounds high, low and hysteresis. |
| `ntp_servers` value type unspecified | `set_param` takes one `const void *` | **CFG-D11:** `CONFIG_PARAM_NTP_SERVERS` takes a `char[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN]` block (every entry NUL-terminated); `CONFIG_PARAM_NTP_SERVER_COUNT` is valid only if `1..4` and the first `count` entries are non-empty. `MQTT_BROKER` takes a NUL-terminated string of 1..127 characters. |
| `apply_loaded` validates each field and defaults any that fail | Validating against the defaults would reject valid pairs (a new low alarm paired with a new high) | **CFG-D13:** every stored field is validated against the stored set itself; failing fields are replaced by their defaults, and if the sensor range / alarm chain is still inconsistent the whole set falls back to the defaults. Cloud and NTP fields are not part of that final check: their defaults (no broker, no servers) are intentionally invalid until provisioned and must not discard the rest of the blob. A wrong length or schema version discards the blob and returns `CONFIG_SERVICE_ERR_INVALID`. |
| Callbacks fire "after a successful write" | `set_param` can change memory and still fail to persist | **CFG-D14:** callbacks fire whenever the in-memory value changed, including when the result is `CONFIG_SERVICE_ERR_PERSIST`. `restore_snapshot` does not fire callbacks (CFG-O6). |
| Callback registration named `config_service_register_change_cb` | Same | Kept (the FD uses `..._change_callback`; the boards differ in this name). |
| Reads of `uint32_t` / `float` fields are atomic (CFG-D3) | A packed layout (as on the FD) puts some wider fields at odd offsets (for example `modbus_poll_period_ms` at 37, `mqtt_port` at 169), where a read can tear against a write | **CFG-D3 made true:** natural alignment with explicit padding, compile-time `_Static_assert`s on the offsets, TC-CFG-049. CFG-O5 closed. |
| One mutex held across `config_store_save()` (§6) | The save measured 762-774 ms on the bench, so every `lock()` / `set_param` waits that long | **CFG-D15:** two mutexes. The data mutex is held for microseconds (copy / write); the persist mutex serialises the flash save. Readers never wait on flash. **CFG-D16:** `set_param_deferred` + `persist_pending` for callers that cannot block. |

Logging uses `LOG_WARN` / `LOG_INFO`; they compile to nothing in host tests, so "logged WARN"
is verified through the returned error, not a log assertion.

---

## §1 Sources

ConfigService maintains the live in-memory configuration for the system
and owns the validate → apply → persist pipeline for every parameter
change, regardless of its source (CLI, Modbus register write, or remote
MQTT command).

The ISP split is architectural (P3): `iconfig_provider_t` (read-only
access, consumed by SensorService, AlarmService, ModbusPoller, LcdUi,
ConsoleService) and `iconfig_manager_t` (write operations, consumed by
ConsoleService, ModbusRegisterMap, CloudPublisher).

ConfigService does **not** own the config snapshot/restore lifecycle —
LifecycleController drives that during EditingConfig transitions.
ConfigService provides the mechanism; LifecycleController decides when
to invoke it (CFG-D9).

### §1.1 Consumed interface signatures

ConfigStore (singleton free functions — middleware):

```c
config_store_err_t config_store_load(void *out, uint32_t *len,
                                     uint32_t max);
config_store_err_t config_store_save(const void *data, uint32_t len);
```

Logger (cross-cutting, P4):

```c
void logger_log(log_level_t level, const char *tag,
                const char *fmt, ...);
```

---

## §2 Parameter schema

The config is held in a single flat struct. Board-specific fields are
conditionally compiled (CFG-D6). Field names define the canonical
parameter identifiers used by CLI and Modbus register map.

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
    float    temp_range_min;             /* °C; default −40.0 */
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

Defaults are applied by `prv_apply_defaults()` and used whenever
ConfigStore returns no valid data (REQ-SA-010, SA-020, SA-050).

---

## §3 Data types

```c
/* config_service.h */

typedef enum {
    CONFIG_SERVICE_OK              = 0,
    CONFIG_SERVICE_ERR_NOT_INIT    = 1,
    CONFIG_SERVICE_ERR_NULL_ARG    = 2,
    CONFIG_SERVICE_ERR_INVALID     = 3,   /* validation failed (REQ-DM-001) */
    CONFIG_SERVICE_ERR_PERSIST     = 4,   /* config_store_save() failed     */
} config_service_err_t;

typedef enum {
    CONFIG_PARAM_POLL_INTERVAL     = 0,
    CONFIG_PARAM_FILTER_ALPHA,
    CONFIG_PARAM_TEMP_RANGE_MIN,
    CONFIG_PARAM_TEMP_RANGE_MAX,
    CONFIG_PARAM_HUMIDITY_RANGE_MIN,
    CONFIG_PARAM_HUMIDITY_RANGE_MAX,
    CONFIG_PARAM_PRESSURE_RANGE_MIN,
    CONFIG_PARAM_PRESSURE_RANGE_MAX,
    CONFIG_PARAM_TEMP_ALARM_HIGH,
    CONFIG_PARAM_TEMP_ALARM_LOW,
    CONFIG_PARAM_TEMP_HYSTERESIS,
    CONFIG_PARAM_HUMIDITY_ALARM_HIGH,
    CONFIG_PARAM_HUMIDITY_ALARM_LOW,
    CONFIG_PARAM_HUMIDITY_HYSTERESIS,
    CONFIG_PARAM_PRESSURE_ALARM_HIGH,
    CONFIG_PARAM_PRESSURE_ALARM_LOW,
    CONFIG_PARAM_PRESSURE_HYSTERESIS,
    CONFIG_PARAM_MODBUS_SLAVE_ADDR,
    CONFIG_PARAM_MODBUS_POLL_PERIOD,
#if defined(BOARD_GATEWAY)
    CONFIG_PARAM_MQTT_BROKER,
    CONFIG_PARAM_MQTT_PORT,
    CONFIG_PARAM_TELEMETRY_INTERVAL,
    CONFIG_PARAM_HEALTH_INTERVAL,
    CONFIG_PARAM_NTP_SERVERS,
    CONFIG_PARAM_NTP_SERVER_COUNT,
#endif
    CONFIG_PARAM_COUNT,
} config_param_id_t;

typedef void (*config_change_cb_t)(config_param_id_t param_id);

#define CONFIG_MAX_CHANGE_CALLBACKS  4U
```

---

## §4 Public API

### §4.1 Provided interfaces — vtable definitions

```c
/* iconfig_provider.h */

/**
 * @brief  Read-side interface (P3 ISP split).
 *
 * Consumed by SensorService, AlarmService, ModbusPoller, LcdUi,
 * ConsoleService. Provides read-only access to live config.
 */
typedef struct {
    void                    *ctx;
    const config_params_t *(*get_params)(void *ctx);
} iconfig_provider_t;
```

```c
/* iconfig_manager.h */

/**
 * @brief  Write-side interface (P3 ISP split).
 *
 * Consumed by ConsoleService, ModbusRegisterMap, CloudPublisher.
 * Provides parameter write and validation.
 */
typedef struct {
    void                  *ctx;
    config_service_err_t (*set_param)(void *ctx,
                                      config_param_id_t id,
                                      const void       *value);
    config_service_err_t (*validate_param)(void *ctx,
                                           config_param_id_t id,
                                           const void       *value);
} iconfig_manager_t;
```

### §4.2 Free functions

```c
/**
 * @brief  Initialise ConfigService.
 *
 * Creates mutex, applies defaults, then attempts to load from
 * ConfigStore. If ConfigStore returns valid data with a matching
 * schema version, overwrites defaults with stored values. Always
 * leaves ConfigService in a fully initialised state regardless of
 * ConfigStore outcome (REQ-SA-010, SA-020, SA-050).
 *
 * @return CONFIG_SERVICE_OK on success.
 * @note   Threading: call once before the scheduler starts.
 */
config_service_err_t config_service_init(void);

/**
 * @brief  Get the read-side interface vtable.
 * @return Pointer to the static iconfig_provider_t singleton.
 * @note   Threading: safe after init.
 */
const iconfig_provider_t *config_service_get_provider(void);

/**
 * @brief  Get the write-side interface vtable.
 * @return Pointer to the static iconfig_manager_t singleton.
 * @note   Threading: safe after init.
 */
const iconfig_manager_t *config_service_get_manager(void);

/**
 * @brief  Apply stored config blob loaded by LifecycleController.
 *
 * Deserialises the blob from ConfigStore into config_params_t.
 * Validates each field; applies defaults for any field that fails
 * validation (corrupt stored value). If schema_version mismatches
 * the compiled-in version, discards blob and applies defaults.
 *
 * @param  blob  Raw bytes from config_store_load().
 * @param  len   Blob byte count.
 * @return CONFIG_SERVICE_OK on success; CONFIG_SERVICE_ERR_INVALID
 *         if schema_version mismatch (defaults applied).
 * @note   Threading: task-context only, non-blocking.
 */
config_service_err_t config_service_apply_loaded(const void *blob,
                                                  uint32_t    len);

/**
 * @brief  Get a const pointer to the live config parameters.
 *
 * Returns a pointer to the internal config_params_t. The pointer
 * remains valid for the system lifetime. Individual 32-bit-aligned
 * fields (uint32_t, float) are read atomically on Cortex-M4
 * without a mutex. String fields (mqtt_broker, ntp_servers) are
 * NOT atomic — callers that read strings must bracket the read
 * with config_service_lock() / config_service_unlock() (CFG-D3).
 *
 * @return Const pointer to the live config, or NULL if not init'd.
 * @note   Threading: task-safe and ISR-safe for 32-bit fields.
 */
const config_params_t *config_service_get_params(void);

/**
 * @brief  Validate and apply a single parameter change.
 *
 * Validates the new value against the parameter's rules (§9).
 * On failure: returns CONFIG_SERVICE_ERR_INVALID; config unchanged.
 * On success: applies to in-memory config; persists via ConfigStore
 * (REQ-DM-090). On ConfigStore failure: logs warning; returns
 * CONFIG_SERVICE_ERR_PERSIST; in-memory config already updated.
 * Fires registered change callbacks after a successful write.
 *
 * @param  id     Parameter identifier.
 * @param  value  Pointer to new value (type matches the field).
 * @return CONFIG_SERVICE_OK on success.
 * @note   Threading: task-safe. Acquires mutex for write. Not
 *         ISR-safe.
 */
config_service_err_t config_service_set_param(config_param_id_t id,
                                               const void       *value);

/**
 * @brief  Validate a parameter without applying it.
 *
 * Used by ConsoleService to give immediate feedback before asking
 * the Field Technician to confirm. Does not modify state.
 *
 * @param  id     Parameter identifier.
 * @param  value  Pointer to proposed value.
 * @return CONFIG_SERVICE_OK if valid; CONFIG_SERVICE_ERR_INVALID
 *         otherwise.
 * @note   Threading: task-safe, read-only.
 */
config_service_err_t config_service_validate_param(config_param_id_t id,
                                                    const void       *value);

/**
 * @brief  Save a snapshot of the current config for rollback.
 *
 * Called by LifecycleController on entering EditingConfig. Stores a
 * copy of config_params_t in s_cs.snapshot.
 *
 * @return CONFIG_SERVICE_OK on success.
 * @note   Threading: task-context only. Acquires mutex.
 */
config_service_err_t config_service_snapshot(void);

/**
 * @brief  Restore config from the last snapshot.
 *
 * Called by LifecycleController on EditingConfig cancel or timeout.
 * Overwrites in-memory config with snapshot; persists the restored
 * config via ConfigStore.
 *
 * @return CONFIG_SERVICE_OK on success; CONFIG_SERVICE_ERR_INVALID
 *         if no snapshot exists.
 * @note   Threading: task-context only. Acquires mutex.
 */
config_service_err_t config_service_restore_snapshot(void);

/**
 * @brief  Persist the current in-memory config to ConfigStore.
 *
 * Explicit flush — used by LifecycleController in the Restarting
 * state to ensure config is persisted before a soft reset.
 *
 * @return CONFIG_SERVICE_OK on success; CONFIG_SERVICE_ERR_PERSIST
 *         on ConfigStore failure.
 * @note   Threading: task-context only. May block on flash write.
 */
config_service_err_t config_service_flush(void);

/**
 * @brief  Register a callback for parameter change notification.
 *
 * Callbacks fire from within config_service_set_param() after a
 * successful write, with the mutex released (no deadlock if the
 * callback calls config_service_get_params()).
 *
 * @param  cb  Function pointer; must remain valid for system lifetime.
 * @return CONFIG_SERVICE_OK, or CONFIG_SERVICE_ERR_INVALID if the
 *         callback table is full (CONFIG_MAX_CHANGE_CALLBACKS).
 * @note   Threading: call during init, before the scheduler starts.
 */
config_service_err_t config_service_register_change_cb(
    config_change_cb_t cb);

/**
 * @brief  Acquire the config mutex for string-field reads.
 *
 * Callers that read string fields (mqtt_broker, ntp_servers) must
 * bracket the read with lock/unlock. 32-bit-aligned fields do not
 * need this. See CFG-D3, CFG-O2.
 */
void config_service_lock(void);

/**
 * @brief  Release the config mutex after a string-field read.
 */
void config_service_unlock(void);

#ifdef TEST
/**
 * @brief  Reset internal state for unit testing.
 * @note   NOT production code — guarded by #ifdef TEST.
 */
void config_service_reset_for_test(void);
#endif
```

---

## §5 Change notification

ConfigService uses **pull-based** access as the primary pattern (P7).
Consumers that read config on their periodic cycle (SensorService reads
`polling_interval_ms` each 100 ms cycle) always see the latest value
via `config_service_get_params()` without registering.

For consumers that must react immediately to a parameter change,
ConfigService provides a callback registration:

```c
typedef void (*config_change_cb_t)(config_param_id_t param_id);

config_service_err_t config_service_register_change_cb(
    config_change_cb_t cb);
```

Registered callbacks fire from within `config_service_set_param()`
after a successful write, with the mutex **released** — avoiding
deadlock if the callback calls `config_service_get_params()`.

Registered consumers:

| Board | Consumer | Parameter(s) |
|-------|----------|-------------|
| FD | ModbusSlave | `modbus_slave_addr` |
| GW | ModbusPoller | `modbus_poll_period_ms` |
| GW | CloudPublisher | `telemetry_interval_ms`, `health_interval_ms` |
| GW | TimeService | `ntp_server_count`, `ntp_servers` |

Static array of `CONFIG_MAX_CHANGE_CALLBACKS` (4) slots. Callbacks
registered at init before the scheduler starts — no runtime
add/remove needed (P5).

---

## §6 Synchronisation

ConfigService is **reactive** — it has no dedicated FreeRTOS task
(`task-breakdown.md` §5.1). All functions execute in the calling
task's context. Internal state is protected by a priority-inheritance
mutex (`StaticSemaphore_t`, created during `config_service_init()`).

| Function | Mutex | Notes |
|----------|-------|-------|
| `config_service_init()` | No | Called once before scheduler |
| `config_service_get_params()` | **No** | Returns pointer to live struct; 32-bit reads are atomic on Cortex-M4 (CFG-D3) |
| `config_service_set_param()` | **Yes** | Acquire → validate → write field → persist → release → fire callbacks |
| `config_service_validate_param()` | **No** | Read-only; validates proposed value against current values |
| `config_service_apply_loaded()` | **Yes** | Overwrites entire param struct from blob |
| `config_service_snapshot()` | **Yes** | Copies live params to snapshot buffer |
| `config_service_restore_snapshot()` | **Yes** | Copies snapshot back; persists |
| `config_service_flush()` | **Yes** | Serialises and calls config_store_save() |
| `config_service_lock()` | **Yes** | For string-field reads |
| `config_service_unlock()` | Release | Paired with lock() |

**Update (v1.1, CFG-D15):** the table above is superseded for the flash writers. There are two mutexes: the data mutex guards `params`, `snapshot` and the callback table and is held only to copy or write them; the persist mutex serialises `config_store_save()` and is held for the whole save. `set_param`, `restore_snapshot`, `flush` and `persist_pending` take the data mutex briefly to copy the parameters into a local blob, release it, and only then save under the persist mutex. `lock()` / `unlock()` use the data mutex, so a string reader waits microseconds, not a flash write.

**Design note (CFG-D3):** `config_service_get_params()` returns a
`const config_params_t *` to the live struct without acquiring the
mutex. Individual `float` and `uint32_t` fields are naturally aligned
on Cortex-M4 and read atomically via a single LDR instruction. String
fields (`mqtt_broker`, `ntp_servers`) are **not** atomic — callers
must use `config_service_lock()` / `config_service_unlock()` when
reading strings. This is documented per-field in the code (CFG-O2).

---

## §7 Internal design

### §7.1 Singleton state

```c
/* config_service.c */

static struct {
    bool                    initialised;
    config_params_t         params;          /* live in-memory config       */
    config_params_t         snapshot;        /* rollback copy               */
    bool                    snapshot_valid;
    SemaphoreHandle_t       mutex;
    StaticSemaphore_t       mutex_buf;       /* static allocation (P5)      */
    config_change_cb_t      change_cbs[CONFIG_MAX_CHANGE_CALLBACKS];
    uint8_t                 change_cb_count;
} s_cs;
```

### §7.2 Defaults

```c
static void prv_apply_defaults(void)
{
    config_params_t *p = &s_cs.params;
    p->polling_interval_ms = 1000U;
    p->filter_alpha        = 0.1f;
    p->temp_range_min      = -40.0f;
    p->temp_range_max      = 85.0f;
    p->humidity_range_min   = 0.0f;
    p->humidity_range_max   = 100.0f;
    p->pressure_range_min   = 300.0f;
    p->pressure_range_max   = 1100.0f;
    p->temp_alarm_high     = 40.0f;
    p->temp_alarm_low      = 0.0f;
    p->temp_hysteresis     = 1.0f;
    p->humidity_alarm_high = 80.0f;
    p->humidity_alarm_low  = 20.0f;
    p->humidity_hysteresis = 2.0f;
    p->pressure_alarm_high = 1050.0f;
    p->pressure_alarm_low  = 950.0f;
    p->pressure_hysteresis = 5.0f;
    p->modbus_slave_addr   = 1U;
    p->modbus_poll_period_ms = 1000U;
#if defined(BOARD_GATEWAY)
    (void)memset(p->mqtt_broker, 0, sizeof(p->mqtt_broker));
    p->mqtt_port             = 8883U;
    p->telemetry_interval_ms = 60000U;
    p->health_interval_ms    = 600000U;
    p->ntp_server_count      = 0U;
#endif
}
```

### §7.3 Serialisation blob

**Schema 2 (CFG-D17, WP8).** The blob also carries the DeviceProfileRegistry's profiles, so they persist
through the one ConfigStore region ConfigService already owns (device-profile-registry-lld.md D-2).

```c
#define CONFIG_SCHEMA_VERSION     2U
#define CONFIG_SCHEMA_VERSION_V1  1U

typedef struct {
    uint8_t          count;                   /* 0..DEVICE_PROFILE_MAX */
    uint8_t          reserved[3];
    device_profile_t entries[DEVICE_PROFILE_MAX];
} config_profiles_t;                          /* 308 bytes */

typedef struct {
    uint32_t          schema_version;
    config_params_t   params;
    config_profiles_t profiles;
} config_blob_t;

typedef struct {                              /* schema 1, read only to migrate */
    uint32_t        schema_version;
    config_params_t params;
} config_blob_v1_t;

_Static_assert(sizeof(config_blob_t) <= CONFIG_STORE_MAX_DATA_BYTES,
               "config blob exceeds ConfigStore capacity");
```

On `config_service_apply_loaded()`: a blob of exactly `sizeof(config_blob_t)` with `schema_version` 2 is
applied. A blob of exactly `sizeof(config_blob_v1_t)` with `schema_version` 1 is **migrated**: the
parameters are applied and the profile list is empty, so the registry seeds its default (CFG-D17). Any
other length or version discards the blob and applies defaults (CFG-D5, CFG-O1), including the profiles.
A stored profile count above `DEVICE_PROFILE_MAX` drops the profiles and keeps the parameters.

**Profile accessors** (they are the registry's storage callbacks):

| Function | Behaviour |
|---|---|
| `config_service_get_profiles(out, max, &count)` | Copies up to `max` stored profiles under the data mutex; false if none |
| `config_service_set_profiles(in, count)` | Replaces the stored profiles and persists the whole blob (about 0.8 s, CFG-O7); on a failed save the previous profiles are restored and false is returned, so memory and flash agree. Profile content is validated by the registry, not here |

Every persist (a parameter change or a profile change) writes the complete blob, so neither can lose
the other.

**Stack discipline (CFG-D18, found on the board 2026-10-31).** The first version of schema 2 built the blob on
the caller's stack and `set_profiles` kept a second copy of the profile list; called from the console task
(3 KB stack) through the registry's own 300-byte candidate table, `profiles add` overflowed the stack and
silenced the board (FreeRTOS stack-overflow hook). Fixed by measurement (`-fstack-usage`):

| Function | Before | After |
|---|---|---|
| `prv_persist` | 896 bytes (blob on the stack) | 152 bytes: the blob is the static `s_persist_blob`, guarded by `persist_mutex` |
| `config_service_apply_loaded` | 1800 bytes (whole-blob copy + v1 copy) | 1048 bytes (main had 1040): parameters and profiles are copied piecewise from the caller's buffer |
| `set_profiles` | a second 308-byte copy for rollback | none: the new list is written from the caller's array and replaces the live one only after a good save |

`persist_mutex` is now taken **before** the data mutex copy (it was after), so a save that starts later always
holds the newest values; the old order let an older copy overwrite a newer one when two persists raced.
The static blob costs about 520 bytes, saved on every caller's stack (the console, CloudPublisher and the
lifecycle task). It sits in **SRAM2** (`.bss.sram2`, like the console's buffers): in the 96 KB main bank it made the
production image fail to link by 504 bytes (found by CI on the merged tree, 2026-10-31). The registry's table
is in SRAM2 for the same reason. Slack of the `Release` image after this: 748 bytes in the main bank, 596 bytes in SRAM2.

### §7.4 Vtable singletons

```c
static const config_params_t *prv_get_params(void *ctx)
{
    (void)ctx;
    return config_service_get_params();
}

static config_service_err_t prv_set_param(void *ctx,
                                           config_param_id_t id,
                                           const void *value)
{
    (void)ctx;
    return config_service_set_param(id, value);
}

static config_service_err_t prv_validate_param(void *ctx,
                                                config_param_id_t id,
                                                const void *value)
{
    (void)ctx;
    return config_service_validate_param(id, value);
}

static const iconfig_provider_t s_provider = {
    .ctx        = NULL,
    .get_params = prv_get_params,
};

static const iconfig_manager_t s_manager = {
    .ctx            = NULL,
    .set_param      = prv_set_param,
    .validate_param = prv_validate_param,
};
```

### §7.5 Test reset

```c
#ifdef TEST
void config_service_reset_for_test(void)
{
    (void)memset(&s_cs, 0, sizeof(s_cs));
}
#endif
```

CMock link-time replacement provides stubs for `config_store_load()`,
`config_store_save()`, and `logger_log()`.

---

## §8 Init ordering

```
config_store_init()                    ← middleware ready
config_service_init()                  ← creates mutex, applies defaults

[LifecycleController.CheckingIntegrity]
  config_store_check_integrity()

[LifecycleController.LoadingConfig]
  config_store_load(&blob, &len, sizeof(blob))
  config_service_apply_loaded(blob, len)
  ← from this point, config_service_get_params() returns loaded data

[All other application modules init after LoadingConfig]
```

Pre-conditions for `config_service_init()`:

- ConfigStore already initialised.
- FreeRTOS kernel initialised (mutex creation needs it) but scheduler NOT started.

---

## §9 Validation rules

Each parameter has a named validation rule applied by
`config_service_validate_param()`. Rules are defined in a
board-specific `config_validation_rules_{fd,gw}.c` as a static
array indexed by `config_param_id_t`.

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
| `modbus_poll_period_ms` | `uint32_t` | [100, 60 000] |
| `mqtt_port` | `uint16_t` | [1, 65 535] |
| `mqtt_broker` | `char[]` | non-empty, ≤ CONFIG_MAX_BROKER_LEN − 1 |
| `ntp_server_count` | `uint8_t` | [1, CONFIG_MAX_NTP_SERVERS] |
| `telemetry_interval_ms` | `uint32_t` | [1 000, 3 600 000] |
| `health_interval_ms` | `uint32_t` | [10 000, 86 400 000] |

**Cross-parameter validation (CFG-D7):** alarm thresholds and
hysteresis form a dependency chain. When any one is changed, the
proposed value is checked against the *current* values of related
parameters. The ordering constraint is:
`alarm_low + hysteresis < alarm_high − hysteresis` for each sensor
channel. Humidity and pressure channels follow the same pattern as
temperature.

---

## §10 Board differences

| Aspect | FD | GW |
|--------|----|----|
| MQTT / NTP params | Absent | Present via `#if defined(BOARD_GATEWAY)` |
| `modbus_slave_addr` | Own address | Not used (GW is master) |
| `modbus_poll_period_ms` | Not used | ModbusPoller poll period |
| Remote config path (SD-07) | — | CloudPublisher → `config_service_set_param()` |
| Local config path (SD-08/SD-10) | ConsoleService + ModbusRegisterMap | ConsoleService |
| Change callbacks | ModbusSlave only | ModbusPoller, CloudPublisher, TimeService |

Handled entirely by conditional compilation (CFG-D6) — no runtime
board branching.

---

## §11 File layout

```
firmware/
├── common/
│   ├── inc/
│   │   ├── config_params.h            /* config_params_t, config_param_id_t */
│   │   ├── config_service.h           /* public API                         */
│   │   ├── iconfig_provider.h         /* iconfig_provider_t vtable          */
│   │   └── iconfig_manager.h          /* iconfig_manager_t vtable           */
│   └── src/
│       └── config_service.c           /* singleton state, core logic        */
├── field-device/
│   └── app/
│       └── config_validation_rules_fd.c
└── gateway/
    └── app/
        └── config_validation_rules_gw.c
tests/
└── test_config_service.c
```

`config_params.h` and the interface headers are shared between boards.
`config_service.c` is shared (conditional compilation for board-specific
fields). Validation rule tables are board-specific because the GW has
additional parameters.

---

## §12 Unit-test plan

| ID | Scenario | Stimulus | Expected outcome |
|----|----------|----------|-----------------|
| TC-CFG-001 | Init with no stored data | `config_store_load()` stub returns error | Defaults applied; all params at default values |
| TC-CFG-002 | Init with valid blob | Stub returns valid blob, correct schema_version | Params match stored values |
| TC-CFG-003 | Init with wrong schema version | Stub returns blob with mismatched schema_version | Defaults applied; `apply_loaded()` returns ERR_INVALID |
| TC-CFG-004 | Set valid poll interval | `set_param(POLL_INTERVAL, &500)` | `params.polling_interval_ms == 500`; `config_store_save()` called |
| TC-CFG-005 | Set invalid poll interval (below min) | `set_param(POLL_INTERVAL, &50)` | ERR_INVALID; params unchanged |
| TC-CFG-006 | Set poll interval at min boundary | `set_param(POLL_INTERVAL, &100)` | OK; value accepted |
| TC-CFG-007 | Set poll interval at max boundary | `set_param(POLL_INTERVAL, &60000)` | OK; value accepted |
| TC-CFG-008 | Cross-param: alarm_high below constraint | Set `temp_alarm_high` ≤ `temp_alarm_low + temp_hysteresis` | ERR_INVALID; params unchanged |
| TC-CFG-009 | Cross-param: hysteresis too large | Set `temp_hysteresis` ≥ `(alarm_high − alarm_low) / 2` | ERR_INVALID; params unchanged |
| TC-CFG-010 | Snapshot and restore | `snapshot()` → change param → `restore_snapshot()` | Original values restored; `config_store_save()` called |
| TC-CFG-011 | Restore without snapshot | `restore_snapshot()` before any `snapshot()` | ERR_INVALID; params unchanged |
| TC-CFG-012 | Validate does not modify state | `validate_param(POLL_INTERVAL, &500)` | OK returned; `polling_interval_ms` unchanged |
| TC-CFG-013 | Change callback fires on success | Register cb; `set_param(POLL_INTERVAL, &500)` | Callback invoked with `CONFIG_PARAM_POLL_INTERVAL` |
| TC-CFG-014 | Callback does not fire on failure | Register cb; `set_param(POLL_INTERVAL, &50)` | Callback NOT invoked |
| TC-CFG-015 | Callback table full | Register `CONFIG_MAX_CHANGE_CALLBACKS + 1` | Fifth registration returns ERR_INVALID |
| TC-CFG-016 | Flush calls save | `flush()` | `config_store_save()` called with serialised blob |
| TC-CFG-017 | Flush handles store failure | Stub `config_store_save()` returns error | ERR_PERSIST returned |
| TC-CFG-018 | Set param with store failure | Stub save returns error | ERR_PERSIST; in-memory value IS updated |
| TC-CFG-019 | Not-init guard | Call `set_param()` before `init()` | ERR_NOT_INIT |
| TC-CFG-020 | Null arg guard | `set_param(POLL_INTERVAL, NULL)` | ERR_NULL_ARG |
| TC-CFG-021 | Provider vtable get_params | `get_provider()->get_params(ctx)` | Same result as `config_service_get_params()` |
| TC-CFG-022 | Manager vtable set_param | `get_manager()->set_param(ctx, id, &val)` | Same result as free function |
| TC-CFG-023 | String field set — mqtt_broker | `set_param(MQTT_BROKER, "test.broker.io")` | Stored correctly; callback fires |
| TC-CFG-024 | String field boundary — broker too long | `set_param(MQTT_BROKER, <128-char string>)` | ERR_INVALID |
| TC-CFG-060 | Profiles empty after init | Fresh init | `get_profiles` false, count 0 |
| TC-CFG-061 | `set_profiles` stores into the blob and reads back | Two profiles | One save of `sizeof(config_blob_t)`, schema 2, profiles present |
| TC-CFG-062 | Failed profile save keeps the previous profiles | Save returns an error | `set_profiles` false; the earlier list is still read |
| TC-CFG-063 | Profile arguments are checked | NULL, count above max, before init | false, no save |
| TC-CFG-064 | Zero profiles persists an empty list | `set_profiles(NULL, 0)` | Saved count 0 |
| TC-CFG-065 | A parameter change keeps the stored profiles | `set_param` after `set_profiles` | Blob still holds the profiles |
| TC-CFG-066 | `apply_loaded` restores a schema 2 blob's profiles | Blob with one profile | Read back |
| TC-CFG-067 | `apply_loaded` migrates a schema 1 blob | v1-length blob | Parameters applied, no profiles |
| TC-CFG-068 | A v1-length blob with a wrong version is discarded | Version 9 | `ERR_INVALID` |
| TC-CFG-069 | Out-of-range profile count drops the profiles only | Count 9 | Parameters kept, no profiles |
| TC-CFG-070 | `get_profiles` copies at most `max` | 3 stored, max 2 | 2 copied |

Additions in v1.1 (all PASS): TC-CFG-041 flash save holds only the persist mutex · 042 deferred set applies and notifies without flash · 043 persist_pending writes once · 044 several deferred changes share one write · 045 failed write stays pending · 046 deferred set rejects what set_param rejects · 047 durable set flushes earlier deferred changes · 048 deferred guards and vtable · 049 natural alignment · TC-CFG-025 NTP servers and count · 026 a corrupt stored field is repaired, the rest kept · 027 an unprovisioned blob is kept · 028 an inconsistent alarm band is repaired · 030 NULL store / idempotent init · 031 wrong blob length · 032 humidity and pressure band rule · 033 range limits and gap · 034 scalar rules · 035 unterminated NTP entry · 036 one mutex take/give per `set_param` · 037 restore with a store failure · 038 unknown parameter id · 039 lock / unlock · 040 blob size. Host result: 47 tests, 47 pass, 0 ignored (`tests/gateway/application/config_service/test_config_service.c`, `tests/project_gateway.yml`).

Hardware (bench harness `integration-tests/config_service`, 2026-10-24, real QspiFlash + ConfigStore): **TC-HW-CFG-001..011 PASS**, including persistence across a software reset (Phase A writes a sentinel configuration and resets; Phase B loads it through ConfigStore and `apply_loaded`). `set_param` including the flash write took 762-774 ms; `set_param_deferred` took 20 us and `persist_pending` 762 ms (TC-HW-CFG-011, CFG-O7 / CFG-O9).

### Integration tests

| ID | Scenario | Boards |
|----|----------|--------|
| TC-CFG-I01 | Boot with empty ConfigStore → defaults active | FD, GW |
| TC-CFG-I02 | Set param via CLI → persist → reboot → param retained | FD |
| TC-CFG-I03 | Snapshot → modify → restore → original values visible | FD, GW |
| TC-CFG-I04 | Set param via MQTT remote config (SD-07) | GW |
| TC-CFG-I05 | Change callback → ModbusPoller updates poll period | GW |

---

## §13 Error and fault behaviour

| Error value | Cause | Local behaviour | Caller-visible | Retry | Observability |
|-------------|-------|-----------------|----------------|-------|---------------|
| `CONFIG_SERVICE_OK` | Success | — | OK return | — | — |
| `CONFIG_SERVICE_ERR_NOT_INIT` | API called before `config_service_init()` | Return error; no state change | Non-OK | No — programming error | Caller logs ERROR |
| `CONFIG_SERVICE_ERR_NULL_ARG` | Null pointer argument | Return error; no state change | Non-OK | No — programming error | Caller logs ERROR |
| `CONFIG_SERVICE_ERR_INVALID` | Validation failure (REQ-DM-001) | Return error; params unchanged | Non-OK | No — caller must correct value | Caller logs WARN; ConsoleService prints `[ERR]` |
| `CONFIG_SERVICE_ERR_PERSIST` | `config_store_save()` failed | In-memory value already applied | Non-OK | Caller may retry; next flush/set_param attempts save again | Logged WARN via Logger |

**Note (CFG-D8):** ConfigService does NOT push to IHealthReport — not
in its USES per `components.md`. ConfigStore handles its own
flash-failure health reporting. ConfigService logs validation and
persistence errors via Logger.

---

## §14 SD trace

| SD | Component role | Key function |
|----|----------------|--------------|
| SD-00a/b | LifecycleController loads blob from ConfigStore, calls ConfigService to apply it at boot | `config_service_apply_loaded()` |
| SD-07 | CloudPublisher routes remote-config command to ConfigService | `config_service_set_param()`, `config_service_validate_param()` |
| SD-08 | ConsoleService calls ConfigService for each CLI config command (FD) | `config_service_set_param()` |
| SD-10 | ConsoleService calls ConfigService for provisioning parameters entered by Field Technician | `config_service_set_param()`, `config_service_validate_param()` |

---

## §15 Sizing estimates

| Item | Size | Notes |
|------|------|-------|
| `s_cs.params` (GW) | ~560 B | Struct with GW-conditional fields |
| `s_cs.params` (FD) | ~120 B | Without GW fields |
| `s_cs.snapshot` | Same as params | Rollback copy |
| `s_cs` total (GW) | ~1 200 B | params + snapshot + mutex_buf + callbacks |
| `config_blob_t` (stack-local) | ~564 B | Largest stack frame during load/save |
| Validation rule table | ~200 B | Static const array (GW) |
| **Total static RAM (GW)** | **~1 400 B** | |
| **Total static RAM (FD)** | **~300 B** | |
| **Total ROM** | **~2.5 KB** | Validation logic, defaults, serialisation |

No task stack — ConfigService is reactive (runs in caller's context).

---

## §16 Principles applied

- **P1 (Strict directional layering).** Depends on ConfigStore
  (middleware) and Logger. No upward concrete dependencies.
- **P2 (Dependency Inversion).** Exposes two vtable interfaces:
  `iconfig_provider_t` (read, consumed by SensorService, AlarmService)
  and `iconfig_manager_t` (write, consumed by ConsoleService,
  CloudPublisher, ModbusRegisterMap). Consumers depend on the
  abstraction, not the concrete singleton.
- **P3 (Interface Segregation).** `iconfig_provider_t` and
  `iconfig_manager_t` are separate interfaces. SensorService /
  AlarmService only see `get_params`; ConsoleService sees both;
  ModbusRegisterMap sees both. Non-overlapping access patterns
  justify the split.
- **P4 (Cross-cutting concern exception).** Logger referenced
  concretely via `logger_log()` per cross-cutting convention.
- **P5 (Bounded resources).** Static singleton state. Mutex via
  `xSemaphoreCreateMutexStatic()`. Change callback array fixed at
  `CONFIG_MAX_CHANGE_CALLBACKS`. No heap after init.
- **P6 (Responsibility traces to requirements).** Get/set/commit
  trace to REQ-DM-000..002. Persistence traces to REQ-DM-090.
  Defaults trace to REQ-SA-010, SA-020, SA-050.
- **P7 (Pull-based data access).** Primary access is pull: consumers
  call `get_params()` on their own schedule. Change callbacks are the
  exception for consumers with immediate-reaction requirements (P7
  exception: "when a consumer's responsiveness requirement is tighter
  than the producer's production rate").

**P8 (Infrastructure traces to pain) does not apply.** No helper
infrastructure introduced.

**P9 (Multi-view diagrams) does not apply.** Not a diagram document.

- **P10 (Naming conventions).** Prefix `config_service_`. Interfaces
  `iconfig_provider_t`, `iconfig_manager_t`. Errors
  `CONFIG_SERVICE_ERR_*`. Parameters `CONFIG_PARAM_*`.

**Coding standard compliance (BARR-C:2018).** Fixed-width types for
all integer fields. Float used for sensor parameters (physical
quantities requiring decimal precision). Error enum returned from
every fallible function — no silent failures. Static allocation
throughout. `const` correctness on returned pointers and vtable
definitions.

---

## §17 References

- `docs/hld.md` §5.6 (ConfigService), §10 (sequence diagrams).
- `docs/components.md` (Application — ConfigService entry, ISP
  section).
- `docs/sequence-diagrams.md` SD-00a/b (boot), SD-07 (remote config),
  SD-08 (CLI config), SD-10 (provisioning).
- `docs/task-breakdown.md` §5.1 (ConfigService is reactive, no task).
- `docs/architecture-principles.md` (P1–P7, P10).

---

## §18 Open items

| ID | Item | Resolution path | Status |
|----|------|-----------------|--------|
| CFG-O1 | Schema version migration — current behaviour on mismatch is "discard and apply defaults." Future firmware upgrades may need partial migration (preserve valid fields, default only new ones). | Design migration path when first firmware upgrade requires it. | Open |
| CFG-O2 | String-field mutex documentation — `mqtt_broker` and `ntp_servers` are not atomically readable on Cortex-M4. All call sites that read string fields must use `config_service_lock()` / `config_service_unlock()`. Add per-field documentation in code and assertions. | Document at implementation; code review checklist item. | Open |
| CFG-O3 | `config_params_t` size — verified at compile time by `_Static_assert` in §7.3. | `config_params_t` is 444 B and `config_blob_t` 448 B (Gateway), against a ConfigStore capacity of 32 712 B. | Closed |
| CFG-O4 | FD ConfigService alignment — if the existing FD implementation uses a different init signature (e.g., explicit `IConfigStore *` parameter), align it with the singleton zero-param pattern documented here, or document the divergence as an intentional board difference. | The FD takes `const iconfig_store_t *`; the Gateway takes a `config_store_handle_t` because its ConfigStore is handle-based. Intentional board difference. | Closed |
| CFG-O5 | Packed `config_params_t` put some 16- and 32-bit fields at odd offsets, so a read overlapping a write could tear. | Natural alignment with explicit padding (CFG-D10), `_Static_assert`s in `config_params.h`, TC-CFG-049. Integer fields need no lock; only the strings do. | Closed |
| CFG-O6 | `config_service_restore_snapshot()` changes parameters without firing change callbacks, so a consumer that caches a value (poll period, publish interval) will not notice a rollback. | Fire one callback per changed parameter when the first consumer needs it. | Open |
| CFG-O7 | `set_param`, `flush` and `restore_snapshot` took 774 ms with the mutex held (a flash write), blocking readers and, for CloudPublisher, a 500 ms alarm budget. | Readers no longer wait: flash is written outside the data mutex (CFG-D15; TC-CFG-041). A caller that cannot block uses `set_param_deferred` (20 us on the bench, TC-HW-CFG-011) and flushes later with `persist_pending` (CFG-D16). The durable `set_param` still blocks its own caller for ~0.76 s; that cost is ConfigStore's (CFG-O9). | Closed for readers; caller-side cost tracked as CFG-O9 |
| CFG-O9 | ConfigStore erases and rewrites a whole 32 KB slot (8 sectors) on every save, because its CRC sits at the end of the slot and covers the padding. A 448-byte blob therefore costs ~0.76 s. | A ConfigStore format change (CRC next to the data, erase only the sectors the blob needs) would cut this to roughly one sector erase (~0.1 s). Changes validated middleware and its on-flash format, so it needs its own decision and PR. | Open |
| CFG-O8 | None of the Gateway consumers read ConfigService yet (CloudPublisher should use `set_param_deferred` and call `persist_pending` from a task that may block): SensorService, AlarmService (SS-O6), TimeService (TS-O4), ModbusPoller and CloudPublisher still use compile-time defaults, and nothing yet calls `apply_loaded` at boot (no LifecycleController or Gateway `main()`). | Wire the consumers to `iconfig_provider_t`, and the boot sequence to `config_store_load` + `apply_loaded`, when the Gateway entry point is built. | Open |

---

## §19 Decisions log

| ID | Decision | Rationale |
|----|----------|-----------|
| CFG-D1 | Singleton pattern with zero-param init | Consistent with all GW Application singletons (ModbusPoller, TimeService, SensorService). ConfigStore consumed via free functions. CMock link-time replacement for testing. |
| CFG-D2 | ISP split: `iconfig_provider_t` / `iconfig_manager_t` | P3 — SensorService/AlarmService need read-only access; ConsoleService/CloudPublisher need write access. Separate vtables prevent read-only consumers from seeing write API. |
| CFG-D3 | Mutex-free reads for 32-bit-aligned fields | Cortex-M4 guarantees atomic LDR for naturally-aligned 32-bit values. Eliminates mutex contention on the hot read path (SensorService polls every 100 ms). String fields excluded — require explicit lock/unlock (CFG-O2). |
| CFG-D4 | Pull-based primary access with callback exception | P7 — most consumers poll on their own cycle. Callbacks only for consumers that must react before their next cycle (ModbusPoller poll-period change, CloudPublisher interval change). |
| CFG-D5 | memcpy serialisation with packed struct and schema version | Simple, zero-overhead. Schema version field enables firmware upgrade detection. Packed attribute eliminates padding differences across compiler versions. Acceptable for single-platform embedded system (CFG-O1 tracks future migration). |
| CFG-D6 | Conditional compilation for board differences | `#if defined(BOARD_GATEWAY)` in `config_params.h`. No runtime branching. Same source file compiled for both boards. Validation rule tables board-specific for cleaner conditional logic. |
| CFG-D7 | Cross-parameter validation against current values | When changing one threshold, validate against current values of related parameters. Prevents inconsistent alarm configurations. Constraint: `alarm_low + hysteresis < alarm_high − hysteresis`. |
| CFG-D8 | ConfigService does not use IHealthReport | Per `components.md`, ConfigService USES ConfigStore and ILogger only. ConfigStore handles its own flash-failure health reporting. ConfigService logs via Logger for validation and persistence errors. |
| CFG-D9 | Reactive — no dedicated task | `task-breakdown.md` §5.1: ConfigService is reactive, called synchronously by ConsoleService, CloudPublisher, LifecycleController. Mutex provides thread safety across calling tasks. |
| CFG-D10 | Parameter types are the Field Device's fixed-point integers, not `float`. | Matches the FD schema and the SensorService / AlarmService units; no floating point on the Gateway. |
| CFG-D11 | `NTP_SERVERS` is set as a whole block; `NTP_SERVER_COUNT` must refer to filled slots; `MQTT_BROKER` is a 1..127 character string. | One `const void *` per `set_param`; setting servers and count separately keeps each call self-validating. |
| CFG-D12 | Absolute limits and range gaps for humidity and pressure filled in (§0). | v1.0 specified temperature only. |
| CFG-D13 | Stored values are validated against the stored set, repaired field by field, then checked as a sensor chain (§0). | Avoids rejecting valid pairs and avoids discarding a blob because the device is unprovisioned. |
| CFG-D14 | Change callbacks fire on any in-memory change, including when persisting failed. | Consumers must see the value that is actually live. |
| CFG-D15 | Two mutexes: data (short) and persist (the flash save). | The save takes ~0.76 s; holding the data mutex for it blocked every reader and every other writer for that long. A save that starts later always copies the newest values, so overlapping persists cannot leave older data in flash. |
| CFG-D16 | `config_service_set_param_deferred()`, `config_service_persist_pending()`, `config_service_has_pending()`, and `set_param_deferred` in `iconfig_manager_t`. | Lets a latency-sensitive caller apply and notify at once and flush from a context that may block. The durable `set_param` stays the default and also writes anything deferred before it. A deferred change is lost on a reset until `persist_pending` has run, so callers own that call. No task is added (§6, CFG-D9). |

---

## Phase H checklist

| # | Check | Status |
|---|-------|--------|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@note Threading` | Done (§4.2) |
| H-02 | Every error code documented in an error table with cause, behaviour, retry policy, and observability | Done (§13) |
| H-03 | Init ordering documented with pre-conditions | Done (§8) |
| H-04 | Synchronisation strategy documented | Done (§6) |
| H-05 | `_reset_for_test()` hook present, guarded by `#ifdef TEST` | Done (§7.5) |
| H-06 | File layout matches project directory structure | Done (§11; Gateway copy under `firmware/gateway/application/config_service/`, see §0) |
| H-07 | Unit-test plan uses CMock with TC-NNN identifiers | Done (§12 — TC-CFG-001..024) |
| H-08 | Open items have structured IDs and resolution paths | Done (§18 — CFG-O1..O4) |
| H-09 | Principles section contains no false claims | Done (§16) — P8, P9 noted as not applicable; coding standard separated from P-numbered principles |
| H-10 | SD trace table uses correct function names matching §4 API | Done (§14) |
| H-11 | State machine mapping — N/A (ConfigService has no state machine) | N/A |
| H-12 | Sizing estimates provided (RAM + ROM) | Done (§15) |
| H-13 | All SRS traces from header verified against section content | Done — REQ-DM-*, REQ-SA-* traced in §4, §9, §13 |
| H-14 | Decisions logged with rationale | Done (§19 — CFG-D1..D9) |

**WP12 D-8 (2026-11-01).** `config_params_t` gains `ntp_interval_min` (uint16, minutes, 1..1440, default 60), carved out of the former tail padding (`reserved_c[3]` became `reserved_c` + the field), so `sizeof` and the stored layout are unchanged and `CONFIG_SCHEMA_VERSION` stays 2: a stored 0 is invalid and `prv_repair` replaces it by the default. This deviates from the decision's "one schema bump" on purpose: a bump with an unchanged layout would discard every stored configuration and its profiles. `ntp_servers` may now hold host names. New `CONFIG_PARAM_NTP_INTERVAL_MIN`, console key `ntp-interval-min`, cloud key `ntp_interval_min`.
