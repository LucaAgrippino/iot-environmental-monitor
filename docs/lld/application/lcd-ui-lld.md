# LLD Companion — LcdUi

**Companion document to `hld.md`.** This artefact specifies the software
design of the `LcdUi` Application component on the Field Device — screen
model, navigation, per-screen logic, the configuration sub-state machine,
pending-vs-committed value handling, concurrency, sizing, and test plan.

The splash screen (REQ-LD-200..-240) is **explicitly out of scope** here.
It is owned by `LifecycleController` during Init, which drives it directly
via `GraphicsLibrary`. `LcdUi` takes over once the system enters
Operational.

**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** LcdUi in `components.md` (FD application layer)
---

## 1. Purpose and scope

### 1.1 In scope

- The **four runtime screens**: Sensor, Status, Alarm, Config.
- The **Strategy pattern** screen dispatch model (`screen_t` interface).
- Per-screen refresh logic (pull-on-tick from providers).
- Touch routing model (LVGL widget-level + navigation tab bar).
- The **configuration sub-state machine** (Viewing → Editing → Confirming).
- Pending-vs-committed value handling (REQ-LD-140).
- Persistence trigger via `IConfigManager` on Confirm.
- Alarm-screen ordering and "no alarms" indicator (REQ-LD-090).
- "Waiting for data" display (REQ-LD-060) and error indicators
  (REQ-LD-040).
- Concurrency model, memory sizing, initialisation order, test plan.

### 1.2 Out of scope

- **Splash screen** (REQ-LD-200..-240) — owned by `LifecycleController`.
- **LVGL internals** (draw buffer, flush callback, tick source) — owned by
  `GraphicsLibrary` LLD (`graphics-library.md` companion).
- **Framebuffer / SDRAM layout** — owned by `LcdDriver` LLD.
- **CLI provisioning** — owned by `ConsoleService`.
- **LED state** (Operational / Alarm / Faulted indicators) — owned by
  `HealthMonitor`.

### 1.3 Terminology

- **Screen** — a full-display view navigated via the top tab bar.
- **Sub-state** — an internal state of the Config screen; not a
  top-level lifecycle concept.
- **Pending buffer** — working copy of editable parameters held during
  editing; discarded on cancel or navigation-away.
- **Committed buffer** — mirror of the last successfully applied
  configuration values; used to populate the Config screen on entry.

---

## 1. Sources

Per `components.md` (FD Application layer):

| Field | Value |
|---|---|
| **Name** | `LcdUi` |
| **Layer** | Application |
| **Board** | Field Device only |
| **Provides (upward)** | *(none — top of the stack)* |
| **Uses (downward)** | `IGraphics`, `ISensorService`, `IAlarmService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `ILogger` |
| **Hosted in task** | `LcdUiTask` (priority 2, 1024 words / 4 KB stack) |

### 2.1 Patterns applied

- **Strategy** — each screen exposes the same `screen_t` shape
  (function-pointer struct); `LcdUi` drives it through a single current
  pointer, making screen addition transparent to the dispatcher.
- **Pull-based data access** (P7) — `on_refresh` pulls fresh values from
  providers at each 200 ms tick; no push subscriptions, no caches inside
  `LcdUi`.
- **ISP** (P3) — reads configuration via `IConfigProvider`; writes via
  `IConfigManager`. Two separate interface pointers.

---

## 3. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Screen navigation | REQ-LD-000 | UC-01, UC-02, UC-03, UC-15 |
| Sensor screen — readings, units, timestamp | REQ-LD-010, LD-020, LD-030 | UC-01 |
| Sensor screen — error indicator | REQ-LD-040 | UC-01 |
| Sensor screen — refresh rate, "waiting" | REQ-LD-050, LD-060 | UC-01 |
| Status screen | REQ-LD-070 | UC-02 |
| Alarm screen — list, no-alarm message | REQ-LD-080, LD-090 | UC-03 |
| Config screen — display, validate, confirm, apply, persist | REQ-LD-100, LD-110, LD-120, LD-130, LD-140, LD-150, LD-0E1 | UC-15 |
| Display refresh rate (5 Hz) | REQ-NF-108 | — |

---

## 4. Screen model

### 4.1 Screen set

| ID | Screen | Tab label | Owners section |
|---|---|---|---|
| `SCR_SENSOR` | Sensor readings | "Sensors" | §6.1 |
| `SCR_STATUS` | System status / health | "Status" | §6.2 |
| `SCR_ALARM` | Active alarms | "Alarms" | §6.3 |
| `SCR_CONFIG` | Editable configuration | "Config" | §6.4 |

### 4.2 Navigation graph

```
         ┌─────────┐     tab tap     ┌─────────┐
         │ Sensor  │◄───────────────►│ Status  │
         └────┬────┘                 └────┬────┘
              │          tab tap          │
         tab tap                      tab tap
              │                          │
         ┌────▼────┐     tab tap     ┌───▼─────┐
         │  Alarm  │◄───────────────►│ Config  │
         └─────────┘                 └─────────┘

 Navigation is always available EXCEPT when Config is in
 EDITING or CONFIRMING sub-state, in which case the tab bar
 is disabled and the user must Cancel or Confirm first.
