# Sequence Diagrams — Companion Document

**Artefact:** HLD #5
**Status:** Specification complete (ready for VP drawing)
**Companion to:** `hld.md` §10 (to be added when this artefact merges)


## 1. Purpose and scope

This document is the authoritative specification of the runtime behaviour of the IoT Environmental Monitoring Gateway, expressed as UML sequence diagrams. It tells *who exchanges what message with whom, in what order, and under which conditions*. Diagrams drawn in Visual Paradigm follow this specification message-for-message.

**Granularity.** Sequence diagrams in this project are at **HLD level**: component-to-component, not function-to-function. A lifeline is a component listed in `components.md`. A message is a logical interaction (function call, queued message, MQTT publish, UART frame on the bus). RTOS queue mechanics, mutex acquisition, function signatures, and stack-frame detail belong to LLD.

**Authoritative split** (mirrors the convention from `state-machines.md`):

| Lives on the diagram                                                                                     | Lives in this document                                                                                              |
|----------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|
| Lifelines, messages, fragments (`alt`, `loop`, `opt`, `ref`, `par`), activation bars, timing constraints | Step 1 SRS-to-message mapping, participant list with stereotypes, full message table, traceability lines, decisions |

The diagram answers *"who talks to whom and when?"*. The document answers *"why does each message exist and which requirement does it satisfy?"*.

**Coverage of rainy paths.** Every flow is specified for both sunny and rainy outcomes. Failures are not afterthoughts: they appear inside `alt` fragments at the point of decision, in dedicated rainy sub-diagrams when the failure machinery is too rich to nest, or as `ref` to the diagram that owns the failure. SD-04 owns all cloud-disconnect handling and is referenced from SD-03, SD-05, and SD-06b at the publish point.

---

## 2. Inventory

| #     | Sequence                              | Sub-diagrams                                                       | Board(s)                  | Owner component                      | Use cases     | Primary SRS area              |
|-------|---------------------------------------|--------------------------------------------------------------------|---------------------------|--------------------------------------|---------------|-------------------------------|
| SD-00 | Boot and lifecycle init               | SD-00a (FD cold boot), SD-00b (GW cold boot + Modbus link establishment), SD-00c (GW post-update boot) | FD, GW             | `LifecycleController`                | UC-01, UC-17, UC-20 | LD, NF-203/-213, DM-040       |
| SD-01 | Sensor acquisition cycle              | one diagram (sunny + rainy via `alt`)                              | Field Device              | `SensorService`                      | UC-01, UC-07  | SA, NF-100                    |
| SD-02 | Modbus polling cycle                  | one diagram (sunny + rainy via `alt`+`loop`)                       | Gateway ↔ FD              | `ModbusPoller`                       | UC-07, UC-10  | MB, NF-103/-104/-105/-215     |
| SD-03 | Cloud telemetry publish (sunny only)  | SD-03a (telemetry), SD-03b (health)                                | Gateway                   | `CloudPublisher`                     | UC-05, UC-06  | CC, NF-106/-111/-112          |
| SD-04 | Store-and-forward (rainy lifecycle)   | SD-04a (disconnect→buffer), SD-04b (reconnect→drain)               | Gateway                   | `CloudPublisher`, `StoreAndForward`  | UC-10, UC-11 (E flows) | BF, NF-200/-209          |
| SD-05 | Alarm propagation                     | one diagram (sunny + rainy via `alt` at publish point)             | FD ↔ GW ↔ Cloud           | `AlarmService` (FD, GW)              | UC-08, UC-09, UC-12 | AM, NF-101/-113/-207     |
| SD-06 | OTA firmware update                   | SD-06a, SD-06b, SD-06c, SD-06d                                     | Cloud → GW                | `UpdateService`                      | UC-18, UC-19, UC-20 | DM, NF-204               |
| SD-07 | Remote configuration command          | one diagram (with profile-update branch)                           | Cloud → GW                | `ConfigService`                      | UC-15, UC-19  | DM, NF-301                    |
| SD-08 | Remote restart                        | one diagram (with confirmation, restart, post-boot self-check)     | Cloud → GW                | `LifecycleController`                | UC-17, UC-19  | DM, NF-203                    |
| SD-09 | Time synchronisation                  | one diagram (NTP plus FC06/16 to FD)                               | Cloud → GW → FD           | `TimeProvider`                       | UC-13         | TS, NF-210/-211               |
| SD-10 | Device provisioning                   | one diagram                                                        | Service Technician → GW   | `ConfigService`, `DeviceProfileRegistry` | UC-16     | DM, MB-110/-111 (proposed)    |

**Total diagrams to draw in VP:** 18 (SD-00a, SD-00b, SD-00c, SD-01, SD-02, SD-03a, SD-03b, SD-04a, SD-04b, SD-05, SD-06a, SD-06b, SD-06c, SD-06d, SD-07, SD-08, SD-09, SD-10).

**Why SD-00 is split into three sub-diagrams.** Cold boot of the field device, cold boot of the gateway with Modbus link establishment, and post-update boot are three distinct paths with disjoint participant sets and different exit states. A single diagram would either mix concerns or hide important pre-conditions.

**Why SD-03 is split into SD-03a and SD-03b.** Telemetry (60 s, REQ-NF-111) and health (600 s, REQ-NF-112) fire at different rates from different producers (`SensorService` vs `HealthMonitor`) and target different topics (REQ-CC-080). A single diagram with `par` would mislead the reader into thinking they're synchronised.

**Why SD-04 is split into SD-04a and SD-04b.** Disconnect-detection / buffering and reconnect / drain are two distinct phases with no shared messages; combining them produces a diagram dominated by white space.

**Why SD-06 is split into four sub-diagrams.** A single OTA sequence would exceed the P9 readability budget and span two MCU reboots, obscuring the structure. Decomposition aligns with the eight-state Firmware Update state machine in `state-machines.md` §7.4.

---

## 3. Conventions

These rules apply to every diagram. They are locked in and mirror the conventions established for state machines.

### 3.1 Lifelines and stereotypes

A lifeline is a component listed in `components.md`. **No invented names.** If a flow seems to need a lifeline that does not exist as a component, the component spec is incomplete — escalate, do not improvise.

| Stereotype          | Meaning                                                                          | Example lifelines                          |
|---------------------|----------------------------------------------------------------------------------|--------------------------------------------|
| `«application»`     | Application-layer component                                                      | `SensorService`, `CloudPublisher`          |
| `«middleware»`      | Middleware-layer component                                                       | `ModbusMaster`, `MqttClient`               |
| `«driver»`          | Driver-layer component                                                           | `ModbusUartDriver`, `WifiDriver`           |
| `«cloud»`           | External cloud service                                                           | `AWS IoT Core`                             |
| `«actor»`           | Human or system actor                                                            | `Remote Operator`, `Service Technician`    |
| `«timer»`           | RTOS timer or periodic event source (left-edge marker, not a full lifeline)      | (timer label at top-left)                  |
| `«bootloader»`      | Bootloader stage at boundary of HLD scope                                        | `Bootloader` (SD-00c)                      |

Cross-cutting components (`Logger`, `HealthMonitor`) are **elided by default** (P4). When their behaviour *is* the subject of the sequence, they appear; otherwise a single UML note suffices: *"Logger and HealthMonitor consumed by all participants (see components.md)."*

### 3.2 Participant ordering

Place lifelines left-to-right in **first-appearance order on the happy path**. Do not sort by layer or alphabet. The reader should follow top-to-bottom and left-to-right without backtracking.

For cross-board sequences (SD-02, SD-05, parts of SD-06, SD-09), insert a vertical **bus boundary** between the gateway lifelines and the field-device lifelines, annotated with the physical medium (`RS-485 / Modbus RTU` or `WiFi / TLS / MQTT`).

### 3.3 Messages

| Type           | UML notation                                      | When to use                                              |
|----------------|---------------------------------------------------|----------------------------------------------------------|
| Synchronous    | Solid line, filled arrowhead                      | In-process function call where the caller blocks         |
| Asynchronous   | Solid line, open arrowhead                        | RTOS queue post, MQTT publish, UART frame on the wire    |
| Return         | Dashed line, open arrowhead                       | Logical return of a value to the caller                  |
| Error / failed | Dashed line, open arrowhead, red (`#CC6666`)      | Failure path returns and timeouts                        |
| Self-message   | Solid line looping back                           | Internal processing step worth showing                   |

Omit return arrows for void functions unless the return point matters for ordering. Show returns when a value flows back that the caller acts upon.

### 3.4 Fragments — encoding in the message table

A fragment (`alt`, `opt`, `loop`, `par`, `ref`, `break`) wraps messages rather than *being* a message. The flat row-per-message table is extended with **fragment delimiter rows** to encode this nesting without breaking the linear reading order.

**Rules:**

1. **Open / close rows.** Each fragment opens with one row and closes with one row. Both rows put `—` in the `#` column so they do not disturb message numbering.
2. **Fragment keyword in bold.** The keyword (`alt`, `opt`, `loop`, `par`, `ref`, `end alt`, `end loop`, …) goes in the `Guard / fragment` column, bold.
3. **Operands of `alt`.** For multi-operand fragments, add a sub-row per operand carrying its guard in the `Guard / fragment` column. Sub-rows also use `—` in the `#` column.
4. **Indentation.** Indent wrapped messages with two leading spaces in the `From` cell per level of nesting. Two levels of nesting → four spaces. The reader scanning the `From` column sees the depth at a glance.
5. **Trace on the open row.** The `Trace` column on the fragment open row records the SRS ID(s) that *motivate the fragment's existence* (e.g., REQ-MB-060 motivates `loop(1..3)`). Wrapped messages keep their own traces.
6. **`ref` is a single row.** A `ref` represents one referenced sequence diagram, like a function call, so it occupies one row with the bold keyword `ref` and the referenced diagram name in `Message text`.
7. **Sub-numbered IDs in the rainy operand.** When `alt` has a sunny operand (which keeps the main numbering 4, 5, 6…) and a rainy operand (which is alternative to the same step), the rainy messages are sub-numbered (`4'`, `5'`, …) to make clear they are *alternatives to*, not *successors of*, the sunny messages. After the `end alt` numbering resumes from the next free integer.

### 3.5 Timing constraints

Where the SRS imposes a non-functional timing requirement, annotate the relevant message or fragment with a constraint in `{...}`. Examples:

- `{≤ 200 ms}` on the Modbus response leg (REQ-NF-105)
- `{≤ 500 ms from FD detection to PUBACK}` on the alarm publish (REQ-NF-113)
- `{≤ 100 ms total}` on the SensorService activation bar (REQ-NF-100)
- `{1 Hz}` on the periodic poll trigger (REQ-NF-110)

Attach the constraint to the leftmost element it applies to. When a constraint covers multiple messages, use a duration constraint bracket spanning the activation bar.

### 3.6 Naming — exact strings to use in VP

- **Lifeline labels:** `<ComponentName> «stereotype»`. Example: `SensorService «application»`. PascalCase for the name; angle-quotes for the stereotype.
- **Synchronous messages:** lowercase verb phrase, no parentheses, no parameter types. Example: `read humidity and temperature`. Not `read_humidity_and_temperature()`.
- **Asynchronous messages:** noun phrase or imperative. Example: `publish telemetry`, `frame on bus`.
- **Return messages:** noun phrase describing the value. Example: `temp, humidity`. Not `OK`, not `0`.
- **Self-messages:** active verb phrase describing the step. Example: `clamp to range`, `compute low-pass filter`.
- **Guards:** square brackets, present tense, no implementation detail. Example: `[response received]`, `[buffer not empty]`. Not `[rx_complete == 1]`.

### 3.7 Colour palette

See `diagram-colour-palette.md` §4 for the canonical palette. Summary:

- Field-device lifelines: blue family (`#DAEAF6` fill, `#7BAFD4` border).
- Gateway lifelines: green family (`#D5F0DE` fill, `#6FBF8E` border).
- Cloud lifelines: amber family (`#FDE8C8` fill, `#E0A84C` border).
- Actor lifelines: neutral grey.
- Synchronous arrows: `#444444` solid, filled head.
- Asynchronous arrows: `#444444` solid, open head.
- Return arrows: `#888888` dashed.
- Error arrows: `#CC6666` dashed.
- `alt`/`loop` frame: `#F7F7F7` fill, `#AAAAAA` border.

VP discipline: VP remembers the last theme used. Set the palette explicitly per diagram before placing the first lifeline. Cross-board diagrams use both blue and green — set per lifeline.

### 3.8 What does NOT belong on an HLD sequence diagram

- RTOS queue names, sizes, or item types
- Mutex / semaphore acquisitions and releases
- Function signatures and parameter types
- Memory allocation events (no allocation after init in this project anyway)
- Stack-frame or register-level detail
- ISR vs task context (unless the boundary itself is the subject)
- Specific Modbus byte encoding (function code yes; CRC bytes no)
- TLS handshake internals (note "TLS handshake" as a single message)
- LVGL widget-level redraw mechanics

If a question can only be answered by writing C code, it belongs in LLD.

---

## 4. Three-step derivation methodology

Each sequence is derived in three steps before any drawing. Mirror of the state-machine methodology.

### Step 1 — SRS to message mapping

Read every SRS requirement traceable to the use case the sequence implements. For each, attach one or more labels:

| Label                                | Meaning                                                                |
|--------------------------------------|------------------------------------------------------------------------|
| `IMPLIES PARTICIPANT`                | A component is involved in this flow → it becomes a lifeline           |
| `IMPLIES MESSAGE`                    | A specific call or queued message must appear                          |
| `IMPLIES MESSAGE PAYLOAD`            | A specific field appears in a message body (rendered as a UML note)    |
| `IMPLIES FRAGMENT(alt/opt/loop/par)` | A control-flow fragment is required                                    |
| `IMPLIES GUARD`                      | A condition narrows when a message or fragment fires                   |
| `IMPLIES TIMING CONSTRAINT`          | A non-functional timing rule applies to a message or activation bar    |
| `IMPLIES RETURN`                     | A value must flow back to the caller                                   |
| `NO SEQUENCE IMPLICATION`            | Configuration / startup / steady-state predicate, not a runtime message|

Common mistake: tagging payload requirements (REQ-CC-071 schema version, REQ-CC-090 serial number) as `NO SEQUENCE IMPLICATION`. They *are* runtime — they appear in every message body. Use `IMPLIES MESSAGE PAYLOAD`.

### Step 2 — Participant list with stereotypes

Build the lifeline list from `IMPLIES PARTICIPANT` labels. For each lifeline:

1. Component name (must match `components.md` exactly — copy-paste, do not retype).
2. Stereotype.
3. Board (FD / GW / Cloud / Actor).
4. One-line role *in this specific sequence* (different from its general responsibility — what does it *do here*?).

If a lifeline does not appear in `components.md`, stop and reconcile.

### Step 3 — Message table

Build a numbered message table from the remaining labels:

| # | From | To | Type | Message text | Guard / fragment | Timing | Trace |
|---|------|----|----|--------------|------------------|--------|-------|

Time order is row order. Numbering restarts at `1` per diagram. Every row that represents a message has a trace; if a row has no trace, it should not be in the diagram. Fragment delimiter rows follow §3.4 conventions.

After Step 3 is complete, drawing in VP is mechanical: the message table *is* the diagram, written down.

---

## 5. SD-00 — Boot and lifecycle init (decomposed)

**Owner:** `LifecycleController` on each board.
**Use cases:** UC-01 (FD boot splash), UC-17 (GW remote restart context), UC-20 (post-update boot).
**Scope:** From power-on / reset to the steady state where the application loops are running. Three sub-diagrams cover the three distinct boot paths: FD cold boot, GW cold boot (with Modbus link establishment via device-profile validation), and GW post-update boot (resumption from persisted flags).

This sequence is the runtime view of the LifecycleController state machine's Init phase (`state-machines.md` §7.1, §7.2).

### 5.1 SD-00a — Field Device cold boot

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                                      | Label                                       |
|-------------|------------------------------------------------------------------------------------|---------------------------------------------|
| REQ-LD-200  | Display boot splash screen                                                         | IMPLIES MESSAGE (LCD render)                |
| REQ-LD-210  | Show progress bar during boot                                                      | IMPLIES MESSAGE (progress update) + IMPLIES FRAGMENT(loop) |
| REQ-LD-220  | Indicate stage of boot                                                             | IMPLIES MESSAGE PAYLOAD (stage label)       |
| REQ-LD-230  | Boot splash visible until Operational                                              | NO SEQUENCE IMPLICATION here (lifecycle predicate) |
| REQ-LD-240  | Splash dismissed on first Operational frame                                        | IMPLIES MESSAGE (final LCD update)          |
| REQ-DM-090  | Persist configuration to non-volatile storage                                      | NO SEQUENCE IMPLICATION here (load side appears) |
| REQ-NF-213  | Boot to operational within [TBD]                                                   | IMPLIES TIMING CONSTRAINT on boot duration  |

#### Step 2 — Participants

| # | Lifeline                | Stereotype     | Board | Role in this sequence                                |
|---|-------------------------|----------------|-------|------------------------------------------------------|
| 1 | `LifecycleController`   | «application»  | FD    | Drives boot stages                                   |
| 2 | `ConfigService`         | «application»  | FD    | Loads persisted configuration                        |
| 3 | `ConfigStore`           | «middleware»   | FD    | Reads from QSPI flash                                |
| 4 | `GraphicsLibrary`       | «middleware»   | FD    | Renders boot splash and progress bar                 |
| 5 | `SensorService`         | «application»  | FD    | Initialises sensor drivers                           |
| 6 | `ModbusSlave`            | «middleware»   | FD    | Initialises slave dispatcher                          |
| 7 | `Logger`                | «middleware»   | FD    | Records boot stages                                  |

#### Step 3 — Message table

| #  | From                  | To                | Type   | Message text                          | Guard / fragment | Timing                           | Trace             |
|----|-----------------------|-------------------|--------|---------------------------------------|------------------|----------------------------------|-------------------|
| 1  | `LifecycleController` | `LifecycleController` | self | enter Init                           |                  | boot total {≤ NF-213 [TBD]}      | NF-213            |
| 2  | `LifecycleController` | `GraphicsLibrary` | sync   | render splash (stage: starting)       |                  |                                  | LD-200, LD-220    |
| 3  | `LifecycleController` | `ConfigService`   | sync   | load configuration                    |                  |                                  | DM-090            |
| 4  | `ConfigService`       | `ConfigStore`     | sync   | read config from flash                |                  |                                  | DM-090            |
| 5  | `ConfigStore`         | `ConfigService`   | return | configuration                         |                  |                                  |                   |
| 6  | `LifecycleController` | `GraphicsLibrary` | sync   | update progress (stage: drivers)      |                  |                                  | LD-210, LD-220    |
| 7  | `LifecycleController` | `SensorService`   | sync   | initialise sensors                    |                  |                                  | SA-070            |
| 8  | `SensorService`       | `LifecycleController` | return | ok                                |                  |                                  |                   |
| 9  | `LifecycleController` | `GraphicsLibrary` | sync   | update progress (stage: comms)        |                  |                                  | LD-210, LD-220    |
| 10 | `LifecycleController` | `ModbusSlave`     | sync   | initialise slave                      |                  |                                  | MB-030            |
| 11 | `ModbusSlave`         | `LifecycleController` | return | ready                              |                  |                                  |                   |
| 12 | `LifecycleController` | `Logger`          | async  | log boot complete                     |                  |                                  |                   |
| 13 | `LifecycleController` | `LifecycleController` | self | transition Init → Operational        |                  |                                  | (state-machines.md §7.1) |
| 14 | `LifecycleController` | `GraphicsLibrary` | sync   | dismiss splash; render Operational UI |                  |                                  | LD-240            |

