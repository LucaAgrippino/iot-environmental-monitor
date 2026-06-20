# LLD Companion — GraphicsLibrary

**Layer:** Middleware
**Board:** Field Device (FD) only
**Provides:** `graphics_*` (direct C API — no vtable, see GL-D8)
**Consumes:** `lcd_driver_*`, `touchscreen_driver_*`, `ILogger`, FreeRTOS timer + mutex
**SRS traces:** REQ-LD-000, REQ-LD-050, REQ-NF-108
**HLD ref:** `components.md` §Middleware — GraphicsLibrary; `hld.md` §5.2; `task-breakdown.md` §4.2 (`LcdUiTask`), §4.4 (IPC)
**Version:** 0.3
**Date:** June 2026
**Status:** Pass-H ready

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
| Display flush callback | Translates LVGL dirty-region notifications into a memcpy + `lcd_flush()` call |
| Touch input callback | Polls `touchscreen_driver_read()` and feeds coordinates to LVGL's input device |
| Tick source | Provides `graphics_tick_increment()` so a FreeRTOS timer can advance LVGL's internal clock |

Everything else — screen layout, widget creation, data binding, screen
transitions — belongs to `LcdUi`. LcdUi depends on LVGL headers directly
and is not portable across graphics libraries; that trade-off is accepted
because wrapping the entire LVGL widget API would produce a mirror of
LVGL with no additional value.

---

## 2. Library choice and version

LVGL was chosen over TouchGFX for portability (MIT licence, no vendor
lock-in, runs on any MCU with a framebuffer). Reference: `components.md`
§"GraphicsLibrary wraps LVGL (chosen over TouchGFX for portability)."

**LVGL version: v8.3.11 LTS** (GL-D1). Pinned via Git submodule at
`vendor/lvgl/` (GL-D3). Do not update without regression testing —
v9 is API-incompatible and v8 has more host-side simulator tooling.

---

## 3. Display parameters

From the STM32F469 Discovery board (UM1932) and the 4" DSI LCD panel:

| Parameter | Value |
|-----------|-------|
| Display | 4" DSI TFT panel (OTM8009A controller) |
| Resolution | 800 × 480 pixels |
| Colour format | **ARGB8888 (32 bpp)** (GL-D2) |
| Framebuffer location | External SDRAM (owned by LcdDriver via SdramDriver) |
| Framebuffer size | 800 × 480 × 4 = 1 536 000 bytes (1.5 MB) |
| Interface to MCU | MIPI DSI → LTDC |

ARGB8888 was chosen because `BSP_LCD_LayerDefaultInit` defaults to it
and the working LCD bring-up validated this pipeline end-to-end. RGB565
is deferred as a possible optimisation (halves framebuffer and draw
buffer footprint, halves DMA2D bandwidth) but is not required —
SDRAM is 16 MB and SRAM has the headroom for 32 KB draw buffers.

---

## 4. Public API

### 4.1 Error type

```c
/* graphics_library.h */

typedef enum {
    GRAPHICS_ERR_OK        = 0,
    GRAPHICS_ERR_NOT_INIT  = 1,
    GRAPHICS_ERR_NULL_ARG  = 2,
    GRAPHICS_ERR_LVGL_FAIL = 3,
} graphics_err_t;
```

### 4.2 Functions

GraphicsLibrary exposes LVGL's `lv_disp_t *` and `lv_indev_t *` opaque
handles to `LcdUi` — not wrapped further. The principle: GraphicsLibrary
hides driver coupling; it does not hide LVGL's widget model.

