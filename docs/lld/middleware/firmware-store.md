# LLD Companion — FirmwareStore

**Layer:** Middleware  
**Board:** Gateway (GW) only  
**Provides:** `IFirmwareStore`  
**Consumes:** `IQspiFlash` (QspiFlashDriver), `ILogger`  
**SRS traces:** REQ-DM-050, REQ-DM-051, REQ-DM-052, REQ-DM-060, REQ-DM-061, REQ-DM-070, REQ-DM-071, REQ-DM-072, REQ-DM-073, REQ-DM-074, REQ-DM-080, REQ-NF-204, REQ-NF-304  
**HLD ref:** `components.md` §Middleware — FirmwareStore; `flash-partition-layout.md` §5.1, §5.2 (D36–D41); `state-machines.md` Machine 3; `sequence-diagrams.md` SD-06a–d

---

## 1. Sources

FirmwareStore manages firmware image storage across three flash regions:

1. **QSPI staging** — receives download chunks; survives power-loss for resumability (REQ-DM-051).
2. **On-chip inactive bank** — receives the verified image after all chunks arrive and signature passes.
3. **On-chip metadata** — owns the boot pointer, `pending_self_check`, and `pending_rollback` flags; controls the bootloader's bank selection.

FirmwareStore owns the mechanics. `UpdateService` (Application) owns the policy: when to begin, retry, discard, commit, and roll back. This separation keeps `FirmwareStore` testable in isolation from the OTA state machine.

FirmwareStore does **not** hold `IHealthReport` — failures are logged via `ILogger` and returned as error codes for `UpdateService` to act on and report to the cloud.

---

## 2. Flash regions used

From `flash-partition-layout.md` §5.1 and §5.2:

| Region | Location | Address | Size | Purpose |
|--------|----------|---------|------|---------|
| QSPI staging | QSPI | `0x9012_0000` – `0x9051_FFFF` | 4 MB | Download buffer; resumable; image header stored here |
| Metadata | On-chip | `0x0800_4000` – `0x0800_5FFF` | 8 KB | Boot pointer, pending flags, image headers |
| Bank A | On-chip | `0x0800_6000` – `0x0807_DFFF` | 480 KB | Firmware image (active or inactive) |
| Bank B | On-chip | `0x0807_E000` – `0x080F_5FFF` | 480 KB | Firmware image (active or inactive) |

The 4 MB staging region is retained (D41) so the full image can be
received and verified on QSPI before a single byte of the inactive
on-chip bank is touched. This prevents Bank B corruption from an
interrupted streaming write.

---

## 3. Firmware image header

Every downloaded image begins with a fixed-size header stored at the
start of the QSPI staging region. The header is validated before the
image is copied to the inactive on-chip bank.

```c
/* firmware_store.h */

#define FIRMWARE_IMAGE_MAGIC   0xFEEDFACEUL
#define FIRMWARE_HEADER_SIZE   128U   /* padded to 128 bytes */

typedef struct __attribute__((packed)) {
    uint32_t magic;           /* FIRMWARE_IMAGE_MAGIC                        */
    uint32_t image_len;       /* byte count of firmware body (excl. header)  */
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint8_t  sha256[32];      /* SHA-256 of image body (excl. header)        */
    uint8_t  signature[64];   /* ECDSA-P256 signature over sha256 field      */
    uint32_t header_crc32;    /* CRC32 over bytes [0 .. 123]; last field     */
    uint8_t  reserved[4];     /* pad to 128 bytes                            */
} firmware_image_header_t;
```

The on-chip metadata partition stores a copy of the header for the
active bank (and optionally the inactive bank) so the bootloader and
`LifecycleController` can inspect version and integrity without touching
the staging region.

---

## 4. Metadata partition layout

The metadata partition (8 KB, on-chip) uses A/B sector rotation for
its most frequently written fields (D38) to spread wear and protect
against power loss.

```
Metadata partition (8 KB, 4 × 2 KB on-chip sectors):

Sector 0 (0x0800_4000): metadata slot A ─┐ A/B rotation
Sector 1 (0x0800_4800): metadata slot B ─┘

Sector 2 (0x0800_5000): Image header copy — Bank A
Sector 3 (0x0800_5800): Image header copy — Bank B
```

