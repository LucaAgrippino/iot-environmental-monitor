# LLD Companion — FirmwareStore

**Board:** Gateway (GW) only
**Layer:** Middleware

This artefact specifies the software design of `FirmwareStore`: staging
of a downloaded OTA image on QSPI, its authentication, installation into
the inactive on-chip slot, and the boot-control metadata shared with the
bootloader.

**Version:** 1.0
**Date:** October 2026
**Status:** Released — implemented and tested on the host (PR `feature/phase-4-gw-firmware_store`); on-target validation blocked on the §4 prerequisites

**HLD anchor:** FirmwareStore in `components.md` (GW middleware layer);
`flash-partition-layout.md` §5, §7; `state-machines.md` Machine 3;
`sequence-diagrams.md` SD-00c, SD-06b, SD-06c, SD-06d.

**Changes from v0.2:** implementation notes only; the design is unchanged. §1, §7.1 and §7.3
(Logger, `get_boot_info` constness), §9 (failed `begin`), §21 (as-built file layout), §22 (delivered
test suites) and §24 (FS-O4, FS-O6 and FS-O8 findings, FS-O11..O13, FS-D15..D19) were updated, and
the status moved to Released.

**Changes from v0.1:** full rewrite. v0.1 could not be implemented as
written: on-chip flash access without a driver, a header layout whose
offsets did not add up, an in-place NOR rewrite for the resume offset, a
metadata layout that contradicted the bootloader contract, an apply-time
estimate wrong by ~20×, false P-claims, and SD-trace function names that
did not exist in the API. Decisions are logged in §24.

---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `IFirmwareImageStore` (consumer: UpdateService) · `IFirmwareBootControl` (consumers: UpdateService, LifecycleController) |
| **Uses** | `IQspiFlash` (QspiFlashDriver) · `IInternalFlash` (InternalFlashDriver — **new, §4**) · Logger (`LOG_*` macros; FS-D15) · mbedTLS (SHA-256, ECDSA-P256 verify) behind a private seam (§21) |
| **Hosted in task** | Called from `UpdateServiceTask` (prio 1, 4 KB) and `LifecycleTask` (prio 1). No task of its own |
| **Activation** | Synchronous calls only |
| **Instances** | One (GW ADT convention with a static pool of size 1 — one set of physical partitions) |

---

## 2. Traceability

| Concern | SRS | Where |
|---|---|---|
| Store the downloaded image | REQ-DM-050 | §10 `begin`, `write_chunk` |
| Resume after interruption | REQ-DM-051 | §10 journal, `resume` |
| Delete partial download | REQ-DM-052 | §10 `discard` |
| Validate the image | REQ-DM-060 | §11 header, slot, hash and vector checks |
| Discard on validation failure; never install unverified | REQ-DM-061 | §12 `install_begin` requires VERIFIED |
| Verify cryptographic signature | REQ-DM-080, REQ-NF-304 | §11 |
| Set the new slot as boot slot | REQ-DM-070 | §13 `activate` |
| Trigger self-check on new firmware | REQ-DM-071 | §13 trial states |
| Roll back on failed self-check | REQ-DM-072 | §13 `rollback` + bootloader rule |
| Retain current firmware until self-check passes | REQ-DM-073 | §12 (inactive slot only), §13 |
| Dual-bank partition scheme | REQ-DM-074 | §5 |
| Rollback within 10 s | REQ-NF-204 | §18 (rollback is one metadata write) |
| Known-good state after power loss during a flash write | REQ-NF-214 | §14 |
| No dynamic allocation after init | REQ-NF-408 | §9, FS-O4 |
| No vendor headers above the driver layer | REQ-NF-501 | §4 InternalFlashDriver |
| 10 s watchdog | REQ-NF-109 | §12 step-wise install |

---

## 3. Responsibility and scope

`FirmwareStore` owns the **mechanics** of firmware storage. `UpdateService`
owns the **policy**: when to start, retry, discard, activate, confirm and
roll back (Machine 3). This keeps `FirmwareStore` testable without the
OTA state machine.

**It does:**
- stage the signed image file on QSPI with a power-safe resume journal;
- authenticate the image (signature over the header, header-bound body hash);
- copy the authenticated image into the inactive on-chip slot and record a slot descriptor;
- read and write the boot-control metadata (A/B copies) whose format is shared with the bootloader;
- report boot state to LifecycleController and UpdateService (pull, P7).

**It does not:**
- talk to the cloud, decide retries, or reset the MCU;
- push health events (failures are returned to UpdateService, which reports them per REQ-DM-055);
- run at boot before the application (that is the bootloader, §13).

**Non-goals** (not in the SRS, P6): version anti-rollback enforcement,
image encryption, delta updates, Field Device OTA (D35). The signed header
carries the version, so anti-rollback can be added later without a format
change.

---

## 4. Prerequisites (blocking)

The project rule is "model first": the HLD and the VP model change before
code diverges. Four HLD amendments and three implementation dependencies
come first.

### 4.1 HLD amendments

| ID | Artefact | Change | Why |
|---|---|---|---|
| **A1** (new D42) | `flash-partition-layout.md` §5.1 | Move Slot B to `0x0808_0000–0x080F_7FFF`; reserve `0x0807_E000–0x0807_FFFF` (8 KB) and `0x080F_8000–0x080F_FFFF` (32 KB). Slot A unchanged | The STM32L475VG flash is two 512 KB hardware banks (bank 2 starts at `0x0808_0000`); read-while-write is only possible **across** banks. In the current layout Slot B straddles both banks, including its vector table. Installing into Slot A while running from Slot B would stall every interrupt vector fetch for ~22 ms per page erase, 240 times: Modbus RX overruns, missed ISR deadlines. After A1 each slot sits entirely in its own hardware bank |
| **A2** | `flash-partition-layout.md` §7, §7.1 | Replace the `pending_self_check` byte with a `boot_state` field (§6.3); one slot descriptor per sector (sector 2 = Slot A, sector 3 = Slot B); 32 B descriptor without the signature (§6.2) | (a) The current §7 protocol cannot tell the **first** boot of a new image from a reboot **during** its self-check, so the bootloader would roll back every update immediately. A trial state set by the bootloader fixes this. (b) With both descriptors in one sector, updating Slot B's descriptor erases Slot A's: power loss leaves the running image without a descriptor. (c) A 64-byte ECDSA signature cannot fit the 64 B header slot defined today |
| **A3** | `components.md` | Add driver **InternalFlashDriver** (`IInternalFlash`, CMSIS; REQ-DM-070, -073, -074). FirmwareStore USES it and PROVIDES `IFirmwareImageStore` + `IFirmwareBootControl` (P3). LifecycleController USES `IFirmwareBootControl`. Move the REQ-DM-074 trace from QspiFlashDriver to InternalFlashDriver | Slots and metadata are on-chip flash; middleware may not touch `FLASH->*` registers (P1, REQ-NF-501). No driver for it exists |
| **A4** | `sequence-diagrams.md` SD-06c msgs 2/5, SD-06d msgs 9/10'; `state-machines.md` Machine 3 Downloading text | Boot indicator and flags go to `InternalFlashDriver`, not `QspiFlashDriver`; Downloading writes to QSPI staging (D41), not the inactive bank | Diagrams predate D41 and the on-chip metadata decision |

**Consequence of A1/A2 for the build:** slots are XIP and not
position-independent, so **each release produces two signed images**, one
linked for Slot A and one for Slot B. The header carries `load_addr`, and
`verify` rejects an image linked for the wrong slot (§11). UpdateService
asks for the image matching the inactive slot (`get_boot_info`).

### 4.2 Implementation dependencies

| # | Dependency | Needed for |
|---|---|---|
| P-1 | **Done 2026-10-26** — `InternalFlashDriver` LLD + implementation (small: unlock, page erase with bank select, double-word programming, error flags, cache reset after writes) | Host tests need only its header; on-target needs the driver |
| P-2 | QSPID-O1 resolved: QspiFlashDriver takes its own mutex **per call** (closes CFL-O7 for all three consumers). Longest hold = one 4 KB erase (81 ms measured) | On-target; FirmwareStore becomes the third QSPI user |
| P-3 | A Slot-A linker script and a minimal boot stub at `0x0800_0000` (set MSP/VTOR, jump to `0x0800_6000`) | On-target tests only. A development image linked at `0x0800_0000` overlaps the metadata partition, and FirmwareStore refuses OTA operations there by design (§13.1) |

