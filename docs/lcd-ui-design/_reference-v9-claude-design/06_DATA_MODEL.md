# 06 · Data Model

The UI never touches hardware directly. It reads four models, which are fed by a **backend** that is swappable: `backend_mock.c` (default, used in the simulator and for UI bring-up) or `backend_hw.c` (real I²C / Modbus, stubbed to start).

```
┌────────────┐    polls / writes    ┌──────────────┐    reads     ┌──────────┐
│  backend   │ ───────────────────▶ │    models    │ ───────────▶ │ screens  │
│ (mock/hw)  │ ◀─────────────────── │ (plain data) │              │ (LVGL)   │
└────────────┘    config applies     └──────────────┘              └──────────┘
```

Rule: **screens read models; backend writes models; screens never call backend data functions directly** (except config apply/save, which is an explicit user action routed through `app.c`).

## Models

### `sensor_model.h`

```c
typedef enum {
    SENSOR_WAITING,   // polling, no reading yet
    SENSOR_VALID,     // fresh reading within tolerance
    SENSOR_STALE,     // last reading older than stale window
    SENSOR_ERROR,     // bus error / bad CRC
    SENSOR_ALARM,     // valid reading but past an alarm threshold
} sensor_state_t;

typedef struct {
    char        id[4];          // "S1"
    char        label[16];      // "TEMPERATURE"
    char        unit[6];        // "°C"
    float       value;          // current reading
    bool        has_value;      // false while WAITING
    sensor_state_t state;
    float       delta_5m;       // change over last 5 min (for footer)
    uint32_t    last_update_ms; // tick of last good reading
    int16_t     spark[11];      // normalized 0..34 ring buffer for the sparkline
    uint8_t     spark_len;      // how many points are valid
} sensor_t;

#define SENSOR_COUNT 3
const sensor_t *sensor_get(int idx);   // idx 0..2
void  sensor_snapshot(sensor_t out[SENSOR_COUNT]); // copy under lock
```

The default trio:

| idx | id | label | unit |
|---|---|---|---|
| 0 | S1 | TEMPERATURE | °C |
| 1 | S2 | HUMIDITY | %RH |
| 2 | S3 | PRESSURE | hPa |

### `alarm_model.h`

```c
typedef enum { SEV_WARN, SEV_ERR } alarm_sev_t;
typedef enum { DIR_HIGH, DIR_LOW } alarm_dir_t;

typedef struct {
    char        sensor_label[20]; // "Temperature · S1"
    alarm_dir_t dir;
    alarm_sev_t sev;
    float       current;          // 42.7
    float       threshold;        // 40.0
    char        unit[6];
    uint32_t    triggered_ms;
    bool        acknowledged;
} alarm_t;

#define ALARM_MAX 8
int  alarm_count(void);
const alarm_t *alarm_get(int idx);
```

When `alarm_count() == 0`, the Alarms screen shows the empty state (screen 09); otherwise the active list (screen 10).

### `system_status.h`

Flat struct of the values shown on the Status screen. The model holds raw values + a per-row status enum; the screen formats them.

```c
typedef enum { ROW_OK, ROW_WARN, ROW_ERR } row_status_t;

typedef struct { row_status_t st; /* value carried as formatted str by screen */ } status_row_meta_t;

typedef struct {
    // COMPUTE
    uint8_t  cpu_load_pct;     row_status_t cpu_st;
    uint32_t free_heap_b;      row_status_t heap_st;
    uint16_t task_watermark_b; row_status_t watermark_st;
    float    mcu_temp_c;       row_status_t mcu_temp_st;
    // CONNECTIVITY
    int8_t   wifi_rssi_dbm;    row_status_t rssi_st;
    uint16_t reconnects_24h;   row_status_t reconnect_st;
    uint16_t mqtt_failures;    row_status_t mqtt_st;
    // MODBUS
    uint32_t mb_success;       row_status_t mb_success_st;
    uint16_t mb_crc_err;       row_status_t mb_crc_st;
    uint16_t mb_timeouts;      row_status_t mb_to_st;
    uint8_t  mb_buf_pct;       row_status_t mb_buf_st;
    // DEVICE
    uint32_t uptime_s;
    char     firmware[12];     // "v2.4.1"
    uint32_t last_save_ago_s;
    // banner (degraded state)
    bool     banner_active;
    char     banner_text[80];
    uint32_t banner_time_ms;
} system_status_t;

const system_status_t *sysstatus_get(void);
```

Row status drives the dot color and the value color (see screen spec 07/08). `banner_active` toggles the degraded banner (screen 08).

### `config_model.h`

```c
typedef enum { CFG_INT, CFG_FLOAT, CFG_ENUM } cfg_type_t;

typedef struct {
    char        key[20];      // persistence key, e.g. "poll_ms"
    char        label[24];    // "Poll interval"
    char        unit[8];      // "ms"
    cfg_type_t  type;
    float       value;        // working (edited) value
    float       saved;        // last persisted value
    float       min, max;     // validation bounds
    const char *hint;         // "100 – 60000"
    const char *const *enum_labels; // for CFG_ENUM (Theme, Bus rate)
    uint8_t     enum_count;
    bool        invalid;      // set by validation
} cfg_field_t;

int  cfg_field_count(void);
cfg_field_t *cfg_field(int idx);
int  cfg_modified_count(void);   // working != saved
int  cfg_invalid_count(void);    // invalid == true
void cfg_validate(void);         // recompute .invalid for all fields
void cfg_revert(void);           // working = saved
bool cfg_apply(void);            // persist working→saved via persistence_*; returns ok
```

