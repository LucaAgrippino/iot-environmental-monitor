# Build configurations (WP3)

**Date:** 2026-10-31 · **Goal:** adding a harness or a module no longer breaks the production image, and CI
builds the production image. No configuration beyond the two that CubeIDE projects have always had: **`Debug`
is the harness build, `Release` is the production image.**

## 1. The problem

Every firmware image in this project is one `main()` chosen by the `excluding=` lists of the source
entries in `.cproject`. The Gateway `Debug` configuration lists **every harness folder except the one
it wants**, so a harness folder that nobody adds to the list is compiled in next to the chosen one
and the link fails with two `main()` functions. The same list also hides modules from the production
code: `Src/main.c`, `sensor_service`, `alarm_service`, `lifecycle_wiring` and `firmware_store` are
excluded in `Debug` because the chosen harness does not use them or stubs them.

So `Debug` cannot be the production image, and until now no configuration was: `Release` was stale and
could not link. Two things fix it: the **`Release` configuration repaired into the production image** (no
harness at all), and a **check** that fails when a configuration would compile other than exactly one `main()`.

## 2. State today (checked 2026-10-31)

| Project | Configuration | What it compiles | `main()` count |
|---|---|---|---|
| Gateway | Debug | the `cloud_publisher` harness; `Src/main.c` and four modules excluded; `-O0`, `DEBUG` | 1 |
| Gateway | **Release** (repaired 2026-10-31) | the production image: `Src/main.c` and every module, no harness; `-Os`, no `DEBUG` | 1 (`Src/main.c`) |
| Field Device | Debug | the `lifecycle_controller` harness | 1 |
| Field Device | Release | `Src/main.c` excluded, no harness included | **0: no entry point** (section 7) |

CI builds Gateway `Debug`, Gateway `Release` and Field Device `Debug`.

## 3. The Gateway `Release` configuration (the production image)

**Done 2026-10-31**, at Luca's request, by repairing the stale `Release` in place: no new configuration and no new
output folder (`firmware/gateway/Release/` was already git-ignored). Verified with the headless CubeIDE build
(`gateway/Release`, 0 errors) and with `scripts/check-build-exclusions.sh`.

**What was wrong with it, and what changed**

| | Before | Now |
|---|---|---|
| Source entries | no `Inc`, `application`, `drivers`, `middleware`, `shared`, `stm32`; a harness as the only `main()` | the production list below |
| Definitions | `STM32L475xx` missing (it selects the CMSIS device header) | added; everything else was already there |
| Include paths | 7 of the 15 | the same 15 as `Debug` (including `certs`) |
| `mbedtls-library` | built at `-Os` | `-O2` override as in `Debug`: the TLS handshake limit was only met with an optimised crypto library |
| Kept | `-Os`, no `-g`, no `DEBUG` | unchanged: that is what makes it a release |

**One caveat to know about.** Everything validated on the board so far (stack margins, RAM, the OTA dry run) was
built at `-O0` with `DEBUG` by `scripts/build-gateway-app.sh`. `Release` is `-Os` without `DEBUG`. `DEBUG` only
changes the CPU driver's assert and breakpoint behaviour, and the static RAM use is the same (127.9 KB either way), but
the generated code and its stack use differ, so the first `Release` image should be run on the board once
(section 8). The script remains the way to get the exact `-O0` image.

The production image is what `scripts/build-gateway-app.sh` builds without CubeIDE. The configuration compiles the
same set of files.

**Source entries of `Release`**

| Entry | Excluding | Note |
|---|---|---|
| `Inc` | none | |
| `Src` | **none** | includes `main.c`, `freertos_hooks.c`, `stm32l4xx_it.c`, `syscalls.c`, `sysmem.c` |
| `Startup` | none | |
| `application` | **none** | `sensor_service`, `alarm_service` and `lifecycle_wiring` are production code |
| `drivers` | none | |
| `middleware` | **none** | `firmware_store` is used by `main.c` |
| `coreMQTT`, `freertos-kernel`, `mbedtls-library`, `shared`, `stm32` | as in `Debug` | linked folders |
| `integration-tests` | **no entry** | no harness is part of the production image |

The bootloader (`firmware/gateway/bootloader`), the boot stub (`boot-stub`) and `certs` are not source
entries in any configuration and stay that way: the bootloader and the boot stub have their own
scripts, and `certs` only supplies headers.

