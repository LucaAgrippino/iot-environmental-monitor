# LLD Companion — LifecycleController

**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `ILifecycle`  
**Consumes (FD):** `IConfigStore`, `IConfigService`, `ISensorService`, `IGraphics`, `ILogger`  
**Consumes (GW):** `IConfigStore`, `IConfigService`, `ISensorService`, `ICloudPublisher`, `IModbusPoller`, `IUpdateService`, `ITimeService`, `IFirmwareStore`, `IResetDriver`, `ILogger`  
**SRS traces (both):** REQ-SA-000–060, REQ-DM-040, REQ-NF-202, REQ-NF-203, REQ-NF-213, REQ-NF-214  
**SRS traces (FD add.):** REQ-LD-200, REQ-LD-210, REQ-LD-220, REQ-LD-230, REQ-LD-240  
**SRS traces (GW add.):** REQ-DM-010, REQ-DM-020, REQ-DM-030, REQ-DM-071, REQ-DM-072  
**HLD ref:** `state-machines.md` Machine 1 (GW), Machine 5 (FD); `hld.md` §7.1, §7.2, §7.6; `sequence-diagrams.md` SD-00a–c; `task-breakdown.md` §4.2, §5.2
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** LifecycleController in `components.md` (FD + GW application layer)

---

## 1. Sources

LifecycleController owns the top-level lifecycle state machine on each
board. Its two roles:

1. **Sequencer** — drives the Init sub-state chain at boot, gating
   Operational entry on each subsystem starting up cleanly.
2. **Event router** — reacts to runtime events (restart commands,
   OTA commands, config-edit enter/exit, faults) by transitioning the
   lifecycle state machine and coordinating the affected components.

LifecycleController does **not** own data acquisition, display content,
cloud publishing, or Modbus protocol. It activates and gates those
components but does not perform their work.

Runs exclusively in `LifecycleTask` (priority 1 — lowest application
task, no hard real-time constraint). Events arrive via a queue of depth
4 (task-breakdown.md §4.4, §5.4).

---

## 2. Data types

```c
/* lifecycle_controller.h */

typedef enum {
    LIFECYCLE_STATE_INIT           = 0,
    LIFECYCLE_STATE_OPERATIONAL    = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING     = 3,  /* GW only */
    LIFECYCLE_STATE_UPDATING_FW    = 4,  /* GW only */
    LIFECYCLE_STATE_FAULTED        = 5,
} lifecycle_state_t;

typedef enum {
    LIFECYCLE_RESET_POWER_ON  = 0,
    LIFECYCLE_RESET_SOFT      = 1,
    LIFECYCLE_RESET_WATCHDOG  = 2,
    LIFECYCLE_RESET_UNKNOWN   = 3,
} lifecycle_reset_cause_t;

typedef enum {
    LIFECYCLE_ERR_OK        = 0,
    LIFECYCLE_ERR_NULL_ARG  = 1,
    LIFECYCLE_ERR_NOT_INIT  = 2,
} lifecycle_err_t;

/* Events posted to LifecycleTask by other components */
typedef enum {
    LC_EVENT_CONFIG_EDIT_ENTER     = 0,
    LC_EVENT_CONFIG_EDIT_APPLY     = 1,
    LC_EVENT_CONFIG_EDIT_CANCEL    = 2,
    LC_EVENT_CONFIG_EDIT_TIMEOUT   = 3,
    LC_EVENT_RESTART_REQUESTED     = 4,  /* GW: first Modbus/MQTT command */
    LC_EVENT_RESTART_CONFIRMED     = 5,  /* GW: confirmation write received (DM-020) */
    LC_EVENT_OTA_REQUESTED         = 6,  /* GW */
    LC_EVENT_SELF_CHECK_PASS       = 7,  /* GW: UpdateService result */
    LC_EVENT_SELF_CHECK_FAIL       = 8,  /* GW: UpdateService result */
    LC_EVENT_UNRECOVERABLE_FAULT   = 9,  /* any component */
} lifecycle_event_type_t;

typedef struct {
    lifecycle_event_type_t type;
    uint32_t               param;   /* event-specific payload (e.g. fault code) */
} lifecycle_event_t;
```

---

## 2. Public API — `ILifecycle`

