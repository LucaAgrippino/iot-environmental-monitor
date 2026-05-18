# LLD Companion — Logger

**Board:** Both (Field Device + Gateway)  
**Branch:** `feature/lld-logger`  
**Status:** Draft  
**Methodology:** lld-methodology.md v1.1, steps 1–8  

---

## 1. Scope

Logger is a cross-cutting Middleware component consumed by virtually every
Application and Middleware component on both boards. It formats
severity-tagged, timestamped log entries and dispatches them to the debug
UART output sink (REQ-NF-500).

| Aspect | Value |
|--------|-------|
| Boards | Both |
| PROVIDES | ILogger |
| USES | DebugUartDriver, RtcDriver |
| Root req | REQ-NF-500, REQ-NF-504 |
| Hosting task | Called from any task; never from ISR context |
| Mutex | `logger_mutex` (task-breakdown.md §7, hold < 2 ms) |

---

## 2. Source references (Step 1)

| Source | Relevant section |
|--------|-----------------|
| `components.md` preamble | Bootstrap exception: Logger uses RtcDriver directly |
| `components.md` Logger entry | PROVIDES ILogger, USES DebugUartDriver + RtcDriver |
| `SRS.md` | REQ-NF-500 (severity + timestamp + source), REQ-NF-504 (diagnostic output channel) |
| `task-breakdown.md` §7 | logger_mutex; hold duration target < 2 ms |
| `architecture-principles.md` | P4 (cross-cutting concern) |
| `rtc-driver.md` | IRtc.get_time() return type; uptime fallback |
| `debug-uart-driver.md` | IDebugUart.write() blocking behaviour |

---

## 3. API — Step 2

### 3.1 ILogger

```c
/* logger.h */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stdarg.h>

typedef enum {
    LOG_LEVEL_ERROR = 0U,
    LOG_LEVEL_WARN  = 1U,
    LOG_LEVEL_INFO  = 2U,
    LOG_LEVEL_DEBUG = 3U
} log_level_t;

typedef enum {
    LOGGER_ERR_OK          = 0,
    LOGGER_ERR_NOT_INIT    = 1,
    LOGGER_ERR_INVALID_ARG = 2
} logger_err_t;

logger_err_t logger_init(log_level_t initial_level);
void         logger_set_level(log_level_t level);
void         logger_log(log_level_t level, const char *module,
                        const char *fmt, ...);

/* Convenience macros — compile to no-ops when LOG_DISABLE is defined */
#ifndef LOG_DISABLE
#define LOG_ERROR(mod, fmt, ...)  logger_log(LOG_LEVEL_ERROR, (mod), (fmt), ##__VA_ARGS__)
#define LOG_WARN(mod, fmt, ...)   logger_log(LOG_LEVEL_WARN,  (mod), (fmt), ##__VA_ARGS__)
#define LOG_INFO(mod, fmt, ...)   logger_log(LOG_LEVEL_INFO,  (mod), (fmt), ##__VA_ARGS__)
#define LOG_DEBUG(mod, fmt, ...)  logger_log(LOG_LEVEL_DEBUG, (mod), (fmt), ##__VA_ARGS__)
#else
#define LOG_ERROR(mod, fmt, ...)  ((void)0)
#define LOG_WARN(mod, fmt, ...)   ((void)0)
#define LOG_INFO(mod, fmt, ...)   ((void)0)
#define LOG_DEBUG(mod, fmt, ...)  ((void)0)
#endif

#endif /* LOGGER_H */
```

Callers use the macros, never `logger_log()` directly. This allows
compile-time removal of all log calls in production builds without changing
call sites.

### 3.2 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| DebugUartDriver | Yes | Yes — `debug_uart_write()` for output |
| RtcDriver | Yes (bootstrap exception) | Yes — `rtc_get_time()` for timestamp |

**Bootstrap exception rationale (P1 / P2):** TimeProvider depends on
ILogger; if Logger depended on ITimeProvider, there would be a circular
dependency. Logger therefore calls RtcDriver directly. This is the only
documented exception to the rule that Middleware uses interfaces, not
concrete drivers. Recorded in `components.md` preamble.

