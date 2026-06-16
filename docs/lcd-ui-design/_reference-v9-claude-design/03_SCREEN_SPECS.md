# 03 · Screen Specs

All 13 screens. Each section gives: purpose, anatomy, exact measurements, and the data inputs it expects. Open `design_reference/Field Device LCD - P + T.html` and look at the P (top) row alongside this.

**Conventions**

- Coordinates are `x, y, w, h` from the top-left of the 800×480 canvas.
- All text colors and fonts reference tokens from `01_DESIGN_TOKENS.md`.
- Sensor IDs `S1 / S2 / S3` are the default trio (Temperature / Humidity / Pressure). They are configurable but the layout is fixed at 3 columns.

---

## Common chrome

Most screens (3–13) share three bars. Build these once as reusable widgets.

### Tab bar — `tab_bar.c`

- Position `0, 0, 800, 64`. Fill `COL_HEADER_BG`, bottom border 1 px `COL_BORDER`.
- 4 equal columns of 200 px, separated by 1 px `COL_BORDER` vertical dividers.
- Active column has top 3 px stripe `COL_ACCENT` flush to top edge, and background `rgba(COL_ACCENT, 10%)` → pre-blend as `#13202B`.
- Each column: icon (24 px, centered horizontally, `COL_ACCENT` if active else `#888888`) above label (`FONT_TAB`, `COL_INK` if active else `#888888`). Gap 4 px between icon and label. Centered vertically.
- Labels: `SENSORS`, `STATUS`, `ALARMS`, `CONFIG` (all caps, baked into the text).
- Touch: each column is a button covering the full 200×64 area.

### Header bar — `header.c`

- Position `0, 64, 800, 40`. Fill `COL_BG`, bottom border 1 px `COL_BORDER`.
- Left: title text (`FONT_EYEBROW`, `COL_INK`), inset 22 px from left, vertically centered.
- Right: 14 px gap row of `[ time, status pill ]`, inset 22 px from right, vertically centered.
  - Time: `FONT_META`, `COL_MUTED`, format `HH:MM:SS`.
  - Status pill: see Status Pill component below.

### Footer bar — `footer.c`

- Position `0, 444, 800, 36`. Fill `COL_HEADER_BG`, top border 1 px `COL_BORDER`.
- Left and right text: `FONT_CAPTION_MONO`, `COL_MUTED`, inset 22 px.
- Contents differ per screen; see each screen below.

### Content area

- `0, 104, 800, 340` (between header and footer).

---

## Status pill — `status_pill.c`

A pill chip used everywhere a system state is shown. Bordered, not filled-flat.

| State key | Label | Color (border + text) | Background |
|---|---|---|---|
| `running` | `● RUNNING` | `COL_OK` | `COL_OK_TINT` |
| `init`    | `● INIT`    | `COL_ACCENT` | `COL_ACCENT_TINT` |
| `alarm`   | `■ ALARM`   | `COL_ERR` (animate 1 Hz blink) | `COL_ERR_TINT` |
| `update`  | `⇪ UPDATE`  | `COL_ACCENT` | `COL_ACCENT_TINT` |

Padding `3 px / 10 px`. Font `FONT_PILL`. Border 1 px. Rectangular, no radius.

---

## Sensor card — `sensor_card.c`

Used 3-up on the Sensors screen. Each card: 245 × 280 (approx — the grid is `1fr 1fr 1fr` with 14 px gap inside 22 px screen padding → cards land at 244 px wide). Background `COL_SURF`, 1 px border `COL_BORDER`.

Anatomy (top → bottom inside `padding: 16 18`):

