# Bootloader — LLD Companion

**Document:** `docs/lld/drivers/bootloader.md`
**Version:** 1.0
**Board scope:** Gateway (B-L475E-IOT01A, STM32L475VG) only. The Field Device has no OTA (D35)
**Layer:** Driver tier, separate image. It runs before the RTOS, owns no task and links no
middleware. It is filed with the drivers because every line of it is bare-metal register access
or logic over the flash driver.
**Status:** Released — implemented, host-tested (32 + 7 cases) and hardware-validated on the Gateway (2026-10-27, 12 of 12)
**Date:** October 2026

**HLD anchor:** `Bootloader` (`«bootloader»` boundary actor, SD-00c, SD-06c, SD-06d, SD-08) and the
bootloader contract of `flash-partition-layout.md` §7 and `hld.md` §13.5 (D36). The HLD treats its
internals as out of scope (SD00-D3); this companion supplies them.

---

## 1. Sources

| Attribute | Value |
|---|---|
| Responsibility | At every reset: choose the boot slot from the A/B boot record, supervise the trial of a new image, verify the chosen slot, and jump to it |
| PROVIDES (upward) | Nothing callable. Its interface to the application is the on-flash state: the boot record and slot descriptors of `flash-partition-layout.md` §7.1, and `SCB->VTOR` |
| USES (downward) | `InternalFlashDriver` (`iinternal_flash_t`), the incremental CRC32 of `config_store_crc.c`, CMSIS (`RCC`, `FLASH`, `GPIOB`, `USART1`, `SCB`, `DWT`) |
| Root requirements | REQ-DM-070 (boot the new partition), -071 (trigger the self-check), -072 (roll back on failure), -073 (retain the previous image until the self-check passes), -074 (dual bank); REQ-NF-204 (rollback within 10 s), REQ-NF-202/-203 (restart within 5 s), REQ-NF-214 (power loss during a flash write) |
| Decisions | D36 (custom 16 KB bootloader), D37, D38 (A/B metadata), D42 (one slot per hardware bank) |
| Image | `0x0800_0000 – 0x0800_3FFF`, 16 KB. Flashed once, separately from the application; never written by it |
| Task context | None. Single-threaded, interrupts masked except NMI, runs to the jump |

**Sequences in which the `Bootloader` lifeline appears:**

| Diagram | Messages |
|---|---|
| SD-00c (§5.3) | 1 — start application from the selected bank |
| SD-06c (§11.5) | 9 — check `boot_state`, `TRIAL_PENDING` → `TRIAL_RUNNING`; 10 — select new bank; 11 — jump |
| SD-06d (§11.6) | 13' — check pending flags, select previous bank; 14' — jump |
| SD-08 (§13) | 14 — check pending flags, select current bank; 15 — jump |

**The contract it implements** is `flash-partition-layout.md` §7 and §7.1 and is not restated. The
shared formats are `firmware_image_format.h`, which this component includes unchanged. The
state-transition table is `firmware-store.md` §13.2; the rows whose actor is *Bootloader* are the
specification of §3.2 below, and `tests/gateway/middleware/firmware_store/sim/bootloader_sim.c`
is the earlier host-side statement of the same rows.

**Device facts used (RM0351, DS11105):**

| Item | Value |
|---|---|
| Reset clock | MSI 4 MHz, `RCC_CR.MSIRGSEL` = 0 (range from `RCC_CSR`); VOS range 1; `FLASH_ACR` reset value `0x0000_0600`, 0 wait states |
| Flash wait states | 0 WS ≤ 16 MHz, 1 WS ≤ 32 MHz, 2 WS ≤ 48 MHz (range 1) |
| ECC | An uncorrectable (double) error sets `FLASH_ECCR.ECCD` and raises NMI; the read itself completes |
| Reset state expected by the application | MSI 4 MHz — `cpu_init()` builds the 80 MHz PLL from MSI with M = 1, N = 40, R = 2 |

---

## 2. Public API

The bootloader exports no function to any other image (REQ-NF-501 is trivially met: nothing above
the driver layer includes its headers). Its internal API is the seam that makes the decision logic
testable on the host.

