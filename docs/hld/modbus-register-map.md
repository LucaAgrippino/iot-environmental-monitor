# HLD Artefact #7 — Modbus Register Map

**Companion document to `hld.md`.** This artefact defines the
field-device-to-gateway Modbus RTU protocol at the register level. It is
the contract between the two boards and the source of truth for any
third-party Modbus master that needs to read or write this device.

---

## 1. Purpose and scope

This document specifies the Modbus RTU register-level interface exposed
by the Field Device. It defines the address layout, the data types and
encoding conventions, the supported function codes, the meaning of every
register, the access rules, and the exception responses. The Gateway's
`ModbusPoller` and the Field Device's `ModbusRegisterMap` both
conform to this specification — together they implement the master/slave
exchange illustrated in SD-02 (Modbus polling cycle).

This document does **not** cover wire-level frame layout (CRC, byte
stuffing, inter-frame silence) — those concerns are part of Modbus RTU
itself and are handled by the `ModbusSlave` middleware on the Field
Device and `ModbusMaster` on the Gateway. Nor does it cover task-level
sequencing or IPC — those concerns belong to HLD Artefact #6 (Task
Breakdown).

---

## 2. Protocol parameters

Sourced from SRS §2.5 Modbus Communication.

| Parameter | Value | Reference |
|---|---|---|
| Physical layer | RS-485 half-duplex | `vision.md` |
| Baud rate | 9600 bps | REQ-MB-010 |
| Data bits | 8 | REQ-MB-010 |
| Parity | None | REQ-MB-010 |
| Stop bits | 1 | REQ-MB-010 |
| Slave address range | 1..247 (Modbus RTU standard) | — |
| Default slave address | 1 *(configurable via provisioning, UC-16)* | — |
| Response timeout (master-side) | 200 ms | REQ-MB-050 |
| Retry count (master-side) | 3 | REQ-MB-060 |
| Inter-frame silence | 3.5 character times (~4 ms at 9600 8N1) | Modbus RTU spec |

---

## 3. Function code support

Only the four function codes below are implemented. Any other received
function code returns exception **0x01 — Illegal Function**.

| FC | Hex | Name | Direction | Used for |
|---|---|---|---|---|
| 3 | 0x03 | Read Holding Registers | GW → FD | Read configuration and command-state registers |
| 4 | 0x04 | Read Input Registers | GW → FD | Read sensor data, device state, metrics |
| 6 | 0x06 | Write Single Register | GW → FD | Write one configuration register or trigger a command |
| 16 | 0x10 | Write Multiple Registers | GW → FD | Write a contiguous configuration block atomically |

Discrete-input and coil access (FC01, FC02, FC05, FC15) is intentionally
not supported. All bit-level information (alarm flags, command results)
is packed into 16-bit registers, keeping the slave to a single access
pattern.

---

## 4. Address-space layout

All addresses below are **0-based on-the-wire register addresses** (the
modern convention; the legacy 30001/40001 notation is not used in this
document).

Gaps between categories are intentional. They reserve room for future
additions without renumbering — a backward-compatible change.

| Range | Category | Function codes | Access |
|---|---|---|---|
| 0x0000 – 0x000F | Identity and version (16 regs) | FC04 | R |
| 0x0010 – 0x002F | Sensor readings (32 regs) | FC04 | R |
| 0x0030 – 0x004F | Device state and metrics (32 regs) | FC04 | R |
| 0x0050 – 0x00FF | *(reserved for future input registers)* | — | — |
| 0x0100 – 0x01FF | Configuration (256 regs) | FC03 / FC06 / FC16 | RW |
| 0x0200 – 0x02FF | Commands and control (256 regs) | FC03 / FC06 / FC16 | RW |
| 0x0300 – 0xFFFF | *(reserved)* | — | — |

Read attempts on reserved addresses return exception **0x02 — Illegal
Data Address**. Write attempts on input-register ranges or reserved
ranges return the same.

---

## 5. Encoding conventions

### 5.1 Endianness

- **Byte order within a register:** big-endian (MSB first). This is the
  Modbus RTU standard.
- **Word order for 32-bit values across two registers:** big-endian —
  the high-order 16 bits occupy the *lower* address. Often called "ABCD"
  order in vendor documentation.

