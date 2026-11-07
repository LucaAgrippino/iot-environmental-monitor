# 02 · Fonts and Icons

## Font strategy

The design uses **Inter** (sans) and **JetBrains Mono** (mono) at 14 distinct sizes. Converting every size at 4 weights would push >400 KB of flash. We trim aggressively:

| Family | Weights to convert | Sizes |
|---|---|---|
| Inter | 500, 600, 700 | 11, 13, 14, 18, 22, 30 |
| JetBrains Mono | 500, 600, 700 | 10, 11, 12, 13, 14, 18, 54 |

Weight 400 is omitted — the design never uses it.

Approximate flash cost (Latin-1 only):

- Inter 500/600/700 × 6 sizes ≈ **88 KB**
- JetBrains Mono 500/600/700 × 7 sizes ≈ **96 KB**
- Total ≈ **184 KB** in flash

That fits comfortably on the STM32F469's 2 MB internal flash alongside the application binary and the two emulated-EEPROM sectors. If flash gets tight, fall back to a single weight per family (drop 500 from Inter, 500 from JBMono) — saves ≈ 60 KB.

## Conversion plan

Use `lv_font_conv` (the npm-installable converter). Drop the source `.ttf` files into `scripts/fonts/src/` and run `scripts/convert_fonts.sh`.

Example invocation for one font:

```bash
lv_font_conv \
  --font Inter-SemiBold.ttf \
  --size 18 --bpp 4 --no-compress \
  --range 0x20-0x7F,0x00B0,0x2022,0x2026,0x2190-0x2199,0x21EA \
  --format lvgl --output font_inter_sb_18.c
```

**Character ranges to include in every font:**

| Range | Glyphs | Why |
|---|---|---|
| `0x20-0x7F` | ASCII printable | All Latin text |
| `0x00B0` | `°` | Temperature unit |
| `0x00B1` | `±` | Threshold tolerance |
| `0x2022` | `•` | Status separator |
| `0x2026` | `…` | "Waiting for data…" |
| `0x2190-0x2199` | Arrows (subset) | Directional indicators |
| `0x2192` | `→` | "v2.4.1 → v2.5.0" |
| `0x21EA` | `⇪` | UPDATE pill icon (or substitute with LVGL symbol) |
| `0x2713` | `✓` | Firmware step checkmarks (or use icon font) |
| `0x2715` | `✕` | Error indicator |
| `0x25CF` | `●` | Status dot inline |
| `0x25A0` | `■` | Alarm pill |
| `0x26A0` | `⚠` | Warning prefix |
| `0x2299` | `⊙` | Optional bullet |
| `0x2010-0x2015` | Hyphens, en/em dashes | Negative values |
| `0x00D7` | `×` | (alt error) |
| `0x2192` | `→` | already listed; included again only if codepoint mode needs it |

You can also use LVGL's built-in symbol font (`LV_SYMBOL_*`) for many of these — see the icon table below — and skip them from text fonts. Pick one approach per project; mixing the two in the same string causes baseline mismatch.

## Per-token font binding

`theme.c` should expose these handles. Compile-time conditional on size availability:

```c
extern const lv_font_t font_inter_sb_30;    // FONT_TITLE_LG  (Inter SemiBold 30)
extern const lv_font_t font_inter_sb_22;    // FONT_TITLE     (Inter SemiBold 22)
extern const lv_font_t font_inter_sb_18;    // FONT_HEAD      (Inter SemiBold 18)
extern const lv_font_t font_inter_md_14;    // FONT_BODY      (Inter Medium 14)
extern const lv_font_t font_inter_md_13;    // FONT_LABEL     (Inter Medium 13)
extern const lv_font_t font_inter_md_11;    // FONT_CAPTION   (Inter Medium 11)
extern const lv_font_t font_inter_sb_11;    // FONT_TAB       (Inter SemiBold 11)
extern const lv_font_t font_inter_b_10;     // FONT_EYEBROW   (Inter Bold 10)
extern const lv_font_t font_inter_sb_9;     // FONT_TINY      (Inter SemiBold 9)

extern const lv_font_t font_jb_sb_54;       // FONT_HERO      (JBMono SemiBold 54)
extern const lv_font_t font_jb_sb_18;       // FONT_VALUE     (JBMono SemiBold 18)
extern const lv_font_t font_jb_md_14;       // FONT_BODY_MONO (JBMono Medium 14)
extern const lv_font_t font_jb_md_13;       // FONT_VALUE_SM
extern const lv_font_t font_jb_md_12;       // FONT_META
extern const lv_font_t font_jb_md_11;       // FONT_CAPTION_MONO
extern const lv_font_t font_jb_b_10;        // FONT_PILL / FONT_FOOTER
```

## Tracking (letter-spacing)

LVGL has `lv_obj_set_style_text_letter_space(obj, px, sel)`. Apply per style — these are the values from the design (px = `em × size`, rounded to nearest int):

| Style binding | letter_space (px) |
|---|---|
| `FONT_TITLE_LG`  | `-1` |
| `FONT_TITLE`     | `0` |
| `FONT_HERO`      | `-2` |
| `FONT_VALUE` / `FONT_VALUE_SM` / `FONT_BODY_MONO` | `0` |
| `FONT_CAPTION`   | `0` |
| `FONT_TAB`       | `+2` (visual 0.14 em on 11 px) |
| `FONT_PILL`      | `+2` |
| `FONT_EYEBROW`   | `+2` |
| `FONT_FOOTER`    | `+1` |
| `FONT_TINY`      | `+2` |

