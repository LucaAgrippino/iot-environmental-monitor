# LLD Companion — DeviceProfileRegistry

**Board:** Gateway only.
**Layer:** Application.

Holds the configurable registry of device profiles. Each profile
binds a Modbus slave address to an expected device identifier
(`MAP_VERSION`) and a human-readable description. Provides the polling
allowlist to `ModbusPoller`, supports profile management from the CLI
and from remote configuration, and delegates persistence to
`ConfigStore`.

**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** DeviceProfileRegistry in `components.md` (GW application layer)
---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `IDeviceProfileProvider` *(read-only)*, `IDeviceProfileManager` *(write)* |
| **Uses** | `IConfigStore`, `ILogger` |
| **Callers of `IDeviceProfileProvider`** | `ModbusPoller`, `ConsoleService` |
| **Callers of `IDeviceProfileManager`** | `ConfigService` *(remote config)*, `ConsoleService` *(CLI provisioning)* |

**Pattern: ISP (P3).** Read and write access are segregated into two
interfaces so `ModbusPoller` never acquires a write capability it does
not need.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Profile carries device ID, slave address, register-map spec | REQ-MB-110, MB-111 | UC-16, UC-19 |
| Polling allowlist provided to ModbusPoller | REQ-MB-100 | UC-07 |
| Profile binding during link establishment | REQ-MB-120 | UC-07 |
| Unsupported MAP_VERSION → reject from allowlist | REQ-MB-130 | UC-07 |
| Accept profile updates via provisioning and remote config | REQ-DM-100 | UC-15, UC-16 |
| Re-probe affected slave on add/update | REQ-DM-101 | UC-15, UC-16 |
| Persistence across reboots | REQ-DM-090 | — |

---

## 3. Profile data type

```c
#define DPR_MAX_PROFILES   8U
#define DPR_DESC_MAX      64U

typedef struct {
    uint8_t  slave_addr;              /* 1..247, Modbus RTU address         */
    uint16_t expected_map_version;    /* MAP_VERSION the gateway accepts
                                         from this slave type; links to the
                                         polling descriptor table in
                                         ModbusPoller LLD                   */
    char     description[DPR_DESC_MAX]; /* human-readable label              */
} device_profile_t;
```

**Register-map specification** is encoded as `expected_map_version`.
`ModbusPoller` holds a static const descriptor table that maps each
supported `MAP_VERSION` to the set of registers to poll. The profile
carries the version key; the polling knowledge lives in `ModbusPoller`.
Adding a new field-device type requires: (a) a new entry in
`ModbusPoller`'s descriptor table, and (b) a profile with the
corresponding `expected_map_version`. No firmware rebuild is needed for
the profile itself — it is pure configuration (D18).

---

## 2. Public API

### 4.1 `IDeviceProfileProvider` (read-only)

```c
typedef struct IDeviceProfileProvider IDeviceProfileProvider;

struct IDeviceProfileProvider {
    void *ctx;

    /* Look up a profile by slave address.
     * Returns DPR_OK and fills *out, or DPR_ERR_NOT_FOUND.             */
    dpr_err_t (*get_by_addr)(void *ctx,
                             uint8_t slave_addr,
                             device_profile_t *out);

    /* Populate out_addrs[] with all registered slave addresses.
     * Returns the count written (≤ max_count).                          */
    uint8_t   (*get_allowlist)(void *ctx,
                               uint8_t *out_addrs,
                               uint8_t max_count);

    /* Return the number of registered profiles.                         */
    uint8_t   (*get_count)(void *ctx);
};
```

### 4.2 `IDeviceProfileManager` (write)

```c
typedef struct IDeviceProfileManager IDeviceProfileManager;

struct IDeviceProfileManager {
    void *ctx;

    /* Add a new profile or overwrite an existing one with the same
     * slave_addr. Validates fields; persists to ConfigStore.
     * Returns DPR_ERR_FULL if DPR_MAX_PROFILES already registered.     */
    dpr_err_t (*add_or_update)(void *ctx,
                               const device_profile_t *profile);

    /* Remove the profile bound to slave_addr.
     * Returns DPR_ERR_NOT_FOUND if no such profile exists.              */
    dpr_err_t (*remove)(void *ctx, uint8_t slave_addr);
};
```

---

## 3. Internal design

