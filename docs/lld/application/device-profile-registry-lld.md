# LLD Companion — DeviceProfileRegistry

**Board:** Gateway only.
**Layer:** Application.

Holds the configurable registry of device profiles. Each profile binds a Modbus slave address to the
identity the Gateway expects from that slave (map version and vendor code) and a human-readable
description. Provides the profile lookup to `ModbusPoller` and `ConsoleService`, accepts profile
changes from the console and from remote configuration, and hands persistence to a storage callback.

**Version:** 0.2
**Date:** October 2026
**Status:** Draft — reconciled with the as-built on 2026-10-31 (WP8 session 1 analysis,
`docs/lld/gate-review/wp8-device-profile-registry-analysis.md`); decisions D-1..D-8 accepted by Luca.
**HLD anchor:** DeviceProfileRegistry in `components.md` (GW application layer)

---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `idevice_profile_provider_t` (read), `idevice_profile_manager_t` (write) |
| **Uses** | a storage callback pair (`device_profile_storage_t`), `Logger` |
| **Callers of the provider** | `ModbusPoller`, `ConsoleService` |
| **Callers of the manager** | `ConfigService` (remote configuration), `ConsoleService` (provisioning) |
| **Called at boot by** | `LifecycleController` / `main()`: `device_profile_registry_init()` then `_load()` |

**Pattern: ISP (P3).** Read and write are separate vtables, so `ModbusPoller` never holds a write capability.

