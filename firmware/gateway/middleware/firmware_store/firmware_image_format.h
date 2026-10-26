/**
 * @file firmware_image_format.h
 * @brief On-flash formats, magics and geometry shared by FirmwareStore and the bootloader.
 *
 * All multi-byte fields are little-endian. CRC is CRC-32/ISO-HDLC (poly
 * 0x04C11DB7 reflected, init and xorout 0xFFFFFFFF, check value 0xCBF43926),
 * the same routine as ConfigStore. On-chip records are multiples of 8 bytes
 * (double-word programming with ECC) and the double word holding the CRC is
 * programmed last. The bootloader will include this header too.
 *
 * @see docs/lld/middleware/firmware-store.md §5, §6
 */

#ifndef FIRMWARE_IMAGE_FORMAT_H
#define FIRMWARE_IMAGE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Geometry (after HLD amendment A1)                                       */
/* ---------------------------------------------------------------------- */

/** Page size of the on-chip flash. */
#define FIRMWARE_PAGE_SIZE (2048U)

/** Slot A base: bank 1. */
#define FIRMWARE_SLOT_A_BASE (0x08006000UL)

/** Slot B base: bank 2 (amendment A1). */
#define FIRMWARE_SLOT_B_BASE (0x08080000UL)

/** Bytes in one slot: 240 pages of 2 KB. */
#define FIRMWARE_SLOT_SIZE (491520UL)

/** Size of the staged image header. */
#define FIRMWARE_HEADER_SIZE (128U)

/** Boot metadata copy A and B (one page each). */
#define FIRMWARE_META_A_ADDR (0x08004000UL)
#define FIRMWARE_META_B_ADDR (0x08004800UL)

/** Slot descriptors (one page each, so neither erase touches the other). */
#define FIRMWARE_DESC_A_ADDR (0x08005000UL)
#define FIRMWARE_DESC_B_ADDR (0x08005800UL)

/** Bootloader region (never written by the application). */
#define FIRMWARE_BOOTLOADER_BASE (0x08000000UL)
#define FIRMWARE_BOOTLOADER_SIZE (0x4000UL)

/**
 * Offset of the staging journal in the QspiFlashDriver address convention
 * (device offset; the OTA staging partition is mapped at 0x9012_0000).
 */
#define FIRMWARE_STORE_STAGING_BASE (0x00120000UL)

/** Journal sector size and the offset of the image file behind it. */
#define FIRMWARE_JOURNAL_SIZE (4096U)
#define FIRMWARE_STAGING_IMAGE_OFFSET (0x1000UL)

/** Granularity of the resume checkpoints: one QSPI sector. */
#define FIRMWARE_CHECKPOINT_STEP (4096U)

/** Largest body the slot can hold, and so the largest staged file less the header. */
#define FIRMWARE_MAX_IMAGE_LEN (FIRMWARE_SLOT_SIZE)

/* ---------------------------------------------------------------------- */
/* Magics and enumerations                                                 */
/* ---------------------------------------------------------------------- */

#define FIRMWARE_IMAGE_MAGIC (0x4F544131UL) /**< Staged image header.   */
#define FIRMWARE_IMAGE_HEADER_VERSION (1U)
#define FIRMWARE_DESC_MAGIC (0x46574453UL)    /**< Slot descriptor.       */
#define FIRMWARE_META_MAGIC (0xC0DEBEEFUL)    /**< Boot metadata record.  */
#define FIRMWARE_JOURNAL_MAGIC (0x4F54414AUL) /**< Staging session record. */

#define FIRMWARE_BANK_A (0x0AU)
#define FIRMWARE_BANK_B (0x0BU)

#define FIRMWARE_META_STATE_CONFIRMED (0x01U)
#define FIRMWARE_META_STATE_TRIAL_PENDING (0x02U)
#define FIRMWARE_META_STATE_TRIAL_RUNNING (0x03U)
#define FIRMWARE_META_STATE_ROLLED_BACK (0x04U)

/** Value of an erased flash word. */
#define FIRMWARE_ERASED_WORD (0xFFFFFFFFUL)

/** Erased-session guard: an image id equal to the erased pattern is illegal. */
#define FIRMWARE_IMAGE_ID_ERASED (0xFFFFFFFFUL)

/** Signature and digest sizes. */
#define FIRMWARE_SHA256_LEN (32U)
#define FIRMWARE_SIGNATURE_LEN (64U)

/** Bytes of the header covered by the signature (offsets 0x00..0x3F). */
#define FIRMWARE_SIGNED_HEADER_LEN (0x40U)

/* ---------------------------------------------------------------------- */
/* Image file header — staged, 128 B                                       */
/* ---------------------------------------------------------------------- */

typedef struct
{
    uint32_t magic;          /**< 0x00 FIRMWARE_IMAGE_MAGIC.                          */
    uint16_t header_version; /**< 0x04 1.                                             */
    uint16_t header_size;    /**< 0x06 128.                                           */
    uint32_t image_len;      /**< 0x08 Body bytes; 8 <= image_len <= 491 520.         */
    uint32_t version;        /**< 0x0C major<<24 | minor<<16 | patch.                 */
    uint32_t load_addr;      /**< 0x10 Slot base the body is linked for.              */
    uint8_t reserved[12];    /**< 0x14 Zero.                                          */
    uint8_t body_sha256[FIRMWARE_SHA256_LEN];  /**< 0x20 SHA-256 of the body.        */
    uint8_t signature[FIRMWARE_SIGNATURE_LEN]; /**< 0x40 ECDSA-P256 r||s, big-endian. */
} firmware_image_header_t;

