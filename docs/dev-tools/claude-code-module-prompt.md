# Claude Code — Module Implementation Session

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

## Authorisation

You have full authorisation to create, overwrite, delete, and commit
files anywhere in this repository without asking for confirmation at
each step. Do not pause to ask "shall I proceed?" — proceed. The only
exception is if the companion document contains an explicit ambiguity
that requires a design decision not already resolved in §3 or §8.
In that case, ask once, get the answer, then continue without further
interruption.

---

## Your role

You are implementing ONE module from its LLD companion document (attached
below). The companion is the authoritative source. Do not make
architectural decisions that contradict it.

---

## Regeneration mode

If files for this module already exist on disk, **delete them all first**
before writing anything new. Do not reuse, patch, or extend existing
files. Start from a clean slate every time.

```bash
# Delete all existing files for this module before starting
rm -rf firmware/field-device/<layer>/<module>/
rm -rf tests/field-device/<layer>/<module>/
rm -rf firmware/field-device/integration-tests/<module>/
rm -f  tests/support/<dep>_stub.h   # only stubs specific to this module
```

Then proceed with Step 3 onwards as if the module never existed.

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
- `StackType_t` arrays used as FreeRTOS task stacks must always carry `__attribute__((aligned(8)))` (AAPCS 8-byte SP alignment requirement).
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
    test_<module>_main.c
```

### Test isolation (critical — read carefully)

Ceedling auto-links `.c` files whose basename matches a `#include`d
header in the test TU. To prevent real driver `.c` files from being
pulled into middleware and application tests:

- The test TU must NOT `#include` real driver headers.
- Instead create `tests/support/<dep>_stub.h` declaring ONLY the
  symbols the SUT actually calls.
- The stub body implementations go inline in the test TU.
- The stub header basename must NOT match any real `.c` in the project.
  Name it `<dep>_stub.h`, not `<dep>.h`, unless an existing shadow
  header already exists in `tests/support/` (check first).

Existing stub/shadow headers in `tests/support/` (do not recreate):
- `FreeRTOS.h`, `task.h`, `queue.h` — FreeRTOS shadow headers
- `freertos_mock.h` / `freertos_mock.c` — FreeRTOS stubs
  (include `freertos_mock.h` in the test TU to auto-link)
- `driver_stubs.h` — rtc_get_time + debug_uart_send stubs
  (reuse if the SUT calls these; extend with new symbols if needed)
- `stm32_cmsis_mock.h` / `stm32_cmsis_mock.c` — CMSIS peripheral
  register stubs for driver-layer tests only
  (include `stm32_cmsis_mock.h` to auto-link)

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

Execute these steps in order. Do not skip ahead.

### Step 0 — Create feature branch

```bash
git fetch origin
git switch main && git pull origin main
git switch -c feature/phase-4-<module>
git branch --show-current
```

The last command must print `feature/phase-4-<module>`.
**Do NOT proceed if it prints `main`.**

---

### Step 1 — Read the companion

Read the companion document in full before writing a single line of
code. Confirm you understand:
- The public API (§2 of the companion).
- The internal design (§3).
- The unit test plan (§7 — TC-NNN cases).
- The open items (§8) — note which are pre-code decisions vs
  post-code validations.

---

### Step 2 — Clean slate

Delete any existing files for this module (see Regeneration mode above).
Then verify the target directories are empty or absent:

```bash
find firmware tests -path "*/<module>*" 2>/dev/null
```

The output must be empty before proceeding.

---

### Step 3 — Write the header

Write `firmware/field-device/<layer>/<module>/<module>.h`.
Include the `#ifdef TEST` block with reset and hook declarations.
Do not write the `.c` yet.

---

### Step 4 — Write the implementation

Write `firmware/field-device/<layer>/<module>/<module>.c`.
Follow the internal design in §3 of the companion exactly.
Flag any deviation with a comment: `/* DEVIATION from companion §X: ... */`

---

### Step 5 — Write stub headers

For each driver the SUT depends on, check if a stub header already
exists. If not, create `tests/support/<dep>_stub.h` with the minimal
declarations.

---

### Step 6 — Write the unit test file

Write `tests/field-device/<layer>/<module>/test_<module>.c`.
Implement every TC-NNN case from the companion §7.
Use `TEST_IGNORE_MESSAGE("TC-NNN: deferred — <reason>")` for any
TC that cannot be implemented now (e.g. depends on hardware or a
module not yet implemented). Do not leave empty test functions.

---

### Step 7 — Write the integration test

