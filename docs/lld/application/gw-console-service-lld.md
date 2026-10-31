# LLD Companion — GW ConsoleService (TUI)

**Board:** Gateway (B-L475E-IOT01A) only.
**Layer:** Application.

This artefact specifies the software design of the Gateway
`ConsoleService` component — a full-screen ANSI/VT100 TUI ("B1
Cockpit") with block-element area chart, 7-screen navigation,
interactive REPL, provisioning workflows, self-test, and device
profile management. It supersedes the Gateway sections of the shared
FD+GW CLI companion; the FD ConsoleService retains its own separate
companion with the line-oriented CLI design.

**Version:** 1.1
**Date:** October 2026
**Status:** Pass H — Implementation ready · Gateway implemented (host-tested, 188 console tests, bench-validated 2026-10-24, 56 of 56 hardware checks)

**HLD anchor:** ConsoleService in `components.md` (GW application layer)
**Design source:** B1 Cockpit design bundle (`design_handoff_envmon_console/`)

---

## 0. Reconciliation with the repository (v1.1)

v1.0 listed interface members that do not exist on the Gateway. The implementation follows the
repository; where a v1.0 listing in §1, §4–§6, §9, §12–§14, §18, §21 or §22 disagrees with this
table, this table wins.

| v1.0 said | Repository / implementation | Decision |
|---|---|---|
| `IDebugUart` receives lines; the ESC parser is fed from an RX ISR | `DebugUartDriver` is line-oriented (`read_line`, one callback per completed line); arrows and Esc cannot be received | **GCS-D1:** `debug_uart_attach_rx_byte()` added to the driver (additive, vtable member appended, line mode untouched, mutually exclusive with it). 7 driver tests. |
| `console_service_init(uart, sensors, alarms, cfg_read, cfg_write, health, prof_read, prof_write, log)` | A `console_deps_t` struct is simpler to inject and test | **GCS-D2:** `console_service_init(const console_deps_t *)`. No logger parameter (`LOG_*` macros) and no `prof_write` (see below). Health is a `health_monitor_handle_t`. |
| `ISensorService.force_read / get_latest` | `sensor_service->get_snapshot()` (int32 fixed point), `is_ready()` | Snapshot pulled every cycle; the chart uses the snapshot's temperature, humidity and pressure. |
| `IAlarmService.get_active()` | `alarm_service->get_all_states()`; limits come from ConfigService | Active alarms are derived from state plus the live limits; **alarm duration is not available** and is not shown. |
| `IHealthSnapshot.get()` | `health_monitor_get_snapshot(handle, &snap)` | Fields used: uptime, RSSI, cloud / Modbus link flags, publish and Modbus counters, buffer entries, time sync. |
| `IConfigManager.apply_prov_block / apply_cfg_block / round_trip_test / store_selftest_result` | `iconfig_manager_t` has `set_param`, `validate_param`, `set_param_deferred`, `persist_pending` | **GCS-D3:** `commit` applies each staged value with `set_param_deferred`; the console flushes with `persist_pending()` after the frame is drawn, so a flash write (about 0.8 s, CFG-O9) never delays the screen. `set` does the same. |
| WiFi SSID / password, Modbus baud / parity, certificates, `profiles add` / `remove`, self-test persistence and a flash round-trip test | None of these back-ends exists (no credential store, Modbus line settings are fixed, `DeviceProfileRegistry` is read-only, ConfigService has no self-test record) | **GCS-D4 (user decision):** the commands validate their input with the §12.1 rules and then print `[ERR] not available: <what is missing>`; nothing is staged. TC-GCS-130..135, 141..144, 182..185 test "validated, then not available". `selftest` checks sensors and links; flash is reported `n/a`; the result is kept in RAM only. Open items GCS-O4..O6. |
| `config set polling-interval-ms 1000` with float / deci-°C staging struct | ConfigService uses the FD fixed-point schema (CFG-D10) | Staging keeps a fixed-point value per key and asks `validate_param` before staging. Keys: polling-interval-ms, filter-alpha, modbus-poll-period-ms, telemetry-interval-ms, health-interval-ms, mqtt-port, and the six sensor range limits. Thresholds are set with `set`. |
| `prov` keys apply the whole block | Only `mqtt-endpoint` (→ ConfigService broker) and `modbus-addr` have a back-end | Those two are staged and applied; endpoint is a hostname or IPv4 of 1..127 characters (an input line of 120 characters limits what can be typed to about 97). |
| Cell is 16 bytes with RGB (62 KB for two grids; 75 KB static in total) | The Gateway has a 96 KB main RAM bank; the image already holds 6 to 42 KB depending on the harness; 75 KB does not link | **GCS-D5 (closes GW-CS-O1):** palette-indexed cells, 3 bytes: glyph index, foreground index, background index + bold bit. Two grids are 11.6 KB. The proximity gradient is 25 precomputed steps (BASE → WARN in 12, WARN → OVER in 12). Measured: **13.0 KB bss + 13.0 KB scrollback, 37 KB flash** (§18 superseded). |
| Chart, proximity and resampling in `float` | The Gateway is fixed point (SS-D2); values are 0.01 °C / 0.01 %RH / 0.1 hPa | **GCS-D6:** all chart arithmetic is integer; proximity is Q8 (256 = at the limit). Bands: 2.00 °C, 12.00 %RH, 12.0 hPa. |
| One threshold per channel; y-range includes it | The Gateway has a high and a low limit per channel | **GCS-D7:** proximity uses both limits (nearest wins); the dashed guide, the "HI / LO" label and the y-range follow only the limit the latest value is closer to, so a far limit does not flatten the data. |
| Time axis -45m / -30m / -15m / now | History is one sample per second, 92 samples | **GCS-D8:** axis reads -90s / -60s / -30s / now. |
| Y-axis label field 2 characters | Pressure labels have four digits | **GCS-D9:** 4-character label field; chart starts at interior column 7. |
| REPL line `text[56]`, one colour per line | UTF-8 glyphs (●, °, →) are 2 to 3 bytes | **GCS-D10:** `text[64]` bytes (about 55 columns), cut on a character boundary; one colour per line as specified. |
| `IConsoleService` task created by LifecycleController | No LifecycleController or Gateway `main()` yet | `console_task_body()` is provided; the bring-up harness creates the task. |
| Confirm prompt timeout via the data-tick counter (CS-O7) | | Implemented: 30 ticks of the 1 Hz timer; CS-O7 closed. |
| Terminal size by `ESC[18t` handshake | Not every terminal answers (minicom) | The query is sent at start and every 5th tick. **GCS-D11 (new):** a terminal that joins late sees only changes, so **Ctrl-L repaints the whole screen**, and the first size answer (or a changed size) repaints once. |
| (not specified) the console and TLS share one 96 KB RAM bank | The first bench image with console, sensors, config **and** WiFi/MQTT had 33 KB of C heap left (mbedTLS allocates from it); `mbedtls_ssl_setup` failed, so MQTT never connected. The Gateway has a 32 KB SRAM2 bank nothing used. | **GCS-D12:** the two cell grids and the scrollback (24.6 KB) live in SRAM2 via `CONSOLE_BIG_BUFFER` (`port/console_mem.h`, section `.bss.sram2`). New zero-initialised, not-loaded `.sram2_bss` output section in both linker scripts, cleared by the startup code. Main-bank `.bss` fell from 62.6 KB to 36.4 KB and MQTT connects. This also closes the RAM-budget question for the console in MQTT-O1. |
| (not specified) PgUp / PgDn | | **GCS-D13 (closes GCS-O7):** `ESC [ 5 ~` / `ESC [ 6 ~` scroll the console history 4 lines at a time; a `↓ N more` marker shows when scrolled back; running a command returns to the live view. Beyond the reference design, added to fix the usability gap. |
| (not specified) a key press should show feedback at once | A frame is sent byte by byte at 115200 baud (about 87 µs per byte); every panel change sent the whole panel before the next key could be read, so the menu highlight lagged by up to 370 ms on the bench and a held arrow key queued behind it | **GCS-D14:** each frame goes out in two parts, the menu columns first, then the panel (`termout_flush_cols`, `grid_copy_cols`). If more keys are already waiting when the menu part is out, the panel is skipped and the next cycle (no wait) handles the new keys, so only the final panel of a run of key presses is ever drawn. Measured on the bench (`scripts/console_latency.py`): highlight latency 51–373 ms → 26–98 ms; worst held-key case 373 → 73 ms; slowest full redraw 468 → 191 ms. |
| (not specified) serial speed 115200 | Frame time dominates the feel of the TUI; 115200 baud moves 11.5 KB/s | **GCS-D15:** the console switches the VCP to 460800 baud (`CONSOLE_LINK_BAUD`, `core/theme.h`) through a new `debug_uart_set_baud()` (also `idebug_uart_t.set_baud`). The driver default stays 115200, so every other harness and its terminal setting is unchanged; only the console harness needs `picocom -b 460800`. 921600 was tried and rejected: at 12 µs per byte a late interrupt (WiFi/TLS activity) overran the single-byte receive register and typed commands arrived garbled; 460800 passed the 50-check bench three runs in a row. The RX ISR now wakes the task only for the first byte of a burst, which lets a 60-byte back-to-back paste through. Menu highlight latency 51-373 ms at 115200 became 22-47 ms. |
| (not specified) every menu item has a panel to enter | Thresholds, Network and Firmware show data only; the "►" marker and Enter/Tab suggested there was something inside | **GCS-D16:** only the Graph (channel select) and the Console (command line) take focus (`ui_screen_is_interactive`). Enter/Tab on the other items does nothing, the "►" marker appears only on the two interactive items, and the footer hint omits "Enter / Tab → panel" for the rest. Thresholds are changed with `set`, `prov` and the reboot with `reboot` in the Console. |
| (not specified) reboot and update check only through the console | The Firmware item showed "type `reboot` in the console" | **GCS-D17 (supersedes the Firmware part of GCS-D16):** the Firmware panel takes focus and offers two actions, **Check for update** and **Reboot gateway** (↑↓ select, Enter run). Reboot asks the usual y/N question (same 30-tick timeout, `CONFIRM_REBOOT`) so a stray Enter cannot restart the board; `y` tears the screen down and resets exactly like the `reboot` command. Check for update has no back-end (no FirmwareStore, no update source), so it reports `not available: no update source yet` in the panel and the console history. Menu marker on Firmware restored. |
| (not specified) lone Esc followed at once by a key | | The parser returns Escape and hands the follow-up byte back (`input_take_pending`). |
| (not specified) startup time | | The task draws the first frame immediately; the screen-setup sequence goes out then, not at init (§17). |

Not in v1.0 but added: a host simulator (`tools/console_simulator/`, builds the real service and core against fake services) and a scripted hardware driver (`scripts/console_bench.py`, renders the serial stream with `pyte`).

---

## 1. Sources

| Field | Value |
|---|---|
| **Board** | Gateway (B-L475E-IOT01A) |
| **Provides** | `IConsoleService` (init, run loop entry) |
| **Uses** | `IDebugUart`, `ISensorService`, `IAlarmService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `IDeviceProfileProvider`, `IDeviceProfileManager`, `ILogger` |
| **Hosted in task** | `ConsoleTask` prio 1, 768 words / 3 KB |
| **Activation** | Event — UART RX byte notify + 1 Hz data tick |
| **Transport** | ST-Link VCP (USART1), 115200 8N1, no flow control |

**Note — `components.md` gaps (CS-O4, CS-O9).** The GW
`ConsoleService` entry in `components.md` omits `IConfigManager` and
`IAlarmService` from its USES list, and lists `DeviceProfileRegistry`
as a concrete dependency rather than its ISP-split interfaces. A
follow-up `components.md` correction must:

1. Add `IConfigManager` — provisioning/config writes (CS-O4).
2. Add `IAlarmService` — alarm panel display.
3. Replace `DeviceProfileRegistry` with `IDeviceProfileProvider` +
   `IDeviceProfileManager` — ISP compliance (P3).

**Stack size rationale.** The FD CLI companion specifies 512 words /
2 KB. The TUI adds an 88×22 cell grid (~3.9 KB as struct, but held
file-scope, not on the stack), chart computation buffers (~400 B on
stack during `chart_render()`), and REPL scrollback (~200 lines ×
pointer = 800 B file-scope). Stack analysis: deepest path is
`run_once → render_compose → chart_render` at ~600 B. 768 words /
3 KB provides 50% headroom over the measured 600 B worst case.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Serial console parameters | REQ-LI-000 | UC-04, UC-16 |
| Diagnostic command execution | REQ-LI-010, LI-020, LI-0E1 | UC-04 |
| Provisioning menu | REQ-LI-030 | UC-16 |
| WiFi credentials | REQ-LI-040 | UC-16 |
| Cloud endpoint | REQ-LI-050 | UC-16 |
| Cloud certificates | REQ-LI-060 | UC-16 |
| Modbus address | REQ-LI-070 | UC-16 |
| Serial port parameters | REQ-LI-080 | UC-16 |
| Input validation | REQ-LI-090, LI-0E2 | UC-16 |
| Confirmation flow | REQ-LI-100, LI-110, LI-120, LI-0E3 | UC-16 |
| Self-test command | REQ-LI-130, LI-140, LI-160 | UC-04 |
| Device serial number | REQ-LI-150 | UC-04 |
| Config persistence | REQ-DM-090 | UC-16 |
| Device profile management | REQ-MB-110, REQ-MB-111 | UC-16 |

---

## 3. Responsibility

`ConsoleService` owns the operator-facing serial TUI on the Gateway.
It renders a full-screen 88-column ANSI/VT100 interface through
`IDebugUart`, processes navigation and REPL input from the serial
byte stream, and dispatches commands to injected service interfaces.
It does not own any peripheral state — all sensor data, alarm state,
config reads/writes, health snapshots, and device profiles are
obtained through injected interfaces (P7 — pull-based access).

The component runs in a dedicated FreeRTOS task (`ConsoleTask`) at
priority 1, activated by:

1. **UART RX byte notify** — direct-to-task from the `IDebugUart`
   RX ISR (keypress events).
2. **1 Hz data tick** — a FreeRTOS software timer sets a notification
   bit, triggering a re-render with fresh sensor data.
3. **2 Hz cursor blink** — a FreeRTOS software timer toggles the
   cursor phase; only the REPL input cell changes.

Between notifications the task is blocked, consuming no CPU.

---

## 4. Provided interface

### 4.1 Singleton accessor

```c
extern const iconsole_service_t * const console_service;
```

### 4.2 Vtable

```c
typedef struct iconsole_service {
    console_service_err_t (*run_once)(void);
} iconsole_service_t;
```

`run_once()` performs one drain-input / update-state / render cycle.
`ConsoleTask` calls it in an infinite loop after waiting on the
notification bits.

### 4.3 Initialisation function

Not part of the vtable. Called once by `LifecycleController` during
boot, before the task that hosts the service is created.

```c
console_service_err_t
console_service_init(const idebug_uart_t              *uart,
                     const isensor_service_t          *sensors,
                     const ialarm_service_t           *alarms,
                     const iconfig_provider_t         *cfg_read,
                     const iconfig_manager_t          *cfg_write,
                     const ihealth_snapshot_t         *health,
                     const idevice_profile_provider_t *prof_read,
                     const idevice_profile_mgr_t      *prof_write,
                     const ilogger_t                  *log);
```

No `self` parameter — follows the project's singleton vtable pattern.

### 4.4 Task entry

```c
void console_task_body(void *arg);
```

`xTaskCreateStatic` references this. Internally waits on combined
notification bits (RX, data tick, cursor blink) and calls
`console_service->run_once()` per iteration.

---

## 5. Data types

### 5.1 Public error enum

```c
typedef enum {
    CONSOLE_SERVICE_ERR_OK              = 0,
    CONSOLE_SERVICE_ERR_NULL_ARG        = 1,
    CONSOLE_SERVICE_ERR_NOT_INIT        = 2,
    CONSOLE_SERVICE_ERR_VALIDATION      = 3,
    CONSOLE_SERVICE_ERR_UNKNOWN_KEY     = 4,
    CONSOLE_SERVICE_ERR_APPLY_FAILED    = 5,
    CONSOLE_SERVICE_ERR_TIMEOUT         = 6,
    CONSOLE_SERVICE_ERR_LINE_OVERFLOW   = 7,
} console_service_err_t;
```

### 5.2 TUI state

```c
typedef enum {
    SCR_GRAPH,
    SCR_STATUS,
    SCR_ALARMS,
    SCR_CONFIG,
    SCR_NETWORK,
    SCR_CONSOLE,
    SCR_FIRMWARE,
} screen_t;

typedef enum {
    FOCUS_MENU,
    FOCUS_PANEL,
} focus_t;

typedef struct {
    uint8_t   sel;           /* selected menu index 0..6 → screen_t */
    focus_t   focus;         /* MENU or PANEL                       */
    uint8_t   channel;       /* 0 temp / 1 humid / 2 press (graph)  */
    char      input[120];    /* REPL input buffer                   */
    uint8_t   input_len;
    bool      cursor_on;     /* blink phase                         */
} ui_state_t;
```

### 5.3 Cell grid

```c
typedef struct {
    char     ch[4];     /* UTF-8 glyph (up to 4 bytes)  */
    uint8_t  ch_len;    /* byte length of glyph          */
    uint8_t  fg_r;      /* foreground RGB                */
    uint8_t  fg_g;
    uint8_t  fg_b;
    uint8_t  bg_r;      /* background RGB (0,0,0 = default) */
    uint8_t  bg_g;
    uint8_t  bg_b;
    bool     bold;       /* SGR bold attribute            */
} cell_t;

#define GRID_COLS  88
#define GRID_ROWS  22   /* banner(8) + gap(1) + menu/panel(11) + footer(1) + margin(1) */

typedef struct {
    cell_t cells[GRID_ROWS][GRID_COLS];
} cell_grid_t;
```

### 5.4 Command table entry

```c
typedef console_service_err_t (*cmd_handler_t)(int argc, const char *argv[]);

typedef struct {
    const char    *token;
    const char    *alias;        /* NULL if no alias */
    uint8_t        min_argc;
    uint8_t        max_argc;
    cmd_handler_t  handler;
    const char    *help;
} cmd_entry_t;
```

The command table is a `static const cmd_entry_t s_cmd_table[]` —
`.rodata`, no RAM cost. Alias support for shorthand (`?` → `help`,
`ls` → `sensors`, `cls` → `clear`).

### 5.5 Pending-change staging structs

```c
typedef struct {
    bool     dirty;
    char     wifi_ssid[64];
    char     wifi_pass[64];
    char     mqtt_endpoint[128];
    uint8_t  modbus_slave_addr;
    uint32_t modbus_baud;
    char     modbus_parity;        /* 'N', 'E', 'O' */
} prov_pending_t;

typedef struct {
    bool     dirty;
    uint32_t polling_interval_ms;
    int16_t  temp_alarm_hi_deci_c;
    /* ... all operational fields */
} cfg_pending_t;
```

### 5.6 Self-test result

```c
typedef struct {
    uint64_t timestamp_unix;
    bool     sensor_pass;
    bool     comms_pass;
    bool     flash_pass;
    bool     overall_pass;
} selftest_result_t;
```

Persisted via `IConfigManager.store_selftest_result()`.

### 5.7 REPL scrollback

```c
typedef struct {
    char     text[56];       /* visible text, truncated to panel inner width */
    uint8_t  fg_r, fg_g, fg_b;   /* line colour (simplified: one colour per line) */
} scrollback_line_t;

#define SCROLLBACK_MAX  200

static scrollback_line_t s_scrollback[SCROLLBACK_MAX];
static uint16_t          s_scrollback_head;
static uint16_t          s_scrollback_count;
```

Ring buffer of output lines. The REPL panel renders the tail 8 lines
plus the live input line.

---

## 6. Internal state

All state is file-scope static — no heap, no per-instance context.

```c
/* Injected interface pointers */
static const idebug_uart_t              *s_uart;
static const isensor_service_t          *s_sensors;
static const ialarm_service_t           *s_alarms;
static const iconfig_provider_t         *s_cfg_read;
static const iconfig_manager_t          *s_cfg_write;
static const ihealth_snapshot_t         *s_health;
static const idevice_profile_provider_t *s_prof_read;
static const idevice_profile_mgr_t      *s_prof_write;
static const ilogger_t                  *s_log;

/* TUI state */
static ui_state_t     s_ui;
static cell_grid_t    s_grid_cur;     /* current frame  */
static cell_grid_t    s_grid_prev;    /* previous frame (for diff) */

/* UART input */
static uint8_t  s_ring_buf[256];      /* power of 2 for masked-index */
static uint16_t s_ring_head;
static uint16_t s_ring_tail;
static bool     s_ring_overflow_flag;

/* ESC sequence parser */
static uint8_t  s_esc_buf[8];
static uint8_t  s_esc_len;
static bool     s_in_esc;

/* Chart data history */
static int16_t  s_chart_history[3][92]; /* 3 channels × 92 samples */
static uint8_t  s_chart_write_idx;

/* Provisioning and config staging */
static prov_pending_t s_prov_pending;
static cfg_pending_t  s_cfg_pending;

/* Scrollback (see §5.7) */
/* s_scrollback[], s_scrollback_head, s_scrollback_count */

static bool s_initialised;
```

---

## 7. TUI layout specification

### 7.1 Grid geometry

Fixed 88-column, minimum 30-row terminal. All rendering targets an
in-memory cell grid; never reflow to terminal width. If the terminal
is smaller than 88×30, render a centred "Resize to at least 88×30"
notice and suppress the TUI.

```
┌─ 88 cols ──────────────────────────────────────────────────────────┐
│  BANNER         full width 88, height 8                            │
│  (blank row)                                                       │
│  ┌ MENU 30 ┐ ┌ PANEL 57 ┐   30 + 1 gap + 57 = 88                   │
│  │  11 rows │ │ 11 rows  │                                          │
│  └──────────┘ └──────────┘                                         │
│  (footer status/hint line)                                         │
└────────────────────────────────────────────────────────────────────┘
```

### 7.2 Colour palette

Truecolor SGR: `ESC[38;2;R;G;Bm` (fg), `ESC[48;2;R;G;Bm` (bg).

| Token | Hex | RGB | Use |
|---|---|---|---|
| `BG` | `#0B0F16` | 11,15,22 | terminal background |
| `BORDER` | `#33485B` | 51,72,91 | box lines `│ ─ ┌ …` |
| `TITLE` | `#9FD0EE` | 159,208,238 | box titles |
| `TITLE_ACT` | `#CFEBFF` | 207,235,255 | focused panel title (brighter + bold) |
| `LOGO` | `#7BAFD4` | 123,175,212 | ANSI-Shadow banner |
| `MUTED` | `#7E8B99` | 126,139,153 | secondary text, labels |
| `ACCENT` | `#7BAFD4` | 123,175,212 | accent, prompts, `►` |
| `OK` | `#6FBF8E` | 111,191,142 | ok status |
| `WARN` | `#E0B252` | 224,178,82 | near / stale |
| `ERR` | `#CC6666` | 204,102,102 | alarm |
| `NUM` | `#FFD479` | 255,212,121 | numeric values |
| `INK` | `#C7D3DF` | 199,211,223 | primary text |
| `AXIS` | `#5E6B7A` | 94,107,122 | chart axes & ticks |
| `THR` | `#7A6A3A` | 122,106,58 | dashed threshold guide `╌` |
| `SEL_BG` | `#7BAFD4` | 123,175,212 | selected menu item bg (fg `#08121A`) |
| `PROMPT` | `#6FBF8E` | 111,191,142 | `envmon>` prompt |

Compile flag `CONSOLE_256COLOR` maps each token and each proximity
colour to the nearest xterm-256 index (`16 + 36r + 6g + b`).

### 7.3 Banner

88 × 8 box. Left: ANSI-Shadow "ENVMON" logo (6 rows, 57 cols) in
`LOGO`. Right: 6 detail lines adapted for the Gateway:

```
ENVIRONMENTAL GATEWAY
─────────────────────
fw 2.5.0 · STM32L475
wifi ●  mqtt ●  modbus ●
uptime 3d 14:22 · 0 alarms
● 5 / 7 sensors online
```

- Labels in `MUTED`, values in `ACCENT`/`NUM`.
- Connectivity dots: `●` in `OK` when connected, `ERR` when not.
- Alarm count flips to `ERR` when > 0.
- `uptime` and counters are live; refreshed on each 1 Hz tick.

### 7.4 Menu rail

30-col box titled `MAIN MENU`, 11 lines: top border, blank, 7 items,
blank, bottom border.

| sel | Label | Screen |
|---|---|---|
| 0 | Live sensor readings | `SCR_GRAPH` |
| 1 | System status | `SCR_STATUS` |
| 2 | Active alarms | `SCR_ALARMS` |
| 3 | Thresholds | `SCR_CONFIG` |
| 4 | Network | `SCR_NETWORK` |
| 5 | Console | `SCR_CONSOLE` |
| 6 | Firmware · Reboot | `SCR_FIRMWARE` |

Selected row: `▸` cursor + `SEL_BG` highlight; number in `NUM`.
Non-selected: number in `NUM`, label in `INK`; items 3,4,6 show
right-aligned `►` in `ACCENT`.

### 7.5 Panel screens

Each panel occupies 57 cols × 11 lines (inner 55 × 9). All data is
pulled from injected interfaces at render time (P7).

**SCR_GRAPH — Live sensor chart.** Block-element area chart with
proximity colouring (see §8). Title shows channel name and unit.
Channel cycles with ↑/↓ when the panel has focus.

**SCR_STATUS — System status.** Read-only text panel. Fields pulled
from `IHealthSnapshot`:

```
WiFi         ● connected  RSSI -42 dBm
MQTT         ● connected  msgs: 1247 tx / 3 rx
Modbus       ● 2/3 slaves online
Sensors      5/7 valid
Uptime       3d 14:22:05
Last boot    Normal reset
MCU temp     38 °C
Free heap    14.2 KB
```

**SCR_ALARMS — Active alarms.** Pulled from `IAlarmService`. Each
alarm shows sensor name, threshold crossed, current value, and
duration. If no alarms active: "No active alarms" in `OK`.

**SCR_CONFIG — Thresholds.** Pulled from `IConfigProvider`. Shows
current operational parameters (polling interval, alarm thresholds
per channel with HI/LO values). Read-only display; editing is via
the Console REPL (`config set`, `set`).

**SCR_NETWORK — Network.** Pulled from `IHealthSnapshot`. Shows:

```
WiFi SSID    HomeNetwork
WiFi IP      192.168.1.42
WiFi RSSI    -42 dBm
MQTT broker  iot.example.com:8883
MQTT state   connected
Modbus       9600 8E1 @ addr 1
Slaves       2/3 online
S&F buffer   0 / 512 entries
```

**SCR_CONSOLE — Interactive REPL.** See §9.

**SCR_FIRMWARE — Firmware · Reboot.** Shows firmware version, build
date, MCU part number, active/inactive flash slots, last self-test
result. Read-only.

### 7.6 Footer status line

Single line below the boxes:

```
↑↓ select · Enter / Tab → panel · 1–7 jump          focus MENU · 115200 8N1
```

Hint text varies by focus + screen:

| State | Hint |
|---|---|
| `FOCUS_MENU` | `↑↓ select · Enter / Tab → panel · 1–7 jump` |
| `FOCUS_PANEL` + Console | `type a command · Enter run · Esc → menu` |
| `FOCUS_PANEL` + Graph | `↑↓ change channel · Esc → menu` |
| `FOCUS_PANEL` + other | `Esc → menu` |

---

## 8. Block-area chart specification

The chart is the signature feature — a 46-column × 6-row block-element
area chart with proximity-based fuzzy colouring.

### 8.1 Block characters

```c
static const char *BLK[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
/* 9 levels: index 0 = space, index 8 = full block */
```

### 8.2 Rendering algorithm

```
1. Resample the last ~92 samples to 46 columns by linear
   interpolation (s_chart_history[channel]).

2. Compute y-range:
     lo = min(data); hi = max(data)
     if channel has hi-threshold: hi = max(hi, threshold)
     if channel has lo-threshold: lo = min(lo, threshold)
     pad = (hi - lo) * 0.12  (min 1.0)
     lo -= pad; hi += pad
     rng = hi - lo

3. Per column c:
     level[c] = round((data[c] - lo) / rng * (6 * 8))   /* 0..48 eighths */

4. For each row r (0 = top):
     fromBottom = 5 - r
     base = fromBottom * 8
     for each column c:
       idx = clamp(level[c] - base, 0, 8)
       if idx == 0:
         if this is the threshold row → emit '╌' in THR
         else → emit ' '
       else:
         emit BLK[idx] coloured by proximity(data[c])
```

### 8.3 Proximity colour algorithm

```c
/* Per-channel sensitivity bands */
static const float s_band[] = {2.0f, 12.0f, 12.0f};  /* temp, humid, press */

float proximity(float value, uint8_t channel) {
    float t = 0.0f;
    if (has_hi_threshold(channel)) {
        float hi = get_hi_threshold(channel);
        t = fmaxf(t, (value - (hi - s_band[channel])) / s_band[channel]);
    }
    if (has_lo_threshold(channel)) {
        float lo = get_lo_threshold(channel);
        t = fmaxf(t, ((lo + s_band[channel]) - value) / s_band[channel]);
    }
    return t;
}
```

Colour interpolation from proximity `t`:

```
t ≤ 0:   rgb = BASE (123,175,212 cyan);    glow = 0
t ≤ 1:   rgb = lerp(BASE, WARN, t);        glow = t * 0.45
t > 1:   k = clamp((t-1)/0.6, 0, 1)
          rgb = lerp(WARN, OVER, k);         glow = 0.45 + k * 0.55
```

Where `BASE = (123,175,212)`, `WARN = (224,178,82)`, `OVER = (204,102,102)`.

**Terminal glow approximation.** Terminals cannot blur text shadows.
Glow is approximated with SGR bold (`ESC[1m`) when `glow > 0.5`, and
by nudging each RGB component brighter by `clamp(glow * 30, 0, 30)`.

Colour is computed **per column** from that column's value — the
chart literally changes colour temperature along its length.

### 8.4 Status chips

```
stale      → "▲ stale"  WARN
t >= 1.0   → "■ alarm"  ERR
t >= 0.6   → "▲ near"   WARN
else       → "● ok"     OK
```

### 8.5 Chart data pipeline

`s_chart_history[channel]` is a circular buffer of 92 samples per
channel. On each 1 Hz data tick, `run_once()` pulls the latest
reading from `ISensorService` and appends to the history. The chart
resamples 92 → 46 columns, giving ~46 seconds of visible history at
1 Hz, or ~92 seconds at 0.5 Hz. Thresholds are pulled live from
`IConfigProvider` at render time — a `set` or `config set` command
that changes a threshold causes the chart to re-colour on the next
frame with no explicit invalidation.

### 8.6 Performance

276 cells per chart frame. Adjacent cells sharing the same colour
are coalesced into a single SGR run. Combined with line diffing
(§10), a 1 Hz update costs well under 1 KB on the wire at 115200
(~11 KB/s usable).

---

## 9. Console REPL specification

### 9.1 Panel layout

57-col panel (inner 55), 11 lines: title `CONSOLE` + 9 content rows
+ bottom border. The 9 content rows show the tail 8 scrollback lines
plus the live input line:

```
┌─[ CONSOLE ]───────────────────────────────────────────┐
│ envmon> sensors                                        │
│   SENSOR     VALUE      STATUS     UPDATED              │
│   temp     24.3 °C    ● ok       12:34:56              │
│   humid    58.2 %RH   ● ok       12:34:56              │
│   press    1013 hPa   ▲ stale    12:34:56              │
│ envmon> set t.hi 24                                     │
│   temp.hi → 24.0 °C   applied                           │
│ envmon> █                                              │
│                                                        │
└────────────────────────────────────────────────────────┘
```

Prompt: `envmon` in `PROMPT` + `> ` in `MUTED`, typed text in `INK`,
block cursor `█` in `ACCENT` blinking at ~2 Hz (only when panel
has focus).

### 9.2 Parser

```
trim input; if empty → echo bare prompt line.
echo the entered command as "envmon> <cmd>" line.
split on whitespace → name, args[].
look up name (case-insensitive) in s_cmd_table (incl. aliases).
  not found → "  unknown command: <name> — try \"help\""  in WARN.
  found → call handler(argc, argv); append returned lines to scrollback.
clamp scrollback to 200 lines.
```

### 9.3 Command table

**Shared diagnostic commands** — same logic as FD CLI companion §8.1,
but output is rendered as scrollback lines in the REPL panel:

| Token | Alias | Args | Handler behaviour | Traces to |
|---|---|---|---|---|
| `help` | `?` | 0 | Print all command tokens and one-line descriptions | — |
| `version` | `about` | 0 | Print firmware version, build, part number | — |
| `serial` | — | 0 | Print MCU UID (12 bytes, hex) | REQ-LI-150 |
| `sensors` | `sensor`, `ls` | 0 | Table: name · value · status · updated (all channels) | REQ-LI-010 |
| `read` | — | 1 | Single channel reading + status (`read t`, `read h`, `read p`) | REQ-LI-010 |
| `status` | — | 0 | Health snapshot via `IHealthSnapshot` | REQ-LI-010 |
| `alarms` | — | 0 | Active alarms via `IAlarmService`, or "No active alarms" | REQ-LI-010 |
| `thresholds` | — | 0 | HI/LO per channel from `IConfigProvider` | REQ-LI-010 |
| `set` | — | 2 | Mutate a threshold: `set t.hi 24` → updates via `IConfigManager` | REQ-LI-010 |
| `uptime` | — | 0 | Device uptime | — |
| `selftest` | — | 0 | Run board self-test; print per-subsystem pass/fail; persist | REQ-LI-130..160 |
| `selftest-result` | — | 0 | Print last stored self-test result | REQ-LI-160 |
| `config list` | — | 0 | All current operational parameters from `IConfigProvider` | REQ-LI-010 |
| `config set` | — | 2 | Stage an operational parameter change | REQ-LI-010 |
| `config commit` | — | 0 | Confirm and apply staged operational changes | REQ-LI-100..120 |
| `config discard` | — | 0 | Discard staged operational changes | REQ-LI-0E3 |
| `prov set` | — | 2 | Stage a provisioning parameter | REQ-LI-030..080 |
| `prov commit` | — | 0 | Confirm and apply staged provisioning changes | REQ-LI-100..120 |
| `prov discard` | — | 0 | Discard staged provisioning changes | REQ-LI-0E3 |
| `clear` | `cls` | 0 | Empty the scrollback | — |
| `reboot` | — | 0 | Schedule NVIC_SystemReset after teardown | — |

**GW-specific commands:**

| Token | Args | Handler behaviour | Traces to |
|---|---|---|---|
| `profiles list` | 0 | Print all device profiles via `IDeviceProfileProvider` | REQ-MB-110 |
| `profiles add` | 1+ | Add/update a profile: `profiles add addr=5 sensors=temp,hum` | REQ-MB-111 |
| `profiles remove` | 1 | Remove a profile by slave address | REQ-MB-111 |
| `wifi status` | 0 | WiFi link state + RSSI from `IHealthSnapshot` | REQ-LI-010 |
| `mqtt status` | 0 | MQTT connection state + counters from `IHealthSnapshot` | REQ-LI-010 |
| `modbus status` | 0 | Modbus master polling status from `IHealthSnapshot` | REQ-LI-010 |
| `net` | `network` | 0 | Combined network info (WiFi + MQTT + Modbus) | — |

### 9.4 `set` command — threshold mutation

```
set <ch>.<hi|lo> <value>

Channel keys: t|temp, h|humid, p|press
Validation: argument must match ^(t|h|p|temp|humid|press)\.(hi|lo)$
            and a parseable float/int.

On success: calls IConfigManager to update the threshold.
            Prints "  <ch>.<side> → <value> <unit>   applied" in OK.
            The chart re-colours on the next render because proximity
            is derived from the live threshold (P7).

On failure: prints usage hint in WARN.
```

This is the coupling between REPL and chart — the `set` command
mutates a threshold through `IConfigManager`, and the chart pulls the
new threshold on the next `render_compose()` via `IConfigProvider`.
No explicit notification or invalidation is needed.

---

## 10. Rendering engine

### 10.1 Render loop

```c
void run_once(void) {
    uint32_t notify_bits = wait_for_notification();

    /* Drain UART input → key events → update ui_state */
    drain_ring_buf();
    while (next_key(&key)) {
        handle_key(&s_ui, key);
    }

    /* On data tick: update chart history */
    if (notify_bits & NOTIFY_DATA_TICK) {
        update_chart_history();
    }

    /* Compose frame into s_grid_cur */
    render_compose(&s_grid_cur, &s_ui);

    /* Diff against s_grid_prev, emit minimal ANSI to s_uart */
    termout_flush(&s_grid_cur, &s_grid_prev, s_uart);

    /* Swap: cur becomes prev for next frame */
    memcpy(&s_grid_prev, &s_grid_cur, sizeof(cell_grid_t));
}
```

### 10.2 `render_compose`

Fills the cell grid top-to-bottom:

1. **Banner** (rows 0–7): logo + detail lines.
2. **Gap** (row 8): blank.
3. **Menu + Panel** (rows 9–19): menu box (cols 0–29) and panel box
   (cols 31–87) rendered side by side.
4. **Footer** (row 20): hint + focus indicator.

Each region writes glyphs and colours into the grid cells. The chart
panel calls `chart_render()` which fills the panel's inner cells
using the block-area algorithm (§8).

### 10.3 Dirty-line diffing

`termout_flush` compares `s_grid_cur` against `s_grid_prev` row by
row. For each row with at least one changed cell:

1. Emit cursor-move: `ESC[<row+1>;<first_changed_col+1>H`.
2. Walk changed cells, coalescing runs of equal colour into a single
   SGR sequence + UTF-8 glyph string.
3. Emit `ESC[0m` at the end of the run.

Unchanged rows are skipped entirely. This keeps a 1 Hz update to a
few hundred bytes — essential at 115200 baud.

### 10.4 Init/teardown sequences

```
On connect:  ESC[?1049h    (alternate screen buffer)
             ESC[?25l      (hide cursor — we draw our own block cursor)
             ESC[2J ESC[H  (clear, home)
             (full frame render)

On exit:     ESC[?25h      (show cursor)
             ESC[?1049l    (restore main screen buffer)
             ESC[0m        (reset attributes)
```

### 10.5 Terminal size detection

Send `ESC[18t` and parse the `ESC[8;rows;colst` report. Fallback:
send cursor to `999;999` + DSR (`ESC[6n`) and read back the clamped
position. If `< 88×30`, render a centred "Resize to at least 88×30"
notice and suppress the TUI.

---

## 11. Navigation and input

### 11.1 ESC sequence parser

A state machine over the incoming byte stream:

```
read byte b:
  if s_in_esc:
     collect into s_esc_buf; on final byte decode:
       ESC [ A → KEY_UP       ESC [ B → KEY_DOWN
       ESC [ C → KEY_RIGHT    ESC [ D → KEY_LEFT
       (ESC O A/B/C/D — application cursor mode: handle both)
     lone ESC (no following '[' within ~10 ms) → KEY_ESCAPE
     clear s_in_esc
  else:
     0x1B → s_in_esc = true; s_esc_len = 0
     '\r' or '\n' → KEY_ENTER
     0x7F or 0x08 → KEY_BACKSPACE
     '\t' → KEY_TAB
     0x03 (Ctrl-C) → ignored (no-op)
     0x20..0x7E → KEY_PRINTABLE(b)
     digits '1'..'7' → KEY_DIGIT(b - '0')
```

The 10 ms timeout for lone-ESC detection is reliable at 115200 baud.

### 11.2 Focus model

**FOCUS_MENU:**
- `↑`/`↓` move selection (wrap at boundaries).
- `1`–`7` jump to an item directly.
- `Enter` or `Tab` push focus into the panel.

**FOCUS_PANEL:**
- **Console screen:** printable chars append to `input`; Backspace
  deletes; Enter runs the command (§9.2); Esc returns to FOCUS_MENU.
- **Graph screen:** `↑`/`↓`/`←`/`→` cycle the channel (temp →
  humid → press); Esc returns to FOCUS_MENU.
- **Other screens:** Esc returns to FOCUS_MENU.

Focused panel title uses `TITLE_ACT` (brighter, optionally bold).

---

## 12. Provisioning workflow (UC-16, REQ-LI-030..120)

Identical to FD CLI companion §9. The REPL provides a line-oriented
interface within the TUI panel.

### 12.1 `prov set <key> <value>`

Validates and stages into `s_prov_pending`. Validation rules:

| Key | Validation |
|---|---|
| `wifi-ssid` | 1..63 printable chars |
| `wifi-pass` | 8..63 printable chars |
| `mqtt-endpoint` | valid hostname or IPv4, ≤128 chars |
| `modbus-addr` | integer 1..247 |
| `modbus-baud` | one of {1200, 2400, 4800, 9600, 19200} |
| `modbus-parity` | one of {N, E, O} |

### 12.2 `prov commit`

Print staged summary → confirm prompt `Apply? [y/N]: ` → read char
with 30 s timeout → on `y`: `s_cfg_write->apply_prov_block()` → on
success: clear pending, print `[OK]`; on failure: retain pending,
print `[ERR]`. On `n` or timeout: discard, print `[INFO] Discarded.`

**Confirm prompt in TUI context.** The confirm prompt replaces the
normal `envmon>` prompt in the REPL input line. The input handler
temporarily enters a single-char-read mode: any character other than
`y`/`Y` is treated as decline. The 30 s timeout is implemented via
the data-tick counter (30 ticks at 1 Hz).

### 12.3 `prov discard`

Clears `s_prov_pending`, sets `dirty = false`, appends
`[INFO] Discarded.` to scrollback.

---

## 13. Operational config workflow

`config set / commit / discard` follow the same algorithm as §12 with
`s_cfg_pending` as the staging struct and
`s_cfg_write->apply_cfg_block()` as the apply call.

---

## 14. Self-test (REQ-LI-130..160)

Same logic as FD CLI companion §11, adapted for GW interfaces.

| Subsystem | Method | Evidence |
|---|---|---|
| Sensors | `s_sensors->force_read()` | `valid == true` = pass |
| Comms links | `s_health->get()` | `wifi_ok && mqtt_ok && modbus_master_ok` |
| Flash | `s_cfg_write->round_trip_test()` | match = pass |

Result printed as a formatted table in the REPL scrollback and
persisted via `s_cfg_write->store_selftest_result()`.

---

## 15. Sequence integration

| SD | Component role | Key function |
|---|---|---|
| SD-10 | Field Technician enters provisioning commands via TUI REPL; ConsoleService parses each and calls `s_cfg_write->apply_prov_block()` or `s_prof_write->add_profile()` | `console_task_body()`, `cmd_prov_commit()` |
| SD-00b | LifecycleController calls `console_service_init()` during boot, before task creation | `console_service_init()` |

---

## 16. Thread safety

`ConsoleService` state (all `s_*` file-scope variables) is accessed
only within `ConsoleTask`. No mutex needed for internal state.

`s_ring_buf` is a single-producer / single-consumer lock-free ring.
The `DebugUartDriver` RX ISR writes (`s_ring_head`); `ConsoleTask`
reads (`s_ring_tail`). Power-of-2 size enables masked-index
arithmetic without locks.

Provider calls (`s_sensors->force_read()`, `s_health->get()`,
`s_alarms->get_active()`, `s_cfg_read->get_params()`,
`s_prof_read->list_profiles()`) are individually thread-safe inside
each provider. `ConsoleTask` does not hold locks while issuing these
calls.

**Chart history arrays** (`s_chart_history`) are written and read
only within `ConsoleTask` — no concurrency concern.

---

## 17. Initialisation order

`LifecycleController` calls `console_service_init()` during the
post-driver, pre-task-creation phase:

```
drivers → middleware → application services:
    ConfigService → SensorService → AlarmService → HealthMonitor
                  → ConsoleService             ← here
→ task creation → scheduler start
```

On return from `console_service_init()`:

- All injected interface pointers stored in file-scope statics.
- `s_ui` zero-initialised (sel=0, focus=MENU, channel=0).
- `s_grid_cur`, `s_grid_prev` zero-initialised.
- `s_ring_buf`, `s_prov_pending`, `s_cfg_pending`, `s_scrollback`
  zero-initialised.
- `s_chart_history` zero-initialised (chart starts flat).
- `s_initialised = true`.
- 1 Hz and 2 Hz software timers created.

`console_task_body` is then registered via `xTaskCreateStatic` by
`LifecycleController`. On first run, the task sends the init
sequence (§10.4), detects terminal size (§10.5), and renders the
first full frame.

---

## 18. Memory and sizing

| Item | Size |
|---|---|
| Injected interface pointers (×9) | 36 B |
| `s_ui` (ui_state_t) | ~132 B |
| `s_grid_cur` (cell_grid_t, 88×22×16 B/cell) | ~30,976 B |
| `s_grid_prev` (cell_grid_t) | ~30,976 B |
| `s_ring_buf` | 256 B |
| ESC parser state | 16 B |
| `s_chart_history` (3×92×2 B) | 552 B |
| `s_scrollback` (200×60 B) | ~12,000 B |
| `s_prov_pending` | ~320 B |
| `s_cfg_pending` | ~48 B |
| Scalar state | ~32 B |
| **Total static RAM** | **~75.3 KB** |
| Command table (~24 × 28 B) `.rodata` | ~672 B |
| Task stack (FreeRTOS, static) | 3,072 B |

**RAM budget note.** The two cell grids (62 KB) dominate. The
B-L475E-IOT01A has 128 KB SRAM (two banks: 96 KB + 32 KB). 75 KB
for ConsoleService is significant but acceptable — the GW has no LCD
UI task competing for RAM. If RAM becomes tight, the cell struct can
be compacted (e.g., palette-indexed colour reduces 6 RGB bytes to 1
index byte, dropping each grid to ~7 KB). Tracked as **GW-CS-O1**.

No dynamic allocation post-init (P5).

---

## 19. Error and fault behaviour

All public functions return `console_service_err_t`. All handler
errors are mapped to a human-readable `[ERR]` line in the REPL
scrollback before returning. Errors do not affect `ConsoleTask`
stability — the TUI is always re-rendered after any command outcome.

| Error value | Cause | Local behaviour | Retry | Observability |
|---|---|---|---|---|
| `ERR_NULL_ARG` | Null pointer to init | Return error; no action | No — programming error | Logged ERROR via ILogger |
| `ERR_NOT_INIT` | Function called before init | Return error | No — programming error | Logged ERROR via ILogger |
| `ERR_VALIDATION` | Input value out of range | `[ERR]` in scrollback; return | No — operator re-enters | Logged DEBUG via ILogger |
| `ERR_UNKNOWN_KEY` | Unknown command or key | `[ERR]` in scrollback; return | No — operator corrects | Logged WARN via ILogger |
| `ERR_APPLY_FAILED` | `IConfigManager.apply_*()` failed | `[ERR]` in scrollback; pending retained | Operator may retry | Logged WARN via ILogger |
| `ERR_TIMEOUT` | Confirm prompt 30 s timeout | `[ERR]` in scrollback; pending discarded | Operator restarts | Logged DEBUG via ILogger |
| `ERR_LINE_OVERFLOW` | Input line exceeded 120 chars | Input cleared; `[ERR]` in scrollback | Operator re-enters | Logged WARN via ILogger |

---

## 20. Principles applied

- **P1 (Strict directional layering).** Depends on `DebugUartDriver`
  (driver layer) for byte I/O; application and middleware targets
  consumed via injected vtable pointers. No upward dependencies.
- **P2 (Dependency Inversion).** Exposes `iconsole_service_t`
  vtable; `LifecycleController` depends on `IConsoleService`.
  Command targets consumed via their interfaces — no concrete-type
  dependency.
- **P3 (Interface Segregation).** `DeviceProfileRegistry` consumed
  via ISP-split `IDeviceProfileProvider` (read, for `profiles list`)
  and `IDeviceProfileManager` (write, for `profiles add/remove`).
  `ConfigService` consumed via `IConfigProvider` (read) and
  `IConfigManager` (write). This separates display-only paths from
  mutation paths.
- **P4 (Cross-cutting concern exception).** `Logger` and
  `TimeProvider` referenced concretely per the cross-cutting
  exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** All
  buffers are static: cell grids, ring buffer, scrollback, chart
  history. Command table is `const` in `.rodata`. No heap.
- **P6 (Responsibility traces to requirements).** Every CLI command
  in §9.3 traces to a specific `REQ-LI-*` or `REQ-DM-*` requirement
  — see §2 table.
- **P7 (Pull-based access).** All data pulled from injected
  providers at render time or command time. The chart pulls
  thresholds each frame. No subscriptions, no callbacks from
  providers into ConsoleService.
- **P8 (Total error propagation, no silent failures).**
  `console_service_err_t` on init and `run_once`; command errors
  printed to REPL scrollback and returned per §19.
- **P9 (BARR-C coding standard).** Fixed-width types throughout;
  `const` for read-only pointers; no magic numbers.
- **P10 (Naming conventions).** Prefix `console_service_`; interface
  `IConsoleService` → `iconsole_service_t`; errors
  `CONSOLE_SERVICE_ERR_*`.

---

## 21. Source file structure

```
firmware/gateway/app/console_service/
├── console_service.c/.h    singleton, init, run_once, task body
├── core/
│   ├── ui_state.c/.h       screen/focus state machine
│   ├── render.c/.h         frame composition → cell grid
│   ├── chart.c/.h          block-area chart + proximity colour
│   ├── repl.c/.h           REPL parser + scrollback management
│   ├── commands.c/.h       command table + handlers
│   ├── theme.c/.h          colour palette, SGR helpers
│   ├── termout.c/.h        escape-sequence writer + dirty-line diff
│   └── input.c/.h          serial byte → key decoder (ESC parser)
└── port/
    ├── serial_host.c       host simulator: stdin/stdout raw mode
    └── serial_vcp.c        GW: ST-Link VCP via IDebugUart
```

`core/` is board-independent. The same `core/` source tree compiles
for both the host simulator and the GW target. `port/` provides the
byte I/O backend.

### 21.1 Host simulator

`serial_host.c` + a `main_host.c` (outside the firmware tree, in
`tools/console_simulator/`): put `stdin` in raw mode (`tcsetattr`,
`~(ICANON|ECHO)`, `VMIN=0/VTIME=0`), write to `stdout`. Run it in
an 88×30+ truecolor terminal to iterate the full TUI without
flashing.

Feed it mock data via a stub that random-walks temperature, humidity,
and pressure, crossing thresholds periodically to demonstrate the
proximity colour gradient.

### 21.2 Build order

1. `theme.c` + `termout.c` — coloured box rendered and diffed.
2. `chart.c` — block-area chart against a static array; match HTML
   reference.
3. `render.c` — banner + menu + graph panel at 88×22.
4. `input.c` + `ui_state.c` — navigation, focus model.
5. `repl.c` + `commands.c` — Console screen; `help`, `sensors`,
   `set` working.
6. Wire injected interfaces; `set` re-colours the chart.
7. `serial_vcp.c` — bring up on the GW over ST-Link VCP.

---

## 22. Unit test plan (Pass H)

### 22.0 Framework, location, and mock strategy

**Framework:** Unity + CMock (ThrowTheSwitch.org).

**Files:**

- `tests/firmware/gateway/application/console_service/test_console_service_gw_tui.c` — init, REPL parser, shared commands, config/prov workflows, self-test, GW-specific commands.
- `tests/firmware/gateway/application/console_service/test_chart.c` — chart rendering, proximity colour, resampling.
- `tests/firmware/gateway/application/console_service/test_ui_state.c` — navigation state machine, focus model, ESC parser.
- `tests/firmware/gateway/application/console_service/test_render.c` — frame composition, dirty-line diffing.

**Build target:** host (PC).

**Mock strategy:**

All dependencies are injected interfaces. CMock generates mocks from
each interface header:

- **`IDebugUart` mock.** TX output captured via a `write` stub into
  `s_uart_capture[]`. Not used for TUI visual assertions (too
  complex); instead, tests assert on cell grid contents after
  `render_compose()`, or on scrollback lines after command dispatch.
- **`ISensorService` mock.** `force_read` / `get_latest` return
  canned `sensor_reading_t`.
- **`IAlarmService` mock.** `get_active` returns a canned alarm list.
- **`IConfigProvider` mock.** `get_params` returns a canned parameter
  set.
- **`IConfigManager` mock.** `apply_prov_block` / `apply_cfg_block` /
  `round_trip_test` / `store_selftest_result` /
  `get_selftest_result` return configurable results.
- **`IHealthSnapshot` mock.** `get` returns a canned health snapshot
  with configurable fields.
- **`IDeviceProfileProvider` mock.** `list_profiles` returns a
  canned list.
- **`IDeviceProfileManager` mock.** `add_profile` / `remove_profile`
  capture calls and return configurable results.
- **`ILogger` mock.** Captures log level and message for
  observability assertions.

**Test isolation:** `console_service_reset_for_test(void)` under
`#ifdef TEST`.

### 22.1 Initialisation suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-001 | `console_service_init(NULL, …)` → `ERR_NULL_ARG` | P8 |
| TC-GCS-002 | Null sensors → `ERR_NULL_ARG` | P8 |
| TC-GCS-003 | Null alarms → `ERR_NULL_ARG` | P8 |
| TC-GCS-004 | Null cfg_read → `ERR_NULL_ARG` | P8 |
| TC-GCS-005 | Null cfg_write → `ERR_NULL_ARG` | P8 |
| TC-GCS-006 | Null health → `ERR_NULL_ARG` | P8 |
| TC-GCS-007 | Null prof_read → `ERR_NULL_ARG` | P8 |
| TC-GCS-008 | Null prof_write → `ERR_NULL_ARG` | P8 |
| TC-GCS-009 | Null logger → `ERR_NULL_ARG` | P8 |
| TC-GCS-010 | Valid init → `ERR_OK`; `s_ui.sel == 0`, `s_ui.focus == FOCUS_MENU` | — |
| TC-GCS-011 | After init: `s_prov_pending.dirty == false`, `s_cfg_pending.dirty == false` | — |
| TC-GCS-012 | `run_once` before init → `ERR_NOT_INIT` | P8 |
| TC-GCS-013 | Init sends alternate-screen + hide-cursor + clear sequences to UART | §10.4 |

### 22.2 Chart algorithm suite (`test_chart.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-020 | Flat data (all 22.0°C) → all columns same height, all BASE colour | §8 |
| TC-GCS-021 | Linearly rising data crossing threshold → columns transition BASE → WARN → OVER | §8.3 |
| TC-GCS-022 | Data exactly at threshold → proximity = 1.0, colour = WARN | §8.3 |
| TC-GCS-023 | Data well below threshold → proximity ≤ 0, colour = BASE | §8.3 |
| TC-GCS-024 | Data well above threshold → proximity > 1, colour → OVER | §8.3 |
| TC-GCS-025 | Threshold row: empty cells show `╌` in THR; filled cells show block | §8.2 |
| TC-GCS-026 | Channel with lo-threshold only: proximity computed from lo side | §8.3 |
| TC-GCS-027 | Resampling 92 → 46 columns by linear interpolation produces correct intermediate values | §8.2 |
| TC-GCS-028 | BLK index clamped to 0..8 (no out-of-bounds) | §8.1 |
| TC-GCS-029 | Status chip: value safe → `● ok`; near threshold → `▲ near`; past → `■ alarm`; stale → `▲ stale` | §8.4 |
| TC-GCS-030 | glow > 0.5 → bold attribute set on cell | §8.3 |
| TC-GCS-031 | Adjacent columns with same colour produce coalesced SGR run | §8.6 |

### 22.3 UI state / navigation suite (`test_ui_state.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-040 | Initial state: sel=0, focus=MENU | — |
| TC-GCS-041 | KEY_DOWN in MENU: sel increments (0→1, 1→2, …) | §11.2 |
| TC-GCS-042 | KEY_DOWN at sel=6: wraps to 0 | §11.2 |
| TC-GCS-043 | KEY_UP at sel=0: wraps to 6 | §11.2 |
| TC-GCS-044 | KEY_DIGIT(3) in MENU: sel jumps to 2 (0-indexed) | §11.2 |
| TC-GCS-045 | KEY_ENTER in MENU: focus → PANEL | §11.2 |
| TC-GCS-046 | KEY_TAB in MENU: focus → PANEL | §11.2 |
| TC-GCS-047 | KEY_ESCAPE in PANEL: focus → MENU | §11.2 |
| TC-GCS-048 | KEY_UP/DOWN in PANEL + Graph: channel cycles 0→1→2→0 | §11.2 |
| TC-GCS-049 | KEY_PRINTABLE in PANEL + Console: appends to input | §11.2 |
| TC-GCS-050 | KEY_BACKSPACE in PANEL + Console: deletes last char | §11.2 |
| TC-GCS-051 | KEY_ENTER in PANEL + Console: dispatches command | §9.2 |
| TC-GCS-052 | KEY_DIGIT in PANEL + Console: treated as printable, not menu jump | §11.2 |

### 22.4 ESC sequence parser suite (`test_ui_state.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-060 | Byte sequence `0x1B 0x5B 0x41` → KEY_UP | §11.1 |
| TC-GCS-061 | Byte sequence `0x1B 0x5B 0x42` → KEY_DOWN | §11.1 |
| TC-GCS-062 | Byte sequence `0x1B 0x5B 0x43` → KEY_RIGHT | §11.1 |
| TC-GCS-063 | Byte sequence `0x1B 0x5B 0x44` → KEY_LEFT | §11.1 |
| TC-GCS-064 | Byte `0x1B` alone (no follow-up within timeout) → KEY_ESCAPE | §11.1 |
| TC-GCS-065 | Application cursor mode `0x1B 0x4F 0x41` → KEY_UP | §11.1 |
| TC-GCS-066 | `0x0D` → KEY_ENTER; `0x0A` → KEY_ENTER | §11.1 |
| TC-GCS-067 | `0x7F` → KEY_BACKSPACE; `0x08` → KEY_BACKSPACE | §11.1 |
| TC-GCS-068 | `0x09` → KEY_TAB | §11.1 |
| TC-GCS-069 | Printable byte `0x61` ('a') → KEY_PRINTABLE('a') | §11.1 |

### 22.5 Render suite (`test_render.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-080 | `render_compose` with sel=0, focus=MENU: graph panel title uses TITLE (not TITLE_ACT) | §7.5 |
| TC-GCS-081 | `render_compose` with sel=0, focus=PANEL: graph panel title uses TITLE_ACT | §11.2 |
| TC-GCS-082 | Selected menu item (sel=2): row has SEL_BG background, `▸` cursor | §7.4 |
| TC-GCS-083 | Banner detail line: alarm count > 0 uses ERR colour | §7.3 |
| TC-GCS-084 | Footer hint matches focus + screen combination per §7.6 table | §7.6 |
| TC-GCS-085 | `termout_flush` with identical grids: zero bytes emitted | §10.3 |
| TC-GCS-086 | `termout_flush` with one changed cell: emits cursor-move + SGR + glyph + reset | §10.3 |
| TC-GCS-087 | `termout_flush` with a run of 5 same-colour changed cells: one SGR for the run | §10.3 |

### 22.6 REPL command suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-100 | `help` adds lines for every entry in s_cmd_table to scrollback | — |
| TC-GCS-101 | `?` (alias) dispatches to help handler | — |
| TC-GCS-102 | `version` adds firmware version line to scrollback | — |
| TC-GCS-103 | `serial` adds MCU UID line to scrollback | REQ-LI-150 |
| TC-GCS-104 | `sensors` adds table with all channels to scrollback | REQ-LI-010 |
| TC-GCS-105 | `read t` adds single temp reading to scrollback | REQ-LI-010 |
| TC-GCS-106 | `read x` (invalid) → `[ERR]` in scrollback | REQ-LI-0E2 |
| TC-GCS-107 | `status` adds health snapshot fields to scrollback | REQ-LI-010 |
| TC-GCS-108 | `alarms` with no active alarms → "No active alarms" in OK colour | REQ-LI-010 |
| TC-GCS-109 | `alarms` with active alarm → alarm entry in scrollback | REQ-LI-010 |
| TC-GCS-110 | `thresholds` adds HI/LO per channel to scrollback | REQ-LI-010 |
| TC-GCS-111 | `set t.hi 24` → calls IConfigManager; `applied` in scrollback | REQ-LI-010 |
| TC-GCS-112 | `set t.hi abc` (non-numeric) → `[ERR]` in scrollback | REQ-LI-090 |
| TC-GCS-113 | `set x.hi 24` (bad channel) → `[ERR]` in scrollback | REQ-LI-0E2 |
| TC-GCS-114 | `uptime` adds uptime line to scrollback | — |
| TC-GCS-115 | `clear` empties scrollback (count=0) | — |
| TC-GCS-116 | `net` adds WiFi + MQTT + Modbus info to scrollback | — |
| TC-GCS-117 | Unknown command → `unknown command` in WARN in scrollback | — |
| TC-GCS-118 | Empty input → bare prompt line echoed, no handler invoked | — |
| TC-GCS-119 | Input exceeding 120 chars → `[ERR] line overflow`, input cleared | — |
| TC-GCS-120 | Scrollback at 200 lines + new output → oldest line evicted | §5.7 |

### 22.7 Provisioning workflow suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-130 | `prov set wifi-ssid MyNetwork` → staged, dirty=true | REQ-LI-040 |
| TC-GCS-131 | `prov set wifi-ssid ""` → `[ERR]`, dirty unchanged | REQ-LI-090 |
| TC-GCS-132 | `prov set wifi-ssid <64-char>` → `[ERR]`, dirty unchanged | REQ-LI-090 |
| TC-GCS-133 | `prov set wifi-pass ValidPa$$` (8 chars) → staged | REQ-LI-040 |
| TC-GCS-134 | `prov set wifi-pass short` (< 8) → `[ERR]` | REQ-LI-090 |
| TC-GCS-135 | `prov set wifi-pass <64-char>` (> 63) → `[ERR]` | REQ-LI-090 |
| TC-GCS-136 | `prov set mqtt-endpoint broker.example.com` → staged | REQ-LI-050 |
| TC-GCS-137 | `prov set mqtt-endpoint <129-char>` → `[ERR]` | REQ-LI-090 |
| TC-GCS-138 | `prov set modbus-addr 50` → staged | REQ-LI-070 |
| TC-GCS-139 | `prov set modbus-addr 0` → `[ERR]` | REQ-LI-090 |
| TC-GCS-140 | `prov set modbus-addr 248` → `[ERR]` | REQ-LI-090 |
| TC-GCS-141 | `prov set modbus-baud 9600` → staged | REQ-LI-080 |
| TC-GCS-142 | `prov set modbus-baud 7200` → `[ERR]` | REQ-LI-090 |
| TC-GCS-143 | `prov set modbus-parity N/E/O` → staged | REQ-LI-080 |
| TC-GCS-144 | `prov set modbus-parity X` → `[ERR]` | REQ-LI-090 |
| TC-GCS-145 | `prov set unknown-key foo` → `ERR_UNKNOWN_KEY` | REQ-LI-0E2 |
| TC-GCS-146 | `prov commit` dirty=false → `[INFO] Nothing staged` | — |
| TC-GCS-147 | `prov commit` + `y` → `apply_prov_block` called, dirty=false, `[OK]` | REQ-LI-100..110 |
| TC-GCS-148 | `prov commit` + `n` → not called, pending cleared | REQ-LI-0E3 |
| TC-GCS-149 | `prov commit` + 30 s timeout → same as decline | REQ-LI-0E3 |
| TC-GCS-150 | `prov commit` + apply error → dirty=true retained, `[ERR]` | REQ-LI-120 |
| TC-GCS-151 | `prov discard` → pending cleared, `[INFO] Discarded` | REQ-LI-0E3 |

### 22.8 Config workflow suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-160 | `config set polling-interval-ms 1000` → staged, dirty=true | REQ-LI-010 |
| TC-GCS-161 | `config set polling-interval-ms abc` → `[ERR]` | REQ-LI-090 |
| TC-GCS-162 | `config set unknown-key 42` → `ERR_UNKNOWN_KEY` | REQ-LI-0E2 |
| TC-GCS-163 | `config commit` dirty=false → `[INFO] Nothing staged` | — |
| TC-GCS-164 | `config commit` + `y` → `apply_cfg_block` called, dirty=false | REQ-LI-100..110 |
| TC-GCS-165 | `config commit` + `n` → decline; pending cleared | REQ-LI-0E3 |
| TC-GCS-166 | `config commit` + timeout → decline | REQ-LI-0E3 |
| TC-GCS-167 | `config commit` + apply error → dirty retained | REQ-LI-120 |
| TC-GCS-168 | `config discard` → pending cleared | REQ-LI-0E3 |

### 22.9 Self-test suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-170 | `selftest` all pass → overall PASS, result persisted | REQ-LI-130, 160 |
| TC-GCS-171 | `selftest` sensor probe invalid → sensor FAIL, overall FAIL | REQ-LI-140 |
| TC-GCS-172 | `selftest` wifi_ok=false → comms FAIL | REQ-LI-140 |
| TC-GCS-173 | `selftest` mqtt_ok=false → comms FAIL | REQ-LI-140 |
| TC-GCS-174 | `selftest` modbus_master_ok=false → comms FAIL | REQ-LI-140 |
| TC-GCS-175 | `selftest` round_trip_test error → flash FAIL | REQ-LI-140 |
| TC-GCS-176 | `selftest` output: formatted table in scrollback | REQ-LI-130 |
| TC-GCS-177 | `selftest-result` → prints last stored result | REQ-LI-160 |
| TC-GCS-178 | `selftest-result` no stored → prints notice | REQ-LI-160 |

### 22.10 GW-specific command suite

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-180 | `profiles list` calls `IDeviceProfileProvider.list_profiles`; lines in scrollback | REQ-MB-110 |
| TC-GCS-181 | `profiles list` empty → `No profiles registered` | — |
| TC-GCS-182 | `profiles add addr=5 sensors=temp,hum` → calls `IDeviceProfileManager.add_profile` | REQ-MB-111 |
| TC-GCS-183 | `profiles add` malformed → `[ERR]` | REQ-LI-0E2 |
| TC-GCS-184 | `profiles remove 5` → calls `remove_profile(5)` | REQ-MB-111 |
| TC-GCS-185 | `profiles remove 0` → `[ERR]` (invalid addr) | REQ-LI-090 |
| TC-GCS-186 | `wifi status` → WiFi fields from IHealthSnapshot in scrollback | REQ-LI-010 |
| TC-GCS-187 | `mqtt status` → MQTT fields in scrollback | REQ-LI-010 |
| TC-GCS-188 | `modbus status` → Modbus master fields in scrollback | REQ-LI-010 |

### 22.11 Cross-cutting

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-GCS-190 | Every command returns to prompt — scrollback ends with prompt line | — |
| TC-GCS-191 | Handler returning ERR_VALIDATION logs at DEBUG | P8 |
| TC-GCS-192 | Handler returning ERR_APPLY_FAILED logs at WARN | P8 |
| TC-GCS-193 | `console_service_init` failure logs at ERROR | P8 |
| TC-GCS-194 | Ring buffer overflow: next render shows `[WARN] RX overflow` in footer | — |

### 22.12 Error-code coverage cross-check

| Error code | Produced by TC(s) |
|---|---|
| `ERR_OK` | TC-GCS-010, -011, -040, -100..118, -130, -133, -136, -138, -141, -143, -146, -147, -151, -160, -163, -164, -168, -170 |
| `ERR_NULL_ARG` | TC-GCS-001..009 |
| `ERR_NOT_INIT` | TC-GCS-012 |
| `ERR_VALIDATION` | TC-GCS-106, -112, -113, -131, -132, -134, -135, -137, -139, -140, -142, -144, -161 |
| `ERR_UNKNOWN_KEY` | TC-GCS-117, -145, -162 |
| `ERR_APPLY_FAILED` | TC-GCS-150, -167 |
| `ERR_TIMEOUT` | TC-GCS-149, -166 |
| `ERR_LINE_OVERFLOW` | TC-GCS-119 |

All eight values exercised.

### 22.13 Coverage target

- 100% of public API functions exercised.
- 100% of `s_cmd_table` entries invoked by at least one TC.
- 100% of error codes produced by at least one TC.
- 100% of validation rules exercised (valid + invalid).
- ≥ 85% statement coverage across all source files.
- Chart algorithm: 100% of proximity regions (safe, near, alarm,
  stale) covered.
- Navigation: all focus transitions covered.

### 22.14 Cannot be host-tested

- Actual UART byte timing and ISR-to-task notification latency.
- Ring-buffer overflow under sustained traffic.
- Confirm-prompt 30 s wall-clock timeout accuracy.
- Config persistence across power cycles.
- Real sensor data rendering on physical terminal.
- Terminal size detection via escape sequence handshake.
- ST-Link VCP throughput and flow control.

These are covered by the §23 integration test plan.

---

### 22.15 Results (v1.1)

Host (Ceedling, Gateway project): `test_console_service_gw_tui` 84, `test_chart` 19, `test_ui_state` 38,
`test_render` 28, `test_fmt` 8 — all pass, 0 ignored. DebugUart raw-RX: 7 added to the driver suite.
The 4-file split in §22.0 became 5 files (formatting helpers have their own suite); the spies live in
`console_spies.h`. Where §22 expected a wording or a staged value that §0 changed, the test checks the
reconciled behaviour.

## 23. Integration test plan

On-target tests (Gateway board). Executed manually with a serial
terminal (PuTTY, minicom, or Windows Terminal) attached to the
ST-Link VCP at 115200 8N1.

| TC | Setup | Pass criterion |
|---|---|---|
| IT-GCS-001 | Boot; observe TUI | Full-screen TUI renders within 2 s; banner, menu, graph panel visible |
| IT-GCS-002 | Verify banner | Logo correct; fw version, board, connectivity dots match reality |
| IT-GCS-003 | Navigate menu ↑↓ | Selection highlight moves; wrap at boundaries |
| IT-GCS-004 | Jump 1–7 | Each digit selects the correct menu item |
| IT-GCS-005 | Enter/Tab → panel | Focus indicator changes; panel title brightens |
| IT-GCS-006 | Esc → menu | Returns to menu focus |
| IT-GCS-007 | Graph channel cycle | ↑↓ in graph panel switches temp → humid → press; chart re-renders |
| IT-GCS-008 | Chart proximity colour | With temp near threshold: chart visibly transitions cyan → amber → red |
| IT-GCS-009 | Status panel | All health fields populated from live system |
| IT-GCS-010 | Alarms panel | Shows active alarms or "No active alarms" |
| IT-GCS-011 | Network panel | WiFi, MQTT, Modbus status populated |
| IT-GCS-012 | Console REPL: `help` | All commands listed |
| IT-GCS-013 | Console REPL: `sensors` | All channels with values and status |
| IT-GCS-014 | Console REPL: `set t.hi 24` | Threshold applied; chart re-colours on next tick |
| IT-GCS-015 | Console REPL: `prov set wifi-ssid Test` + `prov commit` → `y` | Provisioning applied; survives reboot |
| IT-GCS-016 | Console REPL: `prov commit` + `n` | Discarded; config unchanged |
| IT-GCS-017 | Console REPL: `prov commit` + 30 s timeout | Times out; pending discarded |
| IT-GCS-018 | Console REPL: `selftest` | Per-subsystem results; overall pass/fail |
| IT-GCS-019 | Console REPL: `profiles list` | Shows registered device profiles |
| IT-GCS-020 | Console REPL: `profiles add addr=5 sensors=temp,hum` + `profiles list` | New profile appears |
| IT-GCS-021 | Console REPL: `wifi status` | WiFi info matches reality |
| IT-GCS-022 | Console REPL: `mqtt status` | MQTT info matches reality |
| IT-GCS-023 | Console REPL: `modbus status` | Modbus master info matches reality |
| IT-GCS-024 | Console REPL: `reboot` | Teardown sequence sent; board resets; TUI re-appears |
| IT-GCS-025 | Terminal < 88×30 | "Resize" notice shown; TUI suppressed |
| IT-GCS-026 | Terminal ≥ 88×30 after resize | TUI renders correctly |
| IT-GCS-027 | 1 Hz data tick | Chart updates with fresh sensor data each second |
| IT-GCS-028 | Cursor blink | Console input cursor blinks at ~2 Hz when panel focused |
| IT-GCS-029 | Validation: `prov set modbus-addr 300` | `[ERR]` in scrollback; no change |

**Bench result, 2026-10-24** (`integration-tests/console_service` driven by `scripts/console_bench.py` over the ST-Link VCP; real sensors, ConfigService on ConfigStore, WiFi associated, TLS broker on the laptop; two consecutive runs from a clean reset): **56 of 56 checks PASS**, covering IT-GCS-001..019, 021..029 (including the 30 s prompt timeout, the `y` commit, a committed broker address surviving a reboot, the small-terminal notice and its recovery, the cursor blink, PgUp / PgDn paging, status commands against the real WiFi and MQTT links, and the Modbus link correctly reported offline with no Field Device). Not applicable by design: IT-GCS-020 (`profiles add`, GCS-O5).

---

## 24. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| **CS-O4** | GW `components.md` entry omits `IConfigManager` | **Closed 2026-10-31 (WP12):** the Gateway `ConsoleService` entry in `components.md` now lists `IConfigManager`. | Closed |
| **CS-O5** | Self-test depth: driver-level loopback not in interface list | Evaluate at integration | Open |
| **CS-O6** | Certificate provisioning (REQ-LI-060) multi-line paste mode | Define `prov cert-begin` / `prov cert-end` UX at implementation | Open |
| **CS-O7** | Confirm-prompt blocking read timeout mechanism | Implemented with the data-tick counter (30 ticks at 1 Hz), TC-GCS-149 / 166. | Closed |
| **CS-O8** | `profiles add` input format | Recommendation: key=value for CLI simplicity | Open |
| **CS-O9** | GW `components.md` entry omits `IAlarmService` and lists `DeviceProfileRegistry` instead of ISP-split interfaces | **Closed 2026-10-31 (WP12):** the entry lists `IAlarmService`, `IDeviceProfileProvider` and `IDeviceProfileManager`, as the note above prescribes. | Closed |
| **GW-CS-O1** | Cell grid RAM (62 KB for two grids) | Closed by GCS-D5: palette-indexed 3-byte cells, 11.6 KB for both grids, 26 KB static in total. | Closed |
| **GW-CS-O2** | 256-colour fallback mapping — token table defined but per-column proximity mapping needs manual verification against target terminals | Test with PuTTY and minicom | Open |
| **GCS-O4** | WiFi credentials (`prov set wifi-ssid / wifi-pass`) have no back-end: no credential store exists on the Gateway. | Needs its own design pass (storage, encryption, WifiTask hand-off). | Open |
| **GCS-O5** | Modbus line settings (`prov set modbus-baud / modbus-parity`) are not configurable; `DeviceProfileRegistry` is read-only (`profiles add / remove`); certificate provisioning (CS-O6) is not implemented. | Registry and Modbus settings modules first; CS-O6 UX later. | Open |
| **GCS-O6** | `selftest` has no flash round-trip and its result is not persisted (CS-O5). | Needs a ConfigService / ConfigStore self-test hook. | Open |
| **GCS-O7** | The REPL panel showed only the last 8 lines, so a long output (`help`) scrolled out of view. | Closed by GCS-D13 (PgUp / PgDn, bench-verified). | Closed |
| **GCS-O8** | Connectivity dots and the link status commands show what HealthMonitor holds. In a production image the producers (WifiTask RSSI polling, CloudPublisher connect events, ModbusPoller link events) are not wired because there is no Gateway `main()`; the console harness stands in with a small link-monitor task. (An earlier note here said uptime never advanced: that was wrong. HealthMonitor computes it from the tick counter; the banner shows `HH:MM`, so the first minute reads 00:00.) | Gateway entry point. | Open |
| **GCS-O9** | Network panel cannot show the WiFi SSID or IP (HealthMonitor does not carry them). | Add to the health snapshot or a WifiTask accessor. | Open |
| **GCS-O13** | The Firmware "Check for update" action has no back-end. | A FirmwareStore / update-source module (Gateway) is needed; the panel then shows the result in the same line. | Open |
| **GCS-O12** | ConsoleTask is the lowest-priority task by design, so during WiFi association and the TLS handshake (about the first 15 s after boot) it can answer about 2 s late. At 921600 baud the receive path is unreliable while those run (GCS-D15); a more robust path (DMA receive into the ring) would allow it. | Consider UART DMA RX when a Gateway `main()` fixes the task set. | Open |
| **GCS-O11** | Logger and ConsoleTask both write to USART1. The bench image leaves the Logger uninitialised so its messages are dropped; in a production image Logger output would be interleaved with the TUI. | Route log lines into the console scrollback, or give the Logger a different sink, when the Gateway `main()` is built. | Open |
| **GCS-O10** | Nothing creates ConsoleTask in a production image (no Gateway `main()`); `console_service` and `sensor_service` / `alarm_service` stay excluded from the default `.cproject` image only because the `cloud_publisher` harness stubs the latter two. | Gateway entry point. | Open |
| **GW-CS-O3** | Host simulator `main_host.c` placement | Placed in `tools/console_simulator/` (with `build.sh`). | Closed |

---

## 25. Phase H checklist

| Gate | Criterion | Status |
|---|---|---|
| H-01 | Sources (§1) match `components.md` | ✅ (with noted gaps CS-O4, CS-O9) |
| H-02 | Traceability table (§2) maps to SRS REQ-LI-* | ✅ |
| H-03 | Public API (§4) — init + vtable + task entry defined | ✅ |
| H-04 | Data types (§5) — all structs and enums specified | ✅ |
| H-05 | Internal state (§6) — all file-scope statics listed | ✅ |
| H-06 | Thread safety (§16) analysed | ✅ |
| H-07 | Initialisation order (§17) in boot sequence | ✅ |
| H-08 | Memory sizing (§18) with totals | ✅ |
| H-09 | Error behaviour (§19) — all error codes, causes, retry | ✅ |
| H-10 | Principles (§20) — P1..P10 applied or justified | ✅ |
| H-11 | Unit tests (§22) — all public APIs, all error codes | ✅ |
| H-12 | Integration tests (§23) — on-target coverage | ✅ |
| H-13 | Open items (§24) tracked with resolution paths | ✅ |
| H-14 | Source file structure (§21) specified | ✅ |

---

## 26. References

- `docs/components.md` (GW ConsoleService entry).
- `docs/task-breakdown.md` §4.2 (`ConsoleTask` definition).
- `docs/sequence-diagrams.md` SD-10 (provisioning flow, UC-16),
  SD-00b (Gateway boot sequence).
- `docs/lld/config-service.md` (defines `IConfigProvider` /
  `IConfigManager`).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot`).
- `docs/architecture-principles.md` P1..P10.
- `design_handoff_envmon_console/` — B1 Cockpit TUI design bundle
  (layout, chart, navigation, REPL, implementation specs).
- `design_handoff_envmon_console/design_reference/app.js` — reference
  TUI renderer and interaction implementation.

---

*GW Console TUI LLD companion v1.0. Authored by Luca Agrippino;
reviewed against the V-Model gate criteria for LLD Pass H.*