```c
typedef struct {
    device_profile_t profiles[DPR_MAX_PROFILES];
    uint8_t          count;        /* active entries, 0..DPR_MAX_PROFILES */
    StaticSemaphore_t mutex_buf;
    SemaphoreHandle_t mutex;       /* protects profiles[] and count        */
    IConfigStore     *config_store;
    ILogger          *log;
} device_profile_registry_t;
```

The mutex is taken briefly for read (array copy) and write (array
update + persist call). `ConfigStore` I/O is not performed while the
mutex is held — see §8.

---

## 6. Algorithms

### 6.0 Storage model — LLD-D17

**Decision:** the initial implementation embeds a static const default profile
table directly in firmware flash. At boot, `dpr_load_from_store()` seeds the
runtime registry from ConfigStore; if ConfigStore returns an empty result (first
boot or factory reset), the registry is seeded instead from the compile-time
default table:

```c
/* device_profile_registry.c — compiled into firmware */
static const device_profile_t s_default_profiles[] = {
    /* { slave_addr, expected_map_version, description } */
    { 1, 0x0100, "Field Device #1 (default)" },
};
static const uint8_t s_default_profile_count =
    (uint8_t)(sizeof(s_default_profiles) / sizeof(s_default_profiles[0]));
```

**Rationale (LLD-D17).** The field device roster is known at firmware build
time for the first production deployment. Embedding the defaults eliminates a
dependency on a provisioned ConfigStore entry for system startup, reduces
the risk of a factory-blank gateway failing to poll anything, and avoids a
mandatory provisioning step during manufacturing.

**Interface seam (future migration path).** `IDeviceProfileProvider` and
`IDeviceProfileManager` are the stable interface boundaries. The static const
table is an implementation detail of the concrete registry instance. A future
revision can replace the static table with a ConfigStore-only or cloud-sourced
boot load without any change to `ModbusPoller`, `ConsoleService`, or
`ConfigService` — they all consume the interfaces, not the implementation.

**Singleton vtable (LLD-D10).**

```c
/* device_profile_registry.h */

typedef struct {
    dpr_err_t (*get_by_addr)(uint8_t slave_addr, device_profile_t *out);
    uint8_t   (*get_allowlist)(uint8_t *out_addrs, uint8_t max_count);
    uint8_t   (*get_count)(void);
} idevice_profile_provider_t;

typedef struct {
    dpr_err_t (*add_or_update)(const device_profile_t *profile);
    dpr_err_t (*remove)(uint8_t slave_addr);
} idevice_profile_manager_t;

extern const idevice_profile_provider_t * const device_profile_provider;
extern const idevice_profile_manager_t  * const device_profile_manager;
```

The ctx-based `IDeviceProfileProvider` / `IDeviceProfileManager` structs above
(§4.1, §4.2) document the interface contract; the singleton vtables are the
LLD-D10-conformant wire-up. Unit tests substitute mock vtables via the same
`*(const idevice_profile_provider_t **)&device_profile_provider = &mock;`
pattern used elsewhere.

### 6.1 `get_by_addr`

```
get_by_addr(slave_addr, out):
    take mutex
    for i in 0..count-1:
        if profiles[i].slave_addr == slave_addr:
            *out = profiles[i]
            release mutex
            return DPR_OK
    release mutex
    return DPR_ERR_NOT_FOUND
```

### 6.2 `get_allowlist`

```
get_allowlist(out_addrs, max_count):
    take mutex
    n = min(count, max_count)
    for i in 0..n-1:
        out_addrs[i] = profiles[i].slave_addr
    release mutex
    return n
```

Called by `ModbusPoller` at boot and after each `add_or_update` or
`remove` to refresh its internal poll schedule.

### 6.3 `add_or_update`

```
add_or_update(profile):
    validate(profile)   /* slave_addr 1..247; map_version != 0;
                           description not empty — DPR_ERR_INVALID */

    take mutex
    /* Check for existing entry with same slave_addr */
    for i in 0..count-1:
        if profiles[i].slave_addr == profile.slave_addr:
            profiles[i] = *profile
            release mutex
            persist()   /* ConfigStore write outside mutex */
            return DPR_OK

    /* New entry */
    if count >= DPR_MAX_PROFILES:
        release mutex
        return DPR_ERR_FULL

    profiles[count] = *profile
    count++
    release mutex
    persist()
    return DPR_OK
```

