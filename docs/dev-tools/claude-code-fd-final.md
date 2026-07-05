# Claude Code — Module Implementation Session

**Board:** STM32F469I-DISCO (STM32F469NIH6)
**Repository:** `D:\iot-environmental-monitor` (WSL2: `/mnt/d/iot-environmental-monitor`)
**Linker script:** `firmware/field-device/stm32f469nih6_flash.ld`
**CMSIS header:** `stm32f469xx.h`
**Compile define:** `BOARD_FIELD_DEVICE`

---

## Context

This is the IoT Environmental Monitoring Gateway portfolio project.
V-Model methodology. Phase 4 (Implementation).

**Repo root:** `iot-environmental-monitor/`
**Boards:** STM32F469I-DISCO (Field Device), B-L475E-IOT01A (Gateway)
**Language:** C (BARR-C:2018 subset), CMSIS register-level only — no STM32 HAL
**RTOS:** FreeRTOS (static allocation only — no heap after init)
**Test harness:** Ceedling 0.31.1 / Ruby 3.0 / Unity on host (Windows, gcc)
**CI:** GitHub Actions, six required checks before merge

---

## Authorisation and completion contract

You have full authorisation to create, overwrite, delete, and commit
files anywhere in this repository without asking for confirmation at
each step. Do not pause to ask "shall I proceed?" — proceed.

**This prompt is atomic. You execute Steps 0 through 13 in one
continuous session and you finish all of them.** You do not:

- Stop mid-step and say "I'll continue when you respond".
- Leave the unit test file with `TEST_IGNORE_MESSAGE` for tests that
  the companion §7 says to implement now.
- Skip the integration test on the grounds that hardware isn't present
  (host-side compilation and harness layout still get done).
- Skip the dev-tools deliverables in Step 12.
- Defer the bug-log or exercise file with "TBD".
- Push the feature branch without first running `scripts/test-module.ps1`
  to ALL CHECKS PASSED.

The only legitimate reasons to stop short of completing all 13 steps
are these invariant violations — each requires a one-line report to
the user and full halt:

1. Step 0: `git branch --show-current` after switching prints `main`.
2. Step 2: target directories are not empty after the clean slate.
3. Step 9: companion document is not at Pass H or Implementation ready.
4. Step 12: `dev-tools` branch does not exist locally or remotely.
5. The companion contains an ambiguity not resolved in §3 or §8 that
   requires a design decision (ask once, get the answer, continue
   without further interruption).

Any other obstacle (test failure, lint error, compile error, link
error, missing symbol, etc.) is your problem to solve — iterate until
fixed. Do not surface these to the user as a stopping condition.

---

## Your role

You are implementing ONE module from its LLD companion document (attached
below). The companion is the authoritative source. Do not make
architectural decisions that contradict it.

---

## Regeneration mode

If files for **this** module already exist on disk, **delete them all
first** before writing anything new. Do not reuse, patch, or extend
existing files for the module you are building. Start from a clean
slate every time.

```bash
# Delete all existing files for THIS module before starting
rm -rf firmware/field-device/<layer>/<module>/
rm -rf tests/field-device/<layer>/<module>/
rm -rf firmware/field-device/integration-tests/<module>/
rm -f  tests/support/<dep>_stub.h   # only stubs specific to this module
```

**Regeneration applies to the module under construction only.** Files
belonging to other modules — including their mocks, stubs, and CMSIS
mock additions — must NOT be deleted. See "Reuse policy" below.

Then proceed with Step 3 onwards as if the module never existed.

---

## Reuse policy (prior-component artefacts)

Modules built in earlier sessions leave behind reusable test
infrastructure: stub headers, FreeRTOS shadow headers, CMSIS register
mocks, project.yml entries. **Reuse these.** Do not regenerate, copy,
or recreate them.

### Before writing any test infrastructure (Step 5 and Step 6)

1. **List what already exists:**

```bash
ls tests/support/
ls tests/mocks/
grep -E '^:test_' tests/project.yml
```

