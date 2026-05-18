# LLD Companion — StoreAndForward

**Board:** Gateway only.
**Layer:** Application.

Policy wrapper over `CircularFlashLog` that provides the drop-oldest
enqueue strategy, two-phase dequeue/confirm for power-loss-safe drain,
and buffer-occupancy reporting. Its sole caller is `CloudPublisher`.

**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** StoreAndForward in `components.md` (GW application layer)
---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `IStoreAndForward` |
| **Uses** | `ICircularFlashLog`, `IHealthReport`, `ILogger` |
| **Caller** | `CloudPublisher` only (single-task access — no mutex required) |

**Responsibility boundary.** `StoreAndForward` owns:
- The drop-oldest policy (REQ-BF-020).
- Serialisation of topic + QoS + payload into a flat byte blob for flash.
- Entry count tracking and occupancy percentage.
- Two-phase dequeue/confirm state.

`CircularFlashLog` owns:
- Raw append, forward-scan, and consume-oldest on QSPI flash.
- Ring-buffer wrap and head/tail pointer persistence.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Buffer outbound messages when offline | REQ-BF-000 | UC-10 |
| Drain in chronological order on reconnect | REQ-BF-010 | UC-11 |
| Drop oldest when full | REQ-BF-020 | UC-12 |
| Buffer occupancy in health metrics | REQ-CC-010 | UC-06 |

---

## 2. Public API — `IStoreAndForward`

```c
/* application/include/i_store_and_forward.h */

typedef enum {
    SAF_OK = 0,
    SAF_EMPTY,           /* dequeue called on empty buffer           */
    SAF_ERR_FLASH,       /* CircularFlashLog I/O failure             */
    SAF_ERR_NOT_INIT,
    SAF_ERR_OVERSIZE,    /* payload > SAF_PAYLOAD_MAX                */
} saf_err_t;

typedef struct {
    char     topic[SAF_TOPIC_MAX];    /* null-terminated, ≤ 64 chars */
    uint8_t  qos;                     /* 0 or 1                      */
    uint16_t payload_len;
    /* payload pointer valid only until next call to dequeue or confirm */
    const uint8_t *payload;
} saf_entry_t;

typedef struct IStoreAndForward IStoreAndForward;

struct IStoreAndForward {
    void *ctx;

    /* Serialise and append. If full, drops oldest then appends (REQ-BF-020).
     * Returns SAF_OK on success; SAF_ERR_FLASH on CFL write failure.     */
    saf_err_t (*enqueue) (void *ctx, const char *topic,
                          const uint8_t *buf, uint16_t len, uint8_t qos);

    /* Returns pointer to oldest entry in *out. Entry lives in an internal
     * staging buffer inside store_and_forward_t; it remains valid until
     * the next call to confirm() or dequeue(). Does not advance the CFL
     * read position — entry survives a power cycle until confirmed.        */
    saf_err_t (*dequeue) (void *ctx, const saf_entry_t **out);

    /* Consume the last dequeued entry. Advances the CFL read position.
     * Must be called exactly once after a successful publish.              */
    saf_err_t (*confirm) (void *ctx);

    /* Returns entry count and capacity for occupancy calculation.          */
    uint32_t  (*get_count)    (void *ctx);
    uint32_t  (*get_capacity) (void *ctx);
};
```

`SAF_TOPIC_MAX = 64`, `SAF_PAYLOAD_MAX = 4096` (matches `CP_JSON_BUF_SIZE`).

---

## 3. Internal design

```c
#define SAF_STAGE_BUF_SIZE  (SAF_TOPIC_MAX + 1U + 2U + SAF_PAYLOAD_MAX)
/* topic (64) + qos (1) + payload_len (2) + payload (4096) = 4163 bytes */

typedef struct {
    ICircularFlashLog *cfl;
    IHealthReport     *health_write;
    ILogger           *log;

    uint32_t  entry_count;    /* current entries in the ring      */
    uint32_t  capacity;       /* max entries (computed at init)    */

    /* Two-phase drain state */
    bool            has_pending;     /* true while dequeued but not confirmed */
    saf_entry_t     pending_hdr;     /* metadata of the pending entry         */
    uint8_t         pending_payload[SAF_PAYLOAD_MAX]; /* payload staging area */

    /* Serialisation / deserialisation scratch (reused for both directions) */
    uint8_t stage_buf[SAF_STAGE_BUF_SIZE];
} store_and_forward_t;
```

All buffers are static, allocated in `.bss`. No dynamic allocation (P8).
Total RAM: `sizeof(store_and_forward_t)` ≈ **8.4 KB**.

---

## 5. Entry format on flash

StoreAndForward serialises each entry into `stage_buf` before passing
it to `ICircularFlashLog.append()`. `CircularFlashLog` adds its own
length-prefix and CRC framing around this blob.

```
Byte offset  Field          Size
0            topic_len      1 byte    (not null byte; ≤ 64)
1            topic          topic_len bytes
1+topic_len  qos            1 byte
2+topic_len  payload_len    2 bytes   (big-endian)
4+topic_len  payload        payload_len bytes
```

