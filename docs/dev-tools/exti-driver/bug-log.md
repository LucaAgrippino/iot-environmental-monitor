# Bug Log — ExtiDriver

## Non-recursive Ceedling source glob silently excluded all of firmware/shared/'s subdirectories

**File:** tests/project.yml
**Line:** `:source:` block, `../firmware/shared/` entry
**Category:** wrong constant (glob pattern missing recursion)

**What the code does:**
`../firmware/shared/` (no trailing `**`) tells Ceedling to add only the
literal `firmware/shared/` directory as a source path — it never
descends into subdirectories, so any `.c` file placed one level deeper
(e.g. `firmware/shared/drivers/exti/exti_driver.c`) is invisible to the
test build.

**What it should do:**
Match every other multi-directory source entry in the same file (e.g.
`../firmware/gateway/drivers/**`) and recurse, so new shared modules
placed in their own subdirectory are picked up automatically.

**Correct fix:**

    /* before */
    - ../firmware/shared/
    /* after */
    - ../firmware/shared/**

**How to find it with a debugger:**
Not a runtime bug — it manifests at build time. Running
`ceedling test:test_exti_driver_gw` before the fix fails immediately at
the preprocessor step with `fatal error: exti_driver.h: No such file or
directory`, even though the file exists on disk and the test correctly
`#include`s it. Comparing the compiler invocation Ceedling prints (the
`-I` list) against the actual file path shows `firmware/shared` is
present but `firmware/shared/drivers/exti` is not — the giveaway that
the `:source:` glob isn't recursing.

**Why it passes CI:**
It didn't — this was caught in this same session, before any commit,
because `firmware/shared/` had no subdirectories until ExtiDriver
introduced one. Every module before it either lived directly in
`firmware/shared/` (just `firmware_version.h`, a header with no `.c`
file to compile) or under a `**`-globbed board-specific tree, so the
non-recursive entry never mattered until now.
