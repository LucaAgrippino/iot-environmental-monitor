# LLD Companion — GraphicsLibrary

**Layer:** Middleware  
**Board:** Field Device (FD) only  
**Provides:** `IGraphics`  
**Consumes:** `ILcd` (LcdDriver), `ITouchscreen` (TouchscreenDriver), `ILogger`  
**SRS traces:** REQ-LD-000, REQ-LD-050, REQ-NF-108  
**HLD ref:** `components.md` §Middleware — GraphicsLibrary; `hld.md` §5.2; `task-breakdown.md` §4.2 (`LcdUiTask`), §4.4 (IPC)
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** GraphicsLibrary in `components.md` (FD middleware layer)

---

## 1. Sources

GraphicsLibrary wraps LVGL (Light and Versatile Graphics Library) and
owns its driver integration — how pixels reach the screen and how touch
events enter the widget system. `LcdUi` (Application) uses LVGL's widget
API directly but never calls driver functions; GraphicsLibrary is the
seam between LVGL and the two drivers below it.

The three integration points GraphicsLibrary owns:

| Integration point | What GraphicsLibrary does |
|-------------------|--------------------------|
| Display flush callback | Translates LVGL dirty-region notifications into `lcd_driver_flush()` calls |
| Touch input callback | Polls `touchscreen_driver_read()` and feeds coordinates to LVGL's input device |
| Tick source | Provides `graphics_tick_increment()` so a timer can advance LVGL's internal clock |

Everything else — screen layout, widget creation, data binding, screen
transitions — belongs to `LcdUi`.

---

## 2. Library choice — LVGL

LVGL was chosen over TouchGFX (the other leading option for STM32) for
portability (MIT licence, no vendor lock-in, runs on any MCU with a
framebuffer). Reference: `components.md` §"GraphicsLibrary wraps LVGL
(chosen over TouchGFX for portability)."

LVGL version: **v8.x** (latest stable v8 minor). Do not use v9 — the
API changed significantly and v8 has more host-side simulator tooling.
Pin the version in the submodule and do not update without regression
testing.

LVGL is included as a Git submodule under `firmware/field-device/lib/lvgl/`.
`lv_conf.h` lives alongside it in `firmware/field-device/config/` and is
the single point of configuration for the entire LVGL build (colour depth,
features enabled, memory settings).

---

## 3. Display parameters

From the STM32F469 Discovery board (UM1932) and the 4" DSI LCD panel (ZZ1):

| Parameter | Value |
|-----------|-------|
| Display | 4" DSI TFT (ZZ1) |
| Resolution | 800 × 480 pixels |
| Colour format | RGB565 (16 bpp) |
| Framebuffer location | External SDRAM (owned by LcdDriver via SdramDriver) |
| Framebuffer size | 800 × 480 × 2 = 768 KB |
| Interface to MCU | MIPI DSI → LTDC |

---

## 4. Data types

```c
/* graphics_library.h */

typedef enum {
    GRAPHICS_ERR_OK        = 0,
    GRAPHICS_ERR_NOT_INIT  = 1,
    GRAPHICS_ERR_NULL_ARG  = 2,
    GRAPHICS_ERR_LVGL_FAIL = 3,
} graphics_err_t;
```

GraphicsLibrary exposes LVGL's `lv_disp_t *` and `lv_indev_t *` opaque
handles to `LcdUi` — not wrapped further. Wrapping the entire LVGL widget
API would produce a mirror of LVGL with no additional value.
The principle: GraphicsLibrary hides driver coupling; it does not hide
LVGL's own widget model. LcdUi depends on LVGL headers directly and is
not portable across graphics libraries — that trade-off is accepted.

---

## 2. Public API — `IGraphics`

