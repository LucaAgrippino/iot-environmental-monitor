# Technical Exercise — ConfigStore

## Brief (3 minutes)

An embedded system persists its configuration to NOR flash using an A/B slot
rotation. Each 32 KB slot holds a 16-byte header (magic, seq_number, data_len,
reserved), a variable-length data blob, and a CRC32 at the last 4 bytes. The
CRC covers the entire slot minus the CRC field. On power loss the CRC is
written last, so an interrupted write always leaves the slot's CRC invalid.

On load, the module reads both slots, validates each with its CRC, and selects
the slot with the higher `seq_number` among valid ones. If neither is valid it
returns an error and pushes a health event. You must implement the load path.

## Given files

### config_store_exercise.h

```c
#ifndef CONFIG_STORE_EXERCISE_H
#define CONFIG_STORE_EXERCISE_H

#include <stdbool.h>
#include <stdint.h>

#define CS_MAGIC           0xC0FFEE00UL
#define CS_SLOT_SIZE       (32U * 1024U)  /* 32 KB */
#define CS_HEADER_SIZE     (16U)
#define CS_CRC_OFFSET      (0x7FF8U)      /* byte offset of CRC within slot */
#define CS_CRC_COVERAGE    (0x7FF8U)      /* bytes covered: 0x0000..0x7FF7 */
#define CS_MAX_DATA_BYTES  (32712U)

typedef struct {
    uint32_t magic;
    uint32_t seq_number;
    uint32_t data_len;
    uint32_t reserved;
} cs_header_t;

typedef enum {
    CS_OK              = 0,
    CS_ERR_NO_VALID    = 1,
    CS_ERR_TOO_LARGE   = 2,
    CS_ERR_FLASH_READ  = 3,
} cs_err_t;

/**
 * @brief Compute CRC32/ISO-HDLC (zlib, polynomial 0xEDB88320).
 * @param buf  Data buffer.
 * @param len  Byte count.
 * @return CRC32 value.
 */
uint32_t cs_crc32(const uint8_t *buf, uint32_t len);

/**
 * @brief Read len bytes from flash at address addr into buf.
 * @return 0 on success, non-zero on error.
 */
int cs_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief Load the most-recent valid config blob from two A/B slots.
 *
 * Reads both slots starting at slot_a_base and slot_b_base.
 * Selects the valid slot with the higher seq_number.
 * If neither slot is valid returns CS_ERR_NO_VALID.
 * If selected data_len exceeds max_len returns CS_ERR_TOO_LARGE.
 *
 * @param[in]  slot_a_base  Base address of slot A.
 * @param[in]  slot_b_base  Base address of slot B.
 * @param[out] data_out     Caller-supplied buffer.
 * @param[out] len_out      Set to blob byte count on success.
 * @param[in]  max_len      Capacity of data_out in bytes.
 * @return CS_OK on success; error code on failure.
 */
cs_err_t cs_load(uint32_t slot_a_base, uint32_t slot_b_base,
                 void *data_out, uint32_t *len_out, uint32_t max_len);

#endif /* CONFIG_STORE_EXERCISE_H */
```

### config_store_exercise.c (partial)

```c
#include "config_store_exercise.h"
#include <string.h>

/* Validate one slot: check magic, then CRC over CS_CRC_COVERAGE bytes.
 * Returns true and writes seq_number to *seq_out on success. */
static bool validate_slot(uint32_t base, uint32_t *seq_out)
{
    cs_header_t hdr;
    uint8_t     chunk[256U];
    uint32_t    off, rlen, crc_state, computed_crc, stored_crc;

    if (cs_flash_read(base, (uint8_t *)&hdr, sizeof(hdr)) != 0)
    {
        return false;
    }
    if (hdr.magic != CS_MAGIC)
    {
        return false;
    }
    if (cs_flash_read(base + CS_CRC_OFFSET, (uint8_t *)&stored_crc,
                      sizeof(stored_crc)) != 0)
    {
        return false;
    }

    /* TODO: compute CRC over CS_CRC_COVERAGE bytes in 256-byte chunks.
     *       Compare with stored_crc; return false on mismatch. */
    /* TODO: write hdr.seq_number to *seq_out and return true. */
    return false;  /* replace this */
}

cs_err_t cs_load(uint32_t slot_a_base, uint32_t slot_b_base,
                 void *data_out, uint32_t *len_out, uint32_t max_len)
{
    bool     valid[2U];
    uint32_t seq[2U];
    uint8_t  selected;
    uint32_t base;
    cs_header_t hdr;

    /* TODO: validate both slots; select the valid one with the higher seq.
     *       Return CS_ERR_NO_VALID if neither is valid.
     *       Return CS_ERR_TOO_LARGE if selected data_len > max_len.
     *       Copy data blob to data_out; set *len_out. */

    return CS_ERR_NO_VALID;  /* replace this */
}
```

## Questions

Q1: Why is the CRC written as the very last operation, after the header and data blob?

Answer: Writing the CRC last is the power-loss safety mechanism. If the device loses
power during any earlier write step (header or data), the slot contains an incremented
seq_number but no valid CRC. The load path will therefore never select it over an
older, CRC-valid slot. Only after the CRC write commits does the slot become the new
"active" slot on the next boot.

