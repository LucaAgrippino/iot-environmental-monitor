# LLD Companion — LifecycleController

**Boards:** Field Device (FD) + Gateway (GW).
**Layer:** Application.

This artefact specifies the software design of the `LifecycleController`
component — boot orchestration, runtime lifecycle state machine,
EditingConfig workflow, board-specific Restarting and UpdatingFirmware
states, and remote-command dispatch.

**Version:** 0.2
**Date:** June 2026
**Status:** Ready for code implementation

**HLD anchor:** LifecycleController in `components.md` (FD + GW
application layer); `state-machines.md` Machine 5 (FD), Machine 1 (GW);
`sequence-diagrams.md` SD-00a / SD-00b / SD-00c.

---

## 1. Sources

| Field | FD | GW |
|---|---|---|
| **Provides** | `ILifecycle` (vtable: `get_state`, `get_reset_cause`, `post_event`, `handle_remote_command`) | `ILifecycle` (same vtable) |
| **Uses** | `IConfigStore`, `IConfigProvider`, `IConfigManager`, `ISensorService`, `IAlarmService`, `IGraphicsLibrary`, `ILcdUi`, `IModbusSlave`, `IConsoleService`, `IHealthMonitor`, `IHealthReport`, `ILogger` | `IConfigStore`, `IConfigProvider`, `IConfigManager`, `ISensorService`, `IAlarmService`, `ICloudPublisher`, `IModbusPoller`, `IUpdateService`, `ITimeService`, `IConsoleService`, `IFirmwareStore`, `IResetDriver`, `IHealthAdmin`, `IHealthReport`, `ILogger` |
| **Hosted in task** | `LifecycleTask` prio 1, 2048 words / 8 KB | `LifecycleTask` prio 1, 2048 words / 8 KB |
| **Activation** | Event — queue receive (depth 4) | Event — queue receive (depth 4) |

The 8 KB task stack is intentionally larger than typical (compare
ConsoleService 2 KB) because the EditingConfig snapshot path passes the
config buffer on its way to `config_manager->snapshot()` and several
init sub-states call into BSP code with deep stack frames.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Boot orchestration | REQ-SA-000..030 | UC-00 |
| Init sub-state failure → Faulted | REQ-SA-040, REQ-NF-202 | UC-00 |
| Reset cause detection | REQ-NF-203 | UC-00 |
| Init time budget | REQ-NF-213 | UC-00 |
| Operational entry | REQ-SA-050 | UC-01..16 |
| EditingConfig | REQ-SA-060, REQ-DM-040 | UC-09 |
| EditingConfig timeout (5 min) | REQ-NF-214 | UC-09 |
| Splash screen *(FD only)* | REQ-LD-200, LD-210, LD-220, LD-230, LD-240 | UC-00 |
| Post-update boot *(GW only)* | REQ-DM-071, REQ-DM-072 | UC-18 |
| Restarting state *(GW only)* | REQ-DM-010, REQ-DM-020, REQ-DM-030 | UC-17 |
| UpdatingFirmware state *(GW only)* | REQ-DM-054 | UC-18 |
| Reset-metrics command dispatch | REQ-DM-040 | UC-17 |

---

## 3. Responsibility

`LifecycleController` is the top-level orchestrator on each board. Two
roles:

1. **Sequencer** — drives the Init sub-state chain at boot, gating
   Operational entry on each subsystem starting up cleanly.
2. **Event router** — reacts to runtime events (restart commands, OTA
   commands, EditingConfig enter/exit, faults) by transitioning the
   lifecycle state machine and coordinating the affected components
   through their injected vtables.

It does **not** own sensor acquisition, display content, cloud
publishing, or Modbus protocol. It activates and gates those
components but does not perform their work.

It is the **single dispatch point for remote commands** that affect
system state (LLD-D12, LLD-D15). `ModbusRegisterMap` and
`CloudPublisher` never reach cross-layer state-changing components
directly — they call `lifecycle_controller->handle_remote_command()`.

`LifecycleController` runs exclusively in `LifecycleTask` (priority 1
— lowest application task; no hard real-time constraint). Events
arrive via a static FreeRTOS queue of depth 4.

---

## 4. Provided interface

### 4.1 Singleton accessor

```c
extern const ilifecycle_t * const lifecycle_controller;
```

### 4.2 Vtable

```c
typedef struct ilifecycle {
    lifecycle_state_t       (*get_state)(void);
    lifecycle_reset_cause_t (*get_reset_cause)(void);
    bool                    (*post_event)(lifecycle_event_t event);
    lifecycle_err_t         (*handle_remote_command)(lifecycle_remote_cmd_t cmd);
} ilifecycle_t;
```

`get_state` is read-only and thread-safe — backed by a `volatile`
state variable written only from `LifecycleTask`.

`get_reset_cause` returns the cause detected once at startup.

`post_event` is non-blocking; returns `false` if the event queue is
full. Safe from any task context. Not ISR-safe (use ISR-specific
post variant if needed; not currently required).

`handle_remote_command` is task-safe but not ISR-safe; callers are
`ModbusSlaveTask` (FD) or `ModbusPollerTask` (GW), both task context.

### 4.3 Initialisation function

```c
lifecycle_err_t
lifecycle_controller_init(const iconfig_store_t        *config_store,
                          const iconfig_provider_t     *cfg_read,
                          const iconfig_manager_t      *cfg_write,
                          const isensor_service_t      *sensors,
                          const ialarm_service_t       *alarms,
                          const iconsole_service_t     *console,
                          const ihealth_report_t       *health_report,
#ifdef BOARD_FIELD_DEVICE
                          const igraphics_library_t    *graphics,
                          const ilcd_ui_t              *lcd_ui,
                          const imodbus_slave_t        *modbus_slave,
#else  /* BOARD_GATEWAY */
                          const icloud_publisher_t     *cloud,
                          const imodbus_poller_t       *modbus_poller,
                          const iupdate_service_t      *update_service,
                          const itime_service_t        *time_service,
                          const ifirmware_store_t      *firmware_store,
                          const ireset_driver_t        *reset_driver,
                          const ihealth_admin_t        *health_admin,
#endif
                          const ilogger_t              *log);
```