```c
/**
 * @brief  Return the current lifecycle state.
 *
 * Called by HealthMonitor, ConsoleService, and (on GW) CloudPublisher
 * to include lifecycle state in health payloads.
 * Thread-safe — reads a volatile state variable.
 */
lifecycle_state_t lifecycle_get_state(void);

/**
 * @brief  Return the reset cause detected at last boot.
 *
 * Determined from RCC reset-status flags and the watchdog RTC backup
 * register at startup. Exposed in health payloads (REQ-NF-202).
 */
lifecycle_reset_cause_t lifecycle_get_reset_cause(void);

/**
 * @brief  Post an event to LifecycleTask.
 *
 * Non-blocking. Returns false if the event queue is full (depth 4);
 * caller may log and discard. Safe from any task context.
 *
 * @param  event  Event to enqueue.
 * @return true if enqueued; false if queue full.
 */
bool lifecycle_post_event(lifecycle_event_t event);
```

`lifecycle_post_event()` is the only call other components make at
runtime. LifecycleController is otherwise entirely reactive.

---

## 4. Init sub-state sequences

### 4.1 Field Device — Machine 5 Init

Five sequential sub-states. Any failure transitions immediately to
Faulted. The display sub-state (BringingUpLCD) is FD-specific — LCD
is architecturally essential per REQ-LD-000.

```
sub-state 1 — CheckingIntegrity
  config_store_check_integrity()
  → fail: log + post LC_EVENT_UNRECOVERABLE_FAULT → Faulted

sub-state 2 — LoadingConfig
  config_store_load(&cfg_buf, &len, sizeof(cfg_buf))
  config_service_apply_loaded(cfg_buf, len)    /* or apply defaults */
  → fail: log + Faulted

sub-state 3 — BringingUpSensors
  sensor_service_init()
  alarm_service_init()
  → fail: log + Faulted

sub-state 4 — BringingUpLCD          [FD only]
  graphics_init()
  lcd_ui_init()
  lcd_ui_show_splash()                          /* REQ-LD-200, LD-210, LD-220 */
  → fail: log + Faulted
  (Splash remains visible through sub-state 5 — REQ-LD-230)

sub-state 5 — StartingMiddleware
  modbus_slave_set_address(cfg.modbus_address)
  console_service_init()
  health_monitor_init()
  → enter Operational
  lcd_ui_dismiss_splash()                       /* REQ-LD-240 — first Operational frame */
```

BringingUpLCD is the fourth sub-state, not the last, because the splash
screen must be visible while StartingMiddleware completes — the user sees
boot progress through the splash progress bar (REQ-LD-210) during sub-state 5.

### 4.2 Gateway — Machine 1 Init

Five sequential sub-states. SelfChecking replaces BringingUpLCD —
the GW has no display, but has a post-boot functional self-check
(REQ-DM-040).

```
sub-state 1 — CheckingIntegrity
  config_store_check_integrity()
  firmware_store_get_pending_flags(&self_check, &rollback)   /* SD-00c */
  → cfg fail: Faulted
  (pending flags recorded for sub-state 5 routing)

sub-state 2 — LoadingConfig
  config_store_load() + config_service_apply_loaded()
  → fail: Faulted

sub-state 3 — BringingUpSensors
  sensor_service_init() + alarm_service_init()
  → fail: Faulted

sub-state 4 — StartingMiddleware
  modbus_poller_init()
  cloud_publisher_init()
  time_service_init()
  update_service_init()
  console_service_init()
  health_monitor_init()
  → fail on any: Faulted

sub-state 5 — SelfChecking           [REQ-DM-040]
  probe sensor_service_is_ready()
  probe modbus_poller_is_ready()      /* link-check: at least one poll attempt */
  probe cloud_publisher_is_ready()    /* WiFi associated check */

  [post-update boot path — SD-00c]
  if pending_self_check:
      update_service_resume_self_checking()
      → wait for LC_EVENT_SELF_CHECK_PASS / LC_EVENT_SELF_CHECK_FAIL
      on PASS: firmware_store_confirm_self_check() → Operational
      on FAIL: update_service_resume_rollback()    → FirmwareStore.rollback()
               → reset_driver_soft_reset()
  if pending_rollback:
      update_service_resume_after_rollback()
      → cloud_publisher_report_rollback_result()
      → Operational
  else:
      on all probes pass → Operational
      on any probe fail  → Faulted
```

---

## 5. Operational — event handling

In Operational, LifecycleTask blocks on the event queue with a timeout
equal to the EditingConfig watchdog period (5 minutes). On timeout with
no events, it kicks the watchdog (if enabled) and loops.

