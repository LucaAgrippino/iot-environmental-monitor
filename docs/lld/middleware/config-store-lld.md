# LLD Companion — ConfigStore (v0.3)

**Layer:** Middleware
**Boards:** Field Device (FD) · Gateway (GW)
**Provides:** `IConfigStore`
**Consumes:** `IQspiFlash` (QspiFlashDriver), `IHealthReport` (HealthMonitor)
**Cross-cutting:** `ILogger` (P4 exception)
**SRS traces:** REQ-DM-090, REQ-NF-214
**HLD ref:** `components.md` §Middleware — ConfigStore; `hld.md` §5.6; `flash-partition-layout.md` §6.1 (D39)
**Version:** 0.3 (Gateway implemented and hardware-validated)
**Date:** October 2026
**Status:** Implementation-ready — GW implemented and hardware-validated (2026-10-11); FD still on its earlier copy (CS-O5)

---

## 1. Sources and traceability

ConfigStore persists the system configuration across reboots as a single
opaque blob over QSPI flash.  It owns the A/B slot rotation and CRC32
integrity protection mandated by D39.  It reports failures upward via
`IHealthReport`.

The key-value abstraction ("key-value pairs" in `components.md`) is
**ConfigService's concern**, not ConfigStore's.  ConfigStore deals in one
opaque byte array per write — the serialised config struct.  This keeps
the persistence library simple, testable, and independent of the config
schema.

ConfigStore is called from task context only.  It is passive — no thread
of its own.

| Source | What it contributes |
|--------|---------------------|
| `components.md` §Middleware — ConfigStore | Role, provided/consumed interfaces, responsibilities |
| `hld.md` §5.6 | Configuration and persistence view — ConfigStore wraps QspiFlashDriver |
| `flash-partition-layout.md` §6.1 (D39) | A/B slot geometry, CRC position, power-loss safety guarantee |
| `sequence-diagrams.md` SD-00a/b | Boot: ConfigService loads configuration from ConfigStore |
| `sequence-diagrams.md` SD-07 | Remote config update: ConfigService persists via ConfigStore |
| `sequence-diagrams.md` SD-10 | Device provisioning: ConfigService and DeviceProfileRegistry persist via ConfigStore |
| `SRS.md` REQ-DM-090 | Configuration data persistence requirement |
| `SRS.md` REQ-NF-214 | Configuration integrity verification at boot |
| `state-machines.md` Init sub-states | CheckingIntegrity, LoadingConfig transitions |
| `task-breakdown.md` §7 | `config_store_mutex` resource |

---

## 2. Data types

```c
/* config_store.h */

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief  ConfigStore error codes.
 *
 * Single enum for all ConfigStore operations.  Codes 0–3 follow the
 * project-wide ADT convention; 4–8 are domain-specific flash failures.
 */
typedef enum {
    CONFIG_STORE_ERR_OK               = 0,   /**< Success.                      */
    CONFIG_STORE_ERR_NULL_PTR         = 1,   /**< Null pointer argument.        */
    CONFIG_STORE_ERR_NO_RESOURCE      = 2,   /**< Static pool exhausted.        */
    CONFIG_STORE_ERR_NOT_INIT         = 3,   /**< Handle not created / invalid. */
    CONFIG_STORE_ERR_TOO_LARGE        = 4,   /**< Blob exceeds slot capacity.   */
    CONFIG_STORE_ERR_NO_VALID_SLOT    = 5,   /**< Both slots CRC-invalid.       */
    CONFIG_STORE_ERR_FLASH_ERASE      = 6,   /**< Erase failure from driver.    */
    CONFIG_STORE_ERR_FLASH_WRITE      = 7,   /**< Program failure from driver.  */
    CONFIG_STORE_ERR_FLASH_READ       = 8,   /**< Read failure from driver.     */
} config_store_err_t;

/** Maximum config blob size: the 32 744-byte data area less a 32-byte margin. */
#define CONFIG_STORE_MAX_DATA_BYTES    32712U

/** Magic word identifying a written slot. */
#define CONFIG_STORE_MAGIC             0xC0FFEE00UL

/** Number of A/B slots. */
#define CONFIG_STORE_SLOT_COUNT        2U
```

### Fault codes

ConfigStore defines fault codes for use with `HEALTH_EVENT_FAULT`.
Each code is passed as the `param` argument to
`health_monitor_push_event()`.

```c
/* config_store.h */

/** Both flash slots have invalid CRC at boot. */
#define FAULT_CONFIG_NO_VALID_SLOT     0x0100U

/** QSPI flash read failure during load or integrity check. */
#define FAULT_CONFIG_READ_FAIL         0x0101U
```

Write failures use the existing `HEALTH_EVENT_CONFIG_WRITE_FAIL` event
(index 2 in `health_event_t`) — no fault code needed.

---

## 3. Handle and configuration

```c
/* config_store.h */

/** @brief  Opaque handle — all public functions take this as first arg. */
typedef struct config_store_inst *config_store_handle_t;

/**
 * @brief  Configuration struct for dependency injection.
 *
 * All external dependencies are injected at creation time.  The flash
 * handle provides the IQspiFlash interface; the health handle provides
 * IHealthReport for failure event push.
 */
typedef struct {
    qspi_flash_handle_t      flash;    /**< QSPI flash driver handle.  */
    health_monitor_handle_t  health;   /**< HealthMonitor handle.       */
} config_store_config_t;
```

---

## 4. Public API — `IConfigStore`

### Lifecycle

```c
/**
 * @brief  Create a ConfigStore instance.
 *
 * Allocates from the static pool (max 1 instance).  Stores dependency
 * handles and creates the internal mutex.  Does NOT access flash — the
 * caller must invoke check_integrity() and/or load() separately.
 *
 * @param[in]  config      Dependency injection struct (must not be NULL).
 * @param[out] handle_out  Receives the opaque handle on success.
 * @return CONFIG_STORE_ERR_OK on success.
 * @retval CONFIG_STORE_ERR_NULL_PTR    config or handle_out is NULL.
 * @retval CONFIG_STORE_ERR_NO_RESOURCE Pool exhausted (already created).
 * @note   Threading: call once before the scheduler starts.
 */
config_store_err_t config_store_create(const config_store_config_t *config,
                                       config_store_handle_t       *handle_out);
```