```

### 4.3 Static screen allocation

All four screen objects are constructed in `lcd_ui_init()` and remain
allocated for the lifetime of the system. Navigation uses LVGL's
`lv_scr_load()` to switch active screens rather than creating and
destroying widgets on each navigation.

**Rationale:** P8 — static allocation eliminates LVGL heap churn,
fragmentation risk, and per-navigation latency. Total widget object
overhead for four screens is bounded at init; sizing is in §14.

---

## 5. Strategy interface — `screen_t`

Defined in `application/include/lcd_ui.h` (accessible only to
`lcd_ui.c`; not exposed to other components):

```c
typedef struct screen screen_t;

struct screen {
    void (*on_enter)(screen_t *self);
    /* Called when this screen becomes the active screen.
     * Re-populate widget values from providers and reset any
     * internal sub-state.                                           */

    void (*on_exit)(screen_t *self);
    /* Called before navigating away. Must discard any pending state
     * (relevant to ConfigScreen: discard pending buffer, reset
     * sub-state to VIEWING).                                        */

    void (*on_refresh)(screen_t *self);
    /* Called every 200 ms by LcdUiTask. Pull fresh data from
     * providers and update widget text/values.
     * For non-active screens: NOT called (dispatcher skips).       */
};
```

The four concrete screen contexts embed `screen_t` as their first member,
enabling safe casting between `screen_t *` and the concrete type.

```c
typedef struct { screen_t base; /* ... */ } sensor_screen_t;
typedef struct { screen_t base; /* ... */ } status_screen_t;
typedef struct { screen_t base; /* ... */ } alarm_screen_t;
typedef struct { screen_t base; /* ... */ } config_screen_t;
```

---

## 6. Screen catalogue

### 6.1 Sensor Screen (REQ-LD-010..-060)

**Purpose:** Display the three sensor readings (temperature, humidity,
pressure) with value, unit, timestamp, and per-sensor validity indicator.

#### Widget layout (800×480, portrait implied left-to-right)

```
┌────────────────────────────────────────────────┐
│  [ Sensors ] [ Status ] [ Alarms ] [ Config ]  │  ← tab bar (top)
├────────────────────────────────────────────────┤
│  Temperature    23.4 °C   ✓   2024-01-15 10:03 │
│  Humidity       61.2 %    ✓   2024-01-15 10:03 │
│  Pressure      1013 hPa   ✓   2024-01-15 10:03 │
│                                                │
│  [Waiting for data...]  ← shown until first ok │
└────────────────────────────────────────────────┘
```

#### Widget inventory

| Widget | LVGL type | Updated on |
|---|---|---|
| Per-sensor value label (×3) | `lv_label` | `on_refresh` |
| Per-sensor unit label (×3) | `lv_label` | `on_enter` (static text) |
| Per-sensor timestamp label (×3) | `lv_label` | `on_refresh` |
| Per-sensor validity icon (×3) | `lv_label` (✓ / ✗) | `on_refresh` |
| "Waiting for data" overlay | `lv_label` | `on_refresh` (hidden after first valid reading) |

#### on_refresh algorithm

```
sensor_screen_on_refresh(self):
    reading = sensor_service->get_latest(sensor_service)
    if reading.valid == false AND self.first_valid_received == false:
        show waiting_label; return
    hide waiting_label
    self.first_valid_received = true
    for each sensor (temp, humidity, pressure):
        if reading.sensor[i].valid:
            set value_label text = format(reading.sensor[i].value)
            set validity_icon   = "✓"
        else:
            set value_label text = "--"   /* REQ-LD-040 error indicator */
            set validity_icon   = "✗"
        set timestamp_label text = format(reading.timestamp)
