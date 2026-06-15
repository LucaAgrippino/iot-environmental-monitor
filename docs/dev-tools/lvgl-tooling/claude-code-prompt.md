# Claude Code prompt — LVGL vendoring and host-side simulator

**Authorisation:** Luca has reviewed this brief and authorised execution end-to-end.
You may create branches, modify files, run builds, and open a pull request without
further confirmation. Stop and ask only if a step fails in a way the prompt does
not anticipate.

**Mode:** Fresh execution. No prior branch or files for this work exist.

---

## 0. Branch creation

Run `git branch --show-current` to confirm you are on `main`. Pull latest.

```bash
git switch main
git pull --ff-only
git switch -c feature/lvgl-vendoring-and-simulator
```

If `git branch --show-current` does not output `main` before the `switch -c`,
stop and report the current branch.

---

## 1. Goal

Deliver a single PR that:

1. Vendors **LVGL v8.3.11** as a git submodule at `vendor/lvgl/`.
2. Provides a shared `lv_conf.h` at
   `firmware/field-device/middleware/graphics_library/lv_conf.h` used by both
   the embedded firmware and the host-side simulator.
3. Registers the LVGL sources in the F469 CubeIDE project so they are
   cross-compiled by the existing ARM toolchain (no calls into LVGL yet —
   sources must just build clean).
4. Scaffolds a host-side simulator under `simulator/lcd-ui/` that builds
   inside a Docker container (Ubuntu 24.04 + SDL2 + CMake) and runs natively
   on WSL2 via WSLg.
5. Extends `.github/workflows/ci.yml` with a 7th check (`simulator-build`)
   that builds the simulator in the same Dockerfile on every PR.
6. Passes all 7 CI checks and the local smoke test.

No application code, no GraphicsLibrary middleware, no LcdUi — those are
downstream PRs.

---

## 2. Files to create or modify

### 2.1 Submodule

```bash
git submodule add -b release/v8.3 https://github.com/lvgl/lvgl.git vendor/lvgl
cd vendor/lvgl && git checkout v8.3.11 && cd ../..
git add .gitmodules vendor/lvgl
```

Verify `.gitmodules` ends up with the entry pinned to the path `vendor/lvgl`.

### 2.2 Shared `lv_conf.h`

Path: `firmware/field-device/middleware/graphics_library/lv_conf.h`

Start from `vendor/lvgl/lv_conf_template.h`. Make the following changes
from the template defaults; leave everything else at the template default
value unless noted.

| Setting | Value | Reason |
|---|---|---|
| `LV_CONF_INCLUDE_SIMPLE` (the `#if 0` guard at top) | change to `#if 1` | enable the file |
| `LV_COLOR_DEPTH` | `16` | RGB565 |
| `LV_COLOR_16_SWAP` | `0` | DSI on F469 expects native byte order |
| `LV_MEM_CUSTOM` | `0` | use LVGL's built-in allocator |
| `LV_MEM_SIZE` | `(64U * 1024U)` | 64 KB static heap; resize after profiling |
| `LV_MEM_ADR` | `0` | let LVGL place the heap |
| `LV_DISP_DEF_REFR_PERIOD` | `30` | ~33 Hz internal refresh; LcdUi task ticks at 5 Hz separately |
| `LV_INDEV_DEF_READ_PERIOD` | `30` | touch poll period |
| `LV_TICK_CUSTOM` | `1` | LVGL gets ticks from a callback we provide |
| `LV_TICK_CUSTOM_INCLUDE` | `"lv_tick_source.h"` | small adapter header we'll add later in GraphicsLibrary PR — leave the include name pinned now |
| `LV_TICK_CUSTOM_SYS_TIME_EXPR` | `(lv_tick_source_get_ms())` | function returning uint32_t ms; declared in lv_tick_source.h |
| `LV_USE_LOG` | `0` | logging routes through Logger middleware later |
| `LV_USE_ASSERT_NULL` etc. | leave defaults (`1`) | catch obvious bugs |
| `LV_USE_PERF_MONITOR` | `0` | off in release; can be flipped during profiling |
| `LV_USE_GPU_STM32_DMA2D` | wrap in `#if defined(STM32F469xx)` ... `1` ... `#else` ... `0` ... `#endif` | DMA2D only on the F469 build, off on the host simulator |
| `LV_GPU_DMA2D_CMSIS_INCLUDE` | `"stm32f469xx.h"` (only inside the STM32 branch) | CMSIS device header for register definitions |
| `LV_FONT_MONTSERRAT_14` | `1` | small body text |
| `LV_FONT_MONTSERRAT_20` | `1` | medium emphasis |
| `LV_FONT_MONTSERRAT_28` | `1` | large readings on operational screen |
| `LV_FONT_DEFAULT` | `&lv_font_montserrat_20` | sensible default |
| Widget enables: `LV_USE_LABEL`, `LV_USE_BTN`, `LV_USE_SLIDER`, `LV_USE_BAR`, `LV_USE_LINE`, `LV_USE_CHART`, `LV_USE_KEYBOARD`, `LV_USE_MSGBOX`, `LV_USE_TABVIEW`, `LV_USE_LIST`, `LV_USE_SWITCH` | `1` | needed by the design pack screens |
| Widgets we won't use: leave at template defaults | — | avoids surprise dead code |

