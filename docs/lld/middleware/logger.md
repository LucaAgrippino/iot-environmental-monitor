# LLD Companion — Logger

**Board:** Both (Field Device + Gateway)
**Branch:** `feature/phase-4-logger`
**Status:** Pass H complete
**Methodology:** lld-methodology.md v1.1, steps 1–8
**Version:** 0.3
**Date:** May 2026

**HLD anchor:** Logger in `components.md` (FD + GW middleware layer)

---

## 1. Sources

Logger is a cross-cutting Middleware component consumed by virtually every
Application and Middleware component on both boards. It formats
severity-tagged, timestamped log lines and dispatches them to the debug-UART
output sink (REQ-NF-500). Producers never touch the UART directly: a single
drain task owns the UART write, decoupling every caller from the multi-
millisecond transmission time.

| Aspect | Value |
|--------|-------|
| Boards | Both |
| PROVIDES | ILogger |
| USES | DebugUartDriver, RtcDriver |
| Root req | REQ-NF-500, REQ-NF-504 |
| Producers | Any task (via macros); ISRs (via the dedicated ISR entry point) |
| Owns | One FreeRTOS queue + one drain task (created in `logger_init`) |

### 1.1 Source references

| Source | Relevant section |
|--------|-----------------|
| `components.md` preamble | Bootstrap exception: Logger uses RtcDriver directly |
| `components.md` Logger entry | PROVIDES ILogger; USES DebugUartDriver + RtcDriver |
| `SRS.md` | REQ-NF-500 (severity + timestamp + source), REQ-NF-504 (diagnostic output channel) |
| `task-breakdown.md` | Drain task + queue must be added; `logger_mutex` removed (LOG-D2) |
| `architecture-principles.md` | P4 (cross-cutting infrastructure) |
| `rtc-driver.md` | `IRtc.get_time()` return type and behaviour |
| `debug-uart-driver.md` | `debug_uart_send()` blocking behaviour |

---

## 2. Public API

### 2.1 `logger.h`