This convention applies uniformly to every multi-register value defined
in this map. The convention is recorded once here; individual register
entries do not repeat it.

### 5.2 Data types

| Type | Width | Notes |
|---|---|---|
| `uint16` | 1 register | Unsigned 0..65535 |
| `int16` | 1 register | Two's complement signed −32768..32767 |
| `uint32` | 2 registers | Big-endian word order (high word at lower address) |
| `bitfield16` | 1 register | Bit assignments documented per register |
| `enum16` | 1 register | uint16 with constrained values; documented per register |

### 5.3 Scaling convention

Floating-point physical quantities are stored as **scaled integers**
rather than IEEE-754 floats. Every scaled register documents its scale
factor and unit explicitly in §6.

Rationale: scaled integers eliminate NaN/Inf, halve the bandwidth
compared to float32 (which requires two registers), and produce
predictable decoders on any master implementation. The trade-off — fixed
precision — is acceptable given the sensor resolution available.

Example: a temperature reading of 23.50 °C is stored as `2350` in an
`int16` register with scale factor `×0.01`.

### 5.4 Reserved sentinel values

The following values indicate "value unavailable" (e.g., sensor I/O
error). Masters must check for these sentinels before using the value.

| Type | Sentinel | Meaning |
|---|---|---|
| `int16` | `0x8000` (−32768) | Read failed, sensor I/O error |
| `uint16` | `0xFFFF` | Read failed, sensor I/O error |
| `uint32` | `0xFFFFFFFF` | Read failed, sensor I/O error |

Configuration registers never return sentinels — a read always returns
the persisted value.

---

## 6. Register definitions

### 6.1 Identity and version *(0x0000 – 0x000F, Input, FC04, R)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0000 | MAP_VERSION | uint16 | — | — | 1..65535 | 1 | Register map version. Gateway verifies during link establishment (SD-00b); mismatch outside supported range is fatal *(D14, D17)*. |
| 0x0001 | DEVICE_ID_HI | uint16 | — | — | — | hw-fused | High word of device identifier. Concatenated with 0x0002 forms a uint32 unique device ID. Used by `DeviceProfileRegistry` for per-slave profile validation *(REQ-MB-120)*. |
| 0x0002 | DEVICE_ID_LO | uint16 | — | — | — | hw-fused | Low word of device identifier. |
| 0x0003 | HARDWARE_REV | uint16 | — | — | 0..65535 | 1 | Hardware revision code (encoded by vendor convention). |
| 0x0004 | FW_VERSION_MAJOR | uint16 | — | — | 0..255 | 0 | Firmware major version (semver MAJOR). |
| 0x0005 | FW_VERSION_MINOR | uint16 | — | — | 0..255 | 1 | Firmware minor version (semver MINOR). |
| 0x0006 | FW_VERSION_PATCH | uint16 | — | — | 0..255 | 0 | Firmware patch version (semver PATCH). |
| 0x0007 | VENDOR_CODE | uint16 | — | — | — | 0x1A45 | Vendor identifier (project-assigned). |
| 0x0008 – 0x000F | *(reserved)* | — | — | — | — | — | Returns 0 on read; future identity fields. |

### 6.2 Sensor readings *(0x0010 – 0x002F, Input, FC04, R)*

All readings reflect the most recent successful sensor sample. On sensor
I/O failure the corresponding register returns its type's sentinel
value (§5.4).

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0010 | TEMPERATURE | int16 | ×0.01 | °C | −4000..8500 | sentinel | Ambient temperature from `HumidityTempDriver`. |
| 0x0011 | HUMIDITY | uint16 | ×0.01 | %RH | 0..10000 | sentinel | Relative humidity from `HumidityTempDriver`. |
| 0x0012 | PRESSURE | uint16 | ×0.1 | hPa | 3000..11000 | sentinel | Atmospheric pressure from `BarometerDriver`. |
| 0x0013 – 0x002F | *(reserved)* | — | — | — | — | — | Future sensors (air quality, light, etc.). |

