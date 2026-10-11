# LLD Companion — StoreAndForward

**Document:** `docs/lld/application/store-and-forward.md`
**Version:** 0.3 (Pass H complete — Gateway implemented and hardware-validated)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Application
**Status:** Pass H complete — GW v1.0 implemented and hardware-validated (2026-10-11)
**Date:** October 2026

**HLD anchor:** StoreAndForward in `components.md` (GW application layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Stores telemetry in flash when the cloud is unreachable, and forwards it in chronological order when connectivity is restored. Reports buffer occupancy via IHealthReport. | `components.md` |
| PROVIDES (upward) | IStoreAndForward | `components.md` |
| USES (downward) | CircularFlashLog (via `cfl_handle_t`), IHealthReport, ILogger | `components.md` |
| Root requirements | REQ-BF-000, REQ-BF-010, REQ-BF-020, REQ-CC-010 | `SRS.md` |
| HLD refs | `hld.md` §6.3, §7.3; `sequence-diagrams.md` SD-04; `state-machines.md` Machine 2 (Cloud Connectivity) | HLD |

**Responsibility boundary.** StoreAndForward owns:
- The drop-oldest policy (REQ-BF-020).
- Serialisation of topic + QoS + payload into a flat byte blob for flash.
- Two-phase dequeue/confirm state (power-loss-safe drain).
- Buffer occupancy tracking and health push.
- Skipping corrupt CFL records during dequeue.

CircularFlashLog owns:
- Raw append, peek, consume, and drop-oldest on QSPI flash.
- Ring-buffer wrap, head/tail pointer persistence, record CRC.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Buffer outbound messages when offline | REQ-BF-000 | UC-10 |
| Drain in chronological order on reconnect | REQ-BF-010 | UC-11 |
| Drop oldest when full | REQ-BF-020 | UC-12 |
| Buffer occupancy in health metrics | REQ-CC-010 | UC-06 |

---

## 3. Public API

### 3.1 ADT pattern

StoreAndForward follows the Gateway ADT default: an opaque handle
returned by `saf_create()` from a static pool of 1. Dependencies
(CircularFlashLog handle, IHealthReport handle, capacity) are
injected via the config struct. Logger is accessed directly (P4
cross-cutting exception).

### 3.2 Data types

```c
/* store_and_forward.h */

#ifndef STORE_AND_FORWARD_H
#define STORE_AND_FORWARD_H

#include <stdint.h>
#include <stdbool.h>
#include "circular_flash_log.h"
#include "health_monitor.h"

/** @brief Opaque handle to a StoreAndForward instance. */
typedef struct saf_inst *saf_handle_t;

/** @brief Maximum topic length in characters (excluding null terminator). */
#define SAF_TOPIC_MAX       64u

/**
 * @brief Maximum payload size in bytes.
 *
 * Derived from CFL_MAX_RECORD_BYTES minus the SAF header overhead
 * at maximum topic length:
 *   CFL_MAX_RECORD_BYTES(4078) - topic_len(1) - topic(64) - qos(1) - payload_len(2) = 4010
 */
#define SAF_PAYLOAD_MAX     4010u

typedef enum {
    SAF_ERR_OK          = 0,
    SAF_ERR_NULL_PTR    = 1,  /**< Required pointer is NULL.               */
    SAF_ERR_NO_RESOURCE = 2,  /**< Static pool exhausted.                  */
    SAF_ERR_NOT_INIT    = 3,  /**< Called before saf_create().              */
    SAF_ERR_TOO_LARGE   = 4,  /**< Payload exceeds SAF_PAYLOAD_MAX or is 0.*/
    SAF_ERR_EMPTY       = 5,  /**< Dequeue called on empty buffer.         */
    SAF_ERR_FLASH       = 6,  /**< CircularFlashLog I/O failure.           */
} saf_err_t;

/**
 * @brief Dequeued entry returned to the caller.
 *
 * The @c payload pointer is valid only until the next call to
 * saf_dequeue() or saf_confirm().
 */
typedef struct {
    char            topic[SAF_TOPIC_MAX + 1u]; /**< Null-terminated.       */
    uint8_t         qos;                       /**< 0 or 1.                */
    uint16_t        payload_len;
    const uint8_t  *payload;                   /**< Points to internal buf.*/
} saf_entry_t;

/**
 * @brief StoreAndForward creation configuration.
 */
typedef struct {
    cfl_handle_t            cfl;       /**< CircularFlashLog handle.       */
    health_monitor_handle_t health;    /**< IHealthReport handle.          */
    uint32_t                capacity;  /**< Worst-case entry count for
                                            occupancy % calculation.       */
} saf_config_t;
```

### 3.3 Public functions — IStoreAndForward

```c
/**
 * @brief Create and initialise a StoreAndForward instance.
 *
 * Recovers entry_count from cfl_get_occupancy() and pushes the
 * initial occupancy to IHealthReport.
 *
 * @param[in]  config  Injected dependencies.
 * @param[out] handle  Receives the created handle on success.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if config or
 *         handle is NULL; SAF_ERR_NO_RESOURCE if pool exhausted;
 *         SAF_ERR_FLASH if cfl_get_occupancy() fails.
 * @note Threading: task-context only. Call after cfl_create().
 */
saf_err_t saf_create(const saf_config_t *config, saf_handle_t *handle);

/**
 * @brief Serialise and append an entry to the buffer.
 *
 * If the CFL ring is full, drops oldest entries (one at a time) via
 * cfl_drop_oldest() until space is available (REQ-BF-020). Each drop
 * pushes HEALTH_EVENT_BUFFER_OVERFLOW through CFL.
 *
 * @param[in] handle  SAF handle.
 * @param[in] topic   Null-terminated MQTT topic (≤ SAF_TOPIC_MAX chars).
 * @param[in] buf     Payload bytes.
 * @param[in] len     Payload byte count (1 ≤ len ≤ SAF_PAYLOAD_MAX).
 * @param[in] qos     MQTT QoS (0 or 1).
 * @return SAF_ERR_OK on success; SAF_ERR_TOO_LARGE if len == 0 or
 *         len > SAF_PAYLOAD_MAX; SAF_ERR_NULL_PTR if handle, topic
 *         or buf is NULL; SAF_ERR_FLASH on CFL failure.
 */
saf_err_t saf_enqueue(saf_handle_t handle,
                       const char *topic,
                       const uint8_t *buf,
                       uint16_t len,
                       uint8_t qos);

/**
 * @brief Return a pointer to the oldest entry without removing it.
 *
 * If a previous dequeue has not been confirmed, returns the same
 * entry without calling CFL. If the CFL peek returns CFL_ERR_CORRUPT,
 * the corrupt record is silently consumed and the next record is tried.
 *
 * @param[in]  handle  SAF handle.
 * @param[out] out     Receives pointer to the entry. Valid until the
 *                     next call to saf_dequeue() or saf_confirm().
 * @return SAF_ERR_OK on success; SAF_ERR_EMPTY if buffer is empty;
 *         SAF_ERR_FLASH on CFL read failure.
 */
saf_err_t saf_dequeue(saf_handle_t handle, const saf_entry_t **out);

/**
 * @brief Consume the last dequeued entry.
 *
 * Advances the CFL read pointer. Must be called exactly once after
 * a successful MQTT publish.
 *
 * @param[in] handle  SAF handle.
 * @return SAF_ERR_OK on success (including when no entry is pending —
 *         idempotent); SAF_ERR_FLASH on CFL consume failure.
 */
saf_err_t saf_confirm(saf_handle_t handle);

/**
 * @brief Query whether the buffer is empty.
 *
 * Used by CloudPublisher at reconnect to decide whether to enter the
 * Draining sub-state (SD-04b step 3).
 *
 * @param[in]  handle     SAF handle.
 * @param[out] empty_out  Receives true if empty.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if empty_out is NULL.
 */
saf_err_t saf_is_empty(saf_handle_t handle, bool *empty_out);

/**
 * @brief Return buffer occupancy for diagnostics.
 *
 * @param[in]  handle       SAF handle.
 * @param[out] entry_count  Number of entries in the buffer.
 * @param[out] capacity     Configured worst-case capacity.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if an out param is NULL.
 */
saf_err_t saf_get_occupancy(saf_handle_t handle,
                              uint32_t *entry_count,
                              uint32_t *capacity);

#endif /* STORE_AND_FORWARD_H */
```

---

## 4. Entry format on flash

StoreAndForward serialises each entry into `stage_buf` before passing
it to `cfl_append()`. CircularFlashLog adds its own record framing
(12-byte header + 2-byte CRC-16/IBM) around this blob.

```
SAF serialised blob (little-endian):
  Byte offset    Field          Size
  0              topic_len      1 byte    (1 ≤ topic_len ≤ SAF_TOPIC_MAX)
  1              topic          topic_len bytes (no null terminator)
  1+topic_len    qos            1 byte    (0 or 1)
  2+topic_len    payload_len    2 bytes   (little-endian)
  4+topic_len    payload        payload_len bytes
```

| Measure | Value | Derivation |
|---|---|---|
| SAF header overhead (max) | 68 bytes | topic_len(1) + topic(64) + qos(1) + payload_len(2) |
| SAF_PAYLOAD_MAX | 4 010 bytes | CFL_MAX_RECORD_BYTES(4 078) − 68 |
| Max serialised SAF blob | 4 078 bytes | = CFL_MAX_RECORD_BYTES |
| CFL record overhead | 14 bytes | magic(4) + seq_num(4) + data_len(4) + CRC-16(2) |
| Max on-flash per entry | 4 092 bytes | 4 078 + 14 (4-byte aligned) |

---

## 5. Internal design

### 5.1 Private struct and static pool

```c
/* store_and_forward.c */

#define SAF_MAX_INSTANCES    1u
#define SAF_STAGE_BUF_SIZE   CFL_MAX_RECORD_BYTES  /* 4078 */

struct saf_inst {
    /* Injected dependencies */
    cfl_handle_t            cfl;
    health_monitor_handle_t health;

    /* Configuration */
    uint32_t  capacity;       /**< Worst-case entry count.              */

    /* Tracking */
    uint32_t  entry_count;    /**< Current entries in the ring.         */

    /* Two-phase drain state */
    bool            has_pending;
    saf_entry_t     pending_hdr;
    uint8_t         pending_payload[SAF_PAYLOAD_MAX];

    /* Serialisation / deserialisation scratch */
    uint8_t         stage_buf[SAF_STAGE_BUF_SIZE];

    bool            in_use;
};

static struct saf_inst g_pool[SAF_MAX_INSTANCES];
static uint8_t         g_count;
```

All buffers are static, allocated in `.bss`. No dynamic allocation (P5).

### 5.2 Algorithms

#### 5.2.1 `saf_create`

```
saf_create(config, handle):
    if config == NULL or handle == NULL:
        return SAF_ERR_NULL_PTR
    if g_count >= SAF_MAX_INSTANCES:
        return SAF_ERR_NO_RESOURCE

    self = &g_pool[g_count++]
    self->cfl      = config->cfl
    self->health   = config->health
    self->capacity = config->capacity

    rc = cfl_get_occupancy(self->cfl, &self->entry_count, NULL)
    if rc != CFL_ERR_OK:
        g_count--
        return SAF_ERR_FLASH

    self->has_pending = false
    self->in_use      = true
    *handle           = self

    report_occupancy(self)
    log_info("SAF: %u / %u entries at boot", self->entry_count, self->capacity)
    return SAF_ERR_OK
```

#### 5.2.2 `saf_enqueue`

```
saf_enqueue(handle, topic, buf, len, qos):
    if handle == NULL or topic == NULL or buf == NULL:
        return SAF_ERR_NULL_PTR

    if len == 0 or len > SAF_PAYLOAD_MAX:
        return SAF_ERR_TOO_LARGE

    serialise(topic, buf, len, qos → self->stage_buf, &raw_len)

    rc = cfl_append(self->cfl, self->stage_buf, raw_len)

    /* REQ-BF-020: drop oldest entries until space is available */
    while rc == CFL_ERR_FULL:
        rc2 = cfl_drop_oldest(self->cfl)
        if rc2 == CFL_ERR_EMPTY:
            break                 /* ring empty but still can't fit — shouldn't happen */
        if rc2 != CFL_ERR_OK:
            return SAF_ERR_FLASH
        self->entry_count--
        log_warn("SAF: buffer full — oldest entry dropped")
        rc = cfl_append(self->cfl, self->stage_buf, raw_len)

    if rc != CFL_ERR_OK:
        log_error("SAF: flash append failed (%d)", rc)
        return SAF_ERR_FLASH

    self->entry_count++
    report_occupancy(self)
    return SAF_ERR_OK
```

**Drop loop rationale.** CFL packs multiple records per sector. When
the tail must advance to the head sector, CFL returns `CFL_ERR_FULL`.
A single `cfl_drop_oldest()` advances the head pointer past one
record. If the head sector still contains more records, the head sector
is not freed, and the next append attempt may again return `CFL_ERR_FULL`.
The loop repeats until the head sector is freed and the append succeeds.
Termination is guaranteed: either a valid record is dropped each
iteration (bounded by the number of records in the head sector), or the
ring becomes empty and `cfl_drop_oldest()` returns `CFL_ERR_EMPTY`.

#### 5.2.3 `saf_dequeue`

```
saf_dequeue(handle, out):
    if handle == NULL or out == NULL:
        return SAF_ERR_NULL_PTR

    if self->has_pending:
        *out = &self->pending_hdr
        return SAF_ERR_OK

    for (;;):
        rc = cfl_peek_oldest(self->cfl, self->stage_buf,
                              &raw_len, SAF_STAGE_BUF_SIZE)
        if rc == CFL_ERR_EMPTY:
            return SAF_ERR_EMPTY
        if rc == CFL_ERR_CORRUPT:
            /* Skip corrupt record — see CFL §10 contract */
            cfl_consume_oldest(self->cfl)
            if self->entry_count > 0:
                self->entry_count--
            log_warn("SAF: skipped corrupt CFL record")
            continue
        if rc != CFL_ERR_OK:
            return SAF_ERR_FLASH
        break

    deserialise(self->stage_buf, raw_len,
                &self->pending_hdr, self->pending_payload)
    self->pending_hdr.payload = self->pending_payload
    self->has_pending = true
    *out = &self->pending_hdr
    return SAF_ERR_OK
```

The corrupt-skip loop terminates when a valid record is found or the
ring becomes empty (`CFL_ERR_EMPTY`).

#### 5.2.4 `saf_confirm`

```
saf_confirm(handle):
    if handle == NULL:
        return SAF_ERR_NULL_PTR

    if not self->has_pending:
        return SAF_ERR_OK               /* idempotent — no CFL call */

    rc = cfl_consume_oldest(self->cfl)
    if rc != CFL_ERR_OK:
        return SAF_ERR_FLASH

    self->has_pending = false
    self->entry_count--
    report_occupancy(self)
    return SAF_ERR_OK
```

#### 5.2.5 `report_occupancy` (internal)

```
report_occupancy(self):
    pct = (self->entry_count * 100u) / self->capacity
    health_update_buffer_occupancy(self->health,
                                   self->entry_count,
                                   self->capacity,
                                   (uint8_t)pct)
```

Called after every `saf_enqueue` and `saf_confirm`.

### 5.3 Test reset hook

```c
#ifdef TEST
void saf_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 6. Power-loss safety

### 6.1 Unconfirmed entry on power loss

If power is lost after `saf_dequeue()` but before `saf_confirm()`:
- `has_pending` is in RAM — lost.
- The CFL read pointer was not advanced — the entry still exists in flash.
- On next boot, `entry_count` is recovered from `cfl_get_occupancy()`.
  `has_pending` is `false`.
- The next `saf_dequeue()` reads the same entry again — it is re-published.

**Result:** at-least-once delivery for all buffered entries. QoS 1 alarms
re-publish at QoS 1 and receive PUBACK; QoS 0 telemetry is re-sent
without acknowledgement — duplicate telemetry at the cloud side is
acceptable (idempotent by timestamp per SD-04b UML note 3).

### 6.2 Partial flash write on power loss

If power is lost during `cfl_append()`:
- CircularFlashLog detects the incomplete record by CRC check on
  next boot scan.
- The record is silently skipped; `entry_count` is computed from
  validated records only.
- SAF's `entry_count` (from `cfl_get_occupancy()`) is therefore correct.

### 6.3 Drop during enqueue on power loss

If power is lost during the drop-oldest loop in `saf_enqueue()`:
- The drop may or may not have completed:
  - If `cfl_drop_oldest()` completed before power loss, the oldest
    entry is gone but the new append did not happen. One entry is lost.
  - If `cfl_drop_oldest()` did not complete, the ring is unchanged.
- In both cases, `cfl_get_occupancy()` recovers the correct count.

---

## 7. Synchronisation

CloudPublisher is the only component that calls `saf_*` functions.
All calls run in `CloudPublisherTask`. No mutex is needed inside
StoreAndForward — adding one would be dead code.

CircularFlashLog is also called only from `CloudPublisherTask` for
this SAF instance. The two components share the same task context;
no concurrent access exists.

---

## 8. Sequence integration

See `sequence-diagrams.md` SD-04 for inter-component flows.

| SD | Component role | Key functions |
|---|---|---|
| SD-04a | CloudPublisher calls `saf_enqueue()` for each outbound frame when the cloud link is down. If the buffer is full, SAF drops the oldest entry before appending (REQ-BF-020). | `saf_enqueue()` |
| SD-04b | CloudPublisher calls `saf_is_empty()` at reconnect to decide whether to drain (step 3). In the drain loop: `saf_dequeue()` → publish → `saf_confirm()` (steps 5–12). | `saf_is_empty()`, `saf_dequeue()`, `saf_confirm()` |
| SD-05 | If the cloud link is offline when an alarm fires, CloudPublisher buffers the alarm frame via `saf_enqueue()`. | `saf_enqueue()` |

---

## 9. Error and fault behaviour

All public functions return `saf_err_t`; callers must not ignore
non-OK returns. CloudPublisher is the sole consumer — it drives retry
and drain policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `SAF_ERR_OK` | Operation succeeded | — | Success | — | — |
| `SAF_ERR_NULL_PTR` | Required pointer argument is NULL | Return immediately; no flash access | Non-OK return | No retry — programming error | Logged at ERROR |
| `SAF_ERR_NO_RESOURCE` | Static pool exhausted (second `saf_create()`) | Return immediately | Non-OK return | No retry — programming error | Logged at ERROR |
| `SAF_ERR_NOT_INIT` | Function called before `saf_create()` | Return immediately; no flash access | Non-OK return | No retry — programming error | Logged at ERROR |
| `SAF_ERR_TOO_LARGE` | Payload exceeds SAF_PAYLOAD_MAX or is 0 bytes | Entry not stored; return error | Non-OK return | No retry — CloudPublisher truncates or drops | Logged at WARN |
| `SAF_ERR_EMPTY` | `saf_dequeue()` called on empty buffer | Return immediately; no CFL call | Non-OK return | No retry — CloudPublisher exits drain loop (normal termination) | Not logged (expected at end of drain) |
| `SAF_ERR_FLASH` | CircularFlashLog I/O failure (erase, write, read, or corrupt after skip exhaustion) | Return error; operation aborted | Non-OK return | No retry by SAF — CloudPublisher increments IHealthReport flash-error counter; stops drain until next reconnect | Logged at WARN; flash-error IHealthReport counter incremented |

---

## 10. Principles applied

- **P1 (Strict directional layering).** Application → Middleware: depends on CircularFlashLog (middleware) via `cfl_handle_t`. No cross-layer skip.
- **P2 (Dependency Inversion).** CloudPublisher (application, top-of-stack) depends on the opaque `saf_handle_t` and the `store_and_forward.h` public API, not the implementation. Decision SAF-D5.
- **P4 (Cross-cutting exception).** Logger accessed concretely per convention; IHealthReport injected via config.
- **P5 (Bounded resources).** Static pool of 1; `pending_payload` and `stage_buf` in `.bss`; no heap post-init.
- **P6 (Traces to requirements).** `saf_enqueue` → REQ-BF-000; `saf_dequeue`/`saf_confirm` → REQ-BF-010; drop-oldest loop → REQ-BF-020; `report_occupancy` → REQ-CC-010.
- **P7 (Pull-based downstream consumption).** CloudPublisher drains the queue on its own publishing schedule; SAF does not push messages.
- **P8 (Total error propagation).** 7 distinct error codes; CFL errors propagated as `SAF_ERR_FLASH` with log detail.
- **P9 (BARR-C).** Payload length `uint16_t`; entry count `uint32_t`; no floating-point; fixed-width types throughout.
- **P10 (Naming).** Prefix `saf_`; handle `saf_handle_t`; errors `SAF_ERR_*`. Decision SAF-D2.

---

## 11. Memory and sizing

| Item | Size |
|---|---|
| `struct saf_inst` context (handles, counts, flags) | ~24 B |
| `pending_hdr` (`saf_entry_t`) | 72 B |
| `pending_payload[SAF_PAYLOAD_MAX]` | 4 010 B |
| `stage_buf[SAF_STAGE_BUF_SIZE]` | 4 078 B |
| **Total RAM** | **~8.2 KB** |

Flash footprint: no direct flash use — SAF delegates to CircularFlashLog.
At worst-case entry size (4 092 bytes on flash), the 254-sector CFL data
ring holds **254 entries** (~254 minutes of telemetry at 60 s interval).
With average-sized entries (~300 bytes), capacity increases to ~3 400
entries. The `capacity` config parameter should use the conservative
worst-case estimate.

---

## 12. Unit-test plan

Test file: `tests/gateway/application/store_and_forward/test_store_and_forward.c`

### 12.1 Framework and mock strategy

| Item | Value |
|---|---|
| Framework | Unity (via Ceedling 0.31) |
| Mocking | None — see the note below the table. |
| CircularFlashLog | In-test fake FIFO (16 records) defining the `cfl_*` symbols the module calls. It returns `CFL_ERR_FULL` at capacity, rejects NULL out-parameters like the real module, carries a per-record corrupt flag and injects a failure per call. |
| IHealthReport | Strong-symbol spy for `health_update_buffer_occupancy()` recording the last count, capacity and percentage. |
| ILogger | `logger_stub.h` plus a no-op `logger_log()` in the test TU. |
| Test isolation | `saf_reset_for_test()` called in `setUp()` to clear static pool. |

The fake is used instead of CMock so that tests can drive a real FIFO
(enqueue several, drain in order, fill and overflow) rather than script each
call. The test never includes `circular_flash_log.h` directly, so Ceedling
does not link the real module.

### 12.2 Test cases

#### Create / lifecycle

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T01 | CFL mock: `cfl_get_occupancy` returns 0 entries | `saf_create(&cfg, &h)` | `SAF_ERR_OK`; `cfl_get_occupancy` called; IHealthReport occupancy pushed with count=0 |
| SAF-T02 | — | `saf_create(NULL, &h)` | `SAF_ERR_NULL_PTR`; handle unchanged |
| SAF-T02b | — | `saf_create(&cfg, NULL)` | `SAF_ERR_NULL_PTR` |
| SAF-T03 | One handle already created | `saf_create(&cfg, &h2)` | `SAF_ERR_NO_RESOURCE` |
| SAF-T04 | `saf_reset_for_test()` called; no `saf_create` | `saf_enqueue(h_stale, …)` | `SAF_ERR_NOT_INIT` |

#### Enqueue — happy path

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T05 | Handle created; CFL mock accepts append | `saf_enqueue(h, "sensors/temp", data, 64, 0)` | `SAF_ERR_OK`; `cfl_append` called with correctly serialised blob; IHealthReport occupancy pushed with count=1 |
| SAF-T06 | Handle created; enqueue 3 messages A, B, C | Dequeue and confirm all 3 | Messages emerge in FIFO order: A, B, C |
| SAF-T07 | Handle created | Enqueue with 5-char topic, then 40-char topic | Both succeed; `cfl_append` receives correctly variable-length serialised blobs |
| SAF-T07b | Handle created | `saf_enqueue(h, topic_64_chars, data, SAF_PAYLOAD_MAX, 1)` | `SAF_ERR_OK`; `cfl_append` called with raw_len == CFL_MAX_RECORD_BYTES (4 078) |

#### Enqueue — error / boundary

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T08 | Handle created; CFL mock: first `cfl_append` returns `CFL_ERR_FULL`, `cfl_drop_oldest` succeeds, second `cfl_append` succeeds | `saf_enqueue(h, topic, data, 64, 0)` | `SAF_ERR_OK`; `cfl_drop_oldest` called; entry_count unchanged (drop-1, append+1) |
| SAF-T09 | Handle created | `saf_enqueue(h, topic, data, SAF_PAYLOAD_MAX + 1, 0)` | `SAF_ERR_TOO_LARGE`; `cfl_append` not called |
| SAF-T09b | Handle created | `saf_enqueue(h, topic, data, 0, 0)` | `SAF_ERR_TOO_LARGE`; `cfl_append` not called |
| SAF-T10 | Handle created; CFL mock: `cfl_append` returns `CFL_ERR_FLASH_WRITE` | `saf_enqueue(h, topic, data, 64, 0)` | `SAF_ERR_FLASH`; entry_count unchanged |
| SAF-T11 | Handle created; CFL mock: `cfl_append` returns `CFL_ERR_FULL`, `cfl_drop_oldest` returns `CFL_ERR_FLASH_READ` | `saf_enqueue(h, topic, data, 64, 0)` | `SAF_ERR_FLASH`; entry_count unchanged |
| SAF-T12 | Handle created | `saf_enqueue(h, topic, NULL, 64, 0)` | `SAF_ERR_NULL_PTR` |

#### Dequeue / confirm

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T13 | Handle created; CFL mock: `cfl_peek_oldest` returns `CFL_ERR_EMPTY` | `saf_dequeue(h, &out)` | `SAF_ERR_EMPTY` |
| SAF-T14 | Handle created; CFL mock: `cfl_peek_oldest` returns valid serialised blob | `saf_dequeue(h, &out)` | `SAF_ERR_OK`; `out->topic`, `out->qos`, `out->payload_len`, `out->payload` match original data |
| SAF-T15 | SAF-T14 completed (has_pending true) | `saf_dequeue(h, &out)` | `SAF_ERR_OK`; returns same entry; `cfl_peek_oldest` NOT called again |
| SAF-T16 | SAF-T14 completed (has_pending true) | `saf_confirm(h)` | `SAF_ERR_OK`; `cfl_consume_oldest` called; entry_count decremented; IHealthReport occupancy pushed |
| SAF-T17 | Handle created; no prior dequeue | `saf_confirm(h)` | `SAF_ERR_OK` (idempotent); `cfl_consume_oldest` NOT called |
| SAF-T18 | Handle created; CFL mock: `cfl_peek_oldest` returns `CFL_ERR_FLASH_READ` | `saf_dequeue(h, &out)` | `SAF_ERR_FLASH` |
| SAF-T18b | Handle created; CFL mock: first `cfl_peek_oldest` returns `CFL_ERR_CORRUPT`, `cfl_consume_oldest` succeeds, second `cfl_peek_oldest` returns valid blob | `saf_dequeue(h, &out)` | `SAF_ERR_OK`; corrupt record consumed (skipped); valid entry returned; entry_count decremented by 1 for the skip |
| SAF-T19 | SAF-T14 completed (has_pending true); CFL mock: `cfl_consume_oldest` returns error | `saf_confirm(h)` | `SAF_ERR_FLASH`; `has_pending` still true |

#### Power-loss / boot recovery

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T20 | CFL mock: `cfl_get_occupancy` returns entry_count=5 | `saf_create(&cfg, &h)` | `SAF_ERR_OK`; `saf_get_occupancy` returns count=5; IHealthReport occupancy pushed |
| SAF-T21 | SAF-T20 completed; dequeue succeeds (has_pending true); no confirm; `saf_reset_for_test()`; CFL mock: `cfl_get_occupancy` still returns 5 | `saf_create(&cfg, &h2)`; `saf_dequeue(h2, &out)` | Entry count still 5; dequeue returns the same entry (CFL read pointer was never advanced) |

#### Occupancy

| ID | Precondition | Action | Expected |
|---|---|---|---|
| SAF-T22 | Handle created; CFL `cfl_get_occupancy` returns 0 | After `saf_create` | IHealthReport occupancy push: count=0, capacity=cfg.capacity, pct=0 |
| SAF-T23 | Handle created with capacity=100; enqueue 50 entries | After 50th enqueue | IHealthReport occupancy push: count=50, pct=50 |
| SAF-T24 | Handle created with capacity=10; enqueue 10 entries (CFL full); enqueue 11th (triggers drop+append) | After 11th enqueue | IHealthReport occupancy push: count=10, pct=100. Drop did not change net count (drop-1, append+1). |
| SAF-T25 | Handle created | `saf_get_occupancy(h, NULL, &cap)` | `SAF_ERR_NULL_PTR` |

#### Added during implementation

| ID | Scenario | Expected |
|---|---|---|
| SAF-T26 | `cfl_get_occupancy` fails during create | `SAF_ERR_FLASH`; the pool slot is not consumed |
| SAF-T27 | Derived limits | `SAF_PAYLOAD_MAX == 4010`, `SAF_HEADER_OVERHEAD_MAX == 68` |
| SAF-T28 | Empty or over-long topic | `SAF_ERR_TOO_LARGE`; `cfl_append` not called |
| SAF-T29 | Drop loop on a ring that cannot hold the entry | Ends on `CFL_ERR_EMPTY`; `SAF_ERR_FLASH` |
| SAF-T30 | Corrupt record whose consume fails | `SAF_ERR_FLASH`; no endless loop |
| SAF-T31 | CRC-valid record with a malformed blob | Skipped; the next valid entry is returned |
| SAF-T32 | `saf_dequeue` / `saf_confirm` NULL arguments | `SAF_ERR_NULL_PTR` |
| SAF-T33 | Enqueue drops the entry a caller is holding | Pending flag cleared; a later confirm consumes nothing |
| SAF-T34 | `saf_is_empty` across enqueue, dequeue, confirm | Follows the entry count |
| SAF-T35 | `saf_is_empty` NULL arguments | `SAF_ERR_NULL_PTR` |
| SAF-T36 | Zero capacity and an over-full ring | Percentage 0 / 100, clamped at 100 |

**Result: 39 tests, 39 pass, 0 fail, 0 ignored** (2026-10-11). The LLD's own
cases are 29; the rows above add 11, and T02b is folded into T02 (NULL
config, handle and `cfl`), giving 39. T04 is widened to cover every function.

### 12.3 Error-code cross-check

Every `saf_err_t` value must be exercised by at least one test case.

| Error code | Value | Covering test(s) |
|---|---|---|
| `SAF_ERR_OK` | 0 | SAF-T01, T05, T06, T07, T07b, T08, T14, T15, T16, T17, T18b, T20, T22, T23, T24 |
| `SAF_ERR_NULL_PTR` | 1 | SAF-T02, T02b, T12, T25 |
| `SAF_ERR_NO_RESOURCE` | 2 | SAF-T03 |
| `SAF_ERR_NOT_INIT` | 3 | SAF-T04 |
| `SAF_ERR_TOO_LARGE` | 4 | SAF-T09, T09b |
| `SAF_ERR_EMPTY` | 5 | SAF-T13 |
| `SAF_ERR_FLASH` | 6 | SAF-T10, T11, T18, T19 |

**All 7 error codes covered.** ✓

### 12.4 Coverage target

Branch coverage target: **90 %** on `store_and_forward.c`. SAF has
simpler control flow than CFL (no boot scan or sector management) —
90 % is achievable with the test cases above.

### 12.5 Cannot be host-tested

| Scenario | Reason | Verification method |
|---|---|---|
| Real CFL append/peek/consume over QSPI flash | Host has no MX25R6435F | Integration test on hardware |
| Power-loss recovery (actual) | Cannot simulate power cut on host | Manual test: pull power during enqueue, verify buffer integrity on reboot |
| Drain under real cloud reconnect timing | Depends on MQTT/TLS handshake latency | Integration test with AWS IoT Core |

---

### 12.6 Hardware bring-up test

Harness: `firmware/gateway/integration-tests/store_and_forward/main_test_store_and_forward.c`

Two phases around a real reset, because the CircularFlashLog pool holds one
instance. Phase A runs on a blank metadata zone; Phase B runs when the ring
holds the three entries Phase A left. Any other starting state is erased and
reset. The harness is destructive on the CircularFlashLog partition only.

| ID | What to observe | Verifies |
|---|---|---|
| TC-HW-SAF-001 | `qspi_flash_init()` returns OK | Prerequisite driver is up |
| TC-HW-SAF-002 | `saf_create()` on a blank partition: empty, occupancy 0 pushed | Create and the initial health push |
| TC-HW-SAF-003 | Enqueue, dequeue, confirm is byte-identical | Serialisation round trip through real flash |
| TC-HW-SAF-004 | Three entries of differing sizes drain oldest-first | FIFO order |
| TC-HW-SAF-005 | A second dequeue before confirm returns the same entry | Two-phase drain state |
| TC-HW-SAF-006 | A 64-char topic and 4 010-byte payload round-trips | Maximum entry fits one CFL record; sector advance |
| TC-HW-SAF-007 | DWT timings | Cost of enqueue, enqueue with erase, dequeue, confirm |
| TC-HW-SAF-008 | After RESET, three entries recovered | Count recovered from the CFL scan |
| TC-HW-SAF-009 | The entry dequeued but never confirmed is delivered again | §6.1 at-least-once behaviour |
| TC-HW-SAF-010 | The three entries drain in order, then `SAF_ERR_EMPTY` | Drain termination |
| TC-HW-SAF-011 | 254 maximum-size entries fill the ring; the 255th drops exactly one and raises one overflow event | REQ-BF-020 on real flash; occupancy stays at capacity |

**Status: run on hardware 2026-10-11 — all 11 cases PASS** (B-L475E-IOT01A, the
phase change made by an OpenOCD reset). Measured: enqueue of a 200-byte
entry 3 543 µs; dequeue 851 µs; confirm 33 µs; enqueue that forces a sector
erase 210 912 µs; filling all 254 sectors 36.5 s.

---

## 13. Open items

| ID | Item | Status | Resolution path |
|---|---|---|---|
| **SAF-O1** | Capacity configuration value. Worst-case = 254 entries (1 max-size record per sector). Average telemetry (~300 B payload) gives ~3 400 entries. Use worst-case (254) initially; tune at integration once real payloads are measured. | **Open** | Measure at integration. |
| **SAF-O2** | Partition split within the 1 MB QSPI CFL allocation. If Logger also requires a CFL instance (REQ-NF-500), the CFL pool must increase to 2 and the flash partition must be split. Currently SAF is the sole CFL consumer. | **Open** | Deferred to CFL LLD / system integration. |
| **SAF-O3** | **CloudPublisher still uses a bring-up placeholder.** `cloud_publisher.h` declares its own `saf_err_t`, `saf_entry_t` and `store_and_forward_enqueue/dequeue/confirm` (a 512-byte entry, a different error set, no `saf_handle_t`). The two sets are deliberately not guarded against each other, so a translation unit including both fails to compile. CloudPublisher must be migrated onto `store_and_forward.h`: `SAF_ERR_FULL` no longer exists because enqueue drops the oldest entry instead of failing. | **Open** | Migrate in the CloudPublisher follow-up; remove the placeholder block and its harness stand-ins together. |
| **SAF-O4** | **IHealthReport occupancy entry point is a placeholder.** The GW has no HealthMonitor, so `health_update_buffer_occupancy()` is declared in `store_and_forward.h` with a weak no-op default in `store_and_forward.c`, following CFL-O5. | **Open** | Replace with the real GW HealthMonitor call when it lands. |
| **SAF-O5** | **Duplicate delivery window inherited from CFL-O6.** CircularFlashLog persists only the head *sector*, so after a reset, entries already confirmed inside the head sector are replayed. This widens the at-least-once window described in §6.1 beyond the single unconfirmed entry. Acceptable for QoS 1 telemetry that is idempotent by timestamp (SD-04b note 3). | **Open** | Resolve together with CFL-O4 / CFL-O6. |

---

## 14. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SAF-D1 | ADT pattern (opaque handle, static pool of 1) | Gateway default. CFL handle and IHealthReport handle injected via config struct. |
| SAF-D2 | Function prefix `saf_` not `store_and_forward_` | Full prefix exceeds 30 characters per function call. `saf_` is unambiguous within the project. Consistent with CFL's `cfl_` prefix decision (CFL-D6). |
| SAF-D3 | Capacity injected via config struct, not computed from CFL internals | CFL does not expose partition size or sector count. Capacity is a worst-case planning number for the occupancy percentage denominator; the system integrator sets it from knowledge of the flash partition layout. |
| SAF-D4 | Drop-oldest uses `cfl_drop_oldest()` not `cfl_consume_oldest()` | `cfl_drop_oldest()` pushes `HEALTH_EVENT_BUFFER_OVERFLOW` via CFL's IHealthReport (REQ-BF-020 observability). `cfl_consume_oldest()` does not — it is for normal drain. |
| SAF-D5 | No vtable — ADT handle is the DIP seam | CloudPublisher depends on the `store_and_forward.h` header (public API), not the implementation. The opaque handle satisfies P2 (DIP) without the RAM and complexity cost of a function-pointer table. Consistent with CFL's ADT approach. |
| SAF-D6 | SAF_PAYLOAD_MAX derived from CFL constraint | `SAF_PAYLOAD_MAX = CFL_MAX_RECORD_BYTES − SAF_HEADER_OVERHEAD = 4078 − 68 = 4010`. Ensures every SAF entry fits in a single CFL record. The derivation is documented via `#define` in the header. |

---

## 15. File layout

```
firmware/gateway/application/store_and_forward/
├── store_and_forward.h    /* public API — handle, config, error enum, entry struct */
└── store_and_forward.c    /* implementation — serialisation, drop loop, drain state */

tests/gateway/application/store_and_forward/
└── test_store_and_forward.c  /* Unity + CMock (CircularFlashLog, IHealthReport) */
```

---

## 16. Implementation notes (GW v1.0)

Points where the code settles something the companion left open, or differs
from its pseudocode. None changes the public contract in §3.

| # | Topic | Implementation |
|---|---|---|
| 1 | `cfl_get_occupancy` out-parameters | §5.2.1 passes a NULL byte count. The merged CircularFlashLog rejects NULL for either out-parameter, so `saf_create` collects the byte total into a local and discards it. |
| 2 | Dropping the entry a caller holds | If `saf_enqueue` drops the oldest entry while it is dequeued but unconfirmed, `has_pending` is cleared. Otherwise a later `saf_confirm` would consume the record after it, which was never published. |
| 3 | Corrupt-record skip | §5.2.3 ignored the result of `cfl_consume_oldest`. A failed consume now returns `SAF_ERR_FLASH`, since the loop would otherwise peek the same record forever. |
| 4 | Malformed but CRC-valid blobs | Every length in the blob is checked against the record size. A blob that fails the checks is consumed and skipped like a corrupt record. |
| 5 | Topic validation | An empty topic, or one longer than `SAF_TOPIC_MAX`, returns `SAF_ERR_TOO_LARGE` (the format needs `1 ≤ topic_len ≤ 64`). `qos` is stored as given. |
| 6 | Handle checks | Every function returns `SAF_ERR_NULL_PTR` for a NULL handle and `SAF_ERR_NOT_INIT` for a handle that was never created; `saf_create` also rejects a NULL `config->cfl`. |
| 7 | Occupancy percentage | Clamped to 100, and 0 / 100 when the capacity is 0, so a ring of small entries cannot wrap `uint8_t` or divide by zero. |
| 8 | `saf_is_empty` | Answered from the tracked entry count, with no CircularFlashLog call. |
| 9 | Include path | `store_and_forward.h` includes `circular_flash_log/circular_flash_log.h` (resolved from `firmware/gateway/middleware`); Ceedling gets that root through `:paths: :include:`. |

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` | PASS — PROVIDES IStoreAndForward; USES CircularFlashLog, IHealthReport, ILogger |
| H2 | Root SRS requirements cited | PASS — REQ-BF-000/010/020, REQ-CC-010 |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — SAF-D1 |
| H5 | Error enum covers all failure modes | PASS — 7 error codes |
| H6 | Entry format documented with field offsets and size derivation | PASS — §4 |
| H7 | CFL API references match CFL v0.3 public API | PASS — cfl_append, cfl_peek_oldest, cfl_consume_oldest, cfl_drop_oldest, cfl_get_occupancy, cfl_is_empty |
| H8 | Power-loss safety documented for all paths | PASS — §6 |
| H9 | Open items have resolution path | PASS — O1, O2 deferred to integration; O3–O5 raised during implementation |
| H10 | Unit-test plan covers happy + error + boundary cases | PASS — 39 test cases, all passing; all 7 error codes covered (§12.3) |
| H11 | Test file path follows Gateway convention | PASS |
| H12 | P1–P10 compliance reviewed | PASS — §10 |
| H13 | Thread safety documented | PASS — §7, single-task access |
| H14 | `reset_for_test` hook specified | PASS — §5.3 |
| H15 | Decisions log complete | PASS — 6 decisions |
| H16 | Memory and sizing budget documented | PASS — §11, ~8.2 KB RAM |

| H17 | Hardware bring-up test specified and run | PASS — §12.6, 11 cases, all passed 2026-10-11 |

**Verdict: PASS — implemented and hardware-validated for Gateway.**

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