```c
#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>   /* snprintf — used inside the LOG_* macros */

/* ---------------------------------------------------------------------- */
/* Compile-time configuration                                              */
/* ---------------------------------------------------------------------- */

/* Numeric level constants — preprocessor-visible so they can be used in
 * #if expressions for compile-time level filtering. Lower value = higher
 * severity. */
#define LOG_LVL_ERROR  (0)
#define LOG_LVL_WARN   (1)
#define LOG_LVL_INFO   (2)
#define LOG_LVL_DEBUG  (3)

/* Compile-time minimum severity. Levels less severe than this are stripped
 * at compile time — no function call, no argument evaluation, no local
 * buffer. Override per build with -DLOG_LEVEL_MIN=LOG_LVL_xxx. */
#ifndef LOG_LEVEL_MIN
#define LOG_LEVEL_MIN  LOG_LVL_INFO
#endif

/* ANSI colour codes around the level tag. Set to 0 for plain text — useful
 * when capturing to a file or piping through a non-ANSI terminal. The
 * message body is never coloured: it always renders in the terminal's
 * default foreground to remain readable on any colour scheme. */
#ifndef LOGGER_USE_ANSI_COLORS
#define LOGGER_USE_ANSI_COLORS  (1)
#endif

/* Caller-side scratch buffer for printf substitution inside the macros.
 * The substituted message is copied (truncated) into the queue entry. */
#define LOGGER_MESSAGE_MAX   (64U)
#define LOGGER_MODULE_WIDTH  (16U)

/* ---------------------------------------------------------------------- */
/* Types                                                                   */
/* ---------------------------------------------------------------------- */

typedef enum {
    LOG_LEVEL_ERROR = LOG_LVL_ERROR,
    LOG_LEVEL_WARN  = LOG_LVL_WARN,
    LOG_LEVEL_INFO  = LOG_LVL_INFO,
    LOG_LEVEL_DEBUG = LOG_LVL_DEBUG
} log_level_t;

typedef enum {
    LOGGER_OK              = 0,
    LOGGER_ERR_NOT_INIT    = 1,
    LOGGER_ERR_INVALID_ARG = 2
} logger_err_t;

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

/**
 * @brief Initialise the Logger: create the line queue and the drain task.
 *
 * Must be called once during board init, AFTER debug_uart_init() and
 * rtc_init(). May be called before vTaskStartScheduler(); the drain task
 * does not run until the scheduler starts, and pre-scheduler log calls
 * write to the UART synchronously (§3.7).
 *
 * @param initial_level Runtime maximum severity to emit (in addition to
 *                       the compile-time LOG_LEVEL_MIN floor).
 * @return LOGGER_OK on success; LOGGER_ERR_INVALID_ARG if out of range.
 * @note Threading: task-context only, non-blocking. Single invocation,
 *       before the scheduler starts.
 */
logger_err_t logger_init(log_level_t initial_level);

/**
 * @brief Change the runtime severity filter.
 *
 * @param level New maximum severity to emit.
 * @return LOGGER_OK, LOGGER_ERR_NOT_INIT, or LOGGER_ERR_INVALID_ARG.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
logger_err_t logger_set_level(log_level_t level);

/**
 * @brief Submit a plain message string to the logger (task context).
 *
 * The public API takes a finished message string — printf-style argument
 * substitution is done by the LOG_* macros locally in the caller's stack
 * frame before this function is invoked. Logger captures the wall-clock
 * timestamp here (the only step that must run on the caller's stack), then
 * packs level + timestamp + module + message into a structured queue entry
 * and enqueues it. Line assembly — level tag, ANSI colour codes, padding,
 * CRLF — is performed by the drain task (§3.3).
 *
 * Fire-and-forget. On a full queue the entry is dropped and the dropped
 * count is incremented. The call never blocks the caller and never
 * returns an error (see §6 for the failure philosophy).
 *
 * Callers should use the LOG_* macros, not this function directly.
 *
 * @param level  Severity.
 * @param module Source tag (truncated/padded to LOGGER_MODULE_WIDTH).
 * @param msg    Plain, NUL-terminated message string. NULL is a no-op.
 * @note Threading: task-context only. NOT ISR-safe — use
 *       logger_log_from_isr() from interrupt context.
 */
void logger_log(log_level_t level, const char *module, const char *msg);

/**
 * @brief Submit a plain message string from ISR context.
 *
 * Captures uptime (not wall-clock — RtcDriver must not be polled from an
 * ISR), packs the structured queue entry, and enqueues via the FromISR
 * queue API. Requests a context switch if a higher-priority task was
 * woken.
 *
 * @param level  Severity.
 * @param module Source tag.
 * @param msg    Pre-formatted, NUL-terminated message. NULL is a no-op.
 * @note Threading: ISR-context only.
 */
void logger_log_from_isr(log_level_t level, const char *module, const char *msg);

/**
 * @brief Number of log lines dropped due to a full queue since boot.
 *
 * Diagnostic counter for HealthMonitor. Best-effort accuracy.
 *
 * @return Accumulated dropped-line count.
 * @note Threading: any context, non-blocking.
 */
uint32_t logger_get_dropped_count(void);

/* ---------------------------------------------------------------------- */
/* Task-context macros — run snprintf locally, then call logger_log()      */
/* ---------------------------------------------------------------------- */

#if (LOG_LVL_ERROR <= LOG_LEVEL_MIN)
#define LOG_ERROR(mod, fmt, ...) do {                                     \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                 \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_ERROR, (mod), _logger_msg);                      \
} while (0)
#else
#define LOG_ERROR(mod, fmt, ...) ((void)0)
#endif

#if (LOG_LVL_WARN <= LOG_LEVEL_MIN)
#define LOG_WARN(mod, fmt, ...) do {                                      \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                 \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_WARN, (mod), _logger_msg);                       \
} while (0)
#else
#define LOG_WARN(mod, fmt, ...) ((void)0)
#endif

#if (LOG_LVL_INFO <= LOG_LEVEL_MIN)
#define LOG_INFO(mod, fmt, ...) do {                                      \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                 \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_INFO, (mod), _logger_msg);                       \
} while (0)
#else
#define LOG_INFO(mod, fmt, ...) ((void)0)
#endif

#if (LOG_LVL_DEBUG <= LOG_LEVEL_MIN)
#define LOG_DEBUG(mod, fmt, ...) do {                                     \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                 \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_DEBUG, (mod), _logger_msg);                      \
} while (0)
#else
#define LOG_DEBUG(mod, fmt, ...) ((void)0)
#endif

/* ---------------------------------------------------------------------- */
/* ISR-context macros — plain string only (no printf substitution)         */
/* ---------------------------------------------------------------------- */

#if (LOG_LVL_ERROR <= LOG_LEVEL_MIN)
#define LOG_ERROR_ISR(mod, msg)  logger_log_from_isr(LOG_LEVEL_ERROR, (mod), (msg))
#else
#define LOG_ERROR_ISR(mod, msg)  ((void)0)
#endif

#if (LOG_LVL_WARN <= LOG_LEVEL_MIN)
#define LOG_WARN_ISR(mod, msg)   logger_log_from_isr(LOG_LEVEL_WARN, (mod), (msg))
#else
#define LOG_WARN_ISR(mod, msg)   ((void)0)
#endif

#endif /* LOGGER_H */
```