Singleton pattern — no `self` parameter. All injected pointers stored
in file-scope statics. Returns `LIFECYCLE_ERR_NULL_ARG` on any null
pointer; `LIFECYCLE_ERR_OK` otherwise.

Init does **not** transition the state machine — it only stores
pointers, creates the event queue, creates the EditingConfig timeout
timer, and detects reset cause. The state machine drives forward only
after `LifecycleTask` starts and calls `lifecycle_controller_run()`.

### 4.4 Task entry

```c
void lifecycle_task_body(void *arg);
```

`xTaskCreateStatic` references this. It runs the state machine
forever; never returns.

### 4.5 Start gate (used by other tasks)

```c
EventGroupHandle_t lifecycle_get_start_gate(void);
#define LIFECYCLE_START_GATE_BIT  (1U << 0)
```

Other application tasks block on this event-group bit before starting
their work loops. `LifecycleController` sets the bit on Operational
entry. Bit index per LC-O3.

---

## 5. Data types

### 5.1 States

```c
typedef enum {
    LIFECYCLE_STATE_INIT           = 0,
    LIFECYCLE_STATE_OPERATIONAL    = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING     = 3,  /* GW only */
    LIFECYCLE_STATE_UPDATING_FW    = 4,  /* GW only */
    LIFECYCLE_STATE_FAULTED        = 5,
} lifecycle_state_t;
```

### 5.2 Reset cause

```c
typedef enum {
    LIFECYCLE_RESET_POWER_ON  = 0,
    LIFECYCLE_RESET_SOFT      = 1,
    LIFECYCLE_RESET_WATCHDOG  = 2,
    LIFECYCLE_RESET_UNKNOWN   = 3,
} lifecycle_reset_cause_t;
```

### 5.3 Error enum

```c
typedef enum {
    LIFECYCLE_ERR_OK              = 0,
    LIFECYCLE_ERR_NULL_ARG        = 1,
    LIFECYCLE_ERR_NOT_INIT        = 2,
    LIFECYCLE_ERR_QUEUE_FULL      = 3,
    LIFECYCLE_ERR_UNKNOWN_CMD     = 4,
    LIFECYCLE_ERR_BAD_STATE       = 5,
} lifecycle_err_t;
```

### 5.4 Events

```c
typedef enum {
    LC_EVENT_CONFIG_EDIT_ENTER     = 0,
    LC_EVENT_CONFIG_EDIT_APPLY     = 1,
    LC_EVENT_CONFIG_EDIT_CANCEL    = 2,
    LC_EVENT_CONFIG_EDIT_TIMEOUT   = 3,
    LC_EVENT_RESTART_REQUESTED     = 4,  /* GW */
    LC_EVENT_RESTART_CONFIRMED     = 5,  /* GW */
    LC_EVENT_RESTART_TIMEOUT       = 6,  /* GW (LC-O4) */
    LC_EVENT_OTA_REQUESTED         = 7,  /* GW */
    LC_EVENT_SELF_CHECK_PASS       = 8,  /* GW */
    LC_EVENT_SELF_CHECK_FAIL       = 9,  /* GW */
    LC_EVENT_UNRECOVERABLE_FAULT   = 10,
} lifecycle_event_type_t;

typedef struct {
    lifecycle_event_type_t type;
    uint32_t               param;  /* fault code, OTA size, etc. */
} lifecycle_event_t;
```

### 5.5 Remote commands

```c
typedef enum {
    LC_REMOTE_CMD_SOFT_RESTART   = 0,  /* Modbus 0x0202; two-step (REQ-DM-020) */
    LC_REMOTE_CMD_RESET_METRICS  = 1,  /* Modbus 0x0201; direct dispatch       */
} lifecycle_remote_cmd_t;
```

---

## 6. Internal state

All state is file-scope static — no heap, no per-instance context.

```c
/* Injected interface pointers */
static const iconfig_store_t        *s_config_store;
static const iconfig_provider_t     *s_cfg_read;
static const iconfig_manager_t      *s_cfg_write;
static const isensor_service_t      *s_sensors;
static const ialarm_service_t       *s_alarms;
static const iconsole_service_t     *s_console;
static const ihealth_report_t       *s_health_report;
#ifdef BOARD_FIELD_DEVICE
static const igraphics_library_t    *s_graphics;
static const ilcd_ui_t              *s_lcd_ui;
static const imodbus_slave_t        *s_modbus_slave;
#else
static const icloud_publisher_t     *s_cloud;
static const imodbus_poller_t       *s_modbus_poller;
static const iupdate_service_t      *s_update_service;
static const itime_service_t        *s_time_service;
static const ifirmware_store_t      *s_firmware_store;
static const ireset_driver_t        *s_reset_driver;
static const ihealth_admin_t        *s_health_admin;
#endif
static const ilogger_t              *s_log;

/* Machine state */
static volatile lifecycle_state_t   s_state;       /* read by other tasks */
static lifecycle_reset_cause_t      s_reset_cause;
static bool                         s_initialised;

/* GW only — restart/OTA flags */
#ifdef BOARD_GATEWAY
static bool s_restart_pending;
static bool s_pending_self_check;
static bool s_pending_rollback;
#endif

/* Event queue (static) */
static StaticQueue_t s_queue_struct;
static uint8_t       s_queue_storage[4 * sizeof(lifecycle_event_t)];
static QueueHandle_t s_event_queue;

/* EditingConfig timeout timer (5 minutes per REQ-NF-214) */
static StaticTimer_t s_edit_timer_struct;
static TimerHandle_t s_edit_timer;

/* Restart confirmation timer (GW only, LC-O4 — provisional 30 s) */
#ifdef BOARD_GATEWAY
static StaticTimer_t s_restart_timer_struct;
static TimerHandle_t s_restart_timer;
#endif

/* Start-gate event group (other tasks block until Operational) */
static StaticEventGroup_t s_start_gate_struct;
static EventGroupHandle_t s_start_gate;

/* Config snapshot buffer (EditingConfig) */
static uint8_t s_cfg_snapshot[CONFIG_STORE_MAX_DATA_BYTES];
static uint32_t s_cfg_snapshot_len;
```

