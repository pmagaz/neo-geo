# Art asset specification

What the game needs from a piece of art, why, and how to ask an image
generator for it.

Every number here is what the code actually uses today. The sizes the game
works in are generated into headers (`assets/images/sprites/hero.h`,
`assets/images/stages/stage.h`) and the build reads them from there, so this
document and the game cannot drift apart silently — but if you change a size,
change it in the makefile, not here.

---

## 1. What the hardware fixes

These are not preferences. The console cannot do otherwise.

| | |
|---|---|
| Screen | 320 × 224 pixels |
| Safe area | Avoid the leftmost and rightmost 8 pixels, and the top and bottom 16 lines — they fall outside a real TV's picture |
| Sprite tile | 16 × 16 pixels |
| Text tile | 8 × 8 pixels |
| Colours per palette | **15, plus transparent** |
| Palettes on screen | 256, but one sprite uses exactly one |
| Sprites per scanline | **96** |
| Sprites per frame | 381 |

Two consequences worth understanding before drawing anything:

**A sprite is a vertical strip, not a rectangle.** It is 16 pixels wide and up
to 32 tiles tall. A character 64 pixels wide is therefore four sprites side by
side, and it costs four of the 96 sprites available on any line it touches.
Width is expensive; height is nearly free.

**Transparency is a colour.** Index 0 of every palette is transparent and
cannot be drawn with, which is why the limit is 15 colours and not 16.

---

## 2. Sizes the game uses now

### The character

| | |
|---|---|
| On screen | **64 × 64** px (4 × 4 tiles) |
| Animations | walk 8 frames, attack 8, jump 5, crouch 3 |
| Sprites used | 4 (one per 16 px of width) |
| Feet stand on | y = 192 |

The source sheet does **not** have to be this size. `tools/prep_sheet.py`
scales whatever you supply so the tallest frame becomes 64 pixels, and every
frame is scaled by that same factor so the animation keeps its proportions.
The current source is 647 × 718 with frames about 88 px tall.

Drawing larger than the target and letting the build scale down is fine and
usually looks better than drawing at 64 px directly. Two to three times the
final size is a good range; beyond that, detail is lost in the reduction
anyway.

### The stage

Three layers, each **320 pixels wide** — one screen — scrolled at different
speeds. All three share a single 15-colour palette.

| Layer | Size | Sits at | Scrolls | Must tile |
|---|---|---|---|---|
| Sky | 320 × 192 | y = 0 | never | no |
| Hills | 320 × 80 | y = 112 | ¼ speed | **yes** |
| Ground | 320 × 48 | y = 176 | full speed | **yes** |

The floor line — where the character's feet land — is at **y = 192**, which is
16 pixels down the ground layer.

The hills layer is transparent above its ridges so the sky shows through. The
two scrolling layers repeat every 320 pixels, so their left and right edges
must join **exactly**: a one-pixel mismatch becomes a seam crossing the screen
every few seconds.

### Room left

| | |
|---|---|
| Sprite ROM used | 1,168 tiles of 16,384 — about 7% |
| Busiest scanline | 66 sprites of 96 |
| Spare | 30 sprites ≈ **7 more 64-px-wide characters** at once |

Enemies are limited by that 96-per-line budget, not by memory. Three
background layers cost 62 of the 66 currently used.

---

## 3. What a source sheet must look like

The build finds animations by itself: `tools/sheet2neo.py` looks for **fully
transparent gaps**. Rows of animation are separated by transparent rows, and
the frames within a row by transparent columns. Animations are then selected
by number, like `--anim walk:1 --anim jump:4:4-8`.

So the layout rules are:

1. **One animation per row**, in a consistent order.
2. **A clear transparent gap between rows** — a few pixels is enough, but it
   must contain no pixels at all, not even faint ones.
3. **A clear transparent gap between frames** in a row.
4. **Real transparency**, not a painted checkerboard and not white. See the
   pitfalls below; this is the one that has cost the most time.
5. **Margin below the last row.** A row touching the bottom edge of the image
   gets cut off, and the frames are unusable.