Callers' code is unchanged from a `printf`-style caller's perspective:

```c
LOG_ERROR("SensorSvc", "read failed: %d", err);   /* same as ever */
```

The macro expands to a local `snprintf` into a stack buffer, then a single
call to `logger_log()` with the finished string. Logger sees a plain
message and never handles a `va_list`.

### 2.2 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| DebugUartDriver | Yes | Yes — `debug_uart_send()` from the drain task and the pre-scheduler path |
| RtcDriver | Yes (bootstrap exception) | Yes — `rtc_get_time()` for the task-path timestamp |
| FreeRTOS | (infrastructure) | Yes — static queue, drain task, scheduler-state query |

**Bootstrap-exception rationale (P1 / P2):** TimeProvider depends on
ILogger. If Logger depended on ITimeProvider there would be a circular
dependency. Logger therefore calls RtcDriver directly. This is the only
documented exception to the rule that Middleware consumes interfaces, not
concrete drivers. Recorded in `components.md` preamble.

**P4 (cross-cutting infrastructure):** Logger is one of the two cross-cutting
components defined by P4. It may be referenced concretely throughout both
boards' USES lists, while itself depending only on driver-layer abstractions.

### 2.3 No vtable

Logger does **not** expose an `ilogger_t` function-pointer table. This is
the explicit P4 carve-out: cross-cutting infrastructure may be referenced
concretely throughout the codebase, and no consumer holds an injected
`ILogger` pointer or swaps implementations at runtime. A vtable would be
dead surface — every actual call site uses the `LOG_*` macros, which dispatch
to the concrete `logger_log()`. See LOG-D9.

---

## 3. Internal design

### 3.1 Module structure

```
logger.h     — public API (ILogger), convenience macros
logger.c     — singleton state, queue + drain task, format engine, output
```

### 3.2 Singleton state and queue entry

The queue carries **structured data**, not pre-formatted bytes. Each entry
holds enough to reconstruct the line in the drain task: level, timestamp
(wall-clock or uptime — disambiguated by a flag), the module tag, and the
substituted message string.

```c
#define LOGGER_QUEUE_DEPTH   (16U)   /* entries buffered before drop */

typedef struct {
    log_level_t  level;
    bool         use_uptime;              /* true → ts.uptime_ms; false → ts.wallclock */
    union {
        struct {
            uint8_t hour;
            uint8_t minute;
            uint8_t second;
        }        wallclock;
        uint32_t uptime_ms;
    } ts;
    char         module[LOGGER_MODULE_WIDTH + 1];   /* NUL-terminated, truncated */
    char         message[LOGGER_MESSAGE_MAX];        /* NUL-terminated, truncated */
} log_entry_t;
/* ~96 bytes after C struct padding × 16 entries = ~1.5 KB static */

typedef struct {
    log_level_t level;                    /* runtime severity filter            */
    bool        initialised;
} logger_state_t;

static logger_state_t s_log;
static volatile uint32_t s_dropped;       /* lines dropped on full queue        */

/* Static FreeRTOS objects — no heap (P5). */
static StaticQueue_t s_queue_ctrl;
static uint8_t       s_queue_storage[LOGGER_QUEUE_DEPTH * sizeof(log_entry_t)];
static QueueHandle_t s_queue;

static StaticTask_t  s_drain_tcb;
static StackType_t   s_drain_stack[LOGGER_DRAIN_STACK_WORDS];
static TaskHandle_t  s_drain_task;
```