1. **Status stripe** — 4 px wide, full-height, absolute on the left edge, color = status color (OK/WARN/ERR/MUTED for waiting).
2. **Label** — `FONT_EYEBROW`, `COL_MUTED`, UPPERCASE. Examples: `TEMPERATURE · S1`, `HUMIDITY · S2`, `PRESSURE · S3`. Margin-bottom 14 px.
3. **Value row** — baseline-aligned flex:
   - Number: `FONT_HERO` (Mono 54 SemiBold), color = value color (see below), 8 px right gap to unit.
   - Unit: `FONT_BODY_MONO`, `COL_MUTED`. Examples: `°C`, `%RH`, `hPa`.
   - **Waiting variant**: replace the whole row with `Waiting for data…` in `FONT_VALUE` (Mono 18), `COL_MUTED`.
   - Row min-height: 54 px so the baseline doesn't jump.
4. **Sparkline** — 200 × 34 SVG region, draw as an LVGL canvas or `lv_line` (see `04_LVGL_PATTERNS.md`):
   - Polyline path (normalized, will scale): `0,22 20,20 40,24 60,18 80,21 100,16 120,14 140,18 160,12 180,14 200,10`.
   - Stroke 1.5 px in sparkline color (status color, or `COL_DIM` if faded).
   - Filled area beneath, 12% opacity of sparkline color — skip on `dashed`.
   - `dashed = true` for stale; stroke dash `3 3`.
   - Margin-top 14 px.
5. **Footer row** — flex justify-between, `FONT_CAPTION_MONO`:
   - Left: status text + time, in status color. Format: `● VALID · 12:34:56`. Other states: `⚠ STALE · 12:31:14`, `✕ ERROR · 12:30:02`, `… WAITING`, `■ ALARM`.
   - Right: delta string, `COL_MUTED`. Format: `+0.3 / 5m`, `−0.4 / 5m`, or `—` when no data.
   - Margin-top 14 px.

Value color resolution:

```
if faded         → COL_DIM
else if stale    → COL_WARN
else if error    → COL_ERR
else if alarm    → COL_ERR
else             → COL_INK
```

---

## 01 · Splash (boot)

**Use case:** First screen on power-up. Shown while LVGL + drivers initialize and the first sensor poll is in flight.

- Full-screen, no tab/header/footer, `COL_BG` fill.
- Center column, vertically centered:
  - **Brand row** (gap 14, margin-bottom 30):
    - Logo: 56×56 SVG (panel-like glyph from `p-aesthetic.jsx PSplash`). Stroke 2.4 px in `COL_ACCENT`, with one accent square at `(13,20)` and an inline ECG sparkline in `COL_OK`.
    - Right of logo: `ENVMON` (`FONT_TITLE_LG`, `COL_INK`), and `ENVIRONMENTAL GATEWAY` (`FONT_FOOTER`, `COL_MUTED`, 2 px top margin).
  - **Progress bar** (420 px wide, 20 px top margin):
    - Track: 4 px tall, `COL_SURF` fill, 1 px `COL_BORDER`.
    - Fill: animated 0% → 100% over boot, gradient `COL_ACCENT → #5C9FC7` (or solid `COL_ACCENT` if gradient drawing is expensive).
    - Below: flex justify-between, `FONT_CAPTION_MONO`. Left = current step label (`COL_INK`), right = percentage (`COL_MUTED`).
- **Top-right**: small `BOOT` annotation, 8 px ⌀ accent dot + label, `FONT_CAPTION_MONO`, `COL_MUTED`.
- **Bottom strip** (24 px inset): justify-between row, `FONT_FOOTER`, `COL_MUTED`:
  - Left: `FW v2.4.1 · build 20260426 · LVGL 9.2`
  - Right: `STM32F469 · 800×480` — replace with actual target string at compile time.

Stay on this screen ≥ 1.5 s. Exit when (a) first valid sensor reading received OR (b) 5 s timeout — whichever first.

---

## 02 · Firmware update

**Use case:** OTA update in progress. User cannot navigate away.

