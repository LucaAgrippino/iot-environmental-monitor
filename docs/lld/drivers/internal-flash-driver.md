# InternalFlashDriver — LLD Companion

**Document:** `docs/lld/drivers/internal-flash-driver.md`
**Version:** 1.0
**Board scope:** Gateway (B-L475E-IOT01A, STM32L475VG) only
**Layer:** Driver
**Status:** Released — implemented, host-tested and hardware-validated on the Gateway (2026-10-26)
**Date:** October 2026

**HLD anchor:** InternalFlashDriver in `components.md` (GW driver layer) — added by amendment **A3**
of `firmware-store.md` §4.1, which is **not yet applied** to the HLD (FS-O11). Until it is, this
companion and `firmware-store.md` are the authority for the component.

---

## 1. Sources

| Attribute | Value |
|---|---|
| Responsibility | Erases, programs and reads the on-chip flash of the Gateway MCU |
| PROVIDES (upward) | `IInternalFlash` (`iinternal_flash_t`) |
| USES (downward) | CMSIS (`FLASH` controller registers, `__get_PRIMASK`, `__disable_irq`) |
| Root requirements | REQ-DM-070, REQ-DM-073, REQ-DM-074 (dual-bank scheme), REQ-NF-501 (no vendor headers above the driver layer) |
| Board | Gateway only. The Field Device has no OTA |
| Consumers | `FirmwareStore` (middleware) — the only runtime user (FS §16). The bootloader is a separate image with its own flash code |
| Task context | Called under the FirmwareStore mutex from `UpdateServiceTask` and `LifecycleTask`. No ISR use |

**Gateway ADT exception:** the Gateway default is the ADT pattern. This driver is a documented
exception — a singleton with module-prefixed free functions (IFD-D1): there is exactly one flash
controller. Dependency inversion is still honoured through the `iinternal_flash_t` vtable.

**The device (RM0351, DS11105):**

| Item | Value |
|---|---|
| Size / banks | 1 MB, two 512 KB banks (`FLASH_OPTR.DUALBANK` = 1); bank 2 starts at `0x0808_0000` |
| Erase unit | 2 KB page; 256 pages per bank |
| Program unit | 64-bit double word with ECC, written as two consecutive 32-bit stores |
| Page erase | ~22 ms typical, 24.5 ms maximum (datasheet) |
| Double-word program | ~85 µs typical, ~118 µs maximum (datasheet) |
| Read-while-write | Only **across** banks. A program or erase stalls instruction and data fetch from the **same** bank |

---

## 2. Public API

### 2.1 Dependency-conformance check

`internal_flash.h` includes `iinternal_flash.h` and `stdint.h` only. No FreeRTOS, no CMSIS in the
public header (REQ-NF-501). Confirmed clean.

### 2.2 Interface and free functions

```c
internal_flash_err_t internal_flash_init(void);
internal_flash_err_t internal_flash_erase_page(uint32_t addr);
internal_flash_err_t internal_flash_program(uint32_t addr, const uint8_t *data, uint32_t len);
internal_flash_err_t internal_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

extern const iinternal_flash_t *const internal_flash_driver;   /* erase_page, program, read */

#ifdef TEST
void internal_flash_reset_for_test(void);
#endif
```

`internal_flash_init()` is called directly at start-up and is not part of the vtable.

### 2.3 Error codes

`iinternal_flash.h` is owned by this companion. v1.0 adds two codes to the FirmwareStore set:

| Code | Value | Meaning |
|---|---|---|
| `INTERNAL_FLASH_OK` | 0 | Success |
| `INTERNAL_FLASH_ERR_NULL_POINTER` | 1 | A required pointer argument was NULL |
| `INTERNAL_FLASH_ERR_ADDR` | 2 | Address, or address + length, outside `0x0800_0000..0x080F_FFFF`, or an empty read |
| `INTERNAL_FLASH_ERR_ALIGN` | 3 | Address or length violates the alignment rule |
| `INTERNAL_FLASH_ERR_ERASE` | 4 | The page erase failed (controller error flag, stuck busy, or the controller would not unlock) |
| `INTERNAL_FLASH_ERR_PROGRAM` | 5 | The programming failed, including a target that was not erased |
| `INTERNAL_FLASH_ERR_NOT_INITIALISED` | 6 | `internal_flash_init()` has not succeeded |
| `INTERNAL_FLASH_ERR_BANK_MODE` | 7 | The option bytes are not in dual-bank mode |