### IConfigStore — read

```c
/**
 * @brief  Load the most recent valid config blob from flash.
 *
 * Reads both slots.  Selects the slot with the highest seq_number whose
 * CRC32 is valid.  Copies the blob into the caller's buffer.
 *
 * Called once at boot by LifecycleController (REQ-NF-214).  If neither
 * slot is valid, returns CONFIG_STORE_ERR_NO_VALID_SLOT and pushes
 * HEALTH_EVENT_FAULT with FAULT_CONFIG_NO_VALID_SLOT.
 *
 * @param[in]  self       ConfigStore handle.
 * @param[out] data_out   Caller-supplied buffer; receives the config blob.
 * @param[out] len_out    Set to the blob byte count on success.
 * @param[in]  max_len    Size of data_out buffer in bytes.
 * @return CONFIG_STORE_ERR_OK on success.
 * @retval CONFIG_STORE_ERR_NULL_PTR      self, data_out, or len_out is NULL.
 * @retval CONFIG_STORE_ERR_NOT_INIT      Handle not created.
 * @retval CONFIG_STORE_ERR_NO_VALID_SLOT Both slots invalid.
 * @retval CONFIG_STORE_ERR_TOO_LARGE     Blob exceeds max_len.
 * @retval CONFIG_STORE_ERR_FLASH_READ    Flash driver read failure.
 * @note   Threading: task-context only; acquires internal mutex.
 */
config_store_err_t config_store_load(config_store_handle_t  self,
                                     void                  *data_out,
                                     uint32_t              *len_out,
                                     uint32_t               max_len);

/**
 * @brief  Verify flash integrity without loading data.
 *
 * Called by LifecycleController during Init.CheckingIntegrity (REQ-NF-214).
 * Returns OK if at least one slot has a valid CRC32.  Does not modify
 * module state.
 *
 * @param[in]  self  ConfigStore handle.
 * @return CONFIG_STORE_ERR_OK if at least one slot valid.
 * @retval CONFIG_STORE_ERR_NULL_PTR      self is NULL.
 * @retval CONFIG_STORE_ERR_NOT_INIT      Handle not created.
 * @retval CONFIG_STORE_ERR_NO_VALID_SLOT Both slots invalid.
 * @retval CONFIG_STORE_ERR_FLASH_READ    Flash driver read failure.
 * @note   Threading: task-context only; acquires internal mutex.
 */
config_store_err_t config_store_check_integrity(config_store_handle_t self);
```

### IConfigStore — write

```c
/**
 * @brief  Persist a new config blob via the A/B write protocol.
 *
 * Executes the A/B write protocol (§7).  On any flash failure, pushes
 * HEALTH_EVENT_CONFIG_WRITE_FAIL and returns the appropriate error code.
 * The previous slot remains valid — the caller's in-memory state is
 * unchanged.
 *
 * @param[in]  self  ConfigStore handle.
 * @param[in]  data  Opaque config blob (must not be NULL).
 * @param[in]  len   Byte count; must be <= CONFIG_STORE_MAX_DATA_BYTES.
 * @return CONFIG_STORE_ERR_OK on success.
 * @retval CONFIG_STORE_ERR_NULL_PTR    self or data is NULL.
 * @retval CONFIG_STORE_ERR_NOT_INIT    Handle not created.
 * @retval CONFIG_STORE_ERR_TOO_LARGE   len exceeds CONFIG_STORE_MAX_DATA_BYTES.
 * @retval CONFIG_STORE_ERR_FLASH_ERASE Erase failure.
 * @retval CONFIG_STORE_ERR_FLASH_WRITE Write failure.
 * @note   Threading: task-context only; acquires internal mutex.
 */
config_store_err_t config_store_save(config_store_handle_t  self,
                                     const void            *data,
                                     uint32_t               len);

/**
 * @brief  Erase both slots (factory reset).
 *
 * Called by ConsoleService on factory-reset command.  Erases all 16
 * sectors.  After this call, the next config_store_load() returns
 * CONFIG_STORE_ERR_NO_VALID_SLOT — the caller must supply defaults.
 *
 * @param[in]  self  ConfigStore handle.
 * @return CONFIG_STORE_ERR_OK on success.
 * @retval CONFIG_STORE_ERR_NULL_PTR    self is NULL.
 * @retval CONFIG_STORE_ERR_NOT_INIT    Handle not created.
 * @retval CONFIG_STORE_ERR_FLASH_ERASE Erase failure.
 * @note   Threading: task-context only; acquires internal mutex.  May
 *         block for the duration of 16 sector erases.
 */
config_store_err_t config_store_erase(config_store_handle_t self);
```

### Test support

```c
#ifdef TEST
/**
 * @brief  Reset the static pool for unit testing.
 *
 * Releases all instances and zeroes the pool.  Production code must
 * never call this function.
 */
void config_store_reset_for_test(void);
#endif
```

---

## 5. Internal design

### 5.1 Instance structure

```c
/* config_store.c */

#include "config_store.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define CS_MAX_INSTANCES        1U
#define CS_HEADER_SIZE          16U
#define CS_CRC_OFFSET           0x7FF8U   /* see §6.2: 4 bytes remain after it unused */
#define CS_READ_CHUNK_SIZE      256U   /* incremental CRC read buffer */

struct config_store_inst {
    qspi_flash_handle_t      flash;
    health_monitor_handle_t  health;
    SemaphoreHandle_t        mutex;
    StaticSemaphore_t        mutex_buf;
    uint8_t                  active_slot_idx;   /**< 0 or 1; cached after load.  */
    uint32_t                 active_seq;         /**< Cached seq_number.          */
    bool                     loaded;             /**< true after successful load. */
    bool                     in_use;             /**< Pool allocation flag.       */
};

static struct config_store_inst g_pool[CS_MAX_INSTANCES];
static uint8_t                  g_count;
```

