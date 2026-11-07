# 07 · Navigation & State Machine

## App states

The device has a small top-level state machine in `app.c`. Tab navigation only operates inside the `RUNNING` super-state.

```
        power on
           │
           ▼
      ┌─────────┐   first valid reading OR 5s timeout
      │ SPLASH  │ ─────────────────────────────────────┐
      └─────────┘                                       │
                                                        ▼
                                                  ┌───────────┐
                                                  │  RUNNING  │  ◀── normal operation
                                                  └───────────┘
                                                   │  ▲
                                          tap tab  │  │
                                                   ▼  │
                              ┌──────────┬──────────┬──────────┐
                              │ SENSORS  │  STATUS  │  ALARMS  │  CONFIG
                              └──────────┴──────────┴──────────┘
```

> **Note:** The Firmware Update screen (originally screen 02) is **out of scope** for this build. Leave a `STATE_FIRMWARE` enum slot and a `// TODO: OTA` comment in `app.c` so it can be added later, but do not build the screen.

## Tab structure (inside RUNNING)

Four tabs, fixed order. Each maps to one screen module that internally chooses a variant based on model state.

| Tab | Module | Variant logic |
|---|---|---|
| **SENSORS** | `screen_sensors` | Picks per-card state from each `sensor_t.state`. The whole screen is one layout; cards render Normal / Waiting / Stale / Error individually. (Screens 03–06 are *states of one screen*, not separate screens.) |
| **STATUS** | `screen_status` | Healthy (07) vs Degraded (08) chosen by `system_status.banner_active`. |
| **ALARMS** | `screen_alarms` | Empty (09) vs Active (10) chosen by `alarm_count()`. |
| **CONFIG** | `screen_config` | Idle (11) / Validation error (12) / Confirm modal (13) are sub-states inside the screen, driven by `cfg_invalid_count()` and a local `modal_open` flag. |

So although the design enumerates 13 numbered screens, the implementation is:

- 1 splash screen
- 4 tab screens, each handling its own internal states
- (firmware screen skipped)

That's **5 screen modules** total.

## Screen lifecycle

```c
typedef enum {
    STATE_SPLASH,
    STATE_RUNNING,
    // STATE_FIRMWARE,   // TODO: OTA — out of scope for v1
} app_state_t;

typedef enum { TAB_SENSORS, TAB_STATUS, TAB_ALARMS, TAB_CONFIG } tab_t;

static app_state_t s_state;
static tab_t       s_tab;
static lv_obj_t   *s_screen;   // currently loaded screen

void app_goto_tab(tab_t t) {
    if (s_screen) lv_obj_del(s_screen);   // free old screen
    s_tab = t;
    switch (t) {
        case TAB_SENSORS: s_screen = screen_sensors_create(); break;
        case TAB_STATUS:  s_screen = screen_status_create();  break;
        case TAB_ALARMS:  s_screen = screen_alarms_create();  break;
        case TAB_CONFIG:  s_screen = screen_config_create();  break;
    }
    lv_scr_load(s_screen);
}
```

Tab buttons in `tab_bar.c` call `app_goto_tab()`. The tab bar reads `s_tab` to render the active highlight.

## Data refresh

A single `lv_timer` (period = config `poll_ms`, default 5000) drives everything:

```c
static void tick_cb(lv_timer_t *t) {
    backend_poll();                 // refresh models
    if (s_state == STATE_SPLASH) {
        if (any_sensor_valid() || splash_elapsed_ms() > 5000)
            app_enter_running();
        return;
    }
    // Re-render the live parts of the current screen in place.
    switch (s_tab) {
        case TAB_SENSORS: screen_sensors_refresh(s_screen); break;
        case TAB_STATUS:  screen_status_refresh(s_screen);  break;
        case TAB_ALARMS:  screen_alarms_refresh(s_screen);  break;
        case TAB_CONFIG:  break; // config is user-driven, not data-driven
    }
    header_refresh_time();          // tick the HH:MM:SS in the header
}
```

`*_refresh()` functions update text/colors on existing objects — they don't rebuild the tree. Only a tab switch frees and recreates.

The header clock should tick every second even when `poll_ms` is larger; run a separate 1 s timer for the clock, or set the main tick to 1 s and only poll the backend every N ticks.

## Config interaction flow

```
CONFIG screen
   │
   │  user taps a field → focus it, caret blinks
   │  user edits value via the on-screen numeric keypad
   ▼
cfg_validate() runs on every change
   │
   ├─ invalid_count > 0 → field shows error, footer "N invalid field(s)", APPLY disabled  (screen 12)
   │
   └─ invalid_count == 0 && modified_count > 0 → APPLY enabled  (screen 11)
                                                       │
                                          user taps APPLY
                                                       ▼
                                              confirm modal opens  (screen 13)
                                                       │
                                ┌──────────────────────┴──────────────────────┐
                          tap CANCEL                                   tap CONFIRM & SAVE
                                │                                              │
                          close modal                              cfg_apply() → persistence
                          (changes still pending)                  → modal closes, footer "Saved"
```

CANCEL on the main footer (not the modal) calls `cfg_revert()` and clears all pending edits.

Field editing uses an **on-screen numeric keypad** — touch is the only input on this device. Implement as a restyled `lv_keyboard` in `LV_KEYBOARD_MODE_NUMBER` that slides up from the bottom when a field is focused, themed with `theme.h` tokens so it matches the rest of the UI (don't let LVGL's default keyboard styling leak through). Keep at least 220 px of the focused field visible above the keyboard. Dismiss the keyboard on a tap outside any field or on the keyboard's confirm key.

## Back / exit

There is no global back button — the tab bar is always the way out of any tab. The only modal (config confirm) traps focus until dismissed. Splash and (future) firmware screens auto-exit; they have no manual navigation.
