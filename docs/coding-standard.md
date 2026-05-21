# Coding Standard

The authoritative standard for this project is **[BARR-C:2018](https://barrgroup.com/coding-standard)**
(Embedded C Coding Standard, Barr Group, 2018 edition). All firmware source files must conform
to BARR-C:2018 except where an explicit exception is recorded in the Exceptions section below.

## Project subset

The following BARR-C rules are the minimum enforced subset for this project:

- `snake_case` for all function and variable names.
- `UPPER_CASE` for all constants and preprocessor macros.
- Fixed-width integer types (`uint8_t`, `int32_t`, etc.) for all numeric variables where bit width matters.
- Module prefix on every public symbol (e.g. `gpio_init()`, `LOGGER_LEVEL_DEBUG`).
- Braces always — required on every `if`, `else`, `for`, `while`, and `do` body, even single-line.
- No dynamic memory allocation after initialisation completes (no `malloc`/`free` in steady-state code).
- Doxygen documentation block on every public API function and type.
- Header guards in every `.h` file (`#ifndef MODULE_H`, `#define MODULE_H`, `#endif`).
- Error-code return values from every function that can fail; callers must check them.

## Enforcement

Three tooling layers enforce conformance:

- **`.clang-format`** — formatting rules (indentation, brace placement, line length). Applied
  automatically in the editor and checked in CI (`clang-format --dry-run --Werror`). No manual
  formatting disputes: the formatter is the authority.
- **`cppcheck-suppressions.xml`** — records accepted static-analysis exceptions. Any suppression
  added here requires a code comment explaining the justification. CI runs `cppcheck --enable=all`
  against all firmware source.
- **PR review** — naming conventions, design rules (module prefix, Doxygen completeness, no
  post-init allocation), and architectural constraints not expressible in tooling are checked
  during pull-request review.

## Exceptions

*(None recorded. Add entries here as justified exceptions arise during implementation,
referencing the BARR-C rule number and the reason.)*
