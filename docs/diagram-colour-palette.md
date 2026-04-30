# UML Diagram Colour Palette

**Project:** IoT Environmental Monitoring Gateway
**Tool:** Visual Paradigm Community Edition
**Purpose:** Consistent visual language across all UML diagrams in the project.

---

## 1. Node / Subsystem Colours

These are the **primary identity colours**. Every element belonging to a subsystem uses the corresponding colour family. Applied across all diagram types.

| Subsystem           | Fill        | Border      | Text     | Usage                                                  |
|---------------------|-------------|-------------|----------|--------------------------------------------------------|
| Field Device        | `#DAEAF6`   | `#7BAFD4`   | `#1A3A52`| STM32F469 — anything owned by the field device node    |
| Gateway             | `#D5F0DE`   | `#6FBF8E`   | `#1A4231`| B-L475E-IOT01A — anything owned by the gateway node    |
| Cloud / External    | `#FDE8C8`   | `#E0A84C`   | `#4A3318`| AWS IoT Core, dashboards, external services            |
| Communication Link  | `#E0D4F5`   | `#9B7FCC`   | `#2E1F4A`| Modbus RTU, MQTT, UART bus, protocol-level elements    |
| Actor / User        | `#E8E8E8`   | `#888888`   | `#2A2A2A`| Human actors (technician, operator, end user)          |

---

## 2. Software Layer Colours

Used in **component diagrams** and **class diagrams** to distinguish architectural layers within each node.

These layers are frequently **nested** inside one another (e.g., Drivers inside Middleware inside Application, all inside the Node boundary). The shades are chosen with **large contrast gaps** between adjacent layers so boundaries remain clearly visible when nested.

**Rule:** darker shade = closer to hardware. The darkest layer sits at the bottom of the stack.

### Field Device (Blue Family)

| Layer               | Fill        | Border      | Text        | Usage                                        |
|---------------------|-------------|-------------|-------------|----------------------------------------------|
| Application         | `#DAEAF6`   | `#7BAFD4`   | `#1A3A52`   | App tasks, business logic, state machine     |
| Middleware          | `#A3C7E2`   | `#5088B5`   | `#1A3A52`   | FreeRTOS wrappers, Modbus stack, display mgr |
| Drivers             | `#6A9DC4`   | `#3A7098`   | `#E8F2FA`   | Sensor drivers, UART driver, GPIO, LCD driver|
| Hardware / BSP      | `#3E7EA6`   | `#245A78`   | `#DAEAF6`   | Register-level access, HAL calls, MCU config |

### Gateway (Green Family)

| Layer               | Fill        | Border      | Text        | Usage                                        |
|---------------------|-------------|-------------|-------------|----------------------------------------------|
| Application         | `#D5F0DE`   | `#6FBF8E`   | `#1A4231`   | App tasks, data aggregation, cloud publish   |
| Middleware          | `#9DD9B5`   | `#4AA56E`   | `#1A4231`   | FreeRTOS wrappers, Modbus master, MQTT client|
| Drivers             | `#5FBA88`   | `#2E8B5A`   | `#E8F8EE`   | Sensor drivers, UART driver, WiFi driver     |
| Hardware / BSP      | `#3F9A6C`   | `#1C6B42`   | `#D5F0DE`   | Register-level access, HAL calls, MCU config |

### Nesting Guidance

When drawing component diagrams with nested layers:

```
┌─────────────────────────────── Node boundary (subsystem colour) ──┐
│  ┌──────────────────────────── Application (lightest) ──────────┐ │
│  │  ┌─────────────────────── Middleware ─────────────────────┐   │ │
│  │  │  ┌──────────────────── Drivers ─────────────────────┐  │  │ │
│  │  │  │  ┌───────────────── Hardware / BSP (darkest) ──┐ │  │  │ │
│  │  │  │  └─────────────────────────────────────────────┘ │  │  │ │
│  │  │  └──────────────────────────────────────────────────┘  │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────┘
```

- The **node boundary** uses the subsystem colour (e.g., `#DAEAF6` for Field Device).
- Each nested layer must be **visibly distinct** from its parent. The fills have been chosen so
  that each step is at least 20% darker than the previous one.
- Use **at least 8px padding** between nested rectangles so borders do not merge.
- Borders on inner layers should be **1.5pt solid**, not dashed, to stay visible against the parent fill.

---

## 3. State Machine Colours

### Principle