- No tab bar (`noTabs=true` style). Header bar only, title = `FIRMWARE UPDATE`, status pill = `update` (`⇪ UPDATING`, `COL_ACCENT`).
- Content area, top-padding 36, centered column:
  - **Hero row** (gap 18, margin-bottom 30):
    - 32 px bolt icon, `COL_ACCENT`.
    - Right: `Updating firmware` in `FONT_TITLE`, `COL_INK`; below, `FONT_CAPTION_MONO` `COL_MUTED`: `Do not power off the device`.
  - **Stage band** (540 px max-width):
    - Top row: justify-between, `FONT_CAPTION_MONO`. Left = `Writing partition · stage 3 of 5` (`COL_INK`), right = `47% · 4.2 MB / 9.0 MB` (`COL_MUTED`).
    - Progress bar: 14 px tall, `COL_SURF` fill, 1 px `COL_BORDER`. Inner fill: gradient `COL_ACCENT → #5C9FC7`. Overlay: 45° hatch lines (4 px transparent, 4 px white@4%) for the moving effect — animate by shifting `bg_grad_dir` or by `lv_anim` on a small offset.
  - **Version cards** (margin-top 24, two-up grid, 14 px gap):
    - Each: `COL_SURF` fill, 1 px border. Padding `12 16`. Width = (540 − 14) / 2 ≈ 263 px.
    - Left card: eyebrow `CURRENT` (`FONT_EYEBROW`, `COL_MUTED`), value `v2.4.1` (`FONT_VALUE`, `COL_INK`), sub `build 20260426` (`FONT_CAPTION_MONO`, `COL_MUTED`).
    - Right card: border `COL_ACCENT`. Left 3 px stripe `COL_ACCENT`. Eyebrow `INSTALLING` in `COL_ACCENT`. Value `v2.5.0`. Sub `build 20260509`.
  - **Step list** (margin-top 24, `FONT_CAPTION_MONO`, line-height 1.7):
    ```
    ✓ Verified signature · SHA-256 match
    ✓ Erased target partition · 9.0 MB
    ⟳ Writing image · ~2 min remaining
    ○ Verify checksum
    ○ Reboot & commit
    ```
    Glyph colors: `✓` `COL_OK`, `⟳` `COL_ACCENT`, `○` `COL_DIM`. Text color = `COL_MUTED` (done/pending) or `COL_INK` (active line).

No footer bar on this screen.

---

## 03 · Sensors · Normal

**Use case `REQ-LD-010..030`:** Steady-state reading view.

- Tab bar (Sensors active), header bar (title `SENSOR READINGS`, pill `running`).
- Content `padding: 22`, 3-column grid `1fr 1fr 1fr` gap 14.
- Three sensor cards, all `status='valid'`:

| Card | Label | Value | Unit | Delta | Spark color |
|---|---|---|---|---|---|
| 1 | `TEMPERATURE · S1` | `23.4` | `°C` | `+0.3 / 5m` | `COL_ACCENT` |
| 2 | `HUMIDITY · S2`    | `58.2` | `%RH` | `−0.4 / 5m` | `COL_ACCENT` |
| 3 | `PRESSURE · S3`    | `1013` | `hPa` | `+0.1 / 5m` | `COL_ACCENT` |

- Footer: left `POLL 5s · NEXT 3s`, right `STM32F469 · LVGL · 800×480` (substitute actual target).

---

## 04 · Sensors · Waiting

**Use case `REQ-LD-040`:** Polling but no data received yet for one or more channels. Shown briefly after boot and after reset.

- Same layout as 03.
- All three cards in `waiting` state (or just the affected channels — design shows all-3 waiting):
  - Stripe: `COL_DIM`.
  - Value row: `Waiting for data…` in `FONT_VALUE`, `COL_MUTED`.
  - No sparkline.
  - Footer row: `… WAITING` (`COL_MUTED`) · time. Right: `—`.
- Header pill: `init` (`● INIT`, `COL_ACCENT`).
- Footer: left `POLL 5s · NEXT 5s`, right `Acquiring…`.

