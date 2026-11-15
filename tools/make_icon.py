#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Draws Kyujo's application icon (assets/icon.jpg) from scratch.

What the picture shows
    A floodlit battle arena at night, seen from a raised seat: a striped turf oval inside a ring of stands, a glowing hexagonal
    pad at the centre, a trainer box at each end, and four floodlight towers whose beams cross over the pad. It is meant to say
    "kyujo" (the Japanese word for a ball park or arena: the project's name) without borrowing from any game, console maker or trademark.

What it avoids (on purpose)
    No ball, no circle split by a band, no creature, no character, no lettering, no logo or colour scheme of a game or a
    company. Everything is made of ellipses, polygons and a seeded scatter of dots, so anyone can read the program and see
    exactly what is in the picture. No photograph or imported picture is used.

Output
    assets/icon.jpg: 256 x 256, baseline (not progressive) JPEG, the format the Switch homebrew packaging step expects.

Use
    python3 tools/make_icon.py              # rewrites assets/icon.jpg
    python3 tools/make_icon.py out.png      # writes a preview in any format Pillow knows

Needs Pillow and numpy. The drawing is made at 4x size and scaled down, which smooths the edges.
"""
import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

FINAL = 256
SCALE = 4
S = FINAL * SCALE


def px(v):
    """Icon units (0..256) to working-canvas pixels."""
    return v * SCALE


def pw(v):
    """Line widths must be whole pixels."""
    return max(1, int(round(v * SCALE)))


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def sky(img):
    """Night sky: deep indigo at the top, a blue glow near the stands."""
    top, glow = (8, 12, 40), (44, 86, 168)
    ys = np.linspace(0.0, 1.0, S)[:, None]
    t = np.clip(ys / 0.55, 0.0, 1.0) ** 1.4
    arr = np.zeros((S, S, 3), dtype=np.float32)
    for c in range(3):
        arr[:, :, c] = top[c] + (glow[c] - top[c]) * t
    return Image.fromarray(arr.astype(np.uint8), "RGB")


def ellipse_box(cx, cy, rx, ry):
    return [px(cx - rx), px(cy - ry), px(cx + rx), px(cy + ry)]


def hexagon(cx, cy, r, squash):
    """Flat hexagon seen at an angle: the vertical radius is r * squash."""
    return [(px(cx + r * math.cos(math.radians(a))), px(cy + r * squash * math.sin(math.radians(a)))) for a in range(0, 360, 60)]


def draw_stands(img, rng):
    """Two concentric elliptical walls; the band between them is filled with a scatter of lit seats."""
    d = ImageDraw.Draw(img)
    cx, cy = 128, 150
    d.ellipse(ellipse_box(cx, cy, 124, 72), fill=(22, 30, 70))
    # seats: warm dots on rings that get smaller towards the field
    for ring in range(7):
        rx = 122 - ring * 4.2
        ry = 70 - ring * 2.4
        count = int(70 + ring * 8)
        for i in range(count):
            a = 2 * math.pi * i / count + rng.uniform(-0.03, 0.03)
            x = cx + rx * math.cos(a)
            y = cy + ry * math.sin(a)
            if y < cy - 8 and abs(math.cos(a)) < 0.12:
                continue
            col = rng.choice([(255, 214, 120), (255, 160, 90), (240, 240, 255), (120, 200, 255), (60, 70, 120), (60, 70, 120)])
            r = rng.uniform(0.9, 1.6)
            d.ellipse([px(x - r), px(y - r), px(x + r), px(y + r)], fill=col)
    d.ellipse(ellipse_box(cx, cy + 6, 96, 52), fill=(18, 24, 56))   # inner wall shadow


def draw_turf(img):
    """Striped turf oval, white boundary line, trainer boxes."""
    cx, cy = 128, 160
    rx, ry = 92, 48
    turf = Image.new("RGB", (S, S), (0, 0, 0))
    td = ImageDraw.Draw(turf)
    stripe_w = 14
    for i, x in enumerate(range(int(cx - rx), int(cx + rx) + stripe_w, stripe_w)):
        col = (34, 150, 112) if i % 2 == 0 else (26, 128, 98)
        td.rectangle([px(x), px(cy - ry), px(x + stripe_w), px(cy + ry)], fill=col)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse(ellipse_box(cx, cy, rx, ry), fill=255)
    img.paste(turf, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse(ellipse_box(cx, cy, rx, ry), outline=(235, 245, 250), width=pw(1.6))
    d.ellipse(ellipse_box(cx, cy, rx - 5, ry - 3), outline=(150, 220, 200), width=pw(0.7))
    # trainer boxes at both ends
    for sign in (-1, 1):
        bx = cx + sign * (rx - 26)
        box = [(bx - 9, cy - 7), (bx + 9, cy - 7), (bx + 11, cy + 8), (bx - 11, cy + 8)]
        d.polygon([(px(x), px(y)) for x, y in box], outline=(235, 245, 250), width=pw(1.1))


def draw_pad(img):
    """Hexagonal pad with a gold rim and a glowing core, plus soft light on the turf."""
    cx, cy = 128, 160
    glow = Image.new("RGB", (S, S), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse(ellipse_box(cx, cy, 46, 22), fill=(70, 190, 230))
    glow = glow.filter(ImageFilter.GaussianBlur(px(9)))
    img.paste(ImageChops.add(img, glow, scale=1.6, offset=0))
    d = ImageDraw.Draw(img)
    d.polygon(hexagon(cx, cy, 33, 0.46), fill=(214, 168, 70))
    d.polygon(hexagon(cx, cy, 29, 0.46), fill=(30, 52, 104))
    d.polygon(hexagon(cx, cy, 21, 0.46), outline=(120, 230, 255), width=pw(1.2))
    d.polygon(hexagon(cx, cy, 11, 0.46), fill=(150, 240, 255))
    d.polygon(hexagon(cx, cy, 6, 0.46), fill=(255, 255, 255))


def draw_towers_and_beams(img):
    """Four floodlight towers at the corners of the stands; translucent beams cross over the pad."""
    towers = [(26, 70), (230, 70), (54, 40), (202, 40)]
    target = (128, 158)
    beams = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beams)
    for tx, ty in towers:
        dx, dy = target[0] - tx, target[1] - ty
        length = math.hypot(dx, dy)
        nx, ny = -dy / length, dx / length
        w0, w1 = 3.0, 22.0
        pts = [(tx + nx * w0, ty + ny * w0), (tx - nx * w0, ty - ny * w0),
               (target[0] - nx * w1, target[1] - ny * w1), (target[0] + nx * w1, target[1] + ny * w1)]
        bd.polygon([(px(x), px(y)) for x, y in pts], fill=(220, 240, 255, 46))
    beams = beams.filter(ImageFilter.GaussianBlur(px(1.2)))
    img.paste(beams, (0, 0), beams)
    d = ImageDraw.Draw(img)
    for tx, ty in towers:
        d.rectangle([px(tx - 1.2), px(ty), px(tx + 1.2), px(ty + 46)], fill=(16, 20, 44))
        d.rounded_rectangle([px(tx - 7), px(ty - 5), px(tx + 7), px(ty + 3)], radius=px(1.5), fill=(30, 36, 70))
        for k in (-4, 0, 4):
            d.ellipse([px(tx + k - 1.6), px(ty - 3.2), px(tx + k + 1.6), px(ty + 0.4)], fill=(255, 250, 220))


def draw_vignette(img):
    arr = np.asarray(img).astype(np.float32)
    ys, xs = np.mgrid[0:S, 0:S]
    dist = np.hypot((xs - S / 2) / (S / 2), (ys - S / 2) / (S / 2))
    factor = 1.0 - 0.28 * np.clip(dist - 0.7, 0.0, 1.0) ** 1.5
    arr *= factor[:, :, None]
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def build():
    rng = random.Random(20261022)          # fixed seed: the picture is reproducible
    img = sky(None)
    # a few stars
    sd = ImageDraw.Draw(img)
    for _ in range(40):
        x, y, r = rng.uniform(0, 256), rng.uniform(0, 60), rng.uniform(0.4, 0.9)
        sd.ellipse([px(x - r), px(y - r), px(x + r), px(y + r)], fill=(220, 230, 255))
    draw_stands(img, rng)
    draw_turf(img)
    draw_pad(img)
    draw_towers_and_beams(img)
    img = draw_vignette(img)
    return img.resize((FINAL, FINAL), Image.LANCZOS)


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "icon.jpg")
    img = build()
    if out.lower().endswith((".jpg", ".jpeg")):
        img.save(out, "JPEG", quality=92, optimize=False, progressive=False, subsampling=0)
    else:
        img.save(out)
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main(sys.argv)
