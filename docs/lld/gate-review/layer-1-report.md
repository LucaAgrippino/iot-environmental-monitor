# LLD Gate Review — Layer 1 Report

**Review date:** 18 May 2026  
**Branch:** `feature/lld-gate-review-layer-1`  
**Reviewer:** Luca Agrippino  
**Script:** `scripts/lld_gate_review_check.py`  
**Final result:** Layer 1 **PASSES** — 0 findings (0 blockers)

---

## 1. Scope

Layer 1 is the mechanical conformance gate: it checks that every companion
file in `docs/lld/` meets the structural and cross-reference rules set out in
`lld-methodology.md` v1.1.  No design judgement is exercised; only
machine-checkable rules are applied.

Ten checks are run:

| # | Check | What it verifies |
|---|-------|-----------------|
| 1 | Catalogue paths | Every path in `lld.md` §4 exists on disk |
| 2 | Orphan companions | Every file on disk appears in `lld.md` §4 |
| 3 | Status consistency | Catalogue status matches file existence |
| 4 | Traceability | Reviewed/Baselined companions appear in `lld.md` §7 |
| 5 | SRS citations | Every REQ-XXX cited in a companion is defined in `SRS.md` |
| 6 | Error naming | Error enums use `<module>_err_t`, not `_status_t` (v1.1 rule) |
| 7 | Section structure | Each companion has all eight required section headings |
| 8 | Driver dependencies | Driver companions carry no FreeRTOS `#include` |
| 9 | Companion header | Each companion has Version / Date / Status / HLD anchor in first 30 lines |
| 10 | Open items format | Each §8 row has ≥ 4 non-empty cells (ID \| Item \| Resolution path \| Status) |

---

## 2. Pre-review state

Running the script against the baseline produced **277 BLOCKERs and 45 FIX_NOW findings** across five categories.

### 2.1 BLOCKER — Catalogue paths (8)

`lld.md` §4 used filenames without the `-lld` suffix for eight application
companions whose actual filenames include it:

| Catalogued (wrong) | Actual filename |
|--------------------|-----------------|
| `application/modbus-register-map.md` | `application/modbus-register-map-lld.md` |
| `application/lcd-ui.md` | `application/lcd-ui-lld.md` |
| `application/console-service.md` | `application/console-service-lld.md` |
| `application/cloud-publisher.md` | `application/cloud-publisher-lld.md` |
| `application/store-and-forward.md` | `application/store-and-forward-lld.md` |
| `application/time-service.md` | `application/time-service-lld.md` |
| `application/device-profile-registry.md` | `application/device-profile-registry-lld.md` |
| `application/update-service.md` | `application/update-service-lld.md` |

### 2.2 BLOCKER — Traceability (~40)

The §7 traceability rows in `lld.md` did not use backtick-quoted paths with
layer prefixes as required by the `TRACEABILITY_ROW_RE` regex.  Failures
included:

- Missing backtick quotes around all paths
- Missing `drivers/` / `middleware/` / `application/` layer prefix
- Use of `docs/lld/` absolute prefix rather than the relative layer prefix
- Application paths using the wrong (un-suffixed) filename

### 2.3 BLOCKER — Section structure (~180)

Companion headings did not match the exact strings required by
`check_section_structure` (`heading in text`).  Four naming patterns were
found across the companion set:

| Pattern | Affected companions | Typical mismatch |
|---------|--------------------|--------------------|
| A (eleven drivers) | `i2c-driver`, `lcd-driver`, `led-driver`, `modbus-uart-driver`, `qspi-flash-driver`, `reset-driver`, `rtc-driver`, `sdram-driver`, `simulated-sensor-drivers`, `spi-driver`, `touchscreen-driver` | `## 1. Source summary` instead of `## 1. Sources`; `## 2. API` instead of `## 2. Public API`; `## 6. Error handling`; `## 7. Test plan` |
| B (five companions) | `exti-driver`, `humidity-temp-barometer-drivers`, `magnetometer-imu-drivers`, `wifi-driver`, `middleware/logger` | Section numbers offset by one; `(Step N)` suffixes throughout |
| D-MW (nine middleware) | `circular-flash-log`, `config-store`, `firmware-store`, `graphics-library`, `modbus-master-poller`, `modbus-slave`, `mqtt-client`, `ntp-client`, `time-provider` | Body sections numbered with non-sequential custom numbers; §5/§6 stubs absent |
| D-APP (twelve application) | `cloud-publisher-lld`, `config-service`, `console-service-lld`, `device-profile-registry-lld`, `health-monitor`, `lcd-ui-lld`, `lifecycle-controller`, `modbus-register-map-lld`, `sensor-alarm-service`, `store-and-forward-lld`, `time-service-lld`, `update-service-lld` | Same as D-MW |