Maximum serialised size: 1 + 64 + 1 + 2 + 4096 = **4164 bytes**.
`CircularFlashLog` frame overhead (length word + CRC16): 6 bytes.
Maximum on-flash size per entry: **4170 bytes**.

---

## 6. Algorithms

### 6.1 `enqueue`

```
enqueue(topic, buf, len, qos):
    if len > SAF_PAYLOAD_MAX:
        log_warn("SAF: payload %u > max %u — dropped", len, SAF_PAYLOAD_MAX)
        return SAF_ERR_OVERSIZE

    serialise(topic, buf, len, qos, → stage_buf, &raw_len)

    if entry_count >= capacity:
        cfl->consume_oldest(cfl)   /* REQ-BF-020: drop oldest */
        entry_count--
        log_warn("SAF: buffer full — oldest entry dropped")

    rc = cfl->append(cfl, stage_buf, raw_len)
    if rc != CFL_OK:
        log_error("SAF: flash append failed (%d)", rc)
        return SAF_ERR_FLASH

    entry_count++
    report_occupancy()
    return SAF_OK
```

`capacity` is computed at init: `cfl->partition_size(cfl) / (4170 + CFL_OVERHEAD)`.
Using the smallest possible entry size (1-byte topic, 1-byte payload) would
increase capacity; using the maximum entry size is the conservative bound
applied here. Tracked as **SAF-O1**.

### 6.2 `dequeue`

```
dequeue(out):
    if entry_count == 0:
        return SAF_EMPTY

    if has_pending:
        /* Previous entry not yet confirmed — return it again.
         * This handles the case where publish failed mid-drain.  */
        *out = &pending_hdr
        return SAF_OK

    rc = cfl->read_oldest(cfl, stage_buf, sizeof stage_buf, &raw_len)
    if rc != CFL_OK:
        return SAF_ERR_FLASH

    deserialise(stage_buf, raw_len, &pending_hdr, pending_payload)
    pending_hdr.payload = pending_payload
    has_pending = true
    *out = &pending_hdr
    return SAF_OK
```

`cfl->read_oldest()` reads without consuming. The entry remains in the
ring until `confirm()` is called.

### 6.3 `confirm`

```
confirm():
    if not has_pending:
        log_warn("SAF: confirm called with no pending entry")
        return SAF_OK   /* idempotent */

    rc = cfl->consume_oldest(cfl)
    if rc != CFL_OK:
        return SAF_ERR_FLASH

    has_pending = false
    entry_count--
    report_occupancy()
    return SAF_OK
```

### 6.4 `report_occupancy`

```
report_occupancy():
    pct = (entry_count * 100U) / capacity
    health_write->update_buffer_occupancy(health_write,
                                          entry_count,
                                          capacity,
                                          (uint8_t)pct)
```

Called after every `enqueue` and every `confirm`. Single-task access —
no mutex needed.

---

## 7. Power-loss safety

### 7.1 Unconfirmed entry on power loss

If power is lost after `dequeue` but before `confirm`:
- `has_pending` is in RAM → lost.
- The CFL read pointer was not advanced → the entry still exists in flash.
- On next boot, `entry_count` is recomputed by scanning CFL. `has_pending`
  is `false`.
- The next `dequeue` reads the same entry again → re-published.

**Result:** at-least-once delivery for all buffered entries. QoS 1 alarms
receive a PUBACK on re-publish; QoS 0 telemetry is re-sent without
acknowledgement — duplicate telemetry at the cloud side is acceptable
(idempotent by timestamp).

### 7.2 Partial flash write on power loss

If power is lost during `cfl->append()`:
- `CircularFlashLog` detects the incomplete frame by its length/CRC check.
- The entry is silently skipped on the next boot scan.
- `entry_count` is therefore computed from validated entries only.

---

## 8. Boot scan and occupancy initialisation

At `store_and_forward_init()`:

```
store_and_forward_init():
    capacity = cfl->partition_size(cfl) / SAF_MAX_ENTRY_ON_FLASH
    entry_count = cfl->count_valid_entries(cfl)   /* boot scan */
    has_pending = false
    report_occupancy()
    log_info("SAF: %u / %u entries at boot", entry_count, capacity)
```

`cfl->count_valid_entries()` scans the entire partition once.
Per session-summary open item **CFL-O2**: expected boot scan latency is
~40 ms. If measured at integration to exceed 100 ms, a CFL tail pointer
(persisted in a dedicated flash cell) eliminates the scan.

---

## 9. Concurrency

`CloudPublisher` is the only component that calls `IStoreAndForward`.
All calls run in `CloudPublisherTask`. No mutex is needed inside
`StoreAndForward`.

`ICircularFlashLog` is also called only from `CloudPublisherTask` for
this SAF instance. **Note:** a separate `CircularFlashLog` instance
(different base address / partition region within the 1 MB QSPI
allocation) is used by `Logger`. The two instances are independent;
they never share state or locks. Tracked as **SAF-O2** — the exact
partition split between Logger and SAF within the 1 MB region is a
`CircularFlashLog` LLD decision.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

