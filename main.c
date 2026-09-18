/*
 * Iteration 1: a square you can move with the joystick.
 *
 * This is the smallest thing that is still a real Neo Geo cartridge: it boots
 * through the BIOS, sets up a palette, puts one 16x16 sprite in VRAM, reads
 * player 1's joystick every frame and moves the sprite.
 */

#include <ngdevkit/neogeo.h>
#include <ngdevkit/ng-fix.h>
#include <stdio.h>

/* ADDR_SCB1..4 come from ngdevkit's registers.h: the Sprite Control Blocks in
   VRAM. SCB1 holds 64 words per sprite (its tile map); SCB2/3/4 hold one word
   per sprite (shrink, Y + height, X). */

/// The BIOS eye-catcher owns C ROM tiles 0-255, so ours start at 256.
#define SQUARE_TILE 256
/// Sprite 0 is never drawn by the LSPC, so the first usable sprite is 1.
#define SQUARE_SPRITE 1

#define SCREEN_W 320
#define SCREEN_H 224
#define SQUARE_SIZE 16
#define SPEED 2

/* Position of the square, in screen pixels, top-left corner. */
static s16 square_x = (SCREEN_W - SQUARE_SIZE) / 2;
static s16 square_y = (SCREEN_H - SQUARE_SIZE) / 2;


/*
 * Palette 0 is used by the fix layer (the text), palette 1 by the square.
 *
 * Colour words are 16-bit: bit 15 is the shared "dark" bit, then one low bit
 * per channel, then 4 bits each of R, G and B. 0x8000 is black, 0x0fff white.
 */
static void init_palette(void) {
    static const u16 clut[] = {
        /* palette 0 - fix layer text */
        0x8000, 0x0fff, 0x0666, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        /* palette 1 - the square: index 1 is its fill, index 2 its border */
        0x8000, 0x0e33, 0x0fff, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };

    for (u16 i = 0; i < sizeof(clut) / sizeof(clut[0]); i++) {
        MMAP_PALBANK1[i] = clut[i];
    }
}


/*
 * Write the square's position into its sprite control blocks.
 *
 * The hardware counts Y upwards from the bottom: a sprite's first line is
 * (496 - Y). Both X and Y live in bits 15-7 of their word, and SCB3 carries
 * the sprite's height in tiles in its low bits.
 */
static void move_sprite_to(u16 sprite, s16 x, s16 y) {
    *REG_VRAMMOD = ADDR_SCB4 - ADDR_SCB3;   /* so SCB4 follows SCB3 */
    *REG_VRAMADDR = ADDR_SCB3 + sprite;
    *REG_VRAMRW = (((496 - y) & 0x1ff) << 7) | 1;   /* Y, 1 tile tall */
    *REG_VRAMRW = (x & 0x1ff) << 7;                 /* X */
}


static void init_sprite(u16 sprite, s16 x, s16 y) {
    /* SCB1: one tile, drawn with palette 1, no flipping. */
    *REG_VRAMMOD = 1;
    *REG_VRAMADDR = ADDR_SCB1 + (sprite * 64);
    *REG_VRAMRW = SQUARE_TILE;
    *REG_VRAMRW = 1 << 8;

    /* SCB2: no shrinking, full size. */
    *REG_VRAMMOD = 0;
    *REG_VRAMADDR = ADDR_SCB2 + sprite;
    *REG_VRAMRW = 0xfff;

    move_sprite_to(sprite, x, y);
}


/*
 * Read player 1's joystick straight from the hardware.
 *
 * REG_P1CNT is active low - a bit reads 0 while its switch is held - so invert
 * it to get the "1 means pressed" convention of the CNT_* masks. Going to the
 * register directly means we do not depend on the BIOS refreshing
 * bios_p1current for us.
 */
static u8 read_p1(void) {
    return (u8)~(*REG_P1CNT);
}


/// Read the joystick and move the square, keeping it on screen.
static void update_square(void) {
    u8 pad = read_p1();

    if (pad & CNT_UP)    { square_y -= SPEED; }
    if (pad & CNT_DOWN)  { square_y += SPEED; }
    if (pad & CNT_LEFT)  { square_x -= SPEED; }
    if (pad & CNT_RIGHT) { square_x += SPEED; }

    if (square_x < 0) { square_x = 0; }
    if (square_y < 0) { square_y = 0; }
    if (square_x > SCREEN_W - SQUARE_SIZE) { square_x = SCREEN_W - SQUARE_SIZE; }
    if (square_y > SCREEN_H - SQUARE_SIZE) { square_y = SCREEN_H - SQUARE_SIZE; }

    move_sprite_to(SQUARE_SPRITE, square_x, square_y);
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
    init_sprite(SQUARE_SPRITE, square_x, square_y);

    ng_center_text(2, 0, "NEO GEO - ITERATION 1");
    ng_center_text(25, 0, "MOVE WITH W A S D");

    char line[32];
    for (;;) {
        update_square();

        snprintf(line, sizeof(line), "X %3d  Y %3d", square_x, square_y);
        ng_text(14, 27, 0, line);

        wait_vblank();
    }
    return 0;
}