### 2.1 Dependency-conformance check

`boot_core.h` includes `<stdint.h>`, `iinternal_flash.h` and `firmware_image_format.h` only: no
CMSIS, no RTOS. `boot_hw.h` is the only header that reaches registers, and only
`boot_hw_l4.c` includes `stm32l475xx.h`. No HAL anywhere.

### 2.2 Decision logic (`boot_core.h`, hardware-free)

```c
typedef enum
{
    BOOT_OUTCOME_RUN = 0,             /* jump to report.slot_base                          */
    BOOT_OUTCOME_HALT_NO_IMAGE,       /* neither slot is bootable                          */
    BOOT_OUTCOME_HALT_ROLLBACK_LIMIT, /* BOOT_MAX_ROLLBACKS reached                        */
    BOOT_OUTCOME_HALT_FLASH_FAULT     /* flash unusable, or the trial could not be armed   */
} boot_outcome_t;

#define BOOT_EVT_RECORD_INITIALISED (1UL << 0) /* no valid record: factory record written    */
#define BOOT_EVT_TRIAL_STARTED      (1UL << 1) /* TRIAL_PENDING -> TRIAL_RUNNING             */
#define BOOT_EVT_TRIAL_ABANDONED    (1UL << 2) /* TRIAL_RUNNING at reset: rolled back        */
#define BOOT_EVT_ACTIVE_INVALID     (1UL << 3) /* active slot failed verification: fell back */
#define BOOT_EVT_META_WRITE_FAILED  (1UL << 4) /* a record write failed after one retry      */
#define BOOT_EVT_COPY_INVALID       (1UL << 5) /* a non-blank record copy failed validation  */

typedef struct
{
    boot_outcome_t outcome;
    uint32_t events;         /* BOOT_EVT_* bits                                  */
    uint32_t slot_base;      /* slot to run (RUN only)                           */
    uint32_t image_len;      /* its descriptor's length                          */
    uint32_t version;        /* its descriptor's version                         */
    uint8_t  state_in;       /* boot_state found at reset (0 = no valid record)  */
    uint8_t  state_out;      /* boot_state in force when the jump is made        */
    uint8_t  active_bank;    /* FIRMWARE_BANK_A / _B in force                    */
    uint32_t generation;     /* generation of the record in force                */
    uint32_t rollback_count; /* its rollback count                               */
} boot_report_t;

/** Runs steps 1-3 of flash-partition-layout.md 7. Never jumps, never loops. */
boot_outcome_t boot_core_run(const iinternal_flash_t *fl, boot_report_t *report);
```

`boot_core_run` is a plain function over the `iinternal_flash_t` vtable (erase, program, read), the
same abstraction FirmwareStore uses, so the host tests drive the real code against
`fake_internal_flash` (NOR + ECC semantics, power-cut injection). The vtable-versus-direct choice
is by dependency inversion: the logic must not know whether it sits over registers or a fake.

### 2.3 Hardware seam (`boot_hw.h`, target only)

```c
void boot_hw_clocks_fast(void);                       /* MSI 4 -> 48 MHz, 2 WS, saves state   */
void boot_hw_console_init(void);                      /* USART1 115200 8N1 on PB6/PB7         */
void boot_hw_console_puts(const char *s);
void boot_hw_console_hex(uint32_t v);                 /* "0x" + 8 digits                      */
uint32_t boot_hw_cycles(void);                        /* DWT cycle counter                    */
void boot_hw_halt(void) __attribute__((noreturn));    /* LED PB14 blinks, console line once   */
void boot_hw_jump(uint32_t slot_base) __attribute__((noreturn)); /* restore reset state, go   */
```

### 2.4 Error codes

None cross a module boundary. The outcome enum above is the whole status surface; the report is
written to the console (§3.5).

### 2.5 What the bootloader does not do

It does not accept firmware over any interface, verify the ECDSA signature (done once, in the
QSPI staging area, before install — FirmwareStore), write a slot descriptor, or run the
application's self-check. It only reads descriptors and reads and writes the boot record.

