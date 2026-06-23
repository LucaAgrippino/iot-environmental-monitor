# LLD Companion — LcdUi

**Companion document to `hld.md`.** This artefact specifies the software
design of the `LcdUi` Application component on the Field Device — screen
model, navigation, per-screen logic, the configuration sub-state machine,
pending-vs-committed value handling, concurrency, sizing, and test plan.

The splash screen (REQ-LD-200..-240) is **explicitly out of scope** here.
It is owned by `LifecycleController` during Init, which drives it directly
via `GraphicsLibrary`. `LcdUi` takes over once the system enters
Operational.

**Version:** 0.2
**Date:** June 2026
**Status:** Pass-H ready

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
- **LVGL internals** (draw buffer, flush callback, tick source, indev
  poll) — owned by `GraphicsLibrary` LLD (`graphics-library.md`
  companion).
- **Framebuffer / SDRAM layout** — owned by `LcdDriver` LLD.
- **Touchscreen polling** — owned by `TouchscreenDriver`; consumed by
  `GraphicsLibrary` via its indev read callback.
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

## 2. Sources

Per `components.md` (FD Application layer):

| Field | Value |
|---|---|
| **Name** | `LcdUi` |
| **Layer** | Application |
| **Board** | Field Device only |
| **Provides (upward)** | *(none — top of the stack)* |
| **Uses (downward)** | `GraphicsLibrary` (direct API, no vtable per GL-D8), `ISensorService`, `IAlarmService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `IHealthReport`, `ILogger` |
| **Hosted in task** | `LcdUiTask` (priority 2, 1024 words / 4 KB stack — per `task-breakdown.md` §4.2) |

### 2.1 Patterns applied

- **Strategy** (screen dispatch) — each screen exposes the same
  `screen_t` shape (function-pointer struct); `LcdUi` drives it through
  a single current pointer, making screen addition transparent to the
  dispatcher.
- **CQRS-like separation** — configuration reads through
  `IConfigProvider`; writes through `IConfigManager`. Two separate
  interface pointers per ISP (P3).
- **Pull-based data access** (P7) — `on_refresh` pulls fresh values from
  providers at each 200 ms tick; no push subscriptions, no caches inside
  `LcdUi`.

### 2.2 Note on the GraphicsLibrary dependency

`GraphicsLibrary` exposes a **direct C API** (`graphics_init()`,
`graphics_process()`, `graphics_get_display()`, `graphics_get_indev()`)
rather than an `IGraphics` vtable. This is a documented deviation from
the middleware-vtable rule (decision **GL-D8** in
`graphics-library.md`); see that companion's §3 for rationale. `LcdUi`
calls these functions directly. The remaining downstream peers
(`ISensorService`, `IAlarmService`, etc.) are vtables as normal.

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

## 4. Public API

`LcdUi` provides no upward interface (it is at the top of the
Application stack). Its public API is limited to lifecycle:

```c
/* application/lcd_ui/lcd_ui.h */

typedef struct lcd_ui lcd_ui_t;

typedef enum {
    LCD_UI_ERR_OK             = 0,
    LCD_UI_ERR_INVALID_ARG    = 1, /**< NULL self or NULL required dep. */
    LCD_UI_ERR_ALREADY_INIT   = 2, /**< lcd_ui_init() called twice.     */
    LCD_UI_ERR_GRAPHICS_INIT  = 3, /**< graphics_init() returned non-OK.*/
} lcd_ui_err_t;

/**
 * @brief Initialise LcdUi: build the four screens, register tab-bar and
 *        widget event callbacks, enter SCR_SENSOR with the
 *        "Waiting for data" overlay shown.
 *
 * Must be called from the same task that will own LVGL — i.e.,
 * LcdUiTask — and AFTER graphics_init() has returned OK.
 *
 * @param[in,out] self      Static instance.
 * @param[in]     sensors   ISensorService (non-NULL).
 * @param[in]     alarms    IAlarmService  (non-NULL).
 * @param[in]     cfg_read  IConfigProvider (non-NULL).
 * @param[in,out] cfg_write IConfigManager  (non-NULL).
 * @param[in]     health    IHealthSnapshot (non-NULL).
 * @param[in,out] report    IHealthReport   (non-NULL — used to report
 *                          init faults).
 * @param[in,out] log       ILogger (non-NULL).
 * @return LCD_UI_ERR_OK on success.
 * @note Threading: LcdUiTask only.
 */
lcd_ui_err_t lcd_ui_init(lcd_ui_t              *self,
                         const ISensorService  *sensors,
                         const IAlarmService   *alarms,
                         const IConfigProvider *cfg_read,
                         IConfigManager        *cfg_write,
                         const IHealthSnapshot *health,
                         IHealthReport         *report,
                         ILogger               *log);