`CONFIG_STORE_MAX_DATA_BYTES` is at most 32 712 bytes per the
ConfigStore companion. This makes the snapshot buffer the dominant
RAM cost (LC-O1).

---

## 7. Init sub-state sequences

### 7.1 Field Device — Machine 5 Init

Five sequential sub-states. Any failure transitions immediately to
Faulted. The display sub-state (BringingUpLCD) is FD-specific — LCD
is architecturally essential per REQ-LD-000.

```
sub-state 1 — CheckingIntegrity
  s_config_store->check_integrity()
  → fail: log + post LC_EVENT_UNRECOVERABLE_FAULT → Faulted

sub-state 2 — LoadingConfig
  s_config_store->load(buf, &len, sizeof(buf))
  s_cfg_write->apply_loaded(buf, len)            /* or apply defaults */
  → fail: log + Faulted

sub-state 3 — BringingUpSensors
  s_sensors->init()
  s_alarms->init()
  → fail: log + Faulted

sub-state 4 — BringingUpLCD                      [FD only]
  s_graphics->init()
  s_lcd_ui->init()
  s_lcd_ui->show_splash()                        /* REQ-LD-200..220 */
  → fail: log + Faulted
  (Splash visible through sub-state 5 — REQ-LD-230)

sub-state 5 — StartingMiddleware
  s_modbus_slave->set_address(cfg.modbus_address)
  s_console->init_finalise()                     /* prompt, etc.    */
  s_health_report->init()
  → enter Operational
  s_lcd_ui->dismiss_splash()                     /* REQ-LD-240      */
  xEventGroupSetBits(s_start_gate, LIFECYCLE_START_GATE_BIT)
```

The order matters: BringingUpLCD is fourth (not last) because the
splash must be visible while StartingMiddleware completes — the user
sees boot progress through the splash progress bar (REQ-LD-210) during
sub-state 5.

### 7.2 Gateway — Machine 1 Init

Five sequential sub-states. SelfChecking replaces BringingUpLCD —
the GW has no display, but has a post-boot functional self-check
(REQ-DM-040) and handles post-update boot paths (SD-00c).

```
sub-state 1 — CheckingIntegrity
  s_config_store->check_integrity()
  s_firmware_store->get_pending_flags(&self_check, &rollback)
  s_pending_self_check = self_check
  s_pending_rollback   = rollback
  → cfg fail: Faulted

sub-state 2 — LoadingConfig
  s_config_store->load(buf, &len, sizeof(buf))
  s_cfg_write->apply_loaded(buf, len)
  → fail: Faulted

sub-state 3 — BringingUpSensors
  s_sensors->init()
  s_alarms->init()
  → fail: Faulted

sub-state 4 — StartingMiddleware
  s_modbus_poller->init()
  s_cloud->init()
  s_time_service->init()
  s_update_service->init()
  s_console->init_finalise()
  s_health_report->init()
  → fail on any: Faulted

sub-state 5 — SelfChecking                       [REQ-DM-040]
  if s_pending_self_check:
      s_update_service->resume_self_checking()
      /* wait on event */
      LC_EVENT_SELF_CHECK_PASS:
          s_firmware_store->confirm_self_check()
          → Operational
      LC_EVENT_SELF_CHECK_FAIL:
          s_update_service->resume_rollback()
          s_firmware_store->rollback()
          s_reset_driver->soft_reset()           /* will not return */

  else if s_pending_rollback:
      s_update_service->resume_after_rollback()
      s_cloud->report_rollback_result()
      → Operational

  else:
      sensor_ok = s_sensors->is_ready()
      modbus_ok = s_modbus_poller->is_ready()
      cloud_ok  = s_cloud->is_ready()
      if all ok → Operational
      else      → Faulted

  on Operational entry:
      xEventGroupSetBits(s_start_gate, LIFECYCLE_START_GATE_BIT)
```

### 7.3 Init time budget

A FreeRTOS software timer is created at init with a period of
**REQ-NF-213 TBD** (LC-O2). On expiry it posts
`LC_EVENT_UNRECOVERABLE_FAULT` with `param = LC_FAULT_INIT_TIMEOUT`.
The timer is started at the first sub-state and stopped on Operational
entry. Provisional value: **10 seconds** — confirm at coding time.

---

## 8. Operational — event handling

In Operational, `LifecycleTask` blocks on the event queue with a
timeout equal to the watchdog kick period (provisional 1 second). On
timeout with no events, it kicks the watchdog (if enabled) and loops.

| Event | Action | New state |
|---|---|---|
| `LC_EVENT_CONFIG_EDIT_ENTER` | `s_cfg_write->snapshot(s_cfg_snapshot, &s_cfg_snapshot_len, sizeof s_cfg_snapshot)`; start `s_edit_timer` | EditingConfig |
| `LC_EVENT_RESTART_REQUESTED` (GW) | `s_restart_pending = true`; `s_cloud->flush()`; start `s_restart_timer` (LC-O4) | Restarting |
| `LC_EVENT_OTA_REQUESTED` (GW) | If already UpdatingFirmware: reject (REQ-DM-054). Otherwise: `s_update_service->start()` | UpdatingFirmware |
| `LC_EVENT_UNRECOVERABLE_FAULT` | Log `event.param`; push `HEALTH_EVENT_FAULT` via `s_health_report->push_fault()` | Faulted |
| Other events | Log and ignore | Operational |

---

## 9. EditingConfig

**Entry action**: snapshot current config via
`s_cfg_write->snapshot(s_cfg_snapshot, &s_cfg_snapshot_len, sizeof s_cfg_snapshot)`.
Start `s_edit_timer` (5 minutes per REQ-NF-214).

**Exit actions** (and corresponding events):

| Trigger | Action | New state |
|---|---|---|
| `LC_EVENT_CONFIG_EDIT_APPLY` | `s_cfg_write->commit()`; notify affected components (sensor threshold change → `s_sensors->reconfigure()`; Modbus address change → `s_modbus_slave->set_address()`) | Operational |
| `LC_EVENT_CONFIG_EDIT_CANCEL` | `s_cfg_write->restore_snapshot(s_cfg_snapshot, s_cfg_snapshot_len)` | Operational |
| `LC_EVENT_CONFIG_EDIT_TIMEOUT` | Same as CANCEL | Operational |

