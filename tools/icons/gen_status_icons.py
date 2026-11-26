#!/usr/bin/env python3
"""Status bar and quick panel icons -> A8 bitmaps for the firmware.

    python3 tools/icons/gen_status_icons.py

Two sets, same names and order (main/display/icons/status_icons.h):
  - main/display/icons/status_icons.c        our own drawings (the ones in sim/design.js); committed.
  - main/display/icons/local/status_icons.c  only when sim/local/icons.js exists (icons the user exported
                                             for personal use); main/display/icons/local/ is git-ignored and
                                             CMake prefers it when it is there.

Each icon is drawn black on white by resvg at its final pixel size and the darkness kept as alpha, so the
same bitmap is smooth on gray pages (quick panel) and is cut at half alpha on ordinary 1-bit pages.
Parts that are "off" (Wi-Fi arcs, 4G bars above the signal level) are kept as a 50 % checkerboard, which
reads as gray on the 1-bit status bar.

Needs: resvg (brew install resvg), Pillow, numpy.
"""
import json
import math
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SIM = os.path.join(ROOT, "sim")
OUT_DIR = os.path.join(ROOT, "main", "display", "icons")
FONT_BOLD = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans", "MiSans-Bold.ttf")

# Order = enum order in status_icons.h. Keep the two in step.
NAMES = [
    "SB_WIFI_0", "SB_WIFI_1", "SB_WIFI_2", "SB_WIFI_3",
    "SB_CELL_0", "SB_CELL_1", "SB_CELL_2", "SB_CELL_3", "SB_CELL_4",
    "SB_PLANE", "SB_PROXY", "SB_PROXY_SLEEP", "SB_MUTE", "SB_BOLT",
    "DIGIT_0", "DIGIT_1", "DIGIT_2", "DIGIT_3", "DIGIT_4",
    "DIGIT_5", "DIGIT_6", "DIGIT_7", "DIGIT_8", "DIGIT_9",
    "Q_WIFI", "Q_CELL", "Q_PROXY",
    "ROW_BULB_OFF", "ROW_BULB_MIN", "ROW_BULB_MAX",
    "ROW_THERMO_COLD", "ROW_THERMO_MID", "ROW_THERMO_WARM",
    "SEL_QUEST", "SEL_BOOK", "SEL_SENTENCE", "SEL_MIC",
]

SB_H = 27          # Wi-Fi / 4G / plane height in the status bar (px); others keep their size relative to Wi-Fi
DIGIT_CAP = 13     # battery digits: height of a digit (px)
BOLT_H = 12        # charging bolt next to the digits (px)
TILE_H = 32        # Wi-Fi height in a quick panel tile (px)
ROW_K = 0.3655     # quick panel rows, SF icons: px per SF unit (sim RK 0.172 x 2.125)
ROW_BOX = (-106, 117)  # SF rows: same vertical box for every row icon so their baselines line up
SEL_H = 34         # reader selection card: icons in the bottom row (sim 16 px x 2.125)
SEL_Q_H = 28       # reader selection card: the question mark in front of each preset question (sim 13 px)


def render(inner, viewbox, width, height):
    """inner: SVG elements, black = ink, white = cut. Returns alpha (H x W uint8)."""
    x, y, w, h = viewbox
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="{x} {y} {w} {h}">'
           f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="#fff"/>{inner}</svg>')
    with tempfile.TemporaryDirectory() as tmp:
        src, png = os.path.join(tmp, "i.svg"), os.path.join(tmp, "i.png")
        open(src, "w").write(svg)
        subprocess.run(["resvg", src, png], check=True)
        lum = np.asarray(Image.open(png).convert("L"), dtype=np.int32)
    return (255 - lum).astype(np.uint8)


def crop_x(alpha):
    cols = np.where(alpha.max(axis=0) > 8)[0]
    return alpha[:, cols[0]:cols[-1] + 1] if len(cols) else alpha


def crop(alpha):
    rows = np.where(alpha.max(axis=1) > 8)[0]
    return crop_x(alpha[rows[0]:rows[-1] + 1] if len(rows) else alpha)


def checker(alpha):
    yy, xx = np.indices(alpha.shape)
    return np.where((xx + yy) % 2 == 0, alpha, 0).astype(np.uint8)


def levels(layers, viewbox, scale, count):
    """layers: SVG strings, innermost / lowest first. Returns `count + 1`... images for 0..count lit."""
    x, y, w, h = viewbox
    W, H = max(1, round(w * scale)), max(1, round(h * scale))
    parts = [render(fill_black(l), viewbox, W, H) for l in layers]
    out = []
    for lit in range(len(layers) + 1):
        on = np.zeros((H, W), np.uint8)
        off = np.zeros((H, W), np.uint8)
        for i, p in enumerate(parts):
            if i < lit: on = np.maximum(on, p)
            else: off = np.maximum(off, p)
        out.append(np.maximum(on, checker(off)))
    # crop all to the full icon's box so the icon does not move between levels
    full = np.maximum.reduce(parts)
    rows = np.where(full.max(axis=1) > 8)[0]; cols = np.where(full.max(axis=0) > 8)[0]
    return [o[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1] for o in out]