No mutex. Each producer fills a local stack-resident `log_entry_t` and
hands it to `xQueueSend()` (which copies the struct into its slot under its
own internal locking). The drain task takes a local copy back via
`xQueueReceive()`. No shared mutable buffer exists.

### 3.3 Queue, drain task, and line-format helper

`logger_init()` creates the queue and the drain task with static
allocation, before the scheduler starts:

```c
s_queue = xQueueCreateStatic(LOGGER_QUEUE_DEPTH, sizeof(log_entry_t),
                             s_queue_storage, &s_queue_ctrl);
s_drain_task = xTaskCreateStatic(logger_drain_task, "log_drain",
                                 LOGGER_DRAIN_STACK_WORDS, NULL,
                                 LOGGER_DRAIN_TASK_PRIORITY,
                                 s_drain_stack, &s_drain_tcb);
```

The drain task is the sole owner of the UART write **and** of the line
assembly. It blocks on the queue, formats each entry into its own scratch
buffer, then transmits:

```c
static void logger_drain_task(void *arg)
{
    (void)arg;
    log_entry_t entry;
    char        out[LOGGER_OUT_BUF_MAX];
    for (;;)
    {
        if (xQueueReceive(s_queue, &entry, portMAX_DELAY) == pdTRUE)
        {
            uint16_t len = format_line(&entry, out, sizeof(out));
            (void)debug_uart_send((const uint8_t *)out, len);
        }
    }
}
```

`format_line()` is the **single source of truth for the on-wire format**
— shared between the drain task and the pre-scheduler synchronous path
(§3.7). Assembling the line outside the producer means the producer's
critical path stays minimal (capture timestamp, copy strings, enqueue),
and the comparatively expensive `snprintf`-style assembly plus ANSI colour
emission happens off the producer's stack.

`LOGGER_OUT_BUF_MAX` is sized for the worst-case line: level tag with
colour codes (5+9) + `[HH:MM:SS]` (10) + `[module]` padded (18) + space +
message (64) + `\r\n` + NUL ≈ 112 bytes. Conservatively 128.

`LOGGER_DRAIN_TASK_PRIORITY` is low — above idle, below any time-critical
task — so the multi-millisecond UART write never preempts real work. Task
and queue must be added to `task-breakdown.md` (LOG-O1).

### 3.4 Line format

```
\033[31m[ERROR]\033[0m[12:34:56][SensorSvc       ] read failed: -3\r\n
\033[33m[ WARN]\033[0m[12:34:57][TimeSvc         ] NTP retry 2/3\r\n
\033[36m[ INFO]\033[0m[12:34:58][Boot            ] system ready\r\n
\033[2m [DEBUG]\033[0m[12:34:59][Modbus          ] tx frame len=8\r\n
```

The escape sequences are inline above for clarity; in a real terminal they
collapse to colour on the level tag only. The message body and other
brackets are uncoloured — they render in the terminal's default foreground
and stay legible on any colour theme.

**Severity → colour:**

| Level | Code | Colour |
|---|---|---|
| ERROR | `\033[31m` | red |
| WARN  | `\033[33m` | yellow |
| INFO  | `\033[36m` | cyan |
| DEBUG | `\033[2m`  | dim |

Set `LOGGER_USE_ANSI_COLORS=0` (compile-time) to emit plain text with no
escape sequences — useful for piping to a file or to a non-ANSI capture
tool.

**Field rules:**

- **LEVEL** — 5-char padded: `ERROR`, ` WARN`, ` INFO`, `DEBUG`.
- **Timestamp** — `HH:MM:SS` from the entry's `ts.wallclock` when
  `use_uptime` is false, or `T+nnnnnnnn` (uptime milliseconds, 8 digits)
  when `use_uptime` is true.
- **module** — left-aligned, padded/truncated to `LOGGER_MODULE_WIDTH`.
  NULL renders as `?` padded.
- **message** — copied verbatim from `entry.message`; already truncated
  to `LOGGER_MESSAGE_MAX - 1` (NUL).