```

Units are set in `on_enter` and never change at runtime (°C, %, hPa are
fixed for this project). Per REQ-LD-030.

---

### 6.2 Status Screen (REQ-LD-070)

**Purpose:** Display FD-specific health metrics for the Field Technician.

#### FD metric subset

REQ-LD-070 is a shared requirement (written generically). The FD subset
excludes WiFi RSSI, reconnection count, MQTT failure count, and buffer
occupancy — those are Gateway metrics. The FD exposes:

| Metric | Source interface | Field |
|---|---|---|
| Stack high watermarks (per task) | `IHealthSnapshot` | `stack_hwm_*` |
| Free heap | `IHealthSnapshot` | `free_heap_bytes` |
| CPU load estimate | `IHealthSnapshot` | `cpu_load_pct` |
| Modbus CRC errors | `IHealthSnapshot` | `modbus_crc_errors` |
| Modbus timeout count | `IHealthSnapshot` | `modbus_timeouts` |
| Modbus success count | `IHealthSnapshot` | `modbus_successes` |
| Uptime | `IHealthSnapshot` | `uptime_s` |
| MCU die temperature | `IHealthSnapshot` | `mcu_temp_c` |

This interpretation is noted here as a documentation decision, not a
requirement amendment. If the SRS is later clarified to require all
metrics on all boards, this screen will need to be revisited.

#### on_refresh algorithm

```
status_screen_on_refresh(self):
    snap = health_snapshot->get(health_snapshot)
    update all label widgets from snap fields
```

All metric rows use `lv_label` widgets, updated in bulk on each tick.

---

### 6.3 Alarm Screen (REQ-LD-080..-090)

**Purpose:** List active alarms with timestamp. Show "No active alarms"
when none are present.

#### Ordering

Alarms are listed most-recently-raised first (reverse-chronological).
The `IAlarmService.get_active()` call returns a fixed-length array;
`LcdUi` sorts by `raised_at` timestamp descending before rendering.
*(Confirmed decision per LCD-O5.)*

#### "No active alarms" indicator

If `get_active()` returns a count of zero, the alarm list is hidden and
a centred "No active alarms" label is shown (REQ-LD-090).

#### Widget strategy

LVGL's `lv_list` widget. At `on_enter`, the list is cleared and rebuilt
from the active alarm snapshot. On each `on_refresh` tick, the list is
refreshed the same way.

**Note:** LVGL's list widget dynamically adds/removes items. This is
acceptable because `on_refresh` at 200 ms is low-frequency, and a full
list clear-and-rebuild for ≤10 active alarms is within budget.
If the alarm count grows large, a table widget with pre-allocated rows
is the alternative. Tracked as **LCD-O6** (open item).

#### on_refresh algorithm

```
alarm_screen_on_refresh(self):
    alarms = alarm_service->get_active(alarm_service, &count)
    lv_list_clean(list_widget)
    if count == 0:
        show no_alarms_label; return
    hide no_alarms_label
    sort alarms by raised_at descending
    for i in 0..count-1:
        text = format("Sensor %s [%s] @ %s",
                      alarms[i].sensor_id,
                      alarms[i].type,  /* HIGH / LOW */
                      format_time(alarms[i].raised_at))
        lv_list_add_text(list_widget, text)
