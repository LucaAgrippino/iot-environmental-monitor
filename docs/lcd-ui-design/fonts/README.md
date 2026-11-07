# Fonts of the LCD UI

| Family | Weights used | Licence | Source |
|---|---|---|---|
| Inter 4.0 | Medium, SemiBold, Bold | SIL Open Font License 1.1 (`LICENSE-Inter.txt`) | <https://github.com/rsms/inter/releases/tag/v4.0> |
| JetBrains Mono 2.304 | Medium, SemiBold, Bold | SIL Open Font License 1.1 (`OFL-JetBrainsMono.txt`) | <https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304> |

The TrueType files are not stored in the repository. The generated LVGL font files are:
`firmware/field-device/middleware/graphics_library/fonts/*.c` (committed, 190 KB of glyph data).

To regenerate after changing the size table in `scripts/gen-lvgl-fonts.py`:

```sh
python3 scripts/gen-lvgl-fonts.py --src <dir with the unpacked releases> \
    --out firmware/field-device/middleware/graphics_library/fonts
clang-format-17 -i firmware/field-device/middleware/graphics_library/fonts/*.[ch]
```

The icon glyphs (U+E000..E008) are the SVG paths of the design source (`p-aesthetic.jsx`, plus the
trend icon of `sensor-trend.html`), rendered with headless Chrome.