```c
typedef enum {
    SAF_OK = 0,
    SAF_EMPTY,
    SAF_ERR_FLASH,
    SAF_ERR_NOT_INIT,
    SAF_ERR_OVERSIZE,
} saf_err_t;
```

| Error | Caller response |
|---|---|
| `SAF_EMPTY` | `CloudPublisher` stops the drain loop — normal termination |
| `SAF_ERR_FLASH` | Logged; CloudPublisher increments MQTT fail count via `IHealthReport`; drain stops |
| `SAF_ERR_OVERSIZE` | Entry dropped; logged; publish proceeds as if connected (re-serialise path) |

---

## 11. Initialisation

```c
saf_err_t store_and_forward_init(store_and_forward_t *self,
                                 ICircularFlashLog   *cfl,
                                 IHealthReport       *health_write,
                                 ILogger             *log);
```

Called from `LifecycleController` after `QspiFlashDriver` is ready and
`CircularFlashLog` has been initialised for the SAF partition region.

---

## 12. Memory and sizing

| Item | Size |
|---|---|
| `store_and_forward_t` context | ~80 B |
| `pending_payload[SAF_PAYLOAD_MAX]` | 4096 B |
| `stage_buf[SAF_STAGE_BUF_SIZE]` | 4164 B |
| **Total RAM** | **~8.3 KB** |

Flash footprint: the SAF partition within the 1 MB CFL allocation.
At 4170 bytes per entry, a 512 KB partition holds ~125 entries (≈
**125 minutes** of telemetry at 60 s interval). A 256 KB partition
holds ~62 entries (62 minutes). The exact split is a CFL LLD decision
(**SAF-O2**).

---

## 7. Unit-test plan

### 13.1 Unit tests — `tests/application/test_store_and_forward.c`

Mock `ICircularFlashLog` records all calls (append, read_oldest,
consume_oldest, count_valid_entries) and drives the return values.

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; `count_valid_entries` called; occupancy reported |
| Enqueue — space available | `cfl->append` called with correct serialised blob; entry_count++ |
| Enqueue — full | `cfl->consume_oldest` then `cfl->append` called; drop logged; entry_count unchanged |
| Enqueue — oversize | Entry dropped; `SAF_ERR_OVERSIZE` returned; `cfl->append` not called |
| Enqueue — CFL failure | `cfl->append` returns error; `SAF_ERR_FLASH` returned; entry_count unchanged |
| Dequeue — empty | `SAF_EMPTY` returned; no CFL call |
| Dequeue — entry available | `cfl->read_oldest` called; deserialised entry returned correctly; `has_pending = true` |
| Dequeue — pending re-return | Second dequeue before confirm returns the same entry without calling CFL |
| Confirm — happy path | `cfl->consume_oldest` called; entry_count--; `has_pending = false` |
| Confirm — no pending (idempotent) | No CFL call; `SAF_OK` returned |
| Power-loss resilience | Init with `count_valid_entries` returning N; dequeue; no confirm; re-init; count still N; dequeue returns same entry |
| Occupancy — empty | 0% reported |
| Occupancy — half full | 50% reported |
| Occupancy — full before drop | 100% reported; after drop-and-enqueue returns to ~99% |

### 13.2 Integration tests — on target

| Test | Setup |
|---|---|
| Enqueue survives power cycle | Enqueue 3 messages; power-cycle; reboot; verify 3 entries present and drainable |
| Drain in order | Enqueue A then B; drain; verify A published first (REQ-BF-010) |
| Drop oldest | Fill buffer; enqueue N+1; verify oldest was dropped; newest present |
| Partial drain on publish failure | Dequeue; inject publish failure; do not confirm; reconnect; verify same entry dequeued again |

---

## 8. Open items

| ID | Item |
|---|---|
| **SAF-O1** | Capacity calculation uses worst-case entry size (4170 bytes). Actual average entry size (telemetry ~800 bytes, alarms ~300 bytes) would give 3–5× higher capacity. Consider a two-tier estimate at integration once real payloads are measured. |
| **SAF-O2** | Partition split within the 1 MB QSPI CFL allocation between Logger and SAF — deferred to `circular-flash-log.md` LLD companion. |
| **CFL-O2** | Boot scan latency ~40 ms (expected). If >100 ms at integration, add CFL tail pointer to eliminate scan. |

---

## 15. References

- `docs/components.md` (GW StoreAndForward entry).
- `docs/flash-partition-layout.md` §6.2 (CircularFlashLog ring buffer strategy).
- `docs/sequence-diagrams.md` SD-04a (enqueue + drop-oldest), SD-04b (drain).
- `docs/state-machines.md` Machine 2 (Cloud Connectivity, GW — Disconnected state owns buffering).
- `docs/lld/circular-flash-log.md` (defines `ICircularFlashLog`).
- `docs/lld/cloud-publisher.md` (sole caller of `IStoreAndForward`).
- `docs/architecture-principles.md` P8 (no dynamic alloc).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