---

## 3. Internal design

### 3.1 Files

```
firmware/gateway/bootloader/
├── boot_core.c / .h       decision logic; no register access
├── boot_hw.h
├── boot_hw_l4.c           clocks, console, LED, NMI, jump
├── boot_main.c            reset handler, vectors, .data/.bss, calls the above
└── bootloader.ld          16 KB at 0x0800_0000, stack at the top of SRAM1
```

Linked in from elsewhere (not copied): `drivers/internal_flash/internal_flash.c` and
`middleware/config_store/config_store_crc.c`. The CubeIDE project lists its source folders
explicitly, so `bootloader/` is never compiled into the application. It is built by
`scripts/build-bootloader.sh`.

### 3.2 `boot_core_run` flow

1. **Select the record.** Read both copies. A copy is valid when magic, CRC32 (over `0x00..0x0F`),
   both bank values and the state value are legal. Select the valid copy with the higher
   generation (copy A on a tie). A copy that holds something but fails validation (a torn write)
   sets `BOOT_EVT_COPY_INVALID`; a blank copy, the normal state of the one not yet used, does not.
   A copy that cannot be *read* is a flash fault and halts: it is never treated as "no record",
   because that would write a factory record over a perhaps perfectly good one.
2. **No valid copy.** Write `{active A, CONFIRMED, previous B, count 0}` as generation 1
   (§7.2 of the layout) and carry on with that record. Set `BOOT_EVT_RECORD_INITIALISED`.
3. **Apply `boot_state`.**

   | `boot_state` | Action |
   |---|---|
   | `CONFIRMED`, `ROLLED_BACK` | None |
   | `TRIAL_PENDING` | Write `TRIAL_RUNNING` (same banks and count). If the write still fails after one retry, return `HALT_FLASH_FAULT` (BL-D6) |
   | `TRIAL_RUNNING` | If `rollback_count >= BOOT_MAX_ROLLBACKS` return `HALT_ROLLBACK_LIMIT`. Else write `{active = previous, ROLLED_BACK, previous = failed slot, count + 1}` |

4. **Verify the active slot** (§3.3). On success, return `RUN` for it.
5. **Fall back.** If the active slot fails, and `rollback_count < BOOT_MAX_ROLLBACKS`, and the
   *other* slot verifies, write `{other, ROLLED_BACK, previous = failed, count + 1}` and return
   `RUN` for the other slot. If the limit is reached return `HALT_ROLLBACK_LIMIT`; if the other
   slot also fails return `HALT_NO_IMAGE`.

A record write that fails after the retry is **not** fatal on the rollback and fall-back paths
(BL-D6): the slot to be run has just been verified and the unchanged record names the failed
slot, so the next reset repeats the same decision.

### 3.3 Slot verification

A slot is bootable when all of the following hold:

1. its descriptor page reads, `magic == FIRMWARE_DESC_MAGIC`, and `desc_crc32` matches bytes
   `0x00..0x17`;
2. `load_addr` equals the slot base and `1 <= image_len <= FIRMWARE_SLOT_SIZE`;
3. the CRC32 of `image_len` bytes from the slot base equals `body_crc32`. The body is read through
   the vtable in 256-byte chunks into a stack buffer and folded with the incremental CRC;
4. **vector sanity** (BL-D9): the first two words are a plausible initial stack pointer (8-byte
   aligned, within SRAM1 or SRAM2) and a Thumb reset vector inside the body.

Check 4 stops a descriptor that is internally consistent but describes something that cannot run
(for example an image linked for the wrong slot whose descriptor was forged) from being jumped to.

### 3.4 Boot record write

Identical to `fs_meta_write` (layout §7.1): erase the copy that is *not* selected (copy A if none
is); program the first 16 bytes, then the final double word holding the CRC (CRC last); read the
24 bytes back and compare. One retry on any failure. The selected copy is never touched, so at
every instant at least one valid record exists (REQ-NF-214).

### 3.5 Reset sequence (`boot_main.c`)