```

---

### 6.4 Config Screen (REQ-LD-100..-150)

**Purpose:** Display configurable parameters and allow the Field
Technician to edit, validate, and apply them with confirmation. Retains
previous configuration until all new parameters are successfully applied.

#### Editable parameters (per REQ-LD-100)

```c
typedef struct {
    uint32_t polling_interval_ms;      /* sensor sampling rate   */
    int16_t  temp_alarm_hi_deci_c;     /* temperature thresholds */
    int16_t  temp_alarm_lo_deci_c;
    uint16_t humidity_alarm_hi_pct;    /* humidity thresholds    */
    uint16_t humidity_alarm_lo_pct;
    uint16_t pressure_alarm_hi_hpa;    /* pressure thresholds    */
    uint16_t pressure_alarm_lo_hpa;
    uint8_t  display_brightness_pct;   /* display settings       */
    uint16_t screen_timeout_s;
} lcd_ui_editable_params_t;
```

#### Sub-state machine

```
                  ┌─────────────┐
         ─────►   │   VIEWING   │ ◄──────────────────────────────┐
                  └──────┬──────┘                                │
                         │ field tapped                          │
                         ▼                                       │
                  ┌─────────────┐   apply tapped   ┌────────────┴───┐
                  │   EDITING   │─────────────────► │  CONFIRMING   │
                  └──────┬──────┘                   └──────┬────────┘
                         │ exit screen                     │ confirm or
                         │ (tab navigate away)             │ cancel or timeout
                         ▼                                 ▼
                  discard pending                    → VIEWING
```

| Event | From | To | Action |
|---|---|---|---|
| `field_tapped` | VIEWING | EDITING | Copy `committed` → `pending`; enable spinboxes; show Cancel/Apply buttons |
| `value_changed` (valid) | EDITING | EDITING | Update `pending` field; clear error indicator |
| `value_changed` (invalid) | EDITING | EDITING | Show inline error label; reject value (REQ-LD-110) |
| `apply_tapped` | EDITING | CONFIRMING | Show confirmation dialog; start 30 s countdown (LCD-O2) |
| `confirm_tapped` | CONFIRMING | VIEWING | Call `IConfigManager.apply_block(&pending)`; on OK: `committed = pending`; on fail: log + show error toast; discard pending |
| `cancel_tapped` | CONFIRMING | VIEWING | Discard `pending` (REQ-LD-0E1); reset spinboxes to `committed` |
| `timeout` (30 s) | CONFIRMING | VIEWING | Same as `cancel_tapped` |
| `on_exit` | EDITING / CONFIRMING | — | Discard `pending`; reset to VIEWING; hide dialog |
| `on_exit` | VIEWING | — | No-op |

#### Pending-vs-committed split (REQ-LD-140)

```c
typedef struct {
    screen_t              base;
    cfg_screen_state_t    sub_state;
    lcd_ui_editable_params_t committed;   /* last successfully applied */
    lcd_ui_editable_params_t pending;     /* working copy in EDITING   */
    lv_obj_t             *spinbox[N_EDITABLE_FIELDS];
    lv_obj_t             *err_label[N_EDITABLE_FIELDS];
    lv_obj_t             *confirm_dialog;
    lv_timer_t           *confirm_timeout_timer;
    /* provider handles */
    const IConfigProvider *cfg_read;
    IConfigManager        *cfg_write;
    ILogger               *log;
} config_screen_t;
```

#### on_enter algorithm

```
config_screen_on_enter(self):
    self.sub_state = CFG_STATE_VIEWING
    /* Snapshot current config into committed (REQ-LD-140 baseline) */
    self.committed.polling_interval_ms = cfg_read->get_polling_interval()
    /* ... (all fields) */
    /* Pre-populate spinboxes from committed */
    for each field i:
        lv_spinbox_set_value(spinbox[i], committed[i])
        hide err_label[i]
    disable spinboxes  /* read-only in VIEWING */
    hide Cancel/Apply buttons
    hide confirm_dialog