### 5.2 Slot header

```c
/* config_store.c */

typedef struct {
    uint32_t magic;
    uint32_t seq_number;
    uint32_t data_len;
    uint32_t reserved;
} cs_slot_header_t;

_Static_assert(sizeof(cs_slot_header_t) == CS_HEADER_SIZE,
               "Slot header must be exactly 16 bytes");
```

### 5.3 Synchronisation

All public functions that access flash acquire `self->mutex` for the
duration of the flash operation (priority-inheritance mutex, created at
`_create()` time).  Flash erase + write for one slot takes on the order
of tens of milliseconds at QSPI speed.  The calling task (ConfigService,
running at a low priority) blocks for this duration — acceptable since
config writes are rare events.

Functions guarded by the mutex: `config_store_load()`,
`config_store_save()`, `config_store_check_integrity()`,
`config_store_erase()`.

`config_store_create()` is called before the scheduler starts — no mutex
needed.

### 5.4 Pool allocation

`config_store_create()` scans `g_pool[]` for the first entry with
`in_use == false`.  On success it sets `in_use = true`, increments
`g_count`, creates the mutex via `xSemaphoreCreateMutexStatic()`, stores
the dependency handles from the config struct, and writes the handle to
`*handle_out`.  On pool exhaustion it returns
`CONFIG_STORE_ERR_NO_RESOURCE`.

`config_store_reset_for_test()` zeroes `g_pool[]` and `g_count` —
available only under `TEST` (the project-wide test define).

---

## 6. Flash partition and slot layout

### 6.1 Partition geometry

Both boards use the same base address and geometry (D39).

| Item | Value |
|------|-------|
| QSPI base address | `0x9000_0000` |
| Total partition size | 64 KB |
| Slot count | 2 |
| Slot size | 32 KB each |
| Sector size | 4 KB (8 sectors per slot) |
| Slot A base | `0x9000_0000` |
| Slot B base | `0x9000_8000` |
| Max config blob | 32 712 bytes (32 KB − 20-byte header) |

Compile-time constants in `config_store_platform.h`:

```c
/* config_store_platform.h — same values on both boards (see CS-D3) */

#define CONFIG_STORE_QSPI_BASE_ADDR     0x90000000UL
#define CONFIG_STORE_PARTITION_SIZE      (64U * 1024U)
#define CONFIG_STORE_SLOT_SIZE           (32U * 1024U)
#define CONFIG_STORE_SECTOR_SIZE         (4U  * 1024U)
#define CONFIG_STORE_SECTORS_PER_SLOT    (CONFIG_STORE_SLOT_SIZE / CONFIG_STORE_SECTOR_SIZE)
#define CONFIG_STORE_SLOT_A_BASE        CONFIG_STORE_QSPI_BASE_ADDR
#define CONFIG_STORE_SLOT_B_BASE        (CONFIG_STORE_QSPI_BASE_ADDR + CONFIG_STORE_SLOT_SIZE)
```

### 6.2 Slot layout

Each 32 KB slot has the following layout.  All multi-byte fields are
little-endian (host-native on Cortex-M4).

```
Offset      Size   Field
0x0000        4    magic          — 0xC0FFEE00; identifies a written slot
0x0004        4    seq_number     — monotonically increasing; governs slot selection
0x0008        4    data_len       — byte count of the config blob that follows
0x000C        4    reserved       — alignment padding; write as 0x00000000
0x0010   <=32712   data           — opaque config blob (schema owned by ConfigService)
0x7FF8        4    crc32          — CRC32/ISO-HDLC over bytes [0x0000..0x7FF7]
```

The CRC32 field occupies the final 4 bytes of the slot regardless of
`data_len`.  Bytes between `0x0010 + data_len` and `0x7FF7` are erased
flash (0xFF) and are included in the CRC32 calculation — the CRC always
covers the full slot minus the CRC field itself.  This avoids
length-dependent CRC scope and simplifies the verification path.

**Why CRC is written last** (from `flash-partition-layout.md` §6.1):
a slot interrupted mid-write carries an incremented `seq_number` but a
missing or wrong CRC32.  The load logic therefore never selects it over
a valid lower-numbered slot.  This is the power-loss safety guarantee.

---

## 7. A/B write protocol

Power-loss-safe write sequence (D39).  Steps must execute in order;
partial completion is safe because the CRC commit point is the final
write.

```
config_store_save(self, data, len):

1.  Acquire self->mutex.

2.  Determine active slot:
    — Validate both slots (read header, check magic + CRC32).
    — active_slot = slot with higher valid seq_number
                    (or slot A if neither valid).
    — target_slot = the other slot.
    — next_seq    = active_slot.seq_number + 1
                    (or 1 if no valid slot exists).

3.  Erase target_slot (8 x 4 KB sector erase via self->flash).
    -> On failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL via self->health;
       release mutex; return ERR_FLASH_ERASE.

4.  Build slot header in RAM:
    header.magic      = CONFIG_STORE_MAGIC
    header.seq_number = next_seq
    header.data_len   = len
    header.reserved   = 0

5.  Write header (16 bytes) to target_slot base via self->flash.
    -> On failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL;
       release mutex; return ERR_FLASH_WRITE.
    (active_slot still valid — safe to abort.)

6.  Write data blob (len bytes) at target_slot + 0x0010 via self->flash.
    -> On failure: same as step 5.

7.  Compute CRC32 from known data (no flash read-back needed):
    — CRC over header (16 bytes, in RAM).
    — CRC over data blob (len bytes, in caller's buffer).
    — CRC over synthetic 0xFF padding (CS_CRC_OFFSET - CS_HEADER_SIZE - len
      bytes), fed in CS_READ_CHUNK_SIZE chunks from a stack-allocated
      buffer filled with 0xFF.

8.  Write crc32 (4 bytes) at target_slot + CS_CRC_OFFSET via self->flash.
                                                         <- COMMIT POINT
    -> On failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL;
       release mutex; return ERR_FLASH_WRITE.
    (Slot partially committed; CRC invalid -> never selected over
    active_slot on next boot.)

9.  Update cached state:
    self->active_slot_idx = target_slot index.
    self->active_seq      = next_seq.

10. Release self->mutex.  Return CONFIG_STORE_ERR_OK.
```

