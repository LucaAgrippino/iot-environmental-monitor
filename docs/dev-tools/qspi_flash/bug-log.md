# Bug Log — QspiFlashDriver (Gateway)

Bugs encountered while building this module, and how to find them. The
driver itself (`qspi_flash.c`) needed no correction — it is hardware-validated
(10/10 bench cases, 2026-09-16). Both defects below were in the surrounding
build and test scaffolding, and both would have cost a later reader real time.

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
