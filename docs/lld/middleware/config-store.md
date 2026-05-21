# LLD Companion — ConfigStore

**Layer:** Middleware  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `IConfigStore`  
**Consumes:** `IQspiFlash` (QspiFlashDriver), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-DM-090, REQ-NF-214  
**HLD ref:** `components.md` §Middleware — ConfigStore; `hld.md` §5.6, §13.3, §13.4, §13.6; `flash-partition-layout.md` §6.1 (D39)
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** ConfigStore in `components.md` (FD + GW middleware layer)

---

## 1. Sources

ConfigStore persists the system configuration across reboots as a single
opaque blob over QSPI flash. It owns the A/B slot rotation and CRC32
integrity protection mandated by D39. It reports failures upward via
`IHealthReport`.

The key-value abstraction ("key-value pairs" in `components.md`) is
**ConfigService's concern**, not ConfigStore's. ConfigStore deals in one
opaque byte array per write — the serialised config struct. This keeps the
persistence library simple, testable, and independent of the config schema.

ConfigStore is called from task context only. It is passive — no thread of
its own.

---

## 2. Flash partition

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

---

## 3. Slot layout

Each 32 KB slot has the following layout. All multi-byte fields are
little-endian (host-native on Cortex-M4).

```
Offset      Size   Field
0x0000        4    magic          — 0xC0FFEE00; identifies a written slot
0x0004        4    seq_number     — monotonically increasing; governs slot selection
0x0008        4    data_len       — byte count of the config blob that follows
0x000C        4    reserved       — alignment padding; write as 0x00000000
0x0010   ≤32712    data           — opaque config blob (schema owned by ConfigService)
0x7FF8        4    crc32          — CRC32/ISO-HDLC over bytes 0x0000..0x7FF7; WRITTEN LAST
```

The CRC32 field occupies the final 4 bytes of the slot regardless of
`data_len`. Bytes between `0x0010 + data_len` and `0x7FF7` are erased flash
(0xFF) and are included in the CRC32 calculation — the CRC always covers
the full slot minus the CRC field itself. This avoids length-dependent CRC
scope and simplifies the verification path.

**Why CRC is written last (from `flash-partition-layout.md` §6.1):** a
slot interrupted mid-write carries an incremented `seq_number` but a
missing or wrong CRC32. The load logic therefore never selects it over a
valid lower-numbered slot. This is the power-loss safety guarantee.

---

## 4. Data types

```c
/* config_store.h */

typedef enum {
    CONFIG_STORE_ERR_OK               = 0,
    CONFIG_STORE_ERR_NOT_INIT         = 1,
    CONFIG_STORE_ERR_NULL_ARG         = 2,
    CONFIG_STORE_ERR_TOO_LARGE        = 3,   /* blob exceeds slot capacity   */
    CONFIG_STORE_ERR_NO_VALID_SLOT    = 4,   /* both slots CRC-invalid        */
    CONFIG_STORE_ERR_FLASH_ERASE      = 5,   /* erase failure from driver     */
    CONFIG_STORE_ERR_FLASH_WRITE      = 6,   /* program failure from driver   */
    CONFIG_STORE_ERR_FLASH_READ       = 7,   /* read failure from driver      */
} config_store_err_t;

#define CONFIG_STORE_MAX_DATA_BYTES  32712U
#define CONFIG_STORE_MAGIC           0xC0FFEE00UL
```

---

## 2. Public API — `IConfigStore`

