#!/usr/bin/env python3
"""Generate the LVGL v8 font files of the Field Device LCD UI.

The design (docs/lcd-ui-design/_reference-v9-claude-design/02_FONTS_AND_ICONS.md) calls for Inter
and JetBrains Mono at fixed sizes plus a small line-icon font. The pack's tool, lv_font_conv, needs
Node, which this project does not use, so this script renders the glyphs with Pillow (FreeType) and
writes the same uncompressed 4 bpp lv_font_fmt_txt format that lv_font_conv would.

usage: gen-lvgl-fonts.py --src <dir with the TTFs> --out <output dir> [--chrome <browser>]

  Inter-Medium/SemiBold/Bold.ttf and JetBrainsMono-Medium/SemiBold/Bold.ttf must be under --src
  (any depth). Both families are SIL OFL 1.1; their licences are kept in
  docs/lcd-ui-design/fonts/. The icon glyphs are rendered from the SVG paths of the design source
  (p-aesthetic.jsx) with headless Chrome, so the icons stay the design's own artwork.

The output folder receives one C file per font, fonts.h, and nothing else. Regenerate only when the
token table below changes; the generated files are committed.
"""

import argparse
import io
import os
import subprocess
import sys
import tempfile

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

# --- Token table: (C name, family file stem, size px) ---------------------------------------------
# 02_FONTS_AND_ICONS.md "Per-token font binding". Names are the ones theme.c binds.
TEXT_FONTS = [
    ("font_inter_sb_30", "Inter-SemiBold", 30),  # FONT_TITLE_LG
    ("font_inter_sb_22", "Inter-SemiBold", 22),  # FONT_TITLE
    ("font_inter_sb_18", "Inter-SemiBold", 18),  # FONT_HEAD
    ("font_inter_md_14", "Inter-Medium", 14),  # FONT_BODY
    ("font_inter_sb_14", "Inter-SemiBold", 14),  # alarm row label, button text (weight 600)
    ("font_inter_md_13", "Inter-Medium", 13),  # FONT_LABEL
    ("font_inter_md_11", "Inter-Medium", 11),  # FONT_CAPTION
    ("font_inter_sb_11", "Inter-SemiBold", 11),  # FONT_TAB
    ("font_inter_b_10", "Inter-Bold", 10),  # FONT_EYEBROW
    ("font_inter_sb_9", "Inter-SemiBold", 9),  # FONT_TINY
    ("font_jb_sb_54", "JetBrainsMono-SemiBold", 54),  # FONT_HERO
    ("font_jb_sb_18", "JetBrainsMono-SemiBold", 18),  # FONT_VALUE
    ("font_jb_md_14", "JetBrainsMono-Medium", 14),  # FONT_BODY_MONO
    ("font_jb_md_13", "JetBrainsMono-Medium", 13),  # FONT_VALUE_SM
    ("font_jb_md_12", "JetBrainsMono-Medium", 12),  # FONT_META
    ("font_jb_md_11", "JetBrainsMono-Medium", 11),  # FONT_CAPTION_MONO
    ("font_jb_b_10", "JetBrainsMono-Bold", 10),  # FONT_PILL / FONT_FOOTER
]

# 02_FONTS_AND_ICONS.md "Character ranges". 0x2212 is the true minus sign.
RANGES = [(0x20, 0x7E), (0xB0, 0xB1), (0xB7, 0xB7), (0xD7, 0xD7), (0x2010, 0x2015), (0x2022, 0x2022),
          (0x2026, 0x2026), (0x2190, 0x2199), (0x21EA, 0x21EA), (0x2212, 0x2212), (0x2299, 0x2299),
          (0x2713, 0x2713), (0x2715, 0x2715), (0x27F3, 0x27F3), (0x25A0, 0x25A0), (0x25CB, 0x25CB),
          (0x25CF, 0x25CF), (0x26A0, 0x26A0)]

