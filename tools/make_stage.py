#!/usr/bin/env python3
"""Draw the stage background and emit it as Neo Geo sprite tiles.

The Neo Geo has no background layer: everything on screen is either a sprite
or the fix layer, and the fix layer always draws on top. So a background is
made of sprites with lower numbers than the character, since higher-numbered
sprites are drawn in front.

The stage is one screen, 320x224, which is exactly 20x14 tiles. That is also
the layout tiletool expects, so the image needs no rearranging: tile (col, row)
is simply the 16x16 block at that position, numbered left to right then top to
bottom.

Colours are chosen here rather than quantised from a source image, so the
palette is exact. Index 0 is transparent on this hardware and is therefore
never used for a visible pixel.
"""

import argparse
import random

from neogeo_color import snap, to_color_word
from PIL import Image, ImageDraw

W, H = 320, 224

# Index 0 must stay transparent, so the stage draws with indices 1-15.
PALETTE = [
    (0, 0, 0),          # 0 transparent, never drawn
    (16, 16, 40),       # 1  sky, darkest
    (30, 28, 60),       # 2  sky
    (48, 44, 84),       # 3  sky
    (78, 66, 104),      # 4  sky at the horizon
    (232, 224, 200),    # 5  moon and stars
    (12, 12, 28),       # 6  far mountains
    (26, 26, 52),       # 7  far mountains, lit edge
    (20, 30, 40),       # 8  near hills
    (34, 24, 20),       # 9  ground, deepest
    (54, 40, 30),       # 10 ground
    (74, 56, 40),       # 11 ground, upper
    (96, 74, 52),       # 12 ground, lit edge
    (18, 14, 12),       # 13 rock shadow
    (44, 54, 38),       # 14 scrub
    (132, 104, 70),     # 15 highlight
]


def ridge(draw, rng, base_y, amplitude, step, color, seed):
    """Fill everything below a jagged skyline with `color`."""
    rng.seed(seed)
    y = base_y
    pts = []
    for x in range(0, W + step, step):
        y += rng.randint(-amplitude, amplitude)
        y = max(base_y - amplitude * 3, min(base_y + amplitude * 2, y))
        pts.append((x, y))
    pts += [(W, H), (0, H)]
    draw.polygon(pts, fill=color)
    return pts


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-o", "--output", required=True, help="GIF to write")
    p.add_argument("--header", required=True, help="C header to write")
    p.add_argument("--name", default="stage", help="identifier prefix")
    p.add_argument("--ground", type=int, default=184,
                   help="y of the ground line; the character stands on it")
    p.add_argument("--seed", type=int, default=7, help="scenery random seed")
    args = p.parse_args()

    im = Image.new("P", (W, H), 1)
    flat = []
    for c in PALETTE:
        flat += list(c)
    im.putpalette(flat)
    d = ImageDraw.Draw(im)
    rng = random.Random(args.seed)

    # Sky: banded, because 15 colours do not stretch to a smooth gradient.
    # The boundaries are dithered with a checkerboard, which is how the era's
    # artists faked extra shades and reads far better than a hard line.
    horizon = args.ground
    bands = [(0, 0.30, 1), (0.30, 0.55, 2), (0.55, 0.80, 3), (0.80, 1.0, 4)]
    for lo, hi, color in bands:
        d.rectangle([0, int(horizon * lo), W, int(horizon * hi)], fill=color)

    dither_h = 12
    for i in range(len(bands) - 1):
        boundary = int(horizon * bands[i][1])
        upper, lower = bands[i][2], bands[i + 1][2]
        for row in range(dither_h):
            y = boundary - dither_h // 2 + row
            if not 0 <= y < horizon:
                continue
            # Fade from the upper colour to the lower one across the band.
            density = row / (dither_h - 1)
            for x in range(W):
                checker = (x + y) % 2 == 0
                if density > 0.66:
                    im.putpixel((x, y), lower)
                elif density > 0.33:
                    im.putpixel((x, y), lower if checker else upper)
                elif density > 0.1 and checker and (x // 2 + y // 2) % 2 == 0:
                    im.putpixel((x, y), lower)
                else:
                    im.putpixel((x, y), upper)

    # Stars, thinning out towards the brighter horizon.
    for _ in range(90):
        x = rng.randrange(W)
        y = rng.randrange(int(horizon * 0.75))
        if rng.random() < 1.0 - y / (horizon * 0.75):
            im.putpixel((x, y), 5)

    # Moon, with a bite taken out to make a crescent.
    mx, my, r = 248, 40, 15
    d.ellipse([mx - r, my - r, mx + r, my + r], fill=5)
    d.ellipse([mx - r + 8, my - r - 3, mx + r + 8, my + r - 3], fill=2)

    # Two ranges of hills, the nearer one darker so it reads as closer.
    ridge(d, rng, int(horizon * 0.78), 7, 20, 7, args.seed)
    ridge(d, rng, int(horizon * 0.86), 5, 14, 6, args.seed + 1)
    ridge(d, rng, int(horizon * 0.95), 4, 11, 8, args.seed + 2)

    # Ground: a lit edge at the top, then progressively darker bands.
    d.rectangle([0, args.ground, W, H], fill=10)
    d.rectangle([0, args.ground, W, args.ground + 2], fill=12)
    d.rectangle([0, args.ground + 3, W, args.ground + 7], fill=11)
    d.rectangle([0, args.ground + 22, W, H], fill=9)

    # Scatter some stones and scrub so the floor is not a flat band.
    rng.seed(args.seed + 3)
    for _ in range(42):
        x = rng.randrange(W)
        y = rng.randrange(args.ground + 6, H - 2)
        size = rng.choice([1, 1, 2])
        color = rng.choice([13, 13, 9, 15])
        d.rectangle([x, y, x + size, y + size], fill=color)
    for _ in range(26):
        x = rng.randrange(W)
        y = rng.randrange(args.ground - 1, args.ground + 4)
        d.rectangle([x, y, x + rng.choice([1, 2]), y + 1], fill=14)

    im.save(args.output, transparency=0, optimize=False)
    write_header(args, im.size)

    print(f"{args.output}: {W}x{H} px = {W // 16}x{H // 16} tiles "
          f"({W // 16 * (H // 16)} tiles), ground at y={args.ground}")


def write_header(args, size):
    name, up = args.name, args.name.upper()
    words = [0x8000 if i == 0 else to_color_word(*PALETTE[i]) for i in range(16)]
    with open(args.header, "w") as f:
        f.write("/* Generated by tools/make_stage.py - do not edit. */\n")
        f.write(f"#ifndef {up}_H\n#define {up}_H\n\n")
        f.write(f"#define {up}_TILES_W {size[0] // 16}\n")
        f.write(f"#define {up}_TILES_H {size[1] // 16}\n")
        f.write(f"#define {up}_GROUND_Y {args.ground}\n\n")
        f.write("/* 16 colours, index 0 transparent. */\n")
        f.write(f"static const u16 {name}_palette[16] = {{\n")
        for i in range(0, 16, 4):
            f.write("    " + ", ".join(f"0x{w:04x}" for w in words[i:i + 4]) + ",\n")
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    main()