The 5-minute timer is created at `lifecycle_controller_init()` and
started on each EditingConfig entry. On expiry it posts
`LC_EVENT_CONFIG_EDIT_TIMEOUT` to the queue.

While in EditingConfig, all other events are queued but not acted
upon (they are processed on return to Operational). The exception is
`LC_EVENT_UNRECOVERABLE_FAULT` — always honoured immediately.

---

## 10. Restarting (GW only — UC-17, REQ-DM-010..030)

Two-step confirmation guard defined in the Modbus register map
(CMD_SOFT_RESTART at address 0x0202, confirmation token 0xA5A5).

```
Operational + LC_EVENT_RESTART_REQUESTED:
  s_restart_pending = true
  log; s_cloud->flush()
  start s_restart_timer  (LC-O4 — provisional 30 s)
  → Restarting

Restarting + LC_EVENT_RESTART_CONFIRMED:
  stop s_restart_timer
  log
  s_reset_driver->soft_reset()                   /* will not return */

Restarting + LC_EVENT_RESTART_TIMEOUT:
  s_restart_pending = false
  log
  → Operational

Restarting + LC_EVENT_RESTART_REQUESTED (second time):
  Treat as confirmation — same as LC_EVENT_RESTART_CONFIRMED above.
  Handles the case where the Modbus master issues the command twice
  using the same register write.

Restarting + LC_EVENT_UNRECOVERABLE_FAULT:
  → Faulted (always honoured)
```

---

## 11. UpdatingFirmware (GW only — UC-18, REQ-DM-054)

Entered when `LC_EVENT_OTA_REQUESTED` arrives in Operational and no
update is already in progress (REQ-DM-054 lock).

```
Operational + LC_EVENT_OTA_REQUESTED:
  if s_state == LIFECYCLE_STATE_UPDATING_FW: reject (return BAD_STATE)
  s_update_service->start(event.param)          /* event.param = image size */
  → UpdatingFirmware

UpdatingFirmware + LC_EVENT_SELF_CHECK_PASS:
  s_firmware_store->confirm_self_check()
  → Operational

UpdatingFirmware + LC_EVENT_SELF_CHECK_FAIL:
  s_update_service->resume_rollback()
  s_firmware_store->rollback()
  s_reset_driver->soft_reset()

UpdatingFirmware + LC_EVENT_UNRECOVERABLE_FAULT:
  → Faulted (always honoured)
```

Most of the UpdatingFirmware workflow (image receive, signature
verify, flash write) happens inside `UpdateService` — LifecycleController
only orchestrates the state transitions and the post-update reboot.

---

## 12. Faulted

**Entry action**: log fault code; push `HEALTH_EVENT_FAULT` via
`s_health_report->push_fault()`; set LED to fault pattern (via the
health monitor's LED control, which observes the fault via its own
data path — LifecycleController does not call LED directly).

**Do activity**: continue kicking the watchdog (if enabled). No
watchdog reset on top of an application fault — the fault LED must
remain visible to the user.

**Exit**: hardware reset only. No `LC_EVENT_*` causes Faulted exit.

---

## 13. Reset cause detection

Detected once at startup, **before** `lifecycle_controller_init()`, by a
static helper called from early boot:

```c
static lifecycle_reset_cause_t detect_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;  /* clear flags for next boot */

    if (csr & RCC_CSR_IWDGRSTF) { return LIFECYCLE_RESET_WATCHDOG; }
    if (csr & RCC_CSR_SFTRSTF)  { return LIFECYCLE_RESET_SOFT;     }
    if (csr & RCC_CSR_PINRSTF)  { return LIFECYCLE_RESET_POWER_ON; }
    return LIFECYCLE_RESET_UNKNOWN;
}
```

The detected value is passed into `lifecycle_controller_init()` and
stored in `s_reset_cause`. `get_reset_cause()` returns it.

On a soft reset following OTA (SD-00c), `RCC_CSR_SFTRSTF` is set,
which gives `LIFECYCLE_RESET_SOFT`. The pending-flag check in
CheckingIntegrity (`firmware_store->get_pending_flags`) distinguishes
the OTA-reboot case from a plain soft reset.

---

## 14. Remote command dispatch

LifecycleController is the **single dispatch point** for all remote
commands that affect system state (LLD-D12, LLD-D15).

```
handle_remote_command(cmd):
    if not s_initialised: return LIFECYCLE_ERR_NOT_INIT
    switch cmd:
        case LC_REMOTE_CMD_RESET_METRICS:
            s_health_admin->reset_metrics()     /* GW only */
            return LIFECYCLE_ERR_OK
        case LC_REMOTE_CMD_SOFT_RESTART:
            return post_event({type=LC_EVENT_RESTART_REQUESTED, param=0})
                   ? LIFECYCLE_ERR_OK : LIFECYCLE_ERR_QUEUE_FULL
        default:
            log unknown command
            return LIFECYCLE_ERR_UNKNOWN_CMD
```

**`RESET_METRICS` is direct dispatch** (not queued) — it has no
lifecycle-state side-effect and must not block on queue availability.
Called from `ModbusSlaveTask` / `ModbusPollerTask` context.

**`SOFT_RESTART` is queued** — it must coordinate with the two-step
confirmation logic in the state machine, which lives in `LifecycleTask`.

`CMD_ACK_ALARM` is **not** routed through LifecycleController. It
dispatches directly from `ModbusRegisterMap` to `s_alarms->ack_all()`
because alarm acknowledgement has no lifecycle-state dependency
(LLD-D14).

---

## 15. Sequence integration

