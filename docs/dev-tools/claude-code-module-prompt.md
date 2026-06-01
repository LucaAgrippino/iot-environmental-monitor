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

## Your role

You are implementing ONE module from its LLD companion document. The
companion is attached or pasted below. It is the authoritative source.
Do not make architectural decisions that contradict it. If something in
the companion is ambiguous, pause and ask before writing code.

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

### Step 1 — Read the companion

Read the companion document in full before writing a single line of
code. Confirm you understand:
- The public API (§2 of the companion).
- The internal design (§3).
- The unit test plan (§7 — TC-NNN cases).
- The open items (§8) — note which are pre-code decisions vs
  post-code validations.

### Step 2 — Check existing files

```bash
find firmware tests -name "<module>*" 2>/dev/null
find tests/support -name "*stub*" -o -name "freertos*" 2>/dev/null
cat tests/project.yml
```

Do not overwrite files that already exist unless explicitly told to.

### Step 3 — Write the header

Write `firmware/field-device/<layer>/<module>/<module>.h`.
Include the `#ifdef TEST` block with reset and hook declarations.
Do not write the `.c` yet.

### Step 4 — Write the implementation

Write `firmware/field-device/<layer>/<module>/<module>.c`.
Follow the internal design in §3 of the companion exactly.
Flag any deviation with a comment: `/* DEVIATION from companion §X: ... */`

### Step 5 — Write stub headers

For each driver the SUT depends on, check if a stub header already
exists. If not, create `tests/support/<dep>_stub.h` with the minimal
declarations.

### Step 6 — Write the test file

Write `tests/field-device/<layer>/<module>/test_<module>.c`.
Implement every TC-NNN case from the companion §7.
Use `TEST_IGNORE_MESSAGE("TC-NNN: deferred — <reason>")` for any
TC that cannot be implemented now (e.g. depends on hardware or a
module not yet implemented). Do not leave empty test functions.

### Step 7 — Update project.yml

Add the `:test_<module>:` defines block.
Verify the test path is covered.

### Step 8 — Build and fix loop

```bash
cd tests
ceedling test:test_<module>
```

Fix errors iteratively. For each error:
- Read the full error message.
- Identify root cause (type mismatch, missing symbol, wrong include, etc).
- Fix the minimal change that resolves it.
- Re-run.