`IInternalFlash` contract required by this module (the driver LLD
finalises names):

| Operation | Contract |
|---|---|
| `erase_page(addr)` | `addr` 2 KB-aligned inside on-chip flash; selects the hardware bank itself |
| `program(addr, data, len)` | `addr` 8-aligned, `len` a multiple of 8, target erased; double-word programming; leaves caches coherent for subsequent reads |
| `read(addr, buf, len)` | Copies from mapped flash. Exists so FirmwareStore never dereferences flash addresses directly (host-testable) |

Required from `IQspiFlash` (bind to the existing names, FS-O8): `read(addr, buf, len)`, page program within one 256 B page, 4 KB sector erase.

---

## 5. Flash regions (after A1)

| Region | Device | Address | Size | Hardware bank |
|---|---|---|---|---|
| Bootloader | On-chip | `0x0800_0000–0x0800_3FFF` | 16 KB | 1 |
| Metadata copy A | On-chip | `0x0800_4000–0x0800_47FF` | 2 KB | 1 |
| Metadata copy B | On-chip | `0x0800_4800–0x0800_4FFF` | 2 KB | 1 |
| Descriptor, Slot A | On-chip | `0x0800_5000–0x0800_57FF` | 2 KB | 1 |
| Descriptor, Slot B | On-chip | `0x0800_5800–0x0800_5FFF` | 2 KB | 1 |
| **Slot A** | On-chip | `0x0800_6000–0x0807_DFFF` | 480 KB (240 pages) | 1 |
| *(reserved)* | On-chip | `0x0807_E000–0x0807_FFFF` | 8 KB | 1 |
| **Slot B** | On-chip | `0x0808_0000–0x080F_7FFF` | 480 KB (240 pages) | 2 |
| *(reserved)* | On-chip | `0x080F_8000–0x080F_FFFF` | 32 KB | 2 |
| Staging journal | QSPI | staging base + `0x0000` | 4 KB | — |
| Staging image file | QSPI | staging base + `0x1000` | ≤ 491 648 B (121 sectors) | — |

Staging uses 488 KB of the 4 MB D41 partition. The rest stays unused; the
partition is not resized.

Metadata and descriptor writes are in hardware bank 1. While running from
Slot A they stall instruction fetch for one page erase (~22 ms). This
happens at most three times per OTA cycle (`install_begin`, `activate`,
`confirm`) and is measured in TC-HW-FS-006 (FS-O10).

---

## 6. On-flash formats

All multi-byte fields are little-endian. CRC is **CRC-32/ISO-HDLC**
(poly `0x04C11DB7` reflected, init/xorout `0xFFFFFFFF`, check value
`0xCBF43926`), shared with ConfigStore. On-chip records are multiples of
8 bytes (double-word programming with ECC), and the double word holding
the CRC is programmed **last**. All structs live in
`firmware_image_format.h` with `_Static_assert` on size and field offsets.
The bootloader will include the same header.

### 6.1 Image file header (staged; 128 B)

| Offset | Size | Field | Rule |
|---|---|---|---|
| `0x00` | 4 | `magic` | `0x4F544131` |
| `0x04` | 2 | `header_version` | `1` |
| `0x06` | 2 | `header_size` | `128` |
| `0x08` | 4 | `image_len` | Body bytes; `8 ≤ image_len ≤ 491 520` |
| `0x0C` | 4 | `version` | `major<<24 \| minor<<16 \| patch` |
| `0x10` | 4 | `load_addr` | Slot base the body is linked for |
| `0x14` | 12 | `reserved` | `0` |
| `0x20` | 32 | `body_sha256` | SHA-256 of the body |
| `0x40` | 64 | `signature` | ECDSA-P256, raw `r‖s` (big-endian, 32 + 32), over SHA-256 of bytes `0x00–0x3F` |

The signature covers every header field, including `body_sha256`, so the
body is authenticated through the header. No separate header CRC is
needed: structural checks run first, then the signature (§11).

The body follows the header, unmodified: vector table at body offset 0.
The header is **not** copied into the slot.

### 6.2 Slot descriptor (on-chip; 32 B; one per descriptor sector)

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | `magic` = `0x46574453` |
| `0x04` | 4 | `image_len` |
| `0x08` | 4 | `version` |
| `0x0C` | 4 | `load_addr` (must equal the slot base) |
| `0x10` | 4 | `body_crc32` (over the body as programmed) |
| `0x14` | 4 | `reserved` = `0xFFFFFFFF` |
| `0x18` | 4 | `desc_crc32` over `0x00–0x17` |
| `0x1C` | 4 | `pad` = `0xFFFFFFFF` |

The descriptor exists only once its slot is fully installed and checked.
The bootloader checks `body_crc32` (integrity) before jumping.
Authenticity was established before installation (§11).

### 6.3 Boot metadata record (on-chip; 24 B; copies A and B)

| Offset | Size | Field | Values |
|---|---|---|---|
| `0x00` | 4 | `magic` | `0xC0DEBEEF` (unchanged from HLD) |
| `0x04` | 1 | `active_bank` | `0x0A` Slot A, `0x0B` Slot B (unchanged from HLD) |
| `0x05` | 1 | `boot_state` | `0x01` CONFIRMED · `0x02` TRIAL_PENDING · `0x03` TRIAL_RUNNING · `0x04` ROLLED_BACK |
| `0x06` | 1 | `previous_bank` | `0x0A` / `0x0B` |
| `0x07` | 1 | `reserved` | `0xFF` |
| `0x08` | 4 | `rollback_count` | Incremented on every rollback |
| `0x0C` | 4 | `generation` | Strictly increasing |
| `0x10` | 4 | `crc32` | Over `0x00–0x0F` |
| `0x14` | 4 | `pad` | `0xFFFFFFFF` |

A copy is **valid** when the magic, CRC, bank values and state value are
all legal. The selected copy is the valid one with the higher
`generation`; if both are valid with equal generation, copy A wins.

**Write protocol:** target = the copy *not* selected (copy A if neither
is valid) → erase its page → program the three double words (CRC last)
→ read back and validate → only then update the RAM cache. A failure
leaves the selected copy untouched.

### 6.4 Staging journal (QSPI sector 0; 4 KB)

| Offset | Size | Content |
|---|---|---|
| `0x000` | 16 | Session record: `magic` `0x4F54414A`, `image_id`, `file_len`, `crc32` over the first 12 B |
| `0x010` | 8 × 510 | Checkpoint entries `{value, ~value}`; `value` = bytes of the file safely staged |

Entries are written once each into erased space, so NOR never needs a
1→0→1 rewrite (the v0.1 in-place counter was not possible on NOR). Scan
rule: walk entries until the first fully erased one. Entries whose pair
check fails (torn writes) are skipped. The last valid entry is the
checkpoint, and the next append goes into the first erased entry.

A checkpoint is appended each time the write position crosses a 4 KB
boundary (value = the aligned boundary), and once at completion
(value = `file_len`). A maximum-size file needs ≤ 122 entries, against a
capacity of 510.

---

## 7. Provided interfaces

Interface-segregated headers (`ifirmware_image_store.h`,
`ifirmware_boot_control.h`) contain only the vtable and its types, per the
project rule for avoiding circular includes.

**Packaging note:** functions take the opaque instance as `self`.
Consumers receive the vtable pointer and the handle at wiring. If the
repo's GW ADT convention packages handle and ops differently (for
example, an interface struct with a `self` field, as in ConfigStore),
mirror that shape exactly. The semantics below are binding; the packaging
follows the repo.

### 7.1 Creation (wiring only)

```c
typedef struct firmware_store firmware_store_t;   /* opaque */

typedef struct {
    const iqspi_flash_t     *qspi;                /* staging            */
    const iinternal_flash_t *iflash;              /* slots + metadata   */
    uint32_t                 running_image_base;  /* VTOR value at boot (FS-O6) */
} firmware_store_config_t;

/**
 * @brief  Create the FirmwareStore instance and resolve the boot state (§13.1).
 *
 * Reads metadata and descriptors only. Never erases or programs flash.
 * @param[in]  cfg  Dependencies and the running image base.
 * @param[out] out  Instance handle.
 * @return FIRMWARE_STORE_ERR_OK; ERR_NULL_POINTER on any NULL in cfg/out;
 *         ERR_NO_INSTANCE if already created; ERR_FLASH_READ on read failure.
 * @note Threading: call once, before the scheduler starts.
 */
firmware_store_err_t firmware_store_create(const firmware_store_config_t *cfg,
                                           firmware_store_t             **out);

extern const ifirmware_image_store_t  firmware_store_image_ops;
extern const ifirmware_boot_control_t firmware_store_boot_ops;

#ifdef TEST
void firmware_store_reset_for_test(void);
#endif
```

