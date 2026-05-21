# LLD Companion — CircularFlashLog

**Layer:** Middleware  
**Board:** Gateway (GW) only  
**Provides:** `ICircularFlashLog`  
**Consumes:** `IQspiFlash` (QspiFlashDriver), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-BF-000, REQ-BF-010, REQ-BF-020, REQ-NF-407  
**HLD ref:** `components.md` §Middleware — CircularFlashLog; `hld.md` §6.3; `flash-partition-layout.md` §5.2 (D40, D41); `sequence-diagrams.md` SD-04
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** CircularFlashLog in `components.md` (GW middleware layer)

---

## 1. Sources

CircularFlashLog implements a FIFO ring buffer over QSPI flash sectors.
It is the persistence backing for `StoreAndForward` — outbound MQTT
messages written when the cloud connection is down survive power cycles
and are replayed in chronological order on reconnection.

`StoreAndForward` (Application) owns the policy: when to enqueue, when
to drain, when to drop the oldest to make room. CircularFlashLog owns
only the mechanics: flash erase, write, read, record tracking, overflow
event emission.

Two distinct callers of `ICircularFlashLog`:

| Caller | Operation | Trigger |
|--------|-----------|---------|
| `StoreAndForward` | `append` | MQTT publish failed — cloud offline |
| `StoreAndForward` | `peek_oldest` + `consume_oldest` | Cloud reconnected — draining buffer |
| `StoreAndForward` | `drop_oldest` + `append` | Buffer full — must make room (REQ-BF-020) |

---

## 2. Flash partition

From `flash-partition-layout.md` §5.2:

| Item | Value |
|------|-------|
| Base address | `0x9002_0000` |
| End address | `0x9011_FFFF` |
| Total size | 1 MB |
| Sector size | 4 KB (MX25R6435F) |
| Total sectors | 256 |
| Layout | 2 sectors reserved for metadata (see §5); 254 data sectors |

---

## 3. Record format

Each record occupies one or more contiguous bytes within a sector.
A record never spans two sectors — it is written entirely within one
sector. If the remaining space in the current sector is insufficient,
the writer advances to the next sector (erasing it first).

```
Record layout (little-endian):
  Offset  0   4   magic         0xBEEF5A5AUL  — identifies a valid record
  Offset  4   4   seq_num       monotonically increasing across all records
  Offset  8   4   data_len      payload byte count
  Offset 12   N   data          opaque payload (MQTT message bytes)
  Offset 12+N 2   crc16         CRC-16/IBM over bytes [0 .. 12+N-1]
  [pad to 4-byte boundary]
```

Maximum payload per record: 4 KB sector − 18 bytes overhead ≈ **4 078 bytes**.
Maximum MQTT JSON telemetry payload is estimated at ~2 KB — within budget.
See CFL-O1 for OTA chunk size constraint.

```c
/* circular_flash_log.h */

#define CFL_MAGIC             0xBEEF5A5AUL
#define CFL_MAX_RECORD_BYTES  4078U
#define CFL_RECORD_HEADER_SZ  12U   /* magic + seq_num + data_len */
#define CFL_RECORD_CRC_SZ     2U
```

---

## 4. Data types

```c
/* circular_flash_log.h */

typedef enum {
    CFL_ERR_OK            = 0,
    CFL_ERR_NOT_INIT      = 1,
    CFL_ERR_NULL_ARG      = 2,
    CFL_ERR_TOO_LARGE     = 3,   /* payload exceeds CFL_MAX_RECORD_BYTES  */
    CFL_ERR_EMPTY         = 4,   /* no records to consume or peek         */
    CFL_ERR_FLASH_ERASE   = 5,
    CFL_ERR_FLASH_WRITE   = 6,
    CFL_ERR_FLASH_READ    = 7,
    CFL_ERR_CORRUPT       = 8,   /* record CRC mismatch on read           */
} cfl_err_t;
```

