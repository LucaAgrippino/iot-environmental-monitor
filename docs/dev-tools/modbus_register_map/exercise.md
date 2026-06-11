# Technical Exercise — ModbusRegisterMap

## Brief (3 minutes)

You are implementing a two-phase write dispatcher for a Modbus FC16
(Write Multiple Registers) handler. The dispatcher must:

1. **Phase 1 — pre-validate all registers** without applying any changes.
   If any register is unknown, read-only, or its value is out of range,
   return an exception immediately with no side effects.
2. **Phase 2 — apply all writes** only if Phase 1 passed for every register.

This two-phase approach provides atomicity: a partial write (some registers
updated, some not) must never happen.

---

## Given types

```c
typedef enum {
    MB_EXC_NONE               = 0x00,
    MB_EXC_ILLEGAL_DATA_ADDR  = 0x02,
    MB_EXC_ILLEGAL_DATA_VALUE = 0x03,
} modbus_exception_t;

typedef modbus_exception_t (*validate_fn_t)(uint16_t addr, uint16_t value);
typedef modbus_exception_t (*write_fn_t)(uint16_t addr, uint16_t value);

typedef struct {
    uint16_t     addr;
    validate_fn_t validate; /* NULL if no range check */
    write_fn_t    write;    /* NULL if register is read-only */
} reg_slot_t;

/* Returns the slot for the given address, or NULL if not found. */
const reg_slot_t *find_slot(uint16_t addr);

#define MAX_REGS_PER_WRITE  (123U)
```

---

## Task

Implement `dispatch_fc16()`:

```c
/**
 * @brief Dispatch an FC16 (Write Multiple Registers) request.
 *
 * @param start_addr  First register address.
 * @param count       Number of registers to write (1..MAX_REGS_PER_WRITE).
 * @param values      Array of `count` register values (host byte order).
 * @return MB_EXC_NONE on success; MB_EXC_ILLEGAL_DATA_ADDR or
 *         MB_EXC_ILLEGAL_DATA_VALUE on failure.
 */
modbus_exception_t dispatch_fc16(uint16_t        start_addr,
                                  uint16_t        count,
                                  const uint16_t *values)
{
    /* TODO: implement two-phase write */
}
```

---

## Questions

**Q1:** Why must Phase 1 not call `write_fn` even for the registers that
pass validation? Give a concrete scenario where skipping Phase 1 and writing
greedily (stop on first error) would leave the system in a bad state.

**Answer:** Greedy write creates a partially-applied configuration. Suppose
a master sends FC16 to update three alarm thresholds atomically: temperature
low, humidity high, and pressure high. If temperature low passes validation
and is written, but humidity high is then rejected for being out of range, the
device now has an inconsistent alarm configuration — temperature alarm is
updated but humidity/pressure alarms are not. An operator reading back the
registers would see a mix of old and new values. With two-phase writes the
entire transaction either succeeds completely or fails completely, matching the
master's intent.

**Q2:** Phase 2 calls `find_slot()` again for each register. Is this a
problem? Could it be eliminated? What is the trade-off?

**Answer:** Calling `find_slot()` twice per register doubles the slot-table
traversal work. For a table of 39 entries with a linear scan this is
negligible (~78 comparisons total for a count=1 write). Eliminating the
second lookup would require storing the slot pointers from Phase 1 in a
temporary array, which adds stack usage proportional to `count` (up to 123
entries × pointer size). For a resource-constrained embedded system, the
extra 492 bytes of stack (123 × 4 on a 32-bit system) may be unacceptable.
The repeated linear scan is the correct trade-off here: the slot table is
small, the scan is O(n) in table size (not count), and the write path is
not in the fast path for this device.

**Q3:** The `count == 0` case is rejected before the two-phase loop.
Explain why, and describe what would happen if it were allowed through.

**Answer:** A count of zero means no registers are written. Phase 1 would
loop zero times (trivially passing), Phase 2 would loop zero times (no
writes). The function would return `MB_EXC_NONE` — reporting success for
a request that did nothing. The Modbus specification (§6.12) requires a
minimum quantity of 1 for FC16; returning success for count=0 would silently
accept a malformed PDU, misleading the master into thinking its (empty) write
was applied. Rejecting it with `MB_EXC_ILLEGAL_DATA_VALUE` matches the
protocol requirement.

---

## Model solution

```c
modbus_exception_t dispatch_fc16(uint16_t        start_addr,
                                  uint16_t        count,
                                  const uint16_t *values)
{
    if (count == 0u || count > MAX_REGS_PER_WRITE)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }

    /* Phase 1: validate all — no side effects */
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t          addr = (uint16_t)(start_addr + i);
        const reg_slot_t *slot = find_slot(addr);

        if (slot == NULL || slot->write == NULL)
        {
            return MB_EXC_ILLEGAL_DATA_ADDR;
        }
        if (slot->validate != NULL)
        {
            modbus_exception_t rc = slot->validate(addr, values[i]);
            if (rc != MB_EXC_NONE)
            {
                return rc;
            }
        }
    }

    /* Phase 2: apply all */
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t          addr = (uint16_t)(start_addr + i);
        const reg_slot_t *slot = find_slot(addr);
        modbus_exception_t rc  = slot->write(addr, values[i]);
        if (rc != MB_EXC_NONE)
        {
            return rc; /* unexpected — Phase 1 should have caught this */
        }
    }

    return MB_EXC_NONE;
}
```

---

## Marking guide

**Must have:**
- Phase 1 loop validates all registers before any write
- Phase 2 loop only runs if Phase 1 completes without error
- `slot == NULL || slot->write == NULL` → `MB_EXC_ILLEGAL_DATA_ADDR`
- `slot->validate != NULL` guard before calling validate
- `count == 0 || count > MAX_REGS_PER_WRITE` → `MB_EXC_ILLEGAL_DATA_VALUE`
- Phase 2 calls `find_slot()` again (does not cache pointers from Phase 1)

**Good to have:**
- Explicit `(uint16_t)(start_addr + i)` cast to suppress implicit-conversion warning
- Comment marking Phase 2 failure as "unexpected" (should never happen if
  Phase 1 is correct and write_fn is consistent with validate_fn)

**Red flags:**
- Writing some registers in Phase 1 (greedy approach) — fails the atomicity requirement
- Caching `slot` across phases in a VLA (stack blowout at count=123)
- Skipping the NULL guard on `slot->validate` (segfault on registers with no range check)
- Returning `MB_EXC_NONE` for `count == 0` — silent no-op masks malformed PDU