After a successful save, `target_slot` is the new active slot (higher
`seq_number`, valid CRC).  On the next boot, `config_store_load()` will
select it automatically.

---

## 8. Load protocol

```
config_store_load(self, data_out, len_out, max_len):

1.  Acquire self->mutex.

2.  For each slot in {A, B}:
    a. Read header (16 bytes) via self->flash.
       -> On read failure: mark slot invalid; continue to next.
    b. If magic != CONFIG_STORE_MAGIC: mark slot invalid; continue.
    c. Verify CRC32 incrementally (see §9 implementation note):
       — Read slot [0x0000..0x7FF7] in CS_READ_CHUNK_SIZE (256 byte)
         chunks via self->flash, feeding each into running CRC.
       — Read stored crc32 (4 bytes) at slot + CS_CRC_OFFSET.
       — If computed CRC != stored CRC: mark slot invalid;
         log at WARN (single-slot corruption is expected after an
         interrupted write — not a fault event).

3.  If both invalid:
    push HEALTH_EVENT_FAULT (param = FAULT_CONFIG_NO_VALID_SLOT)
    via self->health;
    release mutex; return CONFIG_STORE_ERR_NO_VALID_SLOT.

4.  Select the slot with the higher valid seq_number.

5.  If selected.data_len > max_len:
    release mutex; return CONFIG_STORE_ERR_TOO_LARGE.

6.  Read selected.data (data_len bytes at slot + 0x0010) into data_out
    via self->flash.
    -> On read failure: push HEALTH_EVENT_FAULT
       (param = FAULT_CONFIG_READ_FAIL); release mutex;
       return ERR_FLASH_READ.
    Set *len_out = selected.data_len.

7.  Update cached state:
    self->active_slot_idx = selected slot index.
    self->active_seq      = selected.seq_number.
    self->loaded          = true.

8.  Release self->mutex.  Return CONFIG_STORE_ERR_OK.
```

---

## 9. CRC32 algorithm

CRC32/ISO-HDLC: polynomial 0xEDB88320 (reflected), initial value
0xFFFFFFFF, output XOR 0xFFFFFFFF.  This is the standard zlib/Ethernet
CRC32, widely known and testable on any PC.

```c
/* config_store_crc.h */

#include <stdint.h>

/**
 * @brief  Compute CRC32/ISO-HDLC over a byte buffer (convenience).
 * @param[in] buf  Data buffer.
 * @param[in] len  Byte count.
 * @return 32-bit CRC.
 */
uint32_t config_store_crc32(const uint8_t *buf, uint32_t len);
```

Table-driven implementation (1 KB lookup table in `.rodata`).  This CRC
is private to ConfigStore — it lives in `config_store_crc.c`.  Not
shared with the Modbus CRC (different polynomial).

### Implementation note — incremental CRC

The slot verification in `_load()` and the padding CRC in `_save()` both
require incremental computation.  Internally, three static helper
functions are used:

```c
/* config_store_crc.c — internal helpers */

static uint32_t crc32_begin(void);                                   /* returns 0xFFFFFFFF */
static uint32_t crc32_feed(uint32_t running, const uint8_t *buf,
                           uint32_t len);                            /* table-driven update */
static uint32_t crc32_end(uint32_t running);                         /* XOR 0xFFFFFFFF     */
```

The convenience function `config_store_crc32()` calls all three.  The
chunked CRC in `_load()` calls `begin`, loops `feed` per 256-byte read,
then `end`.  Stack usage per call: 256 bytes (chunk buffer) + 16 bytes
(header struct) — no 32 KB allocation needed (CS-D7).

---

## 10. Error and fault behaviour

All public functions return `config_store_err_t`; callers must not ignore
non-OK returns.  No retry is performed by ConfigStore — ConfigService
decides whether to fall back to defaults.

| Error value | Cause | Health event | Caller action |
|---|---|---|---|
| `ERR_NULL_PTR` | Null pointer argument | — | Programming error; fix caller |
| `ERR_NO_RESOURCE` | Pool exhausted (double create) | — | Programming error; fix init sequence |
| `ERR_NOT_INIT` | Function called on invalid handle | — | Programming error; fix init sequence |
| `ERR_TOO_LARGE` | Blob exceeds `MAX_DATA_BYTES` (save) or `max_len` (load) | — | Programming / schema error |
| `ERR_NO_VALID_SLOT` | Both flash slots CRC-invalid | `HEALTH_EVENT_FAULT` `FAULT_CONFIG_NO_VALID_SLOT` | ConfigService applies defaults, saves them |
| `ERR_FLASH_ERASE` | QspiFlashDriver erase failure | `HEALTH_EVENT_CONFIG_WRITE_FAIL` | ConfigService logs; operates in-memory; retries on next config change |
| `ERR_FLASH_WRITE` | QspiFlashDriver program failure | `HEALTH_EVENT_CONFIG_WRITE_FAIL` | ConfigService logs; operates in-memory; retries on next config change |
| `ERR_FLASH_READ` | QspiFlashDriver read failure | `HEALTH_EVENT_FAULT` `FAULT_CONFIG_READ_FAIL` | Treated as ERR_NO_VALID_SLOT by ConfigService |

**Single-slot CRC mismatch** during load is logged at WARN via ILogger
but does NOT push a health event — it is expected after an interrupted
write and the valid slot is used.  A health event fires only when
**both** slots are invalid.

---

## 11. Sequence integration

ConfigStore is called synchronously; no task-level sequencing diagram is
required beyond the HLD.

