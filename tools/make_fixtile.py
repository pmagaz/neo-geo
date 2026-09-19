#!/usr/bin/env python3
"""Draw the solid tile the screen transition paints with.

The fix layer is a 40x32 grid of 8x8 tiles drawn on top of every sprite, which
makes it the right tool for covering the screen: filling cells with a solid
tile hides what is behind them, and clearing them reveals it again a block at
a time.

ngdevkit's font ROM has no solid block, so this makes one. Every pixel is
colour 3 of whichever palette the map entry names - the game points that at
black. Colour 0 would be transparent and hide nothing.
"""

import argparse

from PIL import Image

COLOUR = 3


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-o", "--output", required=True, help="GIF to write")
    args = p.parse_args()

    im = Image.new("P", (8, 8), COLOUR)
    # The palette here is irrelevant - the hardware uses the index, and the
    # colour comes from the palette the fix map entry selects. It only has to
    # have four entries so the index survives being written out.
    im.putpalette([0, 0, 0, 255, 255, 255, 128, 128, 128, 255, 0, 255])

    # optimize=False keeps the unused low indices, which Pillow would
    # otherwise drop, renumbering our pixels to 0 - transparent, and useless.
    im.save(args.output, optimize=False)
    print(f"{args.output}: 8x8 solid tile, colour index {COLOUR}")


if __name__ == "__main__":
    main()