**`format_line()` pseudocode:**

```
1. Emit ANSI colour-on for level (if LOGGER_USE_ANSI_COLORS != 0).
2. Emit "[LEVEL]" (5-char padded inside the brackets).
3. Emit ANSI reset (if colours on).
4. If entry.use_uptime: emit "[T+%08u]" with ts.uptime_ms.
   Else                : emit "[%02u:%02u:%02u]" with wallclock fields.
5. Emit "[" + module (left-padded to width) + "]".
6. Emit " " separator + entry.message + "\r\n".
7. Return total length written (≤ LOGGER_OUT_BUF_MAX).
```

No external state read; pure function of the entry and the configured
colour mode.

### 3.5 `logger_log()` flow (task context)

```
1.  If !s_log.initialised → return.
2.  If level > s_log.level → return (runtime filter; lower value = severer).
3.  If module == NULL → module = "?".
4.  If msg == NULL → return.
5.  log_entry_t e;  (on caller stack)
6.  e.level = level.
7.  If rtc_get_time(&dt) == RTC_OK:
        e.use_uptime = false;
        e.ts.wallclock = { dt.hour, dt.minute, dt.second };
    else:
        e.use_uptime = true;
        e.ts.uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
8.  strncpy(e.module, module, LOGGER_MODULE_WIDTH);  /* NUL-terminate */
9.  strncpy(e.message, msg,    LOGGER_MESSAGE_MAX);  /* NUL-terminate */
10. If xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED:
        format_line(&e, scratch, sizeof(scratch));
        debug_uart_send(scratch, len);                  (pre-scheduler, §3.7)
    Else if xQueueSend(s_queue, &e, 0) != pdTRUE:
        atomic increment s_dropped.                     (queue full → drop)
11. Return.
```

The runtime filter (step 2) reads `s_log.level` without a lock. A 32-bit
aligned enum read is atomic on Cortex-M; a momentarily stale filter value
is harmless. `xQueueSend` uses a zero timeout — the producer never blocks.

The wall-clock timestamp **must** be captured here, on the caller's stack:
deferring to the drain task would record the *drain* time, not the *event*
time. This is the one piece of producer-side work that cannot move.

### 3.6 `logger_log_from_isr()` flow (ISR context)

```
1.  If !s_log.initialised → return.
2.  If level > s_log.level → return.
3.  If module == NULL → module = "?".
4.  If msg == NULL → return.
5.  log_entry_t e;  (on ISR stack — drain stack budget accounts for it)
6.  e.level = level.
7.  e.use_uptime = true.
    e.ts.uptime_ms = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS.
8.  strncpy(e.module, module, LOGGER_MODULE_WIDTH).
9.  strncpy(e.message, msg,    LOGGER_MESSAGE_MAX).
10. BaseType_t woken = pdFALSE.
    If xQueueSendFromISR(s_queue, &e, &woken) != pdTRUE:
        increment s_dropped (under ISR-safe mask).
11. portYIELD_FROM_ISR(woken).
```

No printf substitution and no call to RtcDriver. Both would be unsafe or
unbounded in interrupt context.

### 3.7 Pre-scheduler usage

`logger_init()` runs before `vTaskStartScheduler()`, so the queue and task
exist but the drain task is not yet running. Log calls made during board
init detect this via `xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED`
and write to the UART **synchronously**, bypassing the queue (step 10 of
§3.5). The line is assembled via the same `format_line()` helper the drain
task uses — there is only one on-wire format in the codebase. No
concurrency exists pre-scheduler, so the direct write is safe. Once the
scheduler starts, all task-context logs enqueue and the drain task takes
over the UART.

### 3.8 Principles applied

- **P1 (Strict directional layering).** Logger (Middleware) depends only on
  RtcDriver and DebugUartDriver (drivers) plus FreeRTOS infrastructure. No
  Application dependency.
- **P2 (Dependency Inversion) — considered, P4 carve-out applies.** Logger
  is one of the two cross-cutting infrastructure components defined by P4
  and is referenced concretely throughout. No `ilogger_t` vtable is exposed
  (LOG-D9): no consumer injects a logger or swaps implementations, so a
  vtable would be dead surface that contradicts the macro-based ergonomic
  path.