# Icon font: PUA codepoints from 02_FONTS_AND_ICONS.md (0xE008 trend is new, SRS v1.2 REQ-LD-000).
ICON_SVG = {
    0xE000: '<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M5.6 18.4l2.1-2.1M16.3 7.7l2.1-2.1"/>',
    0xE001: '<path d="M3 12h4l3-9 4 18 3-9h4"/>',
    0xE002: '<path d="M12 2L1.5 22h21z"/><path d="M12 9v6M12 18.5h0"/>',
    0xE003: '<circle cx="12" cy="12" r="3"/><path d="M19.4 12c0 .4-.1.8-.1 1.2l2 1.5-2 3.5-2.4-.9c-.6.5-1.4.9-2.1 1.2l-.4 2.5h-4l-.4-2.5c-.7-.3-1.5-.7-2.1-1.2l-2.4.9-2-3.5 2-1.5c-.1-.4-.1-.8-.1-1.2s0-.8.1-1.2l-2-1.5 2-3.5 2.4.9c.6-.5 1.4-.9 2.1-1.2l.4-2.5h4l.4 2.5c.7.3 1.5.7 2.1 1.2l2.4-.9 2 3.5-2 1.5c.1.4.1.8.1 1.2z"/>',
    0xE004: '<path d="M5 12l5 5L20 7"/>',
    0xE005: '<path d="M13 2L3 14h7l-1 8 10-12h-7l1-8z"/>',
    0xE006: '<path d="M12 2L3 6v6c0 5 4 9 9 10 5-1 9-5 9-10V6l-9-4z"/>',
    0xE007: '<path d="M6 6l12 12M18 6L6 18"/>',
    0xE008: '<polyline points="3,17 8,11 12,14 17,7 21,4"/><line x1="2" y1="20" x2="22" y2="20"/>',
}
# Sizes: 24 tab bar, 20 modal header, 32 firmware hero, 42 alarms-empty hero.
ICON_SIZES = [20, 24, 32, 42]
ICON_STROKE = 1.6  # 02_FONTS_AND_ICONS.md "24x24 viewBox, 1.6 px stroke"


def find_ttf(src, stem):
    for root, _dirs, files in os.walk(src):
        if stem + ".ttf" in files and "/static/" not in root.replace("\\", "/") + "/":
            return os.path.join(root, stem + ".ttf")
    for root, _dirs, files in os.walk(src):
        if stem + ".ttf" in files:
            return os.path.join(root, stem + ".ttf")
    raise SystemExit(f"{stem}.ttf not found under {src}")


def codepoints_in(path):
    cmap = TTFont(path).getBestCmap()
    wanted = []
    for lo, hi in RANGES:
        wanted.extend(cp for cp in range(lo, hi + 1) if cp in cmap)
    # LVGL's sparse cmap is searched by bisection: the table must be sorted and free of duplicates.
    return sorted(set(wanted))