Per §8: **colour = subsystem ownership, shade = state classification within that subsystem.**

State machine diagrams follow the same rule as every other diagram in the
project. Each state machine belongs to exactly one board (Gateway, in green;
or Field Device, in blue) and inherits that board's colour family. State
*classification* (Initialising, Running, Faulted) is conveyed through
**shade** within the family, not through cross-subsystem colours.

### Field Device State Palette (Blue Family)

| State Classification         | Fill        | Border      | Text        | Usage                                                       |
|------------------------------|-------------|-------------|-------------|-------------------------------------------------------------|
| Normal / Running             | `#DAEAF6`   | `#7BAFD4`   | `#1A3A52`   | Operational, Idle (slave waiting), any steady-state         |
| Initialising / Transient     | `#A3C7E2`   | `#5088B5`   | `#1A3A52`   | Init sub-states, Connecting, Validating, Applying, etc.     |
| Faulted                      | `#DAEAF6`   | `#CC4444`   | `#1A3A52`   | Faulted state — border replaced with red, border weight 2pt |
| Composite container          | `#EFF6FB`   | `#7BAFD4`   | `#1A3A52`   | The outer box of a composite state (e.g. Init)              |

### Gateway State Palette (Green Family)

| State Classification         | Fill        | Border      | Text        | Usage                                                       |
|------------------------------|-------------|-------------|-------------|-------------------------------------------------------------|
| Normal / Running             | `#D5F0DE`   | `#6FBF8E`   | `#1A4231`   | Operational, Connected.Publishing, Idle, etc.               |
| Initialising / Transient     | `#9DD9B5`   | `#4AA56E`   | `#1A4231`   | Init sub-states, Connecting, Validating, Applying, etc.     |
| Faulted                      | `#D5F0DE`   | `#CC4444`   | `#1A4231`   | Faulted state — border replaced with red, border weight 2pt |
| Composite container          | `#EBF7F0`   | `#6FBF8E`   | `#1A4231`   | The outer box of a composite state (e.g. Init, Connected)   |

### Composite State Rule

When a state contains sub-states (Gateway `Init`, `Connected`; Field Device
`Init`), the **outer composite box uses the lighter "Composite container" fill**, and the
**inner sub-states use the standard fills above**. This guarantees the
outer-vs-inner contrast required by the nesting rule in §8.

### Sub-machine Reference States

When a top-level state delegates to a sub-machine (e.g. `UpdatingFirmware`
on the Gateway lifecycle delegates to the Firmware Update sub-machine):

- The state appears on the parent diagram as a **simple state** (not
  composite) using the standard `Initialising / Transient` shade.
- Add a UML stereotype label **`«submachine»`** above the state name.
- Add a **double border** (1.5pt outer + 1.5pt inner, 2px gap) to signal
  "expand elsewhere". Visual Paradigm supports this directly.
- The sub-machine's own diagram lives in a separate `.vpd` and uses the
  full palette of its owning subsystem.

### Pseudo-states

Standard UML conventions, applied uniformly across both subsystems:

| Pseudo-state    | Rendering                                              |
|-----------------|--------------------------------------------------------|
| Initial         | Filled black circle, ~12px diameter                    |
| Final           | Concentric circles (filled inner)                      |
| Choice          | Diamond, fill `#F2F2F2`, border `#666666`              |
| Junction        | Small filled circle `#666666`                          |
| "MCU reset" final | Final-state circle with annotation label "MCU reset (→ Init)", placed adjacent |

The "MCU reset" pseudo-state is project-specific. It marks transitions whose
action triggers an immediate reboot (Restarting → reset, Faulted → reset,
Firmware Update Applying/RollingBack → reset). Visually it is a UML final
state with a textual annotation — not a new colour.

### Transition Arrows

Neutral grey, regardless of source/target subsystem. This stays readable
against both blue and green fills without competing with state colours.

| Transition Type            | Stroke      | Weight | Style    |
|----------------------------|-------------|--------|----------|
| State-changing (default)   | `#555555`   | 1.5 pt | solid    |
| Internal (compartment text)| —           | —      | listed inside state box, not drawn as arrow |
| Composite-boundary         | `#555555`   | 1.5 pt | solid    |
| Reboot-triggering          | `#555555`   | 1.5 pt | solid; action label includes "NVIC_SystemReset()" |

### Internal Transition Compartment