**Certificate headers (decided: placeholders in CI).** `main.c` includes `bringup_certs.h` and
`bringup_secrets.h`, found through the `firmware/gateway/certs` include path, and stops with an `#error` if
they are missing. They are git-ignored. CI runs `scripts/make-placeholder-bringup-headers.sh` before the
`Release` build: the image compiles, links and has the right size, but cannot connect (fake Wi-Fi
credentials, an endpoint that does not resolve, one-byte certificates). The script never overwrites a file
that exists, so it is safe on a machine with real credentials. A CI image is smaller than a local one by the
generated `panic_symbols_data.c` (about 55 KB), which a clean checkout does not have. For a working image
use `scripts/bringup_secrets.h.example` and `scripts/regenerate-bringup-certs.sh`.

**Done when:** adding a new folder under `integration-tests` leaves `Release` building (true by
construction: it has no `integration-tests` entry), and CI builds it: the job `Firmware build (l475-release)`
builds `gateway/Release`, next to `l475`, which still builds the `Debug` harness.

## 4. Harness builds

The plan asks for one configuration per harness. There are 37 Gateway harnesses; 37 configurations
in one `.cproject` would be a maintenance burden of its own. Decided 2026-10-31 (the recommendation was
accepted): not 37 configurations.

- Keep **`Debug` as the harness configuration**: it builds the harness named by its exclusion list,
  chosen by hand in the IDE as today.
- The check below stops it from silently breaking: when a harness folder is added and not excluded,
  the check fails and names both `main()` files.
- For a one-off bench build without the IDE, `scripts/build-*-bench.sh` shows the pattern (the
  bootloader, the MqttClient and the stack-marks benches). A generic `scripts/build-harness.sh <module>`
  is a reasonable next step if you want it; it needs no `.cproject` change.

If you prefer one configuration per harness anyway, each is `Debug` copied with the
`integration-tests` list changed; the check handles any number of configurations.


## 5. The Slot-A image

The Slot-A image runs from `0x0800_6000` with `STM32L475VGTX_SLOT_A.ld`, behind the boot stub.
`scripts/build-slot-a.sh` re-links the objects of a CubeIDE build with that linker script, so no
second configuration is needed. It reads the `Debug` objects unless told otherwise:

```bash
CONFIG=Release scripts/build-slot-a.sh    # after building gateway/Release
```

The output goes to `firmware/gateway/<CONFIG>/gateway_slot_a.elf` and `boot_stub.elf`.


## 6. The check

```bash
scripts/check-build-exclusions.sh                    # every configuration of both projects
scripts/check-build-exclusions.sh --config Debug     # only the named configuration(s)
scripts/check-build-exclusions.sh --list             # also print the main() file of each
```

It reads `.cproject` and `.project`, resolves the source entries and the linked folders, applies the
`excluding=` patterns and counts the `main()` definitions in the `.c` files that remain. It exits 1
if any checked configuration compiles other than exactly one. Verified on 2026-10-31: the current
tree passes for `Debug` on both boards, and adding a folder `integration-tests/zz_new_harness` with
a `main()` makes it report `FAIL gateway / Debug: 2 main()` and name both files.

CI runs it with `--config Debug --config Release` for the Gateway and `--config Debug` for the Field Device (whose `Release` is stale, section 7). The workflow now
also runs on pull requests of any base, so stacked pull requests are checked too.


## 7. Decision for Luca: the stale Field Device `Release`

The Field Device `Release` configuration was never maintained: it excludes `Src/main.c` and includes no harness, so it
has no entry point and could never link. The Field Device has no production `main()` yet, so it is not clear what
`Release` should contain there. Either delete it from `.cproject`, or leave it until the Field Device has a production
`main()`. Until then CI checks only the Field Device `Debug`.

## 8. First run of the `Release` image on the board

Build `gateway/Release` (needs the real credential headers for a working image), flash `Release/gateway.elf`, and check:
it reaches `Operational`, connects to the broker, and the one-minute `Stack` report prints no `LOW` line. Record the
lowest free stack words next to the `-O0` figures in `gateway-main.md` section 10b.

**First run, 2026-10-31 (by the author, on the base of PR #124 without the fixes of #125 to #127):** the `-Os` image
reached `Operational`, synchronised NTP, and connected to the broker over TLS with the same arena peak as the `-O0`
image (20 680 of 28 672 bytes). Free stack words in the one-minute report were equal or higher than the `-O0` figures
for every task (SensorTask 128 against 87, CloudPublisher 489 against 287, WifiTask 176 against 154, lifecycle 302
against 334, ModbusPoller 289 against 261) and no `LOW` line appeared. After the connection the known burst of stale
store-and-forward entries (GM-O20) disturbed the session, as it does on any build of that base. Note that the image
has the console on: the UART runs at 460800 baud after start-up, not 115200.