def render_text_glyphs(path, size, cps):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for cp in cps:
        ch = chr(cp)
        adv16 = int(round(font.getlength(ch) * 16))
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        w, h = right - left, bottom - top
        if w <= 0 or h <= 0:
            glyphs.append({"cp": cp, "adv": adv16, "w": 0, "h": 0, "ox": 0, "oy": 0, "px": b""})
            continue
        img = Image.new("L", (w + 4, h + 4), 0)
        ImageDraw.Draw(img).text((2 - left, 2 - top), ch, font=font, fill=255, anchor="ls")
        img = img.crop((2, 2, 2 + w, 2 + h))
        glyphs.append({"cp": cp, "adv": adv16, "w": w, "h": h, "ox": left, "oy": -bottom,
                       "px": bytes((v * 15 + 127) // 255 for v in img.tobytes())})
    return glyphs, ascent + descent, descent


def render_icon_glyphs(chrome, size):
    glyphs = []
    for cp, body in sorted(ICON_SVG.items()):
        html = ('<html><body style="margin:0;background:transparent"><svg xmlns="http://www.w3.org/2000/svg" '
                f'width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" stroke="#fff" '
                f'stroke-width="{ICON_STROKE}" stroke-linecap="round" stroke-linejoin="round">{body}</svg></body></html>')
        with tempfile.TemporaryDirectory() as tmp:
            page, shot = os.path.join(tmp, "i.html"), os.path.join(tmp, "i.png")
            open(page, "w").write(html)
            subprocess.run([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                            "--default-background-color=00000000", f"--window-size={size},{size}",
                            f"--screenshot={shot}", "file://" + page], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            img = Image.open(shot).convert("RGBA").crop((0, 0, size, size))
        alpha = img.getchannel("A")
        glyphs.append({"cp": cp, "adv": size * 16, "w": size, "h": size, "ox": 0, "oy": 0,
                       "px": bytes((v * 15 + 127) // 255 for v in alpha.tobytes())})
    return glyphs, size, 0


def pack4(px):
    out = bytearray()
    for i in range(0, len(px), 2):
        hi = px[i]
        lo = px[i + 1] if i + 1 < len(px) else 0
        out.append((hi << 4) | lo)
    return bytes(out)


def emit(name, glyphs, line_height, base_line, fp):
    bitmap = bytearray()
    for g in glyphs:
        g["index"] = len(bitmap)
        bitmap += pack4(g["px"])
    first, last = glyphs[0]["cp"], glyphs[-1]["cp"]
    guard = name.upper()
    w = fp.write
    w("/* GENERATED by scripts/gen-lvgl-fonts.py -- do not hand-edit. */\n")
    w('#include "lvgl.h"\n\n')
    w(f"#ifndef {guard}\n#define {guard} 1\n#endif\n\n#if {guard}\n\n")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
    for i in range(0, len(bitmap), 16):
        w("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16]) + ",\n")
    if not bitmap:
        w("    0x00,\n")
    w("};\n\n")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* id 0: reserved */\n")
    for g in glyphs:
        w(f"    {{.bitmap_index = {g['index']}, .adv_w = {g['adv']}, .box_w = {g['w']}, .box_h = {g['h']}, "
          f".ofs_x = {g['ox']}, .ofs_y = {g['oy']}}}, /* U+{g['cp']:04X} */\n")
    w("};\n\n")
    w("static const uint16_t unicode_list[] = {\n")
    offs = [g["cp"] - first for g in glyphs]
    for i in range(0, len(offs), 12):
        w("    " + ", ".join(f"0x{o:x}" for o in offs[i:i + 12]) + ",\n")
    w("};\n\n")
    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    w(f"    {{.range_start = {first}, .range_length = {last - first + 1}, .glyph_id_start = 1,\n"
      f"     .unicode_list = unicode_list, .glyph_id_ofs_list = NULL, .list_length = {len(glyphs)},\n"
      "     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},\n};\n\n")
    w("static lv_font_fmt_txt_glyph_cache_t cache;\n\n")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
      "    .glyph_bitmap = glyph_bitmap, .glyph_dsc = glyph_dsc, .cmaps = cmaps, .kern_dsc = NULL,\n"
      "    .kern_scale = 0, .cmap_num = 1, .bpp = 4, .kern_classes = 0, .bitmap_format = 0,\n"
      "    .cache = &cache};\n\n")
    w(f"const lv_font_t {name} = {{\n"
      "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
      "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
      f"    .line_height = {line_height},\n    .base_line = {base_line},\n"
      "    .subpx = LV_FONT_SUBPX_NONE,\n    .underline_position = -1,\n    .underline_thickness = 1,\n"
      "    .dsc = &font_dsc};\n\n")
    w(f"#endif /* {guard} */\n")
    return len(bitmap)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--chrome", default="google-chrome")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    decls, total = [], 0
    for name, stem, size in TEXT_FONTS:
        path = find_ttf(args.src, stem)
        glyphs, lh, bl = render_text_glyphs(path, size, codepoints_in(path))
        with open(os.path.join(args.out, name + ".c"), "w") as fp:
            n = emit(name, glyphs, lh, bl, fp)
        total += n
        decls.append(name)
        print(f"{name}: {len(glyphs)} glyphs, {n} bytes")
    for size in ICON_SIZES:
        name = f"font_icons_{size}"
        glyphs, lh, bl = render_icon_glyphs(args.chrome, size)
        with open(os.path.join(args.out, name + ".c"), "w") as fp:
            n = emit(name, glyphs, lh, bl, fp)
        total += n
        decls.append(name)
        print(f"{name}: {len(glyphs)} glyphs, {n} bytes")
    with open(os.path.join(args.out, "fonts.h"), "w") as fp:
        fp.write("/**\n * @file fonts.h\n * @brief Declarations of the fonts compiled into the display firmware.\n */\n"
                 "/* GENERATED by scripts/gen-lvgl-fonts.py -- do not hand-edit. */\n"
                 "#ifndef FONTS_H\n#define FONTS_H\n\n#ifdef TEST\n#include \"lvgl_stub.h\"\n#else\n"
                 "#include \"lvgl.h\"\n#endif\n\n")
        for d in decls:
            fp.write(f"extern const lv_font_t {d};\n")
        fp.write("\n/* Icon font codepoints (02_FONTS_AND_ICONS.md; 0xE008 is the trend icon of SRS v1.2). */\n")
        for cp, nm in ((0xE000, "SENSORS"), (0xE001, "STATUS"), (0xE002, "ALARMS"), (0xE003, "CONFIG"),
                       (0xE004, "CHECK"), (0xE005, "BOLT"), (0xE006, "SHIELD"), (0xE007, "X"), (0xE008, "TREND")):
            utf8 = chr(cp).encode("utf-8")
            fp.write(f'#define ICON_{nm} "{"".join(chr(92) + "x%02x" % b for b in utf8)}" /* U+{cp:04X} */\n')
        fp.write("\n#endif /* FONTS_H */\n")
    print(f"total bitmap data: {total} bytes")


if __name__ == "__main__":
    sys.exit(main())
