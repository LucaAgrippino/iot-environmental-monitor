# Bug Log — ConfigStore

## Off-by-one erase: CRC sector not erased before write

**File:** `firmware/field-device/middleware/config_store/config_store.c`
**Line:** 311
**Category:** off-by-one

**What the code does:**
Erases `CS_SECTORS_PER_SLOT - 1` (7) sectors of the target slot, leaving sector 7
(the one containing the CRC field at offset 0x7FF8) intact.

**What it should do:**
Erase all `CS_SECTORS_PER_SLOT` (8) sectors so that the CRC field location is at
erased state (0xFF) before the new CRC value is written.

**Correct fix:**

    /* before */
    if (cs_flash_erase_range(target_base, CS_SECTORS_PER_SLOT - 1U) != QSPI_FLASH_OK)

    /* after */
    if (cs_flash_erase_range(target_base, CS_SECTORS_PER_SLOT) != QSPI_FLASH_OK)

**How to find it with a debugger:**

1. Flash the integration test (`test_config_store_main.c`).
2. Run until the first `===== ALL CHECKS PASSED =====` message appears.
3. Set a breakpoint on the `cs_flash_erase_range` call inside `cs_save_locked`.
4. Press RESET to trigger a second run.
5. When the breakpoint fires, inspect the `sectors` argument — it is 7, not 8.
6. Step over the erase. Then in the memory browser, navigate to `0x90000000 + 0x7000`
   (start of sector 7 of slot A) and verify bytes at `+0x7FF8` are NOT 0xFF — they
   hold the stale CRC from the previous write cycle.
7. Let execution continue to the `cs_flash_write_bytes(target_base + CS_CRC_OFFSET, ...)`
   call. The new CRC is written into a non-erased cell; NOR flash can only program
   bits 1→0, so the stored value is the bitwise AND of the new CRC and the stale CRC,
   which will not match the expected CRC on the next load.

**Why it passes CI:**
The unit test uses a RAM-backed simulation (`g_config_store_flash_sim`, a plain
`uint8_t` array). `memcpy` in `stub_cs_flash_write` overwrites any value regardless
of the previous cell state — it does not enforce the NOR flash constraint that bits
can only change from 1 to 0 without a prior erase. Therefore the CRC write succeeds
in RAM, the computed and stored CRC values match, and all 13 tests pass. The bug is
only observable on real NOR flash hardware.