2. **For each consumed dependency of this module:**

   - **Check whether a stub already exists** in `tests/support/`.
   - If yes → **reuse it as-is** in the test TU `#include`. If the SUT
     calls a symbol the existing stub does not declare, **extend**
     that stub with the new declaration; do not create a parallel
     `<dep>_stub2.h` or rename it.
   - If no → create a new minimal stub per the existing naming
     convention (`<dep>_stub.h`).

3. **For CMSIS register access** (driver-layer modules only):

   - **Check whether `tests/mocks/stm32_cmsis_mock.{c,h}` and
     `tests/mocks/stm32f469xx.h` already provide the registers the SUT
     touches.**
   - If yes → reuse. Include `stm32_cmsis_mock.h` to auto-link.
   - If the SUT touches a register/peripheral the mock does not yet
     declare → **extend** the existing mock with the minimal
     additions. Do not create parallel mock files. Do not duplicate
     existing symbols.

4. **For project.yml:** add a new `:test_<module>:` block per the
   existing pattern. Do not delete or modify any other block.

### Audit before commit

After Step 8 and before Step 9, run:

```bash
git diff --stat origin/main -- tests/support/ tests/mocks/ tests/project.yml
```

Expected: only **additions** to these three locations. Modifications
must be confined to:

- `tests/support/<dep>_stub.h` files where new declarations were
  appended to support symbols this module needs.
- `tests/mocks/stm32_cmsis_mock.{c,h}` and
  `tests/mocks/stm32f469xx.h` where new register declarations or
  stubs were appended.
- `tests/project.yml` where a new `:test_<module>:` block was added.

If any line of an unrelated file was modified or deleted, revert it
before continuing.

### Existing infrastructure inventory

Re-check this list at session start in case it has grown:

- `tests/support/`:
  - `FreeRTOS.h`, `task.h`, `queue.h` — FreeRTOS shadow headers
  - `freertos_mock.h` / `freertos_mock.c` — FreeRTOS stubs
    (include `freertos_mock.h` to auto-link)
  - `driver_stubs.h` — rtc_get_time + debug_uart_send stubs
    (reuse if SUT calls these; extend with new symbols if needed)
  - `<dep>_stub.h` files for each driver already implemented in
    prior sessions
- `tests/mocks/`:
  - `stm32_cmsis_mock.h` / `stm32_cmsis_mock.c` — CMSIS peripheral
    register stubs for driver-layer tests
  - `stm32f469xx.h` — register definitions used by the SUT

---

## Fixed rules — apply to every module

### Code style (BARR-C:2018 subset)

- Snake_case for functions and variables; UPPER_CASE for macros and
  constants; `module_` prefix on all public symbols.
- Fixed-width integer types everywhere (`uint8_t`, `uint16_t`, etc.).
- Braces on every `if`/`else`/`while`/`for` body, even single-line.
- `const` correctness: pointers to data that must not change are `const`.
- No magic numbers — every numeric literal gets a named `#define`.
- No dynamic memory allocation — no `malloc`, `calloc`, `free`.
- No `printf`/`scanf` in firmware code. Debug output via Logger macros.
- Doxygen-style comments on all public API functions.
- Header guards: `#ifndef MODULE_NAME_H` / `#define` / `#endif`.
- Every function that can fail returns an error code or status enum.
  No silent failures.
- British spelling in comments and documentation.

### File layout

```
firmware/field-device/<layer>/<module>/
    <module>.h
    <module>.c

tests/field-device/<layer>/<module>/
    test_<module>.c

tests/support/
    <dep>_stub.h    (one per driver dependency, if not already present)

firmware/field-device/integration-tests/<module>/
    main_test_<module>.c
```

### Test isolation (critical — read carefully)

Ceedling auto-links `.c` files whose basename matches a `#include`d
header in the test TU. To prevent real driver `.c` files from being
pulled into middleware and application tests:

- The test TU must NOT `#include` real driver headers.
- Instead use the stub header `tests/support/<dep>_stub.h` declaring
  ONLY the symbols the SUT actually calls.
- The stub body implementations go inline in the test TU.
- The stub header basename must NOT match any real `.c` in the project.
  Name it `<dep>_stub.h`, not `<dep>.h`, unless an existing shadow
  header already exists in `tests/support/` (check first per Reuse
  policy).