Add a banner comment at the top of the file referencing the project, the
LVGL version, and the path to this prompt.

The CMSIS DMA2D branch will not actually compile until `LcdDriver` provides
the symbols LVGL expects, but the include itself will resolve cleanly
because `stm32f469xx.h` is on the F469 build's include path. Verify this.

### 2.3 Registering LVGL in the F469 CubeIDE project

Edit `firmware/field-device/.cproject`. Add:

- **Include paths** (under both Debug and Release configurations, both
  C and C++ tool entries):
  - `"${workspace_loc:/${ProjName}/../../vendor/lvgl}"`
  - `"${workspace_loc:/${ProjName}/middleware/graphics_library}"` (for `lv_conf.h`)
- **Source folder**: add `vendor/lvgl/src` as a linked source folder
  (relative path `../../vendor/lvgl/src`). Exclude `vendor/lvgl/examples`,
  `vendor/lvgl/demos`, `vendor/lvgl/tests` from the build.
- **Preprocessor define**: `LV_CONF_INCLUDE_SIMPLE` so LVGL uses our
  `lv_conf.h` via simple include rather than a relative path.

After editing, run a Debug-configuration build in CubeIDE headlessly:

CubeIDE is installed on the Windows side at
`C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\stm32cubeide.exe`. From WSL2 this is
reachable as `/mnt/c/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeide.exe`.

```bash
# from repo root, run from WSL2
/mnt/c/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeide.exe -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data /tmp/cubeide-headless-workspace \
  -import firmware/field-device \
  -build "field-device/Debug"
```

The build must succeed with the LVGL sources included. Zero warnings target
is unrealistic for vendored code — record the warning count for the PR
description.

If the Windows-from-WSL invocation fails (path translation issues, headless
mode unavailable in this CubeIDE version, workspace lock conflicts), document
the failure mode in the PR description and rely on the F469 build CI check
as the authoritative verification. Do not block the PR on this step.

### 2.4 Simulator skeleton

Create the following under `simulator/lcd-ui/`:

#### 2.4.1 `Dockerfile`

```dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang-format \
        libsdl2-dev \
        pkg-config \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
```

#### 2.4.2 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(lcd_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Path to the shared lv_conf.h (firmware authoritative copy)
set(LV_CONF_PATH
    ${CMAKE_SOURCE_DIR}/../../firmware/field-device/middleware/graphics_library/lv_conf.h
    CACHE STRING "Path to lv_conf.h")

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_compile_definitions(LV_CONF_PATH=${LV_CONF_PATH})

# LVGL submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/../../vendor/lvgl lvgl_build)

# SDL2
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# lv_drivers SDL2 backend (vendored as a small helper to avoid pulling
# the whole lv_drivers repo). We will inline the two files we need.
add_executable(lcd_sim
    main.c
    sdl_backend.c
)