```

#### on_refresh algorithm

```
config_screen_on_refresh(self):
    if sub_state != CFG_STATE_VIEWING:
        return  /* hands are full — don't overwrite pending edits */
    /* Refresh committed from provider in case another path changed config */
    /* (e.g. Modbus write from Gateway) */
    config_screen_on_enter(self)  /* re-snapshot and re-populate */
```

**Note:** Calling `on_enter` from `on_refresh` while in VIEWING is safe
because VIEWING implies no pending edits and no open dialogs. This avoids
duplicated snapshot logic.

#### apply_block call and error handling

`IConfigManager.apply_block()` validates all fields in the block and
persists via ConfigStore. On failure:
- Log at error level with the failing field identifier.
- Show an error toast on screen.
- Discard `pending`.
- Return to VIEWING with `committed` unchanged — the old values are still
  active on the device (REQ-LD-140 satisfied).

---

## 7. Navigation

### 7.1 Tab bar

Implemented as an LVGL `lv_tabview` widget created at init. Each tab
contains the corresponding screen's root `lv_obj_t`.

Tab-change callback (`tab_change_cb`):

```c
static void tab_change_cb(lv_event_t *e)
{
    uint16_t new_idx = lv_tabview_get_tab_act(tabview);
    screen_t *new_scr = &screens[new_idx];

    if (current == new_scr) { return; }

    /* Block navigation while Config is in EDITING or CONFIRMING */
    if (current == (screen_t *)&config_screen &&
        config_screen.sub_state != CFG_STATE_VIEWING) {
        /* Revert the tab selection — LVGL allows this in the event cb */
        lv_tabview_set_act(tabview, current_tab_index, LV_ANIM_OFF);
        show_toast("Save or cancel changes first.");
        return;
    }

    current->on_exit(current);
    current = new_scr;
    current_tab_index = new_idx;
    current->on_enter(current);
}
```

### 7.2 Active screen dispatch

`LcdUiTask` main loop:

```c
void lcd_ui_task(void *arg)
{
    lcd_ui_t *ui = (lcd_ui_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LCD_REFRESH_MS)); /* 200 ms */

        graphics->tick(graphics);       /* drives lv_tick_inc + lv_timer_handler */
        ui->current->on_refresh(ui->current); /* pull fresh data */
    }
}
```

`LCD_REFRESH_MS = 200` (5 Hz, REQ-NF-108).

`graphics->tick()` is `IGraphics`'s handle for LVGL's time-tick and
handler; the call runs all LVGL timers including the display flush — per
`graphics-library.md` companion (GL-O2 resolution).

---

## 8. Touch dispatch

LcdUi does **not** intercept raw touch events. LVGL routes them at the
widget level:

- Navigation: `lv_tabview` widget callback → `tab_change_cb` (§7.1).
- Config screen spinbox changes: LVGL spinbox `LV_EVENT_VALUE_CHANGED`
  callback → `config_field_changed_cb` (internal to `config_screen_t`).
- Config Apply/Cancel buttons: LVGL button `LV_EVENT_CLICKED` callbacks.
- Confirm/Cancel dialog buttons: LVGL button `LV_EVENT_CLICKED` callbacks.

All callbacks are registered at `lcd_ui_init()` and run in `LcdUiTask`
context when `lv_timer_handler()` is called inside `graphics->tick()`.
No ISR touch; no additional task switches.

---

## 9. "Waiting for data" and error indicators

### 9.1 Waiting for data (REQ-LD-060)

`SensorScreen` holds a `bool first_valid_received` flag initialised to
`false`. On each `on_refresh`:

- If `first_valid_received == false` and `reading.valid == false`, the
  "Waiting for data..." label is shown and the value rows are hidden.
- Once any reading with `valid == true` is received, `first_valid_received`
  is set to `true` and the label is permanently hidden.

The flag is reset to `false` in `on_enter` so that re-entering the Sensor
tab after a prolonged sensor failure shows the waiting message again.

### 9.2 Per-sensor error indicator (REQ-LD-040)

For each sensor row, `on_refresh` checks `reading.sensor[i].valid`:
- Valid → value displayed normally, validity icon = `"✓"` (green).
- Invalid → value = `"--"`, validity icon = `"✗"` (red).

Units and timestamp labels remain visible on invalid readings; the
timestamp reflects when the sensor last attempted a read (per
`ISensorService` reading struct contract).

---

## 3. Internal design

### 10.1 Single-task ownership

All `LcdUi` state and all LVGL calls run exclusively in `LcdUiTask`.
LVGL is **not** thread-safe; calling it from any other task would require
a mutex wrapping every LVGL call — an unacceptable overhead and complexity.
`LcdUiTask` is the sole LVGL executor.

### 10.2 Data access

`on_refresh` calls provider getters (`get_latest()`, `get_active()`,
`get()`, `get_snapshot()`). These are brief, mutex-guarded reads inside
each provider. `LcdUi` holds no lock while doing so; each call is
independent.

`IConfigManager.apply_block()` (called only from `tab_change_cb` in
`LcdUiTask`) is also a brief, mutex-guarded write inside `ConfigService`.

### 10.3 Internal state

`LcdUi` instance state (`current`, `current_tab_index`) and per-screen
state (`sub_state`, `pending`, `committed`) are accessed only within
`LcdUiTask`. No mutex needed.

---

### Principles applied

- **P1 (Strict directional layering).** Depends on middleware interfaces (IGraphics, ITouchscreen) and application-layer peer services (ISensorService, IAlarmService, IConfigProvider, ITimeProvider); no layer is skipped.
- **P2 (Dependency Inversion).** All consumed dependencies injected as vtable pointers; LcdUi does not include any concrete middleware or driver module header.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Screen layout and widget state in static structs; framebuffer owned by GraphicsLibrary / LcdDriver; no heap.
- **P6 (Responsibility traces to requirements).** Every screen and navigation path traces to REQ-LD-* display and REQ-SA-* alarm-display requirements.
- **P7 (Pull-based downstream consumption).** LcdUi polls ISensorService and IAlarmService on its task cadence (LcdUiTask); neither producer pushes updates to it.
- **P8 (Total error propagation, no silent failures).** `lcd_ui_err_t` on init and screen-switch operations; render errors reported via IHealthReport and logged.
- **P9 (BARR-C coding standard).** Pixel coordinates `uint16_t`; screen-index `uint8_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `lcd_ui_`; errors `LCD_UI_ERR_*`.


