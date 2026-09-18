#!/usr/bin/env python3
"""Draw the stage as parallax layers and emit them as Neo Geo sprite tiles.

The Neo Geo has no background layer: everything on screen is either a sprite
or the fix layer, and the fix layer always draws on top. So the stage is made
of sprites with lower numbers than the character, since higher-numbered
sprites are drawn in front.

Three layers, each 320 pixels wide, scrolled at its own rate:

    sky     stars and the moon, never moves
    hills   the ridges, scrolled slowly so they read as distant
    ground  the floor, scrolled with the character

The scrolling layers repeat every 320 pixels, so they have to be seamless:
the right edge must join the left edge exactly. The ridges are therefore
built from sine waves with a whole number of cycles across the width rather
than from a random walk, and anything scattered near an edge is drawn again
on the other side.

All three layers share one 16-colour palette. Index 0 is transparent on this
hardware, so it is never used for a visible pixel.
"""

import argparse
import math
import random

from neogeo_color import to_color_word
from PIL import Image, ImageDraw

W = 320

# Where each layer sits on screen and how tall it is. The sky reaches down
# behind the hills; the hills are transparent above their ridges.
SKY_Y, SKY_H = 0, 192
HILLS_Y, HILLS_H = 112, 80
GROUND_Y, GROUND_H = 176, 48

# Index 0 must stay transparent, so the stage draws with indices 1-15.
PALETTE = [
    (0, 0, 0),          # 0  transparent, never drawn
    (16, 16, 40),       # 1  sky, darkest
    (30, 28, 60),       # 2  sky
    (48, 44, 84),       # 3  sky
    (78, 66, 104),      # 4  sky at the horizon
    (232, 224, 200),    # 5  moon and stars
    (12, 12, 28),       # 6  far ridge
    (26, 26, 52),       # 7  far ridge, lit edge
    (20, 30, 40),       # 8  near ridge
    (34, 24, 20),       # 9  ground, deepest
    (54, 40, 30),       # 10 ground
    (74, 56, 40),       # 11 ground, upper
    (96, 74, 52),       # 12 ground, lit edge
    (18, 14, 12),       # 13 rock shadow
    (44, 54, 38),       # 14 scrub
    (132, 104, 70),     # 15 highlight
]


def new_layer(height, fill=0):
    im = Image.new("P", (W, height), fill)
    im.putpalette([c for rgb in PALETTE for c in rgb])
    return im


def wave(x, terms):
    """A periodic height at x: whole cycles across W, so it always joins up."""
    return sum(a * math.sin(2 * math.pi * f * x / W + p) for f, a, p in terms)


def ridge(im, terms, base_y, color):
    """Fill below a seamless skyline with `color`."""
    d = ImageDraw.Draw(im)
    pts = [(x, base_y + wave(x, terms)) for x in range(W + 1)]
    d.polygon(pts + [(W, im.height), (0, im.height)], fill=color)