| Event | Action |
|-------|--------|
| `LC_EVENT_CONFIG_EDIT_ENTER` | Snapshot config via `config_service_snapshot()`; transition to EditingConfig; start 5-minute timeout timer |
| `LC_EVENT_RESTART_REQUESTED` (GW) | Log intent; transition to Restarting; notify CloudPublisher to flush queue |
| `LC_EVENT_OTA_REQUESTED` (GW) | Lock OTA entry (REQ-DM-054 — reject if already in progress); transition to UpdatingFirmware; call `update_service_start()` |
| `LC_EVENT_UNRECOVERABLE_FAULT` | Log fault code from `param`; push `HEALTH_EVENT_FAULT`; transition to Faulted |

---

## 6. EditingConfig

Entry action: `config_service_snapshot()` — saves current in-memory
config so it can be restored on cancel or timeout.

Exit actions:
- Apply: `config_service_commit()` → notify affected components (SensorService for threshold changes, ModbusSlave for address changes).
- Cancel / timeout: `config_service_restore_snapshot()`.

The 5-minute timeout is a FreeRTOS software timer created at
`lifecycle_controller_init()` and started on each EditingConfig entry.
On expiry the timer posts `LC_EVENT_CONFIG_EDIT_TIMEOUT` to the queue.

---

## 7. Restarting (GW only — UC-17, REQ-DM-010–030)

The two-step confirmation guard is defined in the Modbus register map
(CMD_SOFT_RESTART at address 0x0202, confirmation token 0xA5A5).

```
Operational → LC_EVENT_RESTART_REQUESTED:
  Set restart_pending = true; log; notify CloudPublisher to flush.

    LC_EVENT_RESTART_CONFIRMED (within REQ-DM-020 timeout):
      log; reset_driver_soft_reset()   → NVIC_SystemReset()

    Timeout (no confirmation):
      restart_pending = false; log; stay Operational.
```

If a second `LC_EVENT_RESTART_REQUESTED` arrives while
`restart_pending` is true, it is treated as the confirmation.
This handles the case where the Modbus master issues the command twice
using the same register write.

---

## 8. Faulted

Entry action: log fault code; push `HEALTH_EVENT_FAULT` via IHealthReport;
set LED to fault pattern (via `health_monitor_set_led_fault()`).

Do activity: continue kicking the watchdog if enabled (no watchdog
reset on top of an application fault — the fault LED must remain visible).
No other processing.

Exit: hardware reset only. No `LC_EVENT_*` causes Faulted exit.

---

## 3. Internal design

```c
/* lifecycle_controller.c */

typedef struct {
    bool                    initialised;
    lifecycle_state_t       state;          /* volatile — read by other tasks */
    lifecycle_reset_cause_t reset_cause;
    QueueHandle_t           event_queue;    /* depth 4 */
    TimerHandle_t           edit_timeout_timer;
    bool                    restart_pending;           /* GW only */
    bool                    pending_self_check;        /* GW only */
    bool                    pending_rollback;          /* GW only */
    uint8_t                 cfg_snapshot[CONFIG_STORE_MAX_DATA_BYTES]; /* edit snapshot */
} LifecycleControllerState;

static LifecycleControllerState s_lc;
```

`s_lc.state` is declared `volatile` so `lifecycle_get_state()` reads a
coherent value without a mutex. All writes to `s_lc.state` happen inside
`LifecycleTask` — no concurrent write path exists.

The config snapshot buffer (`CONFIG_STORE_MAX_DATA_BYTES` ≤ 32 712 bytes)
is the largest field. This places `s_lc` in static storage, not on the
stack — annotate clearly in the code. See LC-O1.

---

## 10. Reset cause detection

Detected once at startup before `lifecycle_controller_init()`, using
RCC reset-status flags (RCC_CSR on STM32F469; RCC_CSR on STM32L475):

```c
lifecycle_reset_cause_t detect_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;          /* clear flags for next boot */

    if (csr & RCC_CSR_IWDGRSTF) { return LIFECYCLE_RESET_WATCHDOG; }
    if (csr & RCC_CSR_SFTRSTF)  { return LIFECYCLE_RESET_SOFT;     }
    if (csr & RCC_CSR_PINRSTF)  { return LIFECYCLE_RESET_POWER_ON; }
    return LIFECYCLE_RESET_UNKNOWN;
}
```