**Naming (D-8, fixes M-3).** Module prefix `device_profile_registry_`; type `device_profile_t` (kept: it is
the poller's type today); errors `device_profile_registry_err_t` / `DEVICE_PROFILE_REGISTRY_ERR_*`;
capacity `DEVICE_PROFILE_MAX`. The names `DPR_*`, `dpr_err_t`, `DPR_MAX_SLAVES` of v0.1 are retired.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Registry, one entry per expected field device | REQ-MB-110 | UC-16, UC-19 |
| Profile content: identifier, description, address, register-map specification | REQ-MB-111 | UC-16, UC-19 |
| Polling allowlist | REQ-MB-100 | UC-07 |
| Identity check at link establishment, reject mismatch | REQ-MB-120 | UC-07 |
| Log validation failures with the identifier and address | REQ-MB-130 | UC-07 |
| Accept updates from provisioning and remote configuration | REQ-DM-100 | UC-15, UC-16 |
| Re-probe an affected slave on add or update | REQ-DM-101 | UC-15, UC-16 |
| Persistence across reboots | REQ-DM-090 | — |

---

## 3. Profile data type (D-1, D-4)

```c
#define DEVICE_PROFILE_MAX (8U)
#define DEVICE_PROFILE_DESC_MAX (32U) /* including the terminating NUL */

typedef struct
{
    uint8_t slave_addr;    /* 1..247                                                      */
    uint16_t map_version;  /* MAP_VERSION (register 0x0000) the Gateway accepts; != 0     */
    uint16_t vendor_code;  /* VENDOR_CODE (register 0x0007) the slave must return; != 0   */
    char description[DEVICE_PROFILE_DESC_MAX];
} device_profile_t;
```

**The register-map specification is the map version (REQ-MB-111, D-1).** The span to poll, the scaling and the
channel decode for each supported `map_version` are a `const` table owned by ModbusPoller
(`modbus_map_table`, version 1: FC04 `0x0010`, 3 registers). The profile carries the key. Adding a new
device type needs a table entry (a firmware change); adding a unit of a known type does not.
**This replaces the placeholder fields `input_reg_start` / `input_reg_count` of `device_profile_t`.**

**The identity (D-4, corrected 2026-10-31).** v0.1 and the first analysis assumed a `device_class` register.
The as-built Field Device has none: `0x0001` and `0x0002` are a 32-bit device ID (`MRM_DEVICE_ID_HI/LO`),
currently the constant 0 on every unit, and `0x0007` is `VENDOR_CODE` (`0x1A45`). A constant ID cannot
distinguish units, so the check is **map version supported and equal to the profile's, and vendor code
equal to the profile's**. When units get distinct IDs a 32-bit `device_id` field can be added with a
schema bump. (The Field Device register-map LLD still lists `0x0001` as `device_class`; it disagrees with
its own code and is recorded as RM-F1 for the next revision of that document.)

**Description** is 32 bytes including the NUL (was 64): console input lines are short and the whole table
is 8 × 38 B = 304 B.

---

## 4. Public API

```c
typedef enum
{
    DEVICE_PROFILE_REGISTRY_OK = 0,
    DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
    DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT,
    DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND,
    DEVICE_PROFILE_REGISTRY_ERR_FULL,
    DEVICE_PROFILE_REGISTRY_ERR_INVALID,
    DEVICE_PROFILE_REGISTRY_ERR_PERSIST,
    DEVICE_PROFILE_REGISTRY_ERR_LOAD
} device_profile_registry_err_t;

typedef struct
{
    bool (*get_next)(device_profile_t *out, uint8_t *index);
    device_profile_registry_err_t (*get_by_addr)(uint8_t slave_addr, device_profile_t *out);
    bool (*is_registered)(uint8_t slave_addr);
} idevice_profile_provider_t;

typedef struct
{
    device_profile_registry_err_t (*add_or_update)(const device_profile_t *profile);
    device_profile_registry_err_t (*remove)(uint8_t slave_addr);
} idevice_profile_manager_t;

extern const idevice_profile_provider_t *const device_profile_provider;
extern const idevice_profile_manager_t *const device_profile_manager;
```

`get_next` keeps the iterator contract ModbusPoller already uses (`*index` = 0 first; each true return
advances `*index`) but **copies** the profile into `*out` under the lock (changed from a pointer into the
table, because a writer swaps the whole table). A copy is 38 bytes.
The free functions `device_profile_get_next` and `device_profile_is_registered` of the placeholder remain
as thin wrappers over the vtable until the consumers migrate.

---

## 5. Internal design

```c
typedef struct
{
    device_profile_t profiles[DEVICE_PROFILE_MAX];
    uint8_t count;
    uint32_t revision;           /* +1 on every successful change; lets a reader see that it changed */
    SemaphoreHandle_t mutex;     /* static binary semaphore, created by init                         */
    device_profile_storage_t storage;
    bool initialised;
} device_profile_registry_inst_t;   /* one static instance */
```

Singleton, no heap, mutex held for one array scan (≤ 8 iterations). Task-safe, not ISR-safe.

### 5.1 Storage callback (D-2, fixes M-4 and M-5)

ConfigStore stores one blob and ConfigService owns it. The registry therefore does not call ConfigStore:

```c
typedef struct
{
    /* Read the stored profiles. Returns false if there are none or they are unreadable. */
    bool (*load)(device_profile_t *out, uint8_t max, uint8_t *count_out);
    /* Store the profiles; returns false if the write failed. May block for ~0.8 s (CFG-O7). */
    bool (*save)(const device_profile_t *in, uint8_t count);
} device_profile_storage_t;
```

`main()` binds the pair to ConfigService (`config_service_get_profiles` / `config_service_set_profiles`).
ConfigService's blob gains `profiles[DEVICE_PROFILE_MAX]` and `profile_count`; `CONFIG_SCHEMA_VERSION`
becomes 2 and a schema-1 blob loads with an empty profile list (so the default is seeded, §6.1).
The registry has no compile-time dependency on ConfigService or ConfigStore (P1, P2).

### 5.2 Validation

| Field | Rule |
|---|---|
| `slave_addr` | 1..247 |
| `map_version`, `vendor_code` | ≠ 0 |
| `description` | NUL-terminated within 32 bytes, at least one printable character |
| Existing address | An update, not an error (fixes M-13) |

A `map_version` the poller's table does not know is **accepted** (the profile is configuration) but the
slave will be `link_rejected` until the table knows it; the console warns.

---

## 6. Algorithms

### 6.1 `load` and the default profile (D-6, fixes M-6)

```
load():
    ok = storage.load(profiles, MAX, &count)
    if !ok or count == 0:
        profiles[0] = { 1, 1, 0x1A45, "Field Device 1 (default)" }; count = 1   /* not persisted */
        log_info("DPR: no stored profiles, default seeded")
    else
        log_info("DPR: loaded %u profile(s)", count)
    revision++
    return OK
```

The default is written to flash only by the first successful change. A corrupt blob also yields the default
and a WARN, so a factory-blank or damaged Gateway still polls its one Field Device. A factory reset
(ConfigService erase) returns to the default. This resolves the contradiction between v0.1 §6.0 and §7.3.

### 6.2 `add_or_update` and `remove` (D-3)

```
add_or_update(p):
    validate(p) else INVALID
    take write_mutex                            /* one writer at a time            */
    take mutex; candidate = copy of table and count; release mutex
    update candidate in place, or append (FULL if count == MAX)
    if !storage.save(candidate, count):         /* flash I/O, no lock held         */
        release write_mutex; return PERSIST     /* the live table was never touched */
    take mutex; table = candidate; revision++; release mutex
    release write_mutex
    return OK
```

The change is made on a **candidate copy** and the live table is replaced only after the save succeeded, so
a failed save needs no rollback and a reader (the poller) can never see a table that is not on flash: what
runs is what survives a reboot (D-3). `remove` swaps the last entry into the freed slot on the candidate
(order is not significant) and saves the same way. `mutex` protects the live table and is never held across
`save`; `write_mutex` serialises writers for the whole operation; readers never take `write_mutex`.

### 6.3 Re-probe (D-5, fixes M-8)

The registry does not call ModbusPoller (P1). After a successful `add_or_update` the **caller**
(ConfigService, ConsoleService) calls `modbus_poller_reprobe(slave_addr)`, which marks that slave
unverified so the next poll cycle repeats the identity check (§7.1). A `remove` needs no re-probe: the
slave is no longer in the registry and is not polled.

---

## 7. ModbusPoller behaviour this module requires (D-4, D-5, D-7)

### 7.1 Identity check at link establishment (REQ-MB-120, REQ-MB-130; fixes M-9)

A slave starts **unverified**. Before its first data poll (and again after `reprobe`) the poller reads FC04
`0x0000`, count 8 (map version, device ID, hardware revision, firmware version, vendor code) and compares:

| Result | Action |
|---|---|
| Map version = profile's, supported by the table, vendor code = profile's | Slave **verified**; normal polling begins |
| Any difference | Slave **link_rejected**; WARN log once per transition: slave address, expected and got map version and vendor code (REQ-MB-130); no data polling |
| No response | Ordinary link failure (retries, hysteresis); verification is retried on the next cycle |

A `link_rejected` slave is re-checked at the existing link-retry interval, so replacing the device with the
right one recovers without a reboot. It is logged on entering the state, not each re-check. It reports
`HEALTH_EVENT_MODBUS_LINK_DOWN` as a failed link does; whether a distinct health event is worth adding is
open item DPR-O3.

### 7.2 Map table (D-1)

`modbus_map_table[]` in ModbusPoller: `{ map_version, input_reg_start, input_reg_count, decode }`.
Version 1: `0x0010`, 3 registers, temperature / humidity / pressure. The poller builds its FC04 request
from the table entry of the slave's profile, not from profile fields.

### 7.3 Copy, not pointer

The poller and the console receive a copy of each profile, so a write that commits during a poll cycle
never changes what they are using.

---

## 8. Concurrency

| Caller | Interface | Task | Protection |
|---|---|---|---|
| `main()` | `init`, `load` | Boot, single-threaded | none needed |
| `ModbusPoller` | provider | `ModbusPollerTask` | `mutex` per call |
| `ConsoleService` | both | `ConsoleTask` | `mutex` per read; `write_mutex` for writes |
| `ConfigService` | manager | the task that runs remote commands | `write_mutex` for the whole write |

`save` blocks for ~0.8 s; the poller is never blocked by it because readers do not take `write_mutex`,
and `mutex` is released before `save`.

---

## 9. Error and fault behaviour

| Error | Cause | Local behaviour | Observability |
|---|---|---|---|
| `ERR_NOT_FOUND` | Address not registered | None | Not logged (normal query result) |
| `ERR_FULL` | 8 profiles registered | Not added | WARN |
| `ERR_INVALID` | Failed validation | Registry unchanged | WARN |
| `ERR_PERSIST` | `save` failed | Live table untouched (the change was made on a copy, D-3) | WARN |
| `ERR_LOAD` | Not returned by `load` (§6.1 seeds the default) | Reserved | — |
| `ERR_NULL_ARG` / `ERR_NOT_INIT` | Programming error | None | ERROR |

---

## 10. Memory

Table 8 × 38 B = 304 B, plus count, revision, two static semaphores and the storage pair: about 400 B static.
A writer holds one candidate copy on its stack (8 × 38 B + count, about 310 B). ConfigService's blob grows by
8 × 38 + 1 B.

---

## 11. Unit-test plan (host, `tests/gateway/application/device_profile_registry/`)

Mocks: storage pair, Logger, mutex via the FreeRTOS host fake.

| ID | Case |
|---|---|
| TC-DPR-001 | `init` rejects NULL storage; second `init` is idempotent |
| TC-DPR-002 | Functions before `init` return `NOT_INIT` |
| TC-DPR-003 | `load` with nothing stored: default profile `{1, 1, 0x1A45}` seeded, not saved |
| TC-DPR-004 | `load` with 2 stored profiles: count 2, contents equal |
| TC-DPR-005 | `load` when the storage reports failure: default seeded, WARN logged |
| TC-DPR-006 | `add_or_update` new: appended, saved once, revision +1 |
| TC-DPR-007 | `add_or_update` existing address: overwritten, count unchanged, saved |
| TC-DPR-008 | `add_or_update` with 8 present and a new address: `FULL`, nothing saved |
| TC-DPR-009 | Invalid: address 0, 248; `map_version` 0; `vendor_code` 0; empty description; unterminated description: `INVALID`, nothing saved |
| TC-DPR-010 | `save` fails on add: `PERSIST`, table and count as before |
| TC-DPR-011 | `save` fails on update: old contents restored |
| TC-DPR-012 | `save` fails on remove: entry still present |
| TC-DPR-013 | `remove` existing: count−1, saved; `remove` unknown: `NOT_FOUND`, nothing saved |
| TC-DPR-014 | `get_by_addr` found / not found |
| TC-DPR-015 | `get_next` iterates all, then false; NULL arguments return false |
| TC-DPR-016 | `is_registered` true / false |
| TC-DPR-017 | A reader called from inside a `save` (before and during a failing save) sees the old table |
| TC-DPR-018 | The description is copied, not referenced (caller buffer changed after the call) |

On target (harness): profile survives a reboot; removed profile is no longer polled; identity mismatch
gives `link_rejected` and a log line with both versions; a corrected profile recovers.

---

## 12. Open items

| ID | Item | Status |
|---|---|---|
| DPR-O1 | The map table lives in ModbusPoller (D-1); the exact name and layout are fixed by the ModbusPoller PR | Open (implementation) |
| DPR-O2 | Schema versioning: closed — `CONFIG_SCHEMA_VERSION` 2 carries the profiles (D-2) | Closed |
| DPR-O3 | Distinct health event for `link_rejected`, or reuse `LINK_DOWN` | Open |
| DPR-O4 | A 32-bit `device_id` in the profile once Field Devices have distinct IDs (they return 0 today) | Open |
| RM-F1 | The Field Device register-map LLD lists `0x0001` as `device_class`; the code has `device_id_hi` | Open (FD LLD) |
| DPR-O5 | Visual Paradigm: DeviceProfileRegistry is absent from the Gateway component diagram (COMP-DIAG-01) | Open (Luca) |

---

## 13. References

- `docs/hld/components.md` (DeviceProfileRegistry entry); `docs/hld/sequence-diagrams.md` SD-00b, SD-07, SD-10, Q8.
- `docs/SRS.md` REQ-MB-100/110/111/120/130, REQ-DM-090/100/101.
- `docs/lld/gate-review/wp8-device-profile-registry-analysis.md` (mismatches M-1..M-17, decisions D-1..D-8).
- `docs/lld/application/modbus-poller-lld.md` (MBP-O4), `config-service-lld.md` (CFG-O7).
- `docs/architecture-principles.md` P1, P3.