## Icons

The design uses 8 line icons + 8 inline glyphs.

### Line icons (24×24 viewBox, 1.6 px stroke)

Two acceptable approaches:

**A. SVG → C array, render once into a PNG sprite sheet.** Use `lv_img_dsc_t` to point at the sheet, draw with `lv_image_set_src()`. Cleanest visual result, but flash-heavy at multiple sizes.

**B. Build a small custom icon font.** Use IcoMoon or `lv_font_conv` with the PUA range `0xE000–0xE007`. Then icons inherit text color via `lv_obj_set_style_text_color`. **This is the recommended approach** — the design colors icons by status.

| PUA codepoint | Name | Source paths | Used on |
|---|---|---|---|
| `0xE000` | `sensors` | `<circle cx=12 cy=12 r=3/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M5.6 18.4l2.1-2.1M16.3 7.7l2.1-2.1"/>` | Tab bar, splash |
| `0xE001` | `status`  | `<path d="M3 12h4l3-9 4 18 3-9h4"/>` | Tab bar |
| `0xE002` | `alarms`  | `<path d="M12 2L1.5 22h21z"/><path d="M12 9v6M12 18.5h0"/>` | Tab bar |
| `0xE003` | `config`  | `<circle cx=12 cy=12 r=3/><path d=".../>` (gear, see `p-aesthetic.jsx`) | Tab bar |
| `0xE004` | `check`   | `<path d="M5 12l5 5L20 7"/>` | Alarms empty state, FW steps |
| `0xE005` | `bolt`    | `<path d="M13 2L3 14h7l-1 8 10-12h-7l1-8z"/>` | FW screen |
| `0xE006` | `shield`  | `<path d="M12 2L3 6v6c0 5 4 9 9 10 5-1 9-5 9-10V6l-9-4z"/>` | Save modal |
| `0xE007` | `x`       | `<path d="M6 6l12 12M18 6L6 18"/>` | Generic close (unused in P; keep for future) |

Render each at **24 px** for tab bar, **20 px** for modal header, **32 px** for FW hero, **42 px** for alarms-empty hero. Convert at all four sizes if using the icon-font approach.

Source SVGs are in the JSX file `p-aesthetic.jsx` — copy them out into `scripts/icons/src/*.svg` before running conversion. Trade-off: an icon font can only be one color per draw — fine for this design, since icons are always solid-colored.

### Inline glyphs

These are used as Unicode characters inside text labels; they come from the text font, not the icon font:

| Glyph | Used as | Codepoint |
|---|---|---|
| `●` | Status dot prefix (`● RUNNING`, `● VALID`) | `U+25CF` |
| `■` | Alarm pill (`■ ALARM`) | `U+25A0` |
| `⚠` | Stale prefix, banner | `U+26A0` |
| `✕` | Error prefix (`✕ ERROR`) | `U+2715` |
| `✓` | FW step done | `U+2713` |
| `⟳` | FW step in progress | `U+27F3` |
| `○` | FW step pending | `U+25CB` |
| `…` | Waiting | `U+2026` |
| `°` | °C, °F | `U+00B0` |
| `−` | Minus sign (true minus, not hyphen) | `U+2212` |
| `·` | Mid-dot separator (`POLL 5s · NEXT 3s`) | `U+00B7` |
| `→` | FW version arrow | `U+2192` |
| `⇪` | UPDATE pill (replaceable with `LV_SYMBOL_DOWNLOAD`) | `U+21EA` |

All must be included in the font conversion ranges above.

## Build script

`scripts/convert_fonts.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
SRC=scripts/fonts/src
OUT=src/assets/fonts
mkdir -p "$OUT"

RANGES="0x20-0x7F,0x00B0,0x00B1,0x00B7,0x00D7,0x2010-0x2015,0x2022,0x2026,\
0x2190-0x2199,0x21EA,0x2212,0x2299,0x2713,0x2715,0x27F3,0x25A0,0x25CB,0x25CF,\
0x26A0"

# Inter
for size in 11 13 14 18 22 30; do
  for w in Medium SemiBold Bold; do
    suffix=$(echo $w | tr 'A-Z' 'a-z' | sed 's/medium/md/;s/semibold/sb/;s/bold/b/')
    lv_font_conv \
      --font "$SRC/Inter-$w.ttf" --size $size --bpp 4 --no-compress \
      --range "$RANGES" \
      --format lvgl --lv-include lvgl.h \
      --output "$OUT/font_inter_${suffix}_${size}.c"
  done
done

# JetBrains Mono
for size in 10 11 12 13 14 18 54; do
  for w in Medium SemiBold Bold; do
    suffix=$(echo $w | tr 'A-Z' 'a-z' | sed 's/medium/md/;s/semibold/sb/;s/bold/b/')
    lv_font_conv \
      --font "$SRC/JetBrainsMono-$w.ttf" --size $size --bpp 4 --no-compress \
      --range "$RANGES" \
      --format lvgl --lv-include lvgl.h \
      --output "$OUT/font_jb_${suffix}_${size}.c"
  done
done

# Icon font
lv_font_conv \
  --font "$SRC/envmon-icons.ttf" --size 24 --bpp 4 --no-compress \
  --range 0xE000-0xE007 \
  --format lvgl --lv-include lvgl.h \
  --output "$OUT/font_icons_24.c"
```

Drop unneeded sizes/weights to shrink flash; nothing in the screens spec assumes weights outside this table.