```c
/**
 * @brief  Initialise ConfigStore.
 *
 * Must be called after qspi_flash_driver_init(). Verifies the QSPI partition
 * is accessible. Does NOT load config — load is a separate call.
 *
 * @param  health  IHealthReport handle for failure event push.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
config_store_err_t config_store_init(IHealthReport *health);

/**
 * @brief  Load the most recent valid config blob from flash.
 *
 * Reads both slots. Selects the slot with the highest seq_number whose
 * CRC32 is valid. Copies the blob into the caller's buffer.
 *
 * Called once at boot by LifecycleController (REQ-NF-214). If neither
 * slot is valid, returns CONFIG_STORE_ERR_NO_VALID_SLOT and pushes
 * HEALTH_EVENT_CONFIG_NO_VALID_SLOT — LifecycleController transitions to
 * Faulted.
 *
 * @param[out] data_out   Caller-supplied buffer; receives config blob.
 * @param[out] len_out    Set to the blob byte count on success.
 * @param[in]  max_len    Size of data_out buffer in bytes.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
config_store_err_t config_store_load(void     *data_out,
                                     uint32_t *len_out,
                                     uint32_t  max_len);

/**
 * @brief  Persist a new config blob.
 *
 * Executes the A/B write protocol (§6). On any flash failure, pushes
 * HEALTH_EVENT_CONFIG_WRITE_FAIL and returns the appropriate error code.
 * The previous slot remains valid — the caller's in-memory state is
 * unchanged.
 *
 * Thread-safe (acquires internal mutex).
 *
 * @param[in]  data  Opaque config blob.
 * @param[in]  len   Byte count; must be ≤ CONFIG_STORE_MAX_DATA_BYTES.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 */
config_store_err_t config_store_save(const void *data, uint32_t len);

/**
 * @brief  Verify flash integrity without loading.
 *
 * Called by LifecycleController during Init.CheckingIntegrity (REQ-NF-214).
 * Returns OK if at least one slot has a valid CRC32. Does not modify state.
 * Pushes HEALTH_EVENT_CONFIG_NO_VALID_SLOT if both slots invalid.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
config_store_err_t config_store_check_integrity(void);

/**
 * @brief  Erase both slots (factory reset).
 *
 * Called by ConsoleService on factory-reset command. Erases all 16 sectors.
 * After this call, the next config_store_load() returns
 * CONFIG_STORE_ERR_NO_VALID_SLOT — the caller must supply defaults.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
config_store_err_t config_store_erase(void);
```

---

## 6. A/B write protocol

This is the power-loss-safe write sequence (D39). Steps must execute in
order; partial completion is safe.

```
config_store_save(data, len):

1. Read active slot:
   - Validate both slots (magic + CRC32).
   - active_slot = slot with higher valid seq_number (or slot A if neither valid).
   - target_slot = the other slot.
   - next_seq    = active_slot.seq_number + 1  (or 1 if no valid slot exists).

2. Erase target_slot (8 × 4 KB sector erase via QspiFlashDriver).
   → On erase failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL; return ERR_FLASH_ERASE.

3. Build slot header in RAM:
   header.magic      = CONFIG_STORE_MAGIC
   header.seq_number = next_seq
   header.data_len   = len
   header.reserved   = 0

4. Write header (16 bytes) to target_slot base address.
   → On write failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL; return ERR_FLASH_WRITE.
   (active_slot is still valid — safe to abort here.)

5. Write data blob (len bytes) starting at target_slot base + 0x0010.
   → On write failure: same as step 4.

6. Calculate CRC32 over bytes [0x0000 .. 0x7FF7] of target_slot (full slot
   minus CRC field; unwritten bytes are 0xFF — included in CRC).

7. Write crc32 (4 bytes) at target_slot base + 0x7FF8. ← COMMIT POINT
   → On write failure: push HEALTH_EVENT_CONFIG_WRITE_FAIL; return ERR_FLASH_WRITE.
   (Slot is partially committed but CRC invalid → will never be selected over
   active_slot on next boot.)

8. Return CONFIG_STORE_ERR_OK.
```

After a successful save, `target_slot` is the new active slot (higher
`seq_number`, valid CRC). On the next boot, `config_store_load()` will
select it automatically.

---

## 7. Load protocol

```
config_store_load(data_out, len_out, max_len):

1. For each slot in {A, B}:
   a. Read header (magic, seq_number, data_len).
   b. If magic != CONFIG_STORE_MAGIC: mark slot invalid.
   c. Else: read full slot (header + data + crc32).
      Compute CRC32 over [0x0000..0x7FF7].
      Compare with stored crc32. If mismatch: mark invalid;
      push HEALTH_EVENT_CONFIG_READ_FAIL; log slot index.

2. If both invalid:
   push HEALTH_EVENT_CONFIG_NO_VALID_SLOT;
   return CONFIG_STORE_ERR_NO_VALID_SLOT.

3. Select slot with higher valid seq_number.

4. If selected.data_len > max_len: return CONFIG_STORE_ERR_TOO_LARGE.

5. Copy selected.data to data_out; set *len_out = selected.data_len.
   Return CONFIG_STORE_ERR_OK.
```

---

## 3. Internal design

