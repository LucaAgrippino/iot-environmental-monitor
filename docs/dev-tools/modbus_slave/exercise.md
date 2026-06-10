# Technical Exercise — ModbusSlave

## Brief (3 minutes)

You are implementing the Modbus RTU frame dispatcher for an embedded slave
device. The dispatcher receives a validated frame (address matched, CRC
verified) and must decode the Function Code (FC) field, call the appropriate
register map callback, and build a correct response or exception response.

Your task is to implement the FC dispatcher for FC03 (Read Holding Registers)
and FC06 (Write Single Register) only.

---

## Given files

### modbus_dispatch_exercise.h

```c
#ifndef MODBUS_DISPATCH_EXERCISE_H
#define MODBUS_DISPATCH_EXERCISE_H

#include <stdint.h>

#define MODBUS_MAX_FRAME  (256U)

/* Function codes. */
#define FC_READ_HOLDING   (0x03U)
#define FC_WRITE_SINGLE   (0x06U)

/* Exception codes. */
#define EXC_ILLEGAL_FC    (0x01U)
#define EXC_ILLEGAL_ADDR  (0x02U)
#define EXC_ILLEGAL_VALUE (0x03U)
#define EXC_DEVICE_FAIL   (0x04U)

typedef enum {
    MAP_OK           = 0,
    MAP_INVALID_ADDR = 1,
    MAP_INVALID_VAL  = 2,
    MAP_DEVICE_FAIL  = 3,
} map_err_t;

typedef map_err_t (*fn_read_holding_t)(uint16_t addr, uint16_t *out);
typedef map_err_t (*fn_write_holding_t)(uint16_t addr, uint16_t value);

typedef struct {
    fn_read_holding_t  read_holding;
    fn_write_holding_t write_holding;
} reg_map_t;

typedef struct {
    uint8_t  slave_addr;
    uint8_t  rx_buf[MODBUS_MAX_FRAME];
    uint16_t rx_len;
    uint8_t  tx_buf[MODBUS_MAX_FRAME];
    uint16_t tx_len;
    const reg_map_t *reg_map;
} modbus_ctx_t;

/* CRC function provided. */
uint16_t crc16(const uint8_t *buf, uint16_t len);

/* Dispatch the frame in ctx->rx_buf. Populate ctx->tx_buf and ctx->tx_len.
 * The caller guarantees: address matched, CRC verified, rx_len >= 6. */
void modbus_dispatch(modbus_ctx_t *ctx);

#endif /* MODBUS_DISPATCH_EXERCISE_H */
```

### modbus_dispatch_exercise.c (partial)

```c
#include "modbus_dispatch_exercise.h"

static void append_crc(uint8_t *frame, uint16_t len)
{
    uint16_t crc = crc16(frame, len);
    frame[len]      = (uint8_t)(crc & 0x00FFU);
    frame[len + 1U] = (uint8_t)(crc >> 8U);
}

static void build_exception(modbus_ctx_t *ctx, uint8_t fc, uint8_t exc)
{
    ctx->tx_buf[0] = ctx->slave_addr;
    ctx->tx_buf[1] = fc | 0x80U;
    ctx->tx_buf[2] = exc;
    append_crc(ctx->tx_buf, 3U);
    ctx->tx_len = 5U;
}

void modbus_dispatch(modbus_ctx_t *ctx)
{
    uint8_t  fc         = ctx->rx_buf[1];
    uint16_t start_addr = ((uint16_t)ctx->rx_buf[2] << 8U) | ctx->rx_buf[3];
    uint16_t qty        = ((uint16_t)ctx->rx_buf[4] << 8U) | ctx->rx_buf[5];

    switch (fc)
    {
        case FC_READ_HOLDING:
            /* TODO 1: Read `qty` holding registers starting at `start_addr`.
             *         For each, call ctx->reg_map->read_holding(addr, &value).
             *         On MAP_INVALID_ADDR → build exception 0x02 and return.
             *         On MAP_DEVICE_FAIL  → build exception 0x04 and return.
             *         On success: response is:
             *           [addr][0x03][byte_count=qty*2][val0_hi][val0_lo]...[CRC×2]
             */
            break;

        case FC_WRITE_SINGLE:
            /* TODO 2: Write one holding register.
             *         value = (rx_buf[4] << 8) | rx_buf[5]
             *         Call ctx->reg_map->write_holding(start_addr, value).
             *         On MAP_INVALID_ADDR  → exception 0x02.
             *         On MAP_INVALID_VAL   → exception 0x03.
             *         On MAP_DEVICE_FAIL   → exception 0x04.
             *         On success: echo the request as the response (8 bytes):
             *           [addr][0x06][start_hi][start_lo][val_hi][val_lo][CRC×2]
             */
            break;

        default:
            /* TODO 3: Unsupported FC → exception 0x01. */
            break;
    }
}
```

---

## Questions

**Q1:** Modbus RTU specifies that CRC bytes are appended low-byte-first. Why
does this matter in practice, and what would a master observe if the slave
incorrectly appended CRC high-byte-first?

**Answer:** Modbus CRC-16/IBM uses a 16-bit value split into two bytes. The
RTU spec mandates the low byte first, then the high byte. If the slave reverses
the order, the master computes the CRC over the response payload, reconstructs
the expected 16-bit CRC, and compares it byte-by-byte with what it received.
The comparison fails for every response unless the CRC happens to be a palindrome
(e.g. 0x0000 or 0xFFFF). The master would log every response as a CRC error,
treat all replies as invalid, and retry until it times out — the slave would
appear completely non-responsive to the master's CRC logic even though the data
content is correct.

**Q2:** The FC03 response includes a `byte_count` field equal to `qty × 2`.
Why does Modbus define this field when `qty` (the register count) is already
known to the master from its own request?

