# WP12 open items: analysis for Pass H

**Date:** 2026-10-31 · **Work package:** WP12 (open-item and documentation clean-up) · **Gate:** GATE-DESIGN
**Status:** Decided 2026-11-01: Luca accepted every recommendation D-1 to D-13 (section 7). Implemented in #148, #149, #150, #151 except D-11 and D-12 (see the status table at the end of section 7).

The items of WP12 that the plan marks "analysis, then stop for design" are examined below. Every
statement about the code was checked in the repository on `main` (7328305); where an answer depends
on PR #124 (the Gateway `main()`), it says so.

## 1. Summary

| Item | Verdict | Decisions needed |
|---|---|---|
| LC-O14 ConfigStore "never written" versus "corrupt" | Doable, small, needs a policy | D-1, D-2 |
| SS-O6 (alarm half) AlarmService from ConfigService | The LLD specifies the behaviour, not the API; two real conflicts | D-3 to D-6 |
| CFG-O7 | Already closed for readers; the remaining cost is CFG-O9 | D-7 (CFG-O9) |
| CFG-O8 consumers not wired | Boot feeding done for two; nobody but CloudPublisher follows later changes | follows D-3, D-8, D-13 |
| TS-O4 TimeService from ConfigService | Blocked by a schema gap (no NTP interval field) | D-8 |
| CP-O4 | Already closed (CP-D22), Luca may still veto | none |
| CS-O4, CS-O9 | **Done in this PR** (`components.md`) | none |
| CS-O5..O8, GCS-O4..O13 | Listed in section 6; none is "already specified" except what is done | D-9 to D-12 |

## 2. LC-O14: "never written" versus "corrupt"

**What the code does.** `cs_eval_slot()` in `config_store.c` reads each slot's 16-byte header. A
slot counts as invalid for three different reasons that are not told apart: the magic word is
wrong (an erased slot reads `0xFFFFFFFF`, but so does any garbage), the length is implausible, or the
CRC does not match (the normal result of an interrupted write). When neither slot is valid,
`cs_report_none_valid()` pushes `FAULT_CONFIG_NO_VALID_SLOT` and returns
`CONFIG_STORE_ERR_NO_VALID_SLOT`. `lifecycle_wiring.c` (`cs_check_integrity`) then either runs on the
defaults (LC-D16, the default) or faults when `fault_on_no_config` is set.

So with the default setting a corrupt store silently boots on the defaults, and with the strict
setting a first boot faults. IT-LC-003 ("corrupt configuration faults the boot") cannot be written.

**Option A (recommended).** Add `CONFIG_STORE_ERR_NEVER_WRITTEN`, returned when both slots have an
erased header (magic `0xFFFFFFFF` in both). `NO_VALID_SLOT` keeps its meaning "something is there and
it is damaged". `cs_eval_slot()` gets a fourth slot status (`CS_SLOT_ERASED`); the change is about 20
lines plus tests. `lifecycle_wiring` then always runs on the defaults for `NEVER_WRITTEN` and faults
for `NO_VALID_SLOT` (the `fault_on_no_config` switch disappears or flips its meaning).

**Option B.** Leave `NO_VALID_SLOT` as it is and add a query that reports the two slot states.
More flexible, but pushes the policy into every caller.

**Option C.** Leave it. IT-LC-003 stays unwritable.

**Points to decide**
- **D-1: which option.** Recommendation: A.
- **D-2: an interrupted first write.** The very first save writes slot A while slot B is still
  erased. Power lost during it leaves A with a valid magic and a bad CRC, so the store reads as
  "damaged", not "never written", and under A it would fault the boot of a device that never held a
  configuration. Is that acceptable (a factory-erase command recovers it), or should a store whose
  only non-erased slot has no valid CRC and `seq` 1 count as never written? Recommendation:
  acceptable, provided the console can run a factory erase from the faulted state (check this).
- The Field Device copy of ConfigStore has diverged from the Gateway one (CS-O5 in `config-store-lld.md`: different magic, error values and slot layout);
  say whether the Field Device gets the same change. Recommendation: Gateway first.

## 3. SS-O6, alarm half

**What is specified.** `fd-gw-sensor-alarm-service-lld.md` §4.3 says `alarm_service_init()` reads
thresholds and hysteresis from `IConfigProvider`, rejects a hysteresis that is not positive and less
than `(high - low) / 2`, and falls back to defaults. Test TC-AS-002 repeats the rule. The Gateway code
is `alarm_service_init(void)` with nine compile-time constants and no provider.

**What is not specified, or conflicts**
- **D-3: API shape.** The Gateway SensorService took the other route (SS-D20):
  `sensor_service_reconfigure(const config_params_t *)`, called by the boot code and by the
  change callback (`config_service_register_change_cb`). Recommendation: the same for AlarmService,
  `alarm_service_reconfigure(const config_params_t *)`, rather than an `init` that reads a provider.