The caller (`ConfigService`) is responsible for triggering the re-probe
via `IModbusPoller.reprobe_slave(slave_addr)` after `add_or_update`
returns `DPR_OK` (REQ-DM-101). `DeviceProfileRegistry` does not call
`ModbusPoller` directly — it has no `USES` dependency on it (P1).

### 6.4 `remove`

```
remove(slave_addr):
    take mutex
    for i in 0..count-1:
        if profiles[i].slave_addr == slave_addr:
            /* Compact array */
            profiles[i] = profiles[count-1]
            count--
            release mutex
            persist()
            return DPR_OK
    release mutex
    return DPR_ERR_NOT_FOUND
```

---

## 7. Persistence

### 7.1 Serialisation format

A compact flat binary blob, written to `ConfigStore` under the key
`CONFIG_KEY_DEVICE_PROFILES`:

```
[ count : 1 byte ]
[ profile_0 : sizeof(device_profile_t) bytes ]
[ profile_1 : sizeof(device_profile_t) bytes ]
...
[ profile_{count-1} ]
```

`sizeof(device_profile_t)` = 1 (slave_addr) + 2 (map_version) + 64 (desc)
= **67 bytes**. Maximum blob size: 1 + 8 × 67 = **537 bytes**. Well within
`ConfigStore`'s 32 KB slot.

### 7.2 `persist()` (internal)

```
persist():
    serialise profiles[] → buf[]
    config_store->write(config_store,
                        CONFIG_KEY_DEVICE_PROFILES,
                        buf, buf_len)
```

Called outside the mutex (the array copy under the mutex ensures a
consistent snapshot). `ConfigStore.write()` is not reentrant-safe for
concurrent writes to the same key, but since all write paths (CLI, remote
config) funnel through `IDeviceProfileManager` and take the registry
mutex before modifying the array, only one `persist()` can be in flight
at a time.

### 7.3 Boot load

Called from `LifecycleController` during Init (SD-00b):

```
dpr_load_from_store(self):
    config_store->read(config_store,
                       CONFIG_KEY_DEVICE_PROFILES,
                       buf, sizeof buf, &len)
    if len == 0:
        count = 0  /* first boot — no profiles */
        log_info("DPR: no profiles found, registry empty")
        return DPR_OK
    deserialise(buf, len, profiles, &count)
    log_info("DPR: loaded %u profile(s)", count)
    return DPR_OK
```

If deserialisation fails (truncated blob, unknown format), the registry
starts empty and logs a warning. Profiles must be re-provisioned.

---

## 8. Concurrency

| Caller | Interface | Task | Protection |
|---|---|---|---|
| `LifecycleController` | `dpr_load_from_store()` | Init context (single-threaded boot) | No mutex needed at boot |
| `ModbusPoller` | `IDeviceProfileProvider` | `ModbusMasterTask` | Mutex taken in `get_by_addr`, `get_allowlist` |
| `ConsoleService` | Both | `ConsoleTask` | Mutex taken in all methods |
| `ConfigService` | `IDeviceProfileManager` | `CloudPublisherTask` | Mutex taken in `add_or_update`, `remove` |

The mutex is a FreeRTOS static binary semaphore, created at
`device_profile_registry_init()`. Maximum hold time: one array scan
(≤ 8 iterations) — microseconds.

`ConfigStore.write()` is called outside the mutex to avoid holding the
registry lock during flash I/O (which may block for milliseconds on
QSPI sector erase). The write uses the serialised snapshot taken under
the mutex, so it is consistent even if a concurrent reader updates the
live array between mutex release and the write call. The next `persist()`
call (from the next write operation) will overwrite with the latest state.

---

## 9. Validation rules

| Field | Rule |
|---|---|
| `slave_addr` | 1..247 (Modbus spec) |
| `expected_map_version` | ≠ 0 |
| `description` | At least one non-null character; null-terminated within 64 bytes |
| Duplicate slave_addr | Allowed — treated as update, not error |

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

```c
typedef enum {
    DPR_OK = 0,
    DPR_ERR_NOT_FOUND,
    DPR_ERR_FULL,
    DPR_ERR_INVALID,       /* validation failure          */
    DPR_ERR_PERSIST,       /* ConfigStore write failed    */
    DPR_ERR_LOAD,          /* ConfigStore read / deserialise failed */
    DPR_ERR_NULL_ARG,
    DPR_ERR_NOT_INIT,
} dpr_err_t;
```

