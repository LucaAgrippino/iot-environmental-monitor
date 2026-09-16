# Bug Log — QspiFlashDriver (Gateway)

No bug was intentionally planted in the shipped module. `qspi_flash.c` on
`main` is believed correct and is hardware-validated (10/10 bench cases,
2026-09-16). Planting a defect in merged code that backs `ConfigStore`,
`CircularFlashLog` and `FirmwareStore` would risk real data loss, so this
file follows the `wifi_driver` precedent instead: the **real** defects found
during development are documented first, then the one mock-invisible bug
class worth rehearsing, written out in full as an answer key.

---

## Real defect 1 — build system: a new integration-test folder silently breaks the firmware link

**File:** `firmware/gateway/.cproject`
**Category:** wrong bound / implicit-inclusion build rule

**What the code did:**
The `integration-tests` sourceEntry excludes subfolders **by name**:

    excluding="wifi_task|mqtt_client|wifi_driver|exti|logger|cpu|rtc|debug_uart|spi|gpio"

Anything *not* on that list is compiled. `Src` separately carries
`excluding="main.c"`, so exactly one bring-up `main()` is the firmware's
entry point at a time. Creating `integration-tests/qspi_flash/` therefore
added a **second** `main()` alongside the then-active `cloud_publisher` one.

**What it should do:**
Every integration-test folder except the one deliberately being run must be
on the exclusion list.

**Correct fix:**

    /* before */
    excluding="wifi_task|mqtt_client|wifi_driver|exti|logger|cpu|rtc|debug_uart|spi|gpio"
    /* after */
    excluding="wifi_task|mqtt_client|wifi_driver|exti|logger|cpu|rtc|debug_uart|spi|gpio|qspi_flash"

**How to find it with a debugger:**
No debugger — it is a link-time failure. `arm-none-eabi-gcc` reports
`multiple definition of 'main'` with the two object paths named. The trap is
that it only appears in the **CubeIDE/CI build**, never in a per-file
compile, so a developer who syntax-checks the new file sees nothing wrong.

**Why it passes CI:**
It does not — it fails `Firmware build (l475)`. It is logged because the
*absence* of a failure is what misleads: adding the folder and running only
`ceedling`, `cppcheck` and `clang-format` (none of which look at
`integration-tests/`) gives a clean sweep right up until the CI link step.

**Related trap:** swapping which bring-up test is active requires editing
**two** places — the `excluding=` list *and* the explicit
`<entry ... name="integration-tests/<module>"/>` sourcePath in the second
`sourceEntries` block. Both are bench-local and must never be committed, or
the GW firmware CI job starts building the bring-up test as the firmware.

---

## Real defect 2 — unresolvable include in the integration main

**File:** `firmware/gateway/integration-tests/qspi_flash/main_test_qspi_flash.c`
**Category:** wrong include path

**What the code did:**
Used `#include "status.h"`, copied from the older bring-up mains.
`status.h` lives in `firmware/gateway/drivers/cpu/`, and the build supplies
`-Ifirmware/gateway/drivers` — so the bare form does not resolve from a file
outside that directory. It only works *inside* `cpu/`, where the quoted
include resolves relative to the including file.

**What it should do:**
Use the directory-qualified form, as the newer mains (`cloud_publisher`,
`mqtt_client`, `wifi_task`, `wifi_driver`, `logger`, `exti`) already do.

**Correct fix:**

    /* before */
    #include "status.h"
    /* after */
    #include "cpu/status.h"

**How to find it with a debugger:**
Not needed — `fatal error: status.h: No such file or directory`.

**Why it passes CI:**
It passes CI trivially, because CI never compiles `integration-tests/`. The
older mains (`rtc`, `spi`, `gpio`, `cpu`, `debug_uart`) **still carry the
broken form today** and would fail to compile the moment anyone activates
them for a bench run. Worth fixing the next time each is touched.

---

## The bug worth rehearsing — WIP poll skipped after Page Program

**Not present in the shipped driver.** This is the canonical defect class for
this module: it passes every host test, and only real silicon reveals it.

**File:** `firmware/gateway/drivers/qspi_flash/qspi_flash.c`
**Function:** `qspi_flash_write_page()`
**Category:** missing state-wait / race with the device

**What the buggy code does:**
Issues Write Enable (0x06) and Page Program (0x02), then returns
`QSPI_FLASH_OK` as soon as the QUADSPI peripheral reports the transfer
complete — without polling the flash device's WIP (Write In Progress) bit
via Read Status Register (0x05).

**What it should do:**
The QUADSPI transfer completing means the *bytes reached the chip*, not that
the chip finished writing them. The MX25R6435F is internally busy for up to
10 ms after a page program (and up to 240 ms after a sector erase). Any
command issued while WIP is set is ignored or corrupts the operation. The
driver must poll status until WIP clears before returning.

**Correct fix:**

    /* before */
    err = send_command_with_data(QSPI_CMD_PP, addr, data, len);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    return QSPI_FLASH_OK;

    /* after */
    err = send_command_with_data(QSPI_CMD_PP, addr, data, len);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    return prv_wait_while_busy(QSPI_WIP_TIMEOUT_MS);

**How to find it with a debugger:**

1. Run the bring-up test. TC-HW-QSPI-004 fails: the 256-byte read-back does
   not match what was written. The mismatch is usually **partial** — the
   first bytes are correct and later ones are `0xFF` or stale — which is the
   tell. A wholly wrong buffer suggests an address bug; a *truncated* one
   suggests the device was still busy.
2. Break immediately after the `write_page()` call and single-step a manual
   Read Status Register (0x05). Bit 0 (WIP) reads **1** — the device is
   still writing while the driver has already returned OK.
3. Insert a `cpu_delay_ms(10)` between the write and the read-back. If the
   data now verifies, the defect is confirmed as a missing busy-wait.
4. Confirm the mechanism: the *next* operation is the one that gets eaten.
   Program two pages back to back with no delay — page A lands, page B is
   silently dropped, because the erase/program command for B arrives while
   WIP from A is still set.

**Why it passes CI:**
The host mock has no memory array and no busy state. `qspi_hw_*` returns a
status register with WIP already clear on the first read, so the polling loop
either exits immediately or is skipped entirely with identical observable
behaviour. TC-QSPI-022 ("`write_page()` polls WIP until clear") asserts on
the *command log* — that a 0x05 was issued — so even that test can be made to
pass by issuing the read and discarding the result. Only a device that is
genuinely busy for milliseconds distinguishes the two implementations, which
is precisely why companion §7.5 lists real erase/program timing as
not-host-testable and why Step 6 exists.
