/*
 * Iteration 3: a character with walk, jump, crouch and attack.
 *
 * On the Neo Geo a sprite is a vertical strip of tiles, so a character this
 * wide is several sprites side by side, chained with the "sticky" bit: only
 * the first carries a position and each of the others follows the one before.
 */

#include <ngdevkit/neogeo.h>
#include <ngdevkit/ng-fix.h>
#include <stdio.h>
#include "hero.h"

/// The BIOS eye-catcher owns C ROM tiles 0-255, so ours start at 256.
#define HERO_TILE 256
/// Sprite 0 is never drawn by the LSPC, so the first usable sprite is 1.
#define FIRST_SPRITE 1

#define SCREEN_W 320
#define SCREEN_H 224
#define CHAR_W (HERO_TILES_W * 16)
#define CHAR_H (HERO_TILES_H * 16)

#define WALK_SPEED 2
#define JUMP_SPEED (-9)
#define GRAVITY 1
#define GROUND_Y (SCREEN_H - CHAR_H - 26)

/* Game frames each animation frame is held for. */
#define WALK_RATE 4
#define ATTACK_RATE 3
#define CROUCH_RATE 3

/// The art faces right, so walking left is the mirrored one.
#define FACING_RIGHT 0
#define FACING_LEFT 1

enum state {
    ST_IDLE,
    ST_WALK,
    ST_JUMP,
    ST_CROUCH,
    ST_ATTACK,
};

static s16 hero_x = (SCREEN_W - CHAR_W) / 2;
static s16 hero_y = GROUND_Y;
static s16 hero_vy = 0;
static u8 facing = FACING_RIGHT;
static enum state state = ST_IDLE;
static u8 frame = 0;
static u8 tick = 0;
/* Edge detection, so holding a button does not retrigger the action. */
static u8 prev_pad = 0;