Do not move to Step 9 until all tests pass (or are explicitly
`TEST_IGNORE_MESSAGE`'d with a documented reason).

### Step 9 — Static analysis

```bash
cppcheck --enable=style,warning,performance \
  --suppress=missingIncludeSystem \
  --inline-suppr \
  firmware/field-device/<layer>/<module>/<module>.c \
  firmware/field-device/<layer>/<module>/<module>.h
```

Fix any findings. Do not suppress a finding without a comment
explaining why.

### Step 10 — Format

```bash
clang-format -i firmware/field-device/<layer>/<module>/<module>.c
clang-format -i firmware/field-device/<layer>/<module>/<module>.h
```

Check the diff. Commit format changes as a separate commit from
implementation changes.

### Step 11 — Write the integration test

Write `firmware/field-device/integration-tests/<module>/test_<module>_main.c`.
Follow the pattern in `firmware/field-device/integration-tests/logger/test_logger_main.c`:
- Call `system_clock_init()` first.
- Init all dependencies in order (see companion §1 init ordering).
- Run pre-scheduler diagnostics via Logger.
- Create a test task that exercises the module's behaviour.
- Include a visual checklist in the file header comment.

### Step 12 — Private-branch deliverables

All post-session files go on the `dev-tools` branch, never on `main`.
This branch is a permanent private accumulator — it never merges into
`main`. One subdirectory per module.

**First, ensure the branch exists and is up to date:**

```bash
git fetch origin
git switch dev-tools 2>/dev/null || git switch -c dev-tools origin/main
```

Create the module subdirectory:

```bash
mkdir -p docs/dev-tools/<module>
```

Write the three files described below, then commit and push:

```bash
git add docs/dev-tools/<module>/
git commit -m "chore: add <module> dev-tools (bug log, exercise, session report)"
git push origin dev-tools
git switch feature/phase-4-<module>   # return to the working branch
```

---

#### File 1 — `docs/dev-tools/<module>/session-report.md`

This is the primary post-session deliverable. It contains everything
needed to raise the PR and validate the hardware test without reading
any other file. Structure:

```markdown
# Session Report — <Module>

**Date:** <date>
**Branch:** `feature/phase-4-<module>`
**Companion:** `docs/lld/companions/<module>.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/<layer>/<module>/<module>.h` | N | |
| `firmware/field-device/<layer>/<module>/<module>.c` | N | |
| `tests/support/<dep>_stub.h` | N | new / reused |
| `tests/field-device/<layer>/<module>/test_<module>.c` | N | |
| `firmware/field-device/integration-tests/<module>/test_<module>_main.c` | N | |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-XXX-001 | <description> | PASS / IGNORE |
| ... | | |

**Total:** N pass, N ignored.

Ignored tests (with reason):
- TC-XXX-NNN: <reason — e.g. "requires hardware peripheral not available on host">

---

## Integration test — expected behaviour

What to observe on the terminal and on the board when
`test_<module>_main.c` is flashed. Be specific — exact LED states,
exact UART output lines, exact timing. This is the pass/fail checklist
for the hardware validation step.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | <exact observation> | <what it proves> |
| 2 | | |
| ... | | |

---

## Deviations from companion

List any place the implementation differs from the companion document.
Format: companion says X, implementation does Y, reason Z.
If none: write "None."

---

## Open items

List anything that needs a decision in the next chat session before
the PR can be raised, or that is deferred to a future module.
If none: write "None."

---

## Commit messages

One block per commit, in order. Paste each into the editor when
running `git commit` (no `-m` flag).

### Commit 1
\`\`\`
<type>: <subject line>

<body>
\`\`\`

### Commit 2
\`\`\`
...
\`\`\`

---

## PR description

Title: `<type>: <module> — <one-line summary>`

Body:
\`\`\`markdown
## Summary
...

## What is in this PR
| Commit | Files | Description |
...

## Architecture decisions
...

## Test evidence
...

## Open items
...

## Requirement traceability
| Requirement | Satisfied by |
...
\`\`\`
```

---

#### File 2 — `docs/dev-tools/<module>/bug-log.md`

```markdown
# Bug Log — <Module>

## <module> — <bug category in one line>

**File:** `firmware/field-device/<layer>/<module>/<module>.c`
**Line:** <line number>
**Category:** <off-by-one | wrong constant | missing state-clear |
               wrong bit mask | race condition | wrong return value>

**What the code does:**
<one sentence>

**What it should do:**
<one sentence>

**Correct fix:**
\`\`\`c
/* before */
<buggy line>
/* after */
<corrected line>
\`\`\`

**How to find it with a debugger:**
<step-by-step: what to observe on hardware, which variable or register
to watch, at what point in execution the symptom becomes visible>

**Why it passes CI:**
<one sentence>
```

---

#### File 3 — `docs/dev-tools/<module>/exercise.md`

```markdown
# Technical Exercise — <Module>

## Brief (3 minutes)

<Two paragraphs. First: what the module does and why it exists.
Second: what to implement and what constraints apply. ≤ 150 words.>

## Given files

### `<module>_exercise.h`
<header with function signature(s), types, constants, Doxygen spec>

### `<module>_exercise.c` (partial)
<scaffolding: includes, helpers already written, function stub with TODO>

## Questions

**Q1:** <question>
*Answer:* <2–4 sentences>

**Q2:** <question>
*Answer:* <2–4 sentences>

**Q3:** <question>
*Answer:* <2–4 sentences>

## Model solution

\`\`\`c
<complete correct implementation with inline comments>
\`\`\`

## Marking guide

**Must have:**
- <criterion>

**Good to have:**
- <criterion>

**Red flags:**
- <criterion>
```

---

### Step 13 — Verify private branch state

After committing to `dev-tools`, confirm the files are there and
`main` is clean:

```bash
git switch dev-tools
ls docs/dev-tools/<module>/
git switch feature/phase-4-<module>
git log --oneline main -- docs/dev-tools/   # must return nothing
```

If the last command returns any commits, the dev-tools files were
accidentally committed to a branch that will merge to `main`. Remove
them before raising the PR:

```bash
git rm -r docs/dev-tools/
git commit -m "chore: remove dev-tools files from feature branch (belong on dev-tools branch)"
```

---

## Intentional bug (mandatory)

Every module implementation MUST contain exactly one intentional bug.
This is a deliberate part of the project — it gives the developer a
realistic debugging exercise on real hardware.

### Rules for the bug

1. **One bug, one module.** No more, no less.
2. **Realistic category.** Choose from:
   - Off-by-one in a buffer size, index, or loop bound.
   - Wrong timeout, retry count, or threshold constant (e.g. 3 instead
     of 4, or 200 instead of 2000).
   - Missing state-clear before returning from an error path (e.g.
     flag left set, counter not reset).
   - Incorrect register bit mask or shift — off by one bit, wrong
     field width, or wrong register entirely (driver-layer only).
   - Race condition in shared-state access across tasks — wrong
     critical-section boundary, or critical section omitted entirely
     for a variable that needs it.
   - Wrong return value on a specific error branch (returns OK when it
     should return an error, or vice versa).
3. **Must pass CI.** The bug must NOT be caught by the unit tests or
   cppcheck. Either it lives in a path the test mocks hide, or it only
   manifests on real hardware under specific timing or data conditions.
   If your chosen bug would be caught by the unit tests, pick a
   different one.
4. **Must be findable** with a debugger + the companion document +
   the board datasheet, without external hints. It must be diagnosable
   in a single debug session (30–60 minutes for someone who knows the
   module).
5. **Do NOT announce it.** Do not put a comment near the bug, do not
   name the variable in a way that hints, do not mention it in the
   Step 12 report. The bug is hidden. The only record is in the
   `bug-log.md` file described below.

### Bug log

After writing all code, append an entry to
`docs/dev-tools/bug-log.md` (create the file if it does not exist).
The log is the answer key — it must NOT be committed until after the
developer has found and fixed the bug.

Entry format:

```markdown
## <module> — <bug category in one line>

**File:** `firmware/field-device/<layer>/<module>/<module>.c`
**Line:** <line number in the file as written>
**Category:** <off-by-one | wrong constant | missing state-clear |
               wrong bit mask | race condition | wrong return value>

**What the code does:**
<one sentence describing what the buggy line actually does>

**What it should do:**
<one sentence describing the correct behaviour>

**Correct fix:**
```c
/* before */
<buggy line>
/* after */
<corrected line>
```

**How to find it with a debugger:**
<step-by-step: what to observe on hardware, which variable or register
to watch, at what point in execution the symptom becomes visible>

**Why it passes CI:**
<one sentence explaining why the unit tests and cppcheck do not catch it>
```

---

## Interview test (mandatory)

Every module session MUST produce a self-contained interview test.
This is used as a technical screening exercise for mid-senior embedded
C roles — the kind handed to a candidate during a 40–50 minute
interview (presentation + introductions + questions + coding = 40–50
minutes total, so the coding portion is 20–25 minutes).

### Rules for the test

1. **One function to implement.** Give the candidate a `.h` file with
   the function signature, a partial `.c` file with the scaffolding
   and any helper stubs they need, and a short problem statement.
   The function must be representative of the module — not a toy
   example, not trivial string manipulation.
2. **Solvable in 20–25 minutes** by a mid-senior embedded engineer
   who has just read a 3-minute brief. No deep algorithm knowledge
   required. The difficulty comes from correctness under constraints
   (edge cases, error handling, bit-level correctness), not from
   algorithmic complexity.
3. **Three targeted follow-up questions.** These are asked verbally
   after the candidate submits their code. They probe understanding
   of the design choice, not just the implementation. Each has a
   model answer.
4. **A model solution.** The complete correct implementation, with
   commentary explaining each non-obvious decision.
5. **A marking guide.** What to look for (must-haves, nice-to-haves,
   red flags).

### Output format

Write the interview test to
`docs/dev-tools/interview-tests/<module>-test.md`.

Structure:

```markdown
# Interview Test — <Module> (<estimated time> minutes)

## Brief (read this first — 3 minutes)

<Two short paragraphs. First: what the module does and why it exists
in the system. Second: what the candidate must implement and what
constraints apply. No more than 150 words total.>

## Files given to the candidate

### `<module>_exercise.h`
<The header with the function signature(s) to implement, types, and
any constants they need. Doxygen comment on each function — this is
the spec the candidate codes against.>

### `<module>_exercise.c` (partial)
<The scaffolding: includes, any helper functions already written,
the function stub with a TODO comment. The candidate fills in the
body. Do not give away the algorithm — give only what they cannot
reasonably be expected to produce from scratch in the time.>

## Follow-up questions

**Q1:** <question>
*Model answer:* <answer — 2–4 sentences>

**Q2:** <question>
*Model answer:* <answer — 2–4 sentences>

**Q3:** <question>
*Model answer:* <answer — 2–4 sentences>

## Model solution

```c
<complete correct implementation with inline comments>
```

## Marking guide

**Must have (pass/fail):**
- <criterion>
- <criterion>

**Nice to have (differentiates mid from senior):**
- <criterion>
- <criterion>

**Red flags (automatic fail or strong negative signal):**
- <criterion>
- <criterion>
```

---

## What NOT to do

- Do not make architectural decisions not present in the companion.
- Do not add dependencies the companion does not list.
- Do not use STM32 HAL — CMSIS register access only.
- Do not use dynamic memory allocation.
- Do not use varargs (`...`) in any firmware function (use the
  `LOG_*` macro pattern if logging with format strings is needed).
- Do not suppress cppcheck or clang-format findings silently.
- Do not skip the build-and-fix loop and claim tests will pass.

---

## Companion document

[PASTE OR ATTACH THE COMPANION DOCUMENT BELOW THIS LINE]