```c
/**
 * @brief  Initialise LVGL, register flush and input callbacks, create
 *         the tick timer, and configure the draw buffer.
 *
 * Must be called after lcd_init() and touchscreen_init().
 * Internally calls lv_init(), lv_disp_drv_register(),
 * lv_indev_drv_register(), and starts the 1 ms FreeRTOS tick timer.
 *
 * @return GRAPHICS_ERR_OK on success;
 *         GRAPHICS_ERR_LVGL_FAIL if LVGL init or driver registration fails.
 * @note Threading: task-context only, non-blocking. Must be called
 *       before the scheduler starts (so the tick timer is ready when
 *       the scheduler starts running it).
 */
graphics_err_t graphics_init(void);

/**
 * @brief  Advance LVGL's internal tick counter.
 *
 * Called from the FreeRTOS software timer (period 1 ms). LVGL uses
 * this for animations, press detection, and long-press events.
 *
 * Internally calls lv_tick_inc(), which LVGL v8 documents as safe
 * from any context. This function does NOT take the internal mutex
 * (see §7) — it is the one public function safe to call concurrently
 * with graphics_process().
 *
 * @param  elapsed_ms  Milliseconds since last call. Typically 1.
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
 * Holds the internal mutex while lv_task_handler() runs.
 * Never call from ISR context.
 *
 * @return GRAPHICS_ERR_OK; GRAPHICS_ERR_NOT_INIT if not initialised.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
graphics_err_t graphics_process(void);

/**
 * @brief  Return the LVGL display handle.
 *
 * Used by LcdUi to create screens on the correct display. Valid only
 * after graphics_init().
 *
 * @return lv_disp_t * (NULL if not initialised).
 */
lv_disp_t *graphics_get_display(void);

/**
 * @brief  Return the LVGL touchscreen input device handle.
 *
 * Used by LcdUi to assign input groups to screens. Valid only after
 * graphics_init().
 *
 * @return lv_indev_t * (NULL if not initialised).
 */
lv_indev_t *graphics_get_indev(void);
```

---

## 5. Draw buffer strategy

LVGL renders into a partial draw buffer in SRAM, then the flush
callback copies the rendered region into the SDRAM framebuffer owned
by LcdDriver.

**Buffer configuration (GL-D2, Q-A locked):**

```c
/* graphics_library.c — internal */
#define GFX_DRAW_BUF_PIXELS   (4096U)            /* 4096 pixels per buffer */

/* sizeof(lv_color_t) == 4 with LV_COLOR_DEPTH 32 (ARGB8888) */
static lv_color_t s_draw_buf_1[GFX_DRAW_BUF_PIXELS];  /* 16 KB SRAM */
static lv_color_t s_draw_buf_2[GFX_DRAW_BUF_PIXELS];  /* 16 KB SRAM */
```

**Total SRAM footprint: 32 KB** for draw buffers, plus LVGL internal
state and the `GraphicsLibraryState` struct (negligible).

Two buffers enable double-buffered partial rendering: LVGL renders the
next dirty region into buffer 2 while the flush callback copies buffer
1 to the framebuffer. Acceptable trade-off for a monitoring UI with
mostly static content at 5 Hz.

**Deferred:** Direct-to-framebuffer rendering (point LVGL at SDRAM) is
GL-O1. Tracked as an integration-time decision if tearing is visible.

---

## 6. Internal design

### 6.1 State struct

```c
/* graphics_library.c */

typedef struct {
    bool                initialised;
    lv_disp_t          *display;
    lv_indev_t         *touch_indev;
    lv_disp_drv_t       disp_drv;
    lv_indev_drv_t      indev_drv;
    lv_disp_draw_buf_t  draw_buf;
    lv_color_t          buf_1[GFX_DRAW_BUF_PIXELS];   /* 16 KB */
    lv_color_t          buf_2[GFX_DRAW_BUF_PIXELS];   /* 16 KB */
    SemaphoreHandle_t   mutex;
    StaticSemaphore_t   mutex_storage;
    TimerHandle_t       tick_timer;
    StaticTimer_t       tick_timer_storage;
} GraphicsLibraryState;

static GraphicsLibraryState s_gl;
```

All FreeRTOS objects use static allocation (P5).

### 6.2 Flush callback

Registered with LVGL during `graphics_init()`. Called by LVGL from
inside `lv_task_handler()` (i.e., `LcdUiTask` context, mutex held).

```c
static void flush_cb(lv_disp_drv_t *drv,
                     const lv_area_t *area,
                     lv_color_t *color_p)
{
    uint32_t x = (uint32_t)area->x1;
    uint32_t y = (uint32_t)area->y1;
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    (void)lcd_blit(x, y, w, h, (const uint32_t *)color_p);
    (void)lcd_flush();

    lv_disp_flush_ready(drv);
}
```