**UML note 1 (progress granularity):** *"Progress bar update steps (messages 2, 6, 9, 14) correspond to broad stages, not individual sub-tasks. Stage labels per REQ-LD-220: 'starting', 'drivers', 'comms', 'ready'."*

**UML note 2 (no Modbus probe on FD):** *"The FD slave does not probe its master; it waits for incoming requests. Slave initialisation enables the receive path; first traffic appears in SD-02."*

### 5.2 SD-00b — Gateway cold boot with Modbus link establishment

#### Step 1 — SRS mapping

| SRS                     | Text fragment                                                                      | Label                                       |
|-------------------------|------------------------------------------------------------------------------------|---------------------------------------------|
| REQ-DM-090              | Persist / load configuration                                                       | IMPLIES MESSAGE (load side)                 |
| REQ-MB-110 (proposed)   | Maintain device-profile registry                                                   | IMPLIES MESSAGE (load profiles)             |
| REQ-MB-120 (proposed)   | Verify slave device ID against bound profile during link establishment             | IMPLIES MESSAGE + IMPLIES FRAGMENT(alt)     |
| REQ-MB-130 (proposed)   | Log device-profile validation failures                                             | IMPLIES MESSAGE (log on rainy)              |
| REQ-MB-100              | Address multiple field devices by unique slave address                             | IMPLIES MESSAGE (per-slave probe loop)      |
| REQ-CC-050              | Connect to AWS IoT Core via TLS                                                    | IMPLIES `ref` to SD-04b (TLS handshake)     |
| REQ-NF-203              | Recover from MCU reset within 5 s                                                  | IMPLIES TIMING CONSTRAINT on boot duration  |
| REQ-NF-213              | Boot to operational within [TBD]                                                   | IMPLIES TIMING CONSTRAINT                   |
| REQ-TS-000              | NTP sync at boot                                                                   | IMPLIES `ref` to SD-09                      |
| REQ-DM-040              | Self-check verifies sensor initialisations and communication links after boot      | IMPLIES MESSAGE (probes) + IMPLIES FRAGMENT(alt) |

#### Step 2 — Participants

| #  | Lifeline                | Stereotype     | Board | Role in this sequence                                  |
|----|-------------------------|----------------|-------|--------------------------------------------------------|
| 1  | `LifecycleController`   | «application»  | GW    | Drives boot stages                                     |
| 2  | `ConfigService`         | «application»  | GW    | Loads persisted configuration                          |
| 3  | `ConfigStore`           | «middleware»   | GW    | Reads from QSPI flash                                  |
| 4  | `DeviceProfileRegistry` | «application»  | GW    | Holds loaded device profiles                           |
| 5  | `ModbusPoller`          | «application»  | GW    | Per-slave probe and link establishment                 |
| 6  | `ModbusMaster`          | «middleware»   | GW    | Issues probe requests                                  |
| 7  | `ModbusUartDriver` (GW) | «driver»       | GW    | Bus transmit / receive                                 |
| 8  | `MqttClient`            | «middleware»   | GW    | Cloud connection establishment                         |
| 9  | `WifiDriver`            | «driver»       | GW    | TLS-protected transport                                |
| 10 | `Logger`                | «middleware»   | GW    | Records boot and link establishment events             |
| 11 | `SensorService`         | «application»  | GW    | Probed during Init.SelfChecking to verify sensor initialisation (DM-040) |

#### Step 3 — Message table

| #   | From                    | To                       | Type   | Message text                                | Guard / fragment                          | Timing                       | Trace                       |
|-----|-------------------------|--------------------------|--------|---------------------------------------------|-------------------------------------------|------------------------------|-----------------------------|
| 1   | `LifecycleController`   | `LifecycleController`    | self   | enter Init                                  |                                           | boot total {≤ NF-203 5 s}    | NF-203, NF-213              |
| 2   | `LifecycleController`   | `ConfigService`          | sync   | load configuration                          |                                           |                              | DM-090                      |
| 3   | `ConfigService`         | `ConfigStore`            | sync   | read config from flash                      |                                           |                              | DM-090                      |
| 4   | `ConfigStore`           | `ConfigService`          | return | configuration                               |                                           |                              |                             |
| 5   | `LifecycleController`   | `DeviceProfileRegistry`  | sync   | load device profiles                        |                                           |                              | MB-110 (proposed)           |
| 6   | `DeviceProfileRegistry` | `ConfigStore`            | sync   | read profiles                               |                                           |                              | MB-110, MB-111 (proposed)   |
| 7   | `ConfigStore`           | `DeviceProfileRegistry`  | return | profile list                                |                                           |                              |                             |
| —   |                         |                          |        |                                             | **ref** SD-09 (NTP boot sync)             |                              | TS-000                      |
| —   |                         |                          |        |                                             | **ref** SD-04a (cloud connection on boot) |                              | CC-050                      |
| —   |                         |                          |        |                                             | **loop** `[for each profile in registry]` |                              | MB-100, MB-120 (proposed)   |
| 8   |   `ModbusPoller`        | `DeviceProfileRegistry`  | sync   | get next profile                            |                                           |                              | MB-110 (proposed)           |
| 9   |   `DeviceProfileRegistry`| `ModbusPoller`          | return | profile (slave_addr, expected device_id)    |                                           |                              |                             |
| 10  |   `ModbusPoller`        | `ModbusMaster`           | sync   | probe slave (read device-ID register)       |                                           |                              | MB-040, MB-100              |
| 11  |   `ModbusMaster`        | `ModbusUartDriver` (GW)  | sync   | write probe frame                           |                                           |                              | MB-030                      |
| —   |                         |                          |        |                                             | **alt**                                   |                              | MB-120 (proposed), NF-105   |
| —   |                         |                          |        |                                             | `[response received and device_id matches]` |                            |                             |
| 12  |     `ModbusUartDriver` (GW) | `ModbusMaster`       | async  | response (device_id)                        |                                           | round trip {≤ 200 ms}: NF-105 | MB-030                     |
| 13  |     `ModbusMaster`      | `ModbusPoller`           | return | device_id                                   |                                           |                              |                             |
| 14  |     `ModbusPoller`      | `ModbusPoller`           | self   | mark slave link_up; bind profile            |                                           |                              | MB-120 (proposed)           |
| —   |                         |                          |        |                                             | `[response received but device_id mismatch]` |                           |                             |
| 12' |     `ModbusUartDriver` (GW) | `ModbusMaster`       | async  | response (wrong device_id)                  |                                           |                              | MB-030                      |
| 13' |     `ModbusMaster`      | `ModbusPoller`           | return | device_id                                   |                                           |                              |                             |
| 14' |     `ModbusPoller`      | `Logger`                 | async  | log mismatch (expected, got, slave_addr)    |                                           |                              | MB-130 (proposed)           |
| 15' |     `ModbusPoller`      | `ModbusPoller`           | self   | mark slave link_rejected                    |                                           |                              | MB-120 (proposed)           |
| —   |                         |                          |        |                                             | `[no response after retries]`             |                              |                             |
| 12''|     `ModbusMaster`      | `ModbusPoller`           | return | failure (timeout)                           |                                           |                              | MB-050, NF-105              |
| 13''|     `ModbusPoller`      | `Logger`                 | async  | log slave silent (slave_addr)               |                                           |                              | MB-130 (proposed)           |
| 14''|     `ModbusPoller`      | `ModbusPoller`           | self   | mark slave link_unlinked                    |                                           |                              |                             |
| —   |                         |                          |        |                                             | **end alt**                               |                              |                             |
| —   |                         |                          |        |                                             | **end loop**                              |                              |                             |
| 15  | `LifecycleController`   | `SensorService`          | sync   | probe sensors                               |                                           |                              | DM-040                      |
| 16  | `SensorService`         | `LifecycleController`    | return | sensor status                               |                                           |                              | DM-040                      |
| 17  | `LifecycleController`   | `ModbusPoller`           | sync   | probe Modbus link                           |                                           |                              | DM-040                      |
| 18  | `ModbusPoller`          | `LifecycleController`    | return | link status                                 |                                           |                              | DM-040                      |
| —   |                         |                          |        |                                             | **alt**                                   |                              | DM-040                      |
| —   |                         |                          |        |                                             | `[all probes ok]`                         |                              |                             |
| 19  |   `LifecycleController` | `Logger`                 | async  | log boot complete (link summary)            |                                           |                              |                             |
| 20  |   `LifecycleController` | `LifecycleController`    | self   | transition Init → Operational               |                                           |                              | (state-machines.md §7.2)    |
| —   |                         |                          |        |                                             | `[any probe failed]`                      |                              |                             |
| 19' |   `LifecycleController` | `Logger`                 | async  | log boot failed (self-check failure)        |                                           |                              | DM-040                      |
| 20' |   `LifecycleController` | `LifecycleController`    | self   | transition Init → Faulted                   |                                           |                              | (state-machines.md §7.2)    |
| —   |                         |                          |        |                                             | **end alt**                               |                              |                             |

**UML note 1 (Option C — fall-through):** *"The gateway transitions to Operational regardless of per-slave outcomes. Slaves marked `link_rejected` or `link_unlinked` are not polled; this is the industry-standard 'deny by default' behaviour. The cloud is informed of the per-slave link state via health telemetry (SD-03b)."*

**UML note 2 (NTP and cloud connection precede probing):** *"The two `ref` boxes (NTP boot sync and cloud connection) execute before the per-slave probe loop. The cloud connection enables both NTP and MQTT command intake; if either fails, boot continues but the gateway operates with uptime-based timestamps (REQ-NF-212) and waits for cloud reconnect via SD-04b's reconnect loop."*

**UML note 3 (per-slave link state, decision SD00-D5):** *"Link state is per-slave, not system-wide. The link-state events emitted by SD-02 carry the slave address."*

**UML note 4 (Init.SelfChecking — DM-040):** *"After the per-slave probe loop, `LifecycleController` performs a high-level self-check (messages 15–18): probe `SensorService` and `ModbusPoller` to verify initialisations and communication links (REQ-DM-040). If all probes succeed the gateway transitions to Operational (message 20); if any probe fails it enters Faulted (message 20'). This is the Init.SelfChecking sub-state defined in state-machines.md §7.2."*

### 5.3 SD-00c — Gateway post-update boot (resumption)

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                                      | Label                                       |
|-------------|------------------------------------------------------------------------------------|---------------------------------------------|
| REQ-DM-073  | Retain previous firmware until self-check passes                                   | IMPLIES MESSAGE (check pending flags — message 3) |
| REQ-DM-071  | Trigger self-check validation                                                      | IMPLIES MESSAGE (resume in SelfChecking)    |
| REQ-DM-072  | Rollback if self-check fails                                                       | IMPLIES `ref` to SD-06d                     |
| REQ-NF-204  | Roll back within 10 s post-installation                                            | IMPLIES TIMING CONSTRAINT                   |

#### Step 2 — Participants

| # | Lifeline                | Stereotype      | Board | Role                                                |
|---|-------------------------|-----------------|-------|-----------------------------------------------------|
| 1 | `Bootloader`            | «bootloader»    | GW    | Boundary actor: selects boot bank from indicator    |
| 2 | `LifecycleController`   | «application»   | GW    | Detects pending flags; routes resumption            |
| 3 | `FirmwareStore`         | «middleware»    | GW    | Reads pending flags                                  |
| 4 | `UpdateService`         | «application»   | GW    | Resumes self-check or rollback report               |
| 5 | `Logger`                | «middleware»    | GW    | Records boot path                                    |

#### Step 3 — Message table

| #  | From                  | To                  | Type   | Message text                                | Guard / fragment                       | Timing                            | Trace                                  |
|----|-----------------------|---------------------|--------|---------------------------------------------|----------------------------------------|-----------------------------------|----------------------------------------|
| 1  | `Bootloader`          | `LifecycleController` | sync | start application (from selected bank)      |                                        |                                   | DM-070                                 |
| 2  | `LifecycleController` | `LifecycleController` | self | enter Init                                  |                                        |                                   | (state-machines.md §7.2)               |
| 3  | `LifecycleController` | `FirmwareStore`     | sync   | check pending flags                         |                                        |                                   | DM-073                                 |
| 4  | `FirmwareStore`       | `LifecycleController` | return | flags (pending_self_check / pending_rollback / none) |                              |                                   |                                        |
| —  |                       |                     |        |                                             | **alt**                                |                                   |                                        |
| —  |                       |                     |        |                                             | `[pending_self_check]`                 |                                   |                                        |
| 5  |   `LifecycleController`| `UpdateService`    | sync   | resume in SelfChecking                      |                                        |                                   | DM-071, (state-machines.md §7.4)       |
| —  |                       |                     |        |                                             | **ref** SD-06d (self-check)            | rollback total {≤ 10 s}: NF-204   | NF-204                                 |
| —  |                       |                     |        |                                             | `[pending_rollback]`                   |                                   |                                        |
| 5' |   `LifecycleController`| `UpdateService`    | sync   | resume after rollback                       |                                        |                                   | DM-072                                 |
| 6' |   `UpdateService`     | `Logger`            | async  | log rollback completion                     |                                        |                                   | DM-072                                 |
| —  |                       |                     |        |                                             | `[no flags]`                           |                                   |                                        |
| 5''|   `LifecycleController`| `Logger`           | async  | log clean boot                              |                                        |                                   |                                        |
| —  |                       |                     |        |                                             | **end alt**                            |                                   |                                        |
| 6  | `LifecycleController` | `LifecycleController`| self  | transition Init → Operational (or stay in SelfChecking until update outcome) |          |                                   | (state-machines.md §7.2)               |

**UML note 1 (bootloader scope):** *"`Bootloader` is a boundary actor — its internal logic (read boot indicator, validate bank header, jump to application) is out of HLD scope. The «bootloader» stereotype signals a boundary similar to «cloud» for AWS IoT Core."*

**UML note 2 (Q6 deferred):** *"How `LifecycleController` distinguishes a clean boot from a 'no flags' post-update boot is open question Q6 (see §18). Resolved at LLD by either timestamping flag clears or by maintaining an explicit `last_boot_was_update` indicator."*

### 5.4 Decisions specific to SD-00

| #       | Decision                                                                                                | Rationale                                                                                                          |
|---------|---------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| SD00-D1 | Three sub-diagrams (a / b / c) for boot                                                                  | FD cold, GW cold, and GW post-update have disjoint participant sets; combining would mislead                       |
| SD00-D2 | Per-slave probe with profile-bound device-ID validation; fall-through to Operational on failure (Option C) | Industry-standard "deny by default"; supports REQ-NF-203 5 s budget without blocking on unreachable slaves         |
| SD00-D3 | `Bootloader` modelled as «bootloader» boundary actor                                                     | Bootloader internals are out of HLD scope; its handoff to application *is* in scope (analogous to AWS IoT Core)    |
| SD00-D4 | NTP and cloud connection precede Modbus probing                                                          | Both are needed to log probe outcomes with synchronised timestamps; their failure is not blocking (best-effort)    |
| SD00-D5 | Per-slave link state with profile binding                                                                | Supports REQ-MB-100 operationally; the slave allowlist is a derived view of the registry                           |
| SD00-D6 | Boot splash on FD only (REQ-LD-200..240)                                                                | GW has no LCD; SRS scopes splash to FD                                                                              |

---

## 6. SD-01 — Sensor acquisition cycle (Field Device)

**Owner:** `SensorService` (Application, FD).
**Trigger:** 1 Hz periodic timer driven by RTOS (REQ-NF-110).
**Use cases:** UC-01, UC-07.
**Scope:** Periodic acquisition cycle from timer tick to data-available state, including per-sensor failure handling. **Excludes** the LCD refresh (different rate, separate consumer) and the Modbus master read (separate sequence — SD-02).

### 6.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                               | Label                                                                  |
|-------------|-----------------------------------------------------------------------------|------------------------------------------------------------------------|
| REQ-SA-070  | Read temperature, humidity, and pressure at configurable polling interval   | IMPLIES MESSAGE × 2; IMPLIES PARTICIPANT (`HumidityTempDriver`, `BarometerDriver`) |
| REQ-SA-100  | Store a timestamp with each sensor measurement                              | IMPLIES MESSAGE (self: stamp)                                          |
| REQ-SA-110  | Apply same processing pipeline to periodic and on-demand readings           | NO SEQUENCE IMPLICATION (predicate)                                    |
| REQ-SA-120  | Validate that acquired value is within configured range                     | IMPLIES MESSAGE (self: validate range)                                 |
| REQ-SA-130  | Clamp out-of-range readings to the nearest range boundary                   | IMPLIES MESSAGE (self: clamp)                                          |
| REQ-SA-140  | Filter measurement using a low-pass filter                                  | IMPLIES MESSAGE (self: low-pass filter)                                |
| REQ-SA-150  | Make processed data available for Modbus and LCD                            | IMPLIES MESSAGE (self: publish to ISensorReadings)                     |
| REQ-SA-080  | Log the error code if the reading fails                                     | IMPLIES FRAGMENT(alt) + IMPLIES MESSAGE (log on error operand)         |
| REQ-SA-0E1  | Mark sensor value as invalid if reading fails                               | IMPLIES FRAGMENT(alt) + IMPLIES MESSAGE (mark invalid on error operand)|
| REQ-SA-060  | Continue operation with available sensors if some fail                      | IMPLIES GUARD (cycle does not abort on per-sensor failure)             |
| REQ-SA-160  | Publish sensor data marked as invalid to the cloud with an error flag       | NO SEQUENCE IMPLICATION here (carried downstream by REQ-SA-150 publish)|
| REQ-NF-100  | Full polling cycle within 100 ms                                            | IMPLIES TIMING CONSTRAINT on SensorService activation                  |
| REQ-NF-110  | Default polling rate 1 Hz                                                   | IMPLIES TIMING CONSTRAINT on trigger                                   |
| REQ-NF-208  | Substitute defined error indicator for any sensor that fails to read        | NO SEQUENCE IMPLICATION here (the "mark invalid" self-message embodies this) |
| REQ-NF-212  | Use uptime-based timestamp and flag data unsynchronised if RTC not synced   | NO SEQUENCE IMPLICATION here (payload property of the timestamp)       |

### 6.2 Step 2 — Participants

| # | Lifeline               | Stereotype     | Board | Role in this sequence                              |
|---|------------------------|----------------|-------|----------------------------------------------------|
| 1 | (1 Hz timer)           | «timer»        | FD    | Periodic event source on left edge                 |
| 2 | `SensorService`        | «application»  | FD    | Orchestrates the cycle; holds the latest reading   |
| 3 | `HumidityTempDriver`   | «driver»       | FD    | Returns one (temperature, humidity) sample         |
| 4 | `BarometerDriver`      | «driver»       | FD    | Returns one pressure sample                        |
| 5 | `Logger`               | «middleware»   | FD    | Records driver-failure events (P4 exception — log *is* the rainy behaviour) |