| SD | Component role | Key function |
|---|---|---|
| SD-00a | Orchestrates FD boot: drives Machine 5 Init sub-states 1..5, calls subsystem `init()` operations, transitions to Operational or Faulted | `lifecycle_task_body()` |
| SD-00b | Orchestrates GW boot (normal path): drives Machine 1 Init sub-states 1..5, including SelfChecking probes | `lifecycle_task_body()` |
| SD-00c | Orchestrates GW boot (post-OTA path): branches on `pending_self_check` / `pending_rollback` flags in SelfChecking | `lifecycle_task_body()` |
| SD-06 | Receives OTA start command; transitions to UpdatingFirmware; calls `s_update_service->start()`; on completion transitions back to Operational or Faulted | `lifecycle_task_body()`, `handle_remote_command()` |
| SD-08 | Receives remote-restart command; transitions to Restarting; calls `s_cloud->flush()` then `s_reset_driver->soft_reset()` | `handle_remote_command()`, `lifecycle_task_body()` |
| SD-09 | EditingConfig: snapshot, edit, commit/restore | `lifecycle_task_body()` |

---

## 16. Thread safety

`s_state` is declared `volatile` so `get_state()` reads a coherent
value without a mutex. **All writes to `s_state` happen inside
`LifecycleTask`** — no concurrent write path exists.

`s_reset_cause` and `s_initialised` are written once at init, read-only
afterwards. No synchronisation needed.

`s_event_queue` and `s_start_gate` are FreeRTOS primitives — thread
safety guaranteed by the kernel.

`s_restart_pending`, `s_pending_self_check`, `s_pending_rollback`,
`s_cfg_snapshot`, `s_cfg_snapshot_len` are accessed only from
`LifecycleTask`. No mutex needed.

Provider calls (`s_sensors->init()`, `s_cfg_write->commit()`, etc.)
are individually thread-safe inside each provider per their respective
companions. `LifecycleTask` does not hold locks while issuing them.

**No internal mutex is created.** Earlier draft text claiming an
"internal mutex" was incorrect.

---

## 17. Initialisation order

```
main():
    SystemInit (CMSIS)
    SystemClock_Config / system_clock_init
    reset_cause = detect_reset_cause()  /* before any other init */

    /* Driver layer */
    gpio_init, debug_uart_init, rtc_init, i2c_init, ...

    /* Middleware */
    logger_init, time_provider_init, modbus_slave_init (FD) ...

    /* Application services — pointer table population only */
    config_store_init, config_service_init, sensor_service_init,
    alarm_service_init, health_monitor_init, console_service_init

    lifecycle_controller_init(reset_cause, /* all injected interfaces */)

    /* Task creation (static) */
    xTaskCreateStatic(sensor_task_body, ...)
    xTaskCreateStatic(alarm_task_body, ...)
    xTaskCreateStatic(modbus_task_body, ...)
    xTaskCreateStatic(console_task_body, ...)
    xTaskCreateStatic(health_monitor_task_body, ...)
#ifdef BOARD_FIELD_DEVICE
    xTaskCreateStatic(lcd_ui_task_body, ...)
#endif
    xTaskCreateStatic(lifecycle_task_body, ...)  /* LAST */

    vTaskStartScheduler()
```

`LifecycleTask` is the **last task created**. All other tasks block on
the start-gate event group bit set by `LifecycleController` on
Operational entry — this prevents any application task from racing
ahead of the boot sequence.

---

## 18. Memory and sizing

| Item | Size |
|---|---|
| Injected interface pointers (FD: ×12, GW: ×14) | 48 B / 56 B |
| Machine state (s_state, s_reset_cause, s_initialised, GW flags) | 12 B |
| Event queue storage (4 × 8 B) | 32 B |
| `StaticQueue_t` | ~80 B |
| `StaticTimer_t` × 2 (edit + restart, GW only ×2) | ~80 B (FD) / ~160 B (GW) |
| `StaticEventGroup_t` | ~16 B |
| `s_cfg_snapshot` | up to 32 712 B (LC-O1) |
| `s_cfg_snapshot_len` | 4 B |
| **Total RAM (excl. cfg_snapshot)** | ~270 B |
| **Total RAM (incl. cfg_snapshot)** | up to **~33 KB** |
| Task stack (FreeRTOS, static) | 8192 B |

Recommendation per LC-O1: place `s_cfg_snapshot` in a dedicated BSS
section so the budget is explicit in the linker map and a regression
(e.g. another static buffer crowding the same region) is visible.

No dynamic allocation post-init (P5).

---

## 19. Error and fault behaviour

All public functions return `lifecycle_err_t`; callers must not ignore
non-OK returns. LifecycleController is the system root — errors in its
API indicate programming errors rather than runtime recoverable
conditions, with two exceptions: `QUEUE_FULL` (caller may retry or
log+discard) and `UNKNOWN_CMD` (caller has a protocol bug).

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `LIFECYCLE_ERR_NULL_ARG` | Null pointer to init | Return error; no state change | Non-OK return | No retry — programming error | Logged at ERROR via ILogger |
| `LIFECYCLE_ERR_NOT_INIT` | Function called before init | Return error; no action | Non-OK return | No retry — programming error | Logged at ERROR via ILogger |
| `LIFECYCLE_ERR_QUEUE_FULL` | Event queue at capacity (depth 4) | Return error; event dropped | Non-OK return | Caller may retry after delay | Logged at WARN via ILogger; queue full is a system-overload indicator |
| `LIFECYCLE_ERR_UNKNOWN_CMD` | `handle_remote_command` received an unrecognised value | Return error; no action | Non-OK return | No retry — caller protocol bug | Logged at WARN via ILogger |
| `LIFECYCLE_ERR_BAD_STATE` | OTA requested while already UpdatingFirmware (REQ-DM-054) | Return error; reject second OTA | Non-OK return | Caller waits for completion | Logged at WARN via ILogger |

---

## 20. Principles applied

- **P1 (Strict directional layering).** Depends on driver, middleware,
  and application interfaces injected at init. No component exists
  above the application layer, so no upward dependency arises.
- **P2 (Dependency Inversion).** Exposes `ilifecycle_t` vtable;
  consumes every external dependency via injected vtable pointers;
  `IHealthAdmin` (owned by HealthMonitor) is injected per LLD-D15
  to enable the metric-reset command dispatch path without coupling
  to the HealthMonitor implementation.
- **P4 (Cross-cutting concern exception).** `Logger` and
  `TimeProvider` (where used) are referenced concretely per the
  cross-cutting exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** Event
  queue, timers, event group, and config snapshot buffer are all
  static. No heap.
