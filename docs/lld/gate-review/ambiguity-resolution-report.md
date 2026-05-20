# LLD Layer 2 — Ambiguity Resolution Report

**Branch:** `feature/lld-ambiguity-resolution`  
**Date:** 2026-05-20  
**Author:** Luca Agrippino  
**Reviewer:** Claude Sonnet 4.6  
**Predecessor pass:** Pass A remediation (`feature/lld-gate-review-layer-2-pass-a-remediation`, 2026-05-20)  
**Gate-review script:** `scripts/lld_gate_review_check.py`  
**Result:** 0 BLOCKERs

---

## 1. Purpose

The Pass A remediation report (§7) identified seven open architectural
ambiguities that were not blockers at the time but required resolution before
implementation could begin safely:

> "Seven ambiguities remain unresolved. Each has a designated resolution path.
> No ambiguity blocks the Pass A verdict; all are scheduled for the
> ambiguity-resolution sprint that immediately follows this branch."

This document records how each ambiguity was resolved, which LLD companion
files were modified, and which architectural decisions were adopted.

---

## 2. Ambiguities resolved

### 2.1 NTP transport — UDP via polymorphic WifiDriver API (LLD-D13)

**Prior state:** `NtpClient` §6 referenced non-existent functions
(`wifi_driver_udp_open`, `wifi_driver_udp_send`, etc.). WifiDriver had no UDP
socket support; only a TCP-connect API existed. The transport was undefined.

**Resolution:** WifiDriver gains a polymorphic `open_socket(type, addr, port, handle)`
function where `wifi_socket_type_t` selects `WIFI_SOCKET_TCP` or
`WIFI_SOCKET_UDP`. The ISM43362 AT command `AT+P1=0/1` selects the protocol
before `AT+NCPX`. NtpClient uses `wifi_driver->open_socket(WIFI_SOCKET_UDP, …)`
throughout its per-server loop.

**Files modified:**
- `docs/lld/drivers/wifi-driver.md` — added `wifi_socket_type_t`, `wifi_open_socket()`, updated `iwifi_t` vtable, documented AT command paths, added WIFI-D7 decision.
- `docs/lld/middleware/ntp-client.md` — updated §6 query execution to use `wifi_driver->open_socket(WIFI_SOCKET_UDP, …)`, closed NTP-O2 (DNS), NTP-O3 (IPv6), NTP-OX (transport).
- `docs/hld/components.md` — updated WifiDriver RESPONSIBILITY to include UDP.