```
reset_handler:  copy .data, zero .bss, enable DWT cycle counter
                boot_hw_console_init()          -> "BL " banner
                boot_hw_clocks_fast()           (480 KB verifies in 201 ms; ~2.4 s at the reset clock)
                internal_flash_init()           fail -> HALT_FLASH_FAULT
                outcome = boot_core_run(driver, &report)
                print report line (state in/out, slot, generation, events, cycles)
                RUN  -> boot_hw_jump(report.slot_base)
                else -> boot_hw_halt()
```

`boot_hw_jump` restores the state the application expects: MSI back to 4 MHz, `FLASH_ACR` as found,
USART1 and GPIOB reset through their RSTR bits and their clocks gated, DWT disabled, NMI flag
cleared; then it sets `SCB->VTOR`, loads `MSP` from the slot's first word, `dsb`/`isb`, and branches to the
second. A halted bootloader prints one line and blinks LD2 (PB14), so a dead board is visible.

### 3.6 NMI

`NMI_Handler` tests `FLASH->ECCR & FLASH_ECCR_ECCD`. If set it records the event, clears the flag
(write 1) and returns; the read that raised it has completed with unreliable data, which the CRC
check then rejects. Any other NMI source halts. HardFault, MemManage, BusFault and UsageFault all halt.

### 3.7 Principles applied

P1 single responsibility: `boot_core` decides, `boot_hw_l4` touches registers. P4 no hidden
dependencies: the flash vtable is passed in. P5 fail safe: every unrecoverable state halts visibly;
no path jumps to an image that did not verify. P7 reuse: flash driver and CRC are linked, not copied.
P9 testability: the whole decision tree runs on the host over a fault-injecting flash fake.

### 3.8 Decisions

| ID | Decision | Why |
|---|---|---|
| BL-D1 | Link `internal_flash.c` and `config_store_crc.c` rather than write bootloader-private flash and CRC code | Both are hardware-validated and depend only on CMSIS. Revises the sentence in `internal-flash-driver.md` §1 that the bootloader "has its own flash code" — written when no bootloader existed |
| BL-D2 | Decision logic over `iinternal_flash_t`, hardware seam separate | Host tests run the real logic over `fake_internal_flash` |
| BL-D3 | Verify at MSI 48 MHz; restore the reset clock state before the jump | **Measured:** a 480 KB slot verifies in 201 ms at 48 MHz (TC-HW-BL-009). The cost is linear in the clock, so the same work at the reset 4 MHz would be about 2.4 s of the 5 s restart budget (REQ-NF-203). MSI 4 MHz must be restored because `cpu_init()` builds the PLL from it (TC-HW-BL-010 proves it is) |
| BL-D4 | Freestanding image: own vectors and startup; only `memcpy`/`memcmp`/`memset` from newlib-nano | No RTOS, no libc init, no heap |
| BL-D5 | `BOOT_MAX_ROLLBACKS` = 16, overridable with `-D`; applies to the stored (lifetime) count | Layout §7 step 3 asks for a configurable maximum. See BL-O2 |
| BL-D6 | Write-failure policy as in §3.2: halt if the trial cannot be armed; boot the verified slot if only a rollback record cannot be written | Booting a trial that cannot be supervised would defeat REQ-DM-072; refusing a verified known-good slot would turn a flash glitch into a dead gateway |
| BL-D7 | Console report on USART1 at 115200 | Makes every bench result observable and gives field service a serial boot log; costs ~300 bytes |
| BL-D8 | NMI handler clears an ECC detection and returns | A torn double word after a power cut must make the record invalid, not hang the bootloader. **Seen on the board:** an interrupted record write raised the NMI at boot (TC-HW-BL-008, 40 µs case) and the boot continued |
| BL-D9 | Vector sanity check in addition to descriptor and body CRC | Cheap, and a jump to a non-image is the one failure the CRC cannot see |
| BL-D10 | Companion filed under `drivers/` | Bare-metal, below the RTOS, no upward dependencies |

---

## 4. Hardware contract