target_include_directories(lcd_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/../../firmware/field-device/middleware/graphics_library
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(lcd_sim PRIVATE lvgl ${SDL2_LIBRARIES})
target_compile_options(lcd_sim PRIVATE ${SDL2_CFLAGS_OTHER})
```

#### 2.4.3 `sdl_backend.c` and `sdl_backend.h`

Adapt the SDL2 display/input driver from `lv_drivers/sdl/sdl.c` and
`sdl_common.c` (upstream `lvgl/lv_drivers` repo, tag `release/v8.3`).
Inline a minimal version under MIT licence with a header comment crediting
the source.

The backend must:
- Open an SDL window 800×480 with a 2× logical scale (use
  `SDL_RenderSetScale(renderer, 2.0f, 2.0f)`).
- Register one LVGL display driver with a 50 KB partial render buffer.
- Register one LVGL pointer input device backed by SDL mouse events.
- Provide a `void sdl_pump(void)` function the main loop calls every 5 ms.

#### 2.4.4 `main.c`

```c
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "lvgl.h"
#include "sdl_backend.h"

/* Forward declaration of the tick adapter the firmware will also use. */
uint32_t lv_tick_source_get_ms(void);

int main(void)
{
    lv_init();
    sdl_backend_init();

    /* Hello LVGL — solid background + version label, centred. */
    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t * label = lv_label_create(scr);
    char buf[64];
    snprintf(buf, sizeof(buf), "LVGL %d.%d.%d — simulator OK",
             LV_VERSION_MAJOR, LV_VERSION_MINOR, LV_VERSION_PATCH);
    lv_label_set_text(label, buf);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), LV_PART_MAIN);
    lv_obj_center(label);

    for (;;) {
        lv_timer_handler();
        sdl_pump();
        usleep(5 * 1000);
    }

    return 0;
}

/* Stub tick source — uses SDL's millisecond counter via sdl_backend. */
uint32_t lv_tick_source_get_ms(void)
{
    extern uint32_t sdl_backend_get_ticks_ms(void);
    return sdl_backend_get_ticks_ms();
}
```

`sdl_backend.c` must therefore provide `sdl_backend_get_ticks_ms()` returning
`SDL_GetTicks()`.

#### 2.4.5 `Makefile`

```makefile
IMAGE := lcd-sim-builder:latest
BUILD_DIR := build
WORK := /work

.PHONY: image build run shell clean

image:
	docker build -t $(IMAGE) .

build: image
	docker run --rm -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash -c "cmake -S . -B $(BUILD_DIR) -G Ninja && cmake --build $(BUILD_DIR)"

run:
	./$(BUILD_DIR)/lcd_sim

shell: image
	docker run --rm -it -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash

clean:
	rm -rf $(BUILD_DIR)
```

#### 2.4.6 `.dockerignore`

```
build/
.vscode/
*.swp
```

#### 2.4.7 `README.md`

One-page how-to:
- Prerequisites (WSL2 with WSLg, Docker Desktop with WSL2 backend)
- `make build`
- `make run`
- `make shell` for poking around
- Expected outcome (screenshot description, not screenshot file)
- Troubleshooting: window doesn't appear → check `echo $DISPLAY` is set; reboot WSL with `wsl --shutdown` then reopen.

### 2.5 CI extension

Edit `.github/workflows/ci.yml`. Add:

- `submodules: recursive` to the `actions/checkout@vN` step in every job
  that needs LVGL sources (F469 build, L475 build, new simulator-build).
- A new job named `simulator-build`:

```yaml
  simulator-build:
    name: Simulator build (host)
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - name: Build Docker image
        run: docker build -t lcd-sim-builder:ci simulator/lcd-ui
      - name: Build simulator inside container
        run: |
          docker run --rm -v "${{ github.workspace }}:/work/repo" \
            -w /work/repo/simulator/lcd-ui \
            lcd-sim-builder:ci \
            bash -c "cmake -S . -B build -G Ninja && cmake --build build"
      - name: Confirm binary
        run: test -x simulator/lcd-ui/build/lcd_sim