- **P6 (Responsibility traces to requirements).** Every state, sub-state,
  event, and remote command in this document traces to a specific
  `REQ-SA-*`, `REQ-DM-*`, `REQ-LD-*`, or `REQ-NF-*` requirement
  — see §2.
- **P8 (Total error propagation, no silent failures).** `lifecycle_err_t`
  on all state-modifying functions; `handle_remote_command` returns
  an error for unrecognised commands; init returns an error on any
  null pointer; state-machine errors reported via `IHealthReport`.
- **P9 (BARR-C coding standard).** State values `uint8_t` enum; event
  flags `uint32_t` (FreeRTOS `EventBits_t`); no floating-point;
  designated initialisers throughout.
- **P10 (Naming conventions).** Prefix `lifecycle_`; interface
  `ILifecycle` → `ilifecycle_t`; errors `LIFECYCLE_ERR_*`; states
  `LIFECYCLE_STATE_*`; events `LC_EVENT_*`; remote commands
  `LC_REMOTE_CMD_*`.

P3 (Interface Segregation) does not apply — `LifecycleController`
exposes a small vtable used uniformly by `HealthMonitor`,
`ConsoleService`, and `CloudPublisher` (read-only state queries) plus
`ModbusRegisterMap` (write commands). No class of consumers requires
only a strict subset that would justify splitting.

P7 (Pull-based access) does not apply — `LifecycleController` is
event-driven (queue receive). Reads from injected providers happen
only during Init sub-states (one-shot, not steady-state).

---

## 21. Unit test plan (Pass H)

File: `tests/field-device/application/lifecycle_controller/test_lifecycle_controller_fd.c`
and `tests/gateway/application/lifecycle_controller/test_lifecycle_controller_gw.c`.

All injected interfaces use CMock-generated mocks. The state machine
is driven by calling `lifecycle_task_body()` in single-step mode —
`lifecycle_task_body()` checks for a test hook (`#ifdef TEST`) that
allows one event to be processed per call instead of looping forever.

Test isolation: `lifecycle_controller_reset_for_test()` (TEST-only)
zeroes all file-scope statics and recreates the queue, timers, and
event group between cases.

### 21.1 Init — NULL-argument rejection

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-001 | `lifecycle_controller_init(NULL, …)` (any param) → `LIFECYCLE_ERR_NULL_ARG` | P8 |
| TC-LC-002 | FD build accepts `cloud == NULL` (compile-time, not param) | P2 |
| TC-LC-003 | GW build requires all GW-specific pointers | P2 |
| TC-LC-004 | Post-init: `s_initialised == true`, `s_state == LIFECYCLE_STATE_INIT` | — |
| TC-LC-005 | Post-init: `s_reset_cause` equals the value passed in | REQ-NF-203 |
| TC-LC-006 | Post-init: event queue, timers, event group all created | — |
| TC-LC-007 | Vtable call before init → `LIFECYCLE_ERR_NOT_INIT` | P8 |

### 21.2 Reset cause detection

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-010 | `detect_reset_cause` with `RCC_CSR_IWDGRSTF` set → `LIFECYCLE_RESET_WATCHDOG` | REQ-NF-203 |
| TC-LC-011 | `detect_reset_cause` with `RCC_CSR_SFTRSTF` set → `LIFECYCLE_RESET_SOFT` | REQ-NF-203 |
| TC-LC-012 | `detect_reset_cause` with `RCC_CSR_PINRSTF` set → `LIFECYCLE_RESET_POWER_ON` | REQ-NF-203 |
| TC-LC-013 | `detect_reset_cause` with no flag set → `LIFECYCLE_RESET_UNKNOWN` | REQ-NF-203 |
| TC-LC-014 | `detect_reset_cause` clears `RCC_CSR_RMVF` after read | — |
| TC-LC-015 | Both watchdog and soft flags set → watchdog wins (highest-severity-first) | REQ-NF-203 |

### 21.3 FD Init sub-state sequence (file: `test_lifecycle_controller_fd.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-030 | All sub-states succeed → final state `OPERATIONAL` | REQ-SA-050 |
| TC-LC-031 | CheckingIntegrity: `check_integrity` fails → `FAULTED`; subsequent sub-states NOT entered | REQ-SA-040 |
| TC-LC-032 | LoadingConfig: `load` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-033 | LoadingConfig: `apply_loaded` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-034 | BringingUpSensors: `sensors->init` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-035 | BringingUpSensors: `alarms->init` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-036 | BringingUpLCD: `graphics->init` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-037 | BringingUpLCD: `lcd_ui->init` returns error → `FAULTED` | REQ-SA-040 |
| TC-LC-038 | BringingUpLCD: `show_splash` called exactly once | REQ-LD-200 |
| TC-LC-039 | StartingMiddleware: `modbus_slave->set_address` called with `cfg.modbus_address` | REQ-LD-220 |
| TC-LC-040 | Operational entry: `dismiss_splash` called exactly once | REQ-LD-240 |
| TC-LC-041 | Operational entry: start-gate bit set in event group | — |
| TC-LC-042 | Init timeout fires before completion → `FAULTED` (LC-O2) | REQ-NF-213 |

### 21.4 GW Init sub-state sequence (file: `test_lifecycle_controller_gw.c`)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-050 | All sub-states succeed (normal boot) → `OPERATIONAL` | REQ-SA-050 |
| TC-LC-051 | CheckingIntegrity records `pending_self_check` from `firmware_store->get_pending_flags` | REQ-DM-071 |
| TC-LC-052 | CheckingIntegrity records `pending_rollback` from `firmware_store->get_pending_flags` | REQ-DM-072 |
| TC-LC-053 | SelfChecking normal path: all probes pass → `OPERATIONAL` | REQ-DM-040 |
| TC-LC-054 | SelfChecking: `sensors->is_ready` returns false → `FAULTED` | REQ-DM-040 |
| TC-LC-055 | SelfChecking: `modbus_poller->is_ready` returns false → `FAULTED` | REQ-DM-040 |
| TC-LC-056 | SelfChecking: `cloud->is_ready` returns false → `FAULTED` | REQ-DM-040 |
| TC-LC-057 | SelfChecking: `pending_self_check=true` → `update_service->resume_self_checking` called | REQ-DM-071 |
| TC-LC-058 | After `LC_EVENT_SELF_CHECK_PASS` → `firmware_store->confirm_self_check`, then `OPERATIONAL` | REQ-DM-071 |
| TC-LC-059 | After `LC_EVENT_SELF_CHECK_FAIL` → `update_service->resume_rollback`, `firmware_store->rollback`, `reset_driver->soft_reset` | REQ-DM-072 |
| TC-LC-060 | SelfChecking: `pending_rollback=true` → `update_service->resume_after_rollback` called | REQ-DM-072 |
| TC-LC-061 | SelfChecking: pending_rollback path → `cloud->report_rollback_result` called, then `OPERATIONAL` | REQ-DM-072 |
| TC-LC-062 | SelfChecking: both pending flags false → normal probe path executed | REQ-DM-040 |

