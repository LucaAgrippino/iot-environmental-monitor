# Two-board integration test (WP7)

**Date prepared:** 2026-10-31 · **Status:** prepared, **not run**. The two boards have never talked to each other.
**Who runs it:** Luca (needs both boards, wiring and a logic analyser). Record the results in the ModbusPoller,
ModbusSlave and TimeService LLDs (section 8).

This document tests four flows end to end: SD-00b (link establishment), SD-02 (polling and the link hysteresis of
REQ-NF-103/104), SD-05 (alarm propagation, REQ-NF-113) and SD-09 (time push). Everything below was checked against
the code on 2026-10-31; where the code and the design disagree, section 2 says so and says what the test will show.

## 1. Read this first: five problems found while preparing

Checking the sources for this test found problems that decide which cases can pass. Two of them need a design
decision from you, and one is a feature that no work package covers. None of them was fixed here.

| # | Problem | Effect on the test | Needs |
|---|---|---|---|
| **B1** | ~~The Field Device never drives its RS-485 direction line.~~ **Closed by Luca 2026-11-01: the RS-485 modules turn the bus around automatically (no DE/RE wiring). No code change on either board; the F469 has no hardware DE mode, and the Gateway's PA15 `RTS_DE` output is simply left unconnected.** The wiring diagram in the hardware guide wrongly showed a GPIO on DE/RE and is corrected | None | Nothing |
| **B2** | ~~A two-register write cannot reach the time handler.~~ **Fixed 2026-11-01 (Luca: follow the register-map LLD, least change).** `IModbusRegisterMap` gained an optional `write_block` entry; `ModbusSlave` FC16 calls it once with every value when it is set (per-register fallback otherwise). The Field Device harness bridge now maps `write_multiple_regs` and returns exception 0x03 for a bad value (it returned 0x02). TC-MBS-024/025 | `CMD_SET_TIME` reaches its handler on the real bus | Nothing |
| **B3** | ~~Nothing reads `ALARM_FLAGS`.~~ **Implemented 2026-11-01 (Luca: the poller had to read it per the register map).** `ModbusPoller` reads input register 0x0031 right after the telemetry read of each cycle and calls `modbus_poller_set_alarm_callback` with (slave, before, after) when it changes; `CloudPublisher` publishes one `FIELD_DEVICE` alarm per changed bit (TEMP/HUMIDITY/PRESSURE x LOW/HIGH, CLEAR when the bit falls) with the measured value from the last telemetry read. `threshold_value_raw` is 0: the limit lives on the Field Device. TC-MP-050/051, CP-T27. Not proven on hardware | SD-05 can run | Hardware run (WP7) |
| **B4** | ~~The poll period alone can exceed 500 ms.~~ **Changed 2026-11-01 (Luca: poll faster).** The default `modbus_poll_period_ms` is now 200 ms (was 1000; range 100..60000 unchanged). Worst case from flag to callback = 200 ms + two transactions (~40 ms at 9600 baud), leaving ~250 ms for publish and PUBACK. REQ-NF-110 still says 1 s: the SRS needs the same edit | REQ-NF-113 is reachable; measure it in 2B-007 | SRS REQ-NF-110 wording |
| **B5** | The device-ID check at link establishment (REQ-MB-120/130) is WP8; the profile is a placeholder | SD-00b's "check the device id" step cannot be observed | WP8 |

## 2. The decisions behind B1 to B4

### 2.1 B1: closed

The modules have automatic direction control (option A). Case 2B-001 is kept as a check: if the dongle sees no reply
from the Field Device alone, the module is not automatic and the wiring needs revisiting.

### 2.2 B2: how does a block reach `CMD_SET_TIME`?

| Option | What it means | Cost |
|---|---|---|
| A. `ModbusSlave` uses the register map's block interface (`imodbus_register_map_t`) | The design the register-map LLD assumes; also gives FC16 its atomic two-phase validation back | A refactor of `ModbusSlave` (code, LLD, tests); the harness bridge disappears |
| B. Add a block-write entry to the old interface and call it for FC16 | Smaller | Still a `ModbusSlave` change and an LLD change |
| C. Let the time arrive as two FC06 writes with latch-and-apply on the second | Contradicts the data spec, which says FC06 to either register is refused | Changes the Gateway's `TimeService` and the data spec |