---

## 05 · Sensors · Stale

**Use case `REQ-LD-050`:** A channel hasn't refreshed within tolerance window.

- Cards 1 and 2: same as Normal (valid).
- Card 3 (`PRESSURE · S3`):
  - Stripe: `COL_WARN`.
  - Value `1013` in default ink (not warn-colored — value is still readable; only stripe + status text + sparkline change).
  - Sparkline: stroked in `COL_WARN`, **dashed** (3 3), no fill area.
  - Footer status: `⚠ STALE · 12:31:14` in `COL_WARN`. Delta: `—`.
- Header pill remains `running`.
- Footer: standard.

---

## 06 · Sensors · Error

**Use case `REQ-LD-060`:** A channel reported a bus error or invalid CRC.

- Card 1 (`TEMPERATURE · S1`) errored:
  - Stripe: `COL_ERR`.
  - Value `23.4` rendered in `COL_DIM` (`faded=true`) — value is stale.
  - Sparkline stroked in `COL_ERR`, faded (`COL_DIM`).
  - Footer status: `✕ ERROR · last 12:30:02` in `COL_ERR`. Delta column is `last 12:30:02` from the design data; the design moves this label into the status string. Use `last 12:30:02` as the spec.
- Cards 2, 3: Normal.
- Header pill: `running` (the system is up; one sensor failed).
- Footer: standard.

---

## 07 · Status · Healthy

**Use case `REQ-LD-070`:** System health overview.

- Tab bar (Status active), header bar (title `SYSTEM STATUS`, pill `running`).
- Content `padding: 18 22 50`, 2 columns × `1fr 1fr` with 36 px column-gap.
- Each column is a stack of section eyebrow + rows.

**Left column:**

```
COMPUTE
CPU load                34%
Free heap               48.2 KB
Sensor task watermark   312 B
MCU temperature         51.2 °C

CONNECTIVITY
WiFi RSSI               −62 dBm
Reconnections (24h)     0
MQTT failures           0
```

**Right column:**

```
MODBUS
Success count           14 829
CRC errors              0
Timeouts                0
Buffer occupancy        22%

DEVICE
Uptime                  3d 14:22
Firmware                v2.4.1
Last config save        3d 14:18 ago
```

Each section:

- Eyebrow `FONT_EYEBROW`, `COL_MUTED`, margin-bottom 6.
- Rows: see Status Row component below.
- 14 px vertical gap between sections.

### Status row — `status_row.c`

- Height ~33 px, flex justify-between, padding `9 0`, bottom border 1 px `COL_BORDER`.
- Left: 10 px ⌀ status dot (color by status) · 10 px gap · label (`FONT_LABEL`, `COL_INK`).
- Right: value (`FONT_VALUE_SM`, color = `COL_INK` if ok else status color).
- ERR-state dot gets a soft outer glow: draw a second circle of ⌀14 at 40% opacity behind it (or skip on RGB565 if it bands).

**Footer:** left `Refreshes every 1s`, right `All systems nominal`.

---

## 08 · Status · Degraded

**Use case:** Soft fault — system still running but one or more subsystems are warning.

- Same layout. Two changes:
  - **Banner**: margin `14 22 0`, padding `10 14`, background `rgba(COL_ERR, 12%)` pre-blended, border 1 px `COL_ERR`, text `COL_ERR`, `FONT_BODY` weight 600. Flex justify-between:
    - Left: `⚠ MQTT publish failures detected — broker unreachable for 4m 12s`
    - Right: `12:30:44` (`FONT_VALUE_SM` mono).
  - Status rows that flip:
    - `Sensor task watermark` → status `warn` (value `312 B` unchanged in design).
    - `Reconnections (24h)` → `8`, status `warn`.
    - `MQTT failures` → `47`, status `err`.
    - `CRC errors` → `12`, status `warn`.
    - `Timeouts` → `4`, status `warn`.
  - Header pill: `alarm` (`■ ALARM`, `COL_ERR`, blinking).
  - Footer right: `1 active alert`.