- **P4 (Cross-cutting infrastructure exception).** Logger IS one of the two
  P4 components; concrete references by other components are accepted and
  documented.
- **P5 (Bounded resources, no dynamic allocation).** Static queue storage,
  static task stack/TCB, fixed-size structured queue entry; no heap.
- **P6 (Responsibility traces to requirements).** Output traces to
  REQ-NF-500 and REQ-NF-504.
- **P8 (Total error propagation, no silent failures).** Lifecycle calls
  (`init`, `set_level`) return `logger_err_t`. The logging path is
  deliberately best-effort (void) with internal drop accounting — see §6.
- **P9 (BARR-C coding standard).** Fixed-width integer types; explicit
  unsigned literals; braces on all control flow; no floating point.
- **P10 (Naming conventions).** Prefix `logger_`; levels `LOG_LEVEL_*`;
  error values `LOGGER_ERR_*`; numeric mirrors `LOG_LVL_*`.

---

## 4. Hardware contract

Logger has no direct hardware access. It delegates to:

- **DebugUartDriver** — `debug_uart_send(buf, len)`: blocking, polling-mode
  UART write. At 115200 baud an `LOGGER_LINE_MAX` (96-byte) line transmits in
  ~8.3 ms. This blocking now lives entirely in the low-priority drain task,
  off every producer's critical path — the key benefit of the queue design.
  **LOG-O2 (closed):** DebugUartDriver companion confirms `debug_uart_send`
  is blocking/polling; exact signature to be matched against
  `debug_uart_driver.h` at implementation.

- **RtcDriver** — `rtc_get_time(&dt)`: register read with a brief RSF poll,
  bounded to microseconds in steady state. Called only from the task path.

---

## 5. Sequence integration

```
Task path (e.g. SensorTask)
  → LOG_ERROR("SensorSvc", "read failed: %d", err)
      (macro expansion: snprintf into local stack buffer)
      → logger_log(LOG_LEVEL_ERROR, "SensorSvc", "read failed: -3")
          → filter check
          → rtc_get_time() → fill log_entry_t (level + wall-clock + strs)
          → xQueueSend(0 timeout)        /* returns immediately */
      (returns to SensorTask — no UART blocking on this path)

Drain task (low priority, separate)
  → xQueueReceive(portMAX_DELAY)
  → format_line(&entry, out_buf)         /* level tag + ANSI + ts + module */
  → debug_uart_send(out_buf, len)        /* ~8 ms blocking, harms nobody  */

ISR path
  → LOG_WARN_ISR("Modbus", "frame timeout")
      → logger_log_from_isr(...) → fill entry with uptime → xQueueSendFromISR
      → portYIELD_FROM_ISR(woken)
```

No component owns Logger; the drain task owns only the UART write and the
line assembly. No new sequence diagram is required — Logger appears as a
synchronous sub-call in the callers' existing flows, with the drain task as
an independent consumer.

---

## 6. Error and fault behaviour

Two distinct failure philosophies, by function class:

**Lifecycle calls** (`logger_init`, `logger_set_level`) return `logger_err_t`.
Callers check them; a non-OK return is a programming error (called before
init, level out of range). Debug builds assert; release builds no-op.

**The logging path** (`logger_log`, `logger_log_from_isr`, and all macros) is
**fire-and-forget and returns void** (LOG-D3). A return value here would be
useless: a caller cannot log a logging failure (recursion), and cannot abort
its real work because a diagnostic line was lost. So failure handling lives
*inside* Logger: on a full queue the line is **dropped** and `s_dropped` is
**incremented**; the count is observable via `logger_get_dropped_count()`
(read by HealthMonitor) or surfaced as a single WARN line once the queue
drains below a low-water mark.

| Error value | Cause | Local behaviour | Caller-visible | Observability |
|---|---|---|---|---|
| `LOGGER_ERR_INVALID_ARG` | `logger_init`/`logger_set_level` given an out-of-range level | Return error; no state change | Non-OK return | Caller asserts in debug |
| `LOGGER_ERR_NOT_INIT` | `logger_set_level` called before `logger_init` | Return error | Non-OK return | Caller asserts in debug |
| (queue full) | Producers outrun the drain task | Drop line; increment `s_dropped` | None (void path) | `logger_get_dropped_count()`; periodic WARN |
| (NULL fmt/msg) | Misuse of the logging path | Drop; assert in debug | None (void path) | Debug assert |