/**
 * @brief LcdUiTask body — runs the screen-render loop.
 *
 * Pre-condition: lcd_ui_init() has returned OK and graphics_init() has
 * been called.
 *
 * Loop body (period LCD_REFRESH_MS = 200 ms, REQ-NF-108):
 *   1. graphics_process()                 — flush dirty regions, run
 *                                            LVGL timers, poll indev.
 *   2. current->on_refresh(current)       — pull fresh provider data
 *                                            into widget values.
 *
 * @param[in,out] self  lcd_ui_t * cast to void * (xTaskCreate pattern).
 * @note Threading: entry point for LcdUiTask. Never call directly.
 */
void lcd_ui_task_body(void *self);
```

> **Open item LCD-O7** — a query function `lcd_ui_is_editing()` may be
> added later if `LifecycleController` needs to block soft-restart on
> active Config edits. Not in v0.2 scope.

---

## 5. Strategy interface — `screen_t`

Defined privately in `application/lcd_ui/screen_internal.h` (not exposed
to other components):

```c
typedef struct screen screen_t;

struct screen {
    void (*on_enter)(screen_t *self);
    /* Called when this screen becomes the active screen. Re-populate
     * widget values from providers and reset any internal sub-state.
     * Called once at lcd_ui_init() for the default screen (SCR_SENSOR).
     */

    void (*on_exit)(screen_t *self);
    /* Called before navigating away. Must discard any pending state
     * (relevant to ConfigScreen: discard pending buffer, reset
     * sub-state to VIEWING).                                          */

    void (*on_refresh)(screen_t *self);
    /* Called every LCD_REFRESH_MS by LcdUiTask. Pull fresh data from
     * providers and update widget text/values. Only the active screen
     * is refreshed; the dispatcher does not call on_refresh on
     * non-active screens.                                             */
};
```

The four concrete screen contexts embed `screen_t` as their first
member, enabling safe upcasting:

```c
typedef struct { screen_t base; /* ... */ } sensor_screen_t;
typedef struct { screen_t base; /* ... */ } status_screen_t;
typedef struct { screen_t base; /* ... */ } alarm_screen_t;
typedef struct { screen_t base; /* ... */ } config_screen_t;
```

**Note on touch events:** raw touch routing is **not** part of `screen_t`.
LVGL dispatches widget-level events (tab change, button click,
spinbox value-changed) directly to the callbacks registered at
`lcd_ui_init()` (see §9). The `screen_t` interface concerns only the
periodic refresh and lifecycle hooks; widget callbacks reach
screen-specific state via captured `lv_obj_t` user-data pointers.

---

## 6. Screen model

### 6.1 Screen set

| ID | Screen | Tab label | Owners section |
|---|---|---|---|
| `SCR_SENSOR` | Sensor readings | "Sensors" | §7.1 |
| `SCR_STATUS` | System status / health | "Status" | §7.2 |
| `SCR_ALARM` | Active alarms | "Alarms" | §7.3 |
| `SCR_CONFIG` | Editable configuration | "Config" | §7.4 |

### 6.2 Navigation graph

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

### 6.3 Static screen allocation

All four screen contexts (`sensor_screen_t`, `status_screen_t`,
`alarm_screen_t`, `config_screen_t`) are statically allocated in
`lcd_ui.c` as file-scope `static` instances. Construction in
`lcd_ui_init()` builds LVGL widget hierarchies for each. Navigation uses
LVGL's `lv_scr_load()` (or `lv_tabview` page selection) to switch active
screens — widgets are never created or destroyed at runtime.

**Rationale:** P5 / P8 — static allocation eliminates LVGL heap churn,
fragmentation risk, and per-navigation latency. Total widget object
overhead for four screens is bounded at init; sizing is in §13.

---

## 7. Screen catalogue

### 7.1 Sensor Screen (REQ-LD-010..-060)

**Purpose:** Display the three sensor readings (temperature, humidity,
pressure) with value, unit, timestamp, and per-sensor validity
indicator.

#### Widget layout (800×480, landscape)

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

#### Reading shape

`ISensorService.get_latest()` returns an aggregate per the
`SensorService` LLD. For this screen the relevant fields are:

```c
typedef struct {
    bool      valid;        /* aggregate validity */
    uint32_t  timestamp_ms; /* monotonic from RtcDriver */
    struct {
        bool   valid;       /* per-sensor */
        int32_t value;      /* deci-units (e.g. deci-°C) */
    } temp, humidity, pressure;
} sensor_reading_t;
```

(Exact shape lives in the `SensorService` LLD; this is the contract
consumed here.)

#### on_enter algorithm

```
sensor_screen_on_enter(self):
    self.first_valid_received = false   /* re-arm waiting overlay */
    set unit labels: "°C", "%", "hPa"
    show waiting_label, hide value/timestamp/icon rows