static void init_palette(void) {
    /* Palette 0 draws the fix layer (the text), palette 1 the character. */
    static const u16 text_palette[16] = {
        0x8000, 0x0fff, 0x0666, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    for (u16 i = 0; i < 16; i++) {
        MMAP_PALBANK1[i] = text_palette[i];
        MMAP_PALBANK1[16 + i] = hero_palette[i];
    }
}


/*
 * Load one animation frame into the sprites' tile maps.
 *
 * Tiles run left to right then top to bottom, so the tile at column c, row r
 * of frame f of an animation starting at tile row `anim_row` is
 * HERO_TILE + (anim_row + r) * HERO_SHEET_W + f * HERO_TILES_W + c.
 *
 * Mirroring is not only a per-tile flag: the columns have to be emitted in
 * reverse order too, or the character is assembled back to front.
 */
static void set_frame(u16 anim_row, u8 f, u8 dir) {
    u16 attr = (1 << 8) | (dir == FACING_LEFT ? 1 : 0);   /* palette 1, H-flip */

    for (u16 col = 0; col < HERO_TILES_W; col++) {
        u16 src = (dir == FACING_LEFT) ? (HERO_TILES_W - 1 - col) : col;

        *REG_VRAMMOD = 1;
        *REG_VRAMADDR = ADDR_SCB1 + (FIRST_SPRITE + col) * 64;
        for (u16 row = 0; row < HERO_TILES_H; row++) {
            *REG_VRAMRW = HERO_TILE + (anim_row + row) * HERO_SHEET_W
                          + f * HERO_TILES_W + src;
            *REG_VRAMRW = attr;
        }
    }
}


/*
 * Position the character. Only the first sprite of the chain carries a
 * position; the others set their sticky bit and inherit it.
 */
static void move_hero_to(s16 x, s16 y) {
    *REG_VRAMMOD = ADDR_SCB4 - ADDR_SCB3;   /* so SCB4 follows SCB3 */
    *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE;
    *REG_VRAMRW = (((496 - y) & 0x1ff) << 7) | HERO_TILES_H;
    *REG_VRAMRW = (x & 0x1ff) << 7;
}


static void init_hero(void) {
    set_frame(HERO_WALK_ROW, 0, facing);

    for (u16 col = 0; col < HERO_TILES_W; col++) {
        *REG_VRAMMOD = 0;
        *REG_VRAMADDR = ADDR_SCB2 + FIRST_SPRITE + col;
        *REG_VRAMRW = 0xfff;                /* no shrinking */

        if (col > 0) {
            *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE + col;
            *REG_VRAMRW = 1 << 6;           /* sticky: follow the previous one */
        }
    }

    move_hero_to(hero_x, hero_y);
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


/// Advance `frame`, stopping on the last one. Returns 1 when the end is reached.
static u8 advance_once(u8 frames, u8 rate) {
    if (frame + 1 >= frames) {
        return 1;
    }
    if (++tick >= rate) {
        tick = 0;
        frame++;
    }
    return 0;
}

/// Advance `frame`, looping back to the start.
static void advance_loop(u8 frames, u8 rate) {
    if (++tick >= rate) {
        tick = 0;
        frame = (frame + 1) % frames;
    }
}


static void set_state(enum state s) {
    if (state != s) {
        state = s;
        frame = 0;
        tick = 0;
    }
}


static void update_hero(void) {
    u8 pad = read_p1();
    u8 pressed = pad & ~prev_pad;       /* newly pressed this frame */
    prev_pad = pad;

    /* Attacking and jumping run to completion; they are not interrupted. */
    if (state == ST_ATTACK) {
        if (advance_once(HERO_ATTACK_FRAMES, ATTACK_RATE)) {
            set_state(ST_IDLE);
        }
    } else if (state == ST_JUMP) {
        /* Steering in mid-air is allowed, which is what makes a jump feel
           controllable rather than committed. */
        if (pad & CNT_LEFT) { hero_x -= WALK_SPEED; facing = FACING_LEFT; }
        if (pad & CNT_RIGHT) { hero_x += WALK_SPEED; facing = FACING_RIGHT; }

        hero_y += hero_vy;
        hero_vy += GRAVITY;
        if (hero_y >= GROUND_Y) {
            hero_y = GROUND_Y;
            hero_vy = 0;
            set_state(ST_IDLE);
        } else {
            /* Map the arc onto the animation: rising uses the early frames,
               falling the later ones. */
            u8 f = (hero_vy < 0) ? 2 : 3;
            frame = (hero_y >= GROUND_Y - 8) ? 4 : f;
        }
    } else if (pressed & CNT_A) {
        set_state(ST_ATTACK);
    } else if (pressed & CNT_UP) {
        set_state(ST_JUMP);
        hero_vy = JUMP_SPEED;
        frame = 1;
    } else if (pad & CNT_DOWN) {
        set_state(ST_CROUCH);
        advance_once(HERO_CROUCH_FRAMES, CROUCH_RATE);
    } else if (pad & (CNT_LEFT | CNT_RIGHT)) {
        set_state(ST_WALK);
        if (pad & CNT_LEFT) { hero_x -= WALK_SPEED; facing = FACING_LEFT; }
        else { hero_x += WALK_SPEED; facing = FACING_RIGHT; }
        advance_loop(HERO_WALK_FRAMES, WALK_RATE);
    } else {
        set_state(ST_IDLE);
        frame = 0;
    }

    if (hero_x < 0) { hero_x = 0; }
    if (hero_x > SCREEN_W - CHAR_W) { hero_x = SCREEN_W - CHAR_W; }

    u16 row;
    switch (state) {
    case ST_ATTACK: row = HERO_ATTACK_ROW; break;
    case ST_JUMP:   row = HERO_JUMP_ROW;   break;
    case ST_CROUCH: row = HERO_CROUCH_ROW; break;
    default:        row = HERO_WALK_ROW;   break;   /* idle rests on walk[0] */
    }

    set_frame(row, frame, facing);
    move_hero_to(hero_x, hero_y);
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
    init_hero();

    ng_center_text(2, 0, "NEO GEO - ITERATION 3");
    ng_center_text(27, 0, "A D WALK  W JUMP  S CROUCH  J HIT");

    for (;;) {
        update_hero();
        wait_vblank();
    }
    return 0;
}