---

## 09 · Alarms · Empty

**Use case `REQ-LD-090`:** No active alarms.

- Tab bar (Alarms active), header (title `ACTIVE ALARMS`, pill `running`).
- Content centered, vertically and horizontally:
  - 78 px ⌀ circle, fill `COL_OK_TINT`, centered `check` icon at 42 px in `COL_OK`.
  - Below (gap 18): `No active alarms` (`FONT_HEAD`, `COL_INK`).
  - Below (gap 18): `All sensors are within configured thresholds.` (`FONT_CAPTION_MONO`, `COL_MUTED`).
  - Margin-top 14: `Last alarm cleared 2d 4h ago` (`FONT_CAPTION_MONO`, `COL_MUTED`).
- Footer: left `0 active · 0 acknowledged`, right `Auto-refresh 1s`.

---

## 10 · Alarms · Active

**Use case `REQ-LD-080`:** One or more alarms triggered.

- Tab bar (Alarms active), header pill: `alarm` (blink).
- Content padding 18, vertical stack of alarm rows.

### Alarm row — `alarm_card.c`

Each: `COL_SURF` fill, 1 px `COL_BORDER`, left 5 px stripe in severity color. Padding `14 18`. Grid `2fr 1fr 1fr 1fr` with 14 px col gap, items vertically centered. Margin-bottom 10.

| Cell | Content |
|---|---|
| **1 — Label** | Sensor name (`FONT_BODY` 600, `COL_INK`) + direction tag (`HIGH` / `LOW`, severity color, weight 700, 6 px left margin). Below: `Triggered HH:MM:SS` (`FONT_CAPTION_MONO`, `COL_MUTED`). |
| **2 — Current** | Eyebrow `CURRENT` (`FONT_TINY`, `COL_MUTED`). Value below (`FONT_VALUE` mono 18 600, severity color). |
| **3 — Threshold** | Eyebrow `THRESHOLD` (`FONT_TINY`, `COL_MUTED`). Value below (`FONT_VALUE` mono 18 500, `COL_INK`). |
| **4 — Pill** | Right-aligned `● ACTIVE` pill, severity color. Padding `4 10`. |

Demo data:

```
[err]  Temperature · S1   HIGH   42.7 °C   40.0 °C   ● ACTIVE
       Triggered 12:18:07

[warn] Pressure · S3      LOW    982 hPa   990 hPa   ● ACTIVE
       Triggered 11:54:31
```

Footer: left `2 active · 0 acknowledged`, right `Auto-refresh 1s`.

---

## 11 · Config · Idle

**Use case `REQ-LD-100..130`:** Configuration view with pending edits but no validation error.

- Tab bar (Config active), header (title `CONFIGURATION`, pill `running`).
- Content `padding: 20 22 60`, 3-column grid `1fr 1fr 1fr` with 22 px gap. Each column is a section:

**Column 1 · Acquisition:**

```
Poll interval         5000 ms      hint: 100 – 60000
Sensor bus rate       400 kHz      hint: 100 / 400 kHz
```

**Column 2 · Alarm thresholds:**

```
Temp HIGH (°C)        40.0         hint: −20 to 80
Temp LOW (°C)         −5.0         hint: −20 to 80
Hum HIGH (%RH)        90           hint: 0 to 100
```

**Column 3 · Display:**

```
Backlight             78%          hint: 10 – 100
Theme                 Dark · Operator
Idle dim after        60 s
```

- Section eyebrow: `FONT_EYEBROW`, `COL_ACCENT` (yes — accent here, not muted; this is the visual cue for an editable section). Margin-bottom 14.
- Each field: see Config Field component below. Margin-bottom 14.

### Config field — `field.c`

