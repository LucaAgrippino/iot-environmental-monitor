# LLD Companion — CircularFlashLog

**Document:** `docs/lld/middleware/circular-flash-log.md`
**Version:** 0.3 (Pass H complete — Gateway implemented)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Middleware
**Status:** Pass H complete — GW v1.0 implemented (2026-09-27), hardware validation pending
**Date:** May 2026 (Pass H July 2026; implementation reconciliation September 2026)

**HLD anchor:** CircularFlashLog in `components.md` (GW middleware layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Implements a circular append-and-consume log over flash sectors, preserving chronological order and overwriting oldest records when full. Reports buffer overflow events. | `components.md` |
| PROVIDES (upward) | ICircularFlashLog | `components.md` |
| USES (downward) | QspiFlashDriver, IHealthReport, ILogger | `components.md` |
| Root requirements | REQ-BF-000, REQ-BF-010, REQ-BF-020, REQ-NF-407 | `SRS.md` |
| HLD refs | `hld.md` §6.3; `flash-partition-layout.md` §5.2 (D40, D41); `sequence-diagrams.md` SD-04 | HLD |

CircularFlashLog implements a FIFO ring buffer over QSPI flash sectors.
It is the persistence backing for StoreAndForward — outbound MQTT
messages written when the cloud connection is down survive power cycles
and are replayed in chronological order on reconnection.

StoreAndForward (Application) owns the policy: when to enqueue, when
to drain, when to drop the oldest. CircularFlashLog owns only the
mechanics: flash erase, write, read, record tracking, overflow event.

All traffic arrives from one component in three distinct patterns:

| Caller | Operation | Trigger |
|---|---|---|
| `StoreAndForward` | `cfl_append` | MQTT publish failed — cloud offline |
| `StoreAndForward` | `cfl_peek_oldest` + `cfl_consume_oldest` | Cloud reconnected — draining the buffer |
| `StoreAndForward` | `cfl_drop_oldest` + `cfl_append` | Buffer full — must make room (REQ-BF-020) |

---

## 2. Flash partition

From `flash-partition-layout.md` §5.2:

| Item | Value |
|---|---|
| Base address | `0x9002_0000` (device byte offset `0x0002_0000`) |
| End address | `0x9011_FFFF` |
| Total size | 1 MB |
| Sector size | 4 KB (MX25R6435F) |
| Total sectors | 256 |
| Layout | 2 metadata + 254 data sectors |

`QspiFlashDriver` addresses the device from zero, so every address in
this document is used as a device byte offset: the partition base
`0x9002_0000` quoted by the HLD is offset `0x0002_0000` in driver calls.
The memory-mapped form appears in the HLD because the QSPI peripheral
also supports memory-mapped reads; this component uses indirect mode
only, through the driver.

---

## 3. Record format

Each record occupies contiguous bytes within a sector. A record never
spans two sectors. If insufficient space remains, the writer advances
to the next sector (erasing it first).

```
Record layout (little-endian):
  Offset  0   4   magic         0xBEEF5A5AUL
  Offset  4   4   seq_num       monotonically increasing
  Offset  8   4   data_len      payload byte count
  Offset 12   N   data          opaque payload (MQTT message bytes)
  Offset 12+N 2   crc16         CRC-16/IBM over bytes [0 .. 12+N-1]
  [pad to 4-byte boundary]
```

Maximum payload: 4 KB sector − 18 bytes overhead = **4 078 bytes**.
The largest MQTT JSON telemetry payload is estimated at ~2 KB, well
within budget; see CFL-O1 for the OTA chunk-size constraint.

```c
#define CFL_MAGIC             0xBEEF5A5AUL
#define CFL_MAX_RECORD_BYTES  4078u
#define CFL_RECORD_HEADER_SZ  12u   /* magic + seq_num + data_len */
#define CFL_RECORD_CRC_SZ     2u    /* CRC-16/IBM */
```

---

## 4. Public API

### 4.1 ADT pattern

CircularFlashLog follows the Gateway ADT default: an opaque handle
returned by `cfl_create()` from a static pool of 1. Both dependencies
(QspiFlashDriver, IHealthReport) are injected via the config struct.

### 4.2 Data types

```c
/* circular_flash_log.h */

#ifndef CIRCULAR_FLASH_LOG_H
#define CIRCULAR_FLASH_LOG_H

#include <stdint.h>
#include <stdbool.h>

#include "qspi_flash/qspi_flash.h"   /* iqspi_flash_t — see CFL-D7 */

/** @brief Opaque handle to a CircularFlashLog instance. */
typedef struct cfl_inst *cfl_handle_t;

typedef enum {
    CFL_ERR_OK          = 0,
    CFL_ERR_NULL_PTR    = 1,
    CFL_ERR_NO_RESOURCE = 2,  /**< Static pool exhausted.                    */
    CFL_ERR_NOT_INIT    = 3,
    CFL_ERR_TOO_LARGE   = 4,  /**< Payload exceeds CFL_MAX_RECORD_BYTES, or
                                   the peek buffer is smaller than the
                                   record.                                  */
    CFL_ERR_EMPTY       = 5,  /**< No records to consume or peek.            */
    CFL_ERR_FULL        = 6,  /**< Ring full — caller must drop_oldest first. */
    CFL_ERR_FLASH_ERASE = 7,
    CFL_ERR_FLASH_WRITE = 8,
    CFL_ERR_FLASH_READ  = 9,
    CFL_ERR_CORRUPT     = 10, /**< Record CRC mismatch on read.              */
} cfl_err_t;

/**
 * @brief CircularFlashLog creation configuration.
 */
typedef struct {
    const iqspi_flash_t    *qspi;    /**< QspiFlashDriver vtable (CFL-D7).   */
    health_monitor_handle_t health;  /**< IHealthReport handle; may be NULL. */
} cfl_config_t;
```

`health_monitor_handle_t`, `cfl_health_event_t` and
`health_report_push_buffer_event()` are declared in the same header as
guarded placeholders, because the Gateway has no HealthMonitor
implementation yet. See CFL-D7 and CFL-O5.

### 4.3 Public API — ICircularFlashLog

```c
/**
 * @brief Create and initialise a CircularFlashLog instance.
 *
 * Reads the metadata zone to recover the head pointer (A/B rotation).
 * Scans the data ring to locate the tail (highest valid seq_num).
 * Must be called after qspi_flash_init() has succeeded.
 *
 * @param[in]  config  Injected dependencies (QSPI + IHealthReport).
 * @param[out] handle  Receives the created handle on success.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR if config, handle or
 *         config->qspi is NULL; CFL_ERR_NO_RESOURCE if pool exhausted;
 *         CFL_ERR_FLASH_READ if metadata read fails.
 * @note Threading: task-context only. Call before scheduler starts.
 */
cfl_err_t cfl_create(const cfl_config_t *config, cfl_handle_t *handle);

/**
 * @brief Append a record to the tail of the ring.
 *
 * If the current tail sector has insufficient space, advances to the
 * next sector (erasing it). If the next sector is the head sector
 * (ring full), returns CFL_ERR_FULL — caller must call
 * cfl_drop_oldest() first (REQ-BF-020 policy belongs in
 * StoreAndForward).
 *
 * @param[in] handle  CFL handle.
 * @param[in] data    Payload to store.
 * @param[in] len     Byte count (1 .. CFL_MAX_RECORD_BYTES).
 * @return CFL_ERR_OK on success; CFL_ERR_TOO_LARGE if len is zero or too
 *         big; CFL_ERR_FULL if ring full; CFL_ERR_FLASH_ERASE/WRITE on
 *         flash failure.
 * @note Threading: task-context only, blocking during erase.
 */
cfl_err_t cfl_append(cfl_handle_t handle,
                      const uint8_t *data,
                      uint32_t len);

/**
 * @brief Read the oldest record without removing it.
 *
 * Used by StoreAndForward before publishing — record is held until
 * publish confirmed, then removed with cfl_consume_oldest().
 *
 * @param[in]  handle    CFL handle.
 * @param[out] data_out  Caller buffer.
 * @param[out] len_out   Receives the record's data_len.
 * @param[in]  max_len   Size of data_out.
 * @return CFL_ERR_OK on success; CFL_ERR_EMPTY if no records;
 *         CFL_ERR_TOO_LARGE if the record does not fit in max_len
 *         (len_out still receives the required length);
 *         CFL_ERR_CORRUPT if CRC mismatch.
 */
cfl_err_t cfl_peek_oldest(cfl_handle_t handle,
                           uint8_t *data_out,
                           uint32_t *len_out,
                           uint32_t max_len);

/**
 * @brief Remove the oldest record (advance head).
 *
 * Updates the persistent head pointer in the metadata zone when, and
 * only when, the head crosses into a new sector (CFL-D10).
 *
 * @param[in] handle  CFL handle.
 * @return CFL_ERR_OK on success; CFL_ERR_EMPTY if no records.
 */
cfl_err_t cfl_consume_oldest(cfl_handle_t handle);

/**
 * @brief Drop the oldest record to free space for a new append.
 *
 * Pushes CFL_HEALTH_EVENT_BUFFER_OVERFLOW via IHealthReport. Called by
 * StoreAndForward when ring is full (REQ-BF-020).
 *
 * @param[in] handle  CFL handle.
 * @return CFL_ERR_OK on success; CFL_ERR_EMPTY if nothing to drop.
 */
cfl_err_t cfl_drop_oldest(cfl_handle_t handle);

/**
 * @brief Query whether the ring is empty.
 *
 * @param[in]  handle     CFL handle.
 * @param[out] empty_out  Receives true if empty.
 * @return CFL_ERR_OK on success.
 */
cfl_err_t cfl_is_empty(cfl_handle_t handle, bool *empty_out);

/**
 * @brief Return buffer occupancy for health reporting.
 *
 * @param[in]  handle       CFL handle.
 * @param[out] entry_count  Number of records in the ring.
 * @param[out] byte_count   Total payload bytes (excludes overhead).
 * @return CFL_ERR_OK on success.
 */
cfl_err_t cfl_get_occupancy(cfl_handle_t handle,
                             uint32_t *entry_count,
                             uint32_t *byte_count);

#endif /* CIRCULAR_FLASH_LOG_H */
```

---

## 5. Sector layout — metadata + data ring

```
Zone A — metadata (2 sectors × 4 KB = 8 KB):
  Sector 0 (0x9002_0000): head pointer slot A  ┐ A/B rotation (D40)
  Sector 1 (0x9002_1000): head pointer slot B  ┘

Zone B — data ring (254 sectors × 4 KB ≈ 1016 KB):
  Sector 2 .. 255 (0x9002_2000 .. 0x9011_FFFF)
```

Ring sector indices used inside the implementation are 0-based over the
data ring, so the absolute partition sector is `2 + ring_index`.

**Head pointer persistence (D40):** A/B rotation with sequence number
and CRC-32 over each slot. The slot body is 12 bytes — magic, seq_num,
head_sector_index — followed by a 4-byte CRC-32 over that body, so a
slot occupies 16 bytes at the base of its sector. Each slot owns a
whole sector because erase granularity is 4 KB. Writing the inactive
slot leaves the active one readable throughout the erase-and-write
window, which is the point of the rotation: a power loss mid-write
leaves the previous head pointer recoverable.

**Tail pointer:** RAM only. Recovered from ring scan at boot (§6.4).

---

## 6. Internal design

### 6.1 Private struct and static pool

```c
/* circular_flash_log.c */

#define CFL_MAX_INSTANCES       1u
#define CFL_PARTITION_BASE_ADDR 0x00020000UL
#define CFL_DATA_SECTOR_COUNT   254u
#define CFL_DATA_SECTOR_BASE    2u
#define CFL_META_SLOT_COUNT     2u
#define CFL_META_SLOT_BODY_SZ   12u
#define CFL_META_SLOT_SZ        16u
#define CFL_MIN_RECORD_BYTES    16u   /* header + 1 payload byte + CRC, aligned */
#define CFL_SCAN_CHUNK_BYTES    64u

struct cfl_inst {
    /* Injected dependencies */
    const iqspi_flash_t    *qspi;
    health_monitor_handle_t health;

    /* Ring state */
    uint8_t   head_sector;     /**< Ring index (0..253) of oldest record. */
    uint32_t  head_offset;     /**< Byte offset within head_sector.       */
    uint8_t   tail_sector;     /**< Ring index of next write location.    */
    uint32_t  tail_offset;     /**< Byte offset within tail_sector.       */
    uint32_t  next_seq_num;    /**< Next sequence number to assign.       */
    uint32_t  entry_count;     /**< Unconsumed records.                   */
    uint32_t  byte_count;      /**< Total payload bytes in ring.          */

    /* Metadata A/B rotation state */
    uint8_t   meta_slot;       /**< Slot holding the current head pointer. */
    uint32_t  meta_seq_num;    /**< Sequence number of that slot.          */
    bool      meta_slot_valid; /**< False until a slot has been written.   */

    bool      in_use;
};

static struct cfl_inst g_pool[CFL_MAX_INSTANCES];
static uint8_t         g_count;
```

### 6.2 Append path

```
cfl_append(handle, data, len):

1. if len == 0 or len > CFL_MAX_RECORD_BYTES: return CFL_ERR_TOO_LARGE
   record_size = align4(HEADER + len + CRC)

2. if remaining bytes in tail_sector < record_size:
     next = (tail_sector + 1) % DATA_SECTOR_COUNT
     if next == head_sector and ring not empty: return CFL_ERR_FULL
     erase(next) → on fail: push health event; return ERR_FLASH_ERASE
     tail_sector = next; tail_offset = 0
     if the ring was empty, move head to the new tail as well and
       persist it (CFL-D11)

3. Build header: magic, seq_num, data_len
4. Compute CRC-16/IBM over header + data
5. Write payload, then CRC, then the header LAST (CFL-D8)
     → on fail: push health event; retire the rest of the sector
       (CFL-D9); return ERR_FLASH_WRITE
6. Advance tail_offset; next_seq_num++; entry_count++; byte_count += len
```

Writes are split at 256-byte page boundaries because
`qspi_flash_write_page()` rejects a longer transfer or one that crosses
a page (`QSPI_FLASH_ERR_LEN`).

### 6.3 Consume / drop_oldest path

Consuming advances the head. If the head sector holds no further valid
record, head_sector advances. The consumed sector is not erased — the
tail erases it on wrap. The persistent head pointer is rewritten only
when head_sector changes (CFL-D10).

When the last record is consumed the head is parked on the tail, so an
empty ring always reads as empty regardless of where the tail sits.

`cfl_drop_oldest()` is identical to `cfl_consume_oldest()` but
additionally pushes `CFL_HEALTH_EVENT_BUFFER_OVERFLOW`.

A record whose CRC fails is still consumable — StoreAndForward advances
past a corrupt entry after a `CFL_ERR_CORRUPT` peek (§10). Where the
header itself is unusable, the head advances by `CFL_MIN_RECORD_BYTES`.

### 6.4 Boot scan

```
cfl_create():

1. Read both metadata slots → recover head_sector from the valid slot
   with the higher seq_num. Fresh flash: head_sector = 0.
2. Scan the data ring forward from head_sector:
   - For each sector, walk records from offset 0
   - Valid record: magic matches AND data_len plausible AND CRC correct
   - First invalid record ends that sector's walk
   - A sector with no valid record at offset 0 is the sector past the
     tail and ends the whole scan
3. Set tail_sector/tail_offset to the byte after the last valid record
4. Rebuild entry_count, byte_count and next_seq_num
5. If the bytes from tail_offset to the end of the tail sector are not
   all 0xFF, retire the remainder of that sector (CFL-D9)
```

Slack left at the end of a full sector does not end the scan, because
the following sector still begins with a valid record.

Worst case: 254 sectors × 4 KB = 1 016 KB at QSPI speed (~25 MB/s)
≈ 40 ms. Measured at integration by TC-HW-CFL-002; see CFL-O2 if this
exceeds 100 ms.

### 6.5 IHealthReport events

| Event | Trigger |
|---|---|
| `CFL_HEALTH_EVENT_BUFFER_OVERFLOW` | `cfl_drop_oldest()` — oldest discarded (REQ-BF-020) |
| `CFL_HEALTH_EVENT_BUFFER_FLASH_ERR` | Flash erase or write failure during append |

Direct-push events (Metric Producer Pattern). Occupancy is polled via
`cfl_get_occupancy()`. A rejected append that never reached flash
(`CFL_ERR_FULL`, `CFL_ERR_TOO_LARGE`) raises no event.

### 6.6 Test reset hook

```c
#ifdef TEST
void cfl_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 7. Wear analysis

MX25R6435F: 100 000 erase cycles per sector (CON-009). 254 data
sectors in ring.

Worst case (continuous enqueue/drain at max telemetry rate):
- 60 s period → 1 440 messages/day
- ~2 KB average → fits in one sector
- Full rotation: 254 messages ≈ every 0.18 days
- Erase cycles/sector/year: ~2 030
- Lifetime at 100 000 cycles: **~49 years**

No wear-levelling needed beyond natural circular rotation **for the
data ring**. The metadata zone is a different matter: it has only two
sectors and is erased on every head-sector advance, so its worst-case
cycle count is three orders of magnitude higher than a data sector's.
See CFL-O4.

---

## 8. Synchronisation

Caller serialises. CircularFlashLog is called exclusively from
CloudPublisherTask (via StoreAndForward). No concurrent access. No
mutex — adding one would be dead code.

If a future requirement adds a second caller (for example a diagnostic
logging path from Logger, which `flash-partition-layout.md` §5.2 lists
this partition as also serving), revisit this section and add a
priority-inheritance mutex at that point.

Separately, the *driver* below is shared: QSPID-O1 requires a
cross-cutting `qspi_flash_mutex` that all three QSPI middleware
consumers acquire before calling `QspiFlashDriver`. CircularFlashLog is
the first of the three to be implemented, so that mutex does not exist
yet and is not referenced here. See CFL-O7.

---

## 9. Sequence integration

| SD | Role | Key functions |
|---|---|---|
| SD-04a | StoreAndForward calls `cfl_append()` to buffer outbound MQTT frames when cloud is down | `cfl_append()` |
| SD-04b | StoreAndForward calls `cfl_peek_oldest()` + `cfl_consume_oldest()` to drain on reconnect | `cfl_peek_oldest()`, `cfl_consume_oldest()` |

---

## 10. Error and fault behaviour

All public functions return `cfl_err_t`; callers must not ignore non-OK
returns. No retry is performed inside CircularFlashLog — StoreAndForward
(the sole caller) applies retry or drain-stop policy as appropriate.

| Error | Cause | Local behaviour | Retry | Observability |
|---|---|---|---|---|
| `CFL_ERR_NULL_PTR` | Required pointer is NULL | Return immediately; no flash access | No — programming error | Caller logs at ERROR via ILogger |
| `CFL_ERR_NO_RESOURCE` | Pool exhausted | Return immediately | No — programming error | Caller logs at ERROR via ILogger |
| `CFL_ERR_NOT_INIT` | Handle not produced by `cfl_create()` | Return immediately; no flash access | No — programming error; boot sequence must create CFL before StoreAndForward | Caller logs at ERROR via ILogger |
| `CFL_ERR_TOO_LARGE` | Payload zero or > CFL_MAX_RECORD_BYTES; or peek buffer too small | Return immediately; no write | No — caller must split the payload or enlarge the buffer; StoreAndForward limits entries to `SAF_PAYLOAD_MAX` within `CFL_MAX_RECORD_BYTES` | Caller logs at WARN via ILogger |
| `CFL_ERR_EMPTY` | Peek/consume/drop on empty ring | Return immediately | No — normal drain termination | Not logged (expected condition) |
| `CFL_ERR_FULL` | Ring full; tail would overwrite head | Return error; nothing written | No — caller must `cfl_drop_oldest()` first | Caller logs at WARN via ILogger |
| `CFL_ERR_FLASH_ERASE` | QSPI erase failure | Health event pushed; tail unchanged | No retry by CFL — StoreAndForward stops the drain | `CFL_HEALTH_EVENT_BUFFER_FLASH_ERR` pushed |
| `CFL_ERR_FLASH_WRITE` | QSPI write failure | Health event pushed; remainder of the sector retired; partial record has no header so the boot scan skips it | No retry by CFL — StoreAndForward stops enqueue | `CFL_HEALTH_EVENT_BUFFER_FLASH_ERR` pushed |
| `CFL_ERR_FLASH_READ` | QSPI read failure | Return error; record not readable | No retry by CFL — StoreAndForward skips the record | Caller logs at WARN via ILogger |
| `CFL_ERR_CORRUPT` | Record CRC mismatch | Return error; record left in place | No — record unrecoverable; StoreAndForward calls `cfl_consume_oldest()` to advance past it | Caller logs at WARN via ILogger |

---

## 11. Principles applied

- **P1 (Strict directional layering).** Depends on QspiFlashDriver (driver); IHealthReport and ILogger are cross-cutting (P4).
- **P2 (DIP).** StoreAndForward (application) depends on the opaque `cfl_handle_t`, not the concrete implementation. The driver below is itself consumed through the `iqspi_flash_t` vtable rather than by direct call, so the ring mechanics are testable without any CMSIS mock.
- **P4 (Cross-cutting exception).** Logger and IHealthReport per convention.
- **P5 (Bounded resources).** Static pool; flash partition pre-allocated; no heap. Stack use is bounded independent of record size: the CRC is folded through a 64-byte window.
- **P6 (Traces to requirements).** Append/peek/consume trace to REQ-BF-000/010/020.
- **P7 (Pull-based).** StoreAndForward reads on its own drain schedule; CFL does not push.
- **P8 (Total error propagation).** 10 distinct error codes; flash errors and CRC mismatch each distinguished.
- **P9 (BARR-C).** Record fields `uint32_t`; CRC stored as `uint16_t` (record) / `uint32_t` (metadata A/B slot); no floating-point.
- **P10 (Naming).** Prefix `cfl_`; handle `cfl_handle_t`; errors `CFL_ERR_*`.

---

## 12. Unit-test plan

Test file: `tests/gateway/middleware/circular_flash_log/test_circular_flash_log.c`

Flash simulated via a static RAM array (`uint8_t s_flash_sim[256 * 4096]`)
behind a fake `iqspi_flash_t` vtable. The fake enforces the real
driver's constraints — 256-byte page limit, no page crossing, and
AND-only programming — so NOR 1→0 semantics are reproduced on the host.
IHealthReport is a counting spy. No CMock mock and no CMSIS mock are
required.

| ID | Scenario | Expected | Result |
|---|---|---|---|
| CFL-T01 | `cfl_create` happy path (fresh flash, all 0xFF) | Handle returned; `is_empty` = true; no erase performed | PASS |
| CFL-T02 | `cfl_create` NULL config / handle / qspi | Returns `CFL_ERR_NULL_PTR` | PASS |
| CFL-T03 | `cfl_create` pool exhaustion | Returns `CFL_ERR_NO_RESOURCE` | PASS |
| CFL-T04 | `cfl_peek_oldest` / consume / drop on empty ring | Returns `CFL_ERR_EMPTY`; no overflow event | PASS |
| CFL-T05 | `cfl_append` + `cfl_peek_oldest` | Data matches; peek is non-destructive | PASS |
| CFL-T06 | N appends + N consumes | FIFO order preserved | PASS |
| CFL-T07 | Append beyond single sector | Tail advances to next sector; both records readable | PASS |
| CFL-T08 | Append until full (254 sectors) | Returns `CFL_ERR_FULL`; no flash-error event | PASS |
| CFL-T09 | Full → `cfl_drop_oldest` → append succeeds | Space freed; `CFL_HEALTH_EVENT_BUFFER_OVERFLOW` pushed | PASS |
| CFL-T10 | Simulated write failure mid-record | Returns `CFL_ERR_FLASH_WRITE`; occupancy unchanged; next append recovers in a clean sector | PASS |
| CFL-T11 | CRC corruption on read | Returns `CFL_ERR_CORRUPT`; record still consumable | PASS |
| CFL-T12 | Boot scan after power-loss (partial tail record) | Partial record rejected; previous records intact; next append avoids the dirty region | PASS |
| CFL-T13 | Head pointer A/B recovery after consume | Correct head_sector recovered on reboot | PASS |
| CFL-T14 | `cfl_get_occupancy` consistency | entry_count and byte_count match append/consume/drop history | PASS |
| CFL-T15 | Payload exactly CFL_MAX_RECORD_BYTES | Accepted; fits in sector | PASS |
| CFL-T16 | Payload CFL_MAX_RECORD_BYTES + 1, and zero length | Returns `CFL_ERR_TOO_LARGE` | PASS |
| CFL-T17 | NULL and uninitialised handles on every operation | `CFL_ERR_NULL_PTR` / `CFL_ERR_NOT_INIT` | PASS |
| CFL-T18 | Peek with an undersized buffer | `CFL_ERR_TOO_LARGE`; `len_out` reports the required length; record untouched | PASS |
| CFL-T19 | Metadata read failure, and erase failure on tail advance | `CFL_ERR_FLASH_READ` / `CFL_ERR_FLASH_ERASE`; pool slot not consumed by the failed create | PASS |
| CFL-T20 | Ring wrap past sector 253 with drain in step | Sectors reused in order; records still correct after the wrap | PASS |

**Total: 20 pass, 0 failed, 0 ignored** (2026-09-27).

CFL-T17..T20 were added during implementation: T17/T18 close guard and
buffer-size paths the §12 table did not name, T19 covers the read and
erase failure branches that only T10's write branch exercised, and T20
walks the ring index past its wrap point.

---

## 12.1 Hardware bring-up test

Harness: `firmware/gateway/integration-tests/circular_flash_log/main_test_circular_flash_log.c`

The static pool holds one instance and `cfl_reset_for_test()` does not
exist in a production build, so a second `cfl_create()` in one run is
impossible. Persistence is therefore proven across a real reset: the
harness runs **Phase A** when `cfl_create()` reports an empty ring, and
**Phase B** when it recovers a populated one.

| ID | What to observe | Verifies |
|---|---|---|
| TC-HW-CFL-001 | `qspi_flash_init()` returns OK | Prerequisite driver is up |
| TC-HW-CFL-002 | `cfl_create()` returns OK on a blank partition; DWT-timed | Boot scan works on silicon; supplies the CFL-O2 measurement |
| TC-HW-CFL-003 | Append then peek returns the payload byte-identical | Record round-trip through real flash |
| TC-HW-CFL-004 | Occupancy reports 1 entry, payload size in bytes | Counter accuracy |
| TC-HW-CFL-005 | Three records of differing sizes drain oldest-first | FIFO order on hardware |
| TC-HW-CFL-006 | A max-size record forces an erase and sector advance | Sector advance and head-pointer rewrite |
| TC-HW-CFL-007 | DWT timings for append, append-with-erase and consume | Cost against the QSPID-O4 bench figures (erase 81 ms, page program 3181 µs) |
| TC-HW-CFL-008 | After RESET, `cfl_create()` recovers the two records Phase A left | Boot scan over a populated ring |
| TC-HW-CFL-009 | The recovered oldest record is the expected one | Head pointer came from the metadata A/B slot, not a default of 0 |
| TC-HW-CFL-010 | `cfl_drop_oldest()` reports the overflow; ring drains to empty | Overflow event and drain path on hardware |

**Status: not yet run.** The harness is destructive on the real
CircularFlashLog partition (Phase A erases the metadata zone and six
data sectors) and leaves ConfigStore, CertStore and the OTA staging
area untouched.

---

## 13. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| CFL-O1 | Max record size (4 078 B) must accommodate the largest caller payload. OTA download chunks (SD-06b) are the candidate upper bound; if chunks exceed 4 078 B the record format needs multi-sector spanning or caller-side splitting. | **Open** | Confirm at UpdateService LLD. |
| CFL-O2 | Boot scan latency — if > 100 ms, add a persisted tail pointer to the metadata zone using the same A/B rotation as the head pointer. | **Open** | Measure with TC-HW-CFL-002 at integration. |
| CFL-O3 | Pack records (current) vs one-per-sector. | **Resolved** | Pack records to maximise capacity and minimise erase frequency. Revisit only if CFL-O1 forces redesign. |
| CFL-O4 | **Metadata-zone wear (found during implementation).** The A/B rotation erases one of only two sectors on every head-sector advance. At the §7 worst case (1 440 messages/day, ~2 KB each, two records per sector) that is ~720 advances/day, ~131 000 erases per slot per year — past the 100 000-cycle rating inside a year, against ~49 years for a data sector. It only bites under sustained cloud outage, since an idle ring writes no metadata. Mitigation without a format change: treat each metadata sector as log-structured, appending a fresh 16-byte slot into the next free position and erasing only when the sector fills, which divides the erase count by 256. | **Open** | Decide before StoreAndForward drives CFL in anger; prefer the log-structured slot. |
| CFL-O5 | **IHealthReport is a placeholder.** The Gateway has no HealthMonitor, so `health_monitor_handle_t`, `cfl_health_event_t` and `health_report_push_buffer_event()` are guarded declarations in `circular_flash_log.h`, following the convention in `cloud_publisher.h`. The GW firmware cannot link until a real implementation or a deliberate stub exists. | **Open** | Replace with the real interface at GW HealthMonitor LLD; reconcile the event enum with `health_event_t`. |
| CFL-O6 | **Consumed records inside the head sector are replayed after a reset.** Only `head_sector` is persisted (CFL-D5), not `head_offset`, so a reboot rediscovers records already consumed within that sector. Acceptable under MQTT QoS 1 at-least-once delivery, but it is a duplicate-delivery window that StoreAndForward's own design should acknowledge. Persisting `head_offset` would fix it at the cost of a metadata write per consume — which CFL-O4 argues against. | **Open** | Confirm acceptable at StoreAndForward LLD, or resolve together with CFL-O4. |
| CFL-O7 | **Shared driver mutex (QSPID-O1) is still absent.** `ConfigStore`, `CircularFlashLog` and `FirmwareStore` will all reach `QspiFlashDriver` from different tasks. QSPID-O1 requires a cross-cutting `qspi_flash_mutex` in `task-breakdown.md` §7, acquired by all three before any driver call. CircularFlashLog is the first of the three to land, so nothing is broken yet, but the second consumer makes it a live defect. | **Open** | HLD escalation per QSPID-O1, before the second QSPI consumer is implemented. |
| CFL-O8 | **QSPID-O4 decision is now due.** The driver's WIP poll spins for the whole erase — measured 81 ms — blocking the calling task and everything at or below its priority. CircularFlashLog is the consumer QSPID-O4 named: its erases land during cloud outages, exactly when alarms are buffering against the REQ-NF-113 500 ms budget (81 ms ≈ 16%). | **Open** | Decide spin vs `vTaskDelay(1)` vs a low-priority erase task, per QSPID-O4. |

---

## 14. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| CFL-D1 | ADT pattern (opaque handle, static pool of 1) | Gateway default. QspiFlash and IHealthReport injected via config struct. |
| CFL-D2 | CFL does not auto-drop oldest on full | Drop-oldest is policy; policy belongs in StoreAndForward (P1). CFL returns CFL_ERR_FULL. |
| CFL-D3 | Records packed within sectors (not one-per-sector) | Maximises buffer capacity; minimises erase frequency. |
| CFL-D4 | CRC-16/IBM for record integrity; CRC-32 for metadata A/B slot | Record CRC is 2 bytes (space-efficient within 4 KB sector). Metadata CRC is 4 bytes (head pointer is critical). |
| CFL-D5 | Head pointer persisted; tail recovered by scan | Head must survive power loss (oldest record). Tail scan is fast (~40 ms) and avoids a second persisted A/B pair. See CFL-O6 for the duplicate-delivery consequence. |
| CFL-D6 | Function prefix `cfl_` not `circular_flash_log_` | Full prefix exceeds 30 characters per function call — unwieldy. `cfl_` is unambiguous within the project. |
| CFL-D7 | QspiFlashDriver injected as `const iqspi_flash_t *`, not a handle | The GW driver is a platform singleton with no handle type; it already publishes the `iqspi_flash_t` vtable and a `qspi_flash_driver` pointer for exactly this purpose (`qspi_flash.h`). Injecting the vtable keeps CFL-D1's dependency injection, matches the driver's documented intent, and lets the unit tests substitute a RAM-backed fake with no CMSIS mock. Supersedes the `qspi_flash_handle_t` field named in the v0.2 draft, which never existed. |
| CFL-D8 | The record header is written **last** | The magic is what makes a record discoverable, so writing it after the payload and CRC makes it the commit point. A power loss part-way through leaves a region the boot scan skips, rather than a valid-looking header promising a payload that was never written. |
| CFL-D9 | A sector whose tail region is not pristine is retired, not reused | NOR programming only clears bits, so writing a new record over the partially programmed remains of an interrupted one stores the bitwise AND of the two — a record that fails CRC on every read and halts the scan at that offset permanently. Detected at boot by scanning for non-0xFF bytes, and on an in-session write failure by retiring the remainder directly. Costs at most the tail of one sector. |
| CFL-D10 | The persistent head pointer is rewritten only when `head_sector` changes | The slot stores a sector index, so a write per consumed record would add no information while erasing a metadata sector each time. See CFL-O4. |
| CFL-D11 | On an empty ring the head travels with the tail | An empty ring parks the head on the tail. If an append then crosses into a new sector the head has to move too, or the first record written after the move would be sought at a stale head position holding nothing. |

---

## 15. File layout

```
firmware/gateway/middleware/circular_flash_log/
├── circular_flash_log.h    /* public API — handle, config, error enum */
└── circular_flash_log.c    /* implementation — ring mechanics, CRC    */

firmware/gateway/integration-tests/circular_flash_log/
└── main_test_circular_flash_log.c  /* two-phase hardware bring-up     */

tests/gateway/middleware/circular_flash_log/
└── test_circular_flash_log.c  /* Unity + flash simulation array       */
```

No new file was added to `tests/support/` or `tests/mocks/`: the vtable
injection of CFL-D7 means the fake flash lives in the test TU itself.

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` | PASS — PROVIDES ICircularFlashLog; USES QspiFlashDriver, IHealthReport, ILogger |
| H2 | Root SRS requirements cited | PASS — REQ-BF-000/010/020, REQ-NF-407 |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — CFL-D1, CFL-D7 |
| H5 | Error enum covers all failure modes incl. CFL_ERR_FULL | PASS — 10 error codes |
| H6 | Flash partition documented with addresses | PASS — §2, including the offset-vs-mapped distinction |
| H7 | Record format documented with field offsets and CRC | PASS — §3 |
| H8 | Wear analysis documented | PASS — §7; data ring ~49 years, metadata zone raised as CFL-O4 |
| H9 | Open items have resolution path | PASS — O3 resolved; O1, O2, O4–O8 deferred with a named owner step |
| H10 | Unit-test plan covers happy + error + boundary cases | PASS — 20 test cases, all passing |
| H11 | Test file path follows Gateway convention | PASS |
| H12 | P1–P10 compliance reviewed | PASS — §11 |
| H13 | Thread safety documented | PASS — §8, single-task access; shared-driver gap tracked as CFL-O7 |
| H14 | `reset_for_test` hook specified | PASS — §6.6 |
| H15 | Decisions log complete | PASS — 11 decisions |
| H16 | Hardware bring-up test specified | PASS — §12.1, 10 cases; not yet run |

**Verdict: PASS — implemented for Gateway; hardware validation outstanding.**