```c
/**
 * @brief  Initialise LVGL, register flush and input callbacks, and
 *         configure the draw buffer.
 *
 * Must be called after lcd_driver_init() and touchscreen_driver_init().
 * Internally calls lv_init(), lv_disp_drv_register(), and
 * lv_indev_drv_register().
 *
 * @return GRAPHICS_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
graphics_err_t graphics_init(void);

/**
 * @brief  Advance LVGL's internal tick counter.
 *
 * Call this with the elapsed milliseconds from a FreeRTOS timer callback
 * or SysTick hook. LVGL uses this for animations, press detection,
 * and long-press events.
 *
 * Safe to call from timer task context (FreeRTOS timer callback) —
 * internally calls lv_tick_inc() which is documented as ISR-safe in LVGL v8.
 *
 * @param  elapsed_ms  Milliseconds since last call. Typically 1 (called
 *                     every 1 ms) or accumulated since the last increment.
 */
void graphics_tick_increment(uint32_t elapsed_ms);

/**
 * @brief  Run one LVGL processing cycle.
 *
 * Calls lv_task_handler() which:
 *   - evaluates pending widget updates and redraws dirty regions,
 *   - calls the flush callback to push dirty pixels to LcdDriver,
 *   - calls the touch input callback to poll TouchscreenDriver,
 *   - services LVGL animations and timers.
 *
 * Must be called from LcdUiTask at a rate ≥ 5 Hz (REQ-NF-108).
 * In practice called every 200 ms (periodic notify) AND immediately
 * after a touch event notify (task-breakdown.md §4.4 IPC table).
 *
 * Never call from ISR context.
 *
 * @return GRAPHICS_ERR_OK; GRAPHICS_ERR_NOT_INIT if not initialised.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
graphics_err_t graphics_process(void);

/**
 * @brief  Return the LVGL display handle.
 *
 * Used by LcdUi to create screens on the correct display.
 * Valid only after graphics_init().
 *
 * @return lv_disp_t * (NULL if not initialised).
 */
lv_disp_t *graphics_get_display(void);

/**
 * @brief  Return the LVGL touchscreen input device handle.
 *
 * Used by LcdUi to assign input groups to screens.
 * Valid only after graphics_init().
 *
 * @return lv_indev_t * (NULL if not initialised).
 */
lv_indev_t *graphics_get_indev(void);
```

---

## 6. Draw buffer strategy

LVGL requires one or two draw buffers it renders into before flushing
to the physical framebuffer. Two strategies apply here:

### Option A — Partial draw buffer (default choice)

A static 16 KB partial buffer (4 096 pixels × 2 bytes = 8 KB; two of
these = 16 KB for double-buffered rendering) lives in SRAM. LVGL renders
a dirty region into the partial buffer, calls the flush callback, and
moves to the next dirty region.

```c
/* graphics_library.c */
static lv_color_t s_draw_buf_1[4096];   /* ~8 KB in SRAM */
static lv_color_t s_draw_buf_2[4096];   /* ~8 KB in SRAM — double buffer */
```

Pro: minimal SRAM overhead. Con: multiple flush cycles per full-screen
update → visible tearing at 5 Hz on fast animations. Acceptable for a
monitoring UI with mostly static content.

### Option B — Full-screen framebuffer as LVGL draw buffer

Point LVGL directly at the SDRAM framebuffer and let it render the whole
screen, then notify the flush callback that the buffer is ready. LTDC
reads directly from SDRAM — no copy.

Pro: no tearing; LVGL and LTDC share one buffer (zero-copy). Con: LVGL
must not write while LTDC is reading — coordinate with LTDC VSync
interrupt to gate the flush. More complex flush callback.

**Decision: start with Option A.** Switch to Option B at integration if
tearing is visible on the sensor screen. Tracked as GL-O1.

---

## 7. Flush callback

The flush callback is registered with LVGL during `graphics_init()`. It
is called by LVGL from within `lv_task_handler()` (i.e., from
`LcdUiTask` context):

```c
static void flush_cb(lv_disp_drv_t *drv,
                     const lv_area_t *area,
                     lv_color_t *color_p)
{
    /* Map LVGL area to pixel offset in the SDRAM framebuffer */
    uint32_t x1 = (uint32_t)area->x1;
    uint32_t y1 = (uint32_t)area->y1;
    uint32_t w  = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h  = (uint32_t)(area->y2 - area->y1 + 1);

    lcd_driver_blit(x1, y1, w, h, (uint16_t *)color_p);

    /* Signal LVGL that the flush is complete.
     * If lcd_driver_blit() is synchronous (blocks until DMA done),
     * call immediately. If asynchronous, call from the DMA-complete
     * ISR / callback instead. See GL-O2. */
    lv_disp_flush_ready(drv);
}
```

