# Navigation Map — Field Device LCD UI

> REQ-LD-000 (SRS v1.2): The device shall provide navigation between five operational screens —
> sensor readings, sensor trend, system status, system configuration, and alarms.

---

## Application state machine

```
                          POWER ON
                              │
                              ▼
                     ┌────────────────┐
                     │  SPLASH SCREEN │  splash.html
                     │  (boot loader) │
                     └───────┬────────┘
                             │  first valid sensor reading
                             │  OR 5 s timeout (REQ-LD-200)
                             ▼
              ┌──────────────────────────────┐
              │        RUNNING STATE         │
              │  5-tab navigation bar active │
              └──────────────────────────────┘
```

---

## Five-tab navigation bar

All operational screens share the same 5-tab bar (height 64 px, 5 × 160 px columns).
Tab switches are **instantaneous** — no slide or fade animation.

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│  0 SENSORS │  1 TREND   │  2 STATUS  │  3 ALARMS  │  4 CONFIG  │  ← tab index  │
│  160 px    │  160 px    │  160 px    │  160 px    │  160 px    │               │
└──────────────────────────────────────────────────────────────────────────────────┘
```

| Index | Label | File | Default |
|-------|-------|------|---------|
| 0 | SENSORS | `sensor-readings.html` | ✓ (first tab shown on boot completion) |
| 1 | TREND | `sensor-trend.html` | |
| 2 | STATUS | `system-status.html` | |
| 3 | ALARMS | `active-alarms.html` | |
| 4 | CONFIG | `configuration.html` | |

---

## Transition table

| From | To | Trigger | Notes |
|------|----|---------|-------|
| SPLASH | SENSORS (tab 0) | Boot complete + first valid reading OR 5 s timeout | Automatic; no user action required |
| Any operational tab | Any operational tab | Touch tab button | Instant switch; no confirmation |
| CONFIG (idle) | CONFIG (editing) | Tap a config field | On-screen keyboard appears |
| CONFIG (editing) | CONFIG (idle) | CANCEL button | Discards unsaved changes |
| CONFIG (editing) | CONFIG (modal) | APPLY button (validation pass) | Confirm modal overlays CONFIG |
| CONFIG (modal) | CONFIG (saved) | CONFIRM button | Writes to NVM; modal dismissed |
| CONFIG (modal) | CONFIG (editing) | CANCEL in modal | Modal dismissed; field still focused |

---

## Screen-level state table

| Screen | States | State trigger |
|--------|--------|---------------|
| Splash | Booting (n%), Boot complete 100% | Progress counter from subsystem init sequence |
| Sensor readings | Normal, Waiting, Stale (per-channel), Error (per-channel) | Backend data flags |
| Sensor trend | Accumulating (< 60 samples), Full window (60/60, rolling) | History buffer fill level |
| System status | Healthy, Degraded | Any sensor offline OR active alarm count > 0 |
| Active alarms | Empty, Active (N alarms) | Alarm list length |
| Configuration | Idle, Editing (focused field), Validation error, Confirm modal | User interaction |

---

## Navigation constraints

- The tab bar is **not** accessible during the SPLASH state (not rendered).
- During the CONFIG confirm modal, the tab bar tap events are blocked (modal takes full input focus).
- There is no back/forward history — each tab is independent.
- No deep links or programmatic screen switching are exposed to the application layer;
  only `app_goto_tab(tab_index)` (from `07_NAVIGATION.md`) is used.