## 2. Public API

`LcdUi` provides no interface to other components (it is at the top of
the Application stack). Its public API is limited to lifecycle management:

```c
/* application/include/lcd_ui.h */

typedef struct lcd_ui lcd_ui_t;

lcd_ui_err_t lcd_ui_init(lcd_ui_t             *self,
                         IGraphics            *graphics,
                         const ISensorService *sensors,
                         const IAlarmService  *alarms,
                         const IConfigProvider *cfg_read,
                         IConfigManager       *cfg_write,
                         const IHealthSnapshot *health,
                         ILogger              *log);

void lcd_ui_task_body(lcd_ui_t *self);   /* passed as xTaskCreate pvParameters */
```

The `ILifecycle` interface (owned by `LifecycleController`) may query
`LcdUi` for whether a configuration edit is in progress before allowing
a soft restart — this would be an `lcd_ui_is_editing()` query function.
This interaction is tracked as **LCD-O7** (open item): currently
`LifecycleController` does not block restart on active LCD edits. If this
is deemed necessary, `lcd_ui_is_editing()` can be added without
architectural impact.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

```c
typedef enum {
    LCD_UI_OK = 0,
    LCD_UI_ERR_NULL_ARG,
    LCD_UI_ERR_GRAPHICS_INIT,
    LCD_UI_ERR_PROVIDER_NULL,
} lcd_ui_err_t;
```