### project.yml additions

For every new test TU, add a per-test defines block:

```yaml
:test_<module>:
  - STM32F469xx
  - <any other defines the SUT needs>
```

Add the test path to `:paths: :test:` if it is not already covered
by a wildcard.

### #ifdef TEST pattern

Test-only exposures (reset functions, internal type visibility,
test-hook functions) go in the public header under `#ifdef TEST`.
Implementations in the `.c` file are guarded with `#ifdef TEST` at
the function level. The `LOGGER_TEST_VISIBLE` / `RTC_TEST_VISIBLE`
macro pattern:

```c
#ifdef TEST
#  define <MODULE>_TEST_VISIBLE
#else
#  define <MODULE>_TEST_VISIBLE  static
#endif
```

---

## Session workflow

Execute these steps in order. Do not skip ahead. Do not stop short
of Step 13 except for the five legitimate halt conditions listed in
the Authorisation section.

### Step 0 — Create feature branch

```bash
cd D:\iot-environmental-monitor
git fetch origin
git switch main && git pull origin main
git switch -c feature/phase-4-fd-<module>
git branch --show-current
```

The last command must print `feature/phase-4-fd-<module>`.
**Do NOT proceed if it prints `main`.**

---

### Step 1 — Read the companion

Open `docs/lld/<layer>/<module>.md` (or the companion document
provided in this prompt).  Read §2 (public API), §3 (internal design),
and §7 (unit test plan) completely before writing any code.

---

### Step 2 — Verify HLD alignment

Check `docs/hld/components.md` for the module's PROVIDES / USES /
LAYER.  The implementation must not widen the USES footprint without
an explicit HLD escalation (add a comment and stop).

---

### Step 3 — Create the source files

```
firmware/field-device/<layer>/<module>/
    <module>.h
    <module>.c
```

Place the header and source in the appropriate layer directory per the
HLD component diagram.

---

### Step 4 — Register-level drivers (CMSIS only)

All drivers use CMSIS register definitions (`stm32f469xx.h`) directly.
STM32 HAL is forbidden.

**Key F469 specifics:**
- GPIO clocks are on `RCC->AHB1ENR` (not AHB2ENR like L475).
- SYSCLK target: 180 MHz.
- APB1 max: 45 MHz; APB2 max: 90 MHz.
- HSE crystal: 8 MHz on-board.
- Flash wait states: 5 WS at 180 MHz.
- SRAM: 384 KB; Flash: 2 MB.

Always verify the correct RCC enable register from the CMSIS header.

**Critical rule:** Enable the RCC clock for a peripheral **before** any
register access to that peripheral.

---

### Step 5 — Write tests alongside implementation

Test file: `tests/field-device/<layer>/<module>/test_<module>.c`

- Use Unity (`TEST_ASSERT_*` macros) and CMock.
- One test file per module.
- Cover: every public API function, happy path + at least one error
  case per function, boundary conditions.
- `<module>_reset_for_test(void)` hook guarded by `#ifdef TEST` to
  reset static state between tests.
- Never use `TEST_SOURCE_FILE` directives.
- Use `TEST_IGNORE_MESSAGE("deferred: <reason>")` for tests that
  require hardware.

---

### Step 6 — Integration test main

Create `firmware/field-device/integration-tests/<module>/main_test_<module>.c` 
containing a standalone `main()` that exercises the module on real hardware.

- Call `cpu_init()` first (clock, DWT, fault handlers).
- Initialise only the dependencies this module needs.
- Exercise every public API function with observable output
  (LED toggle, UART printf, debugger breakpoint).
- This file is NOT compiled by default — swap it for `main.c`
  manually in CubeIDE when doing hardware bring-up.
- Commit it alongside the module source.

---

### Step 7 — Update Ceedling project.yml

Add the new test source path to `tests/project.yml` under
`:paths:test:` and `:paths:source:`.  Ensure the module's source
directory is included so Ceedling can find it.

---

### Step 8 — Local verification