- Top: flex justify-between (margin-bottom 5): label (`FONT_CAPTION`, `COL_MUTED`) and hint (`FONT_FOOTER`, `COL_DIM`).
- Input box:
  - Background `COL_SURF2`, 1 px border (`COL_BORDER` default, `COL_ACCENT` if focused, `COL_ERR` if error).
  - Padding `9 12`. No border radius.
  - Value text: `FONT_BODY_MONO`, `COL_INK` (`COL_ERR` if invalid).
  - When focused: caret `|` at the right side of the field, `COL_ACCENT`, blinking 1 Hz.
- If error: below the input, `FONT_CAPTION_MONO`, `COL_ERR`, 4 px top margin. Example: `Out of allowed range (−20 to 80)`.

**Footer:** absolute, padding `10 22`. Flex justify-between, items vertically centered.

- Left text: `FONT_CAPTION_MONO`, `COL_MUTED` (or `COL_ERR` if invalid state): `Changes pending · 4 fields modified`.
- Right buttons (gap 10):
  - `CANCEL` — transparent fill, 1 px `COL_BORDER`, text `COL_MUTED`, padding `8 18`, `FONT_PILL`.
  - `APPLY` — fill `COL_ACCENT`, 1 px `COL_ACCENT`, text `#06080B`, padding `8 18`, `FONT_PILL`. Disabled style: fill `COL_SURF2`, border `COL_BORDER`, text `COL_DIM`.

---

## 12 · Config · Validation error

**Use case `REQ-LD-140`:** One or more fields out of range.

- Same as 11 with these deltas:
  - `Temp HIGH (°C)` field: value `95.0`, border `COL_ERR`, value text `COL_ERR`. Below the input: error message `Out of allowed range (−20 to 80)`.
  - Footer left text: `1 invalid field` in `COL_ERR`.
  - Apply button: disabled style.
  - Focus does NOT shift to Backlight; the Backlight focus state shown in screen 11 is hidden here (caret off).

---

## 13 · Config · Confirm modal

**Use case `REQ-LD-150`:** User pressed Apply with valid changes; confirm before flash write.

- Render screen 11 underneath, dimmed with full-screen overlay `rgba(0,0,0,60%)`.
- Modal centered, 380 px wide, height auto. Background `COL_SURF`, 1 px `COL_BORDER`, padding 24. No radius.
- Header row (gap 12, margin-bottom 14): 20 px shield icon in `COL_ACCENT` + `Save changes to flash?` (`FONT_HEAD`, `COL_INK`).
- Body: `FONT_BODY` (14, 500), `COL_MUTED`, line-height 1.6. Margin-bottom 18:
  > 4 configuration fields will be written to persistent storage. Acquisition will pause briefly during write.
- Diff block: background `COL_BG`, 1 px `COL_BORDER`, padding `10 12`, margin-bottom 18, `FONT_CAPTION_MONO` `COL_MUTED`. Use a monospace grid (each line same width):
  ```
  Poll interval   1000 → 5000 ms
  Temp HIGH       35.0 → 40.0 °C
  Hum HIGH        85   → 90 %RH
  Backlight       60   → 78 %
  ```
- Footer row (gap 10, justify end):
  - `CANCEL` button — same as screen 11 cancel, but text `COL_INK`.
  - `CONFIRM & SAVE` — same shape as Apply.

Touch outside the modal does **not** dismiss — user must press a button.

---

## Cross-screen reminders

- Time string (`12:34:56`) is generated; everywhere it appears it's the same source. Synthesize from device RTC; in simulator, use real clock.
- The footer-right hardware annotation (`STM32F469 · 800×480`) should be `#define`-driven so it matches actual target.
- When a sensor count differs from 3 (future hardware), the Sensors screen layout assumes 3. Document this constraint in `screen_sensors.c`. Adding sensor #4+ is a v2 change requiring scroll or a second page.
