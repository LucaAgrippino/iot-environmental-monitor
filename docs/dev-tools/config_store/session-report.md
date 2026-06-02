# Session Report — ConfigStore

**Date:** 2026-06-02
**Branch:** `feature/phase-4-sensor-alarm-service`
**Companion:** `docs/lld/middleware/config-store.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/middleware/config_store/config_store_config.h` | 19 | board-specific partition constants |
| `firmware/field-device/middleware/config_store/config_store_crc.h` | 48 | streaming + one-shot CRC32/ISO-HDLC API |
| `firmware/field-device/middleware/config_store/config_store_crc.c` | 109 | 1 KB table, start/feed/finish/one-shot |
| `firmware/field-device/middleware/config_store/config_store.h` | 212 | public API, vtable, TEST/UNIT_TEST hooks |
| `firmware/field-device/middleware/config_store/config_store.c` | ~680 | full implementation |
| `firmware/field-device/integration-tests/config_store/test_config_store_main.c` | 193 | on-board integration test |
| `tests/field-device/middleware/config_store/test_config_store.c` | 434 | 13 Unity test cases |
| `tests/project.yml` | +3 lines | `:test_config_store:` defines block added |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-CS-001 | Fresh flash → load returns ERR_NO_VALID_SLOT + health event | PASS |
| TC-CS-002 | Save + load round trip → data matches | PASS |
| TC-CS-003 | Save twice → load returns second blob (higher seq_number wins) | PASS |
| TC-CS-004 | Corrupt lower-seq slot CRC → load selects intact higher-seq slot | PASS |
| TC-CS-005 | Corrupt both slot CRCs → ERR_NO_VALID_SLOT + health event | PASS |
| TC-CS-006 | Erase failure → ERR_FLASH_ERASE; active slot still loadable | PASS |
| TC-CS-007 | Write failure at CRC commit → ERR_FLASH_WRITE; active slot unchanged | PASS |
| TC-CS-008 | config_store_erase() → subsequent load returns ERR_NO_VALID_SLOT | PASS |
| TC-CS-009 | data_len > max_len on load → ERR_TOO_LARGE | PASS |
| TC-CS-010 | Not-init guards on all four functions | PASS |
| TC-CS-011 | NULL arg guards (init, load, save) | PASS |
| TC-CS-012 | check_integrity on fresh flash → ERR_NO_VALID_SLOT + health event | PASS |
| TC-CS-013 | check_integrity after one valid save → OK, no event | PASS |

**Total:** 13 pass, 0 ignored.

**Note on slot selection order (informs TC-CS-003 and TC-CS-004):**
When neither slot is valid (first boot), the companion says active=A, target=B.
Therefore save #1 always goes to slot B (seq=1), save #2 goes to slot A (seq=2).
TC-CS-003 and TC-CS-004 account for this: after two saves, slot A holds the
second blob (higher seq, selected on load) and slot B holds the first blob.

---

## Integration test — expected behaviour

Flash `firmware/field-device/integration-tests/config_store/test_config_store_main.c`
to the STM32F469I-DISCO. Open PuTTY at 115200/8N1 on the ST-Link VCP.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `[INFO][CSTest] ===== ConfigStore integration test =====` | logger + init path |
| 2 | `[INFO][ConfigStore] init OK` | QSPI accessible, mutex created |
| 3 | `[INFO][CSTest]` reports `check_integrity: ERR_NO_VALID_SLOT` | both slots blank |
| 4 | `[INFO][ConfigStore] saved 32 bytes to slot B (seq=1)` | first save → slot B |
| 5 | `[INFO][ConfigStore] integrity OK` | one valid slot found |
| 6 | `[INFO][CSTest]` reports `load 1: OK` + data matches | load path |
| 7 | `[INFO][ConfigStore] saved 32 bytes to slot A (seq=2)` | second save → slot A |
| 8 | `[INFO][CSTest]` reports `load 2: slot B selected` WRONG — actually A selected | slot A (seq=2) wins |
| 9 | `[INFO][ConfigStore] factory erase complete` | all 16 sectors erased |
| 10 | `[INFO][CSTest]` reports `load after erase: ERR_NO_VALID_SLOT` | erase effective |
| 11 | `[INFO][CSTest] === ALL CHECKS PASSED ===` | all assertions pass |
| 12 | Green LED (LD1, PG13) lit continuously | visual PASS indicator |
| 13 | Red LED (LD4, PD4) off | no assertion failure |