```

#### on_refresh algorithm

```
sensor_screen_on_refresh(self):
    reading = sensors->get_latest(sensors)
    if reading.valid == false AND self.first_valid_received == false:
        show waiting_label
        hide value/timestamp/icon rows
        return
    self.first_valid_received = true
    hide waiting_label
    show value/timestamp/icon rows
    for each sensor s in {temp, humidity, pressure}:
        if reading.s.valid:
            set value_label[s]    = format(reading.s.value)
            set validity_icon[s]  = "✓"
        else:
            set value_label[s]    = "--"   /* REQ-LD-040 */
            set validity_icon[s]  = "✗"
        set timestamp_label[s]   = format(reading.timestamp_ms)
```

Units are set in `on_enter` and never change at runtime
(°C, %, hPa fixed per REQ-LD-030).

---

### 7.2 Status Screen (REQ-LD-070)

**Purpose:** Display FD-specific health metrics for the Field
Technician.

#### FD metric subset

REQ-LD-070 is shared (written generically). The FD subset excludes
WiFi RSSI, reconnection count, MQTT failure count, and buffer occupancy
— those are Gateway-only metrics. The FD exposes:

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

This interpretation is a documentation decision, not a requirement
amendment. Flagged as **LCD-O4** open item.

#### on_refresh algorithm

```
status_screen_on_refresh(self):
    snap = health->get(health)
    update all metric labels from snap fields
```

All metric rows use `lv_label` widgets, updated in bulk on each tick.

---

### 7.3 Alarm Screen (REQ-LD-080..-090)

**Purpose:** List active alarms with timestamp. Show "No active alarms"
when none are present.

#### Ordering

Alarms are listed most-recently-raised first (reverse-chronological).
`IAlarmService.get_active()` returns a fixed-length array; `LcdUi` sorts
by `raised_at` timestamp descending before rendering.
*(Confirmed decision per LCD-O5.)*

#### "No active alarms" indicator

If `get_active()` returns count = 0, the alarm list is hidden and a
centred "No active alarms" label is shown (REQ-LD-090).

#### Widget strategy

LVGL's `lv_list` widget. At `on_enter` and on each `on_refresh`, the
list is cleared and rebuilt from the active alarm snapshot.

**Note:** `lv_list` items use LVGL's internal heap pool. Clear-and-rebuild
at 5 Hz for ≤10 alarms is within budget. If alarm counts grow large,
switch to a pre-allocated `lv_table`. Tracked as **LCD-O6**.

#### on_refresh algorithm

```
alarm_screen_on_refresh(self):
    alarms = alarms->get_active(alarms, &count)
    lv_list_clean(list_widget)
    if count == 0:
        show no_alarms_label
        hide list_widget
        return
    hide no_alarms_label
    show list_widget
    sort alarms by raised_at descending
    for i in 0..count-1:
        text = format("Sensor %s [%s] @ %s",
                      alarms[i].sensor_id,
                      alarms[i].type,        /* HIGH / LOW */
                      format_time(alarms[i].raised_at))
        lv_list_add_text(list_widget, text)
```

---

### 7.4 Config Screen (REQ-LD-100..-150)

**Purpose:** Display configurable parameters and allow the Field
Technician to edit, validate, and apply them with confirmation. Retains
previous configuration until all new parameters are successfully
applied.

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

`N_EDITABLE_FIELDS = 9`.

#### Sub-state machine

```
                  ┌─────────────┐
         ─────►   │   VIEWING   │ ◄──────────────────────────────┐
                  └──────┬──────┘                                │
                         │ field tapped                          │
                         ▼                                       │
                  ┌─────────────┐   apply tapped   ┌────────────┴───┐
                  │   EDITING   │─────────────────►│  CONFIRMING    │
                  └──────┬──────┘                  └──────┬─────────┘
                         │ exit screen                    │ confirm or
                         │ (tab navigate away)            │ cancel or
                         ▼                                │ timeout
                  discard pending                         ▼
                                                    → VIEWING