| SD | Component role | Key function |
|---|---|---|
| SD-00a/b | ConfigService calls `config_store_load()` to load configuration from QSPI at boot (FD and GW) | `config_store_load()` |
| SD-07 | ConfigService calls `config_store_save()` to persist updated configuration received from the cloud | `config_store_save()` |
| SD-10 | ConfigService and DeviceProfileRegistry call `config_store_save()` to persist provisioned parameters and device profiles | `config_store_save()` |

---

## 12. Boot sequence and ordering

```
qspi_flash_create(...)                     -> flash_handle
health_monitor_create(...)                 -> health_handle
config_store_create(&cfg, &cs_handle)      -> cs_handle
    |
config_store_check_integrity(cs_handle)    <- LifecycleController.CheckingIntegrity
config_store_load(cs_handle, ...)          <- LifecycleController.LoadingConfig
```

`config_store_check_integrity()` and `config_store_load()` are separate
calls because CheckingIntegrity and LoadingConfig are separate Init
sub-states — a corruption detected in CheckingIntegrity transitions to
Faulted without ever attempting LoadingConfig (CS-D9).

`config_store_create()` does not access flash.  No two-phase init.  No
ISR dependency.

---

## 13. Board differences

ConfigStore is **identical** on both boards.  The same source files
compile for both targets without conditional compilation (`#if BOARD_xxx`
is neither needed nor used — CS-D3).

Board-specific constants are identical today (both STM32F469 and
STM32L475 alias QSPI flash to `0x9000_0000`); they live in
`config_store_platform.h` (§6.1) so the value can be overridden if a
future board differs.

The config blob contents differ between FD and GW, but ConfigStore
treats the blob as opaque bytes — the schema is owned by ConfigService.

---

## 14. Principles applied

| Principle | How applied |
|---|---|
| **P1 (Strict directional layering)** | Depends on IQspiFlash (Driver layer, downward).  Logger and HealthMonitor are cross-cutting exceptions (P4). |
| **P2 (Dependency Inversion)** | Exposes `config_store_handle_t` opaque handle — the DIP seam.  ConfigService depends on the ConfigStore interface, not the implementation.  QSPI flash handle injected via config struct (CS-D4). |
| **P4 (Cross-cutting exception)** | Logger accessed concretely per cross-cutting convention.  HealthMonitor handle injected via config struct — IHealthReport is Application-layer owned but consumed by this Middleware component (DIP). |
| **P5 (Bounded resources)** | Static pool of 1.  256-byte CRC chunk buffer stack-allocated per operation.  No heap allocation post-init. |
| **P6 (Responsibility traces)** | Load / save / erase traces to REQ-DM-090 and REQ-NF-214. |
| **P8 (Total error propagation)** | All operations return `config_store_err_t`.  QSPI errors propagated as distinct codes.  Health events pushed on all flash failures.  Single-slot CRC mismatch logged at WARN. |
| **P9 (BARR-C coding standard)** | Addresses and lengths `uint32_t`; slot indices `uint8_t`; fixed-width integers throughout; no floating-point. |
| **P10 (Naming conventions)** | Prefix `config_store_`; handle `config_store_handle_t`; errors `CONFIG_STORE_ERR_*`; fault codes `FAULT_CONFIG_*`. |

---

## 15. Memory sizing

### RAM

| Item | Size | Notes |
|---|---|---|
| `struct config_store_inst` | ~104 B | Handles, mutex, cached state, flags |
| `StaticSemaphore_t` mutex buffer | 80 B | Included in inst struct |
| `g_pool[1]` | ~104 B | Single instance, total static RAM |
| Stack per call (load/save) | ~288 B | 256-byte chunk buffer + 16-byte header + locals |

### ROM

| Item | Size | Notes |
|---|---|---|
| CRC32 lookup table | 1 024 B | 256 x 4 bytes in `.rodata` |
| Code (`.text`) | ~2.0 KB est. | All functions combined |
| Constants | ~64 B | Magic, slot addresses |
| **Total ROM** | **~3.1 KB** | |

**Calling task stack note:** ConfigStore operations require only ~288
bytes of additional stack (chunk buffer + header).  No special stack
sizing is needed in the calling task beyond the normal FreeRTOS minimum.
This is a direct result of the incremental CRC approach (CS-D7).

---

## 16. Unit-test plan

### Mock strategy

| Dependency | Mock method |
|---|---|
| `qspi_flash_read` | CMock-generated from `qspi_flash.h`; callback stub backed by `uint8_t s_flash_sim[64 * 1024]` RAM array |
| `qspi_flash_write` | CMock-generated; callback writes into `s_flash_sim` |
| `qspi_flash_erase_sector` | CMock-generated; callback fills sector with 0xFF in `s_flash_sim` |
| `health_monitor_push_event` | CMock-generated from `health_monitor.h`; `Expect` variant captures event + param |
| `xSemaphoreCreateMutexStatic` | CMock stub returning a non-NULL handle |
| `xSemaphoreTake` / `xSemaphoreGive` | CMock stubs returning `pdTRUE` |

### Helper functions

```c
/* test_config_store.c */

static uint8_t s_flash_sim[64U * 1024U];

/** Write a valid slot (header + data + CRC) into s_flash_sim. */
static void helper_write_valid_slot(uint8_t slot_idx, uint32_t seq,
                                    const uint8_t *data, uint32_t len);

/** Corrupt the CRC field of a slot in s_flash_sim. */
static void helper_corrupt_slot_crc(uint8_t slot_idx);

/** Return the base address of a slot in s_flash_sim. */
static uint8_t *helper_slot_ptr(uint8_t slot_idx);
```

### Test cases

#### Group 1 — Create / lifecycle

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T01 | Pool empty | `config_store_create(&valid_cfg, &h)` | `ERR_OK`; `h` non-NULL |
| CS-T02 | Pool empty | `config_store_create(NULL, &h)` | `ERR_NULL_PTR` |
| CS-T03 | Pool empty | `config_store_create(&cfg, NULL)` | `ERR_NULL_PTR` |
| CS-T04 | One instance in use | `config_store_create(&cfg, &h2)` | `ERR_NO_RESOURCE` |
| CS-T05 | No instance created | `config_store_load(NULL, ...)` | `ERR_NULL_PTR` |