### 7.2 `IFirmwareImageStore` — UpdateService

| Function | Purpose | Pre-condition (op state, §9) | Post |
|---|---|---|---|
| `begin(self, image_id, file_len)` | Start a fresh staging session | OTA-capable, boot state CONFIRMED, op state ≠ ACTIVATED | RECEIVING, offset 0 |
| `resume(self, image_id, file_len, uint32_t *offset_out)` | Continue a session after a disconnect or reboot | Same as `begin` | RECEIVING at checkpoint, or RECEIVED if complete |
| `write_chunk(self, offset, const uint8_t *data, len)` | Stage the next bytes of the file | RECEIVING, `offset == write_offset`, `offset + len ≤ file_len` | RECEIVED when `file_len` is reached |
| `verify(self, firmware_image_info_t *info_out)` | Authenticate the staged image (§11) | RECEIVED | VERIFIED |
| `install_begin(self)` | Invalidate the target descriptor; arm the copy | VERIFIED | INSTALLING |
| `install_step(self, bool *done_out)` | Copy and check **one** 2 KB page | INSTALLING | INSTALLED when done |
| `activate(self)` | Point the next boot at the new slot (TRIAL_PENDING) | INSTALLED, boot state CONFIRMED | ACTIVATED (reset expected) |
| `discard(self)` | Erase the journal; forget the session | op state ≠ ACTIVATED | IDLE |

### 7.3 `IFirmwareBootControl` — LifecycleController, UpdateService

| Function | Purpose | Allowed boot state | Result |
|---|---|---|---|
| `get_boot_info(const self, firmware_boot_info_t *info_out)` | Report cached boot state; no flash access | any | — |
| `confirm(self)` | Self-check passed | TRIAL_RUNNING (CONFIRMED → OK, no-op) | CONFIRMED; previous slot's descriptor erased |
| `rollback(self)` | Self-check failed | TRIAL_RUNNING | `active = previous`, ROLLED_BACK, `rollback_count + 1` |
| `acknowledge_rollback(self)` | Result reported to the cloud | ROLLED_BACK (CONFIRMED → OK, no-op) | CONFIRMED; failed slot's descriptor erased |

The caller resets after `activate` and `rollback`. FirmwareStore never
resets (LifecycleController owns the reset, Machine 1 I18).

**LifecycleController adapter mapping** (its LLD uses `get_pending_flags`):
`pending_self_check = ota_capable && state == TRIAL_RUNNING`;
`pending_rollback = ota_capable && state == ROLLED_BACK`.

Every function returns `firmware_store_err_t`. All take `self` first and
return `ERR_NULL_POINTER` for a NULL `self` or out-parameter, and
`ERR_NOT_INITIALISED` for a handle not produced by `create`.

---

## 8. Data types

```c
typedef enum {
    FIRMWARE_STORE_ERR_OK               = 0,
    FIRMWARE_STORE_ERR_NULL_POINTER     = 1,
    FIRMWARE_STORE_ERR_NOT_INITIALISED  = 2,
    FIRMWARE_STORE_ERR_NO_INSTANCE      = 3,
    FIRMWARE_STORE_ERR_BAD_PARAM        = 4,
    FIRMWARE_STORE_ERR_BAD_STATE        = 5,
    FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE  = 6,
    FIRMWARE_STORE_ERR_NO_SESSION       = 7,
    FIRMWARE_STORE_ERR_INCOMPLETE       = 8,
    FIRMWARE_STORE_ERR_TOO_LARGE        = 9,
    FIRMWARE_STORE_ERR_BAD_HEADER       = 10,
    FIRMWARE_STORE_ERR_WRONG_SLOT       = 11,
    FIRMWARE_STORE_ERR_SIG_INVALID      = 12,
    FIRMWARE_STORE_ERR_HASH_MISMATCH    = 13,
    FIRMWARE_STORE_ERR_BAD_IMAGE        = 14,
    FIRMWARE_STORE_ERR_CRYPTO           = 15,
    FIRMWARE_STORE_ERR_FLASH_READ       = 16,
    FIRMWARE_STORE_ERR_FLASH_ERASE      = 17,
    FIRMWARE_STORE_ERR_FLASH_WRITE      = 18,
    FIRMWARE_STORE_ERR_INSTALL_VERIFY   = 19,
} firmware_store_err_t;

typedef enum {
    FIRMWARE_SLOT_A    = 0,
    FIRMWARE_SLOT_B    = 1,
    FIRMWARE_SLOT_NONE = 2,   /* running image is not in a slot (dev build) */
} firmware_slot_t;

typedef enum {                 /* values are the on-flash encoding (§6.3) */
    FIRMWARE_BOOT_CONFIRMED     = 0x01U,
    FIRMWARE_BOOT_TRIAL_PENDING = 0x02U,
    FIRMWARE_BOOT_TRIAL_RUNNING = 0x03U,
    FIRMWARE_BOOT_ROLLED_BACK   = 0x04U,
} firmware_boot_state_t;

typedef struct {
    bool                  ota_capable;
    firmware_slot_t       running_slot;
    firmware_boot_state_t state;            /* meaningful only if ota_capable */
    uint32_t              rollback_count;
    uint32_t              running_version;  /* 0 if no valid descriptor     */
} firmware_boot_info_t;

typedef struct {
    uint32_t        version;
    uint32_t        image_len;
    firmware_slot_t target_slot;
} firmware_image_info_t;
```

Geometry constants (`FIRMWARE_SLOT_A_BASE`, `FIRMWARE_SLOT_B_BASE`,
`FIRMWARE_SLOT_SIZE` = 491 520, `FIRMWARE_PAGE_SIZE` = 2048,
`FIRMWARE_HEADER_SIZE` = 128, metadata and descriptor addresses) live in
`firmware_image_format.h`. `FIRMWARE_STORE_STAGING_BASE` is expressed in
the QspiFlashDriver address convention (FS-O8).

---

## 9. Internal state

```c
typedef enum {
    FS_OP_IDLE, FS_OP_RECEIVING, FS_OP_RECEIVED, FS_OP_VERIFIED,
    FS_OP_INSTALLING, FS_OP_INSTALLED, FS_OP_ACTIVATED,
} fs_op_state_t;

struct firmware_store {
    bool                    created;
    firmware_store_config_t cfg;
    firmware_boot_info_t    boot;          /* cached; updated only after a verified write */
    uint8_t                 meta_selected; /* 0 = copy A, 1 = copy B, 0xFF = none */
    firmware_boot_state_t   meta_state;
    uint8_t                 meta_active, meta_previous;
    uint32_t                meta_generation, meta_rollback_count;
    fs_op_state_t           op;
    uint32_t                image_id, file_len, write_offset;
    uint32_t                journal_next;  /* byte offset of next free journal entry */
    uint8_t                 body_digest[32];   /* from the authenticated header */
    uint32_t                image_len, version, target_base;
    uint32_t                install_page, install_crc;
    StaticSemaphore_t       mutex_buf;
    SemaphoreHandle_t       mutex;
};

static struct firmware_store s_pool[1];
static uint8_t s_work[FIRMWARE_PAGE_SIZE];   /* QSPI→flash page buffer, hash read buffer */
static uint8_t s_cmp[256];                   /* readback compare window */
```

Op-state transitions:

| From | Call | To | On error |
|---|---|---|---|
| any except ACTIVATED | `begin` | RECEIVING | IDLE if the journal was touched (FS-D17), else unchanged |
| any except ACTIVATED | `resume` | RECEIVING / RECEIVED | unchanged (`NO_SESSION`) |
| RECEIVING | `write_chunk` (last byte) | RECEIVED | IDLE (caller must `resume`) |
| RECEIVED | `verify` | VERIFIED | RECEIVED |
| VERIFIED | `install_begin` | INSTALLING | VERIFIED |
| INSTALLING | `install_step` (last page) | INSTALLED | VERIFIED (slot left without a descriptor) |
| INSTALLED | `activate` | ACTIVATED | INSTALLED |
| any except ACTIVATED | `discard` | IDLE | IDLE |