```c
/* config_store.c — static module state */

typedef struct {
    bool              initialised;
    IHealthReport    *health;
    SemaphoreHandle_t mutex;    /* priority-inheritance mutex */
    uint8_t           active_slot_index;   /* 0 or 1; cached after load */
    uint32_t          active_seq_number;   /* cached after load         */
} ConfigStoreState;

static ConfigStoreState s_cs;
```

`config_store_save()` and `config_store_load()` both acquire `s_cs.mutex`
for the duration of the flash operation. Flash erase + write for one slot
takes on the order of tens of milliseconds at QSPI speed. The calling task
(ConfigService, running in a low-priority maintenance context) blocks for
this duration — acceptable since config writes are rare.

`config_store_check_integrity()` and `config_store_erase()` also acquire
the mutex.

---


### Synchronisation

This component uses an internal mutex to serialise concurrent callers. The mutex is created during `_init()` and held only for the duration of each guarded operation (bounded, short hold time). All public functions are task-safe but not ISR-safe.

### config_store_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### config_store_save

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### config_store_check_integrity

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### config_store_erase

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### Principles applied

- **P1 (Strict directional layering).** Depends on IQspiFlash (driver layer); Logger and HealthMonitor are cross-cutting exceptions (P4).
- **P2 (Dependency Inversion).** Exposes `iconfig_store_t` vtable; ConfigService depends on `IConfigStore`.
- **P4 (Cross-cutting concern exception).** Logger and HealthMonitor (IHealthReport) referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Slot map and dirty-page tracking in a static struct; no heap; flash page buffer stack-allocated per operation (bounded by QSPI page size).
- **P6 (Responsibility traces to requirements).** Read / write / erase-and-write traces to REQ-DM-090 / REQ-NF-214 configuration persistence requirements.
- **P8 (Total error propagation, no silent failures).** All operations return `config_store_err_t`; QSPI errors propagated; CRC mismatch returned as a distinct error code.
- **P9 (BARR-C coding standard).** Addresses and lengths `uint32_t`; slot indices `uint8_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `config_store_`; interface `IConfigStore` -> `iconfig_store_t`; errors `CONFIG_STORE_ERR_*`.


## 9. CRC32 algorithm

Use CRC32/ISO-HDLC: polynomial 0xEDB88320 (reflected), initial value
0xFFFFFFFF, output XOR 0xFFFFFFFF. This is the standard zlib/Ethernet
CRC32, widely known and testable on any PC.

```c
/* config_store_crc.h */
uint32_t config_store_crc32(const uint8_t *buf, uint32_t len);
```

Table-driven implementation (1 KB lookup table in ROM). Unlike the Modbus
CRC, this one is not shared — it lives in `config_store_crc.c` alongside
the rest of ConfigStore.

---

## 10. Boot sequence and ordering

```
qspi_flash_driver_init()           ← driver ready
config_store_init(health)          ← partition accessible
config_store_check_integrity()     ← called by LifecycleController.CheckingIntegrity
config_store_load(buf, &len, max)  ← called by LifecycleController.LoadingConfig
```

`config_store_check_integrity()` and `config_store_load()` are separate
calls because CheckingIntegrity and LoadingConfig are separate Init
sub-states — a corruption detected in CheckingIntegrity transitions to
Faulted without ever attempting LoadingConfig.

No two-phase init. ConfigStore has no ISR dependency.

---

## 11. Board differences

The component is identical on both boards. Board-specific values are in
`config_store_config.h`, included by the build system per target:

```c
/* config_store_config.h  (board-specific, not shared) */

