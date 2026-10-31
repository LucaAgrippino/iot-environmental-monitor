# WP8 — DeviceProfileRegistry: LLD against the as-built (session 1)

**Date:** 2026-10-31 · **Gate:** GATE-DESIGN. **Luca accepted all recommendations D-1 to D-8 on 2026-10-31** (section 7).
**Compared:** `device-profile-registry-lld.md` v0.1 (Draft, May 2026) with the placeholder
(`firmware/gateway/application/device_profile_registry/`), ModbusPoller, ConfigStore, ConfigService,
ConsoleService, the SRS (REQ-MB-110/-111/-120/-130, REQ-DM-100/-101) and the Field Device register map.

## 1. What exists

The placeholder is two weak functions, `device_profile_get_next` and `device_profile_is_registered`, serving one
hard-coded profile (slave 1, FC04 span 0x0010, 3 registers). ModbusPoller and ConsoleService (`profiles list`) call
them. `profiles add` and `profiles remove` exist as commands that validate the address and answer "read-only on the
Gateway". Nothing else uses the registry: no persistence, no device-ID check, no re-probe.

## 2. Mismatches between the LLD and the as-built

| # | LLD v0.1 says | As built | Consequence |
|---|---|---|---|
| M-1 | `device_profile_t` = `slave_addr`, `expected_map_version`, `description[64]` (67 B) | `device_profile_t` = `slave_addr`, `input_reg_start`, `input_reg_count` (placeholder), used by the poller to build every FC04 request and decode the response | The poller needs a register span per slave; the LLD profile has none. LLD-D18 says it comes from a descriptor table in ModbusPoller keyed by map version, but the poller has no such table (hard-coded in the profile) |
| M-2 | Read API: `get_by_addr`, `get_allowlist`, `get_count`, ctx-based and singleton vtables | Poller calls `device_profile_get_next(&profile, &index)` and `device_profile_is_registered(addr)`; ConsoleService has its own `idevice_profile_provider_t` with only `get_next` | Three different read APIs. MBP-O4 already asks to reconcile |
| M-3 | Names `dpr_err_t`, `DPR_ERR_*`, `DPR_MAX_PROFILES`, and also `device_profile_registry_err_t`, `DEVICE_PROFILE_REGISTRY_ERR_*`, `DEVICE_PROFILE_MAX`, `DPR_MAX_SLAVES` in the same document | Project naming is `device_profile_registry_` prefix | One naming set must be chosen; the LLD uses four |
| M-4 | Persistence: `config_store->write(key = CONFIG_KEY_DEVICE_PROFILES, buf)` and `read(key, ...)` | ConfigStore has **no keys**: `config_store_save(handle, blob, len)` and `config_store_load` handle one blob (max 32 712 B) in a 64 KB double-slot region. ConfigService already owns it: its blob is `schema_version` + `config_params_t` | The LLD's persistence interface does not exist (see D-2) |
| M-5 | Interface to ConfigStore is `IConfigStore`, injected | `config_store_handle_t` functions, no vtable | Dependency injection style differs; a vtable or a ConfigService hook is needed |
| M-6 | Boot: `LifecycleController` calls `dpr_load_from_store()`; empty store seeds `s_default_profiles` (LLD-D17), but §7.3 says an empty store gives an empty registry | No call exists. §6.0 and §7.3 contradict each other | First boot must serve one default profile (the Gateway polls nothing otherwise); §7.3 and the error table must be fixed |
| M-7 | `get_allowlist` is "called by ModbusPoller at boot and after each add or remove to refresh its schedule" | The poller walks the registry every cycle; it caches nothing | No refresh hook is needed if the registry stays the single source |
| M-8 | Re-probe on add/update: ConfigService calls `IModbusPoller.reprobe_slave(addr)` (REQ-DM-101) | `IModbusPoller` has no `reprobe_slave` | New poller API, or a poll-cycle rule that makes it unnecessary (D-5) |
| M-9 | Device-ID check at link establishment (REQ-MB-120/130, SD-00b steps 10 to 15'): read the device-ID register, compare, log mismatch, mark `link_rejected` | The poller has **no identity check**. The first poll marks the link up. No `link_rejected` state exists | New poller behaviour; also `modbus_slave_state_t` and the health events change |
| M-10 | "Device identifier" and `MAP_VERSION` used interchangeably (DPR-O1) | The Field Device exposes `MAP_VERSION` at 0x0000 (value 1, `MRM_MAP_VERSION`) and `device_class` at 0x0001. The SRS says "device identifier" | Which register is the identity is undecided (D-4) |
| M-11 | Console: `profiles add addr=N sensors=<list>`, `profiles remove <addr>`; LLD-D17 calls the manager `add_or_update` | The `sensors=` argument matches no field of the LLD profile | The console syntax follows the register-map idea (Q8), not the LLD |
| M-12 | `DPR_ERR_PERSIST`: "in-memory state already updated" in the error table, but "in-memory state unchanged" in the test plan | | Contradiction; pick one (D-3) |
| M-13 | `DPR_ERR_INVALID` includes "duplicate address", §9 says a duplicate is an update | | Contradiction |
| M-14 | Concurrency table: ModbusPoller runs in `ModbusMasterTask`, ConfigService in `CloudPublisherTask` | Tasks are `ModbusPollerTask`, and ConfigService is called from the cloud command path and the console | Update names; the mutex design itself holds |
| M-15 | Unit tests in `tests/application/` | Tests live under `tests/gateway/application/` | Path |
| M-16 | Section numbers repeat (two sections 2 and 3, 5 to 8 reused, 13.1 under 7) | | Renumber when the LLD is rewritten |
| M-17 | DPR-O2: add a schema-version byte at first implementation | CS-O1 and `CONFIG_SCHEMA_VERSION` exist for the config blob | Resolved by whichever persistence option is chosen |

## 3. What the requirements need

| Requirement | Needs |
|---|---|
| REQ-MB-110 | A configurable registry, one entry per expected Field Device |
| REQ-MB-111 | Each profile has a device identifier, a description, a slave address, **a register-map specification** |
| REQ-MB-120/130 | At link establishment, read the device identifier, compare with the profile, reject a mismatch, log expected, got and slave address |
| REQ-DM-100 | Updates through the console (UC-16) and remote configuration (UC-15) |
| REQ-DM-101 | Adding or updating a profile at runtime triggers re-probing |
| REQ-DM-090 (LLD trace) | Persist across reboots |

Today only the first half of REQ-MB-110 is partly met, and none of the rest.

## 4. Design questions for you

**D-1 Q8: the register-map specification inside a profile.** REQ-MB-111 requires one; the LLD reduces it to a map version.

| Option | What a profile holds | Cost |
|---|---|---|
| A. Version key + built-in table (LLD) | `expected_map_version`; the poller has a const table `map_version` to FC04 span, scaling and channel decode | No new format. Adding a device type needs firmware. Honest fit for one implemented device |
| B. Span in the profile | `input_reg_start`, `input_reg_count` stored per profile (today's placeholder) plus version | Operator can retarget a span; decode of channels is still built in |
| C. Full descriptor (address, type, scaling, unit, access) | A variable-size table per profile | The Q8 "schema design" in full; large blob, parser, validation, hostile-input handling. Beyond what one device needs |

Recommendation: **A, with the span taken from the table.** It satisfies REQ-MB-111 in substance (the map version *is* the register-map specification, REQ-MB-100 says only one device is implemented) and keeps the profile at address, version and description.

**D-2 Persistence.** ConfigStore stores one blob and ConfigService owns it.

| Option | How | Cost |
|---|---|---|
| A. Profiles inside the ConfigService blob | Blob gets a second member `profiles[8]` + count; `CONFIG_SCHEMA_VERSION` 2; registry asks ConfigService to save | One flash write path, one integrity check. Registry depends on ConfigService (or ConfigService on the registry): a layering question. Every profile edit rewrites 16 sector erases (as every config edit does today, 774 ms, CFG-O7) |
| B. A second ConfigStore instance on its own region | New 64 KB-class partition | The flash layout has no free QSPI region assigned for it; needs a layout change and a second store; most isolated |
| C. A new small key-value layer | Out of scope | Largest change |

Recommendation: **A**, with the registry owning the in-memory table and a narrow "profiles section" the config blob carries, written through ConfigService so the LLD's "registry uses IConfigStore" becomes "registry uses a persistence callback". Schema 2 must migrate schema 1 blobs (profiles default).

**D-3 Failed persistence.** If saving fails after an update, keep the new in-memory profile (device keeps working, caller told `ERR_PERSIST`) or roll back. Recommendation: roll back, so what runs is what survives a reboot.

**D-4 Which register is the device identifier (M-10).** `MAP_VERSION` (0x0000) says which map layout; `device_class` (0x0001) says what kind of device. REQ-MB-120 wants identity. Recommendation: check **both**: the class must equal the profile's class and the map version must be supported by the table. That needs `device_class` in the profile (one byte).

**D-5 Re-probe (REQ-DM-101).** Option A: new `modbus_poller_reprobe(addr)` that clears the slave's link state so the next cycle repeats the identity check. Option B: no new API; an update marks the slot "unverified" through the registry's version counter, which the poller compares each cycle. Recommendation: **A**, explicit and testable.

**D-6 Default profile on first boot (M-6).** Recommendation: the registry seeds `{slave 1, class FD, map version 1, "Field Device 1 (default)"}` when the store has no profiles, and persists only after the first change. A factory reset returns to the default.

**D-7 Mismatch behaviour.** On identity mismatch the slave is marked `link_rejected`, never polled for data, and re-checked every N seconds (so replacing the device with the right one recovers). Recommendation: re-check at the existing link-retry interval; log once per transition, not every cycle (REQ-MB-130 without log flooding).

**D-8 One API.** Collapse the three read APIs into the LLD's singleton vtable `idevice_profile_provider_t` with `get_next`, `get_by_addr`, `is_registered`, and keep the manager vtable for add/update/remove. ModbusPoller and ConsoleService move to it.

## 5. Proposed session 2 scope (after your answers and Pass H)

1. LLD v0.2: fix M-1 to M-6, M-11 to M-17 and record D-1 to D-8.
2. Module: registry with mutex, validation, default seeding, persistence hook, host tests (the LLD's 17 cases plus the contradictions resolved).
3. ConfigService schema 2 and migration (if D-2 A); tests.
4. ModbusPoller: identity check, `link_rejected`, `reprobe`, map table (if D-1 A); tests.
5. ConsoleService: `profiles add/remove` become real (`addr=`, `class=`, `map=`, `desc=`); tests.
6. Harness: profile survives reboot, mismatch rejected, remove stops polling (the LLD's four on-target cases).

Dependencies: the Field Device already answers 0x0000 and 0x0001, so no Field Device change is needed. The whole change is Gateway-side; it touches four modules, so it will be split into separate PRs.

## 6. Not verified here

- That the Field Device returns a stable `device_class` value (read the register map LLD for the number before D-4).
- The exact ConfigService save path timing with a larger blob (CFG-O7 is 774 ms today).
- Whether `link_rejected` fits the existing `HEALTH_EVENT_MODBUS_LINK_*` events or needs a new one.

## 7. Decisions taken (2026-10-31)

| ID | Decision |
|---|---|
| D-1 | Version key plus a built-in table in ModbusPoller (span, decode) |
| D-2 | Profiles inside the ConfigService blob, `CONFIG_SCHEMA_VERSION` 2 with migration of schema 1 |
| D-3 | A failed persist rolls the in-memory change back and returns `ERR_PERSIST` |
| D-4 | Identity = device class and a supported map version; the profile gains a class byte |
| D-5 | New `modbus_poller_reprobe(addr)` |
| D-6 | Default profile seeded when the store has none; persisted only after the first change |
| D-7 | Mismatch gives `link_rejected`, re-checked at the link-retry interval, logged once per transition |
| D-8 | One singleton provider vtable (`get_next`, `get_by_addr`, `is_registered`) and one manager vtable |

Work order (separate PRs): LLD v0.2; the registry module; ConfigService schema 2; ModbusPoller (identity,
`link_rejected`, reprobe, map table); ConsoleService `profiles add/remove`; harness.