`begin` or `resume` from VERIFIED or later discards the verification
(new bytes are never trusted from an old check). The op state lives in
RAM: after a reboot UpdateService calls `resume`, and a complete staged
file comes back as RECEIVED and must be verified again.

---

## 10. Download and resume

**`begin(image_id, file_len)`**
1. Reject `image_id == 0xFFFFFFFF` (erased pattern) or `file_len ≤ 128` → `BAD_PARAM`; `file_len − 128 > 491 520` → `TOO_LARGE`.
2. Erase journal sector; program the session record; read back and check → `FLASH_*`.
3. `write_offset = 0`, `journal_next = 0x010`, op = RECEIVING.

**`write_chunk(offset, data, len)`**
1. Check `offset == write_offset`, `len > 0`, `offset + len ≤ file_len` → `BAD_PARAM` with nothing written.
2. For each byte range, split at 256 B QSPI page boundaries. When the write enters a 4 KB sector for the first time (sector-aligned position), erase that sector first (**lazy erase**: no multi-second bulk erase, and a resumed sector is always re-erased).
3. Advance `write_offset`. For every 4 KB boundary crossed, append a checkpoint with the aligned value. At `file_len`, append `file_len` and set op = RECEIVED.
4. On a flash error: return it and set op = IDLE. The journal still holds the last good checkpoint.

**`resume(image_id, file_len, offset_out)`**
Read the journal. The session record must be valid and match both
arguments → else `NO_SESSION` (the caller then calls `begin`). Scan the
checkpoints (§6.4), set `write_offset` to the last one, and return it.
At most 4 KB is downloaded again.

**`discard()`** erases the journal sector only. Image sectors are left;
lazy erase handles their reuse. Slots are never touched.

---

## 11. Verification

`verify` runs on QSPI staging. The on-chip slot is not touched (D41).
Checks run cheapest first, and authentication happens before anything is
read from the 480 KB body:

| # | Check | Error |
|---|---|---|
| 0 | op = RECEIVED | `INCOMPLETE` (RECEIVING) / `BAD_STATE` |
| 1 | `magic`, `header_version == 1`, `header_size == 128`, `image_len == file_len − 128`, `image_len ≥ 8` | `BAD_HEADER` |
| 2 | `image_len ≤ 491 520` | `TOO_LARGE` |
| 3 | `load_addr == base of the inactive slot` | `WRONG_SLOT` |
| 4 | ECDSA-P256 verify of SHA-256(header `0x00–0x3F`) with the compiled-in public key | `SIG_INVALID` (invalid) / `CRYPTO` (library failure) |
| 5 | SHA-256 over the body (2 KB QSPI reads into `s_work`) equals `body_sha256` | `HASH_MISMATCH` / `FLASH_READ` |
| 6 | Reset vector (body word 1) has the Thumb bit set and points inside `[load_addr, load_addr + image_len)` | `BAD_IMAGE` |

On success: store `body_digest`, `image_len`, `version`, `target_base`;
fill `info_out`; op = VERIFIED.

`CRYPTO` is separate from `SIG_INVALID` on purpose: an exhausted mbedTLS
arena is a resource fault (retriable), not tampering, and must not be
reported to the cloud as a forged image.

**Public key:** 64 B raw (X‖Y) P-256 key compiled into the image
(`ota_public_key.c`, generated by `tools/ota/make_image.py`). Its
integrity follows from the image signature chain. This replaces the
v0.1 CertStore plan (FS-O2 closed) and removes a provisioning dependency.

---

## 12. Installation

Installation is **step-wise**, one 2 KB page per call. With A1 the target
slot is in the other hardware bank, so the CPU keeps executing during
erase and program. UpdateServiceTask yields between steps, and the
watchdog (REQ-NF-109) is served by the normal task loop.

**`install_begin()`**: erase the target slot's descriptor sector first, so
a half-written slot is never bootable. Then `install_page = 0`, start
SHA-256, initialise the CRC; op = INSTALLING.

**`install_step(done_out)`**, for page *i*:
1. `n = min(2048, image_len − i·2048)`; read `n` bytes from staging (`0x1000 + 128 + i·2048`) into `s_work`. Pad to a multiple of 8 with `0xFF`.
2. Erase the slot page at `target_base + i·2048`; program the padded bytes.
3. Read back in 256 B windows into `s_cmp`; `memcmp` against `s_work` → `INSTALL_VERIFY`.
4. Feed the `n` bytes to SHA-256 and to the body CRC.
5. On the last page: finish SHA-256 and compare with `body_digest` → `INSTALL_VERIFY`. This re-check closes the gap between `verify` and the copy (a QSPI bit flip, or staging changed after verification). Then program the descriptor (CRC double word last), read it back, op = INSTALLED, `*done_out = true`.

Only the pages the image needs are erased. Stale pages beyond
`image_len` are outside the CRC and harmless. On any error the
descriptor stays erased, op returns to VERIFIED, and `install_begin` may
be retried.

---

## 13. Boot control and bootloader contract

### 13.1 Boot-state resolution at `create` (read-only)

1. `running_slot` = Slot A / Slot B if `running_image_base` equals a slot base; otherwise `NONE`.
2. Read both metadata copies and select one (§6.3).
3. `ota_capable` = running slot ≠ NONE **and** a valid copy exists **and** `active_bank` = running slot **and** state ∈ {CONFIRMED, TRIAL_RUNNING, ROLLED_BACK}.
4. `running_version` = running slot's descriptor version (0 if invalid).
5. Create the static mutex.

Not OTA-capable is **not** a create error: the gateway must still boot
and operate (REQ-NF-200 spirit). The cause is logged once: WARN for a
development image, ERROR for metadata that is absent or inconsistent
while running from a slot, since the bootloader should have written it
(§7.2). `TRIAL_PENDING` seen by the application means the bootloader did
not advance the state; this is treated as inconsistent.

### 13.2 State transitions (application and bootloader)

| Actor | Trigger | From | Written |
|---|---|---|---|
| App | `activate` | CONFIRMED | `active` = inactive, `previous` = running, TRIAL_PENDING |
| Bootloader | Boot | TRIAL_PENDING | TRIAL_RUNNING, then jump to `active` |
| App | `confirm` | TRIAL_RUNNING | CONFIRMED; erase descriptor of `previous` |
| App | `rollback` | TRIAL_RUNNING | `active` = `previous`, `previous` = failed slot, ROLLED_BACK, count + 1 |
| Bootloader | Boot | TRIAL_RUNNING (app never confirmed: crash, watchdog, power loss) | Same as `rollback` |
| App | `acknowledge_rollback` | ROLLED_BACK | CONFIRMED; erase descriptor of `previous` (the failed image) |
| Bootloader | Boot, no valid metadata | — | `active` = A, CONFIRMED, generation 1 (HLD §7.2) |
| Bootloader | Active descriptor invalid or body CRC fails | any | Other slot if its descriptor is valid: `active` = other, ROLLED_BACK, count + 1; else safe halt (HLD §7 step 3a limit) |

Erasing the descriptor of the slot being left behind follows Machine 3's
Committed entry action ("invalidate the now-old bank"). The bootloader's
fallback can then never boot a superseded or known-bad image.

Until the bootloader component exists, these rows are exercised by a
host-side bootloader simulator in the unit tests (TC-FS-090..092). The
on-target swap test is deferred to the bootloader (FS-O7).

---

## 14. Power-loss behaviour (REQ-NF-214)

| Interrupted during | Recovered state | Outcome |
|---|---|---|
| `begin` | No valid session | `resume` → `NO_SESSION` → fresh `begin` |
| `write_chunk` | Last checkpoint | ≤ 4 KB downloaded again; lazy erase rewrites the sector |
| Journal append | Torn entry skipped | Previous checkpoint used |
| `install_begin` / `install_step` | Target descriptor erased; metadata unchanged | Boots the current image; staging still complete, so `resume` → RECEIVED → `verify` again |
| Descriptor write | CRC invalid | Slot not installed |
| `activate` | Old copy selected (CONFIRMED, old active) | Boots the current image. The update is silently not applied; UpdateService/cloud job timeout covers it (UpdateService LLD) |
| Bootloader TRIAL_PENDING → RUNNING | TRIAL_PENDING kept | Bootloader repeats the transition on the next boot |
| `confirm` (before the CRC) | TRIAL_RUNNING kept | Bootloader rolls back a good image. Conservative; reported as a rollback. Accepted |
| `confirm` (after metadata, before descriptor erase) | CONFIRMED; old descriptor still valid | Harmless; overwritten at the next install |
| `rollback` | TRIAL_RUNNING kept | Bootloader performs the same rollback (converges) |
| `acknowledge_rollback` | ROLLED_BACK kept | Result reported again (idempotent on the cloud side) |