The `lv_area_t` → pixel offset mapping assumes the framebuffer layout:
`framebuffer[y * DISPLAY_WIDTH + x]` in row-major RGB565 order.

---

## 8. Touch input callback

The touch input callback is registered with LVGL as an `lv_indev_t`
of type `LV_INDEV_TYPE_POINTER`. It is called by LVGL from within
`lv_task_handler()`:

```c
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    touchscreen_point_t pt;
    bool pressed = (touchscreen_driver_read(&pt) == TOUCHSCREEN_ERR_OK);

    data->point.x = (lv_coord_t)pt.x;
    data->point.y = (lv_coord_t)pt.y;
    data->state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
```

TouchscreenDriver returns the last sampled coordinate and a pressed/
released flag. The ISR → `LcdUiTask` notification (task-breakdown.md §4.4,
`Touchscreen ISR → LcdUiTask: «notify» bit 1`) wakes LcdUiTask, which
calls `graphics_process()`, which calls `lv_task_handler()`, which calls
this callback to consume the touch event. LVGL handles debounce and
long-press detection internally.

---

## 9. Tick source

LVGL requires its tick counter to be updated regularly (1 ms resolution
is sufficient). The `graphics_tick_increment()` function is called by
a FreeRTOS software timer with a 1 ms period:

```c
/* In LcdUiTask or timer service context */
static void tick_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    graphics_tick_increment(1U);
}
```

The timer is created and started in `graphics_init()`. It runs in the
FreeRTOS timer service task — `lv_tick_inc()` is documented as
interrupt/task-safe in LVGL v8.

Do not call `lv_tick_inc()` from SysTick directly — SysTick is used by
FreeRTOS and adding application code there risks priority inversion.

---

## 3. Internal design

```c
/* graphics_library.c */

typedef struct {
    bool           initialised;
    lv_disp_t     *display;
    lv_indev_t    *touch_indev;
    lv_disp_drv_t  disp_drv;
    lv_indev_drv_t indev_drv;
    lv_disp_draw_buf_t draw_buf;
    lv_color_t     buf_1[4096];   /* Option A partial buffer 1 */
    lv_color_t     buf_2[4096];   /* Option A partial buffer 2 */
    TimerHandle_t  tick_timer;
} GraphicsLibraryState;

static GraphicsLibraryState s_gl;
```

Total static RAM: `sizeof(lv_color_t) × 8192 ≈ 16 KB` for the draw
buffers, plus small overhead for LVGL internal state. LVGL's heap
(`lv_mem`) is configured via `lv_conf.h` with `LV_MEM_CUSTOM 0` to use
a static array — **no dynamic allocation post-init** (P8).

---


### Synchronisation

This component uses an internal mutex to serialise concurrent callers. The mutex is created during `_init()` and held only for the duration of each guarded operation (bounded, short hold time). All public functions are task-safe but not ISR-safe.

### graphics_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### graphics_tick_increment

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### graphics_process

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### Principles applied