---

## 5. Sector layout — metadata + data ring

The partition is split into two zones:

```
Zone A — metadata (2 sectors × 4 KB = 8 KB):
  Sector 0 (0x9002_0000): head pointer slot A  ┐ A/B rotation (D40)
  Sector 1 (0x9002_1000): head pointer slot B  ┘

Zone B — data ring (254 sectors × 4 KB ≈ 1016 KB):
  Sector 2 .. 255 (0x9002_2000 .. 0x9011_FFFF)
```

**Head pointer persistence (D40):** The head pointer (index of the sector
containing the oldest unread record) is written to the metadata zone using
the same A/B rotation as ConfigStore: alternating slots, sequence number,
CRC32 over each slot. On power loss the previous valid head pointer is
recovered. The head pointer slot is small (12 bytes: magic, seq_num,
head_sector_index, crc32) — erase + write costs negligible time.

**Tail pointer:** The sector index of the next write location is stored
in RAM only. On boot it is recovered from the ring scan (§6.2).

---

## 2. Public API — `ICircularFlashLog`

```c
/**
 * @brief  Initialise CircularFlashLog.
 *
 * Reads the metadata zone to recover the head pointer. Scans the data
 * ring to locate the tail (highest valid seq_num). Must be called after
 * qspi_flash_driver_init().
 *
 * @param  health  IHealthReport handle for overflow event push.
 * @return CFL_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
cfl_err_t circular_flash_log_init(IHealthReport *health);

/**
 * @brief  Append a record to the tail of the ring.
 *
 * If the current tail sector has insufficient space, advances to the next
 * sector (erasing it). If the next sector is the head sector (ring full),
 * the append fails — caller must call circular_flash_log_drop_oldest()
 * first (REQ-BF-020, per StoreAndForward policy).
 *
 * This function does NOT drop oldest automatically — that is policy, and
 * policy belongs in StoreAndForward.
 *
 * @param  data  Payload to store.
 * @param  len   Byte count; must be ≤ CFL_MAX_RECORD_BYTES.
 * @return CFL_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_append(const uint8_t *data, uint32_t len);

/**
 * @brief  Read the oldest record without removing it.
 *
 * Used by StoreAndForward before publishing — the record is held until
 * the publish is confirmed, then removed with consume_oldest().
 *
 * @param[out] data_out  Caller buffer.
 * @param[out] len_out   Set to the record's data_len on success.
 * @param[in]  max_len   Size of data_out.
 * @return CFL_ERR_EMPTY if no records exist.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_peek_oldest(uint8_t  *data_out,
                                          uint32_t *len_out,
                                          uint32_t  max_len);

/**
 * @brief  Remove the oldest record (advance head).
 *
 * Called by StoreAndForward after a successful MQTT publish. Updates the
 * persistent head pointer in the metadata zone.
 *
 * @return CFL_ERR_EMPTY if no records exist.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_consume_oldest(void);

/**
 * @brief  Drop the oldest record to free space for a new append.
 *
 * Called by StoreAndForward when the ring is full and a new message must
 * be enqueued (REQ-BF-020). Pushes HEALTH_EVENT_BUFFER_OVERFLOW.
 *
 * @return CFL_ERR_EMPTY if no records exist to drop.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_drop_oldest(void);

/**
 * @brief  Query whether the ring is empty.
 * @return CFL_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_is_empty(bool *empty_out);

/**
 * @brief  Return buffer occupancy for health reporting.
 *
 * @param[out] entry_count  Number of records in the ring.
 * @param[out] byte_count   Total data bytes stored (excludes overhead).
 * @return CFL_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
cfl_err_t circular_flash_log_get_occupancy(uint32_t *entry_count,
                                            uint32_t *byte_count);
```

---

## 7. Ring mechanics

### 7.1 Append path