`lcd_ui_init()` returns `LCD_UI_ERR_GRAPHICS_INIT` if the underlying
`IGraphics.init()` call fails. This causes `LifecycleController` to
transition to Faulted (REQ-LD-000 — LCD is essential).

At runtime, provider getter failures (e.g., sensor read returns invalid)
surface as sentinel display values (§9.2), not as fatal errors.
`IConfigManager.apply_block()` failure surfaces as a UI toast and is
logged; the system continues in Operational with the previous config.

---

## 13. Initialisation order

Called from `LifecycleController` after the `BringingUpLCD` sub-step
completes (start-gate event group cleared):

1. `graphics->init()` — LVGL initialised, framebuffer mapped, display up.
2. `lcd_ui_init()` — all four screen objects constructed; tab bar created;
   widget hierarchies built; `on_enter` called for `SCR_SENSOR` (default).
3. `LcdUiTask` created; blocked on start-gate bit until `StartingMiddleware`
   sub-step completes.
4. Start-gate released by `LifecycleController` → `LcdUiTask` unblocks and
   enters the 200 ms refresh loop.

---

## 14. Memory and sizing

### 14.1 Static context (RAM)

| Item | Size (estimate) |
|---|---|
| `lcd_ui_t` (pointers to screens + providers) | ~64 B |
| `sensor_screen_t` (6 widget handles + 1 flag) | ~32 B |
| `status_screen_t` (10 widget handles) | ~48 B |
| `alarm_screen_t` (1 list handle + 1 label handle) | ~16 B |
| `config_screen_t` (spinboxes + labels + 2 × params structs + sub-state) | ~256 B |
| **Total `.bss`** | **~416 B** |

No dynamic allocation via `malloc`/`free`. LVGL's internal heap is a
fixed pool (sized in `graphics-library.md`); widget objects are allocated
from it at `lcd_ui_init()` and never freed.

### 14.2 LVGL widget budget

| Screen | Estimated widget count |
|---|---|
| Sensor | 15 (3 × {value, unit, timestamp, icon} + 1 waiting label + 1 root) |
| Status | 18 (9 metric rows × 2 labels + 1 root) |
| Alarm | 5 (1 list + 1 no-alarms label + 1 root + 2 structural) |
| Config | 30 (9 fields × {label, spinbox, err_label} + buttons + dialog + root) |
| Tab bar | 5 (tabview + 4 tab containers) |
| **Total** | **~73 widgets** |

At ~40 B per LVGL object (v8.x estimate), this is ~2.9 KB of the LVGL
pool. Well within the 8 KB pool size established in
`graphics-library.md`.

### 14.3 Stack usage

`LcdUiTask` stack: 1024 words (4 KB) per `task-breakdown.md` §4.2.
LVGL callbacks run in this task via `lv_timer_handler()`; they do not add
a separate stack frame. Peak stack depth occurs during
`config_screen_on_enter()` when all spinboxes are populated sequentially —
estimated < 512 B.

---

## 7. Unit-test plan

### 15.1 Unit tests — `tests/application/test_lcd_ui.c`

Unity host-side tests. All providers mocked. LVGL is **not** linked in
unit tests; LVGL calls are intercepted via a thin mock layer that records
call arguments (label text set, spinbox value, widget visibility).