`I2cDriver` is not a lifeline at HLD level — driver-to-driver internal traffic is LLD.

### 6.3 Step 3 — Message table

| #   | From                   | To                   | Type   | Message text               | Guard / fragment          | Timing                  | Trace          |
|-----|------------------------|----------------------|--------|----------------------------|---------------------------|-------------------------|----------------|
| 1   | (timer)                | `SensorService`      | async  | tick                       |                           | period 1 s              | NF-110         |
| 2   | `SensorService`        | `HumidityTempDriver` | sync   | read temperature, humidity |                           |                         | SA-070         |
| —   |                        |                      |        |                            | **alt**                   |                         | SA-080, SA-0E1 |
| —   |                        |                      |        |                            | `[driver returned ok]`    |                         |                |
| 3   |   `HumidityTempDriver` | `SensorService`      | return | temp, humidity             |                           |                         | SA-070         |
| —   |                        |                      |        |                            | `[driver returned error]` |                         |                |
| 3'  |   `HumidityTempDriver` | `SensorService`      | return | error code                 |                           |                         | SA-080         |
| 3'' |   `SensorService`      | `Logger`             | async  | log read failure           |                           |                         | SA-080         |
| 3'''|   `SensorService`      | `SensorService`      | self   | mark sample invalid        |                           |                         | SA-0E1, NF-208 |
| —   |                        |                      |        |                            | **end alt**               |                         |                |
| 4   | `SensorService`        | `BarometerDriver`    | sync   | read pressure              |                           |                         | SA-070         |
| —   |                        |                      |        |                            | **alt**                   |                         | SA-080, SA-0E1 |
| —   |                        |                      |        |                            | `[driver returned ok]`    |                         |                |
| 5   |   `BarometerDriver`    | `SensorService`      | return | pressure                   |                           |                         | SA-070         |
| —   |                        |                      |        |                            | `[driver returned error]` |                         |                |
| 5'  |   `BarometerDriver`    | `SensorService`      | return | error code                 |                           |                         | SA-080         |
| 5'' |   `SensorService`      | `Logger`             | async  | log read failure           |                           |                         | SA-080         |
| 5'''|   `SensorService`      | `SensorService`      | self   | mark sample invalid        |                           |                         | SA-0E1, NF-208 |
| —   |                        |                      |        |                            | **end alt**               |                         |                |
| 6   | `SensorService`        | `SensorService`      | self   | stamp with timestamp       |                           |                         | SA-100         |
| 7   | `SensorService`        | `SensorService`      | self   | validate range             |                           |                         | SA-120         |
| 8   | `SensorService`        | `SensorService`      | self   | clamp to range             |                           |                         | SA-130         |
| 9   | `SensorService`        | `SensorService`      | self   | apply low-pass filter      |                           |                         | SA-140         |
| 10  | `SensorService`        | `SensorService`      | self   | update ISensorReadings cache |                         |                         | SA-150         |

The `SensorService` activation bar spans messages 2 through 10. Annotate it with the duration constraint `{≤ 100 ms}` per REQ-NF-100. Even on the rainy operands, messages `3'`–`3'''` and `5'`–`5'''` complete within the 100 ms window, and the publish step (10) runs with invalid samples flagged but is never skipped (REQ-SA-060).

**UML note required on the diagram:** *"Timestamp is uptime-based and flagged unsynchronised when the RTC is not synced (REQ-NF-212). Validation, clamping, and filtering are skipped for samples marked invalid; the invalid flag passes through to publish (REQ-SA-150, REQ-SA-160)."*

### 6.4 Traceability summary