- **D-4: two sets of defaults.** They differ.

  | | AlarmService constants | `config_params_t` defaults |
  |---|---|---|
  | Temperature high / low / hysteresis | 35.00 / 0.00 / 2.00 °C | 40.00 / 0.00 / 1.00 °C |
  | Humidity high / low / hysteresis | 80 / 20 / 5.00 %RH | 80 / 20 / 2.00 %RH |
  | Pressure high / low / hysteresis | 1050.0 / 950.0 / 5.0 hPa | 1050.0 / 950.0 / 5.0 hPa |

  Wiring ConfigService changes the behaviour of a default device (temperature alarm at 40 °C
  instead of 35 °C, two hysteresis values). Which set is canonical? Recommendation: the
  `config_params_t` set, if the SRS agrees; I could not find the alarm defaults in the SRS.
- **D-5: a change while an alarm is active.** The LLD says nothing about moving a threshold while
  the state is `ACTIVE_HIGH` or `ACTIVE_LOW`. Recommendation: keep the state; the new limits apply from
  the next sample, so the next sample may clear it or raise the other side. Alarm events then follow
  the existing rules.
- **D-6: the validation rule.** ConfigService already rejects a band where
  `high - low <= 2 * hysteresis` (CFG-D7) on `set_param`, and `apply_loaded` repairs or replaces by the
  defaults a stored configuration whose channels fail the same check. The
  AlarmService rule in the LLD ("less than (high - low) / 2") is the same test with a different boundary
  (`<` against `<=`) and would be a second copy. Recommendation: validate in ConfigService only and
  let AlarmService trust the snapshot, or copy the exact CFG-D7 inequality.

Cost once decided: about the size of the SensorService half (one function, tests TC-AS-001/002 plus the
change-callback path, a hardware check like TC-HW-SS-010).

## 4. CFG-O7, CFG-O8, TS-O4

**CFG-O7** is closed for readers (CFG-D15: flash is written outside the data mutex). What remains is
**CFG-O9**: ConfigStore erases and rewrites a whole 32 KB slot (8 sectors) per save because its CRC is
at the end of the slot, so a 448-byte blob costs about 0.76 s. A format change (CRC next to the data,
erase only the sectors in use) would cut it to about one sector erase. It changes validated middleware
and its on-flash format, so it needs its own decision.

- **D-7: do CFG-O9?** Recommendation: not now. `set_param_deferred` (20 µs) and `persist_pending` already
  keep the callers that cannot block off the slow path, and a format change needs a migration story for
  stored blobs.

**CFG-O8: where each consumer stands**

| Consumer | State |
|---|---|
| CloudPublisher | Done (CP-D31): publish intervals follow the change callback |
| SensorService | Fed at boot: `lifecycle_wiring.c` calls `sensor_service_reconfigure(config_service_get_params())` (SS-D20). Not re-applied on a later change: only CloudPublisher registers `config_service_register_change_cb` |
| AlarmService | Not wired at all: SS-O6 above |
| TimeService | Not wired at all: TS-O4 below |
| ModbusPoller | Fed at boot: `lifecycle_wiring.c` passes `modbus_poll_period_ms` to `modbus_poller_init`. Not re-applied on a later change (the period is an init-time parameter) |

**TS-O4: the schema has no NTP interval.** `config_params_t` carries `ntp_servers[4][64]` and
`ntp_server_count` (default 0) but **no sync interval**. `time_service.c` has the two seams the item
names: `s_ntp_servers` (two IPv4 literals) and `s_ts.interval_s` (3600 s). The servers can be wired
without a schema change (use the configured ones when `ntp_server_count > 0`, else the defaults); the
interval cannot. Adding a field changes the stored blob layout and must bump `CONFIG_SCHEMA_VERSION`
(the header says so).

- **D-8: add `ntp_interval_s` to `config_params_t`?** Recommendation: yes, together with the first
  schema change that is needed anyway, so the version is bumped once. Also say whether the field in
  `ntp_servers` may hold a host name now that `wifi_resolve()` exists (the header comment says IPv4
  literals; NtpClient takes literals today).

## 5. CP-O4

Already **closed 2026-10-28** in `cloud-publisher-lld.md` (CP-D22: an invalid channel and an offline Field
Device are JSON `null`; schema 1.1). The plan says it stays open until the cloud schema is decided, which has
happened. Luca may still veto before the cloud consumer exists. Nothing to do.

## 6. Console items (CS-O4..O9, GCS-O4..O13)

**Done in this PR.** CS-O4 and CS-O9: the Gateway `ConsoleService` entry in `components.md` now lists
`IAlarmService`, `IConfigManager`, `IDeviceProfileProvider` and `IDeviceProfileManager` instead of the
concrete `DeviceProfileRegistry`, exactly as `gw-console-service-lld.md` prescribes. CS-O7 is closed for
the Gateway already (data-tick counter); it remains to confirm for the Field Device.

