/*
 * Iteration 2: an animated character that walks left and right.
 *
 * The character is 4 tiles wide and 5 tall. On the Neo Geo a sprite is a
 * vertical strip of tiles, so a character this wide is four sprites side by
 * side, chained with the "sticky" bit: only the first carries a position, and
 * each of the others simply follows the one before it.
 */

#include <ngdevkit/neogeo.h>
#include <ngdevkit/ng-fix.h>
#include <stdio.h>
#include "ninja-walk.h"

/* ADDR_SCB1..4 come from ngdevkit's registers.h: the Sprite Control Blocks in
   VRAM. SCB1 holds 64 words per sprite (its tile map); SCB2/3/4 hold one word
   per sprite (shrink, Y + height, X). */

/// The BIOS eye-catcher owns C ROM tiles 0-255, so ours start at 256.
#define WALK_TILE 256
/// Sprite 0 is never drawn by the LSPC, so the first usable sprite is 1.
#define FIRST_SPRITE 1

#define SCREEN_W 320
#define SCREEN_H 224
#define CHAR_W (NINJA_TILES_W * 16)
#define CHAR_H (NINJA_TILES_H * 16)
#define SPEED 2
/// Game frames between animation frames.
#define ANIM_RATE 4

/// The art faces left, so walking right is drawn mirrored.
#define FACING_LEFT 0
#define FACING_RIGHT 1

static s16 walker_x = (SCREEN_W - CHAR_W) / 2;
static const s16 walker_y = SCREEN_H - CHAR_H - 40;   /* stand clear of the text */
static u8 facing = FACING_RIGHT;
static u8 frame = 0;
static u8 anim_tick = 0;


static void init_palette(void) {
    /* Palette 0 draws the fix layer (the text), palette 1 the character. */
    static const u16 text_palette[16] = {
        0x8000, 0x0fff, 0x0666, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    for (u16 i = 0; i < 16; i++) {
        MMAP_PALBANK1[i] = text_palette[i];
        MMAP_PALBANK1[16 + i] = ninja_palette[i];
    }
}


/*
 * Load one animation frame into the sprites' tile maps.
 *
 * Tiles in the sheet run left to right then top to bottom, so the tile at
 * column c, row r of frame f is WALK_TILE + r*NINJA_SHEET_W + f*NINJA_TILES_W + c.
 *
 * Mirroring the character is not just a per-tile flag: the columns have to be
 * drawn in reverse order too, otherwise the sprite is built back to front.
 */
static void set_frame(u8 f, u8 dir) {
    for (u16 col = 0; col < NINJA_TILES_W; col++) {
        u16 src = (dir == FACING_RIGHT) ? (NINJA_TILES_W - 1 - col) : col;
        u16 attr = (1 << 8) | (dir == FACING_RIGHT ? 1 : 0);  /* palette 1, H-flip */

        *REG_VRAMMOD = 1;
        *REG_VRAMADDR = ADDR_SCB1 + (FIRST_SPRITE + col) * 64;
        for (u16 row = 0; row < NINJA_TILES_H; row++) {
            *REG_VRAMRW = WALK_TILE + row * NINJA_SHEET_W + f * NINJA_TILES_W + src;
            *REG_VRAMRW = attr;
        }
    }
}


/*
 * Position the character.
 *
 * Only the first sprite of the chain carries a position: the others set their
 * "sticky" bit and inherit Y, height and vertical shrink from it, sitting
 * immediately to its right.
 */
static void move_walker_to(s16 x, s16 y) {
    *REG_VRAMMOD = ADDR_SCB4 - ADDR_SCB3;   /* so SCB4 follows SCB3 */
    *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE;
    *REG_VRAMRW = (((496 - y) & 0x1ff) << 7) | NINJA_TILES_H;
    *REG_VRAMRW = (x & 0x1ff) << 7;
}


static void init_walker(void) {
    set_frame(0, facing);

    /* SCB2: no shrinking. The chained sprites also need the sticky bit in
       SCB3, which is what glues them to the sprite before them. */
    for (u16 col = 0; col < NINJA_TILES_W; col++) {
        *REG_VRAMMOD = 0;
        *REG_VRAMADDR = ADDR_SCB2 + FIRST_SPRITE + col;
        *REG_VRAMRW = 0xfff;

        if (col > 0) {
            *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE + col;
            *REG_VRAMRW = 1 << 6;          /* sticky: follow the previous one */
        }
    }

    move_walker_to(walker_x, walker_y);
}


/*
 * Read player 1's joystick straight from the hardware.
 *
 * REG_P1CNT is active low - a bit reads 0 while its switch is held - so invert
 * it to get the "1 means pressed" convention of the CNT_* masks.
 */
static u8 read_p1(void) {
    return (u8)~(*REG_P1CNT);
}


static void update_walker(void) {
    u8 pad = read_p1();
    u8 moving = 0;

    if (pad & CNT_LEFT) {
        walker_x -= SPEED;
        facing = FACING_LEFT;
        moving = 1;
    } else if (pad & CNT_RIGHT) {
        walker_x += SPEED;
        facing = FACING_RIGHT;
        moving = 1;
    }

    if (walker_x < 0) { walker_x = 0; }
    if (walker_x > SCREEN_W - CHAR_W) { walker_x = SCREEN_W - CHAR_W; }

    if (moving) {
        if (++anim_tick >= ANIM_RATE) {
            anim_tick = 0;
            frame = (frame + 1) % NINJA_FRAMES;
        }
    } else {
        frame = 0;          /* standing still: rest on the first frame */
        anim_tick = 0;
    }

    set_frame(frame, facing);
    move_walker_to(walker_x, walker_y);
}


/* The cartridge runtime calls this back on every Vertical Blank interrupt,
   which is our frame clock: 59.6 Hz on AES, 59.2 Hz on MVS. */
static volatile u8 vblank = 0;

void rom_callback_VBlank(void) {
    vblank = 1;
}

static void wait_vblank(void) {
    while (!vblank);
    vblank = 0;
}


int main(void) {
    ng_cls();
    init_palette();
    init_walker();

    ng_center_text(2, 0, "NEO GEO - ITERATION 2");
    ng_center_text(25, 0, "WALK WITH A AND D");

    char line[20];
    for (;;) {
        update_walker();

        snprintf(line, sizeof(line), "X %3d  FRAME %d", walker_x, frame);
        ng_text(12, 27, 0, line);

        wait_vblank();
    }
    return 0;
}