Recommendation: A or B, your call. C only if the spec itself should change.

### 2.3 B3: carrying the Field Device's alarm to the cloud

SD-05 expects `ModbusPoller` to read `ALARM_FLAGS`, tell the Gateway's `AlarmService` about a transition, and
`CloudPublisher` to publish it at QoS 1. The poller would have to read register 0x0031 (the placeholder profile covers
0x0010 to 0x0012 only, so this touches WP8's profile too), detect the transition, and hand it on; `CloudPublisher`
already publishes alarm events from the Gateway's own `AlarmService`. This is a feature, not a fix, and it needs its
own work package and an LLD pass first. Suggested home: with WP8, since the register ranges come from the profile.

### 2.4 B4: the poll period

REQ-NF-101 says to detect an alarm "within one polling cycle"; REQ-NF-113 then allows 500 ms from the flag to the
PUBACK. Both can hold only if the alarm registers are read faster than 1 s, or if the 500 ms is counted from the
Gateway's detection of the flag. Choices: a faster poll (the configured range is 100 ms to 60 s), a faster poll of
the alarm register alone, or re-reading the requirement. Decide before measuring, because the measurement will not
settle it.

## 3. What you need

| Item | Notes |
|---|---|
| Field Device | STM32F469I-DISCO with the application image of `firmware/field-device/Src/main.c` (2026-11-01, `docs/lld/application/field-device-main.md`): the real drivers, HealthMonitor, Logger, ConfigService, the register map with the real sensor, alarm and time providers, `ModbusSlave` and a SensorTask. Build and flash with `scripts/build-fd-app.sh --flash` (the CubeIDE `Debug` configuration still links the old harness). Never run on the board yet: bring it up alone first |
| Gateway | B-L475E-IOT01A with the production image (PR #124 and the stack, section 6 of the merge plan) and the real credential headers; the bootloader is not required for this test (`scripts/build-gateway-app.sh --flash`) |
| RS-485 | Two TTL-to-RS-485 modules (3.3 V), a twisted pair, Dupont wires |
| Dongle | The CH340 USB-to-RS-485 dongle: used **listen only** during the Gateway tests, as a master only in 2B-001 |
| Analyser | The 8-channel logic analyser (24 MHz): TX, RX and DE/RE of the Gateway side |
| Laptop | The Mosquitto broker with the operator certificate (`scripts/start-mosquitto-ota.sh` and `scripts/make-operator-cert.sh`, or the plain `scripts/start-mosquitto-tls.sh`), Python with `pyserial`, `paho-mqtt`, `minimalmodbus` |
| UART logs | Gateway: its ST-LINK port at 460800 8N1: the logs are in the console's centre panel (`picocom -b 460800`). Field Device: its ST-LINK port, 115200 8N1, plain log |

## 4. Wiring

Bus parameters: 9600 baud, 8N1, slave address 1 (REQ-MB-030). All grounds common.

| Signal | Field Device (F469 USART6) | Module #1 | Bus | Module #2 | Gateway (L475 UART4) |
|---|---|---|---|---|---|
| TX | PG14 (Arduino D1, CN7) | DI | | DI | PA0 (Arduino D1) |
| RX | PG9 (Arduino D0, CN7) | RO | | RO | PA1 (Arduino D0) |
| Direction | **none today (B1)** | DE/RE | | DE/RE | **PA15** (`UART4_RTS_DE`; find its header pin in UM2153, MBUART-O4) |
| Power | 3.3 V, GND | VCC, GND | GND to GND | VCC, GND | 3.3 V, GND |
| Line | | A | A to A | A | |
| Line | | B | B to B | B | |

Bridge the R16 pads on both modules (120 ohm termination, `hardware-setup-guide.md`). Each board's TX goes to the DI of
**its own** module. DE and RE are tied together on the modules, so one pin controls both: high = transmit.
Put the dongle's A and B on the same two lines, and the analyser on the Gateway TX (PA0), RX (PA1) and PA15.

## 5. Cases

Run them in order; each depends on the previous.

### 2B-000: wiring and power (no firmware)

Continuity of A, B and GND between the two modules; 3.3 V on both modules; R16 bridged. **Pass:** continuity, no short
A to B.

### 2B-001: the Field Device alone (decides B1)

Field Device powered and Operational; **Gateway unplugged**. The dongle is the master:

```
python3 - <<'EOF'
import minimalmodbus      # as in scripts/script_minimalmodbus.py
fd = minimalmodbus.Instrument('/dev/ttyUSB0', 1)
fd.serial.baudrate = 9600
fd.serial.timeout = 0.5
print(fd.read_registers(0x0000, 8, functioncode=4))     # identity: map version 1, vendor code 0x1A45
print(fd.read_registers(0x0010, 3, functioncode=4))     # temperature, humidity, pressure
EOF
```

**Pass:** both reads return values (register 0 is 1, register 7 is 0x1A45 = 6725). **If the read times out and the
Field Device log shows the request was served, B1 is real**: the Field Device cannot answer. Record which, with the
analyser on the Field Device TX line.

### 2B-002: the Gateway alone (the PA15 check, MBUART-O4 and TC-HW-MBUART-005)

Gateway running the production image; Field Device unplugged; the analyser on PA0, PA1 and PA15; the dongle listening.
**Pass:** every Gateway transmission is surrounded by PA15 high (DE asserted before the first start bit, released after
the last stop bit); the bus shows a request of 8 bytes starting `01 04 00 10 00 03`; a transaction is four
transmissions 200 ms apart (REQ-MB-050/060); the Gateway logs `MBM Slave 1: retries exhausted`. **If PA15 does not move,
or the dongle sees nothing, MBUART-O4 is not satisfied** (wrong header pin, or the module is not wired to it).
If every frame also appears in the Gateway's receive line, /RE is not tied to DE (MBUART-O5, echo).

### 2B-003: link establishment (SD-00b, REQ-NF-104)

Both boards on, bus connected. Start the monitor in a terminal:

```
python3 scripts/two_board_monitor.py --serial /dev/ttyUSB0
```

**Pass:** the monitor shows a request/response pair about every second; the Gateway logs `MBP slave 1 link up` after three
successful responses, about 3 s after the Field Device is Operational (the Field Device's tasks wait for its lifecycle start gate, so
expect no answers before that). Record the time from the Field Device's `Operational` to `link up`. Not observable: the device-id check (B5).

### 2B-004: telemetry end to end (SD-02, REQ-MB-010)

With the link up and the Gateway connected to the broker, subscribe to `dt/iotmonitor/+/telemetry`.
**Pass:** the published JSON has `"field_device":{"online":true, ...}` with values equal to those on the bus (the monitor's
`response FC04` line, register 0x0010 = temperature in centi-degrees, 0x0011 = humidity, 0x0012 = pressure), to within one
polling period of staleness.

### 2B-005: link loss and recovery (REQ-NF-103, REQ-NF-104)

Link up. Disconnect the **A** wire at one module. Count from the disconnection.
**Expected:** the Gateway logs `MBP slave 1 node offline` after three failed transactions. A failed transaction is four
attempts of 200 ms (0.8 s) and the poll period is 1 s, so about **3 s**; telemetry then reports `"online":false`.
Reconnect A. **Expected:** `MBP slave 1 link up` after three consecutive successes, about **3 s** later. Record both times.
**Pass:** offline after exactly three failed transactions (not one, not two) and online after exactly three successes
(REQ-NF-103/104); the times are consistent with the poll period.

### 2B-006: time push (SD-09, REQ-MB-020) — expected to fail until B2 is resolved

Force an NTP sync (reset the Gateway). After `NTP sync ok` the Gateway pushes the time with one FC16 of two registers
at 0x0210.
**Today (B2 open):** the monitor shows `FC16 write start=0x0210 count=2 values=...` followed by `exception fc=0x10 code=0x03`;
the Gateway logs `TS FD time push failed: Modbus error 3`; the Field Device logs `MRM CMD_SET_TIME refused: the pair
must arrive in one FC16`. That signature confirms B2 and nothing else is wrong.
**After B2 is resolved, pass:** no `FD time push` warning on the Gateway; the Field Device logs
`MRM CMD_SET_TIME applied: <epoch>`; the FC16 response is the 8-byte echo `start=0x0210 count=2`.

### 2B-007: alarm propagation (SD-05, REQ-NF-113) — blocked by B3 and B4

Part A, the Field Device side, can be run today: raise an alarm on the Field Device (lower the temperature high threshold
below the current reading, from the Field Device console or with the dongle as master in 2B-001's setup:
`fd.write_register(0x0101, 500, functioncode=6)` sets the high limit to 5.00 degrees: it must stay above the low limit, which is 0, or the write is refused with exception 0x03, and it is below room temperature). **Pass:** register 0x0031 changes from
0x0000 to a non-zero value on the next read. **This part cannot be combined with the Gateway** because two masters collide.

Part B, to the cloud, **cannot pass until B3 is implemented.** Today the monitor will show the flag value only if a
master reads 0x0031, which the Gateway never does. When B3 exists, measure with both the bus and the broker on one clock:

```
python3 scripts/two_board_monitor.py --serial /dev/ttyUSB0 --mqtt-host <broker-ip> \
    --cafile local-test/mosquitto-broker/ca.crt --cert local-test/mosquitto-broker/operator.crt \
    --key local-test/mosquitto-broker/operator.key --topic 'dt/iotmonitor/+/alarms'
```

It prints the time from the bus read that first showed the alarm to the broker receiving the alarm message
(the broker acknowledges at that moment). The time from the Field Device setting the flag to that first read, which is
up to one poll period (B4), is not visible: the Field Device logs nothing when it raises an alarm. Report both, and
the poll period in force, against the 500 ms of REQ-NF-113.

## 6. Expected logs

| Board | Event | Line |
|---|---|---|
| Gateway | link up | `[ INFO][hh:mm:ss][MBP ] slave 1 link up` |
| Gateway | link down | `[ WARN][hh:mm:ss][MBP ] slave 1 node offline` |
| Gateway | all attempts failed | `[ WARN][...][MBM ] Slave 1: retries exhausted (err 3)` |
| Gateway | time push failed | `[ WARN][...][TS ] FD time push failed: Modbus error 3` (exception) or `FD time push: response timeout` |
| Gateway | time push ok | nothing (success is silent); look for the absence of the warnings |
| Field Device | time applied | `MRM CMD_SET_TIME applied: <epoch>` |
| Field Device | time refused | `MRM CMD_SET_TIME refused: ...` |
| Field Device | alarm raised | **nothing is logged** (the AlarmService logs only its init and the acknowledgement) |

## 7. What to record

Per case: pass or fail, the date, the images (git commit of each board), and these figures: link-up time after the Field
Device is Operational (2B-003), offline and recovery times (2B-005), the FC16 and response frames (2B-006), the poll
period, the monitor's bus-to-broker time and the missing Field Device part of the latency (2B-007), and for 2B-001/002
the analyser traces of DE.

## 8. Where the results go

| Result | LLD |
|---|---|
| B1, PA15, DE timing, echo (2B-001, 2B-002) | `modbus-uart-driver.md` (MBUART-O4, O5, O6), `modbus-slave.md` |
| Link establishment, hysteresis times, polling, alarm flag (2B-003, 2B-005, 2B-007) | `modbus-poller-lld.md` |
| Frame handling, the FC16 behaviour (2B-006) | `modbus-slave.md`, `modbus-register-map-lld.md` |
| Time push (2B-006) | `time-service-lld.md` (TS-O5: handler written; the two-board check is this case) |

## 9. Tools prepared for this test

- `scripts/two_board_monitor.py`: a passive timeline of the Modbus bus (CRC checked, with the `ALARM_FLAGS` change marked) and
  the MQTT alarm topic on one clock. `--selftest` checks the decoding without hardware (it passes). It never transmits.