Each metadata slot (A or B) contains:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* 0xDEADC0DEUL                               */
    uint32_t seq_num;             /* monotonically increasing — governs slot selection */
    uint8_t  active_bank;         /* 0 = Bank A, 1 = Bank B                    */
    uint8_t  pending_self_check;  /* 1 = new firmware booted; awaiting result   */
    uint8_t  pending_rollback;    /* 1 = rollback flagged; resume to Failed     */
    uint8_t  rollback_count;      /* incremented on each rollback               */
    uint32_t reserved;
    uint32_t slot_crc32;          /* CRC32 over all fields above               */
} firmware_metadata_t;
```

Write protocol: same A/B rotation as ConfigStore — erase target slot,
write fields, write CRC last. On boot, select the slot with the higher
valid `seq_num`.

---

## 5. Data types

```c
/* firmware_store.h */

typedef enum {
    FIRMWARE_STORE_ERR_OK           = 0,
    FIRMWARE_STORE_ERR_NOT_INIT     = 1,
    FIRMWARE_STORE_ERR_NULL_ARG     = 2,
    FIRMWARE_STORE_ERR_FLASH_ERASE  = 3,
    FIRMWARE_STORE_ERR_FLASH_WRITE  = 4,
    FIRMWARE_STORE_ERR_FLASH_READ   = 5,
    FIRMWARE_STORE_ERR_BAD_HEADER   = 6,   /* magic or header CRC invalid   */
    FIRMWARE_STORE_ERR_SHA_MISMATCH = 7,   /* SHA-256 body hash mismatch    */
    FIRMWARE_STORE_ERR_SIG_INVALID  = 8,   /* ECDSA-P256 signature invalid  */
    FIRMWARE_STORE_ERR_TOO_LARGE    = 9,   /* image_len > 480 KB            */
    FIRMWARE_STORE_ERR_NO_METADATA  = 10,  /* metadata partition unreadable */
} firmware_store_err_t;

typedef enum {
    FIRMWARE_SLOT_A = 0,
    FIRMWARE_SLOT_B = 1,
} firmware_slot_t;
```

---

## 2. Public API — `IFirmwareStore`

```c
/**
 * @brief  Initialise FirmwareStore.
 *
 * Reads the metadata partition to determine the active bank and any
 * pending flags. Must be called after qspi_flash_driver_init().
 */
firmware_store_err_t firmware_store_init(void);

/**
 * @brief  Prepare the staging region for a new download.
 *
 * Erases the QSPI staging region and stores image_len for range checks.
 * Persists a "download in progress, offset = 0" record for resumability.
 *
 * @param  image_len  Total expected image body byte count (excl. header).
 */
firmware_store_err_t firmware_store_begin(uint32_t image_len);

/**
 * @brief  Write one chunk to the staging region.
 *
 * Writes data to QSPI staging at the given byte offset (measured from
 * the start of the image body, after the header). Offset must be
 * contiguous with the last write — non-sequential writes are rejected.
 * Persists the highest written offset for resumability (REQ-DM-051).
 *
 * @param  offset  Byte offset into the image body.
 * @param  data    Chunk payload.
 * @param  len     Chunk byte count.
 */
firmware_store_err_t firmware_store_write_chunk(uint32_t       offset,
                                                 const uint8_t *data,
                                                 uint32_t       len);

/**
 * @brief  Return the last successfully persisted byte offset.
 *
 * Called by UpdateService on resume to compute the starting point for
 * the next partial download request (REQ-DM-051).
 *
 * @param[out] offset_out  Last persisted offset; 0 if no download in progress.
 */
firmware_store_err_t firmware_store_get_resume_offset(uint32_t *offset_out);

/**
 * @brief  Verify the staged image.
 *
 * Performs in order:
 *   1. Header CRC32 check.
 *   2. image_len range check (must be ≤ 480 KB = inactive bank size).
 *   3. SHA-256 of image body against header.sha256 (REQ-DM-060).
 *   4. ECDSA-P256 signature over sha256 field against the provisioned
 *      public key (REQ-DM-080, REQ-NF-304).
 *
 * All verification steps run on QSPI staging. The inactive on-chip
 * bank is not touched until firmware_store_apply() (D41).
 *
 * @return ERR_BAD_HEADER, ERR_SHA_MISMATCH, or ERR_SIG_INVALID on failure.
 */
firmware_store_err_t firmware_store_verify(void);

/**
 * @brief  Copy the verified image from QSPI staging to the inactive on-chip bank.
 *
 * Erases the inactive bank sectors, then copies header + image body
 * from QSPI staging. Reads back and verifies each written sector
 * (SHA-256 comparison) before advancing to the next — defence-in-depth
 * against silent write errors.
 *
 * Long-running: at 480 KB, expect ~500 ms. UpdateService must not block
 * other system functions during this call — schedule appropriately.
 *
 * @return ERR_FLASH_ERASE or ERR_FLASH_WRITE on failure.
 */