Field list (matches screen 11):

| key | label | unit | type | min | max | default | hint |
|---|---|---|---|---|---|---|---|
| `poll_ms`      | Poll interval | ms | INT | 100 | 60000 | 5000 | 100 – 60000 |
| `bus_khz`      | Sensor bus rate | kHz | ENUM | — | — | 400 | 100 / 400 kHz |
| `temp_hi`      | Temp HIGH | °C | FLOAT | −20 | 80 | 40.0 | −20 to 80 |
| `temp_lo`      | Temp LOW | °C | FLOAT | −20 | 80 | −5.0 | −20 to 80 |
| `hum_hi`       | Hum HIGH | %RH | INT | 0 | 100 | 90 | 0 to 100 |
| `backlight`    | Backlight | % | INT | 10 | 100 | 78 | 10 – 100 |
| `theme`        | Theme | — | ENUM | — | — | 0 (Dark · Operator) | — |
| `idle_dim_s`   | Idle dim after | s | INT | 0 | 600 | 60 | — |

`cfg_validate()` sets `invalid = (type != ENUM && (value < min || value > max))`. The Apply button is enabled iff `cfg_invalid_count() == 0 && cfg_modified_count() > 0`.

## Backend interface — `backend.h`

```c
#pragma once
#include <stdbool.h>

// Called once at startup.
void backend_init(void);

// Called periodically by app.c on the LVGL thread (or its own task).
// Pulls fresh data into the models. The mock advances simulated values;
// the hw version reads I²C/Modbus.
void backend_poll(void);

// Force a model state for testing/demo (mock honors this; hw ignores).
typedef enum {
    SCENARIO_NORMAL, SCENARIO_WAITING, SCENARIO_STALE,
    SCENARIO_ERROR, SCENARIO_ALARM, SCENARIO_DEGRADED,
} scenario_t;
void backend_set_scenario(scenario_t s);

// Persist current config. Routed from cfg_apply().
bool backend_save_config(void);
```

`backend_mock.c` and `backend_hw.c` both implement this. A compile flag (or excluding one from the project's source folder) picks the active one. Default the build to mock; flip to hw when the board is ready.

## Mock backend behavior — `backend_mock.c`

The mock makes the UI feel alive and lets you reach every screen state without hardware:

- **NORMAL**: each `backend_poll()` nudges each sensor by a small random walk around a base (T≈23.4, H≈58.2, P≈1013), pushes a new sparkline point, recomputes `delta_5m`, sets state `VALID`. System status holds nominal values; no banner; no alarms.
- **WAITING**: sensors `has_value=false`, state `WAITING`. Auto-transitions to NORMAL after ~3 polls (so boot → waiting → normal feels real).
- **STALE**: sensor S3 stops updating `last_update_ms`; after the stale window it flips to `STALE`.
- **ERROR**: sensor S1 → `ERROR`, keeps last value but faded.
- **ALARM**: S1 climbs past `temp_hi`; pushes an `alarm_t` (HIGH, ERR). S3 dips below `pressure` low; pushes a WARN alarm. System pill → alarm.
- **DEGRADED**: sets `system_status.banner_active=true`, bumps `mqtt_failures`, `reconnects_24h`, `crc_err`, flips the relevant row statuses.

Expose `backend_set_scenario()` via a hidden gesture in the simulator (e.g. number keys 1–6) so reviewers can flip through states. Document the gesture in the sim's README.

## Persistence — `persistence.h`

```c
bool persistence_load(void);                 // populate cfg saved values at boot
bool persistence_save_field(const char *key, float value);
bool persistence_commit(void);               // flush
```

- **`persistence_eeprom.c`** (F469): X-CUBE-EEPROM (AN3969) over the last two flash sectors (22, 23). Each config field gets a stable 16-bit virtual address; `EE_WriteVariable` on save, `EE_ReadVariable` on boot. `persistence_commit()` is a no-op (writes are atomic per variable). See `05_PROJECT_SETUP.md` for the linker reservation.
- **`persistence_file.c`** (sim): a `key=value` text file at `~/.envmon/cfg`. Same interface.

`cfg_apply()` calls `persistence_save_field()` for every modified field, then `persistence_commit()`, then copies working→saved on success.

## Threading

If the backend polls on its own FreeRTOS task, it must take the LVGL lock before mutating any model the UI reads, OR keep models behind a mutex and have screens copy a snapshot under that mutex. Simplest for this size of app: **run `backend_poll()` from an `lv_timer`** on the LVGL thread — then no extra locking is needed. Only move to a separate task if a Modbus transaction would block the UI longer than one frame.