def fill_black(svg):
    return svg.replace('fill="currentColor"', 'fill="#000"').replace('stroke="currentColor"', 'stroke="#000"')


def path_bbox(d):
    nums = [float(n) for n in re.findall(r"-?\d*\.?\d+(?:e-?\d+)?", d)]
    xs, ys = nums[0::2], nums[1::2]
    return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)


def digit(ch):
    font = ImageFont.truetype(FONT_BOLD, 100)
    img = Image.new("L", (160, 160), 0)
    ImageDraw.Draw(img).text((20, 10), ch, fill=255, font=font)
    big = crop(np.asarray(img))
    h = DIGIT_CAP
    w = max(1, round(big.shape[1] * h / big.shape[0]))
    return np.asarray(Image.fromarray(big).resize((w, h), Image.LANCZOS))


# ---------- our own drawings (sim/design.js V2_ICONS) ----------
def own_icons():
    js = open(os.path.join(SIM, "design.js")).read()
    icon = {m.group(1): m.group(2) for m in re.finditer(r"^  (\w+): '(.*)',$", js, re.M)}

    def stroke(name, width):
        return f'<g fill="none" stroke="#000" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round">{icon[name]}</g>'

    def line_icon(name, box, height, px):
        k = height / box[3]
        return crop(render(stroke(name, px / k), box, round(box[2] * k), height))

    wifi = re.findall(r"<path[^>]*/>", icon["wifi"])        # wedge, middle, outer
    cell = re.findall(r"<rect[^>]*/>", icon["cell"])        # shortest first
    cell = [f'<g fill="#000">{r}</g>' for r in cell]
    out = {}
    for i, a in enumerate(levels(wifi, (0, 0, 21.31, 15.07), SB_H / 15.07, 3)): out[f"SB_WIFI_{i}"] = a
    for i, a in enumerate(levels(cell, (0, 0, 19.59, 12), SB_H / 12, 4)): out[f"SB_CELL_{i}"] = a
    out["SB_PLANE"] = line_icon("plane", (5, 3.5, 16.5, 17.4), SB_H - 3, 3)
    out["SB_PROXY"] = line_icon("proxy", (3, 1.6, 18, 20.7), SB_H, 2.8)
    # 网络睡下时盾牌跟灰扇形一样用网点，不要实心黑
    out["SB_PROXY_SLEEP"] = checker(out["SB_PROXY"])
    out["SB_MUTE"] = line_icon("mute", (3, 4.5, 19.5, 15), SB_H - 4, 2.8)
    out["SB_BOLT"] = crop(render('<path fill="#000" d="M3.9 0 .2 6.2h2.6L2.3 11 6.2 4.6H3.6z"/>', (0, 0, 6.4, 11), round(6.4 * BOLT_H / 11), BOLT_H))
    out["Q_WIFI"] = crop(render(fill_black("".join(wifi)), (0, 0, 21.31, 15.07), round(21.31 * TILE_H / 15.07), TILE_H))
    out["Q_CELL"] = crop(render("".join(cell), (0, 0, 19.59, 12), round(19.59 * TILE_H / 12), TILE_H))
    out["Q_PROXY"] = line_icon("proxy", (3, 1.6, 18, 20.7), 38, 3.4)
    for key, name in (("ROW_BULB_OFF", "sunOff"), ("ROW_BULB_MIN", "sun"), ("ROW_BULB_MAX", "sun")):
        out[key] = crop_x(render(stroke(name, 1.9), (0, 0, 24, 24), 36, 36))
    for key in ("ROW_THERMO_COLD", "ROW_THERMO_MID", "ROW_THERMO_WARM"):
        out[key] = crop_x(render(stroke("thermo", 1.9), (0, 0, 24, 24), 36, 36))
    # 正文选字卡片（sim/design.js 的 v2SelPop）：整格 24 的图，线宽同模拟界面（16 px 时 1.33 px）
    out["SEL_QUEST"] = render(stroke("quest", 2.0), (0, 0, 24, 24), SEL_Q_H, SEL_Q_H)
    for key, name in (("SEL_BOOK", "book"), ("SEL_SENTENCE", "sentence"), ("SEL_MIC", "mic")):
        out[key] = render(stroke(name, 2.0), (0, 0, 24, 24), SEL_H, SEL_H)
    return out