| Register / field | Use | Reference |
|---|---|---|
| `RCC_CR` `MSIRANGE[7:4]`, `MSIRGSEL[3]`, `MSIRDY[1]` | Select range 11 (48 MHz) from `RCC_CR`; restore range 6 (4 MHz) before the jump | RM0351 §6.4.1 |
| `FLASH_ACR` `LATENCY[2:0]`, `PRFTEN`, `ICEN`, `DCEN` | 2 WS while at 48 MHz; restored to the value found at reset | RM0351 §3.7.1 |
| `FLASH_ECCR` `ECCD[31]` | Read in `NMI_Handler`, cleared by writing 1 | RM0351 §3.7.8 |
| `FLASH_KEYR/CR/SR/OPTR` | Through `InternalFlashDriver` only | `internal-flash-driver.md` §4 |
| `RCC_AHB2ENR.GPIOBEN`, `RCC_APB2ENR.USART1EN`, `RCC_AHB2RSTR`, `RCC_APB2RSTR` | Console clocks on, then reset and off before the jump | RM0351 §6.4 |
| `GPIOB` `MODER/AFR[0]` pin 6 (AF7), pin 14 (output) | USART1 TX to the ST-LINK virtual COM port, as DebugUartDriver; LD2 (PB14) | UM2153 (B-L475E-IOT01A: ST-LINK VCP on USART1 PB6/PB7; LD2 on PB14); `debug_uart.c` |
| `USART1` `BRR`, `CR1`, `ISR.TXE/TC`, `TDR` | 115200 at the 48 MHz PCLK2, polled | RM0351 §38 |
| `DWT->CTRL.CYCCNTENA`, `CoreDebug->DEMCR.TRCENA` | Cycle counter for the boot-time line | Cortex-M4 TRM |
| `SCB->VTOR`, `MSP` | Hand-over | Cortex-M4 generic user guide |

Clock tree: reset MSI 4 MHz is raised to 48 MHz in range 1 (the reset VOS), PCLK2 = SYSCLK.
Peripheral clocks other than GPIOB and USART1 are not touched. No NVIC interrupt is enabled; NMI
cannot be masked. No HAL header appears.

---

## 5. Sequence integration

| Diagram message | Realised by |
|---|---|
| SD-06c 9 — TRIAL_PENDING → TRIAL_RUNNING | §3.2 step 3, row `TRIAL_PENDING` |
| SD-06c 10 — select new bank | §3.2 step 1 (selected record names it) and step 4 |
| SD-06c 11, SD-00c 1, SD-06d 14', SD-08 15 — jump | `boot_hw_jump` |
| SD-06d 13' — check pending flags, select previous bank | §3.2 step 3, row `TRIAL_RUNNING`; or the application having written `ROLLED_BACK` itself (the step 3a protocol), which reaches step 4 unchanged |
| SD-08 14 — select current bank | §3.2 step 1, `CONFIRMED` |
| `firmware-store.md` §13.2 row "Boot, no valid metadata" | §3.2 step 2 |
| `firmware-store.md` §13.2 row "Active descriptor invalid or body CRC fails" | §3.2 step 5 |

---

## 6. Error and fault behaviour

| Condition | Behaviour | Observable |
|---|---|---|
| No valid record copy | Factory record, Slot A, CONFIRMED, generation 1 | `RECORD_INITIALISED` |
| One copy invalid (torn write) | The other is used | `COPY_INVALID` |
| `TRIAL_PENDING` | Armed to `TRIAL_RUNNING`; boot the new slot | `TRIAL_STARTED` |
| `TRIAL_RUNNING` at reset | Roll back to `previous`, count + 1 | `TRIAL_ABANDONED` |
| Active slot fails descriptor, body CRC or vector check | Other slot if it verifies, count + 1 | `ACTIVE_INVALID` |
| Both slots fail | Halt, LED blink | `HALT_NO_IMAGE` |
| `rollback_count >= BOOT_MAX_ROLLBACKS` when a rollback is due | Halt | `HALT_ROLLBACK_LIMIT` |
| Flash driver init fails (for example single-bank option bytes) | Halt | `HALT_FLASH_FAULT` |
| Trial cannot be armed (record write fails twice) | Halt | `HALT_FLASH_FAULT`, `META_WRITE_FAILED` |
| Rollback / fall-back record write fails twice | Boot the verified slot, record unchanged | `META_WRITE_FAILED` |
| Power loss during a record write | Selected copy untouched; see `firmware-store.md` §14 | — |
| ECC double error read (NMI) | Handler clears it; the read's CRC check fails | counted, shown on the console line |
| Any fault exception | Halt | LED, one console line |