#### Group 2 — Load: happy path

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T06 | Slot A valid (seq=1), slot B erased | `config_store_load(h, ...)` | `ERR_OK`; data matches slot A blob |
| CS-T07 | Slot B valid (seq=2), slot A valid (seq=1) | `config_store_load(h, ...)` | `ERR_OK`; data matches slot B (higher seq) |
| CS-T08 | Both valid, A.seq=3, B.seq=2 | `config_store_load(h, ...)` | `ERR_OK`; data matches slot A (higher seq) |

#### Group 3 — Load: error paths

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T09 | Both slots erased (all 0xFF) | `config_store_load(h, ...)` | `ERR_NO_VALID_SLOT`; `HEALTH_EVENT_FAULT` pushed with `FAULT_CONFIG_NO_VALID_SLOT` |
| CS-T10 | Slot A CRC corrupted, slot B valid | `config_store_load(h, ...)` | `ERR_OK`; data matches slot B |
| CS-T11 | Both slots CRC corrupted | `config_store_load(h, ...)` | `ERR_NO_VALID_SLOT`; health event pushed |
| CS-T12 | Valid slot, data_len > max_len arg | `config_store_load(h, buf, &len, 16)` | `ERR_TOO_LARGE` |
| CS-T13 | Flash read stub returns error | `config_store_load(h, ...)` | `ERR_FLASH_READ`; `HEALTH_EVENT_FAULT` pushed with `FAULT_CONFIG_READ_FAIL` |
| CS-T14 | Valid handle | `config_store_load(h, NULL, &len, max)` | `ERR_NULL_PTR` |
| CS-T15 | Valid handle | `config_store_load(h, buf, NULL, max)` | `ERR_NULL_PTR` |

#### Group 4 — Save: happy path

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T16 | Both slots erased | `config_store_save(h, data, len)` | `ERR_OK`; slot A written with seq=1; CRC valid in `s_flash_sim` |
| CS-T17 | Slot A valid (seq=1) | `config_store_save(h, data2, len2)` | `ERR_OK`; slot B written with seq=2; slot A unchanged |
| CS-T18 | After CS-T17 | `config_store_load(h, ...)` | `ERR_OK`; returns data2 (slot B, higher seq) |

#### Group 5 — Save: error paths

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T19 | Valid handle | `config_store_save(h, NULL, len)` | `ERR_NULL_PTR` |
| CS-T20 | Valid handle | `config_store_save(h, data, MAX+1)` | `ERR_TOO_LARGE` |
| CS-T21 | Flash erase stub returns error at step 3 | `config_store_save(h, ...)` | `ERR_FLASH_ERASE`; `HEALTH_EVENT_CONFIG_WRITE_FAIL` pushed; active slot unchanged |
| CS-T22 | Flash write stub returns error at step 5 (header) | `config_store_save(h, ...)` | `ERR_FLASH_WRITE`; health event pushed; active slot unchanged |
| CS-T23 | Flash write stub returns error at step 8 (CRC commit) | `config_store_save(h, ...)` | `ERR_FLASH_WRITE`; health event pushed; next load selects old slot |

#### Group 6 — Check integrity

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T24 | Slot A valid, slot B erased | `config_store_check_integrity(h)` | `ERR_OK` |
| CS-T25 | Both slots valid | `config_store_check_integrity(h)` | `ERR_OK` |
| CS-T26 | Both slots invalid | `config_store_check_integrity(h)` | `ERR_NO_VALID_SLOT`; `HEALTH_EVENT_FAULT` pushed |

#### Group 7 — Erase (factory reset)

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T27 | Both slots valid | `config_store_erase(h)` | `ERR_OK`; all 16 sectors 0xFF in `s_flash_sim` |
| CS-T28 | After CS-T27 | `config_store_load(h, ...)` | `ERR_NO_VALID_SLOT` |
| CS-T29 | Flash erase stub returns error | `config_store_erase(h)` | `ERR_FLASH_ERASE` |

#### Group 8 — CRC32 algorithm

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T30 | Known test vector: `"123456789"` | `config_store_crc32(buf, 9)` | `0xCBF43926` (ISO-HDLC reference) |
| CS-T31 | All-0xFF buffer (256 bytes) | `config_store_crc32(buf, 256)` | Deterministic; regress against PC-computed reference |
| CS-T32 | NULL buffer, len=0 | `config_store_crc32(NULL, 0)` | Graceful return (no crash); value deterministic |

#### Group 9 — Round-trip integration

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T33 | Fresh flash | save(data1) -> load -> save(data2) -> load | Final load returns data2; slot rotation A->B correct |
| CS-T34 | Slot A valid, then save, then corrupt new slot, then load | Save writes slot B; corrupt B CRC; load | Load returns slot A data (fallback to valid slot) |

#### Group 10 — Reset for test

| ID | Precondition | Action | Expected |
|---|---|---|---|
| CS-T35 | One instance in use | `config_store_reset_for_test()` | Pool cleared; next create succeeds |

**Coverage target:** every public API function exercised with at least
one happy-path and one error-path test case.  All nine error codes
triggered.

---

## 17. File layout

The boards are separate CubeIDE projects and the repository keeps one copy of
each middleware module per board, so this layout supersedes the earlier
`firmware/shared/` sketch (CS-O4).

```
firmware/gateway/middleware/config_store/
+-- config_store.h           <- public API, handle, config, errors
+-- config_store.c           <- A/B protocol, slot evaluation, mutex
+-- config_store_config.h    <- partition and slot geometry constants
+-- config_store_crc.h       <- incremental and one-shot CRC32
+-- config_store_crc.c       <- table-driven CRC32/ISO-HDLC

firmware/field-device/middleware/config_store/   <- FD copy (CS-O5)

tests/gateway/middleware/config_store/
+-- test_config_store.c      <- 48 tests (CS-T01..T29, T33..T35, T40..T55)
+-- test_config_store_crc.c  <- 6 tests (CS-T30..T32, T36..T38)

firmware/gateway/integration-tests/config_store/
+-- main_test_config_store.c <- hardware bring-up
```