When a state has internal transitions listed inside it (e.g. Operational
with its 15 internal transitions), the compartment is rendered as plain
text inside the state box, separated from the entry/do/exit compartment
by a horizontal line at `#999999`, 1pt.

If the count exceeds what fits readably (typical threshold ~6–8 lines):

- List internal-transition references as `I1, I2, ..., In` only inside
  the state box.
- Reference the full table in `state-machines.md` Step 3.
- Note this in a small annotation below the state: *"see Step 3 — internal
  transitions Ix..Iy"*.

### Guards and Actions

| Element                    | Colour      | Style                                  |
|----------------------------|-------------|----------------------------------------|
| Event name                 | `#333333`   | regular                                |
| Guard `[expression]`       | `#333333`   | italic                                 |
| Action `/ method()`        | `#333333`   | regular, prefixed with `/`             |

### Choice of Subsystem When the Diagram Belongs to Both

Each of the six state machines in this project is owned by exactly one
subsystem — there is no ambiguity. See `state-machines.md` Part C for the
ownership map. If a future diagram models behaviour spanning both boards
(e.g. an end-to-end protocol diagram), use the **Communication / Protocol
purple family** from §1, not blue or green.

---

## 4. Sequence Diagram Colours

Lifelines inherit the **node/subsystem colour** of the component they represent.

| Element                  | Colour                                   | Usage                             |
|--------------------------|------------------------------------------|-----------------------------------|
| Field Device lifeline    | `#DAEAF6` fill, `#7BAFD4` border         | Any field device component        |
| Gateway lifeline         | `#D5F0DE` fill, `#6FBF8E` border         | Any gateway component             |
| Cloud lifeline           | `#FDE8C8` fill, `#E0A84C` border         | AWS IoT Core, broker              |
| Actor lifeline           | `#E8E8E8` fill, `#888888` border         | Human actors                      |
| Synchronous message      | `#444444` solid, filled arrowhead        | Function calls                    |
| Asynchronous message     | `#444444` solid, open arrowhead          | Queued messages, MQTT publish     |
| Return message           | `#888888` dashed                         | Return values                     |
| Error / failure message  | `#CC6666` dashed                         | Error paths                       |
| Activation bar           | Matches lifeline fill                    | Active processing period          |
| Alt/Loop frame           | `#F7F7F7` fill, `#AAAAAA` border        | Conditional and loop fragments    |

---

## 5. Use Case Diagram Colours

| Element                  | Fill        | Border                  | Text        | Usage                              |
|--------------------------|-------------|-------------------------|-------------|------------------------------------|
| System boundary          | `#F8F8F8`  | `#BBBBBB` (dashed)      | `#555555`   | Overall system or subsystem scope  |
| Field Device use cases   | `#DAEAF6`  | `#7BAFD4`               | `#1A3A52`   | Use cases belonging to field device|
| Gateway use cases        | `#D5F0DE`  | `#6FBF8E`               | `#1A4231`   | Use cases belonging to gateway     |
| Cloud use cases          | `#FDE8C8`  | `#E0A84C`               | `#4A3318`   | Use cases involving cloud services |
| Shared / cross-cutting   | `#E0D4F5`  | `#9B7FCC`               | `#2E1F4A`   | Use cases spanning multiple nodes  |
| Actor (human)            | `#E8E8E8`  | `#888888`               | `#2A2A2A`   | Stick figures                      |
| Actor (system)           | `#F2F2F2`  | `#999999`               | `#2A2A2A`   | Rectangular system actors          |

---

## 6. Deployment Diagram Colours

| Element                  | Fill        | Border      | Text        | Usage                              |
|--------------------------|-------------|-------------|-------------|------------------------------------|
| Field Device node        | `#DAEAF6`  | `#7BAFD4`   | `#1A3A52`   | STM32F469 Discovery board          |
| Gateway node             | `#D5F0DE`  | `#6FBF8E`   | `#1A4231`   | B-L475E-IOT01A board               |
| Cloud node               | `#FDE8C8`  | `#E0A84C`   | `#4A3318`   | AWS IoT Core, cloud services       |
| Artefact (firmware)      | `#F7F7F7`  | `#AAAAAA`   | `#555555`   | Deployed binary, configuration     |
| UART / Modbus link       | `#9B7FCC` solid line                     | —           | Physical wired connection          |
| WiFi / MQTT link         | `#E0A84C` dashed line                    | —           | Wireless connection                |

---