Write `firmware/field-device/integration-tests/<module>/test_<module>_main.c`.
Follow the pattern in
`firmware/field-device/integration-tests/logger/test_logger_main.c`:
- Call `system_clock_init()` first.
- Init all dependencies in order (see companion §1 init ordering).
- Run pre-scheduler diagnostics via Logger.
- Create a test task that exercises the module's behaviour.
- Include a visual checklist in the file header comment.

---

### Step 8 — Update project.yml

Add the `:test_<module>:` defines block.
Verify the test path is covered.

---

### Step 9 — Gate pass: LLD companion check

Before running the test script, verify the companion document is
marked ready for implementation. The companion §1 (or the header
metadata block) must show:

- Status: `Pass H` or `Implementation ready`
- All pre-code open items in §8 resolved

If the companion is not at Pass H, stop and report which open items
remain unresolved. Do not proceed to Step 10 until this is confirmed.

---

### Step 10 — Run the test script

```bash
cd ..
powershell -ExecutionPolicy Bypass -File scripts/test-module.ps1 -Module <module>
```

Fix errors iteratively. For each error:
- Read the full error message.
- Identify root cause (type mismatch, missing symbol, wrong include, etc.).
- Fix the minimal change that resolves it.
- Re-run.

Do not move to Step 11 until the script exits with ALL CHECKS PASSED.
If clang-format violations are found, re-run with `-Fix` to auto-correct:

```bash
powershell -ExecutionPolicy Bypass -File scripts/test-module.ps1 -Module <module> -Fix
```

Then re-run without `-Fix` to verify clean.

---

### Step 11 — Commit implementation to feature branch

Confirm you are on the feature branch:

```bash
git branch --show-current   # must print feature/phase-4-<module>
```

Commit in logical groups:

```bash
# Header + implementation
git add firmware/field-device/<layer>/<module>/
git commit -m "feat: add <Module> — <one-line summary>"

# Tests
git add tests/field-device/<layer>/<module>/
git add tests/support/<dep>_stub.h        # if new
git add tests/project.yml
git commit -m "test: add Unity unit tests for <Module>"

# Integration test
git add firmware/field-device/integration-tests/<module>/
git commit -m "test: add integration test harness for <Module>"
```

Use `type:` prefix (feat, fix, test, docs, style, refactor).
British spelling in commit messages.

Push the feature branch:

```bash
git push -u origin feature/phase-4-<module>
```

---

### Step 12 — Private-branch deliverables

All post-session files go on the `dev-tools` branch, **never on main**.
This branch is a permanent private accumulator — it never merges into main.
All three files for this module go in a **single flat directory**:
`docs/dev-tools/<module>/` — no subdirectories within it.

Switch to dev-tools (it must already exist remotely):

```bash
git fetch origin
git switch dev-tools
git pull origin dev-tools
```

If `dev-tools` does not exist locally or remotely, stop and report
the error. Do NOT create it from main.

Create the module directory and write the three files:

```bash
mkdir -p docs/dev-tools/<module>
```

Write `session-report.md`, `bug-log.md`, and `exercise.md` directly
into `docs/dev-tools/<module>/`. No further nesting.

Commit and push:

```bash
git add docs/dev-tools/<module>/
git commit -m "chore: add <module> dev-tools (session report, bug log, exercise)"
git push origin dev-tools
```

Return to the feature branch:

```bash
git switch feature/phase-4-<module> || { echo "ERROR: feature branch not found"; exit 1; }
git branch --show-current   # must print feature/phase-4-<module>
```

---

#### File 1 — `docs/dev-tools/<module>/session-report.md`

Structure:

```
# Session Report — <Module>

**Date:** <date>
**Branch:** feature/phase-4-<module>
**Companion:** docs/lld/companions/<module>.md

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/field-device/<layer>/<module>/<module>.h | N | |
| firmware/field-device/<layer>/<module>/<module>.c | N | |
| tests/support/<dep>_stub.h | N | new / reused |
| tests/field-device/<layer>/<module>/test_<module>.c | N | |
| firmware/field-device/integration-tests/<module>/test_<module>_main.c | N | |

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
- firmware/field-device/integration-tests/<module>/test_<module>_main.c
- docs/lld/companions/<module>.md — companion updated to v1.0

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

### Step 13 — Verify branch state

```bash
git switch feature/phase-4-<module>
git status                               # must be clean
git log --oneline main -- docs/dev-tools/  # must return nothing
```

If the last command returns any commits, remove the dev-tools files
from the feature branch before raising the PR:

```bash
git rm -r docs/dev-tools/
git commit -m "chore: remove dev-tools files from feature branch"
git push origin feature/phase-4-<module>
```

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
- Do not reuse existing module files in regeneration mode — delete first.

---

## Companion document

[PASTE OR ATTACH THE COMPANION DOCUMENT BELOW THIS LINE]