### 6.3 Device state and metrics *(0x0030 – 0x004F, Input, FC04, R)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0030 | DEVICE_STATE | enum16 | — | — | 0..4 | 0 | Field Device lifecycle state. `0`=Init, `1`=Operational, `2`=EditingConfig, `3`=Error, `4`=Shutdown. Source: `LifecycleController` (see `state-machines.md`). |
| 0x0031 | ALARM_FLAGS | bitfield16 | — | — | — | 0 | Active alarms, one bit per condition (§6.3.1). |
| 0x0032 | UPTIME_SECONDS_HI | uint16 | — | — | — | 0 | High word of seconds since boot (uint32). |
| 0x0033 | UPTIME_SECONDS_LO | uint16 | — | — | — | 0 | Low word of seconds since boot. |
| 0x0034 | MODBUS_RX_OK_COUNT_HI | uint16 | — | — | — | 0 | High word of frames received and processed successfully. Source: `IModbusSlaveStats`. |
| 0x0035 | MODBUS_RX_OK_COUNT_LO | uint16 | — | — | — | 0 | Low word. |
| 0x0036 | MODBUS_CRC_ERR_COUNT_HI | uint16 | — | — | — | 0 | High word of frames discarded due to CRC failure. |
| 0x0037 | MODBUS_CRC_ERR_COUNT_LO | uint16 | — | — | — | 0 | Low word. |
| 0x0038 | MODBUS_TIMEOUT_COUNT_HI | uint16 | — | — | — | 0 | High word of frame-completion timeouts (inter-frame silence violations). |
| 0x0039 | MODBUS_TIMEOUT_COUNT_LO | uint16 | — | — | — | 0 | Low word. |
| 0x003A | SENSOR_READ_ERR_COUNT_HI | uint16 | — | — | — | 0 | High word of failed sensor reads since boot. |
| 0x003B | SENSOR_READ_ERR_COUNT_LO | uint16 | — | — | — | 0 | Low word. |
| 0x003C – 0x004F | *(reserved)* | — | — | — | — | — | Future metrics. |

#### 6.3.1 ALARM_FLAGS bit assignments

Each bit is set while the corresponding alarm condition is active and
cleared when the condition no longer holds **or** when `CMD_ACK_ALARM`
is written.

| Bit | Name | Meaning |
|---|---|---|
| 0 | TEMP_LOW | Temperature below `TEMP_ALARM_LOW` threshold |
| 1 | TEMP_HIGH | Temperature above `TEMP_ALARM_HIGH` threshold |
| 2 | HUMIDITY_LOW | Humidity below `HUMIDITY_ALARM_LOW` |
| 3 | HUMIDITY_HIGH | Humidity above `HUMIDITY_ALARM_HIGH` |
| 4 | PRESSURE_LOW | Pressure below `PRESSURE_ALARM_LOW` |
| 5 | PRESSURE_HIGH | Pressure above `PRESSURE_ALARM_HIGH` |
| 6 | SENSOR_FAULT | One or more sensors failing to respond |
| 7 – 15 | *(reserved)* | Must read as 0 |

### 6.4 Configuration *(0x0100 – 0x01FF, Holding, FC03 / FC06 / FC16, RW)*

Configuration registers are persisted to non-volatile storage by
`ConfigService` via `ConfigStore` (REQ-DM-090). Defaults shown are
loaded when no persisted value exists. Writes outside the documented
range return exception **0x03 — Illegal Data Value**.

#### Temperature thresholds *(0x0100 – 0x010F)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0100 | TEMP_ALARM_LOW | int16 | ×0.01 | °C | −4000..8500 | −2000 (−20.00°C) | Low-temperature alarm threshold. |
| 0x0101 | TEMP_ALARM_HIGH | int16 | ×0.01 | °C | −4000..8500 | 6000 (+60.00°C) | High-temperature alarm threshold. |
| 0x0102 | TEMP_HYSTERESIS | uint16 | ×0.01 | °C | 0..1000 | 50 (0.50°C) | Hysteresis applied on alarm clear to suppress chatter. |
| 0x0103 – 0x010F | *(reserved)* | — | — | — | — | — | Future temperature config. |