Retry policy: one retry per record write; none for reads. There is no downstream: the bootloader
reports only on the console. The application reads the outcome from the boot record
(`firmware_store_get_boot_info`).

---

## 7. Unit-test plan (host)

`tests/gateway/drivers/bootloader/test_boot_core.c`, built with `TEST`, over `fake_internal_flash`
(guards on the bootloader region and on the slot descriptors, power-cut injection). Records and
descriptors are seeded as raw bytes from `firmware_image_format.h`, independently of `boot_core.c`.
Slots hold small synthetic images (valid vector pair, `image_len` ≈ 4 KB).

| ID | Case | Expected |
|---|---|---|
| TC-BL-001 | Blank flash, no image | Factory record written (A, CONFIRMED, previous B, generation 1) and `HALT_NO_IMAGE` |
| TC-BL-002 | Blank metadata, Slot A valid | Factory record; `RUN` A; `RECORD_INITIALISED` |
| TC-BL-003 | CONFIRMED, A active, A valid | `RUN` A; no flash write |
| TC-BL-004 | CONFIRMED, B active, B valid | `RUN` B; no write |
| TC-BL-005 | ROLLED_BACK, active valid | `RUN`; no write |
| TC-BL-006 | TRIAL_PENDING → armed | `RUN` new slot; record TRIAL_RUNNING, generation + 1, same banks and count |
| TC-BL-007 | TRIAL_RUNNING at reset | Rolled back: active = previous, ROLLED_BACK, previous = failed, count + 1; `RUN` previous |
| TC-BL-008 | Second reset after TC-BL-006 (still TRIAL_RUNNING) | Same as TC-BL-007 (the unconfirmed-trial path) |
| TC-BL-009 | Full cycle PENDING → boot → RUNNING → application `confirm` record → boot | Stays on the new slot |
| TC-BL-010 | Both copies valid, higher generation wins; tie → copy A | Selected as stated |
| TC-BL-011 | One copy torn (CRC bad) | Other used; `COPY_INVALID`; new record lands on the invalid copy's page |
| TC-BL-012 | Record with bad bank value / state 0 / state 5 / bad magic | Treated as invalid |
| TC-BL-013 | Active descriptor CRC bad, other valid | Fall back, ROLLED_BACK, count + 1; `ACTIVE_INVALID` |
| TC-BL-014 | Active body one byte flipped | Same as TC-BL-013 |
| TC-BL-015 | Descriptor `load_addr` of the other slot | Slot not bootable |
| TC-BL-016 | `image_len` 0 and `image_len` > slot | Slot not bootable |
| TC-BL-017 | Vector pair bad (SP outside SRAM; PC even; PC outside body) | Slot not bootable |
| TC-BL-018 | Both slots invalid | `HALT_NO_IMAGE`; no jump target |
| TC-BL-019 | `count == BOOT_MAX_ROLLBACKS`, TRIAL_RUNNING (019b: a due fall-back) | `HALT_ROLLBACK_LIMIT`; record unchanged |
| TC-BL-020 | `count == BOOT_MAX_ROLLBACKS - 1`, TRIAL_RUNNING | Rolls back to count == limit |
| TC-BL-021 | Arming write fails twice (program error) | `HALT_FLASH_FAULT`, `META_WRITE_FAILED` |
| TC-BL-022 | Arming write fails once | Retry succeeds; `RUN` |
| TC-BL-023 | Rollback write fails twice | `RUN` previous; record unchanged; `META_WRITE_FAILED` |
| TC-BL-024 | Descriptor read fails (024b: every record read fails) | Slot not bootable (024b: `HALT_FLASH_FAULT`, nothing written) |
| TC-BL-025 | Power cut at every program/erase boundary of the arming write (a), of the rollback write (b) and of the factory record (c), then reboot | At least one valid record every time; reboot converges: never boots an unverified slot, never loses the previous image |
| TC-BL-026 | Read-back corruption of the new record | Detected, retried |
| TC-BL-027 | Flash guards | The bootloader region, both descriptor pages and both slots are never erased or programmed by `boot_core_run` |
| TC-BL-028 | Contract with the real FirmwareStore (`test_fs_bootloader_contract.c`): install → `activate` → boot → `get_boot_info` = TRIAL_RUNNING → `confirm` / `rollback` / crash / rotted image / bootloader-initialised device | Both directions accepted; the old image is gone after confirm; a failed one can never be booted |
| TC-BL-029 | Rollback + application-side rollback converge on the same record | Equal fields |
| TC-BL-030 | Equivalence with the simulator: for each state × slot-validity combination the chosen slot equals `bootloader_sim_boot()` | Identical |