---

## 18. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| CS-D1 | ADT pattern (opaque handle, static pool of 1, config struct) | Matches project-wide convention (CFL, SAF, HM).  DIP seam is the opaque handle, not a vtable. |
| CS-D2 | Single error enum (9 values, codes 0–3 project-standard) | No separate init/load/save error types.  Domain-specific codes 4–8 cover all flash failure modes. |
| CS-D3 | Shared source, no `#if BOARD_xxx` | ConfigStore is functionally identical on both boards.  Flash addresses and geometry match.  Config blob schema is opaque — owned by ConfigService. |
| CS-D4 | QspiFlash handle injected via config struct | DIP (P2).  ConfigStore accesses flash exclusively through the injected handle.  Enables CMock-based testing without `#ifdef UNIT_TEST` preprocessor swaps. |
| CS-D5 | No vtable — ADT handle is the DIP seam | Consistent with HM-D8.  ConfigService depends on `config_store_handle_t`, not an `iconfig_store_t` vtable.  One concrete implementation; polymorphism not needed. |
| CS-D6 | Fault codes for non-write failures | Write failures use existing `HEALTH_EVENT_CONFIG_WRITE_FAIL`.  Read / integrity failures use `HEALTH_EVENT_FAULT` with `FAULT_CONFIG_*` param codes — avoids expanding the HealthMonitor event enum. |
| CS-D7 | Incremental CRC with 256-byte chunk buffer | Slot CRC verification reads flash in 256-byte chunks, feeding each into a running CRC.  Stack usage: ~288 bytes per call.  Avoids the 32 KB stack allocation that a full-slot read would require.  Save computes CRC from RAM data + synthetic 0xFF padding (no flash read-back). |
| CS-D8 | seq_number overflow: no handling needed | `uint32_t` gives >4 billion writes.  At one write per day, exceeds 11 000 years.  Documented; no implementation change. |
| CS-D9 | Separate `check_integrity()` and `load()` | Matches Init sub-state machine: CheckingIntegrity -> LoadingConfig.  Corruption detected in CheckingIntegrity transitions to Faulted without attempting load. |
| CS-D10 | No `_destroy()` — only `_reset_for_test()` | ConfigStore is created at startup and lives for firmware duration.  No runtime destruction use case.  Test reset clears the pool. |
| CS-D11 | Single-slot CRC mismatch: log only, no health event | Expected behaviour after an interrupted write.  Health event fires only when both slots are invalid (actual fault). |

---

## 19. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| CS-O1 | `CONFIG_STORE_MAX_DATA_BYTES` (32 712) must be verified against the actual serialised config struct size. | Verify at ConfigService LLD | Open |
| CS-O2 | QSPI flash driver interface (`IQspiFlash`) — erase and write granularity must match the slot layout.  Erase unit must be <= 4 KB; write must support page-granular (256 B) writes at minimum. | Confirm at QspiFlashDriver LLD | Resolved — QspiFlashDriver (GW) erases 4 KB sectors and programs up to 256 B within one page; ConfigStore splits its writes at page boundaries. Hardware-validated. |
| CS-O3 | `seq_number` overflow (`uint32_t` wraps after 4.29 billion writes). | Documented in CS-D8; no implementation change needed. | Resolved |
| CS-O4 | `firmware/shared/` directory convention.  First use; confirm with build system setup. | Confirm when CMake / Makefile is configured | Resolved — not used; each board keeps its own copy (§17). |
| CS-O5 | **The Field Device ConfigStore differs from this companion.** It uses a different magic (`0xBADC0DE0`), different error-code values (no `NULL_PTR`/`NO_RESOURCE`/`NOT_INIT` block), the `iconfig_store_t` vtable instead of an ADT handle, and no health-event or fault-code behaviour as written here. Its on-flash geometry matches (offset 0, CRC at 0x7FF8, 32 712 B maximum). | Bring the FD module to this API when ConfigService (FD) is implemented. | Open |
| CS-O6 | **Nothing calls the Gateway ConfigStore yet** (no ConfigService or LifecycleController on the Gateway). Timings measured on the board: factory erase 1 345 ms, save of 300 B 685 ms, save of 32 712 B 1 184 ms, integrity check 50 ms, load of 32 712 B 156 ms. A caller must run these from a low-priority task. | Account for the blocking time when ConfigService is designed. | Open |

---

## 19.1 Gateway implementation notes

Where the code settles something this companion left open, contradicted
itself on, or differs from its pseudocode. The public API in §4 is unchanged.