- **P1 (Strict directional layering).** Depends on ILcd and ITouchscreen (driver layer); Logger is a cross-cutting exception (P4).
- **P2 (Dependency Inversion).** Exposes `igraphics_t` vtable; LcdUi depends on `IGraphics`, not the concrete module.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Widget state and layout parameters in static structs; framebuffer owned by LcdDriver in SDRAM (pre-init, bounded); no post-scheduler heap.
- **P6 (Responsibility traces to requirements).** Draw primitives and layout functions trace to REQ-LD-000/050/NF-108 display requirements.
- **P8 (Total error propagation, no silent failures).** `graphics_err_t` on all draw/render operations; invalid coordinate arguments return error.
- **P9 (BARR-C coding standard).** Coordinates `uint16_t`; colour values `uint32_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `graphics_`; interface `IGraphics` -> `igraphics_t`; errors `GRAPHICS_ERR_*`.


## 11. `lv_conf.h` required settings

The following settings must be verified in `lv_conf.h` before building:

| Setting | Required value | Reason |
|---------|---------------|--------|
| `LV_COLOR_DEPTH` | `16` | RGB565 framebuffer |
| `LV_HOR_RES_MAX` | `800` | Display width |
| `LV_VER_RES_MAX` | `480` | Display height |
| `LV_MEM_CUSTOM` | `0` | Static heap — no `malloc` |
| `LV_MEM_SIZE` | `≥ 32768` | 32 KB static LVGL heap; tune at integration |
| `LV_USE_LOG` | `1` | Route LVGL log to `ILogger` |
| `LV_LOG_PRINTF` | map to `log_debug(...)` | Forward via Logger |
| `LV_TICK_CUSTOM` | `0` | Use `lv_tick_inc()` driven by timer |
| `LV_USE_ANIMATION` | `1` or `0` | Disable if RAM is tight |
| `LV_USE_GPU_STM32_DMA2D` | `1` | Hardware-accelerated blending (GL-O3) |

---

## 12. Init ordering

```
sdram_driver_init()           ← framebuffer memory available
lcd_driver_init()             ← LTDC + DSI configured; framebuffer pointer set
touchscreen_driver_init()     ← I2C + touch controller ready
graphics_init()               ← LVGL init, callbacks registered, tick timer created
[LcdUiTask created]           ← calls graphics_process() every 200 ms
```

GraphicsLibrary does not require two-phase init. No ISR consumer needs
to exist before `graphics_init()`. The tick timer is self-contained and
starts inside `graphics_init()`.

---

## 13. Thread safety

`graphics_process()` and `graphics_tick_increment()` are the only runtime
calls. `graphics_tick_increment()` calls `lv_tick_inc()` which is
documented as safe from any task context. `graphics_process()` calls
`lv_task_handler()` which must only be called from one task — `LcdUiTask`.

**No mutex needed** — the single-task caller model applies.
`lv_task_handler()` is not reentrant; document this constraint in the
`LcdUiTask` code.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

LVGL ships a PC simulator (SDL2-based) that allows `LcdUi` development
on a host machine without target hardware. Set up the simulator during
LCD UI development:

```
firmware/field-device/sim/        ← LVGL PC simulator project
  sim_display.c                   ← stub implementation of flush_cb using SDL2
  sim_touchscreen.c               ← stub implementation of touch_read_cb using SDL2 mouse
  main.c                          ← entry point; calls graphics_init(), then LcdUi screens
```

The simulator is not a unit test — it is a development and review tool.
It is not part of the automated CI build. See GL-O4.

Minimum manual test scenarios in the simulator:
- Sensor screen renders all four sensor values with correct units.
- Alarm state change updates the alarm indicator within 200 ms.
- Touch on screen-change button transitions to the correct screen.
- Splash screen displays during boot Init and clears on Operational entry.
- Config screen shows editable threshold values; touch commits or cancels.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| GL-O1 | Draw buffer strategy — confirm Option A (partial buffer) produces acceptable visual quality at 5 Hz on the sensor screen during integration. If tearing is visible on screen transitions, switch to Option B (VSync-gated full-framebuffer mode). | Evaluate visual quality at integration; switch to Option B if tearing observed | Open |
| GL-O2 | `lcd_driver_blit()` blocking vs DMA-async — if LcdDriver uses DMA to transfer from the partial draw buffer to SDRAM, `lv_disp_flush_ready()` must be called from the DMA transfer-complete ISR/callback, not inline in `flush_cb`. Confirm at LcdDriver LLD. | Confirm at LcdDriver LLD companion — DMA async vs blocking flush | Open |
| GL-O3 | DMA2D acceleration — `LV_USE_GPU_STM32_DMA2D 1` enables LVGL to use DMA2D for fill and copy operations. STM32F469 has DMA2D; enabling it is expected to halve render time per dirty region. Confirm at integration by comparing `lv_task_handler()` execution time with and without. | Benchmark lv_task_handler() at integration with/without DMA2D enabled | Open |
| GL-O4 | LVGL PC simulator — confirm SDL2 availability in the development environment (Limerick laptop). If SDL2 is unavailable, use LVGL's framebuffer-to-BMP export mode as a fallback for visual review. | Check SDL2 availability on development machine; use BMP-export fallback if absent | Open |