```powershell
# From the repo root in PowerShell:
.\scripts\test-module.ps1 -Module fd-<module>
```

This runs Ceedling + cppcheck + clang-format for the module.  All
three must pass before committing.

If `test-module.ps1` is not yet adapted for Field Device paths, run
manually:

```bash
# Ceedling
cd tests && ceedling test:test_<module> && cd ..

# cppcheck
cppcheck --enable=all --suppress=missingIncludeSystem \
  --suppressions-list=cppcheck-suppressions.txt \
  firmware/field-device/<layer>/<module>/

# clang-format
clang-format --dry-run --Werror \
  firmware/field-device/<layer>/<module>/*.c \
  firmware/field-device/<layer>/<module>/*.h
```

---

### Step 9 — Apply clang-format

Run `clang-format -i` on all new `.c` and `.h` files before the
final commit.  The CI check will reject unformatted code.

---

### Step 10 — Logical commits

Conventional commit prefixes **without** parenthesised scope:

- `feat: add <module> driver for Field Device`
- `test: add <module> unit tests`
- `docs: update <module> LLD companion`

One logical change per commit.  Interactive rebase before PR if
needed: `git rebase -i` then `git push --force-with-lease`.

---

### Step 11 — Create PR

```bash
git push -u origin feature/phase-4-fd-<module>
```

PR title: `feat: implement <Module> for Field Device`

PR description must include:

- Module purpose (one sentence).
- LLD companion reference.
- Test summary (number of TCs, any deferred).
- Checklist (see Step 12).

---

### Step 12 — Dev-tools deliverables

Switch to the `dev-tools` branch:

```bash
git switch dev-tools
```

Confirm you are on `dev-tools`:

```bash
git branch --show-current   # must print dev-tools
```

Create three files under `docs/dev-tools/<module>/`:

- `session-report.md` — what was accomplished, tests run, decisions made
- `bug-log.md` — bugs encountered and how to find them (if any)
- `exercise.md` — a technical exercise based on this module for interview prep

Then commit and push these files to `dev-tools` **only** — they do not go
on the feature branch:

```bash
git add docs/dev-tools/<module>/
git commit -m "chore: add <module> dev-tools (session report, bug log, exercise)"
git push origin dev-tools
```

Return to the feature branch:

```bash
git switch feature/phase-4-fd-<module> || { echo "ERROR: feature branch not found"; exit 1; }
git branch --show-current   # must print feature/phase-4-fd-<module>
```

---

#### File 1 — `docs/dev-tools/<module>/session-report.md`

Structure:

```
# Session Report — <Module>

**Date:** <date>
**Branch:** feature/phase-4-fd-<module>
**Companion:** docs/lld/<layer>/<module>.md

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/field-device/<layer>/<module>/<module>.h | N | |
| firmware/field-device/<layer>/<module>/<module>.c | N | |
| tests/support/<dep>_stub.h | N | new / reused / extended |
| tests/mocks/stm32_cmsis_mock.{c,h} | N | reused / extended |
| tests/field-device/<layer>/<module>/test_<module>.c | N | |
| firmware/field-device/integration-tests/<module>/main_test_<module>.c | N | |

---

## Reused infrastructure

List each existing file that was reused or extended in this session:

| File | Status | Symbols added (if extended) |
|------|--------|-----------------------------|
| tests/support/<dep>_stub.h | reused / extended | <list> |
| tests/mocks/stm32_cmsis_mock.h | reused / extended | <list> |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-XXX-001 | <description> | PASS / IGNORE |

**Total:** N pass, N ignored.

Ignored tests (with reason):
- TC-XXX-NNN: <reason>

---

## Integration test — expected behaviour

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | <exact observation> | <what it proves> |

---

## Deviations from companion

None.

---

## Open items

None.

---

## PR title

feat: <Module> — <one-line summary>

---

## PR description

## What this PR contains

- firmware/field-device/<layer>/<module>/<module>.h — <summary>
- firmware/field-device/<layer>/<module>/<module>.c — <summary>
- tests/field-device/<layer>/<module>/test_<module>.c — N unit tests
- firmware/field-device/integration-tests/<module>/main_test_<module>.c
- docs/lld/<layer>/<module>.md — companion updated to v1.0
- Extended stubs and mocks (if any): <list>

## Design decisions

- <decision 1 with rationale>

## Test evidence

All 6 CI checks green.
Unity host tests: N pass, 0 fail, N ignore.
Integration test validated on F469 hardware.

## Open items carried forward

- <item>
```

