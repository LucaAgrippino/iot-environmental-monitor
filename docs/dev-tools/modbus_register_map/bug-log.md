# Bug Log — ModbusRegisterMap

## Copy-paste sensor ID in read_pressure() (MRM-BUG-001)

**File:** `firmware/field-device/application/modbus_register_map/modbus_register_map.c`
**Lines:** 382, 387
**Category:** Wrong sensor index (copy-paste error)
**Detected by:** TC-MRM-009 (`test_TC_MRM_009_fc04_pressure_bug_detected`)

---

### What the code does

```c
/* INTENTIONAL BUG (MRM-BUG-001): reads from SENSOR_ID_HUMIDITY instead
 * of SENSOR_ID_PRESSURE. TC-MRM-007 detects this. Fix: replace both
 * SENSOR_ID_HUMIDITY occurrences with SENSOR_ID_PRESSURE.             */
static modbus_exception_t read_pressure(const modbus_register_map_t *s, uint16_t *o)
{
    sensor_snapshot_t snap;
    if (s->sensors->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    if (!snap.readings[SENSOR_ID_HUMIDITY].valid)   /* BUG: should be SENSOR_ID_PRESSURE */
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    *o = (uint16_t)(uint32_t)snap.readings[SENSOR_ID_HUMIDITY].value; /* BUG */
    return MB_EXC_NONE;
}
```

---

### What it should do

Read from `SENSOR_ID_PRESSURE` (index 2) for both the validity check and the
value extraction, because Modbus register 0x0012 is the pressure input register:

```c
static modbus_exception_t read_pressure(const modbus_register_map_t *s, uint16_t *o)
{
    sensor_snapshot_t snap;
    if (s->sensors->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    if (!snap.readings[SENSOR_ID_PRESSURE].valid)   /* FIXED */
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    *o = (uint16_t)(uint32_t)snap.readings[SENSOR_ID_PRESSURE].value; /* FIXED */
    return MB_EXC_NONE;
}
```

---

### How it manifests

A Modbus master reading register 0x0012 (PRESSURE) receives the humidity value
instead. For example, if temperature = 22.00 °C, humidity = 50.00 %RH, and
pressure = 1013.2 hPa:

| Register | Address | Expected | Actual (with bug) |
|----------|---------|----------|-------------------|
| TEMPERATURE | 0x0010 | 2200 (22.00 °C) | 2200 ✓ |
| HUMIDITY    | 0x0011 | 5000 (50.00 %RH) | 5000 ✓ |
| PRESSURE    | 0x0012 | 10132 (1013.2 hPa) | **5000** ✗ |

A connected SCADA system would display constant equal values for humidity and
pressure, likely triggering a spurious pressure alarm since 5000 deci-hPa
(500.0 hPa) is well outside the valid range.

Additionally, if pressure is valid but humidity is invalid (e.g. humidity sensor
failure), the validity check reads the wrong flag and returns sentinel 0xFFFF
for a register whose underlying sensor is actually healthy.

---

### Why it passes compilation

`SENSOR_ID_HUMIDITY` (= 1) and `SENSOR_ID_PRESSURE` (= 2) are both valid
`sensor_id_t` enum values. The array access `snap.readings[1]` is in-bounds.
No compiler warning or static-analysis tool will flag a valid array index as
incorrect — the error is purely semantic (wrong index for the intended register).

---

### Why it is hard to catch without a targeted test

- The humidity and pressure values happen to be different in physical conditions,
  but a Modbus master reading register 0x0012 has no way to know whether it is
  receiving pressure or humidity unless it independently verifies the value range.
- In a lab environment where the sensor is at sea level (~1013 hPa → 10130 in
  fixed-point), the pressure value may visually resemble a valid humidity reading
  if not checked carefully.
- The bug survives a "smoke test" (does init succeed, does the register return
  a non-sentinel value) because humidity is always valid in the default setup.

---

### Correct fix

In `modbus_register_map.c`, in function `read_pressure()`:

```diff
-    if (!snap.readings[SENSOR_ID_HUMIDITY].valid)
+    if (!snap.readings[SENSOR_ID_PRESSURE].valid)
     {
         *o = 0xFFFFu;
         return MB_EXC_NONE;
     }
-    *o = (uint16_t)(uint32_t)snap.readings[SENSOR_ID_HUMIDITY].value;
+    *o = (uint16_t)(uint32_t)snap.readings[SENSOR_ID_PRESSURE].value;
```

After the fix, TC-MRM-009 passes and the full suite is 27/27.
