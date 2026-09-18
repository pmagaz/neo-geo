#!/usr/bin/env python3
"""Prepare a sprite sheet that has no usable transparency.

Sheets that were rendered rather than drawn often arrive as a JPEG with the
background painted in - frequently the grey checkerboard that stands for
transparency in an image editor. That pattern is just pixels, so it has to be
removed before the sheet means anything, and JPEG compression means it is
never exactly the colour it looks.

This writes a PNG with real transparency, and optionally scales the sheet so
the character comes out the size the game wants. sheet2neo.py then reads that
PNG like any other sheet, so the conversion itself stays unchanged.

    prep_sheet.py in.jpeg -o out.png --height 96

Only background connected to the edge of the image is removed, so a highlight
inside the character that happens to be the same colour survives.
"""

import argparse

from PIL import Image


def is_backdrop(p, max_saturation, min_brightness):
    """A near-grey, bright pixel: checkerboard or a plain pale background."""
    return (max(p[:3]) - min(p[:3])) <= max_saturation and max(p[:3]) >= min_brightness


def key_from_edges(im, max_saturation, min_brightness):
    """Flood the backdrop inwards from the border and make it transparent."""
    w, h = im.size
    px = im.load()

    seen = bytearray(w * h)
    stack = [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)]
    stack += [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)]

    while stack:
        x, y = stack.pop()
        if not (0 <= x < w and 0 <= y < h) or seen[y * w + x]:
            continue
        if not is_backdrop(px[x, y], max_saturation, min_brightness):
            continue
        seen[y * w + x] = 1
        stack.append((x + 1, y))
        stack.append((x - 1, y))
        stack.append((x, y + 1))
        stack.append((x, y - 1))

    for y in range(h):
        base = y * w
        for x in range(w):
            if seen[base + x]:
                px[x, y] = (0, 0, 0, 0)
    return im


def content_rows(im):
    """Bands of the sheet that hold something, separated by empty rows."""
    w, h = im.size
    px = im.load()
    rows, start = [], None
    for y in range(h):
        empty = all(px[x, y][3] <= 8 for x in range(w))
        if not empty and start is None:
            start = y
        elif empty and start is not None:
            rows.append((start, y - 1))
            start = None
    if start is not None:
        rows.append((start, h - 1))
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("sheet", help="source image")
    p.add_argument("-o", "--output", required=True, help="PNG to write")
    p.add_argument("--height", type=int, metavar="PX",
                   help="scale so the tallest frame is this many pixels")
    p.add_argument("--saturation", type=int, default=20,
                   help="how far from grey the backdrop may stray (default 20)")
    p.add_argument("--brightness", type=int, default=170,
                   help="how dark the backdrop may be (default 170)")
    args = p.parse_args()

    im = Image.open(args.sheet).convert("RGBA")
    before = im.size
    im = key_from_edges(im, args.saturation, args.brightness)

    rows = content_rows(im)
    if not rows:
        raise SystemExit("error: the whole sheet was keyed out - loosen "
                         "--saturation or --brightness")
    tallest = max(b - a + 1 for a, b in rows)
    print(f"{args.sheet}: {before[0]}x{before[1]}, {len(rows)} rows, "
          f"tallest frame {tallest} px")

    if args.height:
        scale = args.height / tallest
        size = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
        # LANCZOS keeps detail far better than nearest-neighbour when shrinking
        # art this soft, and the edges are hardened again by thresholding alpha.
        im = im.resize(size, Image.LANCZOS)
        px = im.load()
        for y in range(im.height):
            for x in range(im.width):
                r, g, b, a = px[x, y]
                px[x, y] = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
        print(f"  scaled by {scale:.3f} to {size[0]}x{size[1]}, "
              f"character now about {args.height} px tall")

    im.save(args.output)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