firmware_store_err_t firmware_store_apply(void);

/**
 * @brief  Commit the slot switch: update boot pointer and set pending_self_check.
 *
 * Writes the metadata slot with active_bank = inactive bank,
 * pending_self_check = 1, seq_num++. After this call, the next reboot
 * will boot the new firmware (REQ-DM-070, REQ-DM-073).
 */
firmware_store_err_t firmware_store_commit_slot(void);

/**
 * @brief  Erase the QSPI staging region.
 *
 * Called on download failure (REQ-DM-052) or signature failure (REQ-DM-061).
 * Does not affect the on-chip banks.
 */
firmware_store_err_t firmware_store_discard(void);

/**
 * @brief  Revert the boot pointer to the previously active bank.
 *
 * Sets pending_rollback = 1; reverts active_bank to the previous slot;
 * increments rollback_count. After the subsequent reboot, the bootloader
 * selects the previous bank (REQ-DM-072, REQ-NF-204).
 */
firmware_store_err_t firmware_store_rollback(void);

/**
 * @brief  Confirm the self-check succeeded.
 *
 * Clears pending_self_check in the metadata. Called by UpdateService
 * when SelfChecking succeeds — marks the new firmware as permanently
 * committed (REQ-DM-071, REQ-DM-073).
 */
firmware_store_err_t firmware_store_confirm_self_check(void);

/**
 * @brief  Return the currently active bank (A or B).
 */
firmware_store_err_t firmware_store_get_active_slot(firmware_slot_t *slot_out);

/**
 * @brief  Return the pending_self_check flag.
 *
 * Called by LifecycleController at boot to detect a post-OTA resume
 * condition.
 */
firmware_store_err_t firmware_store_get_pending_flags(bool *self_check_out,
                                                       bool *rollback_out);
```

---

## 7. Verification sequence (§6 `firmware_store_verify()`)

```
1. Read header from QSPI staging base (128 bytes).
   Check magic == FIRMWARE_IMAGE_MAGIC.
   Compute CRC32 over bytes [0..123]; compare to header.header_crc32.
   → Mismatch: return ERR_BAD_HEADER.

2. Check header.image_len ≤ (480 KB − FIRMWARE_HEADER_SIZE).
   → Exceeds: return ERR_TOO_LARGE.

3. Compute SHA-256 over image body (bytes [FIRMWARE_HEADER_SIZE ..
   FIRMWARE_HEADER_SIZE + image_len − 1]) on QSPI.
   Compare to header.sha256[32].
   → Mismatch: return ERR_SHA_MISMATCH.          (REQ-DM-060)

4. Verify ECDSA-P256 signature:
   mbedTLS: mbedtls_ecdsa_verify() with the provisioned OTA public key
   over the 32-byte hash from step 3.
   → Fail: return ERR_SIG_INVALID.               (REQ-DM-080, REQ-NF-304)

5. Return FIRMWARE_STORE_ERR_OK.
```

The OTA public key is provisioned separately from the firmware image.
It is stored in the CertStore partition (`0x9001_0000`, 64 KB) alongside
the MQTT X.509 certificates. FirmwareStore reads it at `firmware_store_init()`.

SHA-256 is computed streaming, sector by sector (4 KB chunks), to avoid
requiring a 480 KB RAM buffer. mbedTLS SHA-256 context is stack-allocated
within `firmware_store_verify()` — approximately 216 bytes.

---

## 8. `firmware_store_apply()` sector-by-sector write

```
inactive_bank_base = (active_bank == SLOT_A) ? BANK_B_BASE : BANK_A_BASE;

for each 2 KB sector in inactive bank:
    1. Erase sector.
    2. Read 2 KB from QSPI staging at corresponding offset.
    3. Write 2 KB to on-chip sector.
    4. Read back on-chip sector.
    5. Compare to source — SHA-256 or memcmp.
       → Mismatch: return ERR_FLASH_WRITE (sector corrupted; bank is unusable;
         UpdateService will discard and retry the entire OTA).