| Suite | Coverage |
|---|---|
| Init | Null-pointer rejection; correct screen created (sensor active by default) |
| Sensor screen — happy path | `on_refresh` with valid reading → correct label texts and validity icons |
| Sensor screen — first-data waiting | `on_refresh` with invalid reading and `first_valid_received = false` → waiting label shown |
| Sensor screen — post-first valid | `on_refresh` with subsequent invalid reading → `--` shown, no waiting label |
| Sensor screen — error indicator | Per-sensor `valid = false` → `"✗"` icon for that sensor only |
| Status screen — happy path | `on_refresh` → all metric labels updated from `IHealthSnapshot` |
| Alarm screen — no alarms | `get_active()` returns count 0 → no-alarms label shown, list hidden |
| Alarm screen — active alarms | `get_active()` returns 3 alarms → list rebuilt, sorted most-recent first |
| Config screen — VIEWING refresh | `on_refresh` in VIEWING calls `on_enter` equivalent, updates spinboxes |
| Config screen — EDITING entry | `field_tapped` event → sub_state = EDITING; spinboxes enabled; pending = committed |
| Config screen — EDITING valid input | `value_changed` with in-range value → pending updated; no error label |
| Config screen — EDITING invalid input | `value_changed` with out-of-range value → pending unchanged; error label shown |
| Config screen — apply flow | `apply_tapped` → sub_state = CONFIRMING; dialog shown; timer started |
| Config screen — confirm | `confirm_tapped` → `IConfigManager.apply_block` called with pending values; committed updated; sub_state = VIEWING |
| Config screen — cancel | `cancel_tapped` → pending discarded; spinboxes = committed; sub_state = VIEWING |
| Config screen — timeout | Timer fires at 30 s → same as cancel |
| Config screen — ConfigManager failure on apply | `apply_block` returns error → toast shown; committed unchanged; sub_state = VIEWING |
| Config screen — exit while EDITING | `on_exit` → pending discarded; sub_state = VIEWING |
| Navigation block | Tab-change while EDITING → tab reverted; toast shown |
| Navigation happy path | Tab-change while VIEWING → `on_exit` then `on_enter` called correctly |

### 15.2 Integration tests — on target

| Test | Setup |
|---|---|
| Full config edit cycle | Field Technician edits a threshold via LCD; verify `ConfigStore` persists new value; verify `SensorService` uses new threshold on next comparison |
| Cancel discards changes | Edit a threshold, tap Cancel; verify `ConfigStore` retains original value |
| Alarm screen reaction | Drive sensor reading above threshold; verify Alarm tab shows new alarm within one poll cycle |
| Navigation blocking | Enter EDITING on Config tab; tap another tab; verify navigation is blocked |
| "Waiting for data" | Boot with sensor driver returning invalid; verify Sensor screen shows waiting message; simulate first valid reading; verify message disappears |

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| **LCD-O1** | Editable-field widget: spinbox chosen per scope brief. Confirm behaviour on large range (e.g., pressure alarm hi 800–1100 hPa) — may need explicit step size configuration per spinbox. |
| **LCD-O2** | Confirming-state timeout: **30 s** (provisional, matching LC-O4). Implemented via `lv_timer_create()` in LcdUiTask context. |
| **LCD-O3** | Refresh skip on identical data — per-widget last-value cache recommended as a nice-to-have for CPU efficiency; deferred to integration phase. |
| **LCD-O4** | StatusScreen FD metric subset — documented as interpretation here; flag for SRS clarification if REQ-LD-070 is amended to distinguish FD vs GW. |
| **LCD-O5** | *(Resolved)* Alarm ordering: most-recently-raised first. |
| **LCD-O6** | AlarmScreen list scaling: current `lv_list` rebuild approach acceptable for ≤10 alarms. If alarm count can exceed 20, switch to pre-allocated `lv_table` rows. Revisit at integration. |
| **LCD-O7** | Soft-restart blocking on active Config edit: `lcd_ui_is_editing()` query function not yet defined. If `LifecycleController` must block restart while Config is in EDITING / CONFIRMING, add this function in a follow-up. |

---

## 17. References

- `docs/hld.md` §5.2 (FD data flow view), §5.5 (health telemetry view).
- `docs/components.md` (FD application layer — `LcdUi`, `GraphicsLibrary`,
  `LifecycleController`).
- `docs/state-machines.md` Machine 5 (FD lifecycle — Operational I3).
- `docs/task-breakdown.md` §4.2 (`LcdUiTask` definition).
- `docs/lld/graphics-library.md` (defines `IGraphics` and LVGL pool sizing).
- `docs/lld/config-service.md` (defines `IConfigProvider` / `IConfigManager`).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot`).
- `docs/architecture-principles.md` P3 (ISP), P5 (task responsibility),
  P7 (pull-based), P8 (static allocation).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