### 21.5 EditingConfig

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-070 | Operational + `CONFIG_EDIT_ENTER` → `EDITING_CONFIG`, `cfg_write->snapshot` called once, edit timer started | REQ-SA-060, REQ-DM-040 |
| TC-LC-071 | EditingConfig + `CONFIG_EDIT_APPLY` → `cfg_write->commit` called, `OPERATIONAL` | REQ-SA-060 |
| TC-LC-072 | EditingConfig + `CONFIG_EDIT_CANCEL` → `cfg_write->restore_snapshot(s_cfg_snapshot, s_cfg_snapshot_len)`, `OPERATIONAL` | REQ-SA-060 |
| TC-LC-073 | EditingConfig + `CONFIG_EDIT_TIMEOUT` → same as CANCEL | REQ-NF-214 |
| TC-LC-074 | Edit timer fires at 5 minutes → `CONFIG_EDIT_TIMEOUT` posted | REQ-NF-214 |
| TC-LC-075 | EditingConfig + Modbus address changed in cfg → `modbus_slave->set_address` called on APPLY | REQ-SA-060 |
| TC-LC-076 | EditingConfig + sensor threshold changed → `sensors->reconfigure` called on APPLY | REQ-SA-060 |
| TC-LC-077 | EditingConfig + `UNRECOVERABLE_FAULT` → `FAULTED` (event honoured immediately) | REQ-SA-040 |
| TC-LC-078 | Non-Operational state + `CONFIG_EDIT_ENTER` → ignored, state unchanged | REQ-SA-060 |

### 21.6 Restarting (GW only)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-085 | Operational + `RESTART_REQUESTED` → `RESTARTING`, `cloud->flush` called, restart timer started, `restart_pending=true` | REQ-DM-010 |
| TC-LC-086 | Restarting + `RESTART_CONFIRMED` → `reset_driver->soft_reset` called | REQ-DM-020 |
| TC-LC-087 | Restarting + `RESTART_TIMEOUT` → `restart_pending=false`, `OPERATIONAL` | REQ-DM-030, LC-O4 |
| TC-LC-088 | Restart timer fires at provisional 30 s → `RESTART_TIMEOUT` posted | LC-O4 |
| TC-LC-089 | Restarting + second `RESTART_REQUESTED` → treated as confirmation, `soft_reset` called | REQ-DM-020 |
| TC-LC-090 | Restarting + `UNRECOVERABLE_FAULT` → `FAULTED` (event honoured) | REQ-SA-040 |
| TC-LC-091 | FD build excludes Restarting state entirely (compile-time check) | — |

### 21.7 UpdatingFirmware (GW only)

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-095 | Operational + `OTA_REQUESTED` → `UPDATING_FW`, `update_service->start(event.param)` called | REQ-DM-054 |
| TC-LC-096 | UpdatingFirmware + `OTA_REQUESTED` → rejected, `LIFECYCLE_ERR_BAD_STATE`, no second `start` call | REQ-DM-054 |
| TC-LC-097 | UpdatingFirmware + `SELF_CHECK_PASS` → `firmware_store->confirm_self_check`, `OPERATIONAL` | REQ-DM-071 |
| TC-LC-098 | UpdatingFirmware + `SELF_CHECK_FAIL` → `update_service->resume_rollback`, `firmware_store->rollback`, `reset_driver->soft_reset` | REQ-DM-072 |
| TC-LC-099 | UpdatingFirmware + `UNRECOVERABLE_FAULT` → `FAULTED` (event honoured) | REQ-SA-040 |

### 21.8 Faulted

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-105 | Any state + `UNRECOVERABLE_FAULT` → `FAULTED`; `health_report->push_fault(event.param)` called | REQ-NF-202 |
| TC-LC-106 | Faulted entry happens exactly once (idempotent on repeat fault events) | REQ-NF-202 |
| TC-LC-107 | Faulted state: no event causes Faulted exit | REQ-SA-040 |
| TC-LC-108 | Faulted state: watchdog kicked on idle loop (no app fault on top of fault) | — |

### 21.9 Vtable dispatch

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-115 | `lifecycle_controller->get_state()` returns current `s_state` | — |
| TC-LC-116 | `lifecycle_controller->get_reset_cause()` returns `s_reset_cause` | REQ-NF-203 |
| TC-LC-117 | `lifecycle_controller->post_event(e)` enqueues to event queue | — |
| TC-LC-118 | `post_event` returns `false` on full queue | — |
| TC-LC-119 | Queue depth is exactly 4 | — |
| TC-LC-120 | Events processed FIFO | — |

### 21.10 Remote command dispatch

| TC | Behaviour under test | Traces to |
|---|---|---|
| TC-LC-130 | `handle_remote_command(LC_REMOTE_CMD_RESET_METRICS)` → `health_admin->reset_metrics` called once; `LIFECYCLE_ERR_OK` returned | REQ-DM-040 |
| TC-LC-131 | `RESET_METRICS` does not change `s_state` | — |
| TC-LC-132 | `RESET_METRICS` does not post any event | — |
| TC-LC-133 | `handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART)` → `LC_EVENT_RESTART_REQUESTED` posted; `LIFECYCLE_ERR_OK` returned | REQ-DM-010 |
| TC-LC-134 | `SOFT_RESTART` with full queue → `LIFECYCLE_ERR_QUEUE_FULL` returned | — |
| TC-LC-135 | `handle_remote_command(<unknown>)` → `LIFECYCLE_ERR_UNKNOWN_CMD` | P8 |
| TC-LC-136 | `handle_remote_command` before init → `LIFECYCLE_ERR_NOT_INIT` | P8 |
| TC-LC-137 | (FD) `RESET_METRICS` is also routed via LifecycleController (uniform dispatch) | LLD-D15 |

