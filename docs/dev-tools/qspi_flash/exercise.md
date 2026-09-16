# Technical Exercise — QspiFlashDriver (Gateway)

## Brief (3 minutes)

An external NOR flash device is programmed a **page** at a time. On the
MX25R6435F a page is 256 bytes, and a single Page Program command may not
cross a page boundary: if it does, the address wraps to the start of the
same page and silently overwrites the bytes already written there. NOR flash
also only ever changes bits from 1 to 0 — restoring a bit to 1 requires
erasing the whole 4 KB sector it lives in.

Implement the page-boundary guard for a QSPI flash driver. Given an address,
a buffer and a length, decide whether the write is legal and reject it
otherwise. The callers are middleware components that think in partition
offsets, not in pages, so the driver is the last line of defence: a wrong
answer here is silent data corruption discovered days later, not a crash.

## Given files

### `qspi_flash_exercise.h`

```c
#ifndef QSPI_FLASH_EXERCISE_H
#define QSPI_FLASH_EXERCISE_H

#include <stdint.h>

/** Flash capacity in bytes (8 MB). */
#define QSPI_FLASH_DEVICE_SIZE_BYTES (8UL * 1024UL * 1024UL)

/** Page-program granularity: one write may not cross this boundary. */
#define QSPI_FLASH_PAGE_SIZE_BYTES (256U)

typedef enum
{
    QSPI_FLASH_OK = 0,
    QSPI_FLASH_ERR_ADDR = 3,        /**< addr, or addr + len, exceeds capacity. */
    QSPI_FLASH_ERR_LEN = 4,         /**< len == 0, len > 256, or crosses a page. */
    QSPI_FLASH_ERR_NULL_POINTER = 7 /**< Required pointer argument was NULL. */
} qspi_flash_err_t;

/**
 * @brief Validate the arguments of a page-program request.
 *
 * Rejects, in this order: NULL data; len == 0 or len > 256; a request that
 * runs past the end of the device; a request whose first and last bytes do
 * not lie in the same 256-byte page.
 *
 * @param[in] addr  Byte address of the first byte to program.
 * @param[in] data  Buffer to program from.
 * @param[in] len   Number of bytes (1..256).
 * @return QSPI_FLASH_OK if the request is legal, otherwise the error code
 *         for the first rule violated.
 */
qspi_flash_err_t qspi_flash_validate_write(uint32_t addr,
                                           const uint8_t *data,
                                           uint16_t len);

#endif /* QSPI_FLASH_EXERCISE_H */
```

### `qspi_flash_exercise.c` (partial)

```c
#include "qspi_flash_exercise.h"

qspi_flash_err_t qspi_flash_validate_write(uint32_t addr,
                                           const uint8_t *data,
                                           uint16_t len)
{
    /* TODO: NULL guard. */

    /* TODO: length guard — reject 0 and anything above one page. */

    /* TODO: capacity guard. Careful: addr + len can wrap uint32_t. */

    /* TODO: page-boundary guard. */

    return QSPI_FLASH_OK;
}
```

## Questions

**Q1: Why must the capacity check be written as `len > (DEVICE_SIZE - addr)`
rather than the more natural `addr + len > DEVICE_SIZE`?**

Answer: `addr + len` is computed in 32-bit arithmetic and can wrap. With
`addr = 0xFFFFFFF0` and `len = 0x20`, `addr + len` is `0x10` — a small
number that passes the comparison, so a wildly out-of-range write is
accepted. Subtracting instead keeps both operands within range: `addr` is
already known to be below `DEVICE_SIZE` from the preceding check, so
`DEVICE_SIZE - addr` cannot underflow. This is the standard embedded idiom
for overflow-safe range checks, and it is why the guard order matters — the
`addr >= DEVICE_SIZE` test must come first.

**Q2: A caller asks to write 256 bytes at address `0x100`. Legal or not? What
about 256 bytes at `0x180`?**

