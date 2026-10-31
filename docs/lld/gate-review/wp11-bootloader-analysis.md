# WP11 — Bootloader hardening: analysis (session 1)

**Date:** 2026-10-31 · **Gate:** GATE-DESIGN. **Luca confirmed D-1 to D-5 as recommended on 2026-10-31**; section 8 records what was done.
**Source:** `docs/lld/drivers/bootloader.md` v1.0 §9, `firmware/gateway/bootloader/`, FirmwareStore.
**Rule kept throughout:** no read-out protection level 2 and no option-byte setting that cannot be
undone is proposed. Option bytes are programmed by Luca only.

## 1. Which items are still open

| Item | State in the LLD | This analysis |
|---|---|---|
| BL-O2 | Open | Section 2 |
| BL-O3 | Open | Section 3 |
| BL-O4 | Open | Section 4 |
| BL-O9 | **Closed 2026-10-27** (five A↔B updates, one rollback, a real self-check failure rolled back in 0.23 s) | Nothing to do. The plan's list is stale |
| BL-O10 | Open, optional | Section 5 |

## 2. BL-O2 — the rollback cap is a lifetime counter

**What the code does.** `rollback_count` lives in the boot record and the metadata. The bootloader
halts with `BOOT_OUTCOME_HALT_ROLLBACK_LIMIT` when it reaches `BOOT_MAX_ROLLBACKS` (16), and increments
the count on every rollback it performs (`boot_core.c` lines 317–325 and 366–377). FirmwareStore's
`do_acknowledge_rollback` also adds one (`firmware_store.c:860`), and nothing ever resets it, not even a
successful `confirm`.

**Consequence.** The 17th rollback in the device's whole life halts it in the bootloader until someone
reflashes it over SWD. A fleet device that takes a bad update every few months would eventually be
bricked by attrition, not by a loop. Real cycling is already impossible (the superseded descriptor is
erased on confirm), so the cap protects against a loop that cannot happen.

| Option | What changes | Cost |
|---|---|---|
| **A. Keep it, document it as a lifetime backstop** | LLD wording only | The 17th rollback ever needs SWD |
| **B. Make it consecutive: clear the count when an image is confirmed** | FirmwareStore `confirm` writes count 0; the bootloader is untouched; host tests for confirm | One more field write inside an existing metadata write; no extra flash cycle |
| C. Raise the constant | One define | Moves the problem, does not remove it |

The LLD row suggests clearing on `acknowledge_rollback`, but that function is itself the thing that
counts a rollback, so the natural place for the clear is `confirm`. This contradicts "preserved
across confirm" in the LLD, which would be rewritten.

**Recommendation: B.** A cap that means "16 failures in a row" is what a backstop should mean.

## 3. BL-O3 — CRC only, no signature check in the bootloader

**What the code does.** `slot_is_bootable` checks the descriptor CRC and the body CRC. The signature is
verified by the application (FirmwareStore, FS-D4) before an image is installed.

**Who can exploit the gap.** Anyone who can write flash directly: an SWD probe, or application code
that is already running. Both can already do anything a signed image could, so a bootloader signature
check adds no protection against them unless read-out and write protection are also on. Against
a corrupt download, the CRC is sufficient.

A signature check in the bootloader would also cost: the bootloader is 16 KB (0x08000000..0x08004000),
signature code and a public key would have to fit in what is left of those 16 KB (the image is 4.9 KB, see
`module-implementation-status.md`), and 480 KB would be hashed a second time per boot.

| Option | Cost |
|---|---|
| **A. Record as a decision: the application authenticates, the bootloader checks integrity** | LLD text only |
| B. Verify the signature in the bootloader | Bootloader size, boot time, a second copy of the public key, a signed-boot threat model nobody asked for |

**Recommendation: A**, as the plan expects, with one sentence stating the threat it does not cover
(a local flash writer).

## 4. BL-O4 — write protection over the bootloader pages

**Facts.** On the STM32L475 the pages are 2 KB, the bootloader is pages 0 to 7 (16 KB). `FLASH_WRP1AR`
protects a start..end page range in bank 1. The metadata (0x08004000) and the descriptors (0x08005000)
sit in pages 8 and up and **must stay writable**, because FirmwareStore writes them. So the range is
exactly pages 0 to 7 and nothing else.