**Contract graph edges affected:** 3 (TimeProvider→RtcDriver unchanged; NtpClient→WifiDriver #73, MqttClient→WifiDriver #66, TimeService→NtpClient #101).

---

### 2.2 AlarmService acknowledgement model — non-latched with manual clear (LLD-D14)

**Prior state:** The alarm acknowledgement semantic (latched vs. non-latched, bulk
vs. per-alarm) was not specified. ModbusRegisterMap §8.1 called
`alarms->ack_all(alarms)` using the ctx-based pattern; `ack_all()` was not in
any AlarmService interface definition.

**Resolution:** AlarmService exposes `alarm_service->ack_all(void)` on its
`ialarm_service_t` singleton vtable. The model is non-latched-with-manual-clear:
a bulk ack clears all active alarm flags immediately, but flags re-raise on the
next evaluation cycle if the triggering condition persists. This makes persistent
faults visible after operator acknowledgement.

**Files modified:**
- `docs/lld/application/sensor-alarm-service.md` — added `alarm_service_ack_all()`, `ialarm_service_t` vtable, LLD-D14 semantic documentation.
- `docs/lld/application/modbus-register-map-lld.md` — updated CMD_ACK_ALARM handler to `alarm_service->ack_all()` (singleton vtable, LLD-D14 note).

**Contract graph edges affected:** 1 (ModbusRegisterMap→AlarmService #51).

---

### 2.3 CMD_RESET_METRICS dispatch — LifecycleController as single point (LLD-D15)

**Prior state:** ModbusRegisterMap §8.2 called
`health_write->reset_metrics(health_write)` directly (ctx-based, non-vtable).
This created a direct cross-layer dependency from ModbusRegisterMap (Application)
to HealthMonitor (Application) for a state-affecting command, bypassing the
established LLD-D12 pattern of routing remote commands through LifecycleController.
`health_monitor_reset_metrics()` also did not exist in the HealthMonitor companion.

**Resolution:** LifecycleController becomes the single dispatch point for all
remote commands that affect system state. `ILifecycle` gains
`handle_remote_command(lifecycle_remote_cmd_t cmd)`. CMD_RESET_METRICS dispatches
to `health_admin->reset_metrics()` (IHealthAdmin). CMD_SOFT_RESTART dispatches via
`handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART)`, which posts
`LC_EVENT_RESTART_REQUESTED` to LifecycleTask. CMD_ACK_ALARM is exempt — it
dispatches directly to AlarmService because it has no lifecycle-state dependency.

**Files modified:**
- `docs/lld/application/health-monitor.md` — added `health_monitor_reset_metrics()`, `ihealth_admin_t` vtable (IHealthAdmin), three-way ISP split documented.
- `docs/lld/application/lifecycle-controller.md` — added `lifecycle_remote_cmd_t`, `lifecycle_handle_remote_command()`, `ilifecycle_t` vtable; §3 dispatch rationale; updated USES headers (IHealthAdmin on GW).
- `docs/lld/application/modbus-register-map-lld.md` — CMD_RESET_METRICS and CMD_SOFT_RESTART updated to use `lifecycle_controller->handle_remote_command(…)`.
- `docs/hld/components.md` — added IHealthAdmin to HealthMonitor PROVIDES (FD + GW), added HealthMonitor to LifecycleController USES (FD + GW).
- `docs/hld/modbus-register-map.md` — CMD_RESET_METRICS Sink column changed from `HealthMonitor` to `LifecycleController`.

**Contract graph edges affected:** 1 (ModbusRegisterMap→HealthMonitor #53).  
**New edge introduced:** LifecycleController→HealthMonitor (both boards, edge #123 in verification table — not in original graph, must be added before next gate review).

---

### 2.4 TimeProvider sync-flag persistence — RTC backup register allocation (LLD-D16)

**Prior state:** TimeProvider §5 referenced `TIME_PROVIDER_BKUP_REG = 3U` and
magic `0xA5A5A5A5UL` with the note "index TBD, see TP-O2". No backup register
allocation table existed; index 3 risked collision with the reset-cause flag.
Direct register access (`RTC_BKP3R`) bypassed the vtable seam (LLD-D10 violation).

**Resolution:**
- Backup register index 0 (BKP0R) is allocated to TimeProvider in the new
  project-wide allocation table (`lld.md` §6.2).
- Magic changed to `0xA5A55A5AUL` (asymmetric — cannot be a zero-fill, cannot
  collide with a symmetric value such as the old magic).
- Access via `rtc_driver->read_backup(0, &val)` / `write_backup(0, val)`.
- RtcDriver companion gains `rtc_read_backup()` / `rtc_write_backup()` and an
  updated `irtc_t` vtable.
- Persistence scope documented: survives warm resets; does not survive power-off
  on Discovery boards where VBAT=VDD (correct behaviour — RTC calendar also resets).

**Files modified:**
- `docs/lld/middleware/time-provider.md` — §5 updated; `itime_provider_t` vtable added; TP-O2 closed.
- `docs/lld/drivers/rtc-driver.md` — `rtc_read_backup()` / `rtc_write_backup()` added; `irtc_t` vtable updated; §4.4 cross-target register table corrected; §4.5 persistence scope note added; RTCD-D8 decision added; six new unit test cases.
- `docs/lld/lld.md` — §6.2 backup register allocation table created.

**Contract graph edges affected:** 1 (TimeProvider→RtcDriver #3).  
**Open items closed:** TP-O2.

---

### 2.5 DeviceProfileRegistry storage model (LLD-D17)

**Prior state:** DeviceProfileRegistry §6 documented the algorithms for
`add_or_update` / `remove` / `get_allowlist` but did not specify how the initial
profile set is provided on a factory-blank gateway. A gateway with no provisioned
profiles would poll no slaves and appear non-functional, requiring a mandatory
provisioning step before first use.

**Resolution:** The initial implementation embeds a `static const device_profile_t
s_default_profiles[]` table in firmware flash. On first boot (empty ConfigStore),
`dpr_load_from_store()` seeds the runtime registry from this static array rather
than failing silently. The runtime `add_or_update` / `remove` operations are
unaffected. `IDeviceProfileProvider` and `IDeviceProfileManager` are the stable
interface seam for future migration to a fully dynamic or cloud-sourced boot load.
`idevice_profile_provider_t` / `idevice_profile_manager_t` singleton vtables added
per LLD-D10.

**Files modified:**
- `docs/lld/application/device-profile-registry-lld.md` — §6.0 storage model section added; singleton vtables added.

**Contract graph edges affected:** 0 (storage model change; no new inter-component dependency).

---

## 3. Decisions summary

| ID | Short title | Settled in |
|----|-------------|-----------|
| LLD-D13 | NtpClient uses UDP via `wifi_driver->open_socket(WIFI_SOCKET_UDP, …)` | `wifi-driver.md` WIFI-D7; `ntp-client.md` §6 |
| LLD-D14 | AlarmService non-latched-with-manual-clear `ack_all()` | `sensor-alarm-service.md` §6; `modbus-register-map-lld.md` §8.1 |
| LLD-D15 | LifecycleController single dispatch for remote state-changing commands | `lifecycle-controller.md` §2, §3; `lld.md` §6 |
| LLD-D16 | TimeProvider sync-flag via BKP0R; asymmetric magic; vtable access | `time-provider.md` §5; `rtc-driver.md` §2.4; `lld.md` §6.2 |
| LLD-D17 | DeviceProfileRegistry static-embedded default profiles; interface seam | `device-profile-registry-lld.md` §6.0 |

All five decisions are recorded in `lld.md` §6 decisions log.

---

## 4. Files changed in this pass

| File | Change type | Decisions |
|------|-------------|-----------|
| `docs/lld/drivers/wifi-driver.md` | Modified | D13 |
| `docs/lld/middleware/ntp-client.md` | Modified | D13 |
| `docs/hld/components.md` | Modified | D13, D15 |
| `docs/lld/application/sensor-alarm-service.md` | Modified | D14 |
| `docs/lld/application/modbus-register-map-lld.md` | Modified | D14, D15 |
| `docs/lld/application/health-monitor.md` | Modified | D15 |
| `docs/lld/application/lifecycle-controller.md` | Modified | D15 |
| `docs/hld/modbus-register-map.md` | Modified | D15 |
| `docs/lld/middleware/time-provider.md` | Modified | D16 |
| `docs/lld/drivers/rtc-driver.md` | Modified | D16 |
| `docs/lld/lld.md` | Modified | D16 (§6.2), D13–D17 (§6 log) |
| `docs/lld/application/device-profile-registry-lld.md` | Modified | D17 |
| `docs/lld/gate-review/edge-verification-table.md` | Created | — (audit artefact) |
| `docs/lld/gate-review/ambiguity-resolution-report.md` | Created | — (this document) |

---

## 5. Contract graph delta

The original contract graph (`layer-2-contract-graph.json`) has 122 edges.
This pass introduced one new architectural dependency:

| Edge | Board | Introduced by | Action required |
|------|-------|---------------|-----------------|
| LifecycleController → HealthMonitor | both | LLD-D15 | Add to `layer-2-contract-graph.json` before next gate review |

All 122 original edges verified PASS in `edge-verification-table.md`.

---

## 6. Open items remaining after this pass

| ID | Item | Owner |
|----|------|-------|
| TP-O1 | `TIME_PROVIDER_SYNC_INTERVAL_S` — TBD, driven by REQ-NF-210/211. | Luca — at integration |
| TP-O3 | `TIME_PROVIDER_SANITY_DELTA_S` — NTP delta sanity threshold. | TimeService LLD companion |
| NTP-O1 | `NTP_CLIENT_QUERY_TIMEOUT_MS = 3000` provisional; validate at integration. | Luca — at integration |
| LC-O1 | Config snapshot buffer size — BSS budget verification. | Luca — at integration |
| LC-O2 | Init timeout budget (REQ-NF-213 TBD). | Luca — at coding |
| LC-O3 | Start-gate event group bit — project-wide event-group map not yet produced. | Luca — at implementation |
| LC-O4 | Restart confirmation timeout (REQ-DM-020 TBD). | Luca — at coding |
| RTCD-O1 | Default epoch value on backup domain reset. | Luca — at implementation |
| RTCD-O2 | RSF wait timeout under minimum APB clock. | Clock-config companion |
| DPR-O1 | `expected_map_version` → polling descriptor binding confirmation. | ModbusPoller LLD |
| DPR-O2 | Deserialisation schema versioning. | Luca — at first implementation |
| COMP-DIAG-01 | VP diagram does not show DeviceProfileRegistry. | Luca — before v1.0 |
| — | Add LifecycleController→HealthMonitor edge to layer-2-contract-graph.json. | Next gate prep |

None of the remaining open items block implementation of any component in the current sprint.

---

*Report produced during the LLD ambiguity-resolution sprint, 2026-05-20. Commits on branch `feature/lld-ambiguity-resolution`.*