```
append(data, len):

1. Compute record_size = CFL_RECORD_HEADER_SZ + len + CFL_RECORD_CRC_SZ
   (+ padding to 4 bytes).
   if record_size > CFL_MAX_RECORD_BYTES: return CFL_ERR_TOO_LARGE.

2. Check remaining bytes in s_cfl.tail_sector.
   If insufficient: advance tail_sector = (tail_sector + 1) % DATA_SECTOR_COUNT.
   if tail_sector == head_sector: return CFL_ERR_FULL (no drop_oldest here).
   Erase tail_sector. → On erase fail: push health event; return ERR_FLASH_ERASE.

3. Build record header in RAM: magic, seq_num++, data_len = len.

4. Compute CRC-16/IBM over header + data.

5. Write header + data + CRC to tail offset in tail_sector.
   → On write fail: return ERR_FLASH_WRITE (record partially written;
     next scan on boot will reject it due to bad CRC).

6. Advance s_cfl.tail_offset by record_size.
   s_cfl.entry_count++; s_cfl.byte_count += len.
```

### 7.2 Consume / drop_oldest path

Consuming the oldest record advances the head. If the head sector is
now exhausted (all records consumed), head_sector advances to the next
sector. The consumed sector does not need to be erased — it will be
erased when the tail wraps around to it.

After advancing head_sector, the persistent head pointer is updated in
the metadata zone (A/B rotation write).

### 7.3 Boot scan

On `circular_flash_log_init()`:

```
1. Read metadata zone → recover head_sector from the valid A/B slot.

2. Scan data ring starting from head_sector:
   - For each sector, scan records by reading magic + seq_num + len.
   - Valid record: magic matches AND CRC is correct.
   - Invalid/erased: stop scanning that sector.
   - Track highest valid seq_num and its location → this is the tail.

3. Set s_cfl.tail_sector and s_cfl.tail_offset to byte after last valid record.

4. Rebuild entry_count and byte_count from scan.
```

Worst case: 254 sector reads × 4 KB = 1 016 KB at QSPI XIP speed (~25 MB/s)
≈ 40 ms. Acceptable. If measured >100 ms at integration, add a tail pointer
to the metadata zone to eliminate the scan — tracked as CFL-O2.

---

## 8. IHealthReport events

| Event constant | Trigger |
|----------------|---------|
| `HEALTH_EVENT_BUFFER_OVERFLOW` | `drop_oldest()` called — oldest record discarded to make space (REQ-BF-020). Pushed once per drop. |
| `HEALTH_EVENT_BUFFER_FLASH_ERR` | Flash erase or write failure during append. |

Both are direct-push events (events → push, Metric Producer Pattern).
Buffer occupancy is a counter — exposed via `get_occupancy()` and polled
by CloudPublisher for health metrics.

---

## 9. Wear analysis

The MX25R6435F is rated at 100 000 erase cycles per sector (CON-009).
With 254 data sectors in a ring, each sector is erased once per full
ring rotation.

Worst-case scenario: continuous enqueue/drain at the maximum anticipated
telemetry rate:

- Telemetry period: 60 s → 1 440 messages/day
- Average message: ~2 KB → fits in one sector
- One full ring rotation: 254 sectors = 254 messages → every ~0.18 days
- Erase cycles per sector per year: ~2 030
- Lifetime at rated 100 000 cycles: **~49 years**

No wear-levelling algorithm is needed beyond the natural circular rotation.
Document this calculation in code comments so the next engineer does not
need to re-derive it.

---

## 3. Internal design

```c
/* circular_flash_log.c */

#define DATA_SECTOR_COUNT   254U
#define DATA_SECTOR_BASE    2U    /* first data sector index within partition */

typedef struct {
    bool          initialised;
    IHealthReport *health;
    uint8_t       head_sector;     /* sector index (0..253) of oldest record */
    uint32_t      head_offset;     /* byte offset within head_sector          */
    uint8_t       tail_sector;     /* sector index of next write location     */
    uint32_t      tail_offset;     /* byte offset within tail_sector          */
    uint32_t      next_seq_num;    /* next sequence number to assign          */
    uint32_t      entry_count;     /* number of unconsumed records            */
    uint32_t      byte_count;      /* total payload bytes in ring             */
} CircularFlashLogState;

static CircularFlashLogState s_cfl;
```