---

## 7. Unit-test plan

Host-platform tests (Unity). Logger is the first module that depends on
FreeRTOS, so the harness needs FreeRTOS API stubs in addition to the existing
driver mocks:

- **Format / filter / truncation logic** is exercised through the
  **pre-scheduler path** — stub `xTaskGetSchedulerState()` to return
  `taskSCHEDULER_NOT_STARTED`, mock `debug_uart_send()` to capture output into
  a test buffer, and mock `rtc_get_time()` to return a fixed time or an error.
  No queue/task needed for these.
- **Enqueue / drop / drain logic** uses stubs for `xQueueSend`,
  `xQueueSendFromISR`, `xQueueReceive`, and the static-create calls.

**LOG-O3:** decide host FreeRTOS mock strategy — hand-written minimal stubs
(`xQueueSend`, `xQueueReceive`, `xQueueSendFromISR`, `xQueueCreateStatic`,
`xTaskCreateStatic`, `xTaskGetSchedulerState`, `xTaskGetTickCount`,
`xTaskGetTickCountFromISR`) versus CMock-generated from the FreeRTOS headers.
Recommendation: hand-written minimal stubs — the FreeRTOS headers are large
and only a handful of calls are used.

| Test ID | Scenario | Expected |
|---------|----------|----------|
| TC-LOG-001 | Nominal INFO, RTC OK, pre-scheduler path | Output ends `[ INFO]` colour-wrapped + `[00:01:23][MyModule        ] Hello\r\n` |
| TC-LOG-002 | Runtime filter WARN, call LOG_INFO | No `xQueueSend` and no UART write |
| TC-LOG-003 | Runtime filter WARN, call LOG_ERROR | Output produced |
| TC-LOG-004 | RTC OK | Timestamp field `[HH:MM:SS]` |
| TC-LOG-005 | `rtc_get_time` returns error | Timestamp field `[T+nnnnnnnn]` uptime |
| TC-LOG-006 | Message longer than buffer | Truncated to `LOGGER_MESSAGE_MAX - 1`; NUL-terminated; line ends `\r\n` |
| TC-LOG-007 | NULL module | Module field rendered as `?` padded |
| TC-LOG-008 | NULL msg | No output; no crash |
| TC-LOG-009 | `logger_log` before `logger_init` | No output; no crash |
| TC-LOG-010 | `logger_set_level` out of range | Returns `LOGGER_ERR_INVALID_ARG` |
| TC-LOG-011 | Module name longer than width | Truncated to `LOGGER_MODULE_WIDTH` |
| TC-LOG-012 | Queue full (post-scheduler) | Entry dropped; `logger_get_dropped_count()` increments |
| TC-LOG-013 | Several drops | `logger_get_dropped_count()` returns accumulated total |
| TC-LOG-014 | Post-scheduler nominal | `xQueueSend` invoked with the filled entry; no direct UART write |
| TC-LOG-015 | `logger_init` out-of-range level | Returns `LOGGER_ERR_INVALID_ARG` |
| TC-LOG-016 | `logger_set_level` before init | Returns `LOGGER_ERR_NOT_INIT` |
| TC-LOG-017 | `logger_log_from_isr` | Entry filled with uptime; `xQueueSendFromISR` invoked |
| TC-LOG-018 | Drain task body: dequeue one entry | `format_line` called; `debug_uart_send` called with the assembled line |
| TC-LOG-019 | Level padding | `ERROR`/` WARN`/` INFO`/`DEBUG` exactly 5 chars after tag-stripping ANSI |
| TC-LOG-020 | `logger_init` happy path | Returns `LOGGER_OK`; queue + task created |
| TC-LOG-021 | `format_line` ANSI on: ERROR | Output contains `\033[31m` before `[ERROR]` and `\033[0m` after |
| TC-LOG-022 | `format_line` ANSI on: WARN/INFO/DEBUG | Correct colour code per level |
| TC-LOG-023 | `format_line` `LOGGER_USE_ANSI_COLORS=0` | No escape sequences in output |
| TC-LOG-024 | `format_line` uptime path | Timestamp field `[T+nnnnnnnn]` with leading zeros |
| TC-LOG-025 | Macro-level snprintf truncates an oversized message | Local buffer NUL-terminated at `LOGGER_MESSAGE_MAX - 1` before reaching logger_log |