**P4 (cross-cutting concern):** Logger is referenced concretely in USES
lists throughout both boards. This is consistent with P4 — Logger is
infrastructure, not a domain component. Its concrete reference is accepted
and documented.

---

## 4. Internal design (Step 3)

### 4.1 Module structure

```
logger.h     — public API (ILogger) + convenience macros
logger.c     — singleton state, format engine, mutex, UART output
```

### 4.2 Singleton state

```c
typedef struct {
    log_level_t  level;          /* current filter level     */
    bool         initialised;
} logger_state_t;

static logger_state_t s_log;
static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;

/* Static format buffer — never heap-allocated */
static char s_buf[256];
```

`s_buf` is protected by `s_mutex`. No caller may hold a pointer to
`s_buf` after `logger_log()` returns.

### 4.3 Format

Every log line follows this fixed layout:

```
[LEVEL][HH:MM:SS.mmm][module         ] message\r\n
```

- **LEVEL**: 5-char padded — `ERROR`, ` WARN`, ` INFO`, `DEBUG`
- **Timestamp**: `HH:MM:SS.mmm` from RtcDriver. If RtcDriver returns
  a not-set status, falls back to `xTaskGetTickCount()` formatted as
  uptime milliseconds: `T+xxxxxxx`
- **module**: left-aligned, padded to 15 chars, truncated if longer
- **message**: formatted by `vsnprintf` into remainder of `s_buf`

Maximum line length: 256 bytes including `\r\n\0`. If the formatted
message would exceed the buffer, it is truncated with `...` at byte 252.

### 4.4 `logger_log()` flow

```
1. Check initialised + level filter → return immediately if filtered out
2. xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10))
     → if timeout: drop entry, return (never block indefinitely)
3. rtc_get_time(&t)
     → if RTC not set: use xTaskGetTickCount() as uptime
4. vsnprintf(s_buf, sizeof(s_buf), formatted header + fmt, args)
5. Append \r\n; truncate to 254 chars if needed
6. debug_uart_write((uint8_t *)s_buf, len)   /* blocking, polling UART */
7. xSemaphoreGive(s_mutex)
```

Step 2 timeout (10 ms): Logger never deadlocks. If a higher-priority task
holds the mutex for > 10 ms (a WCET violation), the log entry is dropped
silently. This is correct — logging must never block the system.

### 4.5 Pre-scheduler usage

`logger_init()` may be called before `vTaskStartScheduler()`. Before the
scheduler runs, `xSemaphoreTake` / `xSemaphoreGive` are not safe. Two
approaches are used:

1. `logger_init()` is called before the scheduler; the mutex is created
   with `xSemaphoreCreateMutexStatic()` which is safe pre-scheduler.
2. Before `vTaskStartScheduler()`, direct calls to `logger_log()` bypass
   the mutex (flag `s_log.pre_scheduler = true`), since no concurrency
   exists at that point.

This allows startup logging from `board_init()` before any task runs.

---

## 5. Hardware contract (Step 4)

Logger has no direct hardware access. It delegates to:

- **DebugUartDriver** — `debug_uart_write(buf, len)`: blocking,
  polling-mode UART write. Hold time per call ≤ 256 bytes at 115200 baud
  ≈ 22 ms worst case. This is within the logger_mutex < 2 ms target only
  for short messages. For a full 256-byte line at 115200 baud the actual
  hold is ~22 ms. **LOG-O1**: verify this is acceptable against the
  schedulability check or reduce baud rate / buffer size.

- **RtcDriver** — `rtc_get_time(&t)`: returns immediately (register read,
  no blocking).

---

## 6. Sequence integration (Step 5)

Logger is passive — called synchronously from the caller's task context.
No task owns Logger. The mutex serialises concurrent callers.

```
Any task (e.g. SensorTask)
  → LOG_ERROR("SensorService", "Read failed: %d", err)
      → logger_log(LOG_LEVEL_ERROR, "SensorService", "Read failed: %d", err)
          → mutex acquire (10 ms timeout)
          → rtc_get_time() → format timestamp
          → vsnprintf → s_buf
          → debug_uart_write(s_buf, len)   /* blocks until TX complete */
          → mutex release
```