`DPR_ERR_PERSIST` is non-fatal for the registry itself — the in-memory
state is consistent even if the flash write fails. The error is returned
to the caller (ConfigService or ConsoleService) which logs it and reports
the condition upstream.

`DPR_ERR_LOAD` at boot — logged as a warning; registry starts empty.
`LifecycleController` does not treat this as Faulted; it continues to
Operational with no slaves polled (all link probes will fail until
profiles are re-provisioned).

---

## 11. Initialisation

```c
dpr_err_t device_profile_registry_init(device_profile_registry_t *self,
                                        IConfigStore              *config_store,
                                        ILogger                   *log);
```

Creates the mutex. Does **not** load profiles — loading is a separate
`dpr_load_from_store(self)` call made by `LifecycleController` during
Init, after `ConfigStore` is ready.

---

## 12. Memory and sizing

| Item | Size |
|---|---|
| `profiles[]` (8 × 67 B) | 536 B |
| `count`, mutex, handles | ~24 B |
| Serialisation buf (stack, in `persist()`) | 537 B |
| **Total static RAM** | **~560 B** |

Serialisation buffer is stack-allocated inside `persist()` — no static
buffer needed.

---

## 7. Unit-test plan

### 13.1 Unit tests — `tests/application/test_device_profile_registry.c`

Mocks: `IConfigStore`, `ILogger`.

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; mutex created; count = 0 |
| `load_from_store` — empty | ConfigStore returns 0 bytes; count remains 0 |
| `load_from_store` — 2 profiles | Deserialises correctly; count = 2 |
| `load_from_store` — corrupt blob | Logged warning; count = 0 |
| `add_or_update` — new | Profile added; count++; ConfigStore.write called |
| `add_or_update` — update existing | Profile overwritten; count unchanged; ConfigStore.write called |
| `add_or_update` — full (8 profiles) | Returns DPR_ERR_FULL; count unchanged |
| `add_or_update` — invalid slave_addr (0) | Returns DPR_ERR_INVALID; no write |
| `add_or_update` — invalid slave_addr (248) | Returns DPR_ERR_INVALID; no write |
| `add_or_update` — map_version 0 | Returns DPR_ERR_INVALID; no write |
| `remove` — existing | Profile removed; count--; array compacted; ConfigStore.write called |
| `remove` — not found | Returns DPR_ERR_NOT_FOUND; no write |
| `get_by_addr` — found | Returns correct profile |
| `get_by_addr` — not found | Returns DPR_ERR_NOT_FOUND |
| `get_allowlist` — 3 profiles | Returns 3 addresses |
| `get_allowlist` — max_count < count | Returns max_count entries only |
| Persist failure | ConfigStore.write returns error; DPR_ERR_PERSIST returned; in-memory state unchanged |

### 13.2 Integration tests — on target

| Test | Setup |
|---|---|
| Profile survives reboot | Add profile via CLI; reboot; verify profile present and FD polled |
| Remove profile stops polling | Remove profile via CLI; verify slave not polled |
| Remote profile update | Push profile update via MQTT; verify re-probe triggered; verify FD polled on new slave address |
| Allowlist enforced | Add two profiles with different slave addresses; verify only those addresses polled |

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| **DPR-O1** | `expected_map_version` → polling descriptor binding: the static descriptor table lives in `ModbusPoller`. Confirm the exact field name and lookup API when `ModbusPoller` LLD is finalised. |
| **DPR-O2** | Deserialisation schema versioning: if `device_profile_t` layout changes in a future firmware version, the stored blob will be incompatible. Add a schema version prefix to the blob (single byte) at first implementation. Cross-reference CS-O1. |

---

## 15. References

- `docs/components.md` (GW DeviceProfileRegistry entry).
- `docs/hld.md` §14 (D14, D17, D18 — profile binding and allowlist design decisions).
- `docs/sequence-diagrams.md` SD-00b (boot load), SD-07 (remote profile update and re-probe).
- `docs/modbus-register-map.md` §8 (MAP_VERSION versioning and compatibility policy).
- `docs/architecture-principles.md` P1 (P1 — DPR does not call ModbusPoller), P3 (ISP split).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