---

#### File 2 — `docs/dev-tools/<module>/bug-log.md`

```
# Bug Log — <Module>

## <bug category in one line>

**File:** firmware/field-device/<layer>/<module>/<module>.c
**Line:** <line number>
**Category:** <off-by-one | wrong constant | missing state-clear |
               wrong bit mask | race condition | wrong return value>

**What the code does:**
<one sentence>

**What it should do:**
<one sentence>

**Correct fix:**

    /* before */
    <buggy line>
    /* after */
    <corrected line>

**How to find it with a debugger:**
<step-by-step>

**Why it passes CI:**
<one sentence>
```

---

#### File 3 — `docs/dev-tools/<module>/exercise.md`

```
# Technical Exercise — <Module>

## Brief (3 minutes)

<Two paragraphs. <= 150 words.>

## Given files

### <module>_exercise.h
<header with function signature(s), types, constants, Doxygen spec>

### <module>_exercise.c (partial)
<scaffolding with TODO>

## Questions

Q1: <question>
Answer: <2-4 sentences>

Q2: <question>
Answer: <2-4 sentences>

Q3: <question>
Answer: <2-4 sentences>

## Model solution

<complete correct implementation>

## Marking guide

Must have:
- <criterion>

Good to have:
- <criterion>

Red flags:
- <criterion>
```

---

### Step 13 — Completion checklist

```bash
git switch feature/phase-4-fd-<module>
git status                                  # must be clean
git log --oneline main -- docs/dev-tools/   # must return nothing
```

If the last command returns any commits, remove the dev-tools files
from the feature branch before raising the PR:

```bash
git rm -r docs/dev-tools/
git commit -m "chore: remove dev-tools files from feature branch"
git push origin feature/phase-4-fd-<module>
```

Run the **completion checklist** — every item must be checked off
before the session ends:

- [ ] Step 8 reached ALL CHECKS PASSED
- [ ] Feature branch pushed to origin
- [ ] `tests/support/` and `tests/mocks/` show only **additive**
      changes (verified via `git diff --stat origin/main -- tests/support/ tests/mocks/`)
- [ ] No existing file in `tests/support/` or `tests/mocks/` had
      lines deleted or modified (only appended)
- [ ] dev-tools branch has the three files written in full (no TBDs)
- [ ] dev-tools branch pushed to origin
- [ ] Working tree is on the feature branch and clean
- [ ] No dev-tools files committed to the feature branch
- [ ] Session report contains the PR title and description ready to
      paste into the GitHub PR

Report each line of the checklist with its actual status (checked
or not) at the end of the session.

---

## What NOT to do

- Do not make architectural decisions not present in the companion.
- Do not add dependencies the companion does not list.
- Do not use STM32 HAL — CMSIS register access only.
- Do not use dynamic memory allocation.
- Do not use varargs (`...`) in any firmware function.
- Do not suppress cppcheck or clang-format findings silently.
- Do not skip the test script and claim tests will pass.
- Do not commit anything to `main` directly.
- Do not commit dev-tools files to the feature branch.
- Do not create `dev-tools` branch from `main` — it must already exist.
- Do not nest subdirectories inside `docs/dev-tools/<module>/`.
- Do not reuse existing files for the module under construction in
  regeneration mode — delete first.
- **Do not delete or rewrite existing stubs or mocks from prior
  components.** Extend them additively.
- **Do not create parallel stub or mock files** (`<dep>_stub2.h`,
  `stm32_cmsis_mock2.c`, etc.). One file per concern.
- **Do not stop short of Step 13** except for the five legitimate
  halt conditions in the Authorisation section.

---

## Companion document

[PASTE OR ATTACH THE COMPANION DOCUMENT BELOW THIS LINE]