### 2.4 Operations

| Function | Pre-conditions | Effect | Errors |
|---|---|---|---|
| `internal_flash_init` | none | Checks `DUALBANK`; marks the driver usable. Leaves the controller **locked**. Idempotent | `BANK_MODE` |
| `internal_flash_erase_page(addr)` | initialised; `addr` 2 KB-aligned, page inside flash | Erases the page to `0xFF`; selects the bank itself; flushes the caches | `NOT_INITIALISED`, `ALIGN`, `ADDR`, `ERASE` |
| `internal_flash_program(addr, data, len)` | initialised; `addr` 8-aligned; `len` a non-zero multiple of 8; `[addr, addr+len)` inside flash; target erased | Programs `len / 8` double words; flushes the caches | `NOT_INITIALISED`, `NULL_POINTER`, `ALIGN`, `ADDR`, `PROGRAM` |
| `internal_flash_read(addr, buf, len)` | initialised; `len` ≥ 1; range inside flash | Copies from mapped flash | `NOT_INITIALISED`, `NULL_POINTER`, `ADDR` |

Argument checks run first and touch no register. The check order is: initialised → NULL → alignment
→ range. A failed operation always leaves the controller locked, its flags clear and `PG`/`PER` cleared.

### 2.5 What the driver does not do

It does not verify what it wrote (FirmwareStore reads back: FS-D9), does not decide what may be
written (the running slot, the bootloader and the metadata are FirmwareStore's policy), has no
mass erase, and cannot reach the option bytes, system memory or OTP: the accepted range is exactly
the 1 MB main flash.

---

## 3. Internal design

### 3.1 Hardware seam

All register and flash-bus access goes through the macros of `internal_flash_hw.h`. On target they
are the CMSIS accesses; under `TEST` they call a model of the flash controller
(`fake_flash_ctrl`) that implements the unlock sequence, `PG`/`PER`/`START`, `BSY`, `EOP` and the
error flags, and records every register write. The tests therefore check the real programming
sequence, not just the return codes. Bit masks are the driver's own `IFLASH_*` constants,
`_Static_assert`-ed against the CMSIS names on target.

### 3.2 Erase

1. Validate (§2.4). Compute `index = (addr − 0x0800_0000) / 2048`; bank 2 if `index ≥ 256`, page number `index mod 256`.
2. Wait for `BSY` to clear (bounded). Clear stale error flags and `EOP` (write 1).
3. Unlock: `KEYR ← KEY1`, `KEYR ← KEY2`. If `CR.LOCK` is still set the controller is unusable → `ERASE`.
4. `CR ← PER | (page << PNB_Pos) | (bank2 ? BKER : 0)`, then `CR |= START`.
5. Wait for `BSY` to clear (bounded). Read `SR`: any of `OPERR|PROGERR|WRPERR|PGAERR|SIZERR|PGSERR|MISERR|FASTERR|RDERR` → `ERASE`.
6. Clear `EOP` and the error flags, `CR ← 0` (drops `PER`), `CR |= LOCK`, flush the caches.

### 3.3 Program

1. Validate. Wait for `BSY`, clear stale flags, unlock (as above).
2. `CR ← PG`.
3. For each double word, with interrupts masked for the two stores only (IFD-D5): store the low word at `addr`, then the high word at `addr + 4`, as 32-bit accesses.
4. Wait for `BSY` to clear (bounded); read `SR`; any error flag → `PROGRAM`, stop. Clear `EOP`.
5. After the last double word, or on error: clear the flags, `CR ← 0`, `CR |= LOCK`, flush the caches.

### 3.4 Cache flush

After every mutating call: clear `ACR.ICEN`/`DCEN`, set then clear `ICRST`/`DCRST`, restore the
enables. Without it the CPU can read the old contents of a page it just erased or programmed
(FS §4.2: "leaves caches coherent for subsequent reads").

### 3.5 Bounded polls

`BSY` waits use an iteration count, as QspiFlashDriver does: 4 000 000 polls. At 80 MHz one poll is
at least ~4 cycles (50 ns each), so the bound is ≥ 200 ms, against a 24.5 ms maximum erase. At the
start-up clock the same count is longer in wall time, never shorter than the device needs. A
timeout is reported as `ERASE` or `PROGRAM`.

### 3.6 Memory

No dynamic memory. One `bool` of state. Stack: a handful of locals, < 64 B.

---

## 4. Hardware contract

| Register | Use |
|---|---|
| `FLASH->KEYR` | `0x4567_0123` then `0xCDEF_89AB` to unlock |
| `FLASH->SR` | `BSY` (bit 16), `EOP` (bit 0), error flags (write 1 to clear) |
| `FLASH->CR` | `PG` (0), `PER` (1), `PNB` (3..10), `BKER` (11), `START` (16), `LOCK` (31) |
| `FLASH->OPTR` | `DUALBANK` (bit 21), read at init |
| `FLASH->ACR` | `ICEN`, `DCEN`, `ICRST`, `DCRST` for the cache flush |

* The flash interface is always clocked: no `RCC` enable is needed (unlike a peripheral).
* **Fetch stall.** The reference manual says that while a page erase or a program runs, an
  instruction or data fetch from the **same bank** is stalled until it finishes, and a fetch from the
  other bank continues. **Measured on the board (IFD-O2): no stall was observed**, with the vector
  table, the ISR and the polling loop all in the bank being erased and with the caches both on
  and off. The figure is a 1 kHz interrupt whose gap stayed at 1.0 ms during a 22 ms erase. It is
  one hot, tiny ISR, not a proof for every code path; the design therefore still keeps the
  target slot in the other bank from the running image (amendment A1) and treats the stall as
  possible (FS-O10).
* The two stores of a double word must be consecutive. An interrupt between them can run a handler
  from the same bank, whose fetch would stall inside the programming sequence; hence the short
  `PRIMASK` window (IFD-D5).
* The write-protection and read-protection option bytes are not touched.

---

## 5. Sequence integration

| FirmwareStore step | Calls |
|---|---|
| `install_begin` (SD-06c) | `erase_page` (descriptor) |
| `install_step` | QSPI read of the staged page; then `erase_page` (slot page), `program` (page), `read` (read-back in 256 B windows) |
| `install_step`, last page | `program` ×2 (descriptor body, then the CRC double word), `read` |
| `activate`, `confirm`, `rollback`, `acknowledge_rollback` | `erase_page` + `program` ×2 + `read` (metadata); `erase_page` (descriptor) |
| `create` | `read` only |

---

## 6. Error and fault behaviour

| Error | Cause | Local behaviour | Caller |
|---|---|---|---|
| `NOT_INITIALISED`, `NULL_POINTER`, `ALIGN`, `ADDR` | Programming error | No register touched | Bug — no retry |
| `ERASE` | `WRPERR`/`PGAERR`/… flag, `BSY` stuck, or the controller would not unlock | Flags cleared, `CR` cleared, locked | FirmwareStore maps to `FLASH_ERASE` (retriable, counts toward DM-052) |
| `PROGRAM` | `PROGERR` (target not erased), `PGSERR`, `BSY` stuck, would not unlock | As above | FirmwareStore maps to `FLASH_WRITE` |
| `BANK_MODE` | `DUALBANK` = 0 | Driver stays uninitialised | Boot fault: the whole slot layout assumes two banks |

There is no logging in a driver (Logger is not a driver dependency); FirmwareStore logs the
failure with the address.

---

## 7. Unit-test plan (host)

Against `fake_flash_ctrl`, which models the controller and records every register write.

| TC | Behaviour | Traces |
|---|---|---|
| TC-IFD-001 | `init` on a dual-bank device → OK; the controller is left locked; a second `init` is harmless | §2.4 |
| TC-IFD-002 | `init` with `DUALBANK` = 0 → `BANK_MODE`; later operations → `NOT_INITIALISED` | IFD-D8 |
| TC-IFD-003 | `erase_page`, `program`, `read` before `init` → `NOT_INITIALISED`; no register touched | §2.4 |
| TC-IFD-004 | `read` copies the bytes; at the last byte of flash OK, one past → `ADDR`; length 0 → `ADDR`; NULL → `NULL_POINTER` | §2.4 |
| TC-IFD-010 | Erase of a bank-1 page: `KEYR` gets KEY1 then KEY2, `CR` = `PER` + page number, then `START`; the page reads `0xFF`; `EOP` cleared; locked afterwards | §3.2 |
| TC-IFD-011 | Erase of a bank-2 page: `BKER` set and `PNB` = index − 256; the first and last page of each bank | §3.2 |
| TC-IFD-012 | Erase with a misaligned or out-of-range address → `ALIGN`/`ADDR`; no register written | §2.4 |
| TC-IFD-013 | Erase of a write-protected page (`WRPERR`) → `ERASE`; flags cleared; `CR` cleared; locked | §6 |
| TC-IFD-014 | Erase with `BSY` stuck → `ERASE` after the poll bound; locked | §3.5 |
| TC-IFD-015 | Erase waits out a controller that is busy when called | §3.2 step 2 |
| TC-IFD-016 | Erase flushes the caches (`ICRST`/`DCRST` pulsed, enables restored) | §3.4 |
| TC-IFD-017 | Stale error flags from an earlier failure do not fail a good erase | §3.2 step 2 |
| TC-IFD-018 | A controller that will not unlock (`LOCK` stays set) → `ERASE`; no `CR` write other than the lock | §3.2 step 3 |
| TC-IFD-020 | Program one double word: `PG` set, the low word then the high word stored, `BSY` waited, `EOP` cleared, `PG` dropped, locked; the memory holds the data | §3.3 |
| TC-IFD-021 | Program several double words: stores in ascending address order, two per double word | §3.3 |
| TC-IFD-022 | Program with misaligned address or length, zero length → `ALIGN`; out of range → `ADDR`; NULL → `NULL_POINTER`; no register written | §2.4 |
| TC-IFD-023 | Program a double word that is not erased → `PROGERR` → `PROGRAM`; flags cleared; `PG` dropped; locked; the old data is intact | §6 |
| TC-IFD-024 | Program across the bank boundary (`0x0807_FFF8` + 16 bytes) | §2.4 |
| TC-IFD-025 | Program with `BSY` stuck → `PROGRAM`; locked | §3.5 |
| TC-IFD-026 | Program stops at the first failing double word; the ones before it are written | §3.3 |
| TC-IFD-027 | Interrupt masking: the mask/restore pairs are balanced, and the controller sees no interrupt-enabled store between the two words of a double word | IFD-D5 |
| TC-IFD-028 | Program flushes the caches | §3.4 |
| TC-IFD-029 | Programming `0x00` bytes onto erased flash works | §3.3 |
| TC-IFD-030 | The vtable `internal_flash_driver` points at the three operations | §2.2 |
| TC-IFD-031 | `internal_flash_reset_for_test` returns the driver to un-initialised | §2.2 |

---

## 8. Hardware test plan (on target)

Scratch regions only: `0x0807_E000–0x0807_FFFF` (bank 1, 8 KB) and `0x080F_8000–0x080F_FFFF` (bank 2,
32 KB). Both are reserved by `flash-partition-layout.md` §5 and lie outside the running image.

| TC | Setup | Pass criterion |
|---|---|---|
| TC-HW-IFD-001 | `init` on the board | OK (the device is in dual-bank mode) |
| TC-HW-IFD-002 | Erase a bank-2 scratch page, read it back | All `0xFF`; time logged (IFD-O1) |
| TC-HW-IFD-003 | Program a 2 KB pattern into it, read back | Identical; time logged |
| TC-HW-IFD-004 | Program over the programmed page | `PROGRAM`; the data is unchanged |
| TC-HW-IFD-005 | Same on a bank-1 scratch page | As 002–004 |
| TC-HW-IFD-006 | Erase every page of the bank-2 scratch region | All erased; no error |
| TC-HW-IFD-007 | Cache coherence: read the page (cached), erase it, read again | The second read is `0xFF`, not the old data |
| TC-HW-IFD-008 | 1 kHz timer interrupt while erasing a bank-2 page, then a bank-1 page; then the same with both caches off (008b) | Recorded, not asserted: the reference manual predicts a stall in the same bank, and the result is IFD-O2 |
| TC-HW-IFD-009 | Program a double word, with the program running with interrupts active | No `PGSERR`; time logged |

---

## 9. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| IFD-O1 | Erase and program times are datasheet figures | **Measured 2026-10-26:** page erase 22.10 ms (both banks); 2 KB program (256 double words) 22.26–22.27 ms, i.e. 87 µs per double word; both match the datasheet typicals. Feeds FS-O1: a 240-page install is about 240 × (22.1 + 22.3) ≈ 10.7 s | Closed |
| IFD-O2 | Size of the fetch stall in the same bank, as seen by an interrupt | **Measured 2026-10-26** (TC-HW-IFD-008/008b): longest gap of a 1 kHz interrupt = 1000 µs while erasing a page in the bank that holds the vectors, the ISR and the polling loop, with the caches on and off, and 1000–1045 µs while programming 2 KB. The expected ~22 ms stall was **not observed**; the cause is not understood. Not proof for a cold or large ISR. Feeds FS-O10 | Open |
| IFD-O3 | The HLD does not yet list the component (amendment A3, FS-O11) | Apply A3 in the VP model and `components.md` | Open |
| IFD-O4 | The bootloader needs its own flash routines; sharing this driver's code is not decided | Bootloader LLD (FS-O7) | Open |

## 10. Decisions

| ID | Decision | Rationale |
|---|---|---|
| IFD-D1 | Singleton with free functions plus a vtable pointer | One controller; same shape as QspiFlashDriver |
| IFD-D2 | Register access behind `internal_flash_hw.h`, with a controller model in the tests | The tests check the programming sequence and the error handling, not only return values |
| IFD-D3 | The controller is locked again after every call, success or failure | A stray write cannot erase or program flash between calls |
| IFD-D4 | A timeout is `ERASE` or `PROGRAM`, no separate code | The caller's reaction is identical, and the interface stays small |
| IFD-D5 | Interrupts are masked only for the two stores of one double word | The sequence must be uninterrupted; the window is ~1 µs, far below the Modbus and sensor deadlines |
| IFD-D6 | No read-back inside the driver | FirmwareStore verifies (FS-D9); doing it twice would double the time |
| IFD-D7 | The caches are flushed after every mutating call | Stale cache lines would hide an erase from the CPU |
| IFD-D8 | `init` refuses a device that is not in dual-bank mode | In single-bank mode the page is 4 KB and the program unit 128 bit; the whole layout assumes two banks |
| IFD-D9 | The accepted range is exactly the 1 MB of main flash | The option bytes, system memory and OTP must be unreachable through this interface |
| IFD-D10 | No mass erase | Nothing needs it and a mistake would erase the bootloader |

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H-01 | Every public function specified with pre-conditions, effects and errors | Done (§2.4) |
| H-02 | Every error code with cause, local behaviour and caller reaction | Done (§2.3, §6) |
| H-03 | Hardware contract: registers, sequence, timing, stall behaviour | Done (§3, §4) |
| H-04 | `_reset_for_test()` hook guarded by `#ifdef TEST` | Done (§2.2) |
| H-05 | Unit-test plan with TC identifiers covering all functions and error codes | Done (§7) |
| H-06 | Hardware test plan | Done (§8) |
| H-07 | Open items with IDs and resolution paths; decisions with rationale | Done (§9, §10) |
| H-08 | HLD amendment A3 applied | **No** — IFD-O3 |
