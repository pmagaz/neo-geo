#!/usr/bin/env python3
"""Turn animations from a sprite sheet into Neo Geo sprite tiles.

The Neo Geo draws sprites from 16x16 tiles at 4 bits per pixel, so a sprite
uses at most 16 colours and colour 0 is always transparent. Source art is
normally neither 16-colour nor a multiple of 16 pixels.

Rows of animation and the frames within them are found automatically, by
looking for fully transparent gaps, so animations are selected by row and
frame number rather than by measuring pixels:

    sheet2neo.py sheet.png -o out.gif --header out.h --name hero \\
        --anim walk:1 --anim attack:2 --anim jump:4:4-8

`--anim NAME:ROW` takes a whole row; `--anim NAME:ROW:FIRST-LAST` takes a
range of frames from it, counting from zero. Use --list to print what the
sheet contains and stop.

Every animation shares one palette and one bounding box, so the character
neither changes colour nor jumps vertically when the animation changes. The
box is padded out to whole tiles with the feet kept on the bottom edge.

All the animations are packed into a single image, one animation per band of
tile rows. Tiles are numbered left to right then top to bottom, so with the
sheet SHEET_W tiles wide, frame f of an animation starting at tile row
ROW_OFF has its tile at column c, row r at:

    base + (ROW_OFF + r) * SHEET_W + f * TILES_W + c
"""

import argparse
import sys
from neogeo_color import snap, to_color_word
from PIL import Image



def find_rows(im):
    """Split the sheet into bands separated by fully transparent rows."""
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


def find_frames(im, top, bottom):
    """Split one band into frames separated by transparent columns."""
    w, _ = im.size
    px = im.load()
    frames, start = [], None
    for x in range(w):
        empty = all(px[x, y][3] <= 8 for y in range(top, bottom + 1))
        if not empty and start is None:
            start = x
        elif empty and start is not None:
            frames.append((start, x - 1))
            start = None
    if start is not None:
        frames.append((start, w - 1))
    return frames