```

At 480 KB / 2 KB = 240 sectors, and ~2 ms per sector erase/write cycle
(STM32L475 embedded flash, 1 MHz programming clock): estimated ≈ 500 ms.
Confirm at integration — see FS-O1.

---

## 9. Resumable download design

The resume offset is persisted in the first 16 bytes of the QSPI staging
region ahead of the image header:

```
QSPI staging layout:
  Offset  0:  resume_magic  (0xRESU4A5AUL)
  Offset  4:  resume_offset (uint32_t — last written byte)
  Offset  8:  image_len     (uint32_t — expected total)
  Offset 12:  crc16         (uint16_t — over bytes [0..11]; power-loss protection)
  Offset 16:  firmware_image_header_t (128 bytes)
  Offset 144: image body bytes
```

`firmware_store_write_chunk()` updates `resume_offset` after each
successful chunk write. The update is a 16-byte in-place write (QSPI
NOR allows byte-granular writes without erase). On power loss, the
latest persisted resume offset is valid; the in-progress chunk may
be missing, but the offset was not yet updated, so the next request
restarts from the last safe byte.

---

## 3. Internal design

```c
/* firmware_store.c */

typedef struct {
    bool             initialised;
    firmware_slot_t  active_slot;
    bool             pending_self_check;
    bool             pending_rollback;
    uint8_t          rollback_count;
    uint32_t         staged_image_len;    /* set by firmware_store_begin() */
    uint32_t         staged_resume_offset;
    uint8_t          ota_public_key[64];  /* ECDSA-P256 public key, loaded at init */
} FirmwareStoreState;

static FirmwareStoreState s_fs;
```

No mutex. FirmwareStore is called only from `UpdateServiceTask`. All
operations run in that task's context.

---

## 11. Init ordering

```
qspi_flash_driver_init()     ← driver ready
firmware_store_init()        ← reads metadata; loads OTA public key from CertStore
[LifecycleController]        ← calls get_pending_flags() to detect post-OTA resume
```

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
static uint8_t s_qspi_sim[4U * 1024U * 1024U];   /* 4 MB staging */
static uint8_t s_chip_sim[1U * 1024U * 1024U];   /* 1 MB on-chip */
/* Stub QspiFlashDriver reads/writes against sim arrays */
/* Stub mbedTLS SHA-256 and ECDSA with known test vectors */
#endif
```

Minimum test cases:
- `begin()` → staging region erased; resume_offset = 0.
- `write_chunk()` × N → data in staging at correct offsets.
- `get_resume_offset()` after partial write → returns last persisted offset.
- `verify()` with valid header, correct SHA-256, valid ECDSA → `ERR_OK`.
- `verify()` with corrupted header CRC → `ERR_BAD_HEADER`.
- `verify()` with wrong SHA-256 → `ERR_SHA_MISMATCH`.
- `verify()` with invalid ECDSA → `ERR_SIG_INVALID`.
- `verify()` with image_len > 480 KB → `ERR_TOO_LARGE`.
- `apply()` → inactive bank receives correct bytes; readback matches.
- `commit_slot()` → metadata slot updated; active_bank flipped; pending_self_check = 1.
- `confirm_self_check()` → pending_self_check = 0 in metadata.
- `rollback()` → active_bank reverted; pending_rollback = 1; rollback_count++.
- `discard()` → staging erased; resume_offset = 0.
- Boot with `pending_self_check = 1` → `get_pending_flags()` returns true.
- Boot after power loss mid-`commit_slot()` → valid previous metadata slot selected.

---

## 8. Open items

| ID    | Item |
|-------|------|
| FS-O1 | `firmware_store_apply()` duration — estimated 500 ms at 240 sectors × 2 ms. Measure at integration; if watchdog period is shorter than this, the watchdog must be kicked within the apply loop or suspended for the duration (document the suspension explicitly). |
| FS-O2 | OTA public key provisioning path — the public key must be written to the CertStore partition during manufacturing/provisioning. Confirm the provisioning tool writes it at the correct CertStore sub-offset. FirmwareStore needs that offset defined in `cert_store_config.h`. |
| FS-O3 | SHA-256 streaming chunk size — 4 KB sector-by-sector is proposed. Confirm that the QspiFlashDriver read API supports 4 KB reads efficiently without introducing an extra 4 KB RAM buffer in FirmwareStore (driver may already have an internal buffer). |
| FS-O4 | mbedTLS ECDSA-P256 RAM cost — stack allocation of `mbedtls_ecdsa_context` (~1.5 KB) inside `firmware_store_verify()`. Must be verified against UpdateServiceTask stack budget alongside the existing mbedTLS usage in CloudPublisherTask. |
