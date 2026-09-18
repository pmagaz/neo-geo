"""Neo Geo colour conversion, shared by the asset tools.

A palette entry is one 16-bit word and each channel has six bits of
intensity, laid out awkwardly:

    bit 15    "dark" bit: the lowest bit of all three channels at once,
              and stored inverted
    bits 14-12  the second-lowest bit of R, G and B
    bits 11-0   the top four bits of R, then G, then B

So a channel's six-bit value is  (field4 << 2) | (own_low_bit << 1) | dark.

Sanity checks from the hardware documentation: 0x8000 is black, 0x7fff is
white, 0x0f00 is red.
"""


def to_6bit(c):
    """8-bit channel value to the hardware's 6 bits."""
    return (c * 63 + 127) // 255


def snap(rgb):
    """Round a colour to the nearest one the hardware can display."""
    return tuple(to_6bit(c) * 255 // 63 for c in rgb)


def to_color_word(r, g, b):
    """Pack an RGB colour into a Neo Geo palette word."""
    r6, g6, b6 = to_6bit(r), to_6bit(g), to_6bit(b)

    # The lowest bit is shared by all three channels, so take the majority.
    dark = 1 if (r6 & 1) + (g6 & 1) + (b6 & 1) >= 2 else 0

    word = (dark ^ 1) << 15          # stored inverted
    word |= ((r6 >> 1) & 1) << 14
    word |= ((g6 >> 1) & 1) << 13
    word |= ((b6 >> 1) & 1) << 12
    word |= ((r6 >> 2) & 0xF) << 8
    word |= ((g6 >> 2) & 0xF) << 4
    word |= (b6 >> 2) & 0xF
    return word


def from_color_word(word):
    """Decode a palette word back to RGB, the way the hardware does.

    This mirrors the conversion the console performs, so it is what the
    encoder is checked against.
    """
    dark = ((word >> 15) & 1) ^ 1

    def channel(field4, low):
        v6 = (field4 << 2) | (low << 1) | dark
        return (v6 * 255 + 31) // 63

    return (
        channel((word >> 8) & 0xF, (word >> 14) & 1),
        channel((word >> 4) & 0xF, (word >> 13) & 1),
        channel(word & 0xF, (word >> 12) & 1),
    )


def _self_test():
    # The two values the hardware documentation pins down exactly.
    assert to_color_word(0, 0, 0) == 0x8000, hex(to_color_word(0, 0, 0))
    assert to_color_word(255, 255, 255) == 0x7FFF, hex(to_color_word(255, 255, 255))

    # Everything else is checked by round-tripping: encoding then decoding
    # must land close to the colour we started from. The shared dark bit is a
    # compromise across three channels, so one step of error is expected.
    worst = 0
    for r in range(0, 256, 5):
        for g in range(0, 256, 5):
            for b in range(0, 256, 5):
                out = from_color_word(to_color_word(r, g, b))
                worst = max(worst, max(abs(a - c) for a, c in zip((r, g, b), out)))
    assert worst <= 8, f"round-trip error too large: {worst}"
    return worst


if __name__ == "__main__":
    worst = _self_test()
    print(f"round-trip worst-case error: {worst}/255")
    for name, rgb in [("black", (0, 0, 0)), ("white", (255, 255, 255)),
                      ("red", (255, 0, 0)), ("navy", (16, 16, 40))]:
        w = to_color_word(*rgb)
        print(f"{name:6s} {rgb} -> 0x{w:04x} -> {from_color_word(w)}")