Failure modes of §6 map: no valid copy → 001/002; one copy invalid → 011; pending → 006; running →
007/008; active invalid → 013–017; both → 018; limit → 019/020; flash init → hardware plan only
(BL-HW-007); arm failure → 021/022; rollback write failure → 023; power loss → 025; ECC NMI →
hardware plan only (BL-HW-008); fault exception → hardware plan only.

---

## 8. Hardware test plan and results (on target)

Bench: B-L475E-IOT01A on `/dev/ttyACM0`, openocd. `integration-tests/bootloader/main_test_bootloader.c`
is linked twice (`scripts/build-bootloader-bench.sh`), once for Slot A and once for Slot B. It plays
the application's half of the boot-record protocol with the same flash driver FirmwareStore uses and
reports over USART1. `scripts/bootloader_bench.py` flashes the images, provisions the slot
descriptors (`scripts/make_slot_descriptor.py`), resets the board and checks the bootloader's report
line and the slot image's own. **Run 2026-10-27: 12 passed, 0 failed.**

| ID | Scenario | Result |
|---|---|---|
| TC-HW-BL-001 | Blank metadata, Slot A image: factory record, Slot A runs, VTOR `0x0800_6000` | PASS |
| TC-HW-BL-003 | `TRIAL_PENDING` on B: armed to `TRIAL_RUNNING`, B runs, VTOR `0x0808_0000` | PASS |
| TC-HW-BL-004 | Reset again without confirming: rolled back to A, `ROLLED_BACK`, count + 1 | PASS |
| TC-HW-BL-005 | `confirm` from the application, then resets: stays on B, nothing written | PASS |
| TC-HW-BL-006 | New image fails its body CRC: A runs, `ROLLED_BACK` (006b: factory Slot A has a bad descriptor: falls back to B) | PASS |
| TC-HW-BL-007 | Both slots unbootable: halts, one console line, nothing started. LD2 blinking is checked by eye | PASS (LED not machine-checked) |
| TC-HW-BL-008 | Reset 0..90 µs into a record write, ten times: always a valid record and a running image; 2 of 10 left a torn copy that was rejected, 1 raised an uncorrectable-ECC NMI that the bootloader absorbed | PASS |
| TC-HW-BL-009 | 480 KB slot: descriptor + body CRC + vector check **201 ms** (budget 500 ms, ≈4 % of REQ-NF-203) | PASS |
| TC-HW-BL-010 | After the hand-over `cpu_init()` returns OK and reaches the PLL; UART text is intact at 115 200 from the 80 MHz PCLK2 | PASS |
| TC-HW-BL-011 | The application rolls back itself (`ROLLED_BACK` written by it): the bootloader boots the result and writes nothing | PASS |
| TC-HW-BL-012 | A hang during the trial, ended by the independent watchdog (a genuine crash path): rolled back at the next boot | PASS |
| *(not run)* | Single-bank option bytes (`HALT_FLASH_FAULT` on init) — would mean changing the option bytes; covered on the host only | — |
| *(not run)* | Real FirmwareStore install → `activate` → confirm / rollback on the board (TC-HW-FS-010 in full) | Open: BL-O9 |