Test file: `tests/field-device/middleware/logger/test_logger.c` (gateway
mirror added with the L475 build later).

---

## 8. Open items

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| LOG-D1 | RtcDriver used directly, not via ITimeProvider | Avoids circular dependency (TimeProvider depends on ILogger). Bootstrap exception, `components.md` preamble |
| LOG-D2 | Queue + drain task; no mutex | UART blocking moved off every producer's path into one low-priority task. Per-producer stack buffers + a FreeRTOS queue remove the need for `logger_mutex` entirely |
| LOG-D3 | `void` return from the logging path | A logging failure is unactionable at the call site (cannot log it, cannot abort real work). Failure handled internally: drop + count |
| LOG-D4 | Compile-time `LOG_LEVEL_MIN` filter (in addition to runtime level) | Strips a whole severity (e.g. DEBUG) from a build with zero code and zero argument evaluation. Numeric `LOG_LVL_*` macros are used in the `#if` because enum values are invisible to the preprocessor |
| LOG-D5 | 96-byte structured entry × 16-deep queue (static) | Fits level + timestamp + module + message comfortably; 16 absorbs a burst before dropping; ~1.5 KB static, no heap |
| LOG-D6 | Pre-scheduler synchronous write via `xTaskGetSchedulerState()` | Boot diagnostics work before the drain task runs; scheduler-state query is the correct API, replacing the brittle `pre_scheduler` flag from the draft |
| LOG-D7 | (Withdrawn — see LOG-D9) | — |
| LOG-D8 | Seconds-resolution timestamp; uptime fallback on RTC error | RtcDriver exposes no sub-second accessor, so `[HH:MM:SS]` not `.mmm`. A failed `rtc_get_time` (wedged RTC) falls back to uptime so a line is still emitted |
| LOG-D9 | No `ilogger_t` vtable | Logger is P4 cross-cutting infrastructure with concrete references throughout. No consumer injects a logger; macros are the only call path. A vtable would be dead surface. P4 is the explicit carve-out from the BARR-C OOP-in-C "all IXxx need a function-pointer table" rule |
| LOG-D10 | Producer fills a structured entry; drain task does line assembly (including ANSI colour codes, level tag, timestamp formatting, module padding, CRLF) | Keeps the producer's critical path minimal (timestamp capture + strncpy + enqueue). Drain task is the single source of truth for the on-wire format. `format_line()` is shared between the drain task and the pre-scheduler synchronous path |
| LOG-D11 | ANSI colour codes on the level tag only; compile-time `LOGGER_USE_ANSI_COLORS` toggle | Coloured tag gives a fast visual scan in a live terminal; uncoloured message body remains readable on any colour theme. Toggle to 0 produces plain text for file capture or non-ANSI tools. ~9 bytes per line when enabled, 0 when disabled |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| LOG-O1 | Add the drain task (priority, stack) and the line queue to `task-breakdown.md`; remove `logger_mutex` | Luca | Edit `task-breakdown.md` before implementation; pin `LOGGER_DRAIN_TASK_PRIORITY` and `LOGGER_DRAIN_STACK_WORDS` |
| LOG-O3 | Host FreeRTOS mock strategy for the test build (§7) | Luca | Decide hand-written stubs vs CMock; recommend hand-written minimal stubs |
| LOG-O4 | `debug_uart_send` exact signature/return type | Luca | Match against `debug_uart_driver.h` at implementation; adjust the drain-task call if needed |
| LOG-O5 | Whether ISR logging (`logger_log_from_isr`) is actually needed, or whether the stricter "ISRs notify, owning task logs" discipline is preferable | Luca | Confirm. If dropped, remove the ISR entry point, the ISR macros, and §3.6 — simplifies the module. Built for now per the Q4 decision |

---

*This document is the LLD companion for Logger. Authored for Luca Agrippino;
maintained by the project mentor per the agreed workflow.*