`lcd_blit` is the new LcdDriver method introduced to serve this callback
(see §6.5). For the first cut, its implementation is a synchronous
memcpy loop; this is why `lv_disp_flush_ready` is called immediately.
DMA2D acceleration is tracked as GL-O3 — when adopted, `lcd_blit`
becomes asynchronous and `lv_disp_flush_ready` moves to the
DMA-complete callback.

### 6.3 Touch input callback

Registered as `LV_INDEV_TYPE_POINTER`. Called by LVGL from inside
`lv_task_handler()`.

```c
static void touch_read_cb(lv_indev_drv_t *drv,
                          lv_indev_data_t *data)
{
    touchscreen_point_t pt;
    bool pressed = (touchscreen_driver_read(&pt) == TOUCHSCREEN_ERR_OK);

    data->point.x = (lv_coord_t)pt.x;
    data->point.y = (lv_coord_t)pt.y;
    data->state   = pressed ? LV_INDEV_STATE_PRESSED
                            : LV_INDEV_STATE_RELEASED;
    (void)drv;
}
```

LVGL handles debounce and long-press detection internally.

### 6.4 Tick callback

```c
static void tick_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    graphics_tick_increment(1U);
}
```

The timer is created with `xTimerCreateStatic` and started in
`graphics_init()`. It runs in the FreeRTOS timer-service task.
`graphics_tick_increment` calls `lv_tick_inc(1)` — LVGL documents
`lv_tick_inc` as safe to call from any context, so this function does
not take the mutex.

SysTick is owned by FreeRTOS; do not call `lv_tick_inc` from there.

### 6.5 LcdDriver refactor — new `lcd_blit` method

GraphicsLibrary requires a region copy primitive on LcdDriver. The
merged LcdDriver does not expose one (`lcd_get_framebuffer` + manual
memcpy is possible but leaks framebuffer addressing into middleware,
violating P1). LcdDriver is therefore extended with:

```c
/**
 * @brief Copy a pixel rectangle into the framebuffer.
 *
 * @param x     Left edge in pixels (0..LCD_WIDTH-1).
 * @param y     Top edge in pixels (0..LCD_HEIGHT-1).
 * @param w     Region width in pixels.
 * @param h     Region height in pixels.
 * @param src   Source buffer, ARGB8888, row-major, length w*h pixels.
 *
 * @return LCD_ERR_OK on success;
 *         LCD_ERR_STATE if lcd_init() has not been called;
 *         LCD_ERR_NULL  if src is NULL;
 *         LCD_ERR_ARG   if the region falls outside the framebuffer.
 *
 * @note Threading: task-context only. Initial implementation is
 *       synchronous (blocking memcpy loop). A future revision may
 *       use DMA2D and return immediately; in that case a separate
 *       completion callback will be added (see GL-O3).
 */
lcd_err_t lcd_blit(uint32_t x,
                   uint32_t y,
                   uint32_t w,
                   uint32_t h,
                   const uint32_t *src);
```

**First-cut implementation** (committed alongside GraphicsLibrary):

```c
lcd_err_t lcd_blit(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   const uint32_t *src)
{
    if (!s_lcd.initialised) { return LCD_ERR_STATE; }
    if (src == NULL)        { return LCD_ERR_NULL;  }
    if ((x + w) > LCD_WIDTH || (y + h) > LCD_HEIGHT) {
        return LCD_ERR_ARG;
    }

    uint32_t *fb = s_lcd.framebuffer;
    for (uint32_t row = 0U; row < h; row++) {
        uint32_t *dst = fb + ((y + row) * LCD_WIDTH) + x;
        (void)memcpy(dst, src + (row * w), w * sizeof(uint32_t));
    }
    return LCD_ERR_OK;
}
```

DMA2D acceleration is GL-O3 — when adopted, this function becomes
asynchronous and a `lcd_attach_blit_done()` callback is added so
GraphicsLibrary can defer `lv_disp_flush_ready()` until DMA completes.

This refactor lives on its own short branch
(`refactor/lcd-driver-add-blit`) merged before the GraphicsLibrary
implementation branch starts.

---

## 7. Synchronisation