## 7. Domain Model / Data Flow Colours

| Element                  | Fill        | Border      | Text        | Usage                              |
|--------------------------|-------------|-------------|-------------|------------------------------------|
| Sensor data entity       | `#DAEAF6`  | `#7BAFD4`   | `#1A3A52`   | Raw and processed sensor readings  |
| Configuration entity     | `#FFF2CC`  | `#D4A84C`   | `#4A3318`   | Thresholds, device config, register map |
| System status entity     | `#D5F0DE`  | `#6FBF8E`   | `#1A4231`   | Device state, health, diagnostics  |
| Cloud message entity     | `#FDE8C8`  | `#E0A84C`   | `#4A3318`   | MQTT payload, telemetry packet     |
| Data flow arrow          | `#555555` solid, 1.5 pt                 | —           | Direction of data movement         |

---

## 8. General Rules

### Typography
- **Diagram titles:** Bold, 14 pt, `#2A2A2A`
- **Element labels:** Regular, 11 pt, use the Text colour from the corresponding table
- **Annotations / notes:** 10 pt, italic, `#555555`
- **Note boxes:** `#FFFDE7` fill (pale yellow), `#CCCC88` border, `#555555` text

### Lines and Borders
- **Default border weight:** 1.5 pt
- **Association / dependency lines:** 1.0 pt, `#555555`
- **Composition / aggregation:** 1.5 pt, `#444444`
- **Dashed lines (dependency, async):** 1.0 pt, dash pattern 6-3

### Text Colour Rule
- **Light fills** (Application, Middleware layers and all pastel subsystem colours): use the **dark text** colour from the same row.
- **Dark fills** (Drivers, Hardware/BSP layers): use the **lightest fill** from the same colour family. Never use plain white (`#FFFFFF`).

### Nesting Contrast Rule
> When element B is nested inside element A, their fills must differ by at least **20% perceived brightness**.
> If two adjacent layers look too similar on screen, darken the inner one further.
> Always test nesting on screen before committing a diagram — monitors vary.

### Backgrounds
- **Diagram background:** `#FFFFFF` (white)
- **Package / group background:** `#FAFAFA` with `#DDDDDD` border

### Consistency Principle
> The **colour** of any element tells you **which subsystem owns it** at a glance.
> The **shade** tells you **which architectural layer** it belongs to.
> This rule applies to every diagram in the project without exception.

---

## 9. Quick Reference — Copy-Paste Hex Values

```
FIELD DEVICE FAMILY (Blue)       GATEWAY FAMILY (Green)
───────────────────────────      ───────────────────────────
App:  #DAEAF6 / #7BAFD4 txt:#1A3A52   App:  #D5F0DE / #6FBF8E txt:#1A4231
Mid:  #A3C7E2 / #5088B5 txt:#1A3A52   Mid:  #9DD9B5 / #4AA56E txt:#1A4231
Drv:  #6A9DC4 / #3A7098 txt:#E8F2FA   Drv:  #5FBA88 / #2E8B5A txt:#E8F8EE
BSP:  #3E7EA6 / #245A78 txt:#DAEAF6   BSP:  #3F9A6C / #1C6B42 txt:#D5F0DE

CLOUD / EXTERNAL (Amber)         COMMUNICATION (Purple)
───────────────────────────      ───────────────────────────
Fill: #FDE8C8 / #E0A84C txt:#4A3318   Fill: #E0D4F5 / #9B7FCC txt:#2E1F4A

ACTORS                           NOTES / ANNOTATIONS
───────────────────────────      ───────────────────────────
Human:  #E8E8E8 / #888888 txt:#2A2A2A   Fill: #FFFDE7 / #CCCC88 txt:#555555
System: #F2F2F2 / #999999 txt:#2A2A2A

STATE MACHINE
───────────────────────────
Idle:      #E8E8E8 / #999999 txt:#2A2A2A
Init:      #DAE4F6 / #7B9DD4 txt:#1A3A52
Running:   #D5F0DE / #6FBF8E txt:#1A4231
Warning:   #FFF2CC / #D4A84C txt:#4A3318
Error:     #F5D0D0 / #CC6666 txt:#5A1A1A
Shutdown:  #D9D0E8 / #8870AA txt:#2E1F4A

LINES
───────────────────────────
Default:   #555555
Returns:   #888888
Errors:    #CC6666
Strong:    #444444
```

---

*Last updated: March 2026*