No mutex. CircularFlashLog is called exclusively from `CloudPublisherTask`
(via `StoreAndForward`). All operations run in that task's context.

---


### Synchronisation

Caller serialises. This component holds no internal FreeRTOS synchronisation primitives. It is accessed exclusively from the owning task; no additional locking is required provided the component is not shared across task boundaries.

### circular_flash_log_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### circular_flash_log_append

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### circular_flash_log_consume_oldest

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### circular_flash_log_drop_oldest

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### circular_flash_log_is_empty

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### Principles applied

- **P1 (Strict directional layering).** Depends on IQspiFlash (driver layer); Logger and HealthMonitor are cross-cutting exceptions (P4).
- **P2 (Dependency Inversion).** Exposes `icircular_flash_log_t` vtable; StoreAndForward depends on `ICircularFlashLog`.
- **P4 (Cross-cutting concern exception).** Logger (ILogger) and HealthMonitor (IHealthReport) are referenced concretely; this is the accepted infrastructure exception documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Log descriptor and write-pointer in a static struct; flash partition pre-allocated; no heap.
- **P6 (Responsibility traces to requirements).** Write / read-oldest / advance-read functions trace to REQ-BF-000/010/020 flash-logging requirements.
- **P7 (Pull-based downstream consumption).** StoreAndForward reads log entries on its own drain schedule via `read_oldest()` / `advance_read()`; CircularFlashLog does not push to any consumer.
- **P8 (Total error propagation, no silent failures).** All operations return `circular_flash_log_err_t`; QSPI errors propagated; CRC mismatch on read is a distinct error code.
- **P9 (BARR-C coding standard).** Record header uses `uint32_t` fields; CRC stored as `uint32_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `circular_flash_log_`; interface `ICircularFlashLog` -> `icircular_flash_log_t`; errors `CIRCULAR_FLASH_LOG_ERR_*`.


## 11. Thread safety

CircularFlashLog is called only from `CloudPublisherTask`. No concurrent
access is possible. A mutex would be dead code — do not add one.

If a future requirement adds a second caller (e.g. a diagnostic logging
path from Logger), revisit this section and add a priority-inheritance
mutex at that point.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-04 | SD-04a: `StoreAndForward` calls `circular_flash_log_write()` to buffer outbound MQTT frames when the cloud link is down. SD-04b: `StoreAndForward` calls `circular_flash_log_read()` and `circular_flash_log_pop()` to drain buffered frames on reconnect | `circular_flash_log_write()`, `circular_flash_log_read()`, `circular_flash_log_pop()` |

---

## 6. Error and fault behaviour

All public functions return `cfl_err_t`; callers must not ignore non-OK returns.
No retry is performed inside CircularFlashLog — StoreAndForward (the sole caller)
applies retry or drain-stop policy as appropriate.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `CFL_ERR_NOT_INIT` | Function called before `circular_flash_log_init()` succeeded | Return error; no flash access | Non-OK return | No retry — programming error; boot sequence must initialise CFL before StoreAndForward | Caller logs at ERROR via ILogger |
| `CFL_ERR_NULL_ARG` | Null pointer passed to an output parameter | Return error; no flash access | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `CFL_ERR_TOO_LARGE` | Payload exceeds `CFL_MAX_RECORD_BYTES` | Return error; no write performed | Non-OK return | No retry — caller must split the payload; StoreAndForward limits entries to `SAF_PAYLOAD_MAX` which is within `CFL_MAX_RECORD_BYTES` | Caller logs at WARN via ILogger |
| `CFL_ERR_EMPTY` | `peek_oldest()` or `consume_oldest()` called on an empty ring | Return error; no flash read | Non-OK return | No retry — StoreAndForward treats this as normal drain termination | Not logged (expected condition) |
| `CFL_ERR_FLASH_ERASE` | QspiFlashDriver erase returned non-OK | Return error; sector may be partially erased | Non-OK return | No retry by CFL — StoreAndForward increments `IHealthReport` flash-error counter and stops the drain | Caller logs at WARN via ILogger; `HEALTH_EVENT_CONFIG_WRITE_FAIL` pushed to IHealthReport |
| `CFL_ERR_FLASH_WRITE` | QspiFlashDriver write returned non-OK | Return error; record not written | Non-OK return | No retry by CFL — StoreAndForward stops enqueue and logs the failure | Caller logs at WARN via ILogger; IHealthReport counter incremented |
| `CFL_ERR_FLASH_READ` | QspiFlashDriver read returned non-OK | Return error; record not readable | Non-OK return | No retry by CFL — StoreAndForward skips the record and advances the read pointer | Caller logs at WARN via ILogger |
| `CFL_ERR_CORRUPT` | Record CRC mismatch on read | Return error; record discarded | Non-OK return | No retry — record is unrecoverable; StoreAndForward calls `consume_oldest()` to advance past the corrupt entry | Caller logs at WARN via ILogger |


## 7. Unit-test plan

```c
#ifdef UNIT_TEST
static uint8_t s_flash_sim[256U * 4096U];
#define qspi_flash_erase(addr, len)       stub_flash_erase(s_flash_sim, addr, len)
#define qspi_flash_write(addr, buf, len)  stub_flash_write(s_flash_sim, addr, buf, len)
#define qspi_flash_read(addr, buf, len)   stub_flash_read(s_flash_sim,  addr, buf, len)
#endif
```

Minimum test cases:
- Fresh (all 0xFF) → `is_empty()` returns true; `peek_oldest()` returns `CFL_ERR_EMPTY`.
- `append()` + `peek_oldest()` → data matches.
- `append()` × N + `consume_oldest()` × N → FIFO order preserved.
- `append()` beyond single sector capacity → tail advances to next sector.
- `append()` until full (all 254 sectors) → returns non-OK; `drop_oldest()` frees space.
- `drop_oldest()` → `HEALTH_EVENT_BUFFER_OVERFLOW` pushed.
- Simulated write failure → `CFL_ERR_FLASH_WRITE` returned; state consistent on next operation.
- Boot scan after power-loss simulation (partial record at tail) → partial record rejected; previous valid records intact.
- Head pointer A/B rotation: verify that on boot after `consume_oldest()` the correct head_sector is recovered.
- `get_occupancy()` → entry_count and byte_count consistent with appended/consumed records.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| CFL-O1 | Maximum record size (4 078 bytes) must accommodate the largest caller payload. OTA download chunks (SD-06b) are the candidate upper bound — UpdateService LLD must confirm per-chunk size ≤ 4 078 bytes. If chunks are larger, the record format must be redesigned (multi-sector spanning or chunk-splitting at the caller). | Confirm at UpdateService LLD — chunk size must fit CFL_MAX_RECORD_BYTES | Open |
| CFL-O2 | Boot scan latency — measured at integration. If >100 ms, add a persisted tail pointer to the metadata zone using the same A/B rotation as the head pointer, eliminating the scan entirely. | Measure boot-scan latency at integration; add persisted tail pointer if > 100 ms | Open |
| CFL-O3 | Sector-boundary alignment of records — current design allows multiple records per sector if they fit. Alternative: one record per sector (simpler erase granularity, wastes flash). Decision: pack records (current) to maximise buffer capacity and minimise erase frequency. Revisit only if CFL-O1 forces a redesign. | Design decision confirmed (pack records); revisit only if CFL-O1 forces redesign | Open |