/* ---------------------------------------------------------------------- */
/* Slot descriptor — on-chip, 32 B                                         */
/* ---------------------------------------------------------------------- */

typedef struct
{
    uint32_t magic;      /**< 0x00 FIRMWARE_DESC_MAGIC.                    */
    uint32_t image_len;  /**< 0x04                                         */
    uint32_t version;    /**< 0x08                                         */
    uint32_t load_addr;  /**< 0x0C Must equal the slot base.               */
    uint32_t body_crc32; /**< 0x10 Over the body as programmed.            */
    uint32_t reserved;   /**< 0x14 0xFFFFFFFF.                             */
    uint32_t desc_crc32; /**< 0x18 Over 0x00..0x17.                        */
    uint32_t pad;        /**< 0x1C 0xFFFFFFFF.                             */
} firmware_slot_descriptor_t;

/** Bytes of the descriptor covered by desc_crc32. */
#define FIRMWARE_DESC_CRC_SPAN (0x18U)

/* ---------------------------------------------------------------------- */
/* Boot metadata record — on-chip, 24 B, copies A and B                    */
/* ---------------------------------------------------------------------- */

typedef struct
{
    uint32_t magic;          /**< 0x00 FIRMWARE_META_MAGIC.                      */
    uint8_t active_bank;     /**< 0x04 FIRMWARE_BANK_A or _B.                    */
    uint8_t boot_state;      /**< 0x05 FIRMWARE_META_STATE_*.                    */
    uint8_t previous_bank;   /**< 0x06 FIRMWARE_BANK_A or _B.                    */
    uint8_t reserved;        /**< 0x07 0xFF.                                     */
    uint32_t rollback_count; /**< 0x08 Incremented on every rollback.            */
    uint32_t generation;     /**< 0x0C Strictly increasing.                      */
    uint32_t crc32;          /**< 0x10 Over 0x00..0x0F.                          */
    uint32_t pad;            /**< 0x14 0xFFFFFFFF.                               */
} firmware_boot_meta_t;

/** Bytes of the metadata record covered by crc32. */
#define FIRMWARE_META_CRC_SPAN (0x10U)

/* ---------------------------------------------------------------------- */
/* Staging journal — QSPI sector 0, 4 KB                                   */
/* ---------------------------------------------------------------------- */

/** Session record at offset 0: magic, image_id, file_len, crc32 over the first 12 B. */
typedef struct
{
    uint32_t magic;
    uint32_t image_id;
    uint32_t file_len;
    uint32_t crc32;
} firmware_journal_session_t;

#define FIRMWARE_JOURNAL_SESSION_CRC_SPAN (12U)

/** Checkpoint entry: {value, ~value}; value = bytes of the file safely staged. */
typedef struct
{
    uint32_t value;
    uint32_t inverse;
} firmware_journal_entry_t;

/** Offset of the first checkpoint entry and the number the journal can hold. */
#define FIRMWARE_JOURNAL_ENTRIES_OFFSET (0x10U)
#define FIRMWARE_JOURNAL_ENTRY_COUNT (510U)

/* ---------------------------------------------------------------------- */
/* Compile-time layout checks                                              */
/* ---------------------------------------------------------------------- */

_Static_assert(sizeof(firmware_image_header_t) == FIRMWARE_HEADER_SIZE, "image header is 128 B");
_Static_assert(offsetof(firmware_image_header_t, image_len) == 0x08U, "image_len offset");
_Static_assert(offsetof(firmware_image_header_t, version) == 0x0CU, "version offset");
_Static_assert(offsetof(firmware_image_header_t, load_addr) == 0x10U, "load_addr offset");
_Static_assert(offsetof(firmware_image_header_t, body_sha256) == 0x20U, "body_sha256 offset");
_Static_assert(offsetof(firmware_image_header_t, signature) == FIRMWARE_SIGNED_HEADER_LEN,
               "signature offset");

_Static_assert(sizeof(firmware_slot_descriptor_t) == 32U, "slot descriptor is 32 B");
_Static_assert(offsetof(firmware_slot_descriptor_t, body_crc32) == 0x10U, "body_crc32 offset");
_Static_assert(offsetof(firmware_slot_descriptor_t, desc_crc32) == FIRMWARE_DESC_CRC_SPAN,
               "desc_crc32 offset");

_Static_assert(sizeof(firmware_boot_meta_t) == 24U, "boot metadata is 24 B");
_Static_assert(offsetof(firmware_boot_meta_t, active_bank) == 0x04U, "active_bank offset");
_Static_assert(offsetof(firmware_boot_meta_t, rollback_count) == 0x08U, "rollback_count offset");
_Static_assert(offsetof(firmware_boot_meta_t, generation) == 0x0CU, "generation offset");
_Static_assert(offsetof(firmware_boot_meta_t, crc32) == FIRMWARE_META_CRC_SPAN, "crc32 offset");

_Static_assert(sizeof(firmware_journal_session_t) == 16U, "journal session is 16 B");
_Static_assert(sizeof(firmware_journal_entry_t) == 8U, "journal entry is 8 B");
_Static_assert(FIRMWARE_JOURNAL_ENTRIES_OFFSET + (FIRMWARE_JOURNAL_ENTRY_COUNT * 8U) ==
                   FIRMWARE_JOURNAL_SIZE,
               "journal entries fill the sector exactly");

#endif /* FIRMWARE_IMAGE_FORMAT_H */