Q2: The CRC covers all 32760 bytes from offset 0x0000 to 0x7FF7, not just the header
and data. Why include the unwritten 0xFF bytes between the data end and 0x7FF7?

Answer: Including the full slot (minus CRC field) makes the CRC scope independent of
`data_len`. If the CRC only covered header + data, the scope would need to be stored
and re-applied on every validation — adding complexity and a new failure mode. By
always covering exactly CS_CRC_COVERAGE bytes, the validation path is a fixed-length
read and a single CRC comparison. The 0xFF padding bytes are deterministic erased-flash
values, so they do not introduce any information loss.

Q3: In `validate_slot`, the CRC is computed in 256-byte chunks rather than reading the
entire 32760-byte slot into a single buffer. Why?

Answer: The module uses no dynamic memory and must keep stack usage bounded. A 32 KB
stack allocation would likely overflow the FreeRTOS task stack. The streaming CRC API
(start → repeated feed → finish) allows arbitrary-length inputs to be processed in
fixed-size chunks, keeping stack usage at only chunk-size (256 bytes) regardless of
blob size.

## Model solution

```c
static bool validate_slot(uint32_t base, uint32_t *seq_out)
{
    cs_header_t hdr;
    uint8_t     chunk[256U];
    uint32_t    off, rlen, crc_state, computed_crc, stored_crc;

    if (cs_flash_read(base, (uint8_t *)&hdr, sizeof(hdr)) != 0)
    {
        return false;
    }
    if (hdr.magic != CS_MAGIC)
    {
        return false;
    }
    if (cs_flash_read(base + CS_CRC_OFFSET, (uint8_t *)&stored_crc,
                      sizeof(stored_crc)) != 0)
    {
        return false;
    }

    crc_state = 0xFFFFFFFFUL;
    off       = 0U;
    while (off < CS_CRC_COVERAGE)
    {
        rlen = CS_CRC_COVERAGE - off;
        if (rlen > sizeof(chunk))
        {
            rlen = sizeof(chunk);
        }
        if (cs_flash_read(base + off, chunk, rlen) != 0)
        {
            return false;
        }
        /* feed chunk into running CRC state */
        crc_state = cs_crc32_feed(crc_state, chunk, rlen);
        off += rlen;
    }
    computed_crc = crc_state ^ 0xFFFFFFFFUL;

    if (computed_crc != stored_crc)
    {
        return false;
    }

    *seq_out = hdr.seq_number;
    return true;
}

cs_err_t cs_load(uint32_t slot_a_base, uint32_t slot_b_base,
                 void *data_out, uint32_t *len_out, uint32_t max_len)
{
    bool     valid[2U];
    uint32_t seq[2U];
    uint32_t bases[2U];
    uint8_t  selected;
    uint32_t base;
    cs_header_t hdr;

    bases[0U] = slot_a_base;
    bases[1U] = slot_b_base;

    seq[0U]   = 0U;
    seq[1U]   = 0U;
    valid[0U] = validate_slot(bases[0U], &seq[0U]);
    valid[1U] = validate_slot(bases[1U], &seq[1U]);

    if (!valid[0U] && !valid[1U])
    {
        return CS_ERR_NO_VALID;
    }

    if (!valid[0U])
    {
        selected = 1U;
    }
    else if (!valid[1U])
    {
        selected = 0U;
    }
    else
    {
        selected = (seq[0U] >= seq[1U]) ? 0U : 1U;
    }

    base = bases[selected];

    if (cs_flash_read(base, (uint8_t *)&hdr, sizeof(hdr)) != 0)
    {
        return CS_ERR_FLASH_READ;
    }
    if (hdr.data_len > max_len)
    {
        return CS_ERR_TOO_LARGE;
    }

    if (cs_flash_read(base + CS_HEADER_SIZE, (uint8_t *)data_out, hdr.data_len) != 0)
    {
        return CS_ERR_FLASH_READ;
    }

    *len_out = hdr.data_len;
    return CS_OK;
}
```

## Marking guide

Must have:
- `validate_slot` reads and checks magic before attempting CRC.
- CRC computed over exactly CS_CRC_COVERAGE bytes starting at slot base.
- CRC computed incrementally (chunks) — not a single 32 KB read.
- CRC compare with stored value; false on mismatch.
- seq_number written to output only when CRC is valid.
- Slot selection: valid slot with higher seq_number wins.
- CS_ERR_NO_VALID returned when both invalid.
- CS_ERR_TOO_LARGE returned before reading data when data_len > max_len.
- len_out written only on success.

Good to have:
- Consistent error returns for flash-read failures inside `validate_slot`.
- No dynamic allocation (stack buffer only for chunk reads).
- Correct handling of the tie-breaking case (seq[0] == seq[1]).

Red flags:
- Reading the full 32 KB slot into a stack or global buffer.
- Using only `data_len` bytes for CRC scope instead of the full CS_CRC_COVERAGE.
- Selecting the slot with the LOWER seq_number.
- Ignoring `data_len > max_len` and reading beyond the output buffer.
- Not checking the return value of cs_flash_read.
