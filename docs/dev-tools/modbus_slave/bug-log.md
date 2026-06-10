# Bug Log — ModbusSlave

## Missing critical section in set_address() (MBS-BUG-01)

**File:** `firmware/field-device/middleware/modbus_slave/modbus_slave.c`
**Line:** 368
**Category:** Missing synchronisation

**What the code does:**

```c
modbus_slave_err_t modbus_slave_set_address(uint8_t new_addr)
{
    if (!s_slave.initialised) { return MODBUS_SLAVE_ERR_NOT_INIT; }
    if (new_addr < MODBUS_ADDR_MIN || new_addr > MODBUS_ADDR_MAX)
    {
        return MODBUS_SLAVE_ERR_INVALID_ADDR;
    }

    /* DEVIATION (MBS-BUG-01): no critical section */
    s_slave.slave_addr = new_addr;        /* <- bare write */

    return MODBUS_SLAVE_ERR_OK;
}
```

**What it should do:**

Wrap the write in a critical section so that `modbus_slave_process()` —
which reads `s_slave.slave_addr` from `ModbusTask` context — cannot observe
a torn or half-updated value:

```c
    taskENTER_CRITICAL();
    s_slave.slave_addr = new_addr;
    taskEXIT_CRITICAL();
```

**The race condition:**

`set_address()` is called from one task (e.g. a configuration task) while
`modbus_slave_process()` runs in `ModbusTask`. On a Cortex-M4 the write to
`slave_addr` (a `uint8_t`) is effectively atomic, but the read–validate–write
in `set_address()` and the read in `process()` can interleave in any order
relative to the FreeRTOS scheduler. Without a critical section, a frame could
be accepted or rejected based on a stale slave address if `set_address()` is
pre-empted between its range check and its store.

**Why it passes CI:**

`taskENTER_CRITICAL()` and `taskEXIT_CRITICAL()` are no-ops in the host build
(defined as `((void)0)` in `tests/support/FreeRTOS.h`). Unit tests are
single-threaded — `set_address()` and `process()` never run concurrently.

**How to find it with a debugger or instrumentation:**

1. On target, call `modbus_slave_set_address(7)` from a low-priority task
   while `ModbusTask` is processing a stream of frames at maximum rate.
2. Log which slave address each frame was matched against (add a diagnostic
   print inside the address-filter branch of `modbus_slave_process()`).
3. Without the critical section, you may observe frames addressed to `1`
   (old address) accepted after `set_address(7)` has been called, if
   `process()` read `slave_addr` before the store completed.

Alternatively, use a Segger SystemView or ITM trace to instrument the
critical path and verify that no context switch occurs between the
validation and the store.

**Correct fix:**

```c
/* modbus_slave_set_address() — lines 356-371 */
modbus_slave_err_t modbus_slave_set_address(uint8_t new_addr)
{
    if (!s_slave.initialised)
    {
        return MODBUS_SLAVE_ERR_NOT_INIT;
    }
    if (new_addr < MODBUS_ADDR_MIN || new_addr > MODBUS_ADDR_MAX)
    {
        return MODBUS_SLAVE_ERR_INVALID_ADDR;
    }

    taskENTER_CRITICAL();           /* <- add */
    s_slave.slave_addr = new_addr;
    taskEXIT_CRITICAL();            /* <- add */

    return MODBUS_SLAVE_ERR_OK;
}
```
