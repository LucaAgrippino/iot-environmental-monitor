# Build configurations (WP3)

**Date:** 2026-10-31 · **Owner of the CubeIDE step:** Luca (the author never edits `.cproject`)
**Goal:** adding a harness or a module no longer breaks the production image, and CI builds the
production image.

## 1. The problem

Every firmware image in this project is one `main()` chosen by the `excluding=` lists of the source
entries in `.cproject`. The Gateway `Debug` configuration lists **every harness folder except the one
it wants**, so a harness folder that nobody adds to the list is compiled in next to the chosen one
and the link fails with two `main()` functions. The same list also hides modules from the production
code: `Src/main.c`, `sensor_service`, `alarm_service`, `lifecycle_wiring` and `firmware_store` are
excluded in `Debug` because the chosen harness does not use them or stubs them.

Two things fix it: a **`Gateway-App`** configuration that contains no harness at all, and a **check**
that fails when any configuration would compile other than exactly one `main()`.

## 2. State today (checked 2026-10-31)

| Project | Configuration | What it compiles | `main()` count |
|---|---|---|---|
| Gateway | Debug | the `cloud_publisher` harness; `Src/main.c` and four modules excluded | 1 |
| Gateway | **Gateway-App** (added 2026-10-31) | the production image: `Src/main.c` and every module, no harness | 1 (`Src/main.c`) |
| Gateway | Release | the `cloud_publisher` harness; source list has no `Inc`, `application`, `drivers`, `middleware`, `shared` or `stm32` | 1, but it cannot link |
| Field Device | Debug | the `lifecycle_controller` harness | 1 |
| Field Device | Release | `Src/main.c` excluded, no harness included | **0: no entry point** |

The two Release configurations are stale (see section 7). CI builds `Debug` only.

## 3. The `Gateway-App` configuration

**Done 2026-10-31**, at Luca's request, by cloning `Debug` in `.cproject` (fresh ids) and setting the source
entries below. Verified with the headless CubeIDE build (`gateway/Gateway-App`, 0 errors) on the working tree
and on a clean checkout without credentials, and with `scripts/check-build-exclusions.sh`. Its output
directory `firmware/gateway/Gateway-App/` is git-ignored. The recipe stays here as the description of what
the configuration must contain, for checking it after an IDE edit and for recreating it.

The production image is what `scripts/build-gateway-app.sh` already builds without CubeIDE. The
configuration must compile the same set of files.

**Steps, if you ever need to recreate it**

1. Project properties, C/C++ Build, Manage Configurations: create **`Gateway-App`** by copying
   **`Debug`**. Everything else (compiler definitions, include paths including `certs`, the linker
   script `STM32L475VGTX_FLASH.ld`, the `-O2` override on `mbedtls-library`, the long
   `freertos-kernel` exclusion list) is then already right.
2. In `Gateway-App`, Paths and Symbols, Source Location, set the entries below.
3. Build it (headless or IDE). Check the result with `scripts/check-build-exclusions.sh --config Gateway-App`.
4. Commit `.cproject`.

**Source entries of `Gateway-App`**

| Entry | Excluding | Note |
|---|---|---|
| `Inc` | none | |
| `Src` | **none** | includes `main.c`, `freertos_hooks.c`, `stm32l4xx_it.c`, `syscalls.c`, `sysmem.c` |
| `Startup` | none | |
| `application` | **none** | `sensor_service`, `alarm_service` and `lifecycle_wiring` are production code |
| `drivers` | none | |
| `middleware` | **none** | `firmware_store` is used by `main.c` |
| `coreMQTT`, `freertos-kernel`, `mbedtls-library`, `shared`, `stm32` | as in `Debug` | linked folders; copy the `Debug` lists unchanged |
| `integration-tests` | **remove the entry** | no harness is part of the production image |

The bootloader (`firmware/gateway/bootloader`), the boot stub (`boot-stub`) and `certs` are not source
entries in any configuration and stay that way: the bootloader and the boot stub have their own
scripts, and `certs` only supplies headers.

**Certificate headers (decided: placeholders in CI).** `main.c` includes `bringup_certs.h` and
`bringup_secrets.h`, found through the `firmware/gateway/certs` include path, and stops with an `#error` if
they are missing. They are git-ignored. CI runs `scripts/make-placeholder-bringup-headers.sh` before the
`Gateway-App` build: the image compiles, links and has the right size, but cannot connect (fake Wi-Fi
credentials, an endpoint that does not resolve, one-byte certificates). The script never overwrites a file
that exists, so it is safe on a machine with real credentials. A CI image is smaller than a local one by the
generated `panic_symbols_data.c` (about 55 KB), which a clean checkout does not have. For a working image
use `scripts/bringup_secrets.h.example` and `scripts/regenerate-bringup-certs.sh`.

**Done when:** adding a new folder under `integration-tests` leaves `Gateway-App` building (true by
construction: it has no `integration-tests` entry), and CI builds it: the job `Firmware build (l475-app)`
builds `gateway/Gateway-App`, next to `l475`, which still builds the `Debug` harness.

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
CONFIG=Gateway-App scripts/build-slot-a.sh    # after building Gateway-App
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

CI runs it with `--config Debug --config Gateway-App`, the configurations it builds. The workflow now
also runs on pull requests of any base, so stacked pull requests are checked too.

## 7. Decision for Luca: the stale Release configurations

Both Release configurations were never maintained: the Field Device one has no entry point and the
Gateway one omits `Inc`, `application`, `drivers`, `middleware`, `shared` and `stm32`. Either delete them from `.cproject`, or fix them (the Gateway
one would become redundant with `Gateway-App`). Recommendation: delete both Release configurations
once `Gateway-App` exists, so a configuration named Release cannot be mistaken for a working build.
Until then run the check with `--config Debug`.