**Why it matters.** `internal_flash_driver` accepts any address in the 1 MB main flash and leaves the
bootloader to FirmwareStore's policy (`internal-flash-driver.md` §2.5). A bug anywhere in the application
that erases page 0 to 7 leaves a board that cannot boot and can only be recovered by SWD. Hardware
protection removes that class of bug. The driver already maps `WRPERR` to its `ERASE` error
(TC-IFD-013), so an attempted erase would be reported, not silently ignored.

**Reversibility.** WRP is set and cleared through the option bytes by an SWD probe at RDP level 0.
It is **not** read-out protection and cannot lock the board. Clearing it needs the probe, the same as
reflashing the bootloader today.

**What needs checking before it is proposed to be set (the bench session, not this one):**
1. `RM0351` on mass erase with WRP active: I have not verified from the manual whether a mass erase skips,
   or fails on, protected pages. That decides whether `openocd program` of a full image still works.
2. The workflow cost: flashing a new bootloader image (`scripts/build-bootloader.sh`) must first clear WRP
   and set it again afterwards, so the bench recipe gains two steps.
3. An option-byte change ends in a reset (`OBL_LAUNCH`); the order with the other option bytes (dual-bank,
   `nBOOT0`) must be read back after the change.

| Option | Cost |
|---|---|
| **A. Set WRP over pages 0..7 on the bench, once, by Luca, after the checks above** | Two extra steps in the bootloader flashing recipe |
| B. Leave unprotected, rely on FirmwareStore policy | A bug can erase the bootloader |
| C. Add a software guard in `internal_flash_driver` refusing the range 0x08000000..0x08003FFF | Host-testable, no option bytes; does not stop code that bypasses the driver |

A and C are not exclusive. **Recommendation: C now (reversible, testable on the host), and A as a bench
step for Luca after verification of the three points.**

## 5. BL-O10 — verification speed

**Now.** 201 ms for 480 KB at MSI 48 MHz (TC-HW-BL-009), byte-wise table CRC fed through a 256-byte copy.
The budget is 500 ms and about 4 % of REQ-NF-203.

| Option | Gain | Cost |
|---|---|---|
| **A. Leave it** | none | none |
| B. Word-wise loop, slicing-by-4 table | Several times faster | About 4 KB of table in the 16 KB bootloader; the image is 4.9 KB of 16 KB today |
| C. Hardware CRC peripheral | Fastest | Its polynomial 0x04C11DB7 with input/output reflection and final XOR matches the table CRC (0xEDB88320, init and final 0xFFFFFFFF), so images stay compatible, but the equivalence has to be proven on the board with the existing test images, and the peripheral clock enable and restore on hand-over must be added |

**Recommendation: A.** The time is already within budget and a faster table costs bootloader space
(4.9 KB of 16 KB used).

## 6. Decisions for Luca

| ID | Decision | Recommendation |
|---|---|---|
| D-1 | BL-O2: A keep as lifetime backstop, B clear on confirm, C raise | **B** |
| D-2 | BL-O3: A record "application authenticates", B signature in the bootloader | **A** |
| D-3 | BL-O4: A set WRP pages 0..7 (bench, Luca), B nothing, C software guard in the driver | **C now, A after the bench checks** |
| D-4 | BL-O10: A leave, B word-wise, C hardware CRC | **A** |
| D-5 | BL-O9: close the plan's item (already closed in the LLD) | Yes |

## 7. What happens after your answers

D-1 B is a FirmwareStore change with host tests, then the LLD rows are closed. D-2 and D-4 A are LLD text.
D-3 C is a small driver change plus tests. D-3 A is not mine to do: it needs your option-byte session and
the three checks in section 4.

## 8. Outcome (2026-10-31)

| ID | Done |
|---|---|
| D-1 | `settle_confirmed` writes count 0 (so `confirm` and `acknowledge_rollback`, which share it, both clear it). Tests updated: TC-FS-073, -078, -088a, -088c; new test_BL028f. BL-D5 and BL-O2 rewritten |
| D-2 | BL-D11 added, BL-O3 closed |
| D-3 | Guard in `internal_flash_erase_page` and `internal_flash_program` (`INTERNAL_FLASH_WRITABLE_FROM_ADDR` = `0x0800_4000`); TC-IFD-032, -033; existing driver tests moved from page 5 to page 20. BL-D12 added. BL-O4 stays open for the bench option-byte step, Luca only |
| D-4 | BL-O10 closed, nothing changed |
| D-5 | BL-O9 was already closed in the LLD |

One consequence to know: `acknowledge_rollback` clears the count too, because it settles through the same
function. After a rolled-back update the count therefore returns to 0 as soon as the application on the old
image acknowledges it, which is what the LLD row originally suggested.