Invariant: at every point at least one metadata copy is valid, and it
names a slot whose descriptor was valid when the record was written.

---

## 15. Sequence integration

| SD / Machine 3 | Role | Function |
|---|---|---|
| SD-00c msg 3–4 | LifecycleController reads post-update state | `get_boot_info` |
| SD-06b msgs 4–7, Machine 3 T2, I1 | Stage chunks | `begin` / `resume`, `write_chunk` |
| SD-06b msg 9'', Machine 3 T4 | Delete partial download (DM-052) | `discard` |
| SD-06b msg 9, Machine 3 Validating | Signature and integrity | `verify` |
| SD-06b msg 11''', Machine 3 T6 | Discard after validation failure | `discard` |
| SD-06c msgs 1–6, Machine 3 Applying, T7 | Install, then set the new boot slot | `install_begin`, `install_step` loop, `activate` |
| SD-06c msgs 13–14 | Post-boot check | `get_boot_info` → TRIAL_RUNNING |
| SD-06d msgs 8–9, Machine 3 T9 | Commit | `confirm` |
| SD-06d msgs 9'–10', Machine 3 T10/T11 | Roll back | `rollback` |
| SD-06d msgs 15'–16', Machine 3 T12 | Rollback reported | `get_boot_info` → ROLLED_BACK, then `acknowledge_rollback` |

Machine 3 Idle entry "clear update flags" needs no call: flags are
replaced by `boot_state`, which is CONFIRMED in Idle.

**Required changes in the UpdateService companion (Draft):** request the
image for `get_boot_info().running_slot`'s opposite slot; drive
`install_step` in a loop with a yield; on ROLLED_BACK at boot, report and
then `acknowledge_rollback`; apply the retry classes of §19.

---

## 16. Thread safety

- One static mutex (priority inheritance, D26) is taken at entry of every public function except `create`, and held for the whole call. The longest holds are `verify` (FS-O5, target < 2 s) and one `install_step` (~45 ms). LifecycleController calls happen at boot or after self-check, when UpdateService is idle, so contention is theoretical.
- Lock order is **FirmwareStore mutex → QspiFlashDriver internal lock** (P-2). The reverse never happens, so there is no deadlock.
- InternalFlashDriver has a single runtime user (this module); it needs no lock. The bootloader is a separate image.
- `s_work`, `s_cmp` and the crypto context are used only under the mutex.
- No ISR access.

---

## 17. Initialisation order

```
qspi_flash_driver_init()
internal_flash_driver_init()
logger_init()
firmware_store_create(&cfg, &fs)   /* reads metadata, never writes; before scheduler */
… other middleware / application create calls …
lifecycle_controller_init(…, boot-control ops + fs handle, …)
vTaskStartScheduler()
LifecycleTask CheckingIntegrity → get_boot_info()
```

`running_image_base` is supplied by wiring from the vector-table base the
image booted with (FS-O6).

---

## 18. Memory and timing

| RAM item | Size |
|---|---|
| Instance (pool of 1) | ~130 B |
| `s_work` | 2048 B |
| `s_cmp` | 256 B |
| `StaticSemaphore_t` | ~80 B |
| SHA-256 context (crypto seam) | ~110 B |
| **Total static** | **~2.6 KB** |
| Stack (own frames) | < 256 B, excluding mbedTLS ECDSA (FS-O4) |

Flash: public key 64 B; code estimated ≤ 6 KB excluding mbedTLS.

| Operation | Estimate | Basis |
|---|---|---|
| `begin`, `discard` | ~85 ms | 1 QSPI sector erase (81 ms measured) |
| `write_chunk`, per 4 KB staged | ~132 ms | 81 ms erase + 16 × 3.2 ms page program (measured) |
| `verify` | < 2 s target | SHA-256 over ≤ 480 KB + one ECDSA verify; FS-O5 |
| `install_step` | ~43 ms | RM0351/datasheet: page erase ~22 ms + 256 double words × ~82 µs; FS-O1 |
| Full install (240 pages) | ~10.3 s worst | Not one blocking call: step-wise, no CPU stall after A1 |
| `activate`, `rollback`, `acknowledge_rollback` | ~25 ms | 1 page erase + 3 double words |
| `confirm` | ~45 ms | Metadata write + descriptor erase |
| Rollback budget (REQ-NF-204) | ~25 ms + reset + boot | Well inside 10 s |

v0.1 estimated the install at ~500 ms (2 ms per page). On-chip page
erase alone is ~22 ms, hence the step API.

---

## 19. Error behaviour

FirmwareStore never retries and never reports to the cloud. It returns
codes; UpdateService decides. The class column is the contract for the
UpdateService LLD.

| Error | Cause | Local behaviour | Class for UpdateService | Observability |
|---|---|---|---|---|
| `NULL_POINTER`, `NOT_INITIALISED` | Programming error | No flash access | Bug — no retry | ERROR (no logging if the logger pointer itself is NULL) |
| `NO_INSTANCE` | Second `create` | None | Bug | ERROR |
| `BAD_PARAM` | Non-contiguous or oversized chunk, illegal `image_id` | Nothing written | Bug or protocol error — `resume` re-syncs offset | WARN |
| `BAD_STATE` | Call out of order; trial or rollback pending | None | Bug | WARN |
| `NOT_OTA_CAPABLE` | Dev image or inconsistent metadata | None | Reject the OTA command (REQ-DM-055 failure report) | Logged once at create |
| `NO_SESSION` | No matching journal session | None | Call `begin` | DEBUG |
| `INCOMPLETE` | `verify` before the last byte | None | Bug | WARN |
| `TOO_LARGE`, `BAD_HEADER`, `WRONG_SLOT`, `SIG_INVALID`, `HASH_MISMATCH`, `BAD_IMAGE` | Image rejected | Op state RECEIVED | **Terminal**: `discard`, Failed (REQ-DM-061, -062) | ERROR, with the code |
| `CRYPTO` | mbedTLS failure (memory or internal) | Op state RECEIVED | Retriable once, then Failed | ERROR |
| `FLASH_READ` | QSPI or on-chip read failed | Op state unchanged | Retriable (counts toward DM-052's 3 attempts) | ERROR |
| `FLASH_ERASE`, `FLASH_WRITE` | Erase or program failed or read back wrong | During download: op = IDLE. During install: descriptor erased, op = VERIFIED. Metadata: selected copy untouched | Retriable (DM-052 count); a metadata failure in `confirm`/`rollback` → Machine 1 Faulted path | ERROR |
| `INSTALL_VERIFY` | Readback or post-copy digest mismatch | Descriptor erased, op = VERIFIED | Retry install once; then `discard`, Failed | ERROR |

Every failure path logs once, inside FirmwareStore, at the level above,
with the code and the address involved. There are no silent failures.

---

## 20. Principles applied

- **P1 — applied.** Depends only on the driver layer (QspiFlashDriver, InternalFlashDriver), Logger, and the mbedTLS library. No application-layer dependency. On-chip flash registers are reached only through InternalFlashDriver (amendment A3).
- **P2 — not applicable.** No upward data flow: callers pull state and receive return codes.
- **P3 — applied.** LifecycleController needs boot state, confirm and rollback but never staging; UpdateService needs both. Hence `IFirmwareBootControl` and `IFirmwareImageStore`.
- **P4 — applied.** Logger referenced concretely as a cross-cutting component.
- **P5 — not applicable, deliberately.** No counters and no pushed events. Failures go back to UpdateService, which reports per REQ-DM-055. This matches `components.md` (no `IHealthReport` in FirmwareStore's USES).
- **P6 — applied.** Every operation traces to §2; non-goals are listed in §3 and not implemented.
- **P7 — applied.** `get_boot_info` is a pull interface used by both consumers.
- **P8 — applied.** No helper infrastructure is introduced. The crypto seam is a private file-level test seam, not a component.
- **P9 — not applicable** at module level.
- **P10 — applied.** Component `FirmwareStore`; interfaces `IFirmwareImageStore` → `ifirmware_image_store_t`, `IFirmwareBootControl` → `ifirmware_boot_control_t`; prefix `firmware_store_`; errors `FIRMWARE_STORE_ERR_*`.

Coding standard (BARR-C subset, not a P-principle): fixed-width types,
no magic numbers (all magics and offsets named in
`firmware_image_format.h`), `_Static_assert` on every on-flash struct, no
floating point, no heap.

---

## 21. File layout

```
firmware/gateway/middleware/firmware_store/
├── firmware_store.h            create, config, ops externs, reset_for_test
├── firmware_store_types.h      error codes, enums, info structs (shared by both interface headers)
├── ifirmware_image_store.h     vtable only
├── ifirmware_boot_control.h    vtable only
├── firmware_image_format.h     §6 structs, magics, geometry, static asserts (bootloader will share)
├── firmware_store.c            public API, op-state machine, verify, install, mutex
├── fs_metadata.c / .h          on-chip flash access, A/B metadata, descriptors   (private)
├── fs_staging.c / .h           QSPI access, journal, lazy erase, checkpoints     (private)
├── ota_crypto_types.h          crypto seam types and prototypes (no mbedTLS types, no .c)
├── ota_crypto.c / .h           SHA-256 + ECDSA-P256 verify over mbedTLS          (private seam)
├── ota_public_key.c / .h       64 B release public key: committed as an all-zero placeholder
└── (generated by tools/ota/make_image.py genkey: the real ota_public_key.c)

firmware/gateway/drivers/internal_flash/
└── iinternal_flash.h           IInternalFlash contract only; the driver is prerequisite P-1

tools/ota/
├── make_image.py               genkey, sign (link-base check), vectors (TEST ONLY)
└── README.md                   key handling: release private key never in git

tests/gateway/middleware/firmware_store/
├── test_firmware_store.c       handle checks, verification, install, mutex, format      (31)
├── test_fs_staging.c           download, journal, resume, discard                       (28)
├── test_fs_metadata.c          boot resolution, metadata protocol, boot control, sim    (34)
├── test_fs_power_loss.c        cut-at-every-operation sweeps                             (6)
├── test_fs_crypto_vectors.c    real mbedTLS on host with generated vectors              (12)
├── test_fs_crypto_placeholder.c the all-zero placeholder key fails closed                (3)
├── fakes/fake_qspi_flash.c     NOR semantics: erase→0xFF, program only clears bits
├── fakes/fake_internal_flash.c ECC semantics: programming a non-erased double word fails; write guards; op log
├── fakes/power_cut.c           fail after N operations, leaving a torn write
├── fakes/fake_ota_crypto.c     deterministic stand-in for the crypto seam
├── fakes/fs_test_util.c        logger spy, seeding, drivers over the public API (no crypto)
├── fakes/fs_test_image.c       signed-image builder using the fake crypto
├── sim/bootloader_sim.c        §13.2 bootloader rows
├── mbedtls_host/               unity build of the mbedTLS sources + a minimal test config
└── vectors/test_key_*.pem, test_vectors.h   TEST ONLY keys and images, generated by make_image.py

firmware/gateway/integration-tests/firmware_store/   bring-up harness (Slot-A linked, P-1 and P-3)
scripts/ota_feed.py             streams an image over the debug UART to the harness
```

Tests run in `tests/project_gateway.yml` (not the default project): `config_store_crc.c` is shared
with ConfigStore and the Gateway and Field Device ConfigStore share basenames. `test-module.sh`
runs `test_firmware_store` only; the other five suites are run by name.

**Fakes, not mocks, for flash.** A mock only checks call sequences. A
fake with real NOR and ECC semantics catches whole classes of bugs, such
as v0.1's in-place counter rewrite. The crypto is a hand-written fake with
error injection (`fake_ota_crypto`), and the Logger a spy; CMock is not used. FreeRTOS comes from
the project mock.

---

## 22. Unit test plan (host)

Each case runs after `firmware_store_reset_for_test()` and fresh fakes.

**Delivered (v1.0):** all 70 cases below are implemented, as 114 test functions in six suites (§21),
none ignored. Cases
with a letter suffix (`TC-FS-023b`, `-031c` ...) are extra guards found while writing the test and
trace to the same row. `TC-FS-M01..M05` cover the metadata write protocol of `fs_metadata.c` (§6.3),
`TC-FS-C01..C06` and `C10..C12` the crypto seam itself (known-answer SHA-256, independently signed
ECDSA vectors, degenerate signatures, the placeholder key). `TC-FS-041/044/045/046/047` run twice: with
the fake crypto for the state flow, and with real mbedTLS in `test_fs_crypto_vectors.c`. The power-loss
sweeps (`TC-FS-085..088`) cut at every mutating flash operation of the run and were checked against a
mutant that overwrites the selected metadata copy: all four metadata sweeps fail on it.

**Create and boot resolution**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-001 | NULL `cfg`, any NULL member, NULL `out` → `NULL_POINTER` | §7.1 |
| TC-FS-002 | Second `create` → `NO_INSTANCE` | — |
| TC-FS-003 | Ops on a handle not from `create` → `NOT_INITIALISED`; NULL `self`/out → `NULL_POINTER` | — |
| TC-FS-004 | Blank metadata, running from Slot A → not capable; ERROR logged | §13.1 |
| TC-FS-005 | `running_image_base` not a slot → not capable; WARN | §13.1 |
| TC-FS-006 | Valid CONFIRMED, active = running → capable; version from descriptor | DM-074 |
| TC-FS-007 | `active_bank` ≠ running slot → not capable | §13.1 |
| TC-FS-008 | TRIAL_PENDING seen by app → not capable | §13.1 |
| TC-FS-009 | Both copies valid → higher generation selected | NF-214 |
| TC-FS-010 | Newer copy torn (bad CRC) → older selected | NF-214 |
| TC-FS-011 | `create` performs zero erase/program operations in every case | §7.1 |

**Download and resume**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-020 | `begin` when not capable → `NOT_OTA_CAPABLE` | DM-050 |
| TC-FS-021 | `begin` with TRIAL_RUNNING or ROLLED_BACK → `BAD_STATE` | DM-073 |
| TC-FS-022 | `file_len ≤ 128` or `image_id = 0xFFFFFFFF` → `BAD_PARAM`; oversize → `TOO_LARGE` | DM-060 |
| TC-FS-023 | `begin` writes a valid session record; offset 0 | DM-051 |
| TC-FS-024 | Non-contiguous offset → `BAD_PARAM`, nothing programmed | — |
| TC-FS-025 | Chunk past `file_len` → `BAD_PARAM` | — |
| TC-FS-026 | Chunk spanning a 256 B page → split programs; data correct | — |
| TC-FS-027 | Each 4 KB sector erased exactly once, on first entry | §10 |
| TC-FS-028 | Checkpoint appended at each 4 KB crossing; values aligned | DM-051 |
| TC-FS-029 | Final byte → RECEIVED; checkpoint = `file_len` | DM-050 |
| TC-FS-030 | Flash error mid-chunk → `FLASH_WRITE`; op IDLE | DM-052 |
| TC-FS-031 | `resume` with matching session → last checkpoint | DM-051 |
| TC-FS-032 | `resume` with id or length mismatch → `NO_SESSION` | DM-051 |
| TC-FS-033 | Torn last journal entry → previous checkpoint used | NF-214 |
| TC-FS-034 | Interrupt, `resume`, finish → staged file byte-identical to source | DM-051 |
| TC-FS-035 | `resume` of a complete file → RECEIVED | DM-051 |
| TC-FS-036 | `discard` → journal erased; `resume` → `NO_SESSION`; slots untouched | DM-052 |
| TC-FS-037 | Maximum-size file uses ≤ 122 journal entries | §6.4 |

**Verification** (TC-FS-041/045/046/047 use real mbedTLS and generated vectors)

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-040 | `verify` while RECEIVING → `INCOMPLETE` | — |
| TC-FS-041 | Valid image → OK; info filled; op VERIFIED | DM-060, DM-080 |
| TC-FS-042 | Bad magic, header version or header size → `BAD_HEADER` | DM-060 |
| TC-FS-043 | `image_len` ≠ `file_len − 128` → `BAD_HEADER` | DM-060 |
| TC-FS-044 | `load_addr` = running slot → `WRONG_SLOT` | DM-073 |
| TC-FS-045 | One byte of the signed header changed (e.g. version) → `SIG_INVALID` | DM-080, NF-304 |
| TC-FS-046 | One body byte changed → `HASH_MISMATCH` | DM-060 |
| TC-FS-047 | Signed with a different key → `SIG_INVALID` | NF-304 |
| TC-FS-048 | Crypto backend error (mock) → `CRYPTO`, never `SIG_INVALID` | §11 |
| TC-FS-049 | Reset vector outside the image, or Thumb bit clear → `BAD_IMAGE` | DM-060 |
| TC-FS-050 | QSPI read failure → `FLASH_READ` | — |
| TC-FS-051 | `begin`/`resume` after VERIFIED → verification discarded | §9 |

**Installation**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-060 | `install_begin` without VERIFIED → `BAD_STATE` | DM-061 |
| TC-FS-061 | Target descriptor erased before any slot page (fake operation log) | NF-214 |
| TC-FS-062 | Step loop → slot bytes equal body; descriptor correct (len, version, load_addr, CRC) | DM-070 |
| TC-FS-063 | No write ever lands in the running slot, the bootloader or metadata (fake write-guard) | DM-073 |
| TC-FS-064 | Number of steps = ⌈image_len / 2048⌉ | §12 |
| TC-FS-065 | Last partial page padded with `0xFF`; CRC covers `image_len` only | §6.2 |
| TC-FS-066 | Readback bit flip (fake) → `INSTALL_VERIFY`; no descriptor | §12 |
| TC-FS-067 | Staging altered after `verify` → `INSTALL_VERIFY` | §12 |
| TC-FS-068 | Erase failure → `FLASH_ERASE`; op VERIFIED; second `install_begin` succeeds | — |

**Boot control**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-070 | `activate` without INSTALLED → `BAD_STATE` | DM-070 |
| TC-FS-071 | `activate` → TRIAL_PENDING, active = inactive, previous = running, generation + 1, other copy intact | DM-070, DM-071 |
| TC-FS-072 | `activate` when not capable → `NOT_OTA_CAPABLE` | — |
| TC-FS-073 | `confirm` in TRIAL_RUNNING → CONFIRMED; previous descriptor erased | DM-073 |
| TC-FS-074 | `confirm` in CONFIRMED → OK with zero writes | — |
| TC-FS-075 | `confirm` in ROLLED_BACK or TRIAL_PENDING → `BAD_STATE` | — |
| TC-FS-076 | `rollback` → active = previous, ROLLED_BACK, count + 1 | DM-072 |
| TC-FS-077 | `rollback` in CONFIRMED → `BAD_STATE` | — |
| TC-FS-078 | `acknowledge_rollback` → CONFIRMED; failed slot's descriptor erased | DM-072 |
| TC-FS-079 | `get_boot_info` performs no flash access | P7 |

**Power loss** (cut at every operation index N, re-create, check invariants)

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-085 | Download + resume sweep → resumed file always identical to source | NF-214, DM-051 |
| TC-FS-086 | Install sweep → metadata unchanged; target descriptor valid only if the install completed | NF-214 |
| TC-FS-087 | `activate` sweep → selected record is either old CONFIRMED or new TRIAL_PENDING | NF-214 |
| TC-FS-088 | `confirm`, `rollback`, `acknowledge_rollback` sweeps → selected record is pre or post state (§14) | NF-214 |

**Bootloader contract (simulator)**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-090 | Activate → sim boot → TRIAL_RUNNING on new slot → `confirm` → CONFIRMED | DM-070, DM-071 |
| TC-FS-091 | Self-check fail: `rollback` → sim boot → previous slot, ROLLED_BACK → `acknowledge_rollback` | DM-072 |
| TC-FS-092 | Crash during trial: two sim boots without `confirm` → rolled back to previous | DM-072, NF-204 |

**Concurrency and format**

| TC | Behaviour | Traces |
|---|---|---|
| TC-FS-095 | Mutex taken and given exactly once around each public op, on success and error paths | §16 |
| TC-FS-098 | CRC-32 of `"123456789"` = `0xCBF43926` | §6 |
| TC-FS-099 | Format: struct sizes and offsets (compile-time `_Static_assert`; listed for traceability) | §6 |

---

## 23. Integration test plan (on target, Slot-A harness, P-3)

**Status (v1.0):** the harness `integration-tests/firmware_store/main_test_firmware_store.c` and the
host feeder `scripts/ota_feed.py` are written and the harness compiles for the target, but it
**cannot link or run** until InternalFlashDriver (P-1) and the Slot-A link (P-3) exist (FS-O12).
TC-HW-FS-006 is measured with a 1 kHz TIM7 interrupt instead of a logic analyser. None of the rows
below has been executed.

| TC | Setup | Pass criterion |
|---|---|---|
| TC-HW-FS-001 | Harness seeds CONFIRMED / active A; create | `get_boot_info` = capable, Slot A, CONFIRMED |
| TC-HW-FS-002 | Stream a max-size Slot-B image via `scripts/ota_feed.py` | RECEIVED; staging matches source digest; staging time logged |
| TC-HW-FS-003 | Pull power at random during TC-HW-FS-002, ×5 | `resume` offset ≤ bytes sent and 4 KB-aligned; final `verify` OK |
| TC-HW-FS-004 | `verify` on the staged image | OK; duration logged (FS-O5); mbedTLS arena peak logged (FS-O4) |
| TC-HW-FS-005 | Install while SensorTask and ModbusPoller run | Slot B equals body; per-step and total time logged (FS-O1); SensorTask worst cycle and Modbus CRC/timeout counters unchanged from baseline (read-while-write proof, A1) |
| TC-HW-FS-006 | 1 kHz timer ISR toggling a GPIO, logic analyser on it, during `activate` and `confirm` | Longest gap logged (FS-O10); ≤ 30 ms |
| TC-HW-FS-007 | Image with one flipped body byte | `HASH_MISMATCH` |
| TC-HW-FS-008 | Image linked for Slot A | `WRONG_SLOT` |
| TC-HW-FS-009 | `activate`, then harness dumps both metadata copies over UART | TRIAL_PENDING, active = B, generation + 1, other copy intact |
| TC-HW-FS-010 | *(deferred to bootloader, FS-O7)* Full swap, self-check pass/fail, crash during trial | Boots B, confirm/rollback as §13.2; rollback ≤ 10 s (NF-204) |

---

## 24. Open items and decisions

### 24.1 Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| FS-O1 | Install time: estimated ~43 ms per page, ~10.3 s for 240 pages, from datasheet figures | Measure in TC-HW-FS-005 | Open |
| FS-O2 | OTA public-key location | Compiled into the image (FS-D5) | **Closed** |
| FS-O3 | Streaming hash buffer | Static 2 KB `s_work`; QSPI reads split by FirmwareStore | **Closed** |
| FS-O4 | mbedTLS memory: confirm the project build uses the static `MBEDTLS_MEMORY_BUFFER_ALLOC_C` arena (REQ-NF-408), that `MBEDTLS_ECDSA_C` and `MBEDTLS_ECP_DP_SECP256R1_ENABLED` are on, and that one ECDSA verify fits in the arena **while a TLS session is open** | **Checked at implementation:** `mbedtls_config_gateway.h` has `MBEDTLS_ECP_C`, `MBEDTLS_ECP_DP_SECP256R1_ENABLED`, `MBEDTLS_SHA256_C`, `MBEDTLS_BIGNUM_C`, `MBEDTLS_ASN1_PARSE_C` and `MBEDTLS_ASN1_WRITE_C`, but **not `MBEDTLS_ECDSA_C`**, and no `MBEDTLS_MEMORY_BUFFER_ALLOC_C`. Enabling `MBEDTLS_ECDSA_C` in the TLS configuration is a prerequisite for linking `ota_crypto.c` (until then the module is excluded from the firmware image). The arena and its peak are still to be measured in TC-HW-FS-004 | Open |
| FS-O5 | `verify` duration (target < 2 s) | Measure in TC-HW-FS-004 | Open |
| FS-O6 | Source of `running_image_base`: CpuDriver accessor for VTOR, or the linker symbol of the vector table | **Checked at implementation:** `cpu.h` has no VTOR accessor. The harness reads `SCB->VTOR` directly; the real wiring (LifecycleController `main()`) must supply it, and a CpuDriver accessor is the cleaner home | Open |
| FS-O7 | Bootloader not built: on-target swap, trial and rollback deferred | Bootloader component LLD (D36); reuse `firmware_image_format.h` and the §13.2 table | Open |
| FS-O8 | Bind `IQspiFlash` names and its address convention for `FIRMWARE_STORE_STAGING_BASE` | Bound to `iqspi_flash_t` (`read`, `write_page`, `erase_sector`) and the device-offset convention: `FIRMWARE_STORE_STAGING_BASE` = `0x0012_0000`, i.e. `0x9012_0000 − 0x9000_0000`, as ConfigStore does | **Closed** |
| FS-O9 | Two signed images per release (one per slot) need two linker scripts and a build step | Bootloader/build LLD | Open |
| FS-O10 | Fetch stall from metadata writes in hardware bank 1 while running from Slot A (≤ 3 per OTA) | Measure in TC-HW-FS-006; Modbus retries (REQ-MB-060) absorb one lost frame | Open |
| FS-O11 | HLD amendments A1–A4 (§4.1) are **not applied**: `flash-partition-layout.md` §5.1/§7, `components.md`, `sequence-diagrams.md` and `state-machines.md` still show the old Slot B address, the `pending_self_check` byte and no InternalFlashDriver. The code follows this companion, not the HLD | Apply A1–A4 in the VP model and the HLD documents before the bootloader and InternalFlashDriver LLDs | Open |
| FS-O12 | **P-1 InternalFlashDriver now exists and is hardware-validated** (`docs/lld/drivers/internal-flash-driver.md`, 2026-10-26; page erase 22.1 ms, 2 KB program 22.3 ms). Still missing: the per-call QSPI mutex (P-2, QSPID-O1) and the Slot-A linker script with a boot stub (P-3). FirmwareStore is excluded from the firmware image (`.cproject`), its harness cannot link, and TC-HW-FS-001..009 are not run | Separate LLD and PR per prerequisite; then enable the module and run the harness | Open |
| FS-O13 | No production caller: UpdateService and FirmwareStore wiring in LifecycleController (`get_boot_info` → `pending_self_check`, `pending_rollback`) are stand-ins | UpdateService implementation; LifecycleController adapter per §7.3 | Open |

### 24.2 Decisions

| ID | Decision | Rationale |
|---|---|---|
| FS-D1 | Header stays in staging; the slot holds the raw body; a 32 B descriptor goes to its own sector | Body runs XIP from the slot base; descriptors never share an erase unit |
| FS-D2 | `boot_state` trial machine replaces the pending flags (A2) | Distinguishes the first trial boot from a failed one; every power-loss point converges (§14) |
| FS-D3 | Slot-specific images; `load_addr` signed; `WRONG_SLOT` check | XIP slots are not position-independent |
| FS-D4 | Signature over the header digest; header binds the body digest; signature checked before the body hash | Every field authenticated; no 480 KB read for a forged header |
| FS-D5 | Public key compiled in | Integrity through the signed image; no provisioning dependency |
| FS-D6 | Lazy per-sector erase and a 4 KB checkpoint journal | NOR-correct; ≤ 4 KB re-download; no 10-second bulk erase |
| FS-D7 | Step-wise install; slots in separate hardware banks (A1) | Watchdog and real-time tasks unaffected during a ~10 s copy |
| FS-D8 | `IFirmwareImageStore` / `IFirmwareBootControl` split | P3: two consumer classes with different needs |
| FS-D9 | Readback compare plus re-hash during install | Closes the window between verify and copy |
| FS-D10 | No `IHealthReport`; errors returned to UpdateService | Consistent with `components.md`; the cloud report is UpdateService's job |
| FS-D11 | `CRYPTO` separate from `SIG_INVALID` | A resource fault is not tampering |
| FS-D12 | Module mutex; QSPI lock inside the driver; lock order FS → QSPI | Two caller tasks; three QSPI users (closes CFL-O7 with P-2) |
| FS-D13 | Not OTA-capable is a state, not a boot failure | The gateway keeps operating without OTA |
| FS-D14 | `confirm` and `acknowledge_rollback` erase the descriptor of the slot left behind | Machine 3 Committed entry action; bootloader fallback can never boot a superseded or known-bad image |
| FS-D15 | Logging uses the `LOG_*` macros of Logger; there is no `ilogger_t` in the repository and the `log` member of `firmware_store_config_t` is dropped. Flash failures are logged at the failing call with the address; other failures once, at the level of §19 | Logger is a cross-cutting component used directly (P4); an interface nobody else implements would only add a seam |
| FS-D16 | `firmware_store_types.h` holds the error codes, enums and info structs; `ota_crypto_types.h` holds the crypto types and prototypes and has no matching `.c`; `ota_public_key.h` is a marker header | Two interface headers must share the types without including the creation API; Ceedling links a `.c` only when a test includes its header, so the real and fake crypto can each be selected by a test |
| FS-D17 | A failed `begin` (journal erase, program or read-back) leaves the operation state IDLE, not unchanged as §9 said for v0.2 | The journal sector has already been erased, so an older RECEIVING session no longer exists; leaving it RECEIVING would accept chunks that no resume could find |
| FS-D18 | `get_boot_info` takes `const firmware_store_t *self` | It is a pure query; the vtable type says so |
| FS-D19 | The committed `ota_public_key.c` is an all-zero placeholder, which is not a point on P-256. `ota_ecdsa_p256_verify` then returns the library-failure result, and `verify` returns `CRYPTO`, never `SIG_INVALID`. No image can be installed until `make_image.py genkey` provisions a key. A failed mutex creation maps to `NO_INSTANCE` (the code list has no resource error) | Fail closed, and do not report an unprovisioned device as a forged image |

---

## 25. Phase H checklist

| # | Check | Status |
|---|---|---|
| H-01 | Every public function specified with pre-conditions, effects and errors | Done (§7, §10–§13) |
| H-02 | Every error code in the error table with cause, local behaviour, caller class, observability | Done (§19) |
| H-03 | Init ordering with pre-conditions | Done (§17) |
| H-04 | Synchronisation strategy and lock order | Done (§16) |
| H-05 | `_reset_for_test()` hook guarded by `#ifdef TEST` | Done (§7.1) |
| H-06 | File layout matches project structure | Done (§21) |
| H-07 | Unit-test plan with TC identifiers covering all functions and error codes | Done (§22) |
| H-08 | Open items with structured IDs and resolution paths | Done (§24.1) |
| H-09 | Principles section contains no false claims; non-applicable principles stated as such | Done (§20) |
| H-10 | SD trace uses function names from §7 | Done (§15) |
| H-11 | State machine mapping (Machine 3) documented | Done (§15) |
| H-12 | RAM, ROM and timing estimates | Done (§18) |
| H-13 | SRS traces verified against section content | Done (§2) |
| H-14 | Decisions logged with rationale | Done (§24.2) |
| H-15 | On-flash formats with exact offsets and power-loss analysis | Done (§6, §14) |
| H-16 | Blocking HLD amendments identified | Done (§4). **Not yet applied** — FS-O11 |

---

## References

- `docs/components.md` (GW FirmwareStore, UpdateService, LifecycleController)
- `docs/flash-partition-layout.md` §5, §7, D36–D41
- `docs/state-machines.md` Machine 1 (UpdatingFirmware, I18), Machine 3
- `docs/sequence-diagrams.md` SD-00c, SD-06a–d
- `docs/task-breakdown.md` (`UpdateServiceTask`, D23, D26)
- `docs/lld/application/lifecycle-controller.md` (`get_pending_flags` consumer)
- `docs/lld/application/update-service-lld.md` (Draft — to be aligned, §15)
- `docs/lld/drivers/qspi-flash-driver.md` (QSPID-O1)
- `docs/architecture-principles.md` P1, P3, P4, P6, P7, P8, P10
- ST RM0351 (STM32L4x5/L4x6 reference manual) — flash organisation, read-while-write, programming and erase timings

---

*LLD companion v0.2, produced for Pass H review.*