---

## 22. Integration test plan

On-target tests. Executed manually with debug UART and (for GW) a
Modbus master tool + WiFi/MQTT broker.

| TC | Setup | Pass criterion |
|---|---|---|
| IT-LC-001 (FD) | Cold boot from POR | UART log shows all 5 sub-states completed; state reaches Operational; splash dismissed |
| IT-LC-002 (GW) | Cold boot from POR | UART log shows all 5 sub-states; state reaches Operational; `reset_cause == POWER_ON` reflected in health |
| IT-LC-003 | Corrupt config in flash; cold boot | State enters Faulted; LED shows fault pattern; UART shows fault code |
| IT-LC-004 | Sensor I²C bus pulled low; cold boot | State enters Faulted at BringingUpSensors |
| IT-LC-005 | Operational → Console `config commit` → reboot | Snapshot applied, persists across reboot |
| IT-LC-006 | Operational → Console `config discard` | Snapshot rolled back, original values restored |
| IT-LC-007 | EditingConfig entry, no input for 5 minutes | Auto-cancel; rollback to original; state Operational |
| IT-LC-008 (GW) | Modbus master writes CMD_RESET_METRICS (0x0201) | `health_admin->reset_metrics` called; counters zero on next read |
| IT-LC-009 (GW) | Modbus master writes CMD_SOFT_RESTART (0x0202) + confirmation token within 30 s | Board reboots; `reset_cause == SOFT` on next boot |
| IT-LC-010 (GW) | Modbus master writes CMD_SOFT_RESTART, no confirmation in 30 s | State returns to Operational; no reset |
| IT-LC-011 (GW) | Cloud OTA: image push, signature OK | UpdatingFirmware entered; image written; soft reset; pending_self_check on next boot; SelfChecking passes; Operational |
| IT-LC-012 (GW) | Cloud OTA: image push, intentionally broken (signature fail in UpdateService) | Fault during OTA; state Faulted; no soft reset |
| IT-LC-013 (GW) | Cloud OTA passes signature; SelfChecking-after-boot fails (sensor unresponsive) | Rollback path executes; previous firmware boots; `cloud->report_rollback_result` published |
| IT-LC-014 | Operational, then watchdog hardware reset | `reset_cause == WATCHDOG` reflected in health snapshot |

---

## 23. Board differences summary

| Aspect | FD | GW |
|---|---|---|
| Init sub-state 4 | BringingUpLCD | (continues StartingMiddleware contents) |
| Init sub-state 5 | StartingMiddleware | SelfChecking (REQ-DM-040) |
| Post-update boot path | — | `pending_self_check` / `pending_rollback` branches |
| Restarting state | — | Yes (UC-17, REQ-DM-010..030) |
| UpdatingFirmware state | — | Yes (UC-18, REQ-DM-054) |
| Splash screen | Yes (REQ-LD-200..240) | — |
| ResetDriver consumed | — | Yes (Restarting and post-OTA rollback) |
| `s_cfg_snapshot` buffer | Same | Same |

Code is split per board: `lifecycle_fd.c` / `lifecycle_gw.c`. Shared
declarations live in `lifecycle_types.h`. Shared logic
(event queue post, reset-cause detection, EditingConfig timeout)
lives in `lifecycle_common.c`.

---

## 24. Open items

Earlier IDs (LC-O1..LC-O0) assigned in passes A–G.

| ID | Item | Resolution path | Status |
|---|---|---|---|
| **LC-O1** | `s_cfg_snapshot` buffer is up to 32 712 B — dominates the static RAM cost. Confirm it fits in BSS without pushing other static data out of budget; recommend a dedicated BSS section so the linker map shows it explicitly. | Verify BSS budget at integration; place in dedicated section if needed | Open |
| **LC-O2** | Init timeout budget (REQ-NF-213, value TBD) — provisional 10 s. A FreeRTOS software timer fires `LC_EVENT_UNRECOVERABLE_FAULT` with `param = LC_FAULT_INIT_TIMEOUT` if total Init exceeds budget. | Implement at coding time once SRS TBD resolved | Open |
| **LC-O3** | Start-gate event group bit allocation — needs a project-wide event-group bit map (similar to RTC backup register map). Provisional bit 0. | Allocate bit from project-wide map when produced | Open |
| **LC-O4** | Restart confirmation timeout (GW, REQ-DM-020) — value TBD in SRS. Provisional 30 s. Implemented as a FreeRTOS software timer started on `LC_EVENT_RESTART_REQUESTED`; on expiry posts `LC_EVENT_RESTART_TIMEOUT`. | Implement timer at coding time once SRS TBD resolved | Open |
| **LC-O5** | Test hook design: `lifecycle_task_body` needs a `#ifdef TEST` single-step mode (process one event and return) so unit tests can drive the state machine deterministically. Define the exact hook shape at coding time. | Define and implement at coding time | Open |

---

## 25. References

- `docs/components.md` (FD + GW LifecycleController entries).
- `docs/state-machines.md` Machine 1 (GW), Machine 5 (FD).
- `docs/hld.md` §7.1, §7.2, §7.6.
- `docs/sequence-diagrams.md` SD-00a, SD-00b, SD-00c, SD-06, SD-08, SD-09.
- `docs/task-breakdown.md` §4.2, §4.4, §5.2, §5.4.
- `docs/lld/config-service.md` (`IConfigProvider`, `IConfigManager`).
- `docs/lld/config-store.md` (`IConfigStore`, `CONFIG_STORE_MAX_DATA_BYTES`).
- `docs/lld/health-monitor.md` (`IHealthReport`, `IHealthAdmin`).
- `docs/lld/console-service.md` (`IConsoleService`).
- `docs/architecture-principles.md` P1, P2, P4, P5, P6, P8, P9, P10.

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD Pass H.*