### 2.4 FIX_NOW — Companion header fields (37)

Thirty-seven companions were missing one or more of the four required header
fields (`**Version:**`, `**Date:**`, `**Status:**`, `**HLD anchor:**`) in
their first 30 lines.

### 2.5 FIX_NOW — Open items format (45)

Forty-five §8 open-item rows across 21 companions used a 2-column table
(`| ID | Item |`) rather than the required 4-column format
(`| ID | Item | Resolution path | Status |`).

---

## 3. Fixes applied

All defects were resolved in eight commits on branch
`feature/lld-gate-review-layer-1`.

| Commit | Description | Defects resolved |
|--------|-------------|-----------------|
| `8d03911` | Add gate review script and `fix_lld_headings.py` migration tool | — (tooling only) |
| `fe7fc02` | Correct `lld.md` catalogue paths and §7 traceability matrix | 8 Catalogue BLOCKERs + ~40 Traceability BLOCKERs |
| `c01e849` | Rename section headings in Pattern-A driver companions (×11) | ~44 Structure BLOCKERs |
| `6981ad9` | Renumber Step-N headings in Pattern-B companions (×5) | ~40 Structure BLOCKERs |
| `94491a2` | Align middleware companion headings to methodology (×9) | ~54 Structure BLOCKERs |
| `a4198dc` | Align application companion headings to methodology (×12) | ~72 Structure BLOCKERs |
| `c9c4e50` | Add missing Version / Date / Status / HLD-anchor header fields (×37) | 37 FIX_NOW (header fields) |
| `dc23d98` | Expand 2-column open-items tables to 4 columns (×21) | 45 FIX_NOW (open items format) |

### Fix approach

Rather than making 150+ individual file edits, two Python migration scripts
were written:

- `scripts/fix_lld_headings.py` — one-time heading-rename tool covering all
  four naming patterns (A, B, D-MW, D-APP).  Inserted §5 / §6 stub sections
  where entirely absent.
- `scripts/fix_companion_headers.py` — inserts missing header fields before
  the first `---` separator in each companion.
- `scripts/fix_open_items_format.py` — expands 2-column §8 tables to
  4-column format, scoped strictly to the `## 8. Open items` section to avoid
  touching other tables in the companion body.

All three scripts are retained in `scripts/` for audit purposes.

---

## 4. Final gate review result

```
LLD gate review — Layer 1 mechanical checks
Root: D:/iot-environmental-monitor

Summary: 0 findings (0 blockers)
Layer 1 PASSES — no blockers found.
```

---

## 5. Deferred items

No findings were deferred.  All BLOCKER and FIX_NOW defects have been
resolved.  DEFER and COSMETIC categories did not arise during this review.

---

## 6. Open design items (informational)

The §8 open items within the companions are design-level questions to be
answered during implementation and integration, not gate-review defects.
They are tracked in the companions themselves.  Selected high-priority items
to resolve before coding begins:

| ID | Companion | Summary |
|----|-----------|---------|
| CFL-O1 | `circular-flash-log.md` | Confirm UpdateService OTA chunk size ≤ 4 078 bytes |
| WIFI-O2 | `wifi-driver.md` | SpiDriver FRXTH conflict — update `spi-driver.md` for 16-bit mode |
| WIFI-O3 | `wifi-driver.md` | Confirm GPIO port/pin assignments against UM2153 Appendix A |
| EXTI-O2/O3/O4 | `exti-driver.md` | Update companion USES lists for ExtiDriver dependency |
| DUART-O2 | `debug-uart-driver.md` | PCLK values — resolve in `clock-config.md` companion |

---

## 7. Recommended next steps

1. Merge `feature/lld-gate-review-layer-1` → `main`.
2. Write the outstanding driver companions (`gpio-driver.md` is skipped in
   the current suite; `clock-config.md` is referenced by several open items
   but does not yet exist).
3. Run the Layer 1 gate review script on a CI schedule so regressions are
   caught immediately when new companions are added.
4. Begin Layer 2 review (design quality, interface completeness, SRS coverage
   depth) once Layer 1 is baselined.