Answer: `0x100` is legal. It is page-aligned, so bytes `0x100`–`0x1FF` sit
entirely in one page — a full-page write at a page base is the maximum legal
request. `0x180` is illegal: the last byte would be at `0x27F`, so the write
spans pages 1 and 2. On hardware without the guard the address would wrap to
`0x180`'s own page, and the final 128 bytes would overwrite `0x180`–`0x1FF`
— the data the caller had just written moments earlier.

**Q3: Why compare the page bases of the first and last byte, rather than
checking `(addr % 256) + len <= 256`?**

Answer: They are arithmetically equivalent, but the masked-base comparison
states the hardware rule directly — "first and last byte must live in the
same page" — and avoids a division/modulo on a Cortex-M4, where `%` on a
non-constant compiles to a library call or a multi-cycle divide. Masking with
`~0xFFu` is a single-cycle AND. The intent also survives a change of page
size: redefine the mask and the logic still reads correctly. Note the last
byte is `addr + len - 1`, not `addr + len`; the off-by-one here is the most
common wrong answer, and it wrongly rejects every exactly-full-page write.

## Model solution

```c
#include "qspi_flash_exercise.h"

/** Mask clearing the offset bits, leaving the 256-byte page base. */
#define QSPI_PAGE_BASE_MASK (~((uint32_t) QSPI_FLASH_PAGE_SIZE_BYTES - 1U))

qspi_flash_err_t qspi_flash_validate_write(uint32_t addr,
                                           const uint8_t *data,
                                           uint16_t len)
{
    uint32_t last;

    if (data == NULL)
    {
        return QSPI_FLASH_ERR_NULL_POINTER;
    }

    if ((len == 0U) || (len > QSPI_FLASH_PAGE_SIZE_BYTES))
    {
        return QSPI_FLASH_ERR_LEN;
    }

    /* Overflow-safe: addr is bounded first, so the subtraction is sound. */
    if ((addr >= QSPI_FLASH_DEVICE_SIZE_BYTES) ||
        ((uint32_t) len > (QSPI_FLASH_DEVICE_SIZE_BYTES - addr)))
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    /* First and last byte must share a page base (last = addr + len - 1). */
    last = addr + (uint32_t) len - 1U;
    if ((addr & QSPI_PAGE_BASE_MASK) != (last & QSPI_PAGE_BASE_MASK))
    {
        return QSPI_FLASH_ERR_LEN;
    }

    return QSPI_FLASH_OK;
}
```

## Marking guide

**Must have:**

- NULL guard before any dereference or arithmetic on `data`.
- Rejects `len == 0` as well as `len > 256` — zero-length is the commonly
  forgotten half.
- Page comparison uses `addr + len - 1`, not `addr + len`.
- A full 256-byte write at a page-aligned address is **accepted**.
- Guards ordered so that each one's precondition is established by the
  previous one.

**Good to have:**

- Overflow-safe capacity check (subtraction, not addition), and can explain
  the wrap case unprompted.
- Mask derived from `QSPI_FLASH_PAGE_SIZE_BYTES` rather than a literal `0xFF`
  — no magic numbers, and it survives a page-size change.
- Fixed-width types and explicit casts where `uint16_t` meets `uint32_t`.
- Notes that the driver enforcing this spares every caller from
  reimplementing it (the rationale recorded as QSPID-D3).
- Mentions that a host test cannot prove the guard works on real silicon —
  only a write that actually wraps on the device does.

**Red flags:**

- `addr + len > DEVICE_SIZE` with no acknowledgement of overflow.
- Using `%` or `/` on the hot path without being able to justify the cost.
- Silently clamping `len` to the page boundary instead of returning an error
  — the caller then believes a short write was a complete one, which is the
  corruption this guard exists to prevent.
- Dereferencing `data` before the NULL check.
- Treating the page boundary as identical to the 4 KB sector boundary.