GraphicsLibrary holds an internal FreeRTOS mutex created during
`graphics_init()` via `xSemaphoreCreateMutexStatic`. The contract:

| Function | Mutex behaviour |
|---|---|
| `graphics_init` | Creates the mutex; not yet taken |
| `graphics_process` | Takes mutex, runs `lv_task_handler()`, releases |
| `graphics_get_display` | Takes mutex, reads handle, releases |
| `graphics_get_indev` | Takes mutex, reads handle, releases |
| `graphics_tick_increment` | **Does NOT take the mutex** — `lv_tick_inc()` is documented safe from any context |

This makes every public function task-safe (Q-B option B locked). The
only function callable concurrently with `graphics_process()` is
`graphics_tick_increment()` from the timer-service task, which is the
intended design (the tick timer must not block on a long render cycle).

`lv_task_handler()` itself is not reentrant — the mutex guarantees no
re-entry from another task.

---

## 8. `lv_conf.h` configuration

Location: **`firmware/field-device/middleware/graphics_library/lv_conf.h`**
(GL-D4). Lives next to the wrapper so the config travels with the
component.

Required settings:

| Setting | Required value | Reason |
|---------|----------------|--------|
| `LV_COLOR_DEPTH` | `32` | ARGB8888 framebuffer (GL-D2) |
| `LV_HOR_RES_MAX` | `800` | Display width |
| `LV_VER_RES_MAX` | `480` | Display height |
| `LV_MEM_CUSTOM` | `0` | Static internal heap — no `malloc` |
| `LV_MEM_SIZE` | `≥ 32768` | 32 KB static LVGL heap; tune at integration |
| `LV_USE_LOG` | `1` | Route LVGL log to `ILogger` |
| `LV_LOG_PRINTF` | map to `log_debug(...)` | Forward via Logger |
| `LV_TICK_CUSTOM` | `0` | Tick driven by `lv_tick_inc()` from timer callback |
| `LV_USE_ANIMATION` | `1` | Required for screen transitions; disable if RAM tight |
| `LV_USE_GPU_STM32_DMA2D` | `1` | Hardware-accelerated blending (GL-O3) |
| `LV_DISP_DEF_REFR_PERIOD` | `30` | LVGL internal redraw period 30 ms |
| `LV_INDEV_DEF_READ_PERIOD` | `30` | Input poll period 30 ms |

---

## 9. Initialisation order

```
sdram_init()                  ← framebuffer memory available
lcd_init()                    ← LTDC + DSI configured; framebuffer pointer set
touchscreen_init()            ← I2C + touch controller ready
graphics_init()               ← LVGL init, callbacks registered, tick timer
                                created (timer starts running once the
                                scheduler starts)
[scheduler starts]
[LcdUiTask created]           ← calls graphics_process() every 200 ms or
                                on touch-event notify
```

GraphicsLibrary does not require two-phase init. The tick timer is
self-contained — created and started inside `graphics_init()`; it
begins firing once the FreeRTOS scheduler starts.

---

## 10. SD trace

Boot sequence reference (SD-00, see `sequence-diagrams.md`):

LcdUi calls LVGL widget API directly to construct screens. GraphicsLibrary
appears only at init time and as the implicit provider of the flush and
touch callbacks during runtime. The boot-time interaction is therefore
limited to:

| SD | Component role | Key function |
|---|---|---|
| SD-00a msg N | `LifecycleController` calls `graphics_init()` after drivers | `graphics_init()` |
| SD-00a msg N+1 | `LcdUi` retrieves the display handle for screen construction | `graphics_get_display()` |
| SD-00a msg N+2 | `LcdUi` retrieves the input device for input-group binding | `graphics_get_indev()` |

LcdUi constructs all widgets (splash screen, progress bar, operational
screens) by calling LVGL widget functions (`lv_obj_create`, `lv_label_create`,
`lv_bar_create`, etc.) directly on the display handle. GraphicsLibrary
plays no role in widget creation.

Runtime flow (per `task-breakdown.md` §4.4):