**Logger is never called from an ISR.** ISR handlers are kept to the
minimum ISR contract (acknowledge, notify, return). Any logging triggered
by an ISR event happens in the owning task after the notification is
received, not in the ISR itself.

---

## 7. Error handling (Step 6)

| Condition | Response |
|-----------|----------|
| `logger_log()` called before `logger_init()` | Return immediately; no output, no crash |
| Mutex timeout (> 10 ms wait) | Drop log entry silently; no return value |
| `vsnprintf` truncation | Append `...` at byte 252; always null-terminate |
| RtcDriver not set | Substitute uptime timestamp; log entry still produced |
| `module` or `fmt` is NULL | Substitute `"?"` for NULL module; skip format for NULL fmt |

Logger has no error return from `logger_log()` — it is `void`. Callers
must never depend on a log call succeeding.

---

## 8. Test plan (Step 7)

Host-platform tests (Unity). Mock `debug_uart_write` captures output into
a test buffer. Mock `rtc_get_time` returns a fixed time or a not-set flag.

| Test ID | Scenario | Expected |
|---------|----------|----------|
| LOG-T01 | Nominal INFO log | Output matches `[ INFO][00:01:23.000][MyModule       ] Hello\r\n` |
| LOG-T02 | Level filter: set to WARN, call LOG_INFO | No output |
| LOG-T03 | Level filter: set to WARN, call LOG_ERROR | Output produced |
| LOG-T04 | RTC not set | Timestamp field shows uptime e.g. `[T+0001234]` |
| LOG-T05 | Message exactly 256 bytes | Output truncated to 254 chars + `...` + `\r\n` |
| LOG-T06 | NULL module | Module field shows `?` |
| LOG-T07 | Call before `logger_init()` | No crash, no output |
| LOG-T08 | `logger_set_level(LOG_LEVEL_DEBUG)` then LOG_DEBUG | Output produced |
| LOG-T09 | Pre-scheduler call (s_log.pre_scheduler = true) | Output produced without mutex |
| LOG-T10 | Module name > 15 chars | Truncated to 15 chars in output |

Test file: `tests/middleware/test_logger.c`.

---

## 9. Open items and decisions log (Step 8)

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| LOG-D1 | RtcDriver used directly, not via ITimeProvider | Avoids circular dependency: TimeProvider depends on ILogger. Documented as bootstrap exception in components.md preamble. |
| LOG-D2 | 10 ms mutex timeout → drop on timeout | Logger must never block the system. A dropped log entry is better than a priority inversion or deadlock. |
| LOG-D3 | `void` return from `logger_log()` | Callers must not branch on log success. Log calls are fire-and-forget. |
| LOG-D4 | `LOG_DISABLE` macro eliminates all call sites | Production builds can strip all logging with a single compile flag. The macro approach means zero overhead — no function call, no argument evaluation. |
| LOG-D5 | 256-byte static buffer | Matches the largest expected log line. No heap. Buffer is module-private and mutex-protected. |
| LOG-D6 | Pre-scheduler bypass (no mutex) | `vTaskStartScheduler()` has not run; no concurrency exists. Direct UART write is safe and necessary for boot diagnostics. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| LOG-O1 | At 115200 baud, a full 256-byte log line takes ~22 ms to transmit. This exceeds the logger_mutex < 2 ms hold-duration target in task-breakdown.md §7. Either accept the longer hold (low-priority tasks only affected), increase baud rate (230400/921600), or reduce `s_buf` to 64 bytes. | Luca | Decide at integration. Recommended: increase baud to 921600 (reduces 256-byte hold to ~2.8 ms). |
| LOG-O2 | `debug_uart_write()` API: the exact signature (blocking vs callback) depends on DebugUartDriver LLD companion. Confirm it is blocking/polling before finalising §4.4. | Luca | Confirm when DebugUartDriver companion is written. |

---

*This document is the LLD companion for Logger. It is authored by
Luca Agrippino and reviewed by the project mentor.*