def parse_anim(spec):
    """NAME:ROW or NAME:ROW:FIRST-LAST"""
    parts = spec.split(":")
    if len(parts) == 2:
        return parts[0], int(parts[1]), None
    if len(parts) == 3:
        first, last = parts[2].split("-")
        return parts[0], int(parts[1]), (int(first), int(last))
    raise SystemExit(f"error: bad --anim {spec!r}, want NAME:ROW[:FIRST-LAST]")


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("sheet", help="source PNG")
    p.add_argument("-o", "--output", help="GIF to write")
    p.add_argument("--header", metavar="FILE.h", help="C header to write")
    p.add_argument("--name", default="sprite", help="identifier prefix")
    p.add_argument("--anim", action="append", default=[], metavar="NAME:ROW[:A-B]",
                   help="an animation to include; repeatable")
    p.add_argument("--colors", type=int, default=15,
                   help="colours besides transparent (max 15)")
    p.add_argument("--list", action="store_true",
                   help="describe the sheet's rows and frames, then stop")
    args = p.parse_args()

    im = Image.open(args.sheet).convert("RGBA")
    rows = find_rows(im)

    if args.list or not args.anim:
        print(f"{args.sheet}: {im.size[0]}x{im.size[1]}, {len(rows)} rows")
        for i, (top, bottom) in enumerate(rows):
            fr = find_frames(im, top, bottom)
            print(f"  row {i}: y {top}-{bottom} (h={bottom - top + 1}), "
                  f"{len(fr)} frames")
        return 0 if args.list else 1

    if not 1 <= args.colors <= 15:
        raise SystemExit("error: --colors must be between 1 and 15")
    if not args.output or not args.header:
        raise SystemExit("error: --output and --header are both required")

    # Cut every requested animation into frames.
    anims = []
    for spec in args.anim:
        name, row, rng = parse_anim(spec)
        if row >= len(rows):
            raise SystemExit(f"error: {name}: row {row} but sheet has {len(rows)}")
        top, bottom = rows[row]
        boxes = find_frames(im, top, bottom)
        if rng:
            first, last = rng
            if last >= len(boxes):
                raise SystemExit(
                    f"error: {name}: frames {first}-{last} but row {row} "
                    f"has {len(boxes)}")
            boxes = boxes[first:last + 1]
        frames = [im.crop((a, top, b + 1, bottom + 1)) for a, b in boxes]
        anims.append({"name": name, "frames": frames})

    # One bounding box for every frame of every animation, so the character
    # keeps a constant size and its feet stay put when the animation changes.
    everything = [f for a in anims for f in a["frames"]]
    boxes = [f.getbbox() for f in everything if f.getbbox()]
    if not boxes:
        raise SystemExit("error: all frames are empty")
    bw = max(b[2] - b[0] for b in boxes)
    bh = max(b[3] - b[1] for b in boxes)

    tw = (bw + 15) // 16
    th = (bh + 15) // 16
    cell_w, cell_h = tw * 16, th * 16

    sheet_w = max(len(a["frames"]) for a in anims) * tw
    sheet_h = sum(th for _ in anims)

    out = Image.new("RGBA", (sheet_w * 16, sheet_h * 16), (0, 0, 0, 0))
    for band, a in enumerate(anims):
        a["row_off"] = band * th
        for i, f in enumerate(a["frames"]):
            bb = f.getbbox()
            if not bb:
                continue
            art = f.crop(bb)
            # Centre horizontally, sit on the bottom edge: a walk cycle only
            # looks planted if the feet do not drift between frames.
            ox = i * cell_w + (cell_w - art.width) // 2
            oy = band * cell_h + (cell_h - art.height)
            out.paste(art, (ox, oy))

    # Snap to the hardware's colour space before quantising, so the palette we
    # pick is one the console can actually reproduce.
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, alpha = px[x, y]
            px[x, y] = snap((r, g, b)) + (255,) if alpha > 127 else (0, 0, 0, 0)

    quant = out.convert("RGB").quantize(colors=args.colors, method=Image.MEDIANCUT)
    # Art with few distinct colours quantises to fewer than asked for, and
    # then the palette comes back short. Pad it, or the entries past the end
    # are missing rather than black.
    src_pal = quant.getpalette()[: args.colors * 3]
    src_pal += [0] * (args.colors * 3 - len(src_pal))

    # Shift every colour up by one so index 0 can mean transparent.
    indexed = Image.new("P", out.size, 0)
    indexed.putpalette([0, 0, 0] + src_pal + [0, 0, 0] * (15 - args.colors))
    qpx, opx, ipx = quant.load(), out.load(), indexed.load()
    for y in range(out.height):
        for x in range(out.width):
            ipx[x, y] = 0 if opx[x, y][3] < 128 else qpx[x, y] + 1
    indexed.save(args.output, transparency=0, optimize=False)

    write_header(args, indexed.getpalette(), anims, tw, th, sheet_w)

    print(f"{args.output}: {sheet_w}x{sheet_h} tiles "
          f"({sheet_w * 16}x{sheet_h * 16} px), frame {tw}x{th} tiles, "
          f"{args.colors} colours + transparent")
    for a in anims:
        print(f"  {a['name']:8s} {len(a['frames'])} frames, tile row {a['row_off']}")
    return 0


def write_header(args, palette, anims, tw, th, sheet_w):
    name = args.name
    up = name.upper()
    palette = list(palette) + [0] * (48 - len(palette))
    words = [0x8000 if i == 0 else
             to_color_word(*palette[i * 3: i * 3 + 3]) for i in range(16)]

    with open(args.header, "w") as f:
        f.write("/* Generated by tools/sheet2neo.py - do not edit. */\n")
        f.write(f"#ifndef {up}_H\n#define {up}_H\n\n")
        sheet_h = sum(th for _ in anims)
        f.write(f"#define {up}_TILES_W {tw}\n")
        f.write(f"#define {up}_TILES_H {th}\n")
        f.write(f"#define {up}_SHEET_W {sheet_w}\n")
        # How many C ROM tiles this sheet occupies, so whatever is loaded
        # after it knows where it starts.
        f.write(f"#define {up}_TILE_COUNT {sheet_w * sheet_h}\n\n")
        for a in anims:
            an = f"{up}_{a['name'].upper()}"
            f.write(f"#define {an}_FRAMES {len(a['frames'])}\n")
            f.write(f"#define {an}_ROW {a['row_off']}\n")
        f.write(f"\n/* 16 colours, index 0 transparent. */\n")
        f.write(f"static const u16 {name}_palette[16] = {{\n")
        for i in range(0, 16, 4):
            f.write("    " + ", ".join(f"0x{w:04x}" for w in words[i:i + 4]) + ",\n")
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    sys.exit(main())