**Hardware bug observable after repeated saves** (see bug-log.md):
After the green LED lights, press RESET. On the second run, the test will save
to slot A (seq=3) and slot B (seq=4) etc. Eventually (when the same slot is
reused for the second time), the load on the following run will fail because the
CRC field was not erased before the new CRC was written. Symptom: load returns
ERR_NO_VALID_SLOT unexpectedly. Set a JTAG breakpoint in cs_validate_slot and
inspect the raw bytes of the affected slot.

---

## Deviations from companion

1. **Companion §7 UNIT_TEST macro hint**: The companion sketches `#define qspi_flash_erase(addr, len) stub_flash_erase(...)` etc. inside `config_store.c`. The implementation instead uses internal static wrapper functions (`cs_flash_erase_range`, `cs_flash_write_bytes`, `cs_flash_read_bytes`) and a `#ifdef UNIT_TEST` block that defines them as macros. Effect is identical; naming avoids collision with the real driver function `qspi_flash_read`. Reason: cleaner namespace, no risk of redeclaring a static function with the same name as an extern function.

2. **Streaming CRC API**: The companion mentions only `config_store_crc32(buf, len)`. The implementation also adds `config_store_crc32_start`, `_feed`, `_finish` to support incremental computation over 32 KB slot content without a large stack allocation. This is additive; `config_store_crc32()` still exists as specified.

3. **CS-O3 (seq_number overflow)**: Documented in `config_store_crc.c` file comment as specified.

---

## Open items

| ID | Status | Notes |
|----|--------|-------|
| CS-O1 | Open | `CONFIG_STORE_MAX_DATA_BYTES` (32 712) must be verified against the actual serialised config struct when ConfigService LLD is produced. |
| CS-O2 | Resolved | QspiFlashDriver LLD confirms 4 KB sector erase and byte-granular write. QSPI base-address offset conversion handled internally in `cs_flash_erase_range` / `cs_flash_write_bytes` / `cs_flash_read_bytes`. |
| CS-O3 | Closed | seq_number overflow bound documented in config_store_crc.c file comment. |

---

## Commit messages

### Commit 1
```
feat: add ConfigStore middleware — A/B slot NOR flash persistence with CRC32

Implements IConfigStore: config_store_init, _load, _save, _check_integrity,
_erase. Power-loss-safe A/B slot rotation per flash-partition-layout.md §6.1
(D39). CRC32/ISO-HDLC integrity protection; internal priority-inheritance
mutex serialises concurrent callers. UNIT_TEST RAM-backed simulation with
fault injection for host-side testing. Intentional hardware bug in
config_store_save erase range (7 of 8 sectors erased) for integration debug
exercise. Companion: docs/lld/middleware/config-store.md.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
```

### Commit 2
```
test: add ConfigStore unit tests TC-CS-001..013 and project.yml entry

13 Unity test cases covering: fresh-flash no-valid-slot, save/load roundtrip,
dual-save slot selection, single-slot CRC corruption, dual-slot corruption
with health event, erase and write fault injection, factory erase, too-large
guard, not-init guards, null-arg guards, check_integrity (empty and valid).
All 13 pass. UNIT_TEST RAM sim with countdown fault injection in config_store.c.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
```

---

## PR description

Title: `feat: ConfigStore middleware — A/B slot NOR flash persistence`

Body:
```markdown
## Summary
- Adds `IConfigStore`: power-loss-safe A/B slot write, CRC32/ISO-HDLC integrity,
  internal mutex, health-event reporting.
- CRC32 implementation in a standalone `config_store_crc.c` with streaming API.
- UNIT_TEST RAM simulation with countdown fault injection enables deterministic
  host-side testing without flash hardware.

## What is in this PR
| Commit | Files | Description |
|--------|-------|-------------|
| `feat:` | `firmware/.../config_store/` | 5 source files (config, crc, main) |
| `test:` | `tests/.../test_config_store.c`, `project.yml` | 13 unit tests |

## Architecture decisions
- **Separate CRC module** (`config_store_crc.c`): table-driven, streaming API,
  isolated and independently testable.
- **UNIT_TEST macro bridge**: flash ops are macro-replaced in test builds;
  no stub header needed, no driver cascade into the test link.
- **Slot selection**: when both slots are invalid, target = slot B (active = A)
  per companion §6. First boot writes to slot B (seq=1), second to slot A (seq=2).

## Test evidence
```
TESTED:  13  PASSED:  13  FAILED:  0  IGNORED:  0
```

## Open items
- CS-O1: verify CONFIG_STORE_MAX_DATA_BYTES against ConfigService struct size.

## Requirement traceability
| Requirement | Satisfied by |
|-------------|--------------|
| REQ-DM-090  | config_store_save / config_store_load |
| REQ-NF-214  | config_store_check_integrity + config_store_load (boot sequence §10) |
```