| Element                        | Traces to                                                                |
|--------------------------------|--------------------------------------------------------------------------|
| 1 Hz trigger                   | REQ-NF-110, UC-07 step 1                                                 |
| Reads to two drivers           | REQ-SA-070, UC-07 step 2                                                 |
| Both `alt` fragments           | REQ-SA-080, REQ-SA-0E1, UC-01 E1                                         |
| Self-messages 6–10             | REQ-SA-100, -120, -130, -140, -150                                       |
| Activation duration constraint | REQ-NF-100                                                               |
| Cycle-continues guarantee      | REQ-SA-060 (encoded structurally — failures don't `break` out)           |

### 6.5 Decisions specific to SD-01

| #       | Decision                                                  | Rationale                                                                                                  |
|---------|-----------------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| SD01-D1 | Both driver calls wrapped in symmetric `alt` (not `opt`)  | Surfacing the sunny operand explicitly makes the diagram readable in either direction; `opt` hides success |
| SD01-D2 | Logger appears as a lifeline (P4 exception)               | Logging *is* the rainy behaviour; eliding it would erase the SRS-required action                           |
| SD01-D3 | I2cDriver and per-driver internal mechanics not shown     | Driver-to-driver traffic is LLD scope; HLD lifelines are component-level                                   |

---


## 7. SD-02 — Modbus polling cycle (Gateway ↔ Field Device)

**Owner:** `ModbusPoller` (Application, GW). Protocol-frame work delegated to `ModbusMaster` middleware.
**Trigger:** Periodic poll timer at the configured polling rate (default 1 Hz, REQ-NF-110).
**Use cases:** UC-07 (gateway view), UC-10.
**Scope:** One polling round trip — request, transmit on bus, slave response, master delivery to poller. Includes timeout, retry, and link-state hysteresis events.

This sequence handles **runtime behaviour after link establishment** (which is SD-00b's responsibility). The `node_offline` and `link_up` events are emitted to feed the Modbus Master state machine (`state-machines.md` §7.5).

### 7.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                                  | Label                                                  |
|-------------|--------------------------------------------------------------------------------|--------------------------------------------------------|
| REQ-MB-010  | Read sensor data from FD via Modbus read input registers                       | IMPLIES MESSAGE (read input registers, FC=04)          |
| REQ-MB-030  | RTU at 9600/8N1                                                                | NO SEQUENCE IMPLICATION (config; appears as bus annotation) |
| REQ-MB-040  | Function codes 03/04/06/16                                                     | IMPLIES MESSAGE (FC=04 in this flow)                   |
| REQ-MB-050  | Timeout 200 ms                                                                 | IMPLIES TIMING CONSTRAINT (response leg)               |
| REQ-MB-060  | Retry up to 3 times                                                            | IMPLIES FRAGMENT(loop)                                 |
| REQ-MB-100  | Address multiple field devices by unique slave address                         | IMPLIES MESSAGE PAYLOAD (request carries slave address)|
| REQ-NF-103  | Declare failure after 3 consecutive unanswered requests                        | IMPLIES GUARD on `node_offline` event                  |
| REQ-NF-104  | Declare restored after 3 consecutive successful responses                      | IMPLIES GUARD on `link_up` event                       |
| REQ-NF-105  | Failed if no response within 200 ms                                            | IMPLIES TIMING CONSTRAINT + IMPLIES FRAGMENT(alt)      |
| REQ-NF-201  | Recover Modbus communication automatically after a transient bus error         | IMPLIES FRAGMENT(loop) — same loop as MB-060           |
| REQ-NF-215  | Report "node offline" after 3 consecutive failed polls                         | IMPLIES MESSAGE (`node_offline` event)                 |
| REQ-MB-000  | Update Modbus register when new measurement is available                       | NO SEQUENCE IMPLICATION here (covered by SD-01 publish step) |

### 7.2 Step 2 — Participants

| # | Lifeline                       | Stereotype    | Board | Role in this sequence                                          |
|---|--------------------------------|---------------|-------|----------------------------------------------------------------|
| 1 | (poll timer)                   | «timer»       | GW    | Periodic event source                                          |
| 2 | `ModbusPoller`                 | «application» | GW    | Schedules polls; tracks per-slave link-state hysteresis        |
| 3 | `ModbusMaster`                 | «middleware»  | GW    | Encodes Modbus frames; manages timeout                         |
| 4 | `ModbusUartDriver` (GW)        | «driver»      | GW    | Transmits and receives bytes on RS-485                         |
| 5 | `ModbusUartDriver` (FD)        | «driver»      | FD    | Receives and transmits bytes on RS-485                         |
| 6 | `ModbusSlave`                  | «middleware»  | FD    | Decodes frames; dispatches to register map                     |
| 7 | `IModbusRegisterMap`           | «application» | FD    | Reads input registers from cached sensor data; DIP interface — concrete binding `ModbusRegisterMap` specified at LLD |

Insert a vertical bus boundary between participants 4 and 5. Annotate `RS-485 / Modbus RTU @ 9600 8N1` (REQ-MB-030).

### 7.3 Step 3 — Message table

| #   | From                        | To                      | Type        | Message text                          | Guard / fragment                   | Timing                          | Trace          |
|-----|-----------------------------|-------------------------|-------------|---------------------------------------|------------------------------------|---------------------------------|----------------|
| 1   | (poll timer)                | `ModbusPoller`          | async       | tick                                  |                                    | period 1 s                      | NF-110         |
| 2   | `ModbusPoller`              | `ModbusMaster`          | sync        | send request (slave addr, FC 04, regs)|                                    |                                 | MB-010, MB-100 |
| —   |                             |                         |             |                                       | **loop(1..3)** `[no success yet]`  |                                 | MB-060, NF-201 |
| 3   |   `ModbusMaster`            | `ModbusUartDriver` (GW) | sync        | write frame                           |                                    |                                 | MB-030, MB-040 |
| —   |                             |                         |             |                                       | **alt**                            |                                 | NF-105         |
| —   |                             |                         |             |                                       | `[response received within 200 ms]`|                                 |                |
| 4   |     `ModbusUartDriver` (GW) | `ModbusUartDriver` (FD) | async (bus) | RTU frame on bus                      |                                    | round trip {≤ 200 ms}: NF-105   | MB-030         |
| 5   |     `ModbusUartDriver` (FD) | `ModbusSlave`           | async       | frame received                        |                                    |                                 | MB-030         |
| 6   |     `ModbusSlave`           | `IModbusRegisterMap`    | sync        | read input registers                  |                                    |                                 | MB-040         |
| 7   |     `IModbusRegisterMap`    | `ModbusSlave`           | return      | register values                       |                                    |                                 | SA-150         |
| 8   |     `ModbusSlave`           | `ModbusUartDriver` (FD) | sync        | write response frame                  |                                    |                                 | MB-030         |
| 9   |     `ModbusUartDriver` (FD) | `ModbusUartDriver` (GW) | async (bus) | RTU response frame on bus             |                                    |                                 | MB-030         |
| 10  |     `ModbusUartDriver` (GW) | `ModbusMaster`          | async       | frame received                        |                                    |                                 | MB-030         |
| 11  |     `ModbusMaster`          | `ModbusPoller`          | return      | response (register values)            |                                    |                                 | MB-010         |
| —   |                             |                         |             |                                       | `[no response within 200 ms]`      |                                 |                |
| 4'  |     `ModbusMaster`          | `ModbusMaster`          | self        | timeout expired                       |                                    |                                 | MB-050, NF-105 |
| 5'  |     `ModbusMaster`          | `ModbusPoller`          | return      | failure                                |                                    |                                 | MB-050         |
| —   |                             |                         |             |                                       | **end alt**                        |                                 |                |
| —   |                             |                         |             |                                       | **end loop**                       |                                 |                |
| 12  | `ModbusPoller`              | `ModbusPoller`          | self        | record poll outcome (per slave)       |                                    |                                 | NF-103, NF-104 |
| 13  | `ModbusPoller`              | `ModbusPoller`          | self        | emit link_up event (slave id)         | `[3rd consecutive success]`        |                                 | NF-104         |
| 14  | `ModbusPoller`              | `ModbusPoller`          | self        | emit node_offline event (slave id)    | `[3rd consecutive failure]`        |                                 | NF-103, NF-215 |

The duration constraint `{≤ 200 ms}` brackets messages 4 through 11 inside the response-received operand. The retry loop (REQ-MB-060) re-issues the request up to three times; on success the loop exits early via the `[no success yet]` guard.

**UML note 1 (events):** *"`link_up` and `node_offline` events are mutually exclusive — at most one fires per poll outcome. Both carry the slave ID (per-slave link state, decision SD00-D5). Subscribed by `HealthMonitor` and `CloudPublisher` (elided per P4); see state-machines.md §7.5 for the link-state machine."*

**UML note 2 (Modbus exception responses):** *"A valid response with the exception bit set (function codes 0x81/0x84/0x86/0x90; exception codes 0x01/0x02/0x03 per REQ-MB-040) is decoded inside `ModbusMaster` and surfaced to `ModbusPoller` as a `failure` return — same path as the timeout operand. The exception code is logged. Internal exception decoding is not shown at HLD level (LLD)."*

**UML note 3 (FD reboot recovery, A5):** *"FD recovery from reboot is automatic. If the slave reboots mid-operation, polls during its downtime time out (3 → `node_offline`); polls after it returns succeed (3 → `link_up`). No special recovery sequence is required at the master."*

**UML note 4 (frame structure from profile):** *"The request frame's slave address and target register addresses come from the bound device profile in `DeviceProfileRegistry` (see SD-00b). At HLD level, profile lookup is implicit; at LLD it becomes a lookup at request-build time."*

**UML note 5 (`IModbusRegisterMap` DIP interface):** *"`ModbusSlave` (middleware) must not call `ModbusRegisterMap` (application) directly — this would violate P1 strict directional layering. The `IModbusRegisterMap` interface is declared in the Application layer; `ModbusSlave` depends on it by injection. The concrete binding (`ModbusRegisterMap`) is specified in LLD."*

### 7.4 Traceability summary

| Element                                  | Traces to                       |
|------------------------------------------|---------------------------------|
| Request frame structure (FC, slave addr) | REQ-MB-010, MB-040, MB-100      |
| Bus traversal (both directions)          | REQ-MB-030                      |
| 200 ms response window                   | REQ-NF-105, REQ-MB-050          |
| Retry loop                               | REQ-MB-060, REQ-NF-201          |
| Hysteresis on link state                 | REQ-NF-103, REQ-NF-104          |
| node_offline event publication           | REQ-NF-215                      |

### 7.5 Decisions specific to SD-02

| # | Decision | Rationale |
|---|----------|-----------|
| SD02-D1 | Single diagram (not split into SD-02a / SD-02b) | Total rows fit within P9's 25-message budget |
| SD02-D2 | `loop(1..3)` outside the `alt` | The retry retries the *whole* round trip; the `alt` discriminates each attempt's outcome |
| SD02-D3 | `node_offline` consumed by HealthMonitor and CloudPublisher; both elided per P4, named in a UML note | Drawing them would add two lifelines for one event in an already-busy diagram |
| SD02-D4 | Modbus exception responses surfaced as `failure` (same path as timeout); decoding inside ModbusMaster | Differentiating exception vs timeout is LLD detail; the architectural response (count toward hysteresis, log, retry) is identical |
| SD02-D5 | `link_up` and `node_offline` shown as `ModbusPoller` self-messages with mutual-exclusion guards, carrying the slave ID | Events feed the link-state machine (consumed via pull, P7); per-slave granularity supports SD00-D5 |

---

## 8. SD-03 — Cloud telemetry publish (sunny path)

**Owner:** `CloudPublisher` (Application, GW).
**Trigger:** Two periodic timers — telemetry every 60 s (REQ-NF-111), health every 600 s (REQ-NF-112).
**Use cases:** UC-05 (telemetry), UC-06 (health).
**Scope:** Steady-state, connection up, buffer empty, NTP synchronised. **All rainy paths (disconnect, buffer, reconnect, drain) live in SD-04.**

**Why no rainy paths here?** QoS 0 is fire-and-forget per REQ-NF-206 — the protocol has no acknowledgement and no retry. Connection-level failures are owned by SD-04 (per the Cloud Connectivity sub-machine in `state-machines.md` §7.3). Adding rainy paths to SD-03 would either be vacuous (QoS 0 has nothing to fail at the application level) or duplicate SD-04. See decision SD03-D1.

The telemetry leg and the health leg are split into two sub-diagrams (decision SD03-D2): different periods, different producers, different topics, different payloads.

### 8.1 Step 1 — SRS to message mapping (shared across both legs)

| SRS         | Text fragment                                                                  | Label                                                  |
|-------------|--------------------------------------------------------------------------------|--------------------------------------------------------|
| REQ-CC-000  | Publish periodically the most recent sensor measurements with timestamps        | IMPLIES MESSAGE (telemetry leg only) + IMPLIES PARTICIPANT (`SensorService`) |
| REQ-CC-010  | Publish the most recent device health metrics                                  | IMPLIES MESSAGE (health leg only) + IMPLIES PARTICIPANT (`HealthMonitor`)    |
| REQ-CC-030  | Publish sensor telemetry at a configurable interval                            | IMPLIES TIMING CONSTRAINT on telemetry trigger         |
| REQ-CC-040  | Publish health data at a configurable interval                                 | IMPLIES TIMING CONSTRAINT on health trigger            |
| REQ-CC-050  | Connect to AWS IoT Core using MQTT over TLS with X.509 certificates            | IMPLIES MESSAGE (TLS-protected transport via WifiDriver) |
| REQ-CC-070  | Use JSON format for all MQTT payloads                                          | IMPLIES MESSAGE (self: serialise to JSON)              |
| REQ-CC-071  | Include schema version identifier in all MQTT payloads                         | IMPLIES MESSAGE PAYLOAD (UML note)                     |
| REQ-CC-080  | Use separate MQTT topics for telemetry, alarms, and device health              | IMPLIES MESSAGE (each leg targets a different topic)   |
| REQ-CC-090  | Include device serial number in all cloud health messages                      | IMPLIES MESSAGE PAYLOAD (UML note on health leg)       |
| REQ-NF-106  | Queue MQTT message for publication within 200 ms of data being ready           | IMPLIES TIMING CONSTRAINT on serialise→publish span    |
| REQ-NF-111  | Publish sensor measurements each 60 seconds                                    | IMPLIES TIMING CONSTRAINT on telemetry timer           |
| REQ-NF-112  | Publish health data each 10 minutes                                            | IMPLIES TIMING CONSTRAINT on health timer              |
| REQ-NF-206  | Use MQTT QoS 0 for sensor telemetry                                            | IMPLIES MESSAGE (no PUBACK at QoS 0 — absence drawn as the lack of a return arrow) |
| REQ-NF-216  | Use MQTT QoS 0 for device health data                                          | IMPLIES MESSAGE (same; no PUBACK)                      |

### 8.2 Step 2 — Participants (shared across both legs)

| # | Lifeline           | Stereotype    | Board | Role                                                         |
|---|--------------------|---------------|-------|--------------------------------------------------------------|
| 1 | (timer)            | «timer»       | GW    | Periodic event source (60 s for telemetry, 600 s for health) |
| 2 | `CloudPublisher`   | «application» | GW    | Pulls latest data, serialises, hands off to MqttClient       |
| 3 | `SensorService` *or* `HealthMonitor` | «application» | GW | Provides `ISensorReadings` *or* `IHealthSnapshot` (pull, P7) |
| 4 | `MqttClient`       | «middleware»  | GW    | Encodes MQTT, manages QoS, delegates transport               |
| 5 | `WifiDriver`       | «driver»      | GW    | TLS-protected TCP transport over WiFi                        |
| 6 | `AWS IoT Core`     | «cloud»       | Cloud | MQTT broker                                                  |

`StoreAndForward` does not appear — connection is up, buffer is empty (precondition).

### 8.3 SD-03a — Telemetry leg (60 s)

| #  | From               | To                | Type   | Message text                          | Guard / fragment | Timing                            | Trace          |
|----|--------------------|-------------------|--------|---------------------------------------|------------------|-----------------------------------|----------------|
| 1  | (telemetry timer)  | `CloudPublisher`  | async  | tick                                  |                  | period 60 s                       | NF-111         |
| 2  | `CloudPublisher`   | `SensorService`   | sync   | get latest readings                   |                  |                                   | CC-000         |
| 3  | `SensorService`    | `CloudPublisher`  | return | readings                              |                  |                                   | CC-000         |
| 4  | `CloudPublisher`   | `CloudPublisher`  | self   | serialise to JSON                     |                  | publish queued {≤ 200 ms}: NF-106 | CC-070, CC-071 |
| 5  | `CloudPublisher`   | `MqttClient`      | sync   | publish QoS 0 on telemetry topic      |                  |                                   | CC-030, CC-080 |
| 6  | `MqttClient`       | `WifiDriver`      | sync   | send TLS payload                      |                  |                                   | CC-050         |
| 7  | `WifiDriver`       | `AWS IoT Core`    | async  | MQTT PUBLISH (telemetry topic)        |                  |                                   | CC-000, NF-206 |

The duration constraint `{≤ 200 ms}` brackets messages 4 through 7 (data ready → publish queued on the wire) per REQ-NF-106. See §13 Q-M7 for an open question on the exact anchor of REQ-NF-106.

**UML note 1 (payload):** *"JSON payload includes: schema version (REQ-CC-071), measurements with timestamps (REQ-CC-000), per-sensor invalid flag where applicable (REQ-SA-160). The full payload schema is documented in the Modbus / MQTT register map (LLD)."*

**UML note 2 (no PUBACK):** *"QoS 0 — no PUBACK from broker (REQ-NF-206). Absence of a return arrow from AWS IoT Core is intentional, not omission."*

**UML note 3 (transport):** *"TLS handshake is established at connection setup (see SD-04b reconnect flow) and not redrawn here. WifiDriver is a passthrough at HLD level — bytes go through TLS-protected TCP."*

### 8.4 SD-03b — Health leg (600 s)

| #  | From            | To                | Type   | Message text                       | Guard / fragment | Timing                            | Trace                  |
|----|-----------------|-------------------|--------|------------------------------------|------------------|-----------------------------------|------------------------|
| 1  | (health timer)  | `CloudPublisher`  | async  | tick                               |                  | period 600 s                      | NF-112                 |
| 2  | `CloudPublisher`| `HealthMonitor`   | sync   | get latest health snapshot         |                  |                                   | CC-010                 |
| 3  | `HealthMonitor` | `CloudPublisher`  | return | health snapshot                    |                  |                                   | CC-010                 |
| 4  | `CloudPublisher`| `CloudPublisher`  | self   | serialise to JSON                  |                  | publish queued {≤ 200 ms}: NF-106 | CC-070, CC-071, CC-090 |
| 5  | `CloudPublisher`| `MqttClient`      | sync   | publish QoS 0 on health topic      |                  |                                   | CC-040, CC-080         |
| 6  | `MqttClient`    | `WifiDriver`      | sync   | send TLS payload                   |                  |                                   | CC-050                 |
| 7  | `WifiDriver`    | `AWS IoT Core`    | async  | MQTT PUBLISH (health topic)        |                  |                                   | CC-010, NF-216         |

**UML note 1 (payload):** *"JSON payload includes: schema version (REQ-CC-071), device serial number (REQ-CC-090), full health metric set per REQ-CC-010 (stack high watermarks, free heap, CPU load, WiFi RSSI, reconnection count, MQTT failure count, Modbus CRC/timeout/success counts, uptime, MCU temperature, buffer occupancy)."*

**UML note 2 (Metric Producer Pattern, P5):** *"Health metrics are aggregated continuously by `HealthMonitor` via the Metric Producer Pattern (see components.md §6 and architecture-principles.md P5): middleware components push event-based metrics via `IHealthReport`; `HealthMonitor` polls counter-based metrics via `IXxxStats` interfaces. Only the snapshot pull at publish time appears in this sequence."*

**UML note 3 (no PUBACK):** *"QoS 0 — no PUBACK from broker (REQ-NF-216). Same convention as SD-03a."*

### 8.5 Traceability summary (both legs)

| Element                              | Traces to                              |
|--------------------------------------|----------------------------------------|
| Periodic triggers                    | REQ-NF-111 (telemetry), REQ-NF-112 (health) |
| Pull from producer                   | REQ-CC-000 (telemetry), REQ-CC-010 (health), P7 |
| Serialise to JSON                    | REQ-CC-070, REQ-CC-071, REQ-CC-090 (health) |
| Topic per category                   | REQ-CC-080                             |
| Transport via TLS                    | REQ-CC-050                             |
| QoS 0                                | REQ-NF-206 (telemetry), REQ-NF-216 (health) |
| Queued for publication ≤ 200 ms      | REQ-NF-106                             |

### 8.6 Decisions specific to SD-03

| # | Decision | Rationale |
|---|----------|-----------|
| SD03-D1 | SD-03 covers only the steady-state happy path; rainy paths live in SD-04 | QoS 0 has no protocol-level error path; connection-level failures map cleanly to the Disconnected state in state-machines.md §7.3 |
| SD03-D2 | Two sub-diagrams (SD-03a, SD-03b), not one with `par` | Different periods, producers, topics, and payloads. `par` would imply they fire together |
| SD03-D3 | `WifiDriver` shown as a passthrough lifeline | Symmetric with SD-02's treatment of `ModbusUartDriver`; makes layering transparent |
| SD03-D4 | TLS handshake elided into a UML note | Handshake happens once at connection setup (SD-04b); redrawing would obscure the signal |
| SD03-D5 | QoS 0 shown by *absence* of PUBACK return arrow, with a UML note | Drawing a non-existent message would be wrong; absence is the architectural fact |

---

## 9. SD-04 — Store-and-forward (rainy lifecycle)

**Owners:** `CloudPublisher`, `StoreAndForward` (Application, GW); `CircularFlashLog` (Middleware, GW).
**Use cases:** UC-10, UC-11, UC-12 exception flows.
**Scope:** What happens when WiFi or the broker becomes unreachable, and how queued data drains when connectivity returns. The connected publish path itself is **not** redrawn here — SD-03a and SD-03b own it. SD-04 is the runtime view of the Cloud Connectivity sub-machine's transitions (`state-machines.md` §7.3).

Two sub-diagrams: SD-04a (disconnect detection → buffering, including the buffer-full drop-oldest case and the ongoing reconnect loop), SD-04b (reconnect → drain in chronological order).

### 9.1 Step 1 — SRS to message mapping (shared)

| SRS         | Text fragment                                                                       | Label                                                  |
|-------------|-------------------------------------------------------------------------------------|--------------------------------------------------------|
| REQ-BF-000  | Buffer all outbound cloud messages in non-volatile storage when internet is lost    | IMPLIES MESSAGE (enqueue) + IMPLIES PARTICIPANT (`StoreAndForward`, `CircularFlashLog`) |
| REQ-BF-010  | Publish buffered messages in chronological order when connectivity is restored      | IMPLIES FRAGMENT(loop) + IMPLIES GUARD (oldest first)  |
| REQ-BF-020  | Discard oldest buffered messages when buffer reaches max capacity                   | IMPLIES FRAGMENT(alt) inside enqueue (drop-oldest)     |
| REQ-CC-050  | Reconnect uses MQTT/TLS/X.509                                                       | IMPLIES MESSAGE (TLS handshake on reconnect)           |
| REQ-CC-060  | Reconnect automatically if MQTT connection is lost                                  | IMPLIES MESSAGE + IMPLIES FRAGMENT(loop) on reconnect attempt |
| REQ-NF-200  | Continue local sensor acquisition and alarm evaluation when internet is lost        | NO SEQUENCE IMPLICATION here (UML note)                |
| REQ-NF-209  | Reconnect attempt at 1 Hz                                                           | IMPLIES TIMING CONSTRAINT on reconnect loop            |

### 9.2 Step 2 — Participants (shared)

| # | Lifeline             | Stereotype    | Board | Role                                                              |
|---|----------------------|---------------|-------|-------------------------------------------------------------------|
| 1 | (publish trigger)    | «timer»       | GW    | Telemetry / health / alarm trigger (whatever fires while disconnected) |
| 2 | `CloudPublisher`     | «application» | GW    | Decides connected vs disconnected; routes message                 |
| 3 | `StoreAndForward`    | «application» | GW    | Buffer manager (enqueue / dequeue / drop-oldest policy)           |
| 4 | `MqttClient`         | «middleware»  | GW    | Reconnect attempts; publish on connection                         |
| 5 | `CircularFlashLog`   | «middleware»  | GW    | Persists buffered messages on QSPI flash                          |
| 6 | `WifiDriver`         | «driver»      | GW    | TLS transport                                                     |
| 7 | `AWS IoT Core`       | «cloud»       | Cloud | Broker                                                            |

### 9.3 SD-04a — Disconnect detection → buffering

| #   | From               | To                | Type   | Message text                         | Guard / fragment                       | Timing       | Trace          |
|-----|--------------------|-------------------|--------|--------------------------------------|----------------------------------------|--------------|----------------|
| 1   | `MqttClient`       | `CloudPublisher`  | async  | mqtt_disconnected event              |                                        |              | CC-060         |
| 2   | `CloudPublisher`   | `CloudPublisher`  | self   | mark connection state disconnected   |                                        |              | CC-060         |
| —   |                    |                   |        |                                      | **par**                                |              |                |
| —   |                    |                   |        |                                      | **operand: enqueue new outbound messages** |          |                |
| —   |                    |                   |        |                                      | **loop** `[disconnected]`              |              | BF-000         |
| 3   |   (publish trigger)| `CloudPublisher`  | async  | tick (telemetry / health / alarm)    |                                        |              |                |
| 4   |   `CloudPublisher` | `StoreAndForward` | sync   | enqueue message                      |                                        |              | BF-000         |
| —   |                    |                   |        |                                      | **alt**                                |              | BF-020         |
| —   |                    |                   |        |                                      | `[buffer has space]`                   |              |                |
| 5   |     `StoreAndForward` | `CircularFlashLog` | sync | append entry                       |                                        |              | BF-000         |
| 6   |     `CircularFlashLog` | `StoreAndForward` | return | ok                                |                                        |              |                |
| —   |                    |                   |        |                                      | `[buffer full]`                        |              |                |
| 5'  |     `StoreAndForward` | `CircularFlashLog` | sync | drop oldest entry                  |                                        |              | BF-020         |
| 6'  |     `StoreAndForward` | `CircularFlashLog` | sync | append entry                       |                                        |              | BF-020         |
| —   |                    |                   |        |                                      | **end alt**                            |              |                |
| —   |                    |                   |        |                                      | **end loop**                           |              |                |
| —   |                    |                   |        |                                      | **operand: reconnect attempts**        |              |                |
| —   |                    |                   |        |                                      | **loop** `[disconnected]`              |              | CC-060, NF-209 |
| 7   |   `MqttClient`     | `MqttClient`      | self   | reconnect attempt                    |                                        | period 1 s   | NF-209         |
| 8   |   `MqttClient`     | `WifiDriver`      | sync   | open TCP / TLS handshake             |                                        |              | CC-050         |
| —   |                    |                   |        |                                      | **alt**                                |              |                |
| —   |                    |                   |        |                                      | `[handshake ok]`                       |              |                |
| 9   |     `WifiDriver`   | `MqttClient`      | return | connected                            |                                        |              | CC-050         |
| 10  |     `MqttClient`   | `MqttClient`      | self   | exit reconnect loop                  |                                        |              |                |
| —   |                    |                   |        |                                      | `[handshake failed]`                   |              |                |
| 9'  |     `WifiDriver`   | `MqttClient`      | return | failure (continue loop)              |                                        |              | CC-050         |
| —   |                    |                   |        |                                      | **end alt**                            |              |                |
| —   |                    |                   |        |                                      | **end loop**                           |              |                |
| —   |                    |                   |        |                                      | **end par**                            |              |                |

The `par` fragment captures the architectural fact that the two activities — buffering new messages and attempting reconnection — proceed independently while disconnected.

**UML note 1 (local acquisition unaffected):** *"SD-01 (sensor acquisition) and the alarm-evaluation step inside SD-05 continue to run on both boards while the gateway is disconnected. The cloud-disconnected state has no effect on local data paths (REQ-NF-200)."*

**UML note 2 (shared buffer state, A3):** *"Both `par` operands share `StoreAndForward.buffer` state. Buffer fills during the disconnect window can trigger drop-oldest (REQ-BF-020) before reconnection succeeds. The `par` does not imply isolation — it expresses concurrency, not data independence."*

**UML note 3 (reconnect handoff to drain):** *"On successful reconnection the loop exits and execution transfers to SD-04b (drain). The transition is owned by the Cloud Connectivity sub-machine in state-machines.md §7.3."*

### 9.4 SD-04b — Reconnect → drain

| #   | From               | To                | Type   | Message text                              | Guard / fragment                  | Timing       | Trace          |
|-----|--------------------|-------------------|--------|-------------------------------------------|-----------------------------------|--------------|----------------|
| 1   | `MqttClient`       | `CloudPublisher`  | async  | mqtt_connected event                      |                                   |              | CC-060         |
| 2   | `CloudPublisher`   | `CloudPublisher`  | self   | mark connection state connected           |                                   |              | CC-060         |
| 3   | `CloudPublisher`   | `StoreAndForward` | sync   | get buffer status                         |                                   |              |                |
| 4   | `StoreAndForward`  | `CloudPublisher`  | return | occupancy                                 |                                   |              |                |
| —   |                    |                   |        |                                           | **opt** `[buffer not empty]`      |              | BF-010         |
| —   |                    |                   |        |                                           | **loop** `[buffer not empty]`     |              | BF-010         |
| 5   |   `CloudPublisher` | `StoreAndForward` | sync   | dequeue oldest                            |                                   |              | BF-010         |
| 6   |   `StoreAndForward`| `CloudPublisher`  | return | message (oldest)                          |                                   |              | BF-010         |
| 7   |   `CloudPublisher` | `MqttClient`      | sync   | publish (preserves original QoS and topic)|                                   |              | BF-010, CC-080 |
| 8   |   `MqttClient`     | `WifiDriver`      | sync   | send TLS payload                          |                                   |              | CC-050         |
| 9   |   `WifiDriver`     | `AWS IoT Core`    | async  | MQTT PUBLISH                              |                                   |              | BF-010         |
| —   |                    |                   |        |                                           | **opt** `[QoS 1 message]`         |              |                |
| 10  |     `AWS IoT Core` | `MqttClient`      | return | PUBACK                                    |                                   |              | NF-207         |
| —   |                    |                   |        |                                           | **end opt**                       |              |                |
| 11  |   `CloudPublisher` | `StoreAndForward` | sync   | confirm published                         |                                   |              | BF-010         |
| 12  |   `StoreAndForward`| `CloudPublisher`  | return | ok (entry removed)                        |                                   |              | BF-010         |
| —   |                    |                   |        |                                           | **end loop**                      |              |                |
| —   |                    |                   |        |                                           | **end opt**                       |              |                |
| 13  | `CloudPublisher`   | `CloudPublisher`  | self   | resume normal publishing                  |                                   |              |                |

**UML note 1 (chronological order):** *"Chronological order is guaranteed structurally — `dequeue oldest` always returns the entry with the lowest timestamp. The buffer is FIFO."*

**UML note 2 (live messages during drain):** *"New telemetry / health / alarm messages arriving during drain are queued behind the backlog (enqueued via SD-04a's loop, which runs continuously). Live messages do not jump the queue. This preserves chronological order at the cost of latency for live data — acceptable per REQ-NF-200's framing of cloud connectivity as best-effort."*

**UML note 3 (QoS preservation):** *"Each buffered entry carries its original QoS and topic. QoS 1 alarms (REQ-NF-207) re-publish at QoS 1 and require PUBACK; QoS 0 telemetry / health re-publish at QoS 0 with no acknowledgement. The `opt [QoS 1 message]` fragment captures this."*

### 9.5 Traceability summary

| Element                             | Traces to                              |
|-------------------------------------|----------------------------------------|
| Disconnect event from MqttClient    | REQ-CC-060                             |
| Buffer to flash                     | REQ-BF-000                             |
| Drop oldest on full                 | REQ-BF-020                             |
| 1 Hz reconnect attempts             | REQ-NF-209, REQ-CC-060                 |
| TLS handshake on reconnect          | REQ-CC-050                             |
| Drain in chronological order        | REQ-BF-010                             |
| Local acquisition unaffected (note) | REQ-NF-200                             |
| QoS 1 PUBACK preservation on drain  | REQ-NF-207                             |

### 9.6 Decisions specific to SD-04

| # | Decision | Rationale |
|---|----------|-----------|
| SD04-D1 | Two sub-diagrams (disconnect+buffer, reconnect+drain), no separate "Phase A" diagram | Phase A is just SD-03 — referencing it via `ref` from inside SD-04 would add an empty diagram |
| SD04-D2 | Live messages during drain queue behind the backlog, not interleaved | Preserves REQ-BF-010 chronological order; live latency loss acceptable per REQ-NF-200 |
| SD04-D3 | `par` fragment captures buffering and reconnect as concurrent activities | Both proceed independently; serial representation would imply false ordering |
| SD04-D4 | Drop-oldest is encoded as an `alt` inside the enqueue path | Buffer-full is a normal operating mode (REQ-BF-020), not an error |
| SD04-D5 | TLS handshake drawn explicitly here (not on SD-03) | This is the connection-establishment sequence — handshake belongs where the connection is set up |
| SD04-D6 | QoS 1 PUBACK on drain encoded as `opt [QoS 1 message]` | Preserves the QoS contract per buffered entry; drain is QoS-aware, not a flat replay |

---

## 10. SD-05 — Alarm propagation (Field Device → Gateway → Cloud)

**Owners:** `AlarmService` (Application, FD); `AlarmService` (Application, GW); `CloudPublisher` (Application, GW).
**Use cases:** UC-08, UC-09, UC-12.
**Scope:** From the moment the field device's `SensorService` publishes a fresh reading that crosses a threshold to the moment AWS IoT Core acknowledges the alarm message — or the alarm is buffered if the cloud is disconnected.

### 10.1 Step 1 — SRS to message mapping

| SRS          | Text fragment                                                                       | Label                                            |
|--------------|-------------------------------------------------------------------------------------|--------------------------------------------------|
| REQ-AM-000   | Compare sensor measurements with configured alarm thresholds                        | IMPLIES MESSAGE (threshold check)                |
| REQ-AM-020   | Trigger an alarm notification if measurement is out of range                        | IMPLIES FRAGMENT(opt: out-of-range)              |
| REQ-AM-030   | Send alarm notification to AWS IoT Core                                             | IMPLIES MESSAGE (cloud publish)                  |
| REQ-AM-040   | Include sensor ID, type, value, threshold, timestamp, device ID in alarm            | IMPLIES MESSAGE PAYLOAD (UML note)               |
| REQ-AM-011   | Apply hysteresis when clearing alarms                                               | NO SEQUENCE IMPLICATION here — clearance flow deferred to LLD |
| REQ-NF-101   | Detect alarm within one polling cycle                                               | IMPLIES TIMING CONSTRAINT (FD activation)        |
| REQ-NF-113   | Publish alarm to AWS within 500 ms of detection                                     | IMPLIES TIMING CONSTRAINT (FD-detection→PUBACK)  |
| REQ-NF-207   | Use MQTT QoS 1 for publishing alarm notifications                                   | IMPLIES MESSAGE (PUBACK return)                  |
| REQ-MB-000   | Update Modbus register when new measurement is available                            | IMPLIES MESSAGE (FD updates alarm flag in register map) |
| REQ-CC-050   | MQTT over TLS                                                                       | IMPLIES MESSAGE (transport via WifiDriver)       |
| REQ-CC-080   | Separate MQTT topics for telemetry, alarms, device health                           | IMPLIES MESSAGE (alarm topic)                    |
| REQ-NF-200   | Continue local acquisition and alarm evaluation when internet is lost               | IMPLIES GUARD on cloud-publish branch            |
| REQ-BF-000   | Buffer outbound cloud messages when internet is lost                                | IMPLIES `ref` to SD-04a in rainy operand         |

### 10.2 Step 2 — Participants

| #  | Lifeline                  | Stereotype    | Board | Role                                                            |
|----|---------------------------|---------------|-------|-----------------------------------------------------------------|
| 1  | `SensorService` (FD)      | «application» | FD    | Source of the fresh reading (output of SD-01)                   |
| 2  | `AlarmService` (FD)       | «application» | FD    | Threshold check; raises alarm flag in register map              |
| 3  | `ModbusRegisterMap` (FD)  | «application» | FD    | Holds alarm flags as readable input registers                   |
| 4  | `Logger` (FD)             | «middleware»  | FD    | Records alarm-raise event (P4 exception)                        |
| 5  | `ModbusPoller` (GW)       | «application» | GW    | Reads input registers including alarm flags (uses SD-02)        |
| 6  | `AlarmService` (GW)       | «application» | GW    | Detects alarm flag transition; assembles alarm message          |
| 7  | `CloudPublisher` (GW)     | «application» | GW    | Publishes the alarm message at QoS 1, or routes to buffer       |
| 8  | `MqttClient` (GW)         | «middleware»  | GW    | Manages QoS 1 acknowledgement                                   |
| 9  | `WifiDriver` (GW)         | «driver»      | GW    | TLS-protected transport                                         |
| 10 | `AWS IoT Core`            | «cloud»       | Cloud | Broker; emits PUBACK                                            |

Insert a vertical bus boundary between FD lifelines (1–4) and GW lifelines (5–9). The cloud lifeline (10) sits to the right of all GW lifelines.

### 10.3 Step 3 — Message table

| #   | From                         | To                       | Type   | Message text                              | Guard / fragment                   | Timing                                     | Trace                  |
|-----|------------------------------|--------------------------|--------|-------------------------------------------|------------------------------------|--------------------------------------------|------------------------|
| 1   | `SensorService` (FD)         | `AlarmService` (FD)      | async  | post-acquisition event                    |                                    | within 1 cycle (NF-101)                    | SA-150, AM-000         |
| 1a  | `AlarmService` (FD)          | `SensorService` (FD)     | sync   | pull latest reading (ISensorReadings)     |                                    |                                            | SA-150                 |
| 1b  | `SensorService` (FD)         | `AlarmService` (FD)      | return | reading                                   |                                    |                                            | SA-150                 |
| 2   | `AlarmService` (FD)          | `AlarmService` (FD)      | self   | compare to threshold                      |                                    |                                            | AM-000                 |
| —   |                              |                          |        |                                           | **opt** `[reading out of range]`   |                                            | AM-020                 |
| 3   |   `AlarmService` (FD)        | `ModbusRegisterMap` (FD) | sync   | set alarm flag                            |                                    |                                            | AM-020, MB-000         |
| 4   |   `AlarmService` (FD)        | `Logger` (FD)            | async  | log alarm raised                          |                                    |                                            | AM-020 (P4 exception)  |
| —   |                              |                          |        |                                           | **end opt**                        |                                            |                        |
| —   |                              |                          |        |                                           | **ref** SD-02 (Modbus polling cycle)|                                           | MB-010                 |
| 5   | `ModbusPoller` (GW)          | `AlarmService` (GW)      | async  | alarm flag transition detected            |                                    |                                            | AM-020                 |
| 6   | `AlarmService` (GW)          | `AlarmService` (GW)      | self   | assemble alarm payload                    |                                    |                                            | AM-040                 |
| 7   | `AlarmService` (GW)          | `CloudPublisher` (GW)    | sync   | publish alarm                             |                                    |                                            | AM-030                 |
| —   |                              |                          |        |                                           | **alt**                            |                                            | NF-200                 |
| —   |                              |                          |        |                                           | `[connected]`                      |                                            |                        |
| 8   |   `CloudPublisher` (GW)      | `MqttClient` (GW)        | sync   | publish QoS 1 on alarm topic              |                                    |                                            | AM-030, CC-080, NF-207 |
| 9   |   `MqttClient` (GW)          | `WifiDriver` (GW)        | sync   | send TLS payload                          |                                    |                                            | CC-050                 |
| 10  |   `WifiDriver` (GW)          | `AWS IoT Core`           | async  | MQTT PUBLISH (alarm topic, QoS 1)         |                                    |                                            | AM-030                 |
| 11  |   `AWS IoT Core`             | `MqttClient` (GW)        | return | PUBACK                                    |                                    | end-to-end {≤ 500 ms from FD detection}: NF-113 | NF-207             |
| 12  |   `MqttClient` (GW)          | `CloudPublisher` (GW)    | return | publish confirmed                         |                                    |                                            | NF-207                 |
| —   |                              |                          |        |                                           | `[disconnected]`                   |                                            | BF-000, NF-200         |
| 8'  |   `CloudPublisher` (GW)      | `StoreAndForward` (GW)   | sync   | enqueue alarm (preserves QoS 1, alarm topic) |                                |                                            | BF-000                 |
| —   |                              |                          |        |                                           | **ref** SD-04a (buffering details) |                                            | BF-000, BF-020         |
| —   |                              |                          |        |                                           | **end alt**                        |                                            |                        |

The end-to-end timing constraint `{≤ 500 ms from FD detection to PUBACK}` (REQ-NF-113) brackets from message 3 (alarm flag set on FD) to message 11 (PUBACK on GW).

**UML note 1 (event-driven AlarmService FD, A2):** *"`AlarmService` (FD) and `SensorService` (FD) share the same dispatch context: when `SensorService` completes its acquisition cycle (SD-01 message 10), it posts a `post-acquisition` event. `AlarmService` consumes the event from the same dispatch loop and pulls the latest reading via `ISensorReadings` (P7). The event delivery is the trigger; the data access is pull-based — consistent with the project-wide pull principle."*

**UML note 2 (payload):** *"Alarm payload includes (REQ-AM-040): sensor ID, alarm type (high / low), measured value, threshold value, timestamp, device ID. Schema version per REQ-CC-071. Carried as JSON per REQ-CC-070."*

**UML note 3 (alarm clearance out of scope):** *"Alarm clearance with hysteresis (REQ-AM-010, REQ-AM-011) is a separate flow not drawn here. Per state-machines.md open questions, the per-channel alarm state machine and clear sequence are deferred to LLD."*

**UML note 4 (concern separation, P1):** *"`AlarmService` does not call `MqttClient` directly. `CloudPublisher` mediates all cloud publishes — this preserves layering and concentrates connection-state decisions in one component."*

**UML note 5 (rainy path delegation):** *"On `[disconnected]` the alarm is enqueued at QoS 1 with the alarm topic preserved. SD-04a's enqueue path takes over (drop-oldest applies if buffer is full per REQ-BF-020). On reconnect, SD-04b's drain re-publishes the alarm at QoS 1 and waits for PUBACK per REQ-NF-207. The 500 ms timing constraint from REQ-NF-113 is *not honoured* on the rainy path — buffered alarms surface to the cloud with arbitrary delay; the SRS implicitly accepts this via REQ-NF-200's framing of cloud connectivity as best-effort."*

### 10.4 Traceability summary

| Element                                  | Traces to                              |
|------------------------------------------|----------------------------------------|
| FD detection within one polling cycle    | REQ-NF-101                             |
| Threshold check                          | REQ-AM-000                             |
| Alarm flag in input registers            | REQ-AM-020, REQ-MB-000                 |
| Polling pickup on GW                     | REQ-MB-010 (via SD-02 ref)             |
| Alarm payload fields                     | REQ-AM-040                             |
| QoS 1 publish + PUBACK                   | REQ-NF-207                             |
| Alarm topic                              | REQ-CC-080                             |
| End-to-end 500 ms                        | REQ-NF-113 (sunny path only)           |
| Rainy path: enqueue and buffer           | REQ-BF-000 (via SD-04a ref)            |
| Local detection unaffected by cloud      | REQ-NF-200                             |

### 10.5 Decisions specific to SD-05

| # | Decision | Rationale |
|---|----------|-----------|
| SD05-D1 | Sunny and rainy paths in one diagram, joined by `alt` at the cloud-publish point | Both share the entire FD detection and Modbus pickup leg |
| SD05-D2 | `ref` to SD-02 for the Modbus pickup leg | Preserves SD-02 as the single source of truth |
| SD05-D3 | `ref` to SD-04a for the rainy buffering leg | Preserves SD-04 as the single source of truth |
| SD05-D4 | `AlarmService` (GW) does not call `MqttClient` directly | P1 layering and concern separation |
| SD05-D5 | 500 ms timing constraint shown only on the sunny operand, with a UML note explaining rainy-path behaviour | Constraint is meaningful only when cloud is reachable |
| SD05-D6 | Alarm-clearance flow not drawn (deferred to LLD) | Per-channel alarm state machine deferred per state-machines.md open question |
| SD05-D7 | Logger appears as FD lifeline (P4 exception) | Logging the alarm raise *is* a required SRS behaviour |
| SD05-D8 | Message 1 named `post-acquisition event` (event-driven), not `reading available` | Consistent with pull-based access (P7) — event triggers access; data flows via pull |

---

## 11. SD-06 — OTA firmware update (decomposed)

**Owner:** `UpdateService` (Application, GW).
**Use cases:** UC-18 (initiate update), UC-19 (cloud-authenticated remote command), UC-20 (firmware delivery).
**Scope:** From the cloud-side update command to either commit or rollback. Spans up to two MCU reboots. Decomposed into four sub-diagrams per P9 and aligned with the Firmware Update state machine in `state-machines.md` §7.4.

### 11.1 Sub-diagram inventory

| #      | Sub-sequence                                | State-machine span                                      |
|--------|---------------------------------------------|---------------------------------------------------------|
| SD-06a | Initiation, authentication, lock            | Idle → (verify) → Downloading                           |
| SD-06b | Download and signature verification         | Downloading → Validating → (Failed if signature bad)    |
| SD-06c | Bank swap, reboot, post-boot resumption     | Validating → Applying → reboot → SelfChecking           |
| SD-06d | Self-check, commit or rollback              | SelfChecking → Committed *or* RollingBack → reboot → Failed |

Reboots are **not states** (per `state-machines.md` §7.4); they are transition actions. On the sequence diagrams a reboot is a single message to `ResetDriver` followed by a labelled discontinuity across all lifelines.

### 11.2 Participants (used across SD-06a–d)

| Lifeline               | Stereotype     | Board | Role                                                              |
|------------------------|----------------|-------|-------------------------------------------------------------------|
| `Remote Operator`      | «actor»        | Cloud | Initiates the update                                              |
| `AWS IoT Core`         | «cloud»        | Cloud | Delivers update command and image bytes                           |
| `MqttClient`           | «middleware»   | GW    | Receives MQTT command and image chunks                            |
| `CloudPublisher`       | «application»  | GW    | Forwards update events to UpdateService                           |
| `UpdateService`        | «application»  | GW    | Orchestrates the entire update flow                               |
| `FirmwareStore`        | «middleware»   | GW    | Stores image bytes; verifies signature                            |
| `QspiFlashDriver`      | «driver»       | GW    | Writes bytes to QSPI flash                                        |
| `Bootloader`           | «bootloader»   | GW    | Bank selection on reset (SD-06c, SD-06d rollback)                  |
| `LifecycleController`  | «application»  | GW    | Detects post-boot pending flags; resumes UpdateService            |
| `ResetDriver`          | «driver»       | GW    | Triggers MCU reset                                                |
| `Logger`               | «middleware»   | GW    | Records errors (P4 exception)                                     |
| `SensorService`, `ModbusPoller`, `CloudPublisher` (as self-check probes) | «application» | GW | Self-check participants in SD-06d |

### 11.3 SD-06a — Initiation, authentication, lock

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                            | Label                                       |
|-------------|--------------------------------------------------------------------------|---------------------------------------------|
| REQ-DM-054  | Reject a new update command if one is in progress                        | IMPLIES FRAGMENT(alt) on lock check         |
| REQ-DM-056  | Only accept update commands from authenticated source                    | IMPLIES FRAGMENT(alt) on auth check         |
| REQ-DM-002  | Send confirmation or rejection response to cloud                         | IMPLIES MESSAGE (response on each branch)   |
| REQ-DM-055  | Report update result to cloud                                            | IMPLIES MESSAGE                             |
| REQ-NF-301  | Authenticate using X.509 client certificates                             | NO SEQUENCE IMPLICATION here (TLS handshake established earlier) |

#### Step 2 — Participants

`Remote Operator`, `AWS IoT Core`, `MqttClient`, `CloudPublisher`, `UpdateService`, `Logger`.

#### Step 3 — Message table

| #   | From               | To                | Type   | Message text                            | Guard / fragment                  | Timing | Trace        |
|-----|--------------------|-------------------|--------|-----------------------------------------|-----------------------------------|--------|--------------|
| 1   | `Remote Operator`  | `AWS IoT Core`    | async  | publish update command                  |                                   |        | UC-18 step 1 |
| 2   | `AWS IoT Core`     | `MqttClient`      | async  | MQTT message (update command)           |                                   |        | UC-18 step 2 |
| 3   | `MqttClient`       | `CloudPublisher`  | async  | command received                        |                                   |        |              |
| 4   | `CloudPublisher`   | `UpdateService`   | sync   | start update                            |                                   |        | DM-050       |
| —   |                    |                   |        |                                         | **alt**                           |        | DM-054       |
| —   |                    |                   |        |                                         | `[update in progress]`            |        |              |
| 5'  |   `UpdateService`  | `Logger`          | async  | log rejection (in-progress)             |                                   |        | DM-053       |
| 6'  |   `UpdateService`  | `CloudPublisher`  | sync   | reject (update in progress)             |                                   |        | DM-002, DM-054 |
| 7'  |   `CloudPublisher` | `MqttClient`      | sync   | publish QoS 1 on result topic           |                                   |        | DM-002, DM-055 |
| 8'  |   `MqttClient`     | `AWS IoT Core`    | async  | MQTT PUBLISH (rejection)                |                                   |        | DM-055       |
| —   |                    |                   |        |                                         | `[no update in progress]`         |        |              |
| 5   |   `UpdateService`  | `UpdateService`   | self   | verify command authentication           |                                   |        | DM-056       |
| —   |                    |                   |        |                                         | **alt**                           |        | DM-056       |
| —   |                    |                   |        |                                         | `[auth failed]`                   |        |              |
| 6'' |     `UpdateService`| `Logger`          | async  | log rejection (auth failure)            |                                   |        | DM-053       |
| 7'' |     `UpdateService`| `CloudPublisher`  | sync   | reject (unauthenticated)                |                                   |        | DM-002, DM-056 |
| 8'' |     `CloudPublisher`| `MqttClient`     | sync   | publish QoS 1 on result topic           |                                   |        | DM-002, DM-055 |
| 9'' |     `MqttClient`   | `AWS IoT Core`    | async  | MQTT PUBLISH (rejection)                |                                   |        | DM-055       |
| —   |                    |                   |        |                                         | `[auth ok]`                       |        |              |
| 6   |     `UpdateService`| `UpdateService`   | self   | acquire update lock                     |                                   |        | DM-054       |
| 7   |     `UpdateService`| `UpdateService`   | self   | transition Idle → Downloading           |                                   |        | (state-machines.md §7.4) |
| 8   |     `UpdateService`| `CloudPublisher`  | sync   | accept (ready for download)             |                                   |        | DM-002       |
| 9   |     `CloudPublisher`| `MqttClient`     | sync   | publish QoS 1 on result topic           |                                   |        | DM-002       |
| 10  |     `MqttClient`   | `AWS IoT Core`    | async  | MQTT PUBLISH (acceptance)               |                                   |        | DM-002       |
| —   |                    |                   |        |                                         | **end alt**                       |        |              |
| —   |                    |                   |        |                                         | **end alt**                       |        |              |

**UML note 1:** *"Authentication leverages the TLS X.509 client certificate established at connection setup (REQ-NF-301, see SD-04b TLS handshake). REQ-DM-056 verifies that the certificate-bound identity is on the allowed-publishers list. Internal certificate-validation logic is LLD."*

**UML note 2:** *"All result publishes use QoS 1 to guarantee the cloud sees the update outcome (REQ-DM-055). Topic is the dedicated update-result topic per REQ-CC-080."*

**UML note 3 (CloudPublisher routing):** *"`CloudPublisher` receives inbound MQTT commands from `MqttClient` (messages 2–3) and routes them to `UpdateService`. This `USES` relationship must be reflected in the `components.md` CloudPublisher entry — tracked as F-03."*

### 11.4 SD-06b — Download and signature verification

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                                 | Label                                        |
|-------------|-------------------------------------------------------------------------------|----------------------------------------------|
| REQ-DM-050  | Download the firmware image                                                   | IMPLIES FRAGMENT(loop) over chunks           |
| REQ-DM-051  | Resume download from interruption point if connection is restored             | IMPLIES FRAGMENT(opt) — resume               |
| REQ-DM-052  | Delete partial firmware after 3 download retries                              | IMPLIES FRAGMENT(loop) outer retry × 3       |
| REQ-DM-053  | Log error if download fails                                                   | IMPLIES MESSAGE (log on failure)             |
| REQ-DM-055  | Report update result to cloud                                                 | IMPLIES MESSAGE (on signature fail or download exhaustion) |
| REQ-DM-060  | Validate firmware image                                                       | IMPLIES MESSAGE (verify signature)           |
| REQ-DM-061  | Discard update and restore current firmware if validation fails               | IMPLIES MESSAGE (discard) on rainy operand   |
| REQ-DM-062  | Log error if validation fails                                                 | IMPLIES MESSAGE (log on signature fail)      |
| REQ-DM-080  | Verify cryptographic signature before applying update                         | IMPLIES MESSAGE (verify signature)           |

#### Step 2 — Participants

`AWS IoT Core`, `MqttClient`, `CloudPublisher`, `UpdateService`, `FirmwareStore`, `QspiFlashDriver`, `Logger`.

#### Step 3 — Message table

| #   | From               | To                | Type   | Message text                            | Guard / fragment                       | Timing | Trace          |
|-----|--------------------|-------------------|--------|-----------------------------------------|----------------------------------------|--------|----------------|
| —   |                    |                   |        |                                         | **loop(1..3)** `[download not complete]` |      | DM-052         |
| —   |                    |                   |        |                                         | **loop** `[chunk in image]`            |        | DM-050         |
| 1   |     `AWS IoT Core` | `MqttClient`      | async  | image chunk                             |                                        |        | DM-050         |
| 2   |     `MqttClient`   | `CloudPublisher`  | async  | chunk delivered                         |                                        |        |                |
| 3   |     `CloudPublisher`| `UpdateService`  | sync   | append chunk                            |                                        |        | DM-050         |
| 4   |     `UpdateService`| `FirmwareStore`   | sync   | write chunk at offset                   |                                        |        | DM-050         |
| 5   |     `FirmwareStore`| `QspiFlashDriver` | sync   | program flash page                      |                                        |        | DM-050         |
| 6   |     `QspiFlashDriver`| `FirmwareStore` | return | ok                                      |                                        |        |                |
| 7   |     `FirmwareStore`| `UpdateService`   | return | offset advanced                         |                                        |        |                |
| —   |                    |                   |        |                                         | **opt** `[connection dropped mid-loop]`|        | DM-051         |
| 8'  |       `UpdateService`| `UpdateService` | self   | record resume offset                    |                                        |        | DM-051         |
| —   |                    |                   |        |                                         | **ref** SD-04a, SD-04b (buffer & reconnect) |   | DM-051         |
| 9'  |       `UpdateService`| `CloudPublisher` | sync  | request resume from offset              |                                        |        | DM-051         |
| 10' |       `CloudPublisher`| `MqttClient`   | sync   | publish resume request                  |                                        |        | DM-051         |
| —   |                    |                   |        |                                         | **end opt**                            |        |                |
| —   |                    |                   |        |                                         | **end loop**                           |        |                |
| —   |                    |                   |        |                                         | **alt**                                |        | DM-050         |
| —   |                    |                   |        |                                         | `[all chunks received]`                |        |                |
| 8   |     `UpdateService`| `UpdateService`   | self   | exit retry loop                         |                                        |        | DM-050         |
| —   |                    |                   |        |                                         | `[chunk loop failed]`                  |        |                |
| —   |                    |                   |        |                                         | (next outer iteration)                 |        |                |
| —   |                    |                   |        |                                         | **end alt**                            |        |                |
| —   |                    |                   |        |                                         | **end loop**                           |        |                |
| —   |                    |                   |        |                                         | **alt**                                |        | DM-052         |
| —   |                    |                   |        |                                         | `[3 download attempts failed]`         |        |                |
| 9'' |   `UpdateService`  | `FirmwareStore`   | sync   | discard partial image                   |                                        |        | DM-052         |
| 10''|   `UpdateService`  | `Logger`          | async  | log download failure                    |                                        |        | DM-053         |
| 11''|   `UpdateService`  | `CloudPublisher`  | sync   | report failure (download exhausted)     |                                        |        | DM-055         |
| 12''|   `CloudPublisher` | `MqttClient`      | sync   | publish QoS 1 on result topic           |                                        |        | DM-055         |
| 13''|   `UpdateService`  | `UpdateService`   | self   | release lock; transition → Failed       |                                        |        | (state-machines.md §7.4) |
| —   |                    |                   |        |                                         | `[download succeeded]`                 |        |                |
| 9   |   `UpdateService`  | `FirmwareStore`   | sync   | verify signature                        |                                        |        | DM-060, DM-080 |
| —   |                    |                   |        |                                         | **alt**                                |        | DM-061         |
| —   |                    |                   |        |                                         | `[signature valid]`                    |        |                |
| 10  |     `FirmwareStore`| `UpdateService`   | return | valid                                   |                                        |        | DM-080         |
| 11  |     `UpdateService`| `UpdateService`   | self   | transition Validating → Applying        |                                        |        | (state-machines.md §7.4) |
| —   |                    |                   |        |                                         | `[signature invalid]`                  |        |                |
| 10''' |   `FirmwareStore`| `UpdateService`   | return | invalid                                 |                                        |        | DM-080         |
| 11'''|   `UpdateService` | `FirmwareStore`   | sync   | discard image                           |                                        |        | DM-061         |
| 12'''|   `UpdateService` | `Logger`          | async  | log signature failure                   |                                        |        | DM-062         |
| 13'''|   `UpdateService` | `CloudPublisher`  | sync   | report failure (signature)              |                                        |        | DM-055         |
| 14'''|   `CloudPublisher`| `MqttClient`      | sync   | publish QoS 1 on result topic           |                                        |        | DM-055         |
| 15'''|   `UpdateService` | `UpdateService`   | self   | release lock; transition → Failed       |                                        |        | (state-machines.md §7.4) |
| —   |                    |                   |        |                                         | **end alt**                            |        |                |
| —   |                    |                   |        |                                         | **end alt**                            |        |                |

**UML note 1 (chunking):** *"Chunk size and total count are negotiated at the protocol level (LLD detail). The HLD shows one canonical loop iteration."*

**UML note 2 (resume semantics):** *"Mid-download disconnect uses the same store-and-forward machinery as cloud telemetry (REQ-DM-051). The resume request carries the last persisted offset. If three full download attempts all fail, the partial image is discarded per REQ-DM-052."*

### 11.5 SD-06c — Bank swap, reboot, post-boot resumption

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                                 | Label                                  |
|-------------|-------------------------------------------------------------------------------|----------------------------------------|
| REQ-DM-070  | Set firmware partition as boot partition                                      | IMPLIES MESSAGE (write boot indicator) |
| REQ-DM-071  | Trigger self-check validation on new firmware                                 | IMPLIES MESSAGE (write self-check pending flag) |
| REQ-DM-073  | Retain current firmware until self-check success                              | NO SEQUENCE IMPLICATION (dual-bank)    |
| REQ-DM-074  | Maintain dual-bank partition scheme                                           | NO SEQUENCE IMPLICATION                |

#### Step 2 — Participants

`UpdateService`, `FirmwareStore`, `QspiFlashDriver`, `ResetDriver`, `Bootloader`, `LifecycleController`.

#### Step 3 — Message table

| #   | From                  | To                  | Type   | Message text                              | Guard / fragment | Timing | Trace                |
|-----|-----------------------|---------------------|--------|-------------------------------------------|------------------|--------|----------------------|
| 1   | `UpdateService`       | `FirmwareStore`     | sync   | set boot bank to new                      |                  |        | DM-070, DM-074       |
| 2   | `FirmwareStore`       | `QspiFlashDriver`   | sync   | write boot indicator                      |                  |        | DM-070               |
| 3   | `QspiFlashDriver`     | `FirmwareStore`     | return | ok                                        |                  |        |                      |
| 4   | `UpdateService`       | `FirmwareStore`     | sync   | set pending_self_check flag               |                  |        | DM-071               |
| 5   | `FirmwareStore`       | `QspiFlashDriver`   | sync   | write flag                                |                  |        | DM-071               |
| 6   | `QspiFlashDriver`     | `FirmwareStore`     | return | ok                                        |                  |        |                      |
| 7   | `UpdateService`       | `UpdateService`     | self   | transition Applying → (reset)             |                  |        | (state-machines.md §7.4) |
| 8   | `UpdateService`       | `ResetDriver`       | sync   | trigger MCU reset                         |                  | recovery {≤ 5 s}: NF-203 | DM-070         |
| —   |                       |                     |        | **── MCU reset; flow resumes after boot ──** | (lifeline discontinuity across all participants) | |    |
| 9   | `Bootloader`          | `Bootloader`        | self   | check pending flags                       |                  |        | DM-073               |
| 10  | `Bootloader`          | `Bootloader`        | self   | select new bank                           |                  |        | DM-070               |
| 11  | `Bootloader`          | `LifecycleController` | async | jump to application reset vector         |                  |        |                      |
| —   |                       |                     |        | **── Bootloader lifeline ends ──**        | (lifeline transition) |    |                      |
| 12  | `LifecycleController` | `LifecycleController` | self | enter Init                                |                  |        | (state-machines.md §7.2) |
| 13  | `LifecycleController` | `FirmwareStore`     | sync   | check pending flags                       |                  |        | DM-073               |
| 14  | `FirmwareStore`       | `LifecycleController` | return | pending_self_check                      |                  |        |                      |
| 15  | `LifecycleController` | `UpdateService`     | sync   | resume in SelfChecking                    |                  |        | (state-machines.md §7.4) |

**UML note 1 (discontinuity convention):** *"The horizontal break across all lifelines after message 8 represents the MCU reset (REQ-NF-203 5 s recovery target). All in-RAM state is lost; only data persisted to QSPI flash (boot indicator, pending flags) survives."*

**UML note 2 (dual-bank):** *"The previous firmware bank is retained (REQ-DM-073). If self-check fails (SD-06d rainy path), the rollback path triggers. Until commit, both banks remain valid."*

**UML note 3 (continuation):** *"Control passes to SD-06d (self-check) at message 15."*

### 11.6 SD-06d — Self-check, commit or rollback

#### Step 1 — SRS mapping

| SRS         | Text fragment                                                                 | Label                                       |
|-------------|-------------------------------------------------------------------------------|---------------------------------------------|
| REQ-DM-040  | Self-check verifies sensor initialisations and communication links            | IMPLIES MESSAGE (probes to subsystems)      |
| REQ-DM-055  | Report update result to cloud                                                 | IMPLIES MESSAGE on each branch              |
| REQ-DM-072  | Rollback previous firmware if self-check fails                                | IMPLIES MESSAGE (write rollback flag) on rainy |
| REQ-DM-073  | Retain current firmware until self-check success                              | IMPLIES MESSAGE (commit) on sunny           |
| REQ-NF-204  | Roll back and resume normal operation within 10 s                             | IMPLIES TIMING CONSTRAINT on rollback path  |

#### Step 2 — Participants

`UpdateService`, `SensorService`, `ModbusPoller`, `CloudPublisher`, `FirmwareStore`, `QspiFlashDriver`, `ResetDriver`, `Bootloader`, `LifecycleController`, `MqttClient`, `Logger`.

#### Step 3 — Message table

| #   | From                  | To                  | Type   | Message text                              | Guard / fragment                       | Timing                            | Trace                |
|-----|-----------------------|---------------------|--------|-------------------------------------------|----------------------------------------|-----------------------------------|----------------------|
| 1   | `UpdateService`       | `SensorService`     | sync   | probe sensors                             |                                        |                                   | DM-040               |
| 2   | `SensorService`       | `UpdateService`     | return | sensor status                             |                                        |                                   | DM-040               |
| 3   | `UpdateService`       | `ModbusPoller`      | sync   | probe Modbus link                         |                                        |                                   | DM-040               |
| 4   | `ModbusPoller`        | `UpdateService`     | return | link status                               |                                        |                                   | DM-040               |
| 5   | `UpdateService`       | `CloudPublisher`    | sync   | probe cloud connection                    |                                        |                                   | DM-040               |
| 6   | `CloudPublisher`      | `UpdateService`     | return | connection status                         |                                        |                                   | DM-040               |
| 7   | `UpdateService`       | `UpdateService`     | self   | aggregate self-check result               |                                        |                                   | DM-040               |
| —   |                       |                     |        |                                           | **alt**                                |                                   | DM-072, DM-073       |
| —   |                       |                     |        |                                           | `[all probes ok]`                      |                                   |                      |
| 8   |   `UpdateService`     | `FirmwareStore`     | sync   | mark new bank as committed                |                                        |                                   | DM-073               |
| 9   |   `FirmwareStore`     | `QspiFlashDriver`   | sync   | clear pending_self_check; mark committed  |                                        |                                   | DM-073               |
| 10  |   `UpdateService`     | `UpdateService`     | self   | release lock; transition → Committed      |                                        |                                   | (state-machines.md §7.4) |
| 11  |   `UpdateService`     | `CloudPublisher`    | sync   | report success                            |                                        |                                   | DM-055               |
| 12  |   `CloudPublisher`    | `MqttClient`        | sync   | publish QoS 1 on result topic             |                                        |                                   | DM-055               |
| —   |                       |                     |        |                                           | `[any probe failed]`                   |                                   |                      |
| 8'  |   `UpdateService`     | `Logger`            | async  | log self-check failure                    |                                        |                                   | DM-053               |
| 9'  |   `UpdateService`     | `FirmwareStore`     | sync   | set pending_rollback flag; restore old bank |                                      |                                   | DM-072               |
| 10' |   `FirmwareStore`     | `QspiFlashDriver`   | sync   | write rollback flag; revert boot indicator |                                       |                                   | DM-072               |
| 11' |   `UpdateService`     | `UpdateService`     | self   | transition SelfChecking → RollingBack     |                                        |                                   | (state-machines.md §7.4) |
| 12' |   `UpdateService`     | `ResetDriver`       | sync   | trigger MCU reset                         |                                        | rollback total {≤ 10 s}: NF-204   | NF-204               |
| —   |                       |                     |        | **── MCU reset; flow resumes after boot ──** | (lifeline discontinuity)            |                                   |                      |
| 13' | `Bootloader`          | `Bootloader`        | self   | check pending flags; select previous bank |                                        |                                   | DM-072, DM-073       |
| 14' | `Bootloader`          | `LifecycleController` | async | jump to application reset vector         |                                        |                                   |                      |
| 15' | `LifecycleController` | `FirmwareStore`     | sync   | check pending flags                       |                                        |                                   |                      |
| 16' | `FirmwareStore`       | `LifecycleController` | return | pending_rollback                        |                                        |                                   |                      |
| 17' | `LifecycleController` | `UpdateService`     | sync   | resume after rollback                     |                                        |                                   | DM-072               |
| 18' | `UpdateService`       | `CloudPublisher`    | sync   | report rollback                           |                                        |                                   | DM-055               |
| 19' | `CloudPublisher`      | `MqttClient`        | sync   | publish QoS 1 on result topic             |                                        |                                   | DM-055               |
| 20' | `UpdateService`       | `UpdateService`     | self   | release lock; transition → Failed         |                                        |                                   | (state-machines.md §7.4) |
| —   |                       |                     |        |                                           | **end alt**                            |                                   |                      |

The duration constraint `{≤ 10 s}` (REQ-NF-204) brackets the rollback operand from message 7 (self-check failure decision) to message 20' (final transition to Failed).

**UML note 1 (serial probes, A4 / SD06-D7):** *"Probes are serial sync calls. Aggregate worst-case ~500 ms, well within REQ-NF-204's 10 s budget. Serialisation simplifies aggregation logic at no architectural cost; concurrent probing would require coordination primitives (event group / counting semaphore) that are not justified at this scale."*

**UML note 2 (commit semantics):** *"Commit is atomic at the QSPI flash level: clearing `pending_self_check` and marking the bank committed are written in one journaled operation. If power is lost mid-commit, the flag remains set and `LifecycleController` re-enters SelfChecking on next boot — idempotent."*

**UML note 3 (rollback path):** *"Rollback reverts the boot indicator to the previous bank and triggers a second reset. Post-boot, `Bootloader` selects the previous bank, `LifecycleController` detects `pending_rollback` and resumes `UpdateService` to publish the rollback result. The previous firmware was retained per REQ-DM-073, so the system returns to a known-good state."*

### 11.7 Traceability summary (SD-06 overall)

| Element                                  | Traces to                                       | Sub-diagram |
|------------------------------------------|-------------------------------------------------|-------------|
| Cloud command intake                     | UC-18 step 1, REQ-DM-050                        | SD-06a      |
| Authentication                           | REQ-DM-056, REQ-NF-301                          | SD-06a      |
| Reject if in progress                    | REQ-DM-054                                      | SD-06a      |
| Update result reporting                  | REQ-DM-002, REQ-DM-055                          | SD-06a, b, d|
| Chunked download                         | REQ-DM-050                                      | SD-06b      |
| Resume on disconnect                     | REQ-DM-051                                      | SD-06b      |
| 3 retries before discard                 | REQ-DM-052                                      | SD-06b      |
| Signature verification                   | REQ-DM-060, REQ-DM-080                          | SD-06b      |
| Discard on signature fail                | REQ-DM-061, REQ-DM-062                          | SD-06b      |
| Dual-bank scheme                         | REQ-DM-070, REQ-DM-073, REQ-DM-074              | SD-06c      |
| Set self-check pending                   | REQ-DM-071                                      | SD-06c      |
| Self-check probes                        | REQ-DM-040                                      | SD-06d      |
| Commit on success                        | REQ-DM-073                                      | SD-06d      |
| Rollback on failure                      | REQ-DM-072                                      | SD-06d      |
| Rollback within 10 s                     | REQ-NF-204                                      | SD-06d      |

### 11.8 Decisions specific to SD-06

| # | Decision | Rationale |
|---|----------|-----------|
| SD06-D1 | Four sub-diagrams (a–d) | Single diagram would exceed P9 budget |
| SD06-D2 | Reboots as `ResetDriver` call + lifeline discontinuity, not as states | Mirrors `state-machines.md` §7.4 |
| SD06-D3 | All update results published at QoS 1 | Cloud must reliably learn of update outcome |
| SD06-D4 | Both rejection paths in SD-06a are drawn explicitly | Rejection is the most likely rainy outcome of a botched OTA campaign |
| SD06-D5 | Outer retry loop in SD-06b is `loop(1..3)` (REQ-DM-052), inner loop is unbounded `loop [chunk in image]` | Two distinct concerns |
| SD06-D6 | Resumption on mid-download disconnect references SD-04 rather than redrawing | Reconnection logic is owned by SD-04 |
| SD06-D7 | Self-check probes in SD-06d are **serial sync**, not `par` (revised from earlier draft) | Aggregate ~500 ms within 10 s budget; avoids over-engineering coordination logic; "boring is correct" |
| SD06-D8 | Commit clears `pending_self_check` and marks bank committed in one journaled write | Idempotent recovery from power loss mid-commit |
| SD06-D9 | Logger appears as a lifeline (P4 exception) on every rainy path | REQ-DM-053 and REQ-DM-062 explicitly require logging on failure |

---

## 12. SD-07 — Remote configuration command

**Owner:** `ConfigService` (Application, GW).
**Use cases:** UC-15, UC-19.
**Scope:** Remote operator pushes a parameter change (threshold, polling interval, **or device profile**) via MQTT. Gateway validates, persists, applies, and acknowledges. Includes the device-profile-update branch which triggers re-probing of affected slaves.

### 12.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                              | Label                                          |
|-------------|----------------------------------------------------------------------------|------------------------------------------------|
| REQ-DM-000  | Accept operational parameter changes from Remote Operator via cloud command| IMPLIES MESSAGE (cloud command intake)         |
| REQ-DM-001  | Validate remote parameter changes before applying                          | IMPLIES MESSAGE (self: validate)               |
| REQ-DM-002  | Send confirmation or rejection response to cloud                           | IMPLIES MESSAGE (response on each branch)      |
| REQ-DM-090  | Persist all configuration changes to non-volatile storage                  | IMPLIES MESSAGE (write to ConfigStore)         |
| REQ-DM-100 (proposed) | Accept device-profile updates via the provisioning mechanism      | IMPLIES MESSAGE (profile update)               |
| REQ-DM-101 (proposed) | Trigger re-probing of affected slaves on profile update           | IMPLIES MESSAGE (re-probe)                     |
| REQ-NF-301  | Authenticate using X.509 client certificates                               | NO SEQUENCE IMPLICATION                        |

### 12.2 Step 2 — Participants

| #  | Lifeline                  | Stereotype    | Board | Role                                                              |
|----|---------------------------|---------------|-------|-------------------------------------------------------------------|
| 1  | `Remote Operator`         | «actor»       | Cloud | Initiates the configuration change                                |
| 2  | `AWS IoT Core`            | «cloud»       | Cloud | Delivers MQTT command                                             |
| 3  | `MqttClient`              | «middleware»  | GW    | Receives command                                                  |
| 4  | `CloudPublisher`          | «application» | GW    | Routes command to ConfigService                                   |
| 5  | `ConfigService`           | «application» | GW    | Validates, applies, and reports                                   |
| 6  | `ConfigStore`             | «middleware»  | GW    | Persists configuration                                            |
| 7  | `DeviceProfileRegistry`   | «application» | GW    | Updates registry on profile changes                               |
| 8  | `ModbusPoller`            | «application» | GW    | Re-probes affected slaves                                         |
| 9  | `Logger`                  | «middleware»  | GW    | Records validation failures (P4 exception)                        |

### 12.3 Step 3 — Message table

| #   | From               | To                       | Type   | Message text                            | Guard / fragment              | Timing | Trace          |
|-----|--------------------|--------------------------|--------|-----------------------------------------|-------------------------------|--------|----------------|
| 1   | `Remote Operator`  | `AWS IoT Core`           | async  | publish config command                  |                               |        | DM-000         |
| 2   | `AWS IoT Core`     | `MqttClient`             | async  | MQTT message (config command)           |                               |        | DM-000         |
| 3   | `MqttClient`       | `CloudPublisher`         | async  | command received                        |                               |        |                |
| 4   | `CloudPublisher`   | `ConfigService`          | sync   | apply config change                     |                               |        | DM-000         |
| 5   | `ConfigService`    | `ConfigService`          | self   | validate parameters                     |                               |        | DM-001         |
| —   |                    |                          |        |                                         | **alt**                       |        | DM-001         |
| —   |                    |                          |        |                                         | `[validation failed]`         |        |                |
| 6'  |   `ConfigService`  | `Logger`                 | async  | log validation failure                  |                               |        | DM-001         |
| 7'  |   `ConfigService`  | `CloudPublisher`         | sync   | reject (validation)                     |                               |        | DM-002         |
| 8'  |   `CloudPublisher` | `MqttClient`             | sync   | publish QoS 1 on result topic           |                               |        | DM-002         |
| —   |                    |                          |        |                                         | `[validation ok]`             |        |                |
| 6   |   `ConfigService`  | `ConfigStore`            | sync   | persist parameters                      |                               |        | DM-090         |
| 7   |   `ConfigStore`    | `ConfigService`          | return | ok                                      |                               |        | DM-090         |
| —   |                    |                          |        |                                         | **opt** `[profile update]`    |        | DM-100         |
| 8   |     `ConfigService`| `DeviceProfileRegistry`  | sync   | update profile                          |                               |        | DM-100         |
| 9   |     `ConfigService`| `ModbusPoller`           | sync   | re-probe affected slave                 |                               |        | DM-101         |
| 10  |     `ModbusPoller` | `ConfigService`          | return | re-probe outcome                        |                               |        | DM-101         |
| —   |                    |                          |        |                                         | **end opt**                   |        |                |
| 11  |   `ConfigService`  | `CloudPublisher`         | sync   | confirm change                          |                               |        | DM-002         |
| 12  |   `CloudPublisher` | `MqttClient`             | sync   | publish QoS 1 on result topic           |                               |        | DM-002         |
| —   |                    |                          |        |                                         | **end alt**                   |        |                |

**UML note 1 (config domain):** *"Configurable parameters include: polling rate, alarm thresholds, telemetry / health intervals, MQTT topics, and **device profiles** (REQ-DM-100, proposed). Each parameter has a validation rule applied in message 5; failure halts the flow before persistence."*

**UML note 2 (re-probe scope):** *"On profile update (sunny operand), `ModbusPoller` re-probes only the slave bound to the updated profile, not all slaves. The re-probe uses the same probe-and-validate logic as SD-00b's link establishment; failure marks the slave `link_rejected`."*

**UML note 3 (CloudPublisher routing):** *"`CloudPublisher` receives inbound MQTT commands from `MqttClient` (messages 2–3) and routes them to `ConfigService`. This `USES` relationship must be reflected in the `components.md` CloudPublisher entry — tracked as F-03."*

### 12.4 Decisions specific to SD-07

| # | Decision | Rationale |
|---|----------|-----------|
| SD07-D1 | One diagram covering all parameter types | Validation, persistence, and acknowledgement are uniform across parameter types; the profile-update branch is encoded as `opt` |
| SD07-D2 | Profile update triggers re-probe of affected slave only, not full re-establishment | Minimises disruption to other slaves' polling; aligns with per-slave link state (SD00-D5) |
| SD07-D3 | Validation failure logged before reject response | REQ-DM-001 implies validation outcome must be observable; logging supports diagnostics (P4 exception) |

---

## 13. SD-08 — Remote restart

**Owner:** `LifecycleController` (Application, GW).
**Use cases:** UC-17, UC-19.
**Scope:** Remote operator requests a device restart; gateway requests confirmation, awaits operator confirmation, executes restart with self-check, reports outcome.

### 13.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                              | Label                                          |
|-------------|----------------------------------------------------------------------------|------------------------------------------------|
| REQ-DM-010  | Execute remote restart command                                             | IMPLIES MESSAGE (restart trigger)              |
| REQ-DM-020  | Request confirmation from Remote Operator before restart                   | IMPLIES MESSAGE (confirmation request)         |
| REQ-DM-021  | Cancel restart if operator declines                                        | IMPLIES FRAGMENT(alt) on confirmation response |
| REQ-DM-030  | Report restart success after reboot and self-check                         | IMPLIES MESSAGE (post-boot ack)                |
| REQ-DM-040  | Self-check after restart (sensor init, comms links)                        | IMPLIES `ref` to SD-00b Init.SelfChecking step (SD-B02); not separately redrawn |
| REQ-NF-203  | Recover from MCU reset within 5 s                                          | IMPLIES TIMING CONSTRAINT                      |

### 13.2 Step 2 — Participants

`Remote Operator`, `AWS IoT Core`, `MqttClient`, `CloudPublisher`, `LifecycleController`, `ResetDriver`, `Bootloader`.

### 13.3 Step 3 — Message table

| #   | From                  | To                | Type   | Message text                              | Guard / fragment           | Timing                       | Trace          |
|-----|-----------------------|-------------------|--------|-------------------------------------------|----------------------------|------------------------------|----------------|
| 1   | `Remote Operator`     | `AWS IoT Core`    | async  | publish restart command                   |                            |                              | DM-010         |
| 2   | `AWS IoT Core`        | `MqttClient`      | async  | MQTT message (restart command)            |                            |                              | DM-010         |
| 3   | `MqttClient`          | `CloudPublisher`  | async  | command received                          |                            |                              |                |
| 4   | `CloudPublisher`      | `LifecycleController` | sync | initiate restart                          |                            |                              | DM-010         |
| 5   | `LifecycleController` | `CloudPublisher`  | sync   | request confirmation                      |                            |                              | DM-020         |
| 6   | `CloudPublisher`      | `MqttClient`      | sync   | publish QoS 1 (confirmation request)      |                            |                              | DM-020         |
| 7   | `MqttClient`          | `AWS IoT Core`    | async  | MQTT PUBLISH (confirmation request)       |                            |                              | DM-020         |
| 8   | `Remote Operator`     | `AWS IoT Core`    | async  | publish confirmation response             |                            |                              | DM-020         |
| 9   | `AWS IoT Core`        | `MqttClient`      | async  | MQTT message (confirmation response)      |                            |                              |                |
| 10  | `MqttClient`          | `CloudPublisher`  | async  | response received                         |                            |                              |                |
| 11  | `CloudPublisher`      | `LifecycleController` | sync | response                                  |                            |                              |                |
| —   |                       |                   |        |                                           | **alt**                    |                              | DM-021         |
| —   |                       |                   |        |                                           | `[operator declined]`      |                              |                |
| 12' |   `LifecycleController` | `CloudPublisher`| sync   | report restart cancelled                  |                            |                              | DM-021         |
| 13' |   `CloudPublisher`    | `MqttClient`      | sync   | publish QoS 1 on result topic             |                            |                              | DM-021         |
| —   |                       |                   |        |                                           | `[operator confirmed]`     |                              |                |
| 12  |   `LifecycleController` | `LifecycleController` | self | set restart_flag in NV flags region    |                            |                              | DM-030         |
| 13  |   `LifecycleController` | `ResetDriver`   | sync   | trigger MCU reset                         |                            | recovery {≤ 5 s}: NF-203     | DM-010, NF-203 |
| —   |                       |                   |        | **── MCU reset; flow resumes after boot ──** | (lifeline discontinuity) |                              |                |
| 14  |   `Bootloader`        | `Bootloader`      | self   | check pending flags; select current bank  |                            |                              |                |
| 15  |   `Bootloader`        | `LifecycleController` | async | jump to application reset vector         |                            |                              |                |
| 16  |   `LifecycleController` | `LifecycleController` | self | enter Init                              |                            |                              |                |
| —   |                       |                   |        |                                           | **ref** SD-00b (cold boot incl. Init.SelfChecking DM-040) |   | NF-203, DM-040 |
| —   |                       |                   |        |                                           | **end alt**                |                              |                |

**UML note 1 (post-restart self-check):** *"The Init.SelfChecking step (DM-040) is handled inside the SD-00b cold-boot path (see SD-B02 fix). The `restart_flag` (set in message 12) is a NV flag that survives the reset; `LifecycleController` Operational entry action reads it, publishes 'restart success' (REQ-DM-030) via `CloudPublisher`, and clears the flag — see state-machines.md §7.2 Operational entry action."*

**UML note 2 (cold-boot reuse):** *"After the MCU reset (message 13) the gateway follows the standard SD-00b cold-boot path including Init.SelfChecking (DM-040). Post-boot, `LifecycleController` enters Operational and its entry action detects the `restart_flag` set in message 12, publishes 'restart success' (REQ-DM-030) via `CloudPublisher`, and clears the flag — see state-machines.md §7.2."*

**UML note 3 (CloudPublisher routing):** *"`CloudPublisher` receives inbound MQTT commands from `MqttClient` (messages 2–3) and routes them to `LifecycleController`. This `USES` relationship must be reflected in the `components.md` CloudPublisher entry — tracked as F-03."*

### 13.4 Decisions specific to SD-08

| # | Decision | Rationale |
|---|----------|-----------|
| SD08-D1 | Operator confirmation handshake before restart | REQ-DM-020 explicit; safety-net against accidental restarts |
| SD08-D2 | DM-040 probes reused from SD-00b Init.SelfChecking (via `ref`) | Both cold boot and restart follow the same probe-then-enter-Operational path |
| SD08-D3 | `restart_flag` persists in NV flags region across reboot to enable success-reporting | RAM is lost at reset; flag in NV tells Operational entry action to publish ack (REQ-DM-030) |

---

## 14. SD-09 — Time synchronisation

**Owner:** `TimeService` (Application, GW).
**Use case:** UC-13.
**Scope:** NTP synchronisation at boot and periodically; propagation of time to FD via Modbus holding register write; rainy operand for NTP unreachable.

### 14.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                              | Label                                          |
|-------------|----------------------------------------------------------------------------|------------------------------------------------|
| REQ-TS-000  | Update internal time during boot and periodically                          | IMPLIES TIMING CONSTRAINT (boot + periodic)    |
| REQ-TS-010  | Synchronise time using NTP, configurable list of NTP servers               | IMPLIES MESSAGE (NTP request)                  |
| REQ-TS-020  | Update RTC with the synchronised time                                      | IMPLIES MESSAGE (write RTC)                    |
| REQ-TS-030  | Write current time to FD via Modbus holding register on initial connection and at regular intervals | IMPLIES MESSAGE (FC 06/16 to FD)     |
| REQ-TS-040  | Use uptime-based timestamps and flag data unsynchronised if NTP not done   | NO SEQUENCE IMPLICATION (predicate; affects payloads) |
| REQ-TS-0E1  | Retry NTP as soon as internet connection is restored                       | IMPLIES MESSAGE (retry trigger from CloudPublisher) |

### 14.2 Step 2 — Participants

| #  | Lifeline                   | Stereotype     | Board | Role in this sequence                                              |
|----|----------------------------|----------------|-------|-------------------------------------------------------------------|
| 1  | (NTP timer)                | «timer»        | GW    | Periodic event source (boot and configured interval)              |
| 2  | `TimeService`              | «application»  | GW    | Orchestrates NTP sync and FD time propagation                     |
| 3  | `NtpClient`                | «middleware»   | GW    | Issues UDP NTP queries                                            |
| 4  | `WifiDriver`               | «driver»       | GW    | UDP / TLS transport                                               |
| 5  | NTP server                 | «cloud»        | Cloud | External time reference                                           |
| 6  | `RtcDriver`                | «driver»       | GW    | Writes synchronised time to hardware RTC                          |
| 7  | `ModbusPoller`             | «application»  | GW    | Routes holding-register write to FD (`IModbusPoller` interface, P1) |
| 8  | `ModbusMaster`             | «middleware»   | GW    | Encodes and transmits Modbus FC 06/16 frame                       |
| 9  | `ModbusUartDriver` (GW)    | «driver»       | GW    | Bus transmit                                                      |
| 10 | `ModbusUartDriver` (FD)    | «driver»       | FD    | Bus receive                                                       |
| 11 | `ModbusSlave`              | «middleware»   | FD    | Decodes frame; dispatches to register map                         |
| 12 | `IModbusRegisterMap` (FD)  | «application»  | FD    | Writes time to holding register; DIP interface                    |
| 13 | `Logger`                   | «middleware»   | GW    | Records NTP unreachable event (P4 exception)                      |
| 14 | `CloudPublisher`           | «application»  | GW    | Triggers NTP retry on reconnect (TS-0E1)                          |

### 14.3 Step 3 — Message table

| #   | From                  | To                  | Type        | Message text                              | Guard / fragment           | Trace          |
|-----|-----------------------|---------------------|-------------|-------------------------------------------|----------------------------|----------------|
| 1   | (NTP timer)           | `TimeService`       | async       | tick                                      |                            | TS-000         |
| 2   | `TimeService`         | `NtpClient`         | sync        | request time sync                         |                            | TS-010         |
| 3   | `NtpClient`           | `WifiDriver`        | sync        | UDP NTP query                             |                            | TS-010         |
| 4   | `WifiDriver`          | NTP server          | async       | NTP request                               |                            | TS-010         |
| —   |                       |                     |             |                                           | **alt**                    | TS-010, TS-0E1 |
| —   |                       |                     |             |                                           | `[NTP response received]`  |                |
| 5   |   NTP server          | `WifiDriver`        | async       | NTP response                              |                            | TS-010         |
| 6   |   `WifiDriver`        | `NtpClient`         | return      | timestamp                                 |                            | TS-010         |
| 7   |   `NtpClient`         | `TimeService`       | return      | synchronised time                         |                            | TS-010         |
| 8   |   `TimeService`       | `RtcDriver`         | sync        | write current time                        |                            | TS-020         |
| 9   |   `RtcDriver`         | `TimeService`       | return      | ok                                        |                            | TS-020         |
| 10  |   `TimeService`       | `ModbusPoller`      | sync        | request time write to FD (FC 06/16, holding reg) |                     | TS-030         |
| 10a |   `ModbusPoller`      | `ModbusMaster`      | sync        | send request (FC 06/16, holding reg)      |                            | MB-040         |
| 11  |   `ModbusMaster`      | `ModbusUartDriver` (GW) | sync    | write frame                               |                            | MB-030         |
| 12  |   `ModbusUartDriver` (GW) | `ModbusUartDriver` (FD) | async (bus) | RTU frame on bus                  |                            | MB-030         |
| 13  |   `ModbusUartDriver` (FD) | `ModbusSlave`   | async       | frame received                            |                            | MB-030         |
| 14  |   `ModbusSlave`       | `IModbusRegisterMap` (FD) | sync  | write holding register (time)             |                            | MB-040         |
| 15  |   `TimeService`       | `TimeService`       | self        | mark time synchronised                    |                            | TS-040         |
| —   |                       |                     |             |                                           | `[NTP timeout]`            |                |
| 5'  |   `NtpClient`         | `TimeService`       | return      | failure                                   |                            | TS-0E1         |
| 6'  |   `TimeService`       | `Logger`            | async       | log NTP unreachable                       |                            | TS-0E1         |
| 7'  |   `TimeService`       | `TimeService`       | self        | mark time unsynchronised                  |                            | TS-040         |
| —   |                       |                     |             |                                           | **end alt**                |                |

**Periodic re-sync trigger from CloudPublisher (post-reconnect):**

| #   | From               | To             | Type   | Message text             | Guard / fragment | Trace |
|-----|--------------------|----------------|--------|--------------------------|------------------|-------|
| 16  | `CloudPublisher`   | `TimeService`  | async  | retry NTP (post-reconnect)|                  | TS-0E1|

This message is shown as an outbound async event; on receipt, `TimeService` re-enters the sequence at message 2.

**UML note 1 (FD timestamp use):** *"Once the FD's holding register is written, the FD uses the gateway-provided time for its own timestamps (REQ-SA-100). Until then, the FD uses uptime-based timestamps and flags data unsynchronised (REQ-NF-212, REQ-TS-040)."*

**UML note 2 (period configuration):** *"NTP sync period is configurable (REQ-TS-000 [TBD]). Boot sync runs once during SD-00b cold boot; periodic sync uses the same flow."*

**UML note 3 (`ModbusPoller` as P1 gatekeeper):** *"`TimeService` (application) routes the holding-register write through `ModbusPoller` (message 10) via the `IModbusPoller` interface to respect P1 strict directional layering — application-layer components must not call `ModbusMaster` (middleware) directly. `ModbusPoller` is the single Modbus bus gatekeeper for all application-layer callers."*

**UML note 4 (`IModbusRegisterMap` DIP interface):** *"`ModbusSlave` (middleware) must not call `ModbusRegisterMap` (application) directly. The `IModbusRegisterMap` interface is declared in the Application layer; `ModbusSlave` depends on it by injection. Concrete binding specified at LLD."*

### 14.4 Decisions specific to SD-09

| # | Decision | Rationale |
|---|----------|-----------|
| SD09-D1 | NTP server modelled as `«cloud»` lifeline | Boundary actor like AWS IoT Core; not a project component |
| SD09-D2 | FD time propagation uses Modbus FC 06 / FC 16 (holding register write) | REQ-TS-030 explicit; reuses Modbus channel without adding a new transport |
| SD09-D3 | NTP failure logged but not cloud-reported | Time sync is internal; cloud telemetry already carries the unsynchronised flag (REQ-TS-040) |
| SD09-D4 | Retry triggered by `CloudPublisher` on reconnect (REQ-TS-0E1) | Reuses existing connection-state events; no new mechanism |

---

## 15. SD-10 — Device provisioning

**Owners:** `ConfigService`, `DeviceProfileRegistry` (Application, GW).
**Use case:** UC-16.
**Scope:** Field Technician provisions the gateway with all configuration needed before deployment: WiFi credentials, MQTT certificates, **device profiles for expected slaves**, and other parameters. Performed via the gateway's CLI (`ConsoleService`).

### 15.1 Step 1 — SRS to message mapping

| SRS         | Text fragment                                                              | Label                                          |
|-------------|----------------------------------------------------------------------------|------------------------------------------------|
| REQ-DM-090  | Persist all configuration changes to non-volatile storage                  | IMPLIES MESSAGE (write to ConfigStore)         |
| REQ-MB-100  | Address multiple field devices by unique slave address                     | IMPLIES MESSAGE (one profile per slave)        |
| REQ-MB-110 (proposed) | Maintain a configurable registry of device profiles               | IMPLIES PARTICIPANT (`DeviceProfileRegistry`)  |
| REQ-MB-111 (proposed) | Each profile contains identifier, description, address, register map | IMPLIES MESSAGE PAYLOAD (UML note)         |
| UC-16       | Steps 2–5: technician sets parameters, system validates, confirms, persists| IMPLIES MESSAGE × 4                            |

### 15.2 Step 2 — Participants

| #  | Lifeline                  | Stereotype    | Board | Role                                                   |
|----|---------------------------|---------------|-------|--------------------------------------------------------|
| 1  | `Field Technician`        | «actor»       | local | Provisions the gateway via CLI                         |
| 2  | `ConsoleService`          | «application» | GW    | CLI command parser and prompt                          |
| 3  | `ConfigService`           | «application» | GW    | Validates and persists                                 |
| 4  | `ConfigStore`             | «middleware»  | GW    | QSPI-backed config storage                             |
| 5  | `DeviceProfileRegistry`   | «application» | GW    | Holds device profiles                                  |
| 6  | `Logger`                  | «middleware»  | GW    | Records validation failures (P4 exception)             |

### 15.3 Step 3 — Message table

| #   | From                | To                       | Type   | Message text                            | Guard / fragment             | Timing | Trace          |
|-----|---------------------|--------------------------|--------|-----------------------------------------|------------------------------|--------|----------------|
| 1   | `Field Technician`  | `ConsoleService`         | async  | connect to console                      |                              |        | UC-16 step 1   |
| —   |                     |                          |        |                                         | **loop** `[more parameters]` |        | UC-16 step 2   |
| 2   |   `Field Technician`| `ConsoleService`         | async  | set parameter (key, value)              |                              |        | UC-16 step 2   |
| 3   |   `ConsoleService`  | `ConfigService`          | sync   | apply parameter                         |                              |        |                |
| 4   |   `ConfigService`   | `ConfigService`          | self   | validate parameter                      |                              |        | DM-001         |
| —   |                     |                          |        |                                         | **alt**                      |        | DM-001         |
| —   |                     |                          |        |                                         | `[validation failed]`        |        |                |
| 5'  |     `ConfigService` | `Logger`                 | async  | log validation failure                  |                              |        | DM-001         |
| 6'  |     `ConfigService` | `ConsoleService`         | return | reject (validation)                     |                              |        |                |
| 7'  |     `ConsoleService`| `Field Technician`       | async  | display error                           |                              |        |                |
| —   |                     |                          |        |                                         | `[validation ok]`            |        |                |
| 5   |     `ConfigService` | `ConsoleService`         | return | request confirmation                    |                              |        | UC-16 step 3   |
| 6   |     `ConsoleService`| `Field Technician`       | async  | display confirmation prompt             |                              |        | UC-16 step 3   |
| 7   |     `Field Technician`| `ConsoleService`       | async  | confirm or decline                      |                              |        | UC-16 step 4   |
| —   |                     |                          |        |                                         | **alt**                      |        |                |
| —   |                     |                          |        |                                         | `[confirmed]`                |        |                |
| 8   |       `ConsoleService`| `ConfigService`        | sync   | commit parameter                        |                              |        | UC-16 step 5   |
| 9   |       `ConfigService`| `ConfigStore`           | sync   | persist                                 |                              |        | DM-090         |
| —   |                     |                          |        |                                         | **opt** `[parameter is profile]` |    | MB-110         |
| 10  |         `ConfigService`| `DeviceProfileRegistry`| sync  | add or update profile                   |                              |        | MB-110         |
| —   |                     |                          |        |                                         | **end opt**                  |        |                |
| 11  |       `ConfigService`| `ConsoleService`        | return | ok                                      |                              |        |                |
| 12  |       `ConsoleService`| `Field Technician`     | async  | display success                         |                              |        |                |
| —   |                     |                          |        |                                         | `[declined]`                 |        |                |
| 8'' |       `ConsoleService`| `Field Technician`     | async  | display "discarded"                     |                              |        | UC-16 E2       |
| —   |                     |                          |        |                                         | **end alt**                  |        |                |
| —   |                     |                          |        |                                         | **end alt**                  |        |                |
| —   |                     |                          |        |                                         | **end loop**                 |        |                |
| 13  | `Field Technician`  | `ConsoleService`         | async  | exit console                            |                              |        |                |

**UML note 1 (parameter scope):** *"Parameters set during provisioning include: WiFi credentials, MQTT broker endpoint and X.509 client certificate, gateway serial number, Modbus bus parameters (baud, parity, stop bits), telemetry / health / alarm intervals, alarm thresholds, and **device profiles** (one per expected slave). The same provisioning flow handles all parameter types via the parameter-key dispatch in `ConfigService`."*

**UML note 2 (device profile content):** *"Each device profile contains: device identifier (probed at link establishment), device description (human-readable label for logs and cloud telemetry), slave address (Modbus RTU bus address), and a register-map specification (LLD detail — see `modbus-register-map.md`). The profile is the single source of truth for what the gateway expects from each slave."*

**UML note 3 (provisioned state):** *"Once provisioning is complete, the gateway is ready to boot per SD-00b. An unprovisioned gateway (no MQTT credentials, no device profiles) detects this state during SD-00b's `load configuration` step and remains in a CLI-only mode until provisioning completes — it does not start `ModbusPoller` or `CloudPublisher` against incomplete configuration."*

### 15.4 Decisions specific to SD-10

| # | Decision | Rationale |
|---|----------|-----------|
| SD10-D1 | One unified provisioning flow for all parameter types | Same validate / confirm / persist pattern; profile-update branch is `opt` |
| SD10-D2 | Device profile is a first-class provisioning parameter | Reflects industry-standard device-profile pattern (EDS / GSD equivalents) |
| SD10-D3 | Provisioning loop for multiple parameters | Matches realistic CLI use (technician sets multiple parameters in one session) |
| SD10-D4 | Unprovisioned gateway remains in CLI-only mode at boot | Defends against the gateway making outbound connections (Modbus, MQTT) with incomplete or default configuration |
| SD10-D5 | `Logger` appears as a lifeline (P4 exception) | Validation failures during provisioning must be auditable |

---

## 16. Cross-diagram relationships

| Source        | References                  | Relationship                                                                                                              |
|---------------|-----------------------------|---------------------------------------------------------------------------------------------------------------------------|
| SD-00b, SD-00c | state-machines.md §7.2     | Realises the LifecycleController state machine's Init phase                                                              |
| SD-00b        | SD-04b (`ref`)              | Cloud connection at boot uses TLS handshake from reconnect path                                                          |
| SD-00c        | SD-06d (`ref`)              | Post-update boot resumes self-check directly                                                                             |
| SD-03a, SD-03b | SD-04 (architectural only) | Sunny path of SD-03 is the connected steady state; rainy paths are owned by SD-04                                        |
| SD-04a, SD-04b| SD-03                       | The drain in SD-04b re-publishes messages using the same path as SD-03 (preserving QoS and topic)                         |
| SD-05         | SD-02 (`ref`)               | Modbus pickup of the alarm flag uses the standard polling cycle                                                          |
| SD-05         | SD-04a (`ref`)              | Rainy operand: alarm enqueued via store-and-forward when cloud is disconnected                                           |
| SD-06b        | SD-04a, SD-04b (`ref`)      | Mid-download disconnect handled by the standard buffering and reconnect machinery                                        |
| SD-06a–d      | state-machines.md §7.4      | Each sub-diagram covers a slice of the same Firmware Update state machine along the time axis                            |
| SD-07         | SD-00b (`ref` to probe logic) | Profile-update branch re-probes the affected slave using the same logic as link establishment                          |
| SD-08         | SD-00b (`ref`)              | Post-restart self-check follows cold-boot path                                                                           |
| SD-09         | (none — standalone)         | Triggered by boot (SD-00b) and by post-reconnect events (SD-04b)                                                         |
| SD-10         | SD-00b                      | Provisioning is a precondition for SD-00b's successful boot                                                              |
| SD-02         | state-machines.md §7.5      | The `link_up` and `node_offline` events emitted by SD-02 feed the Modbus Master state machine                            |
| SD-04         | state-machines.md §7.3      | SD-04 is the runtime view of the Cloud Connectivity sub-machine's transitions                                            |
| All           | components.md               | Lifelines are components (with §3.1.1 / §3.1.2 stereotyped exceptions)                                                   |

---

## 17. Decisions adopted in this artefact

These are the cross-cutting decisions; each sub-diagram has its own decision table.

| #   | Decision                                                                                                  | Rationale                                                                                                          |
|-----|-----------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| D1  | HLD sequences are component-level only; LLD content (queues, mutexes, function signatures) is excluded    | Mirrors the state-machine convention                                                                               |
| D2  | Cross-cutting Logger and HealthMonitor are elided from sequences by default (P4); included where logging *is* the rainy behaviour | Diagrams stay readable                                                                              |
| D3  | Fragments encoded in the message table via delimiter rows (§3.4)                                          | Keeps tables flat and linear-readable while preserving fragment structure                                          |
| D4  | Rainy paths are first-class                                                                               | Failure handling is part of the architecture, not an afterthought                                                  |
| D5  | SD-04 owns all cloud-disconnect handling; SD-03 and SD-05 reference it rather than redrawing              | Single source of truth                                                                                             |
| D6  | SD-06 decomposed into four sub-diagrams (SD-06a–d) per P9                                                 | Single-diagram size would exceed the readability budget                                                            |
| D7  | Reboots rendered as single messages plus a labelled lifeline discontinuity                                | Consistent with state-machines.md treatment of reboots as transition actions                                       |
| D8  | QoS levels visibly distinguished                                                                          | Reflects MQTT contract                                                                                             |
| D9  | Activation bars carry duration constraints sourced from REQ-NF-* requirements where present              | Makes timing requirements falsifiable from the diagram                                                              |
| D10 | Lifelines named identically to `components.md` entries; no invented names                                 | Traceability and consistency                                                                                       |
| D11 | Bare SRS IDs in the `Trace` column                                                                        | Less visual noise; consistent with state-machines.md                                                                |
| D12 | `WifiDriver` and `ModbusUartDriver` shown as passthrough lifelines                                        | Makes the layering visible without exploding the message count                                                     |
| D13 | Timer realisation: RTOS software timer with event-style propagation; no software delays for periodic triggers | Per architectural preference; keeps consumers decoupled from the timing source                                |
| D14 | Master-slave link establishment via per-slave probe with profile-bound device-ID validation; fall-through to Operational on failure (Option C) | Industry-standard "deny by default"; supports REQ-NF-203 5 s boot budget                       |
| D15 | Self-check probes in SD-06d are serial sync, not `par`                                                    | "Boring is correct"; aggregate <500 ms within 10 s budget; avoids over-engineering                                  |
| D16 | Bootloader treated as a boundary actor with `«bootloader»` stereotype                                     | Bootloader is out of HLD application scope; its interactions *are* in scope                                        |
| D17 | Per-slave link state with profile binding (supersedes earlier global link-state notion)                   | Supports REQ-MB-100 operationally; allowlist is a derived view                                                     |
| D18 | Device profile registry as a first-class architectural concept                                            | Industry-standard pattern (EDS / GSD equivalent); decouples register-map knowledge from firmware code              |
| D19 | Pull-based downstream consumption documented via UML notes rather than additional diagrams                | Consumers read on their own schedules; redrawing each consumer would clutter the artefact                          |
| D20 | Event-driven dispatch within a single dispatch context (e.g., AlarmService FD on post-acquisition event) is consistent with pull-based access | Event triggers access; data flows via pull (P7)                          |

---

## 18. Open questions (deferred to LLD or to follow-up SRS amendments)

### 18.1 Deferred to LLD

| #   | Question                                                                                                | Where it surfaces  | Defer because                                                                  |
|-----|---------------------------------------------------------------------------------------------------------|--------------------|--------------------------------------------------------------------------------|
| Q1  | Chunk size for OTA download (adaptive or fixed)                                                         | SD-06b              | Depends on QSPI page size, MQTT message size limit, and broker constraints     |
| Q2  | Per-channel alarm clearance flow with hysteresis (REQ-AM-010, REQ-AM-011)                                | SD-05 (out of scope)| Per-channel state machine deferred per state-machines.md open questions         |
| Q3  | Buffer max capacity (REQ-BF-020 [TBD])                                                                  | SD-04a              | Depends on QSPI partition layout                                                |
| Q4  | RTC sync interval (REQ-NF-210, REQ-NF-211 [TBD])                                                        | SD-09               | Depends on measured RTC drift during integration testing                        |
| Q5  | Self-check post-rollback final state (Idle vs Failed-with-operator-ack)                                  | SD-06d              | State-machine refinement                                                        |
| Q6  | How does `LifecycleController` distinguish a clean boot from a post-update boot when no flags are set?  | SD-00c              | Negative-case state-machine design                                              |
| Q7  | Modbus exception decoding internals                                                                     | SD-02               | Internal to `ModbusMaster` middleware                                           |
| Q8  | Modbus register-map specification format inside device profiles                                         | SD-00b, SD-10       | Schema design (register address, type, scaling, units, access mode)             |
| Q9  | Consumer schedules for `ISensorReadings` (LCD refresh rate, Modbus poll rate, AlarmService cadence)     | SD-01 UML note      | Component-level scheduling                                                      |
| Q10 | Watchdog handling — is hardware watchdog in scope?                                                       | SD-04a, SD-06b      | Not in current SRS; needs scope confirmation                                    |
| Q11 | Power-loss handling during QSPI writes (other than OTA commit, already handled)                         | SD-04a, SD-06b      | Failure-modes analysis at LLD                                                   |
| Q12 | Concurrent alarm publishes when multiple channels alarm in the same poll cycle                          | SD-05               | LLD coordination logic in `AlarmService` (GW)                                   |
| Q13 | Local CLI command coverage (UC-15 CLI variant)                                                          | (no diagram)        | If in scope, would be SD-11 — Local CLI command                                 |
| Q14 | `IHealthReport` push-side traffic (middleware → HealthMonitor)                                           | SD-03b UML note     | Visible only at LLD; HLD note suffices                                          |

### 18.2 Proposed SRS amendments

This artefact discovered gaps in the SRS. Proposed for a follow-up SRS PR:

| Proposed REQ ID       | Text                                                                                                | Motivation             |
|-----------------------|-----------------------------------------------------------------------------------------------------|------------------------|
| REQ-MB-110 (proposed) | The gateway shall maintain a configurable registry of device profiles, one per expected field-device | SD-00b, SD-10          |
| REQ-MB-111 (proposed) | Each device profile shall contain: device identifier, device description, slave address, and a register-map specification | SD-00b, SD-10 |
| REQ-MB-120 (proposed) | The gateway shall verify the device identifier received from a slave against the profile bound to its address during link establishment, and reject mismatches | SD-00b |
| REQ-MB-130 (proposed) | The gateway shall log device-profile validation failures with the offending identifier and address | SD-00b              |
| REQ-DM-100 (proposed) | The system shall accept device-profile updates via the provisioning mechanism                       | SD-07, SD-10           |
| REQ-DM-101 (proposed) | The system shall trigger re-probing of affected slaves when a device profile is added or updated at runtime | SD-07          |

### 18.3 SRS interpretation flag

| #   | Question                                                                                              | Where it surfaces |
|-----|-------------------------------------------------------------------------------------------------------|-------------------|
| Q-M7 | REQ-NF-106's 200 ms anchor: "from data ready" or "from publish-trigger event"?                        | SD-03a, SD-03b   |

The current diagrams interpret REQ-NF-106 as "200 ms from publish-trigger event." A literal reading of the SRS could also support "200 ms from data ready" (i.e., from when SensorService publishes to ISensorReadings). The literal reading would be impossible to satisfy because data is typically ready ~60 s before the next publish trigger. Recommend the SRS author clarify; document the interpretation here to make it traceable.

---

## 19. Drawing checklist (per diagram)

- [ ] Step 1 SRS table complete; every requirement labelled.
- [ ] Step 2 participant list matches `components.md` exactly (copy-paste, do not retype).
- [ ] Step 3 message table complete; every numbered row has a trace.
- [ ] Lifeline order matches first-appearance order on the happy path.
- [ ] Cross-board diagrams have a vertical bus boundary annotated with the medium.
- [ ] Stereotypes set on every lifeline.
- [ ] Synchronous vs asynchronous arrowhead distinction correct.
- [ ] Return messages drawn only where a value flows back.
- [ ] Fragments (`alt`, `loop`, `opt`, `par`, `ref`, `break`) used per §3.4.
- [ ] Fragment delimiter rows in the message table; sub-numbered IDs (`4'`, `5'`) on rainy operands.
- [ ] Indentation in the `From` cell reflects nesting depth (two spaces per level).
- [ ] Timing constraints attached where SRS requires them.
- [ ] Cross-cutting components (`Logger`, `HealthMonitor`) elided unless central; UML note added if elided.
- [ ] Colour palette applied per §3.7; VP theme set explicitly per diagram.
- [ ] Diagram fits on one page with ≤ 25 messages (P9). Split if not.
- [ ] Each rainy path either drawn inline (within an `alt`) or `ref`'d to the diagram that owns it.
- [ ] Each `ref` to another diagram named exactly as in the inventory (§2).
- [ ] Trace from each numbered message to one or more SRS IDs documented.

**Two checkpoints per diagram during drawing:**

1. **After lifelines placed (before any messages):** verify participant list and order against §X.2 (Step 2). Five minutes here saves an hour of re-routing later.
2. **After all messages placed (before fragments and constraints):** verify message order and arrowhead style against §X.3 (Step 3). Adding fragments to wrong-ordered messages compounds error.
