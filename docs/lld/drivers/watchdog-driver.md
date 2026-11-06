# WatchdogDriver — LLD Companion

**Document:** `docs/lld/drivers/watchdog-driver.md`
**Version:** 1.0
**Board scope:** Gateway (B-L475E-IOT01A) only; the Field Device follows later (WDG-O1)
**Layer:** Driver
**Status:** Implemented, hardware-validated 2026-11-06 (TC-HW-WDG-001, 002)
**Date:** November 2026

**HLD anchor:** `lld.md` §5 O3 (hardware watchdog in scope — decided here: yes), REQ-NF-109, REQ-NF-202, LC-O8

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Starts the independent watchdog (IWDG) and refreshes it | REQ-NF-109 |
| PROVIDES (upward) | `watchdog_start`, `watchdog_kick` | this document |
| USES (downward) | CMSIS (`IWDG`, `DBGMCU`) | `hld.md` §9 |
| Consumer | LifecycleWiring supplies the `lc_watchdog_if_t` that LifecycleController calls from its 1 s loop (`kick_watchdog()`) | `lifecycle-controller-lld.md` §7 |

REQ-NF-109: the system shall use a system watchdog of 10 seconds. The rationale in the SRS is that 10 s is about 12 times the longest legitimate blocking time of any task, so a hung task is caught within one alarm-detection cycle.

## 2. Public API (`watchdog_driver.h`)

```c
typedef enum { WATCHDOG_OK = 0, WATCHDOG_ERR_RANGE = 1 } watchdog_err_t;

#define WATCHDOG_TIMEOUT_MIN_MS (100U)
#define WATCHDOG_TIMEOUT_MAX_MS (30000U)

watchdog_err_t watchdog_start(uint32_t timeout_ms);  /* cannot be stopped */
void           watchdog_kick(void);                  /* no effect before start */
bool           watchdog_is_running(void);
```

- `watchdog_start` programs prescaler /256 on the 32 kHz LSI (125 Hz tick), reload = `timeout_ms * 125 / 1000` (1 to 4095), waits for the register-update flags with a bounded loop, then starts the counter. The IWDG cannot be stopped once started; only a reset clears it.
- The LSI is not accurate (the datasheet allows roughly ±20 %), so a 10 s setting expires between about 8 and 12 s. REQ-NF-109 is a ceiling for hang detection, not a timer, so this is accepted.
- `watchdog_start` also sets `DBGMCU_APB1FZR1.DBG_IWDG_STOP` so a halted debugger does not trigger a reset (bench work, openocd).
- A second `watchdog_start` is ignored and returns `WATCHDOG_OK` (the first timeout stays).
- Threading: task context; the registers are written whole, so no lock is needed. Not ISR-safe by contract.

## 3. Policy (not in the driver)

- The watchdog is started in `main.c` immediately before BootTask turns into LifecycleTask, after the join, NTP and broker setup, so the long boot steps are not supervised. The first kick comes from the LifecycleController loop.
- LifecycleWiring's kick function refreshes the counter only if SensorTask has completed a cycle since the previous kick (`sensor_snapshot_t.cycle_count` moved). If SensorTask was never created the gate is skipped. A stuck acquisition therefore stops the kicks and the board resets within 10 s.
- The Faulted state keeps kicking (lifecycle LLD §Faulted) so the fault LED stays up.
- Test aid: building with `-DGATEWAY_WDG_TEST_STALL` stops the kicks 20 s after the first one, to prove the reset on the board.

## 4. Hardware contract

| Item | Value |
|---|---|
| Registers | `IWDG->KR` (0x5555 unlock, 0xCCCC start, 0xAAAA refresh), `PR`, `RLR`, `SR`; `DBGMCU->APB1FZR1` |
| Clock | LSI, 32 kHz nominal, started by hardware when the IWDG starts |
| Reset cause | `RCC->CSR.IWDGRSTF`, read by LifecycleController (`LIFECYCLE_RESET_WATCHDOG`) |

## 5. Error and fault behaviour

`WATCHDOG_ERR_RANGE` for a timeout outside 100 to 30 000 ms; nothing is started. If `SR` does not clear within the bounded wait the counter is started anyway with the registers as written (the reload is latched when the flags clear).

## 6. Unit-test plan

| ID | Case |
|---|---|
| T-WDG-01 | `watchdog_start(10000)`: PR = 6, RLR = 1250, last key written 0xAAAA (the register sequence is start 0xCCCC, unlock 0x5555, set, refresh 0xAAAA) |
| T-WDG-02 | out-of-range timeouts return `WATCHDOG_ERR_RANGE` and write nothing |
| T-WDG-03 | `watchdog_kick` before start does nothing; after start writes 0xAAAA |
| T-WDG-04 | a second start is ignored |
| T-WDG-05 | the debug freeze bit is set |
| T-WDG-06 | the maximum timeout gives RLR <= 4095 |

Hardware: TC-HW-WDG-001 normal run, 10 minutes, no reset; TC-HW-WDG-002 stalled kicks (`GATEWAY_WDG_TEST_STALL`) reset in 8 to 12 s and the next boot reports a watchdog reset cause.

## 7. Open items

| ID | Item | Status |
|---|---|---|
| WDG-O1 | Field Device watchdog (the FD lifecycle LLD already has the kick hook) | Open |
| WDG-O2 | Only SensorTask is supervised; ModbusPoller and CloudPublisher hang detection would need task check-in flags | Open |

## 8. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| WDG-D1 | Hardware watchdog is in scope (closes `lld.md` O3 and LC-O8) | REQ-NF-109 |
| WDG-D2 | Independent watchdog (IWDG), not the window watchdog | Runs from its own clock; cannot be disabled by software |
| WDG-D3 | The kick is gated on SensorTask progress, in LifecycleWiring, not in the driver | The driver stays policy-free |
| WDG-D4 | Start after the long boot steps | Join, NTP and TLS setup take seconds and are covered by the init timer (LC-O2) |