# ---------- icons exported by the user (sim/local/icons.js, not in git) ----------
def local_icons():
    src = open(os.path.join(SIM, "local", "icons.js")).read()
    data = json.loads(src[src.index("=") + 1:].strip().rstrip(";"))
    k_sb = SB_H / 63.09    # SF units -> px: Wi-Fi is SB_H high

    def box(v):
        return tuple(float(n) for n in v["box"].split())

    def layers_of(v):
        # every <path> is one layer; erase paths (class="solid erase") cut what is under them
        return [p.replace('class="solid erase"', 'fill="#fff"') for p in re.findall(r"<path[^>]*/>", v["svg"])]

    def whole(v):
        return fill_black(v["svg"].replace('class="solid erase"', 'fill="#fff"'))

    out = {}
    wifi = layers_of(data["wifi"])[::-1]          # exported outer first; want wedge first
    for i, a in enumerate(levels(wifi, box(data["wifi"]), k_sb, 3)): out[f"SB_WIFI_{i}"] = a
    cell = layers_of(data["cell"])[::-1]          # exported tallest first; want shortest first
    for i, a in enumerate(levels(cell, box(data["cell"]), k_sb, 4)): out[f"SB_CELL_{i}"] = a
    pb = box(data["plane"]); k = SB_H / pb[3]
    out["SB_PLANE"] = crop(render(whole(data["plane"]), pb, round(pb[2] * k), SB_H))
    xb = box(data["proxy"])
    out["SB_PROXY"] = crop(render(whole(data["proxy"]), xb, round(xb[2] * k_sb), round(xb[3] * k_sb)))
    out["SB_PROXY_SLEEP"] = checker(out["SB_PROXY"])

    def row(name, k=ROW_K):
        v = data[name]
        vb = (v["x"], ROW_BOX[0], v["w"], ROW_BOX[1])
        inner = v["row"].replace('class="erase"', 'fill="#fff"').replace("<path d", '<path fill="#000" d')
        return crop_x(render(inner, vb, round(v["w"] * k), round(ROW_BOX[1] * k)))

    mute = row("speaker.slash", (SB_H - 3) / 82.92)
    out["SB_MUTE"] = crop(mute)
    bolt = data["bolt"]; bb = box(bolt)
    out["SB_BOLT"] = crop(render(f'<path fill="#000" d="{bolt["d"]}"/>', bb, round(bb[2] * BOLT_H / bb[3]), BOLT_H))
    k_tile = TILE_H / 63.09
    for key, name in (("Q_WIFI", "wifi"), ("Q_CELL", "cell"), ("Q_PROXY", "proxy")):
        b = box(data[name])
        out[key] = crop(render(whole(data[name]), b, round(b[2] * k_tile), round(b[3] * k_tile)))
    for key, name in (("ROW_BULB_OFF", "lightbulb.slash"), ("ROW_BULB_MIN", "lightbulb.min"),
                      ("ROW_BULB_MAX", "lightbulb.max"), ("ROW_THERMO_COLD", "thermometer.snowflake"),
                      ("ROW_THERMO_MID", "thermometer.variable"), ("ROW_THERMO_WARM", "thermometer.sun")):
        out[key] = row(name)
    return out


def emit(icons, path, note):
    for d in "0123456789":
        icons[f"DIGIT_{d}"] = digit(d)
    missing = [n for n in NAMES if n not in icons]
    if missing: sys.exit(f"missing icons: {missing}")
    lines = [f"// Generated by tools/icons/gen_status_icons.py ({note}). Do not edit.",
             '#include "status_icons.h"', ""]
    for n in NAMES:
        a = icons[n]
        h, w = a.shape
        body = ",".join(str(int(v)) for v in a.flatten())
        lines.append(f"static const uint8_t k{n}[{w * h}] = {{{body}}};")
    lines += ["", "const lv_image_dsc_t valet_icons[VALET_ICON_COUNT] = {"]
    for n in NAMES:
        h, w = icons[n].shape
        lines.append(f"    {{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = {w}, .h = {h}, "
                     f".stride = {w}}}, .data_size = {w * h}, .data = k{n}}},  // {n}")
    lines.append("};")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write("\n".join(lines) + "\n")
    sizes = ", ".join(f"{n}={icons[n].shape[1]}x{icons[n].shape[0]}" for n in NAMES if not n.startswith("DIGIT"))
    print(f"{os.path.relpath(path, ROOT)}: {sizes}")


def preview(icons, path):
    """All icons on one sheet, 1-bit as the status bar shows them (top) and gray (bottom), 4x."""
    pad, row_h = 6, 50
    width = sum(icons[n].shape[1] + pad for n in NAMES) + pad
    sheet = Image.new("L", (width, row_h * 2), 255)
    x = pad
    for n in NAMES:
        a = icons[n]
        gray = Image.fromarray(255 - a)
        bw = Image.fromarray(np.where(a > 127, 0, 255).astype(np.uint8))
        sheet.paste(bw, (x, 4)); sheet.paste(gray, (x, row_h + 4))
        x += a.shape[1] + pad
    sheet.resize((width * 4, row_h * 8), Image.NEAREST).save(path)


if __name__ == "__main__":
    own = own_icons()
    emit(own, os.path.join(OUT_DIR, "status_icons.c"), "our own drawings, sim/design.js")
    if "--preview" in sys.argv: preview(own, sys.argv[sys.argv.index("--preview") + 1] + "-own.png")
    if os.path.exists(os.path.join(SIM, "local", "icons.js")):
        loc = local_icons()
        for name in NAMES:          # icons the user did not export: use our own drawing
            if name not in loc and name in own: loc[name] = own[name]
        emit(loc, os.path.join(OUT_DIR, "local", "status_icons.c"), "sim/local/icons.js, personal use, not in git")
        if "--preview" in sys.argv: preview(loc, sys.argv[sys.argv.index("--preview") + 1] + "-local.png")