```

| Event | From | To | Action |
|---|---|---|---|
| `field_tapped` | VIEWING | EDITING | Copy `committed` → `pending`; enable spinboxes; show Cancel/Apply buttons |
| `value_changed` (valid) | EDITING | EDITING | Update `pending` field; clear error indicator |
| `value_changed` (invalid) | EDITING | EDITING | Show inline error label; reject value (REQ-LD-110) |
| `apply_tapped` | EDITING | CONFIRMING | Show confirmation dialog; start 30 s confirm timer |
| `confirm_tapped` | CONFIRMING | VIEWING | Call `cfg_write->apply_block(&pending)`; on OK: `committed = pending`; on fail: log + show error toast; discard pending |
| `cancel_tapped` | CONFIRMING | VIEWING | Discard `pending` (REQ-LD-0E1); reset spinboxes to `committed` |
| `timeout` (30 s) | CONFIRMING | VIEWING | Same as `cancel_tapped` |
| `on_exit` | EDITING / CONFIRMING | — | Discard `pending`; reset to VIEWING; hide dialog |
| `on_exit` | VIEWING | — | No-op |

#### Pending-vs-committed split (REQ-LD-140)

```c
typedef enum {
    CFG_STATE_VIEWING,
    CFG_STATE_EDITING,
    CFG_STATE_CONFIRMING
} cfg_screen_state_t;

typedef struct {
    screen_t                  base;
    cfg_screen_state_t        sub_state;
    lcd_ui_editable_params_t  committed;   /* last successfully applied */
    lcd_ui_editable_params_t  pending;     /* working copy in EDITING   */
    lv_obj_t                 *spinbox[N_EDITABLE_FIELDS];
    lv_obj_t                 *err_label[N_EDITABLE_FIELDS];
    lv_obj_t                 *confirm_dialog;
    lv_timer_t               *confirm_timeout_timer; /* see note below */
    /* provider handles */
    const IConfigProvider *cfg_read;
    IConfigManager        *cfg_write;
    ILogger               *log;
} config_screen_t;
```

**Note on `lv_timer`:** LVGL timer objects are allocated from LVGL's
internal heap pool (the same fixed pool that holds widget objects).
They are NOT C-heap allocations and do not violate P5/P8. The timer is
allocated once at `lcd_ui_init()` and reused — `lv_timer_pause()` /
`lv_timer_resume()` and `lv_timer_set_period()` reset cycle without
re-allocation. Tracked as **LCD-O2**.

#### on_enter algorithm

```
config_screen_on_enter(self):
    self.sub_state = CFG_STATE_VIEWING
    /* Snapshot current config into committed (REQ-LD-140 baseline) */
    self.committed.polling_interval_ms = cfg_read->get_polling_interval(cfg_read)
    /* ... (all fields) */
    /* Pre-populate spinboxes from committed */
    for each field i:
        lv_spinbox_set_value(spinbox[i], committed[i])
        hide err_label[i]
    disable spinboxes  /* read-only in VIEWING */
    hide Cancel/Apply buttons
    hide confirm_dialog
    lv_timer_pause(confirm_timeout_timer)
```

#### on_refresh algorithm

```
config_screen_on_refresh(self):
    if sub_state != CFG_STATE_VIEWING:
        return  /* hands are full — don't overwrite pending edits */
    /* Refresh committed from provider in case another path changed
       config (e.g. Modbus write from Gateway). */
    config_screen_on_enter(self)  /* re-snapshot and re-populate */