def draw_sky(args):
    im = new_layer(SKY_H, 1)
    d = ImageDraw.Draw(im)
    rng = random.Random(args.seed)

    # Banded, because 15 colours do not stretch to a smooth gradient. The
    # boundaries are dithered, which is how the era's artists faked extra
    # shades and reads far better than a hard line.
    bands = [(0, 0.34, 1), (0.34, 0.62, 2), (0.62, 0.86, 3), (0.86, 1.0, 4)]
    for lo, hi, color in bands:
        d.rectangle([0, int(SKY_H * lo), W, int(SKY_H * hi)], fill=color)

    dither_h = 12
    for i in range(len(bands) - 1):
        boundary = int(SKY_H * bands[i][1])
        upper, lower = bands[i][2], bands[i + 1][2]
        for row in range(dither_h):
            y = boundary - dither_h // 2 + row
            if not 0 <= y < SKY_H:
                continue
            density = row / (dither_h - 1)
            for x in range(W):
                checker = (x + y) % 2 == 0
                if density > 0.66:
                    im.putpixel((x, y), lower)
                elif density > 0.33:
                    im.putpixel((x, y), lower if checker else upper)
                elif density > 0.1 and checker and (x // 2 + y // 2) % 2 == 0:
                    im.putpixel((x, y), lower)

    # Stars, thinning out towards the brighter horizon.
    for _ in range(110):
        x, y = rng.randrange(W), rng.randrange(int(SKY_H * 0.8))
        if rng.random() < 1.0 - y / (SKY_H * 0.8):
            im.putpixel((x, y), 5)

    # Moon, with a bite taken out of it to make a crescent.
    mx, my, r = 254, 34, 11
    d.ellipse([mx - r, my - r, mx + r, my + r], fill=5)
    d.ellipse([mx - r + 6, my - r - 2, mx + r + 6, my + r - 2], fill=2)
    return im


def draw_hills(args):
    """Ridges drawn on transparent, so the sky shows through above them."""
    im = new_layer(HILLS_H, 0)
    ridge(im, [(1, 6, 0.0), (2, 3, 1.1), (3, 2, 2.3)], 34, 7)
    ridge(im, [(1, 5, 2.0), (3, 3, 0.4), (5, 2, 1.7)], 50, 6)
    ridge(im, [(2, 4, 1.0), (3, 2, 2.9), (7, 1, 0.2)], 64, 8)
    return im


def draw_ground(args):
    im = new_layer(GROUND_H, 0)
    d = ImageDraw.Draw(im)
    rng = random.Random(args.seed + 3)
    top = args.ground - GROUND_Y          # the ground line, within this layer

    d.rectangle([0, top, W, GROUND_H], fill=10)
    d.rectangle([0, top, W, top + 2], fill=12)
    d.rectangle([0, top + 3, W, top + 7], fill=11)
    d.rectangle([0, top + 22, W, GROUND_H], fill=9)

    # Stones and scrub. Anything crossing an edge is drawn on the other side
    # too, so the layer still joins up where it repeats.
    def blot(x, y, w, h, color):
        d.rectangle([x, y, x + w, y + h], fill=color)
        if x + w >= W:
            d.rectangle([x - W, y, x - W + w, y + h], fill=color)

    for _ in range(46):
        blot(rng.randrange(W), rng.randrange(top + 6, GROUND_H - 2),
             rng.choice([1, 1, 2]), rng.choice([1, 1, 2]),
             rng.choice([13, 13, 9, 15]))
    for _ in range(30):
        blot(rng.randrange(W), rng.randrange(top - 1, top + 4),
             rng.choice([1, 2]), 1, 14)
    return im


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--outdir", default="assets", help="where to write the GIFs")
    p.add_argument("--header", required=True, help="C header to write")
    p.add_argument("--name", default="stage", help="identifier prefix")
    p.add_argument("--ground", type=int, default=192,
                   help="y of the ground line the character stands on")
    p.add_argument("--seed", type=int, default=7, help="scenery random seed")
    args = p.parse_args()

    layers = [
        ("sky", draw_sky(args), SKY_Y, SKY_H),
        ("hills", draw_hills(args), HILLS_Y, HILLS_H),
        ("ground", draw_ground(args), GROUND_Y, GROUND_H),
    ]

    for lname, im, _, _ in layers:
        path = f"{args.outdir}/{args.name}-{lname}.gif"
        # optimize=False matters: Pillow otherwise drops the unused index 0
        # and shifts every colour down a place. Index 0 is transparent on this
        # hardware, so the whole layer would come out wrong.
        im.save(path, transparency=0, optimize=False)
        print(f"{path}: {W}x{im.height} px = {W // 16}x{im.height // 16} tiles")

    write_header(args, layers)


def write_header(args, layers):
    name, up = args.name, args.name.upper()
    words = [0x8000 if i == 0 else to_color_word(*PALETTE[i]) for i in range(16)]

    with open(args.header, "w") as f:
        f.write("/* Generated by tools/make_stage.py - do not edit. */\n")
        f.write(f"#ifndef {up}_H\n#define {up}_H\n\n")
        f.write(f"#define {up}_COLS {W // 16}\n")
        f.write(f"#define {up}_FLOOR_Y {args.ground}\n\n")
        for lname, im, y, h in layers:
            ln = f"{up}_{lname.upper()}"
            f.write(f"#define {ln}_Y {y}\n")
            f.write(f"#define {ln}_ROWS {h // 16}\n")
            f.write(f"#define {ln}_TILE_COUNT {(W // 16) * (h // 16)}\n")
        f.write("\n/* 16 colours, index 0 transparent. */\n")
        f.write(f"static const u16 {name}_palette[16] = {{\n")
        for i in range(0, 16, 4):
            f.write("    " + ", ".join(f"0x{w:04x}" for w in words[i:i + 4]) + ",\n")
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    main()
