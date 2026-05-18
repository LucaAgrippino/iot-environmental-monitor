# LcdDriver — LLD Companion

**Document:** `docs/lld/drivers/lcd-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Draft

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Renders application-supplied frames to the LCD, owning the framebuffer in external SDRAM | `components.md` |
| PROVIDES (upward) | `ILcd` | `components.md` |
| USES (downward) | `SdramDriver`, CMSIS | `components.md` |
| Root requirements | REQ-LD-010, REQ-NF-108 | `components.md`, `SRS.md` |
| Hardware | MIPI DSI host, LTDC, DMA2D, 4-inch 800×480 LCD panel | UM1932 §4.14 |

**Consumer:** `GraphicsLibrary` (middleware, LVGL wrapper). Called from `LcdUiTask` (priority 2, 200 ms period).

**ISR surface (task-breakdown.md §6.1):**
- `DSI_LTDC_IRQHandler` — frame-flush done → `LcdUiTask` → `«notify»`

**REQ-NF-108:** 5 Hz display refresh rate (200 ms between frames).

**Hardware specifics (UM1932 §4.14):**
- LCD module: 4-inch 800×480 TFT, capacitive touch, MIPI DSI 2-lane
- DSI clock lanes: `DSI_D0_N/P`, `DSI_D0_P`, `DSI_CK_N/P`, `DSI_D1_N/P`
- Panel/touchscreen reset: PH7 (active low, resets both LCD and FT6206)
- Tearing Effect input: PJ2 (synchronises framebuffer writes with LCD scan)
- Backlight: controlled via DSI commands + CABC signal (default configuration)

---

## 2. Public API

### 2.1 Dependency-conformance check

`lcd_driver.h` includes `sdram_driver.h` (for `SDRAM_ERR_OK` — actually not needed; only `stdint.h` and `stdbool.h`). The SdramDriver is called from `lcd_driver.c` to get the framebuffer base address. The header `lcd_driver.h` includes only `stdint.h` and `stdbool.h`. No FreeRTOS in the header. Confirmed clean.

### 2.2 Two-phase init

The frame-done callback (`DSI_LTDC_IRQHandler`) notifies `LcdUiTask`. Per established convention, the callback is registered after the task exists. `lcd_init()` configures hardware and enables the display; `lcd_attach_frame_done()` registers the callback and enables the LTDC line interrupt. Called from `LcdUiTask`'s startup prologue.

### 2.3 Data types

```c
typedef enum {
    LCD_ERR_OK      = 0,
    LCD_ERR_TIMEOUT = 1, /**< DSI busy or LTDC flag did not clear. */
} lcd_err_t;

/**
 * @brief Frame-done callback. Called from ISR context after LTDC signals
 * frame transfer complete. Consumer calls xTaskNotifyFromISR internally.
 */
typedef void (*lcd_frame_done_cb_t)(void *context);
```

### 2.4 Public API (`lcd_driver.h`)

```c
/**
 * @brief Initialise DSI host, LTDC, DMA2D, and the LCD panel.
 *
 * Sequence:
 *   1. Call sdram_init() is a pre-condition — must have been called first.
 *   2. Assert reset on PH7 (resets both LCD panel and FT6206).
 *   3. Configure DSI host (PLL, lane count, video mode parameters).
 *   4. Configure LTDC (resolution 800×480, pixel format RGB565, layer
 *      pointing to framebuffer in SDRAM).
 *   5. Release reset on PH7.
 *   6. Send DSI initialisation commands to the LCD panel IC.
 *   7. Enable LTDC; enable backlight.
 *
 * Does NOT enable the LTDC line interrupt — that is done in
 * lcd_attach_frame_done(), after LcdUiTask exists.
 *
 * @return LCD_ERR_OK on success; LCD_ERR_TIMEOUT on DSI or LTDC error.
 */
lcd_err_t lcd_init(void);

/**
 * @brief Register the frame-done callback and enable the LTDC interrupt.
 *
 * Called from LcdUiTask startup prologue (after scheduler starts).
 * The callback is invoked from DSI_LTDC_IRQHandler when one frame has
 * been transferred to the panel. LVGL's flush_cb calls lcd_flush() to
 * start a transfer, then waits for the callback to signal completion.
 *
 * @param callback  Function called from ISR on frame done. Must not be NULL.
 * @param context   Opaque pointer passed to the callback.
 */
void lcd_attach_frame_done(lcd_frame_done_cb_t callback, void *context);