6. **A consistent baseline.** The build fits one bounding box to every frame
   of every animation so the character does not change size when the
   animation changes, and aligns them by their feet. Frames drawn at wildly
   different scales will shrink everything else to fit.
7. **Side view, facing right.** Only one direction is needed: the hardware
   mirrors sprites for free. Drawing both directions wastes half the ROM.

Run `python3 tools/sheet2neo.py <sheet> --list` to see what the build found
before wiring anything up. If the frame counts do not match what you drew, the
gaps are not clean.

---

## 4. Asking an image generator for it

Image models are good at the drawing and bad at the layout. Expect to accept
the art and fix the layout, and write the prompt to minimise the fixing.

### A prompt that works

> A pixel art sprite sheet of **[character]**, side view, facing right.
>
> Four rows of animation, in this order: row 1 walk cycle, 8 frames; row 2
> attack, 8 frames; row 3 jump, 5 frames; row 4 crouch, 3 frames.
>
> Every frame the same size, evenly spaced, with clear empty space between
> each frame and between each row. Leave empty space below the last row.
>
> All frames share one ground line — the character's feet at the same height
> in every frame of a row.
>
> Limited palette, about 12 to 15 flat colours, no gradients, no
> anti-aliasing, hard pixel edges, no outline glow, no drop shadow.
>
> Plain solid magenta background (#FF00FF), nothing else in the image, no
> text, no labels, no frame numbers, no borders.

Magenta rather than "transparent" is deliberate: models rarely produce a real
alpha channel, and a saturated colour that appears nowhere in the art is
trivial to key out cleanly. White is a poor choice — it collides with
highlights, eyes and metal.

### For a stage layer

> A seamless horizontally tiling pixel art **[sky / distant hills / ground]**
> layer, exactly 320 pixels wide and **[192 / 80 / 48]** pixels tall.
>
> The left and right edges must match exactly so the image repeats without a
> visible seam.
>
> Limited palette, at most 8 flat colours, no gradients, no anti-aliasing.
> **[For hills and ground: everything above the terrain must be plain solid
> magenta (#FF00FF) so it can be made transparent.]**
> Side-on view, no perspective, no characters, no text.

In practice, generated layers are rarely seamless. `tools/make_stage.py` draws
the current stage in code instead, which guarantees it: the ridges are sine
waves with a whole number of cycles across the width, so they cannot help but
join up. For a generated layer you will likely need to fix the seam by hand.

---

## 5. Pitfalls, all of which have already happened here

**A checkerboard background is not transparency.** A generated sheet arrived
as a JPEG with the grey transparency checkerboard painted into it as ordinary
pixels. `tools/prep_sheet.py` exists to flood that away from the edges inwards
— flooding rather than deleting every grey pixel, so grey *inside* the
character survives. Ask for flat magenta and this step is reliable; accept a
checkerboard and it is guesswork.

**JPEG is the wrong format.** Its compression smears colour across hard pixel
edges, which both makes the background harder to key and wastes palette
entries on artefacts. Ask for PNG.

**Thousands of colours become fifteen.** One sheet arrived with 33,798 distinct
colours and came out muted, because the palette had to be spread across skin,
cloth and metal at once. Art drawn with a deliberately small palette survives
the conversion far better than art that is quantised down to one.

**A row touching the bottom edge is lost.** One sheet's idle poses were cut off
by the image boundary, so that animation had to be dropped. Ask for margin.

**Frames that merge are counted as one.** Where a sword or a cape crossed into
the neighbouring frame, the build saw 7 frames instead of 8. Gaps must be
truly empty.

---

## 6. Adding a new asset

For a character sheet, point the makefile at it and name its animations:

```make
SHEET=assets/images/sprites/your-sheet.png
    --anim walk:0 --anim attack:1 --anim jump:2:1-4 --anim crouch:2:0-1
```

`--anim NAME:ROW` takes a whole row; `--anim NAME:ROW:FIRST-LAST` takes part
of one, which is how the jump was split out of a row that begins with a
crouch. The character's size is `--height` in the `prep_sheet` step.

Both tools write a C header next to their output with the palette, the frame
size and where each animation starts, and the game reads those rather than
hard-coded numbers — so a new sheet of a different size needs no code change.