#### Humidity thresholds *(0x0110 – 0x011F)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0110 | HUMIDITY_ALARM_LOW | uint16 | ×0.01 | %RH | 0..10000 | 2000 (20.00%) | Low-humidity alarm threshold. |
| 0x0111 | HUMIDITY_ALARM_HIGH | uint16 | ×0.01 | %RH | 0..10000 | 8000 (80.00%) | High-humidity alarm threshold. |
| 0x0112 | HUMIDITY_HYSTERESIS | uint16 | ×0.01 | %RH | 0..1000 | 100 (1.00%) | Hysteresis on alarm clear. |
| 0x0113 – 0x011F | *(reserved)* | — | — | — | — | — | Future humidity config. |

#### Pressure thresholds *(0x0120 – 0x012F)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0120 | PRESSURE_ALARM_LOW | uint16 | ×0.1 | hPa | 3000..11000 | 9500 (950.0 hPa) | Low-pressure alarm threshold. |
| 0x0121 | PRESSURE_ALARM_HIGH | uint16 | ×0.1 | hPa | 3000..11000 | 10500 (1050.0 hPa) | High-pressure alarm threshold. |
| 0x0122 | PRESSURE_HYSTERESIS | uint16 | ×0.1 | hPa | 0..100 | 10 (1.0 hPa) | Hysteresis on alarm clear. |
| 0x0123 – 0x012F | *(reserved)* | — | — | — | — | — | Future pressure config. |

#### Acquisition parameters *(0x0130 – 0x013F)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0130 | SAMPLING_PERIOD_MS | uint16 | — | ms | 100..60000 | 100 | Sensor sampling period. REQ-SA-070. |
| 0x0131 – 0x013F | *(reserved)* | — | — | — | — | — | Future acquisition config. |

#### LCD parameters *(0x0140 – 0x014F)*

| Addr | Name | Type | Scale | Unit | Range | Default | Description |
|---|---|---|---|---|---|---|---|
| 0x0140 | LCD_BRIGHTNESS_PCT | uint16 | — | % | 0..100 | 80 | Backlight brightness percentage. |
| 0x0141 | LCD_TIMEOUT_S | uint16 | — | s | 0..3600 | 0 | Auto-dim timeout; `0` disables. |
| 0x0142 – 0x014F | *(reserved)* | — | — | — | — | — | Future LCD config. |

#### Reserved configuration ranges *(0x0150 – 0x01FF)*

176 registers reserved for future configuration. Read returns 0; write
returns exception **0x02 — Illegal Data Address**.

### 6.5 Commands and control *(0x0200 – 0x02FF, Holding, FC03 / FC06 / FC16)*

Writes to these registers trigger actions. Reads return the last-written
value (or 0 if never written), which is useful for confirming a command
was received. Destructive commands require a "magic" trigger value to
prevent accidental triggers from incorrect writes.

| Addr | Name | Type | Trigger value | Effect |
|---|---|---|---|---|
| 0x0200 | CMD_ACK_ALARM | uint16 | `0x0001` | Acknowledges all currently active alarms; clears `ALARM_FLAGS` (REQ-AM-040). |
| 0x0201 | CMD_RESET_METRICS | uint16 | `0x0001` | Resets all Modbus and sensor metrics counters to 0. Does not affect uptime. |
| 0x0202 | CMD_SOFT_RESTART | uint16 | `0xA5A5` | Triggers soft restart of the Field Device (UC-17). Any other value is rejected with exception **0x03**. Magic value prevents accidental triggers. |
| 0x0203 – 0x02FF | *(reserved)* | — | — | Future commands. |

---

## 7. Exception responses

The Field Device implements the minimum Modbus exception set required for
correctness. Other Modbus exception codes (0x05 Acknowledge, 0x06 Slave
Busy, 0x07 NAK, 0x08 Memory Parity Error, 0x0A Gateway Path Unavailable,
0x0B Gateway Target No Response) are not used.

| Code | Hex | Name | Returned when |
|---|---|---|---|
| 1 | 0x01 | Illegal Function | An unsupported FC is received (anything other than 3, 4, 6, 16). |
| 2 | 0x02 | Illegal Data Address | The requested register address is in a reserved range, or a write was attempted on a read-only register (input register or reserved holding). |
| 3 | 0x03 | Illegal Data Value | A write value is outside the register's documented valid range, or the magic value mismatched on a destructive command. |
| 4 | 0x04 | Slave Device Failure | An underlying read or write operation failed (sensor I/O error during register read, flash write fault during config persistence). |