**Answer:** The byte count serves two purposes. First, it allows a master to
read a variable-length response without having to re-parse the original request
to know how many bytes to receive — it can stream the response directly from the
wire, reading `byte_count + 5` bytes total (addr + FC + byte_count + data + CRC).
Second, it provides a sanity check: if `byte_count` does not equal `qty × 2`
(e.g. due to a partial response), the master can detect the framing error rather
than silently accepting corrupt data.

**Q3:** A Modbus slave must never respond to a broadcast frame (address 0x00).
This rule is enforced in the address-filter step *before* dispatch. Explain why
enforcing it in the dispatcher (adding `if (ctx->slave_addr == 0) return;` at
the top of `modbus_dispatch()`) would be an incorrect design, even if it
produced the same visible behaviour.

**Answer:** The dispatcher's contract is: "the caller guarantees address was
matched." Putting broadcast detection inside the dispatcher would introduce a
hidden dependency on the caller doing the address check correctly — if the check
were removed or bypassed, the dispatcher's guard would not catch it for unicast
frames. It also violates single-responsibility: the dispatcher should not need
to know about protocol-level addressing rules. The correct place is the
address-filter step in `modbus_slave_process()`, where the full addressing logic
(unicast match, broadcast drop) is handled once for all FCs.

---

## Model solution

```c
void modbus_dispatch(modbus_ctx_t *ctx)
{
    uint8_t  fc         = ctx->rx_buf[1];
    uint16_t start_addr = ((uint16_t)ctx->rx_buf[2] << 8U) | ctx->rx_buf[3];
    uint16_t qty        = ((uint16_t)ctx->rx_buf[4] << 8U) | ctx->rx_buf[5];

    switch (fc)
    {
        case FC_READ_HOLDING:
        {
            uint8_t byte_count = (uint8_t)(qty * 2U);
            ctx->tx_buf[0] = ctx->slave_addr;
            ctx->tx_buf[1] = FC_READ_HOLDING;
            ctx->tx_buf[2] = byte_count;

            for (uint16_t i = 0U; i < qty; i++)
            {
                uint16_t  value = 0U;
                map_err_t err   = ctx->reg_map->read_holding(start_addr + i,
                                                             &value);
                if (err == MAP_INVALID_ADDR)
                {
                    build_exception(ctx, FC_READ_HOLDING, EXC_ILLEGAL_ADDR);
                    return;
                }
                if (err == MAP_DEVICE_FAIL)
                {
                    build_exception(ctx, FC_READ_HOLDING, EXC_DEVICE_FAIL);
                    return;
                }
                ctx->tx_buf[3U + i * 2U]      = (uint8_t)(value >> 8U);
                ctx->tx_buf[3U + i * 2U + 1U] = (uint8_t)(value & 0x00FFU);
            }

            uint16_t pdu_len = 3U + (uint16_t)byte_count;
            append_crc(ctx->tx_buf, pdu_len);
            ctx->tx_len = pdu_len + 2U;
            break;
        }

        case FC_WRITE_SINGLE:
        {
            uint16_t  value = ((uint16_t)ctx->rx_buf[4] << 8U) | ctx->rx_buf[5];
            map_err_t err   = ctx->reg_map->write_holding(start_addr, value);

            if (err == MAP_INVALID_ADDR)
            {
                build_exception(ctx, FC_WRITE_SINGLE, EXC_ILLEGAL_ADDR);
                return;
            }
            if (err == MAP_INVALID_VAL)
            {
                build_exception(ctx, FC_WRITE_SINGLE, EXC_ILLEGAL_VALUE);
                return;
            }
            if (err == MAP_DEVICE_FAIL)
            {
                build_exception(ctx, FC_WRITE_SINGLE, EXC_DEVICE_FAIL);
                return;
            }

            /* Echo request as ACK. */
            ctx->tx_buf[0] = ctx->slave_addr;
            ctx->tx_buf[1] = FC_WRITE_SINGLE;
            ctx->tx_buf[2] = (uint8_t)(start_addr >> 8U);
            ctx->tx_buf[3] = (uint8_t)(start_addr & 0x00FFU);
            ctx->tx_buf[4] = (uint8_t)(value >> 8U);
            ctx->tx_buf[5] = (uint8_t)(value & 0x00FFU);
            append_crc(ctx->tx_buf, 6U);
            ctx->tx_len = 8U;
            break;
        }

        default:
            build_exception(ctx, fc, EXC_ILLEGAL_FC);
            break;
    }
}
```

---

## Marking guide

**Must have:**
- FC03: `byte_count = qty * 2` in response byte [2], not `qty`
- FC03: register value stored big-endian (high byte first at index `3 + i*2`)
- FC03: CRC appended over `3 + byte_count` bytes, not the full `tx_buf`
- FC06: echo `start_addr` and `value` back, not re-read from register map
- Both FCs: correct exception code mapping (INVALID_ADDR→0x02, INVALID_VAL→0x03, FAIL→0x04)
- Default: exception 0x01 for unsupported FC

**Good to have:**
- Early return from FC03 loop on first error (avoids reading further registers)
- `uint16_t pdu_len` kept as a separate variable for clarity
- `(uint8_t)(value >> 8U)` — explicit cast suppresses implicit-narrowing warning

**Red flags:**
- `byte_count = qty` — off by factor of 2; every master will reject the response
- CRC appended over `tx_len` before `tx_len` is set — undefined behaviour
- Exception FC byte as `fc | 0x80` computed correctly but exception code value
  of 0x02 returned for an invalid *value* (should be 0x03)
- Writing `tx_buf[3 + i*2]` = low byte first — big-endian register convention
  violated; master will read values byte-swapped