| Item | What it needs | Specified? | Next step |
|---|---|---|---|
| CS-O5 / GCS-O6 | Self-test depth: a flash round-trip and a persisted result need a ConfigService / ConfigStore hook | No | **D-9** whether the current depth is acceptable for REQ-LI-130 |
| CS-O6 | `prov cert-begin` / `prov cert-end` paste mode for certificates | UX named, not designed | Design with GCS-O4 (provisioning) |
| CS-O8 | `profiles add` format: key=value or JSON | Recommendation exists (key=value), "confirm before implementing" | **D-10** confirm key=value |
| GCS-O4 | WiFi credentials (`prov set wifi-ssid / wifi-pass`) have no store: the Gateway reads them from a git-ignored header | No (needs storage, encryption, WifiTask hand-off) | Own design pass |
| GCS-O5 | Modbus line settings and profile add/remove need DeviceProfileRegistry (read-only today) | Depends on WP8 | After WP8 |
| GCS-O8 | Link producers (RSSI polling, connect events, Modbus link events) not wired in a production image | Needs `main()` | Re-check after PR #124 merges: CloudPublisher pushes the connect events (WP1); RSSI polling by WifiTask and the Modbus link events were not verified |
| GCS-O9 | Network panel cannot show the SSID or IP: the health snapshot has `wifi_rssi_dbm` only and `WifiTask` has `get_rssi` but no SSID or address accessor | No | **D-11** health snapshot field or a WifiTask accessor |
| GCS-O10 | Nothing creates `ConsoleTask` in a production image | Needs `main()` | Closed by PR #124 once merged (`GATEWAY_WITH_CONSOLE`) |
| GCS-O11 | Logger and the console both write USART1: log lines interleave with the TUI | No | **D-12** route log lines into the scrollback, or give the Logger another sink; #124 currently needs `-DGATEWAY_WITH_CONSOLE=0` for a plain log |
| GCS-O12 | UART DMA receive so a high baud rate survives the Wi-Fi/TLS start | No | Own item once the task set is final (#124 fixes it) |
| GCS-O13 | "Check for update" has no back-end | Needs an update-source module | After the OTA work (WP5) |

## 7. Decisions, in one place

| ID | Question | Decision (Luca, 2026-11-01) |
|---|---|---|
| D-1 | ConfigStore: how to tell "never written"? | Option A, a new error code (accepted) |
| D-2 | Is a first write interrupted by power loss a fault? | Yes, with a factory-erase recovery (accepted) |
| D-3 | AlarmService API shape | `alarm_service_reconfigure(const config_params_t *)`, like SS-D20 (accepted) |
| D-4 | Which alarm defaults are canonical | The `config_params_t` set; check the SRS defaults against it first (accepted) |
| D-5 | Threshold change while an alarm is active | Keep the state, apply from the next sample (accepted) |
| D-6 | Where the hysteresis rule lives | In ConfigService (CFG-D7) only (accepted) |
| D-7 | CFG-O9 ConfigStore format change | Not now (accepted) |
| D-8 | Add `ntp_interval_s` to the schema; host names in `ntp_servers` | Yes, one schema bump (accepted) |
| D-9 | Self-test depth | Accept the current depth for now (accepted) |
| D-10 | `profiles add` format | key=value (accepted) |
| D-11 | SSID / IP in the console network panel | Add them to the health snapshot (accepted) |
| D-12 | Logger and console sharing USART1 | Route log lines into the console scrollback (accepted) |
| D-13 | Should SensorService and ModbusPoller follow *later* configuration changes (change callback), or only the boot values? | Follow changes: one `register_change_cb` user in `lifecycle_wiring` for both (accepted) |

**Implementation status (2026-11-01).**

| Decision | State |
|---|---|
| D-1, D-2 | PR #148. Deviation: a damaged store still follows `fault_on_no_config` (default: run on the defaults), because the console is not up at the integrity step, so a boot fault could not be cleared by a console factory erase |
| D-3 to D-6 | PR #150. A default device now alarms at 40 degC (was 35) |
| D-7 | Deferred as decided |
| D-8 | PR #149. Deviation: `ntp_interval_min` uses the former tail padding, so no schema bump (a bump with an unchanged layout would discard every stored configuration) |
| D-9 | No work |
| D-10 | Already true: `profiles add` is key=value since WP8 |
| D-11 | **Not done.** WifiDriver has no accessor for the local address or SSID; the ISM43362 status command output has to be read and parsed on the module, which needs the board |
| D-12 | **Not done.** Needs a Logger sink and a queue into the console task; the Release image has 596 B of SRAM2 and 748 B of the main bank left (GM-O25), so it needs a RAM decision first |
| D-13 | PR #151 (contains #149 and #150) |

Before the status above, this section said: no code was changed for any of D-1 to D-13. Suggested implementation groups: ConfigStore (D-1, D-2); ConfigService schema (D-8; D-7 deferred); AlarmService (D-3 to D-6); wiring (D-13); console and health snapshot (D-10 to D-12). D-9 needs no work.