| Trigger | Path |
|---|---|
| Periodic 200 ms notify | `LcdUiTask` → `graphics_process()` → `lv_task_handler()` → `flush_cb` → `lcd_flush` |
| Touchscreen ISR notify | `LcdUiTask` → `graphics_process()` → `lv_task_handler()` → `touch_read_cb` → `touchscreen_driver_read` |
| 1 ms tick | Timer-service task → `tick_timer_cb` → `graphics_tick_increment` → `lv_tick_inc` |

---

## 11. Error and fault behaviour

All public functions return `graphics_err_t` (except `graphics_tick_increment`
which returns void by design — tick advance cannot meaningfully fail
post-init). Callers must not ignore non-OK returns.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `GRAPHICS_ERR_NOT_INIT` | Function called before `graphics_init()` succeeded | Return error; no LVGL interaction | Non-OK return | No retry — programming error; init order in §9 must be followed | LcdUi logs at ERROR via ILogger |
| `GRAPHICS_ERR_NULL_ARG` | Null pointer argument | Return error; no LVGL interaction | Non-OK return | No retry — programming error | LcdUi logs at ERROR via ILogger |
| `GRAPHICS_ERR_LVGL_FAIL` | An LVGL API call returned a failure indicator (e.g., `lv_disp_drv_register` returned NULL due to insufficient LVGL heap) | Return error; partial init state possible | Non-OK return | No retry — heap exhaustion is a design-time bug; tune `LV_MEM_SIZE` | LcdUi logs at ERROR via ILogger; `HEALTH_EVENT_SENSOR_FAIL` pushed if init fails so the boot path can fall back to a no-UI mode |

GraphicsLibrary does not own recovery policy — LcdUi decides whether a
failure is fatal or whether the device should continue without UI
(monitoring still runs via Modbus and sensor services).

---

## 12. Test plan (Pass H)

LVGL ships an SDL2-based PC simulator. We use it for visual development
and review of LcdUi screens. It is not a unit test framework — it is a
development tool. See GL-O4.

Simulator project lives under:

```
firmware/field-device/sim/        ← LVGL PC simulator entry point
  sim_display.c                   ← SDL2-based flush stub
  sim_touchscreen.c               ← SDL2 mouse-based touch stub
  main.c                          ← calls graphics_init() then constructs screens
```

The simulator is **not** part of CI. Manual verification only.

### 12.1 Host unit tests (Ceedling)

| TC ID | Scenario | Asserts |
|---|---|---|
| TC-GFX-001 | `graphics_init` on fresh state | Returns `GRAPHICS_ERR_OK`; subsequent calls to `graphics_get_display` and `graphics_get_indev` return non-NULL |
| TC-GFX-002 | `graphics_process` before init | Returns `GRAPHICS_ERR_NOT_INIT`; no LVGL stub call recorded |
| TC-GFX-003 | `graphics_get_display` before init | Returns NULL |
| TC-GFX-004 | `graphics_get_indev` before init | Returns NULL |
| TC-GFX-005 | `graphics_process` after init | Returns `GRAPHICS_ERR_OK`; LVGL stub records one `lv_task_handler` call |
| TC-GFX-006 | `graphics_tick_increment` after init | LVGL stub records `lv_tick_inc` with correct elapsed_ms |
| TC-GFX-007 | `graphics_init` when LVGL stub forces driver registration failure | Returns `GRAPHICS_ERR_LVGL_FAIL`; `s_gl.initialised` remains false |
| TC-GFX-008 | `graphics_process` exclusivity | With LVGL stub recording call count, two `graphics_process()` calls from the same task succeed; mutex contract verified via stub instrumentation |

LVGL itself is stubbed out for host unit tests (function-level stubs in
`tests/support/lvgl_stub.{h,c}`). The stub records calls to
`lv_init`, `lv_tick_inc`, `lv_task_handler`, `lv_disp_drv_register`,
`lv_indev_drv_register`, and exposes call-count globals for assertion.

The flush and touch callbacks themselves are exercised by the SDL2
simulator, not by host unit tests — wiring real LVGL render output
through a mock LcdDriver provides no useful coverage.

### 12.2 Integration test (target)

Manual verification on the F469 board, run alongside LcdUi development.
Captured in the LcdUi integration test plan, not here.

---

## 13. Principles applied