The rollback latency of REQ-NF-204 is dominated by the application's own self-check; the bootloader's
part is the one reset plus the verification above (≈0.3 s including start-up).

---

## 9. Open items

| ID | Item | Owner / milestone | Status |
|---|---|---|---|
| BL-O1 | Visual Paradigm model and diagrams do not yet show the bootloader's internals (SD00-D3 keeps them out of the HLD); no HLD change is needed | Luca — with the A1–A4 model update (FS-O11) | Open |
| BL-O2 | `rollback_count` is a lifetime counter (FirmwareStore preserves it across `confirm`), so `BOOT_MAX_ROLLBACKS` caps the device's rollbacks for life, not consecutive ones. Layout §7 step 3 does not say which it means. Real cycling is already impossible (the superseded descriptor is erased on confirm), so 16 is a backstop. To make it "consecutive" FirmwareStore would clear the count on `acknowledge_rollback` | Luca decides; FirmwareStore change if so | Open |
| BL-O3 | The bootloader does not authenticate: it checks CRCs, not the signature. Anyone able to write flash (SWD, or application code) can install an image with a valid CRC. Read-out protection and write protection of pages 0–7 and of the metadata are not configured | Security review | Open |
| BL-O4 | Write-protect the bootloader pages (`FLASH_WRP1AR`) so a faulty application cannot erase it. Option-byte change; bench decision | Luca | Open |
| BL-O5 | No watchdog during boot; the hardware-IWDG option byte must stay off (the verification alone is 0.2 s) | Documented | Closed |
| BL-O6 | UpdateService (FS-O13) is the first real caller of `activate` | UpdateService milestone | Open |
| BL-O7 | `STM32L475VGTX_SLOT_B.ld` is new in this change and is used by the bench build; the application's `.cproject` still builds the unslotted image, and no release step links and signs both slot images (FS-O9) | Real `main()` / release build | Open |
| BL-O8 | **Implemented 2026-10-27, built, not triggered on the board:** `NMI_Handler` in `firmware/gateway/Src/stm32l4xx_it.c` clears the ECC detection and counts it (`g_flash_ecc_nmi_count`); the harness image contains it. **Found on the board:** a torn boot record raises an uncorrectable-ECC NMI on the first read of that double word. The bootloader absorbs it (BL-D8). The *application* does not: its default `NMI_Handler` is an infinite loop, so FirmwareStore's own read of a torn copy would hang the Gateway. Needs an NMI handler in the real firmware that clears `FLASH_ECCR.ECCD` (the bench harness has one) | Handler added to the real firmware; **not yet triggered on the board** (no torn record was read by the application). Needs no `main()` | Closed in code |
| BL-O9 | Run the real FirmwareStore on the board against this bootloader (install → activate → confirm / rollback, TC-HW-FS-010 in full). | **Done 2026-10-27** with the UpdateService harness (update-service-lld.md §26.4): five successful A↔B updates and one rolled back, a real self-check failure rolled back in 0.23 s, a reset mid-download resumed at the exact offset | Closed |
| BL-O10 | Verification speed: 201 ms for 480 KB comes from a byte-wise table CRC fed through a 256-byte copy. A word-wise loop or a hardware CRC peripheral would be several times faster. Not needed for the 5 s budget | Optional | Open |

---

## Phase H — Readiness review

Pass H checklist against `lld-methodology.md` §5: sources cited per row (§1); every API item has a
threading statement (none: single-threaded) and a Doxygen block in the header; the private state is
a `boot_report_t` and a 256-byte buffer on the stack, plus five flags in `boot_hw_l4.c` (§3); every register is named and cited
(§4); every sequence message is mapped (§5); every outcome has a producing condition (§6); every
failure mode has a host or hardware case (§7, §8); open items have an owner (§9). British
spelling. Nothing is restated from the HLD that is not refined.