/**
 * @brief Return a pointer to the framebuffer in SDRAM.
 *
 * LVGL (via GraphicsLibrary) writes rendered pixels directly into this
 * buffer. The buffer is 800 × 480 × 2 bytes = 768,000 bytes, RGB565
 * format, row-major.
 *
 * @return Pointer to the framebuffer base address.
 */
uint16_t *lcd_get_framebuffer(void);

/**
 * @brief Signal LTDC to latch the current framebuffer contents and
 *        transfer them to the panel on the next TE pulse.
 *
 * Non-blocking. The frame-done callback fires when the transfer is
 * complete. LVGL calls this at the end of its flush_cb.
 *
 * @return LCD_ERR_OK always (the request is queued in hardware).
 */
lcd_err_t lcd_flush(void);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
static uint16_t         *s_framebuffer    = NULL; /* set at init from sdram_get_base_addr() */
static lcd_frame_done_cb_t s_frame_done_cb = NULL;
static void              *s_frame_done_ctx = NULL;
static bool               s_initialised   = false;
```

The framebuffer pointer is set once at `lcd_init()` by calling `sdram_get_base_addr()`. It is immutable thereafter. No heap allocation.

### 3.2 Peripheral chain: LTDC → DSI → panel

The STM32F469 LCD path is: LTDC (generates digital RGB + sync signals) → DSI host (serialises to MIPI DSI) → LCD panel IC (drives the physical display). Key points:

- **LTDC** reads the framebuffer from SDRAM via AHB and generates the pixel stream. The framebuffer is a 800×480 array of `uint16_t` in RGB565 format.
- **DSI host** operates in **Video Mode** (continuous stream, TE-synchronised). The LTDC acts as the timing master; the DSI host wraps the pixel data in DSI packets.
- **TE (Tearing Effect) pin PJ2**: the LCD panel asserts TE during the vertical blanking interval. Configuring LTDC to wait for TE before latching a new frame (via LTDC_SRCR shadow reload) eliminates visible tearing at 5 Hz (REQ-NF-108).
- **DMA2D**: the Chrom-Art accelerator can fill and copy regions of the framebuffer in hardware. LcdDriver exposes `lcd_get_framebuffer()` so GraphicsLibrary/LVGL can use DMA2D directly if it chooses. LcdDriver itself does not call DMA2D — that is GraphicsLibrary's concern.

### 3.3 Frame flush sequence

```
1. GraphicsLibrary (LVGL) renders into the framebuffer via lcd_get_framebuffer().
2. GraphicsLibrary calls lcd_flush().
3. lcd_flush() sets LTDC_SRCR.VBR = 1 (shadow reload on next vertical blank).
4. LTDC waits for the TE pulse on PJ2, then latches the new layer settings.
5. DSI host transmits the frame to the panel.
6. LTDC asserts the line interrupt (LTDC_IER.LIE) when the reload is complete.
7. DSI_LTDC_IRQHandler fires → calls s_frame_done_cb(s_frame_done_ctx).
8. GraphicsLibrary callback calls xTaskNotifyFromISR → LcdUiTask unblocks.
9. LVGL marks the flush complete; the next render cycle can begin.
```

### 3.4 Reset pin PH7 (shared with TouchscreenDriver)

PH7 resets both the LCD panel and the FT6206 touchscreen controller. LcdDriver asserts and releases PH7 during `lcd_init()`. **TouchscreenDriver init must be called after lcd_init()** — the touchscreen controller is guaranteed to be out of reset only after LcdDriver has completed its init sequence. This ordering is a pre-condition of `touchscreen_init()`.

LcdDriver configures PH7 directly via CMSIS (GPIO mode output, push-pull). It does not call GpioDriver — PH7 is a non-bus control line owned by LcdDriver, and `components.md` shows LcdDriver USES CMSIS directly for this.

### 3.5 No FreeRTOS in driver

The frame-done callback (`s_frame_done_cb`) is called from `DSI_LTDC_IRQHandler`. GraphicsLibrary wires it to `xTaskNotifyFromISR`. The driver holds only a function pointer — no FreeRTOS primitive.

---

## 4. Hardware contract

### 4.1 DSI configuration (open items — LCDD-O1, LCDD-O2)

DSI PLL configuration (DSIHOST_WRPCR) determines the bit clock and lane data rate. The values depend on the panel's required pixel clock (800×480 at 5 Hz requires a very low pixel clock; at 60 Hz for smooth LVGL it requires ~29 MHz pixel clock). For 5 Hz (REQ-NF-108) this is trivial, but LVGL renders at full speed and uses the TE mechanism to limit panel updates. Verify lane data rate against panel IC datasheet. Tracked as **LCDD-O1**.

### 4.2 LCD panel IC (open item — LCDD-O2)

The LCD panel IC (DSI display driver IC, e.g. OTM8009A) requires specific DSI command-mode initialisation before video mode starts. These init commands are vendor-specific and must be sent via the DSI host in command mode during `lcd_init()`. Tracked as **LCDD-O2** — verify panel IC part number against UM1932 and obtain the initialisation sequence.

### 4.3 Framebuffer parameters

| Parameter | Value |
|---|---|
| Resolution | 800 × 480 pixels |
| Pixel format | RGB565 (2 bytes/pixel) |
| Framebuffer size | 768,000 bytes = 750 KB |
| Location | SDRAM base address (from `sdram_get_base_addr()`) |
| Single / double buffer | Single (LCDD-O3 — double-buffering deferred to integration) |

### 4.4 LTDC timing (open item — LCDD-O4)

LTDC timing registers (SSCR, BPCR, AWCR, TWCR) encode the horizontal/vertical sync, back-porch, active, and total dimensions for the panel. Values come from the panel datasheet. Tracked as **LCDD-O4**.

---

## 5. Sequence integration

`LcdDriver` is not an explicit lifeline in any HLD sequence diagram. Its ISR (`DSI_LTDC_IRQHandler`) is listed in task-breakdown.md §6.1. No SD changes required.

---

## 6. Error and fault behaviour

On `LCD_ERR_TIMEOUT` from `lcd_init()`, the BringingUpLCD Init sub-state fails → Faulted. The LCD is essential (REQ-LD-000). Failure is non-recoverable at boot.

---

## 7. Unit-test plan

The DSI and LTDC peripherals cannot be realistically mocked on a host. Primary verification is integration on hardware.

| ID | Test case | Expected result |
|---|---|---|
| T-LCD-01 | `lcd_init` with mocked SdramDriver returning valid base address | DSI and LTDC configured; no timeout; s_framebuffer set |
| T-LCD-02 | `lcd_get_framebuffer` returns non-NULL pointer after init | Matches sdram_get_base_addr() |
| T-LCD-03 | `lcd_attach_frame_done`: callback pointer stored; LTDC line interrupt enabled | ISR vector table checked; callback stored |
| T-LCD-04 | `lcd_flush`: LTDC_SRCR.VBR set | Register write verified via mock |
| T-LCD-05 | `DSI_LTDC_IRQHandler` fires: callback invoked with correct context | Mock callback called once |
| T-LCD-06 | Integration: display visible content on panel | Verified visually on hardware |

Test file: `tests/drivers/test_lcd_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| LCDD-O1 | DSI PLL and lane data rate configuration. Requires panel IC datasheet for required bit clock. | Luca | Resolve with LCDD-O2 |
| LCDD-O2 | LCD panel IC identity and DSI command-mode init sequence. Verify part number from UM1932; obtain initialisation sequence from manufacturer. OTM8009A is a common choice for this board. | Luca | Check UM1932 at implementation |
| LCDD-O3 | Double-buffering decision. Single buffer is simpler but may produce tearing on fast content. TE + LTDC shadow reload mitigates this at 5 Hz. Evaluate at integration. | Luca | Defer to integration |
| LCDD-O4 | LTDC timing register values (SSCR, BPCR, AWCR, TWCR). Derive from panel datasheet active area, sync, and porch timings. | Luca | Resolve with LCDD-O2 |
| LCDD-O5 | DMA2D use by GraphicsLibrary. LcdDriver does not call DMA2D, but exposes the framebuffer pointer. LVGL can optionally use DMA2D for accelerated fill/copy. Confirm LVGL DMA2D configuration is applied in GraphicsLibrary companion. | GraphicsLibrary LLD | Out of scope for this companion |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| LCDD-D1 | LTDC in single-layer, single-buffer mode initially | Simplest correct implementation; double-buffering and overlay layers are optimisations deferred to integration |
| LCDD-D2 | TE synchronisation via PJ2 | Eliminates visible tearing without software timing hacks; hardware-guaranteed at the panel's blanking interval |
| LCDD-D3 | PH7 reset pin configured directly by LcdDriver via CMSIS, not through GpioDriver | PH7 is an LCD-subsystem control line; ownership belongs to LcdDriver per USES list. It is not a shared GPIO resource |
| LCDD-D4 | lcd_flush() non-blocking, frame-done via callback | Allows LcdUiTask to do other work (touch event processing) while the frame is being transferred; avoids a busy-wait in the task |
| LCDD-D5 | Singleton — no handle | One DSI+LTDC+panel combination; consistent with all prior companions |