- **P1 (Strict directional layering).** Depends on LcdDriver and TouchscreenDriver (driver layer); Logger is a cross-cutting exception (P4).
- **P2 (Dependency Inversion).** GraphicsLibrary depends on the LCD and Touchscreen driver APIs (concrete C functions, per the project rule that drivers do not expose vtables).
- **P4 (Cross-cutting concern exception).** Logger referenced concretely.
- **P5 (Bounded resources, no dynamic allocation post-init).** 32 KB draw buffers in `.bss`; LVGL internal heap configured via `lv_conf.h` with `LV_MEM_CUSTOM 0` (static); FreeRTOS mutex and timer use static allocation.
- **P6 (Responsibility traces to requirements).** Trace to REQ-LD-000, REQ-LD-050, REQ-NF-108.
- **P8 (Total error propagation, no silent failures).** `graphics_err_t` on all public functions except `graphics_tick_increment` (rationale documented in §11).
- **P9 (BARR-C coding standard).** `uint32_t` for pixel data; no floating-point; static allocation only; const-correct.
- **P10 (Naming conventions).** Prefix `graphics_`; errors `GRAPHICS_ERR_*`.

### 13.1 Deviation — no vtable (GL-D8)

The project rule states middleware components expose ADT+vtable interfaces.
GraphicsLibrary deviates: it exposes direct C functions.

Rationale:
- Nothing else implements an `IGraphics` interface — there is no second
  graphics library candidate. Substitution at the boundary serves no
  purpose.
- Host unit tests stub LVGL itself, not GraphicsLibrary. The vtable's
  primary justification (test substitution) does not apply.
- The Phase 4.5 refactor (see project memory) introduces ADT+vtable
  uniformly across the codebase. GraphicsLibrary can adopt it then if
  the rule is enforced retroactively.

This deviation is recorded as a known exception. To be revisited at
Phase 4.5.

---

## 14. Open items

| ID | Item | Resolution path | Status |
|----|------|-----------------|--------|
| GL-O1 | Draw buffer strategy — confirm two 16 KB partial buffers produce acceptable visual quality at 5 Hz on the sensor screen during integration. If tearing is visible on screen transitions, switch to direct-to-framebuffer rendering with VSync gating. | Visual evaluation at LcdUi integration | Open |
| GL-O2 | Flush is synchronous (memcpy + `lcd_flush`). If profiling at integration shows the memcpy dominates `lv_task_handler` time, replace with DMA2D-async copy and signal `lv_disp_flush_ready` from the DMA-complete callback. | Profile at integration; convert if needed | Open |
| GL-O3 | DMA2D acceleration via `LV_USE_GPU_STM32_DMA2D 1` — enable and benchmark `lv_task_handler` execution time with and without. STM32F469 supports DMA2D natively. | Benchmark at integration | Open |
| GL-O4 | LVGL PC simulator — confirm SDL2 toolchain availability (WSL2 + WSLg already validated; Docker simulator runs natively per PR #41). | Already validated for LVGL standalone; confirm for the project's `sim/` entry point | Open |

---

## 15. Decisions log

| ID | Decision | Date |
|----|----------|------|
| GL-D1 | LVGL v8.3.11 LTS, pinned submodule | June 2026 |
| GL-D2 | Pixel format ARGB8888 (matches LcdDriver as merged) | June 2026 |
| GL-D3 | LVGL location `vendor/lvgl/` as Git submodule | June 2026 |
| GL-D4 | `lv_conf.h` lives in `firmware/field-device/middleware/graphics_library/` | June 2026 |
| GL-D5 | Flush implementation: memcpy + `lcd_flush()` (DMA2D blit deferred to GL-O3) | June 2026 |
| GL-D6 | SD trace reflects direct LVGL calls from LcdUi (no widget wrappers in GraphicsLibrary) | June 2026 |
| GL-D7 | Internal mutex serialises all public functions except `graphics_tick_increment` | June 2026 |
| GL-D8 | No vtable — direct C function API; deviation from middleware rule (§13.1) | June 2026 |
| GL-D9 | LcdDriver extended with `lcd_blit(x, y, w, h, src)` on a dedicated refactor branch merged before GraphicsLibrary; first-cut implementation is synchronous memcpy, DMA2D deferred to GL-O3 | June 2026 |