| # | Topic | Gateway behaviour |
|---|---|---|
| 1 | CRC offset and blob limit | §5.1 wrote `CS_CRC_OFFSET` as `SLOT_SIZE - 4` (0x7FFC) while its comment, §6.2 and the Field Device code all say 0x7FF8, and the "32 KB minus 20 bytes" note does not equal 32 712. The code uses 0x7FF8 (CRC over 0x0000..0x7FF7, 32 744-byte data area) and keeps `CONFIG_STORE_MAX_DATA_BYTES` at 32 712, a 32-byte margin. §5.1 and §2 are corrected above. |
| 2 | Addresses are device offsets | The QspiFlashDriver takes offsets from 0, not mapped addresses, so the partition at 0x9000_0000 is at offset 0 (slot B at 0x8000), as for CircularFlashLog. |
| 3 | Flash dependency | Injected as `const iqspi_flash_t *` (the driver's vtable), not a `qspi_flash_handle_t`; the driver has no handle. |
| 4 | Read failures (load / integrity) | §8 step 2a marks a slot whose read fails as invalid, which would turn CS-T13 (all reads fail) into `NO_VALID_SLOT`. The code distinguishes the two: if neither slot is valid and a flash read failed, it returns `FLASH_READ` and raises `FAULT_CONFIG_READ_FAIL`; if both slots were readable but invalid it returns `NO_VALID_SLOT` and raises `FAULT_CONFIG_NO_VALID_SLOT`. One unreadable slot with a valid other slot is not a fault. |
| 5 | Empty blob | A zero-length blob is accepted. The driver rejects a zero-length read, so `config_store_load()` skips the data read when `data_len` is 0 (found by the unit-test flash, which enforces the driver's rules). |
| 6 | Implausible header | A slot whose header has the magic but a `data_len` above `CONFIG_STORE_MAX_DATA_BYTES` is treated as invalid without reading its CRC. |
| 7 | Page-aligned programming | The blob starts 16 bytes into the slot, so every program is split at 256-byte page boundaries; the driver rejects a write that crosses one. |
| 8 | On-flash integers | Header and CRC are written byte by byte, little-endian, not by casting a struct, so the format does not depend on compiler padding. |
| 9 | CRC helpers | `config_store_crc32_begin/_feed/_end` are public in `config_store_crc.h`. §9 made them `static`, but `config_store.c` calls them for the chunked verification and the padding. |
| 10 | HealthMonitor | The real Gateway HealthMonitor now exists, so `config_store.c` calls `health_monitor_push_event()` directly; no placeholder. A NULL `health` disables events. `config_store.h` includes `health_monitor.h` for the handle type. |
| 11 | Equal sequence numbers | Resolve to slot A. A correct write never produces them. |
| 12 | Test define and layout | The reset hook is under `TEST`, not `UNIT_TEST`; the tests run under `tests/project_gateway.yml` because the Gateway and Field Device modules share basenames. |

### 19.2 Gateway test results

`tests/gateway/middleware/config_store/`: 54 tests, 54 pass (2026-10-11).
`test_config_store.c` has 48 (CS-T01..T29, T33..T35 from §16, and T40..T55 added);
`test_config_store_crc.c` has 6 (CS-T30..T32, T36..T38). The tests drive a RAM flash
behind the `iqspi_flash_t` vtable that obeys the real driver's rules (256-byte
pages, no page crossing, AND-only programming, 4 KB erase) and counts any
violation, instead of CMock. Slot images are built independently of
`config_store_save()`. Five deliberate mutations (CRC ignoring the padding,
choosing the lower sequence, no page splitting, read errors reported as
no-valid-slot, skipping the erase) each failed the intended tests.

### 19.3 Hardware bring-up

Harness: `firmware/gateway/integration-tests/config_store/main_test_config_store.c`.
Real QspiFlashDriver and real HealthMonitor, no scheduler. Two phases around a
reset; it erases the ConfigStore partition (device offsets 0x0000..0xFFFF) only.

| ID | What to observe | Verifies |
|---|---|---|
| TC-HW-CS-001 | QSPI, HealthMonitor and ConfigStore come up | Boot order (§12) |
| TC-HW-CS-002 | Factory erase leaves both slots erased | `config_store_erase` |
| TC-HW-CS-003 | Blank partition: integrity and load report `NO_VALID_SLOT` | §8 |
| TC-HW-CS-004 | First save in slot A, sequence 1; integrity and load succeed | §7 |
| TC-HW-CS-005 | Second save in slot B, sequence 2; slot A intact | Slot rotation |
| TC-HW-CS-006 | A 32 712-byte blob round-trips | Page splitting, eight sectors |
| TC-HW-CS-007 | Clearing bits in the newest slot's CRC makes load return the previous blob | Interrupted commit |
| TC-HW-CS-008 | A newer header with no CRC is never chosen; the next save recovers | Torn write |
| TC-HW-CS-009 | Error paths; HealthMonitor saw no write failure | §10 |
| TC-HW-CS-010 | The persistence blob is saved | Phase B input |
| TC-HW-CS-011 | After RESET the blob is there and the integrity check passes | REQ-NF-214, REQ-DM-090 |
| TC-HW-CS-012 | Factory erase clears the partition; the next run starts at Phase A | Clean-up |

**Status: run on hardware 2026-10-11 — all 12 cases PASS** (B-L475E-IOT01A).
Timings: factory erase 1 345 ms; save of 300 B 685 ms; save of 32 712 B 1 184 ms;
integrity check 50 ms; load of 300 B 50 ms and of 32 712 B 156 ms. The partition was
left blank. The FAULT events are only logged by HealthMonitor and so are covered by
the unit tests, not observed on the bench.

---

## Phase H — readiness review

| # | Check | Status |
|---|---|---|
| 1 | ADT pattern: opaque handle, `_create()`, static pool, config struct | PASS |
| 2 | All public functions take `self` handle as first parameter | PASS |
| 3 | Single error enum with project-standard codes 0–3 | PASS |
| 4 | Dependencies injected via config struct (DIP) | PASS |
| 5 | No `#if BOARD_xxx` conditional compilation | PASS |
| 6 | Health events reference only existing HM v0.2 events or documented fault codes | PASS |
| 7 | SD trace function names match actual API (`load`/`save` throughout) | PASS |
| 8 | Every public function has Doxygen with `@brief`, `@param`, `@return`, `@retval`, `@note` | PASS |
| 9 | Synchronisation documented: which functions acquire mutex, hold time bounded | PASS |
| 10 | Error table covers all 9 error codes with cause, health event, and caller action | PASS |
| 11 | Unit-test plan uses CS-Tnn IDs with precondition/action/expected columns | PASS |
| 12 | Mock strategy documented (CMock, no `#ifdef UNIT_TEST` swaps) | PASS — implemented with a rule-enforcing RAM flash instead (§19.2) |
| 13 | File layout section present with source paths | PASS |
| 14 | Memory sizing section present (RAM + ROM estimates) | PASS |
| 15 | Decisions log with CS-Dnn IDs and rationale | PASS |
| 16 | Principles applied (P1–P10) with specific justification | PASS |
| 17 | Boot sequence updated for ADT create pattern | PASS |
| 18 | Gateway implemented, host-tested and hardware-validated | PASS — §19.1–19.3: 54 host tests, 12 hardware cases |

**Verdict:** implemented and hardware-validated for the Gateway; the Field Device copy is still to be brought across (CS-O5).