```

Pin the action versions (`actions/checkout@v4`) to match the existing jobs.

The new job must be added to the branch-protection required-checks list
manually by Luca after the PR merges; you do not have permission to modify
branch protection.

### 2.6 `.gitignore` updates

The root `.gitignore` must cover the new artefacts produced by the simulator
build and Docker workflow. Append the following section to the existing
`.gitignore` (do not reorder or rewrite existing entries):

```gitignore
# --- Simulator (host-side LCD UI build) ---
simulator/lcd-ui/build/
simulator/lcd-ui/.cache/
simulator/lcd-ui/compile_commands.json

# CMake intermediate artefacts (if ever generated at repo root)
/build/
/CMakeCache.txt
/CMakeFiles/
/cmake_install.cmake

# Docker / WSL editor scratch
.docker/
*.swp
*.swo

# CubeIDE headless build scratch (when invoked from /tmp)
/.metadata/
```

After editing, run from the repo root:

```bash
git status
```

Expected output: only the files this PR intentionally creates or modifies
appear. If anything else shows up (build artefacts, IDE metadata, generated
CMake files), add the relevant pattern to `.gitignore` before continuing.
Common offenders to watch for: `simulator/lcd-ui/build/CMakeCache.txt`,
`compile_commands.json` symlinks, `.vscode/` if Luca's editor wrote settings
during the session.

Commit the `.gitignore` update before the file additions it covers — that
way the working tree stays clean throughout the commit sequence.

---

## 3. Smoke test (locally, before push)

Run these from the repo root after all files are in place:

1. `git submodule status` — `vendor/lvgl` at `v8.3.11`.
2. `cd simulator/lcd-ui && make image && make build` — exit 0, `build/lcd_sim`
   exists and is executable.
3. `./build/lcd_sim` from WSL2 — SDL window opens 1600×960 (800×480 logical
   × 2 upscale), dark background, LVGL version label centred. Press the
   window close button to exit cleanly. *(If running this remotely without
   WSLg, document the binary's existence and let CI handle the verification.)*
4. CubeIDE headless build of `field-device/Debug` — exit 0. *(Skip with a
   note if headless CubeIDE unavailable.)*

---

## 4. Commit strategy

Stage and commit in this order. Conventional prefixes, no parenthesised scope.

1. `chore: extend .gitignore for simulator and Docker artefacts`
2. `chore: vendor LVGL v8.3.11 as submodule`
3. `feat: add shared lv_conf.h with DMA2D + RGB565 + static heap`
4. `chore: register LVGL sources in F469 CubeIDE project`
5. `feat: scaffold host-side LCD UI simulator (Docker + SDL2)`
6. `ci: add simulator-build job and recursive submodule checkout`

Each commit must compile (the previous commits' state plus the new one).
If editing `.cproject` produces a noisy diff, that's expected — accept it.

---

## 5. Pull request

Open the PR against `main` with the title:

> `feat: LVGL vendoring and host-side simulator`

Body must include:

- Summary (3-5 lines)
- File-by-file change list
- Smoke test results (which steps passed, which were skipped and why)
- Note that branch-protection update is a manual follow-up by Luca
- Link back to this prompt: `docs/dev-tools/lvgl-tooling/claude-code-prompt.md`

Use `gh pr create` if available.

---

## 6. Post-merge follow-ups (for Luca, not Claude Code)

Listed for the PR description, do not execute:

- Update branch-protection rules to require the new `simulator-build` check.
- Tag `lvgl-tooling-v1.0` on `main` after merge.
- Run `clang-format` on `simulator/lcd-ui/*.c` once (the `.clang-format` in
  the repo applies); the firmware exclusion patterns must not catch
  simulator code — verify and amend `.clang-format-ignore` if needed.

---

## 7. Stop conditions

Stop and report to Luca if any of the following occur:

- LVGL submodule add fails (network, auth, or wrong URL).
- The F469 build fails to compile LVGL sources after `.cproject` edits.
- The Docker build fails for reasons other than a typo you can fix.
- The simulator binary builds but segfaults on startup.
- CI passes locally (via `act` if available) but fails on GitHub for an
  unclear reason.

Otherwise proceed end-to-end and present the PR for review.