```

#### apply_block call and error handling

`cfg_write->apply_block()` validates all fields in the block and
persists via ConfigStore. On failure:
- Log at ERROR with the failing field identifier (`ILogger`).
- Show an error toast on screen.
- Discard `pending`.
- Return to VIEWING with `committed` unchanged — the old values are
  still active on the device (REQ-LD-140 satisfied).

---

## 8. Navigation

### 8.1 Tab bar

Implemented as an LVGL `lv_tabview` widget created at init. Each tab
contains the corresponding screen's root `lv_obj_t`.

Tab-change callback (`tab_change_cb`):

```c
static void tab_change_cb(lv_event_t *e)
{
    lcd_ui_t *ui = (lcd_ui_t *)lv_event_get_user_data(e);
    uint16_t  new_idx = lv_tabview_get_tab_act(ui->tabview);
    screen_t *new_scr = ui->screens[new_idx];

    if (ui->current == new_scr) { return; }

    /* Block navigation while Config is in EDITING or CONFIRMING */
    if (ui->current == (screen_t *)&ui->config_screen &&
        ui->config_screen.sub_state != CFG_STATE_VIEWING) {
        lv_tabview_set_act(ui->tabview, ui->current_tab_idx, LV_ANIM_OFF);
        show_toast(ui, "Save or cancel changes first.");
        return;
    }

    ui->current->on_exit(ui->current);
    ui->current = new_scr;
    ui->current_tab_idx = new_idx;
    ui->current->on_enter(ui->current);
}
```

### 8.2 Active screen dispatch

`LcdUiTask` body (per §4 public API):

```c
void lcd_ui_task_body(void *arg)
{
    lcd_ui_t *ui = (lcd_ui_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LCD_REFRESH_MS));

        (void)graphics_process();              /* per GL public API   */
        ui->current->on_refresh(ui->current);  /* pull fresh data     */
    }
}
```

`LCD_REFRESH_MS = 200` (5 Hz, REQ-NF-108).

`graphics_process()` runs `lv_task_handler()`, which drives the flush
callback (pixels → `LcdDriver`), the indev poll (touch →
`TouchscreenDriver`), and LVGL timer callbacks. The LVGL tick is
advanced by a separate 1 ms FreeRTOS software timer that calls
`graphics_tick_increment(1)` — owned by `GraphicsLibrary`, decision
**GL-D7**.

---

## 9. Touch dispatch

LcdUi does **not** intercept raw touch events. LVGL routes them at the
widget level via callbacks registered at `lcd_ui_init()`:

- Navigation: `lv_tabview` event → `tab_change_cb` (§8.1).
- Config screen spinbox changes: spinbox `LV_EVENT_VALUE_CHANGED`
  → `config_field_changed_cb`.
- Config Apply / Cancel buttons: button `LV_EVENT_CLICKED` callbacks.
- Confirmation-dialog Confirm / Cancel: button `LV_EVENT_CLICKED`
  callbacks.

All callbacks run in `LcdUiTask` context when `lv_task_handler()` is
called inside `graphics_process()`. No ISR touch; no additional task
switches; no mutex required (single-task LVGL ownership — see §11).

---

## 10. "Waiting for data" and error indicators

### 10.1 Waiting for data (REQ-LD-060)

`sensor_screen_t` holds a `bool first_valid_received` flag, initialised
to `false` in `on_enter` (§7.1).

- While `first_valid_received == false` AND `reading.valid == false`,
  the "Waiting for data..." overlay is shown; value/timestamp/icon rows
  are hidden.
- Once any reading with `valid == true` is received,
  `first_valid_received` is set to `true` and the overlay is hidden for
  the lifetime of the screen instance.
- Re-arming on prolonged sensor failure happens implicitly when the
  user navigates away and back: `on_exit` followed by `on_enter` resets
  the flag.

### 10.2 Per-sensor error indicator (REQ-LD-040)

For each sensor row, `on_refresh` checks `reading.<sensor>.valid`:
- Valid → value displayed normally, icon = `"✓"` (green).
- Invalid → value = `"--"`, icon = `"✗"` (red).

Units and timestamp labels remain visible on invalid readings; the
timestamp reflects when the sensor last attempted a read (per
`SensorService` LLD contract).

---

## 11. Internal design

### 11.1 Single-task ownership

All `LcdUi` state and all LVGL calls run exclusively in `LcdUiTask`.
LVGL is not thread-safe; calling it from any other task would require a
mutex around every LVGL call — an unacceptable overhead. `LcdUiTask` is
the sole LVGL executor (consistent with `graphics-library.md` §3
thread-safety model: GL mutex is held inside `graphics_process()`, but
`LcdUi` does not need to lock anything itself because no other task
ever touches LVGL).

### 11.2 Provider access

`on_refresh` calls provider getters (`sensors->get_latest`,
`alarms->get_active`, `health->get`, `cfg_read->get_*`). These are
brief, mutex-guarded reads inside each provider. `LcdUi` holds no lock
across these calls; each call is independent.

`cfg_write->apply_block()` (called only from button event callbacks in
`LcdUiTask`) is also a brief, mutex-guarded write inside
`ConfigService`.

### 11.3 Internal state

`lcd_ui_t` instance state (`current`, `current_tab_idx`) and per-screen
state (`sub_state`, `pending`, `committed`, `first_valid_received`) are
accessed only within `LcdUiTask`. No mutex needed.

### 11.4 Synchronisation summary

Caller serialises; component holds no FreeRTOS synchronisation
primitives.

---

## 12. Initialisation order

Called from `LifecycleController` per `state-machines.md` Machine 5
(BringingUpLCD → StartingMiddleware → Operational):

1. `graphics_init()` returns OK — LVGL initialised, framebuffer mapped,
   display up (per `graphics-library.md` §12).
2. `lcd_ui_init(self, sensors, alarms, cfg_read, cfg_write, health,
   report, log)` — screen contexts wired; tab bar created; widget
   hierarchies built; per-screen event callbacks registered;
   `confirm_timeout_timer` allocated and paused. `SCR_SENSOR` set as
   default current; `on_enter` called → "Waiting for data..." overlay
   shown (no provider reads yet — see step 4).
3. `LcdUiTask` created; blocks on the start-gate event group bit until
   `StartingMiddleware` sub-step completes.
4. `LifecycleController` releases the start-gate → `LcdUiTask` enters
   the 200 ms refresh loop; first `on_refresh` call reads providers,
   resolves "Waiting" once first valid sensor data arrives.

**Note on step 2:** `lcd_ui_init()` does **not** call providers (which
may not yet be initialised). The `on_enter` for `SCR_SENSOR` shows the
"Waiting" overlay and defers provider reads to the first
`on_refresh()`.

---

## 13. Memory and sizing

### 13.1 Static context (RAM, `.bss`)

| Item | Size (estimate) |
|---|---|
| `lcd_ui_t` (pointers to screens + providers + tab state) | ~80 B |
| `sensor_screen_t` (6 widget handles + 1 flag) | ~32 B |
| `status_screen_t` (10 widget handles) | ~48 B |
| `alarm_screen_t` (1 list handle + 1 label handle) | ~16 B |
| `config_screen_t` (spinboxes + labels + 2 × params structs + sub-state + timer ptr) | ~272 B |
| **Total `.bss`** | **~448 B** |

No dynamic `malloc`/`free`. LVGL's internal heap pool (sized in
`graphics-library.md`) holds widget objects, the confirm-timer object,
and the `lv_list` items — all allocated from it at `lcd_ui_init()` (or
on first list rebuild) and never freed back to the C heap.

### 13.2 LVGL widget budget

| Screen | Estimated widget count |
|---|---|
| Sensor | 15 (3 × {value, unit, timestamp, icon} + 1 waiting label + 2 structural) |
| Status | 18 (9 metric rows × 2 labels + 1 root) |
| Alarm | 5 (1 list + 1 no-alarms label + 1 root + 2 structural) |
| Config | 30 (9 fields × {label, spinbox, err_label} + Cancel + Apply + dialog + 2 dialog buttons + root) |
| Tab bar | 5 (tabview + 4 tab containers) |
| **Total** | **~73 widgets** |

At ~40 B per LVGL object (v8.x estimate), this is ~2.9 KB of the LVGL
pool. Well within the 8 KB pool size established in
`graphics-library.md`.

### 13.3 Stack usage

`LcdUiTask` stack: 1024 words (4 KB) per `task-breakdown.md` §4.2.
LVGL callbacks run in this task via `lv_task_handler()`; they do not
add a separate stack frame. Peak stack depth occurs during
`config_screen_on_enter()` when all spinboxes are populated
sequentially — estimated < 512 B.

---

## 14. Sequence integration

See `sequence-diagrams.md` for inter-component flows:

- **UC-15 Config edit cycle** — touch → spinbox event → state
  transition → apply_block → ConfigStore commit.
- **UC-03 Alarm raised** — SensorService → AlarmService update → next
  Alarm-screen on_refresh tick picks it up.

`LcdUi` is called synchronously from `LifecycleController` for init
only; runtime activity is task-internal.

---

## 15. Error and fault behaviour

All public functions return `lcd_ui_err_t`; callers must not ignore
non-OK returns. No retry is performed inside `LcdUi` —
`LifecycleController` handles init failures as Faulted (REQ-LD-000).

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `LCD_UI_ERR_INVALID_ARG` | NULL `self` or any NULL required dependency in `lcd_ui_init()` | No state mutated | Non-OK return | No — programming error | Logged at ERROR via `ILogger` |
| `LCD_UI_ERR_ALREADY_INIT` | `lcd_ui_init()` called twice on the same instance | No state mutated | Non-OK return | No — programming error | Logged at ERROR |
| `LCD_UI_ERR_GRAPHICS_INIT` | Pre-condition failure: `graphics_init()` not called or returned non-OK before `lcd_ui_init()` | No screens built | Non-OK return | No — `LifecycleController` enters Faulted | Logged at ERROR; `HEALTH_EVENT_LCD_FAIL` pushed to `IHealthReport` |
| **Runtime: `cfg_write->apply_block()` failure** | `ConfigStore` reject or persist fail | Discard `pending`; toast displayed; `sub_state` → VIEWING | n/a (handled internally) | Yes — user may retry from EDITING | Logged at ERROR with failing field id |

---

## 16. Unit-test plan

### 16.1 Host unit tests — `tests/application/test_lcd_ui.c`

Ceedling host-side tests. All providers mocked via CMock. LVGL is
**stubbed** (same pattern as `graphics-library.md` §8): a thin stub
records widget-state changes (label text set, spinbox value, widget
visibility) so tests can assert on rendering decisions without linking
real LVGL on the host.

Test IDs follow the `TC-LCDUI-NNN` convention.

| TC | Suite | Coverage |
|---|---|---|
| `TC-LCDUI-001` | Init | `lcd_ui_init(NULL, ...)` returns `LCD_UI_ERR_INVALID_ARG` |
| `TC-LCDUI-002` | Init | `lcd_ui_init` with any NULL dep returns `LCD_UI_ERR_INVALID_ARG` |
| `TC-LCDUI-003` | Init | Second call to `lcd_ui_init` returns `LCD_UI_ERR_ALREADY_INIT` |
| `TC-LCDUI-004` | Init | Happy-path init: sensor screen is current; waiting overlay visible |
| `TC-LCDUI-005` | Sensor — waiting | `on_refresh` with invalid reading and `first_valid_received=false` → waiting overlay shown |
| `TC-LCDUI-006` | Sensor — first valid | First valid reading hides overlay; value/icon/timestamp rendered |
| `TC-LCDUI-007` | Sensor — post-first invalid | After first valid, subsequent invalid reading → `--` shown, no waiting overlay |
| `TC-LCDUI-008` | Sensor — partial validity | Temperature valid, humidity invalid → `✓` and `✗` rendered per-row |
| `TC-LCDUI-009` | Sensor — re-arm waiting | `on_exit` then `on_enter` resets `first_valid_received` |
| `TC-LCDUI-010` | Status | `on_refresh` updates all FD metric labels from `IHealthSnapshot` |
| `TC-LCDUI-011` | Alarm — empty | `get_active()` count=0 → no-alarms label shown, list hidden |
| `TC-LCDUI-012` | Alarm — active | 3 alarms returned → list rebuilt, sorted by `raised_at` desc |
| `TC-LCDUI-013` | Alarm — list cleaned | Repeated `on_refresh` does not leak list items |
| `TC-LCDUI-014` | Config — VIEWING entry | `on_enter` populates spinboxes from `committed`; spinboxes disabled |
| `TC-LCDUI-015` | Config — field tap | `field_tapped` event → sub_state = EDITING; `pending = committed`; spinboxes enabled |
| `TC-LCDUI-016` | Config — valid edit | `value_changed` in range → `pending` updated; no error label |
| `TC-LCDUI-017` | Config — invalid edit | `value_changed` out of range → `pending` unchanged; error label shown |
| `TC-LCDUI-018` | Config — apply pressed | `apply_tapped` → sub_state = CONFIRMING; dialog shown; timer started |
| `TC-LCDUI-019` | Config — confirm | `confirm_tapped` → `cfg_write->apply_block(&pending)` called once; `committed = pending`; sub_state = VIEWING |
| `TC-LCDUI-020` | Config — cancel | `cancel_tapped` from CONFIRMING → `pending` discarded; spinboxes = `committed`; sub_state = VIEWING |
| `TC-LCDUI-021` | Config — timeout | Timer fires after 30 s → same as cancel; no `apply_block` call |
| `TC-LCDUI-022` | Config — apply_block fails | `apply_block` returns error → toast shown; `committed` unchanged; sub_state = VIEWING |
| `TC-LCDUI-023` | Config — exit while EDITING | `on_exit` → `pending` discarded; sub_state = VIEWING; dialog hidden |
| `TC-LCDUI-024` | Config — refresh during edit | `on_refresh` while sub_state ≠ VIEWING is a no-op (does not overwrite `pending`) |
| `TC-LCDUI-025` | Navigation — blocked | Tab-change while EDITING → tab reverted; toast shown; current unchanged |
| `TC-LCDUI-026` | Navigation — happy path | Tab-change while VIEWING → `on_exit` then `on_enter` called in order |
| `TC-LCDUI-027` | Navigation — same tab | Tab-change to current tab → no on_exit/on_enter calls |

### 16.2 Integration tests — on target

| Test | Setup |
|---|---|
| Full config edit cycle | Field Technician edits a threshold via LCD; verify `ConfigStore` persists new value; verify `SensorService` uses new threshold on next comparison |
| Cancel discards changes | Edit a threshold, tap Cancel; verify `ConfigStore` retains original value |
| Confirm timeout | Enter CONFIRMING; wait 30 s without tapping; verify sub_state returns to VIEWING, no persistence |
| Alarm screen reaction | Drive sensor reading above threshold; verify Alarm tab shows new alarm within one poll cycle |
| Navigation blocking | Enter EDITING on Config tab; tap another tab; verify navigation is blocked |
| "Waiting for data" | Boot with sensor driver returning invalid; verify Sensor screen shows waiting overlay; simulate first valid reading; verify overlay disappears |

---

## 17. Principles applied

- **P1 (Strict directional layering).** Depends on middleware
  (`GraphicsLibrary` via direct API) and application-layer peer services
  via vtables. No layer skipped.
- **P2 (Dependency Inversion).** All peer-service dependencies injected
  as vtable pointers; `LcdUi` does not include any concrete driver or
  middleware header except `graphics_library.h` (justified by **GL-D8**).
- **P3 (ISP) / CQRS-like split.** `IConfigProvider` (read) and
  `IConfigManager` (write) are separate; `LcdUi` consumes both
  explicitly.
- **P4 (Cross-cutting concern exception).** `ILogger` injected
  concretely per the cross-cutting exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** All
  screen contexts and widget pointers in `.bss`; LVGL widget pool fixed;
  `lv_timer` reused (paused / resumed).
- **P6 (Responsibility traces to requirements).** Every screen and
  navigation path traces to REQ-LD-* (see §3).
- **P7 (Pull-based downstream consumption).** `on_refresh` polls
  `ISensorService`, `IAlarmService`, `IHealthSnapshot`, `IConfigProvider`
  on the 200 ms cadence; no producer pushes updates to `LcdUi`.
- **P8 (Total error propagation, no silent failures).** All public
  functions return `lcd_ui_err_t`; runtime persistence failure logged
  and surfaced via toast + `IHealthReport`.
- **P9 (BARR-C coding standard).** Pixel coordinates `uint16_t`;
  sub-state `enum`; no floating-point in screen logic.
- **P10 (Naming conventions).** Module prefix `lcd_ui_`; error enum
  `LCD_UI_ERR_*`; private types `_t` suffix.

---

## 18. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| **LCD-O1** | Spinbox step sizes per field (large ranges, e.g. pressure 800–1100 hPa) — confirm per-field `lv_spinbox_set_step()` configuration during integration | Decided during widget bring-up | Open |
| **LCD-O2** | Confirm-state timeout duration: **30 s** (provisional, matching LC-O4). Implemented as a pre-allocated paused `lv_timer` reused on each entry to CONFIRMING | Confirmed: pre-allocated timer, paused/resumed | Resolved |
| **LCD-O3** | Refresh skip on identical data — per-widget last-value cache as a CPU optimisation; deferred to integration phase | Defer | Open |
| **LCD-O4** | StatusScreen FD metric subset — documentation interpretation; flag for SRS clarification if REQ-LD-070 is amended to distinguish FD vs GW | SRS review | Open |
| **LCD-O5** | Alarm ordering: most-recently-raised first | Confirmed | Resolved |
| **LCD-O6** | AlarmScreen list scaling: `lv_list` rebuild acceptable for ≤10 alarms; switch to pre-allocated `lv_table` rows if count can exceed 20 | Revisit at integration | Open |
| **LCD-O7** | Soft-restart blocking on active Config edit: `lcd_ui_is_editing()` query function not yet defined. If `LifecycleController` must block restart while Config is in EDITING / CONFIRMING, add this function in a follow-up | Follow-up if requested by LifecycleController | Open |

---

## 19. References

- `docs/hld.md` §5.2 (FD data flow view), §5.5 (health telemetry view).
- `docs/components.md` (FD application layer — `LcdUi`,
  `GraphicsLibrary`, `LifecycleController`).
- `docs/state-machines.md` Machine 5 (FD lifecycle — Operational I3).
- `docs/task-breakdown.md` §4.2 (`LcdUiTask` definition).
- `docs/lld/graphics-library.md` (`graphics_init`,
  `graphics_process`, `graphics_get_display`, `graphics_get_indev`,
  decisions GL-D1..GL-D9, pool sizing, tick model GL-D7, direct-C
  API GL-D8).
- `docs/lld/lcd-driver.md` (framebuffer base, `lcd_blit` contract,
  pixel format ARGB8888).
- `docs/lld/config-service.md` (defines `IConfigProvider` /
  `IConfigManager` and `apply_block` contract).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot` and
  `IHealthReport`).
- `docs/architecture-principles.md` P1–P10.

---

*Companion produced during the LLD Application Phase. v0.2 review pass
applied: section numbering normalised, GraphicsLibrary API references
aligned with direct-C decision (GL-D8), tick model corrected to
GL-D7, error table expanded, init order clarified, principles list
made consistent. Authored by Luca Agrippino; reviewed against the
V-Model gate criteria for LLD.*
