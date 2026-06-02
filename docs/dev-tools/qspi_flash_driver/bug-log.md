# Bug Log — QspiFlashDriver

## qspi_flash_driver — wrong page-boundary check (off-by-one in length)

**File:** `firmware/field-device/drivers/qspi_flash_driver/qspi_flash_driver.c`
**Line:** 255 (in `qspi_flash_write_page`)
**Category:** off-by-one

**What the code does:**
Uses `addr + len` (exclusive end) in the page-boundary comparison, so a write
whose last byte falls exactly on a page boundary is incorrectly rejected.

**What it should do:**
Use `addr + len - 1` (the index of the last byte being written) to check that
both the first and last byte lie in the same 256-byte page.

**Correct fix:**
```c
/* before */
if ((addr & QSPI_PAGE_MASK) != ((addr + (uint32_t)len) & QSPI_PAGE_MASK))

/* after */
if ((addr & QSPI_PAGE_MASK) != ((addr + (uint32_t)len - 1U) & QSPI_PAGE_MASK))
```

**How to find it with a debugger:**
1. Set a breakpoint at the start of `qspi_flash_write_page`.
2. Call `qspi_flash_write_page(0x0080U, data, 128U)` — write the second half of
   the first 256-byte page (bytes 0x80 through 0xFF).
3. Step through: observe that `addr & ~0xFF = 0x00` and
   `(addr + len) & ~0xFF = 0x100`. These differ → function returns
   `QSPI_FLASH_ERR_LEN` without issuing WREN or PP.
4. Yet the call is valid: `0x80 + 128 - 1 = 0xFF`, still within the first page.
5. Correct the comparison to use `addr + len - 1U` and re-test: the call
   succeeds and the WIP poll completes normally.

**Why it passes CI:**
Unit test T-QSPI-06 exercises `addr = 0, len = 128` where both `addr + len = 128`
and `addr + len - 1 = 127` round down to the same page base (0x00), so the buggy
and correct expressions agree. T-QSPI-07 tests a genuine boundary crossing where
both expressions detect the error. No test case hits the specific pattern where
`addr + len` is a multiple of 256 but `addr + len - 1` is not. cppcheck does not
flag a numeric comparison that is logically wrong but syntactically valid.