/* Same on both boards — QSPI aliased to 0x9000_0000 on both STM32F469 and STM32L475 */
#define CONFIG_STORE_QSPI_BASE_ADDR  0x90000000UL
#define CONFIG_STORE_PARTITION_SIZE  (64U * 1024U)
#define CONFIG_STORE_SLOT_SIZE       (32U * 1024U)
#define CONFIG_STORE_SECTOR_SIZE     (4U  * 1024U)
```

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-00 | SD-00a: `ConfigService` calls `config_store_read()` to load FD configuration from QSPI at boot. SD-00b: same for Gateway configuration | `config_store_read()` |
| SD-07 | `ConfigService` calls `config_store_write()` to persist the updated configuration received from the cloud | `config_store_write()` |
| SD-10 | `ConfigService` and `DeviceProfileRegistry` call `config_store_write()` to persist provisioned parameters and device profiles | `config_store_write()` |

---

## 6. Error and fault behaviour

All public functions return `config_store_err_t`; callers must not ignore
non-OK returns.  No retry is performed by ConfigStore — ConfigService decides
whether to fall back to defaults and log the event.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `CONFIG_STORE_ERR_NOT_INIT` | Function called before `config_store_init()` succeeded | Return error; no flash access | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `CONFIG_STORE_ERR_NULL_ARG` | Null pointer passed to an output parameter | Return error; no flash access | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `CONFIG_STORE_ERR_TOO_LARGE` | Blob exceeds `CONFIG_STORE_MAX_DATA_BYTES` | Return error; no write performed | Non-OK return | No retry — schema change required; programming error at design time | Caller logs at ERROR via ILogger |
| `CONFIG_STORE_ERR_NO_VALID_SLOT` | Both flash slots have invalid CRC at boot | Apply defaults; return error | Non-OK return | No retry — ConfigService applies defaults and saves them, recovering on the next boot | `HEALTH_EVENT_CONFIG_NO_VALID_SLOT` pushed to IHealthReport; logged at WARN |
| `CONFIG_STORE_ERR_FLASH_ERASE` | QspiFlashDriver erase returned non-OK | Return error; slot left blank | Non-OK return | No retry by ConfigStore — ConfigService may retry the save on the next config-change event | Caller logs at WARN via ILogger; `HEALTH_EVENT_CONFIG_WRITE_FAIL` pushed |
| `CONFIG_STORE_ERR_FLASH_WRITE` | QspiFlashDriver program returned non-OK | Return error; slot partially written (CRC will fail on next boot) | Non-OK return | No retry by ConfigStore — ConfigService logs and operates in-memory; next save attempt clears the slot first | Caller logs at WARN via ILogger; `HEALTH_EVENT_CONFIG_WRITE_FAIL` pushed |
| `CONFIG_STORE_ERR_FLASH_READ` | QspiFlashDriver read returned non-OK | Return error; data not populated | Non-OK return | No retry by ConfigStore — treated equivalently to `NO_VALID_SLOT` by ConfigService | Caller logs at WARN via ILogger; `HEALTH_EVENT_CONFIG_READ_FAIL` pushed |


## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* Back the QSPI with a RAM array for host-side tests */
static uint8_t s_flash_sim[64U * 1024U];
#define qspi_flash_erase(addr, len)       stub_flash_erase(s_flash_sim, addr, len)
#define qspi_flash_write(addr, buf, len)  stub_flash_write(s_flash_sim, addr, buf, len)
#define qspi_flash_read(addr, buf, len)   stub_flash_read(s_flash_sim,  addr, buf, len)
#endif
```

Minimum test cases:
- Fresh flash (all 0xFF) → `config_store_load()` returns `ERR_NO_VALID_SLOT`.
- Save + load round trip → data matches.
- Save twice → second load returns second blob; first slot is superseded.
- Corrupt slot A CRC → load selects slot B.
- Corrupt both slots → `ERR_NO_VALID_SLOT`; `HEALTH_EVENT_CONFIG_NO_VALID_SLOT` fired.
- Simulated erase failure in step 2 → `ERR_FLASH_ERASE`; active slot unchanged.
- Simulated write failure in step 7 (CRC commit) → `ERR_FLASH_WRITE`; active slot unchanged on next boot.
- `config_store_erase()` → subsequent `config_store_load()` returns `ERR_NO_VALID_SLOT`.
- `data_len > max_len` on load → `ERR_TOO_LARGE`.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| CS-O1  | `CONFIG_STORE_MAX_DATA_BYTES` (32 712) must be verified against the actual serialised config struct size when ConfigService LLD is produced. If the struct exceeds this limit, the slot layout must be revised. | Verify at ConfigService LLD — struct must fit CONFIG_STORE_MAX_DATA_BYTES | Open |
| CS-O2  | QSPI flash driver interface (`IQspiFlash`) — erase and write granularity must match the slot layout. Specifically: erase unit must be ≤ 4 KB (sector erase, not bulk erase) and write unit must support byte-granular writes. Confirm at QspiFlashDriver LLD companion. | Confirm at QspiFlashDriver LLD — erase/write granularity check | Open |
| CS-O3  | Seq_number overflow — `uint32_t` gives 4 294 967 295 write cycles before wrap. At one config write per day, this exceeds 11 000 years. No overflow handling needed; document the bound in code comments. | Document overflow bound in code comments; no implementation change needed | Open |