On a soft reset following OTA (SD-00c), `RCC_CSR_SFTRSTF` is set, which
would give `LIFECYCLE_RESET_SOFT`. The pending-flag check in
CheckingIntegrity distinguishes the OTA-reboot case from a plain
soft reset.

---

## 11. Init ordering

```c
/* main.c / startup — before LifecycleTask is created */
s_lc.reset_cause = detect_reset_cause();

/* LifecycleTask entry point — runs init sub-states sequentially */
void vLifecycleTask(void *pvParameters)
{
    lifecycle_controller_run();  /* drives state machine; never returns */
}
```

`LifecycleTask` is the **last task created** in `main()`. All other tasks
exist before it starts, but they block on a start-gate event group bit
set by LifecycleController at the end of Init. This ensures no application
task races ahead of the boot sequence.

---

## 12. Board differences summary

| Aspect | FD | GW |
|--------|----|----|
| Init sub-state 4 | BringingUpLCD | StartingMiddleware continues |
| Init sub-state 5 | StartingMiddleware | SelfChecking (DM-040) |
| Post-update boot path | — | pending_self_check / pending_rollback |
| Restarting state | — | Yes (UC-17, DM-010–030) |
| UpdatingFirmware state | — | Yes (UC-18) |
| Splash screen | Yes (LD-200–240) | — |
| ResetDriver | — | Used in Restarting and post-OTA rollback |
| `config_service_snapshot` buffer | ~same | ~same |

The code is distinct per board (`lifecycle_fd.c` / `lifecycle_gw.c`), sharing
`lifecycle_types.h` for the shared enums and `lifecycle_common.c` for shared
logic (queue-post, reset-cause detection, EditingConfig timeout).

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

All subsystem calls (`sensor_service_init()`, `config_store_load()`, etc.)
are replaced with stubs that return configurable success/failure codes.

Minimum test cases:
- All Init sub-states succeed → state == OPERATIONAL.
- CheckingIntegrity fails → state == FAULTED; no further sub-states entered.
- LoadingConfig fails → state == FAULTED.
- BringingUpLCD fails (FD) → state == FAULTED.
- `LC_EVENT_CONFIG_EDIT_ENTER` in Operational → state == EDITING_CONFIG.
- `LC_EVENT_CONFIG_EDIT_APPLY` → `config_service_commit()` called; state == OPERATIONAL.
- `LC_EVENT_CONFIG_EDIT_TIMEOUT` → `config_service_restore_snapshot()` called; state == OPERATIONAL.
- GW: `LC_EVENT_RESTART_REQUESTED` + `LC_EVENT_RESTART_CONFIRMED` → `reset_driver_soft_reset()` called.
- GW: `LC_EVENT_RESTART_REQUESTED` + timeout → `restart_pending` cleared; state stays OPERATIONAL.
- GW: pending_self_check on boot → `update_service_resume_self_checking()` called.
- GW: pending_rollback on boot → `update_service_resume_after_rollback()` called.
- `LC_EVENT_UNRECOVERABLE_FAULT` in any state → state == FAULTED.
- `lifecycle_get_state()` from another task context → correct state returned.

---

## 8. Open items

| ID    | Item |
|-------|------|
| LC-O1 | Config snapshot buffer size — `CONFIG_STORE_MAX_DATA_BYTES` (32 712 bytes) makes `LifecycleControllerState` very large for a static variable. Confirm it fits in BSS without pushing other static data out of budget. Alternative: allocate the snapshot buffer separately in a dedicated BSS section. |
| LC-O2 | Init timeout budget (REQ-NF-213, value TBD) — a hardware timer started on Init entry must fire and push `LC_EVENT_UNRECOVERABLE_FAULT` if the total Init duration exceeds the budget. Timer not yet specified; implement at coding time once the TBD value is set. |
| LC-O3 | Start-gate event group bit — the bit index for gating other tasks must be allocated from a project-wide event group map (similar to the RTC backup register map). Not yet produced. |
| LC-O4 | Restart confirmation timeout (GW, REQ-DM-020) — value TBD in SRS. Provisional: 30 seconds. Implement as a FreeRTOS software timer started on `LC_EVENT_RESTART_REQUESTED`; on expiry post `LC_EVENT_CONFIG_EDIT_CANCEL` (or a dedicated LC_EVENT_RESTART_TIMEOUT) to reset `restart_pending`. |