Validation failures (D14, REQ-MB-130) are logged by the slave and the
exception is returned to the master. The master's behaviour on each
exception is defined in `sequence-diagrams.md` SD-02.

---

## 8. Versioning and compatibility

The `MAP_VERSION` register (0x0000) provides a single compatibility
signal that the master can check during link establishment.

**Bump MAP_VERSION when:**
- A register is removed
- A register's address changes
- A register's type, scale, or unit changes
- A register's access mode changes (R → W or RW → R)
- An exception response semantic changes
- A reserved bit in a `bitfield16` is repurposed with non-zero meaning

**Do not bump MAP_VERSION when:**
- A new register is added in a reserved range
- A new bit is added to a `bitfield16` that previously read as 0
- A new command value is added to an existing command register
- A new exception code is documented (but only standard Modbus codes are used)

The Gateway reads `MAP_VERSION` during link establishment (SD-00b) and
binds the corresponding device profile from `DeviceProfileRegistry`
(D14, D17, D18). Slaves reporting a version outside the Gateway's
supported set are rejected from the polling allowlist
(REQ-MB-120, REQ-MB-130).

---

## 9. Change log

| Version | Date | Author | Change |
|---|---|---|---|
| 1 | May 2026 | Luca Agrippino | Initial release. |

---

## 10. Test plan

The register map is verified end-to-end using an off-the-shelf Modbus
master tool. Recommended tools: `modpoll` (CLI, scriptable) or
QModMaster (GUI, interactive). The test plan below is for manual
verification during integration; automation is deferred to the test
harness phase.

### 10.1 Read tests

- **TC-RM-001** — Read `MAP_VERSION` with FC04 at address 0x0000.
  Expected value: 1.
- **TC-RM-002** — Read sensor block (0x0010..0x0012) with FC04. Verify
  values are sensible and within range. Cross-check against LCD display.
- **TC-RM-003** — Read full identity block (0x0000..0x0007) with FC04
  in a single transaction. Verify values match firmware build.
- **TC-RM-004** — Read state and metrics block (0x0030..0x003B) with
  FC04. Verify `DEVICE_STATE` equals 1 (Operational) and uptime is
  increasing across consecutive reads.

### 10.2 Write tests

- **TC-RM-010** — Write `TEMP_ALARM_LOW` to a valid value with FC06.
  Read it back with FC03. Verify it matches. Power-cycle the FD. Read
  it again. Verify persistence (REQ-DM-090).
- **TC-RM-011** — Write multiple configuration registers atomically
  with FC16 (e.g., 0x0100..0x0102). Read back. Verify all three
  values match.

### 10.3 Exception tests

- **TC-RM-020** — Send FC07 (unsupported). Expect exception **0x01**.
- **TC-RM-021** — Read address 0x00A0 (reserved). Expect exception
  **0x02**.
- **TC-RM-022** — Write to address 0x0010 (read-only sensor register).
  Expect exception **0x02**.
- **TC-RM-023** — Write `TEMP_ALARM_LOW` with value `0x9000` (below
  range). Expect exception **0x03**.
- **TC-RM-024** — Write `CMD_SOFT_RESTART` with value `0x0001` (wrong
  magic). Expect exception **0x03**. Field Device must not restart.
- **TC-RM-025** — Disconnect a sensor and read its register. Expect
  the type's sentinel value (`0x8000` for `int16`, `0xFFFF` for
  `uint16`).

### 10.4 Command tests

- **TC-RM-030** — Write `CMD_SOFT_RESTART` with correct magic
  (`0xA5A5`). Verify FD restarts within REQ-MB-050 budget plus boot
  time.
- **TC-RM-031** — Trigger alarm conditions (e.g., by adjusting
  thresholds to current readings). Verify `ALARM_FLAGS` bits set.
  Write `CMD_ACK_ALARM` with `0x0001`. Verify `ALARM_FLAGS` clears.
- **TC-RM-032** — Write `CMD_RESET_METRICS`. Verify metrics counters
  go to 0 but `UPTIME_SECONDS` continues counting.

---

*This document is HLD Artefact #7. It is updated whenever the protocol
contract between the Field Device and Gateway changes. Backward-
incompatible changes require a `MAP_VERSION` bump per §8.*
