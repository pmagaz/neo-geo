/*
 * Iteration 4: a character on a parallax stage that scrolls forever.
 *
 * On the Neo Geo a sprite is a vertical strip of tiles, and there is no
 * background layer at all: the stage is sprites too, numbered below the
 * character because higher-numbered sprites are drawn in front.
 */

#include <ngdevkit/neogeo.h>
#include <ngdevkit/ng-fix.h>
#include "hero.h"
#include "stage.h"

/* C ROM layout. The BIOS eye-catcher owns tiles 0-255, then each sheet is
   loaded after the one before it, in the order the makefile lists them. */
#define HERO_TILE 256
#define SKY_TILE (HERO_TILE + HERO_TILE_COUNT)
#define HILLS_TILE (SKY_TILE + STAGE_SKY_TILE_COUNT)
#define GROUND_TILE (HILLS_TILE + STAGE_HILLS_TILE_COUNT)

/*
 * Sprite numbering decides drawing order: higher numbers draw in front, and
 * sprite 0 is never drawn. The scrolling layers get one sprite more than the
 * screen is wide, so a column is always available to cover the seam as the
 * others slide left.
 */
#define SKY_SPRITE 1
#define HILLS_SPRITE (SKY_SPRITE + STAGE_COLS)
#define GROUND_SPRITE (HILLS_SPRITE + STAGE_COLS + 1)
#define FIRST_SPRITE (GROUND_SPRITE + STAGE_COLS + 1)

#define SCREEN_W 320
#define SCREEN_H 224
#define CHAR_W (HERO_TILES_W * 16)
#define CHAR_H (HERO_TILES_H * 16)

#define WALK_SPEED 2
#define JUMP_SPEED (-9)
#define GRAVITY 1
#define GROUND_LEVEL (STAGE_FLOOR_Y - CHAR_H)

/*
 * The camera only follows once the character leaves a dead zone in the middle
 * of the screen, so small steps do not drag the whole stage around. The zone
 * runs from 40% to 60% of the width, measured at the character's centre.
 */
#define CAM_RIGHT ((SCREEN_W * 60 / 100) - CHAR_W / 2)
#define CAM_LEFT ((SCREEN_W * 40 / 100) - CHAR_W / 2)

/*
 * Both scrolling layers repeat every 320 pixels, and the far layer moves at a
 * quarter speed, so the pair only lines up again every 4 * 320. Wrapping the
 * world there keeps the coordinates small without ever showing a seam.
 */
#define WORLD_WRAP (4 * 320)

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

static s16 hero_world_x = SCREEN_W / 2 - CHAR_W / 2;
static s16 camera_x = 0;
static s16 hero_y = GROUND_LEVEL;
static s16 hero_vy = 0;
static u8 facing = FACING_RIGHT;
static enum state state = ST_IDLE;
static u8 frame = 0;
static u8 tick = 0;
/* Edge detection, so holding a button does not retrigger the action. */
static u8 prev_pad = 0;
/* The tile column each scrolling layer is currently showing, so its tile maps
   are only rewritten when the scroll crosses a whole tile. */
static s16 hills_tile_scroll = -1;
static s16 ground_tile_scroll = -1;


static void init_palette(void) {
    /* Palette 0 draws the fix layer (the text), 1 the character, 2 the stage. */
    static const u16 text_palette[16] = {
        0x8000, 0x0fff, 0x0666, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };

    for (u16 i = 0; i < 16; i++) {
        MMAP_PALBANK1[i] = text_palette[i];
        MMAP_PALBANK1[16 + i] = hero_palette[i];
        MMAP_PALBANK1[32 + i] = stage_palette[i];
    }
}


/*
 * The sky never moves, so it is a plain sticky chain: only the leftmost
 * column carries a position and the rest follow it. Written once.
 */
static void init_sky(void) {
    for (u16 col = 0; col < STAGE_COLS; col++) {
        *REG_VRAMMOD = 1;
        *REG_VRAMADDR = ADDR_SCB1 + (SKY_SPRITE + col) * 64;
        for (u16 row = 0; row < STAGE_SKY_ROWS; row++) {
            *REG_VRAMRW = SKY_TILE + row * STAGE_COLS + col;
            *REG_VRAMRW = 2 << 8;               /* palette 2 */
        }

        *REG_VRAMMOD = 0;
        *REG_VRAMADDR = ADDR_SCB2 + SKY_SPRITE + col;
        *REG_VRAMRW = 0xfff;                    /* no shrinking */

        *REG_VRAMADDR = ADDR_SCB3 + SKY_SPRITE + col;
        if (col == 0) {
            *REG_VRAMRW = (((496 - STAGE_SKY_Y) & 0x1ff) << 7) | STAGE_SKY_ROWS;
        } else {
            *REG_VRAMRW = 1 << 6;               /* sticky: follow the previous */
        }
    }

    *REG_VRAMADDR = ADDR_SCB4 + SKY_SPRITE;
    *REG_VRAMRW = 0;
}


/*
 * A scrolling layer cannot use the sticky chain, because each column needs
 * its own X as the layer slides. Shrink and vertical position never change,
 * so they are set once here; only X is touched per frame.
 */
static void init_scrolling_layer(u16 first, u16 rows, s16 y) {
    for (u16 i = 0; i <= STAGE_COLS; i++) {
        *REG_VRAMMOD = 0;
        *REG_VRAMADDR = ADDR_SCB2 + first + i;
        *REG_VRAMRW = 0xfff;

        *REG_VRAMADDR = ADDR_SCB3 + first + i;
        *REG_VRAMRW = (((496 - y) & 0x1ff) << 7) | rows;
    }
}


/*
 * Point each column of a layer at a source column of the artwork.
 *
 * Only needed when the scroll crosses a whole tile: in between, the layer is
 * moved by changing X alone.
 */
static void layer_set_tiles(u16 first, u16 tile_base, u16 rows, u16 tile_scroll) {
    for (u16 i = 0; i <= STAGE_COLS; i++) {
        u16 src = i + tile_scroll;
        if (src >= STAGE_COLS) {
            src -= STAGE_COLS;      /* i and tile_scroll are both < COLS+1 */
        }

        *REG_VRAMMOD = 1;
        *REG_VRAMADDR = ADDR_SCB1 + (first + i) * 64;
        for (u16 row = 0; row < rows; row++) {
            *REG_VRAMRW = tile_base + row * STAGE_COLS + src;
            *REG_VRAMRW = 2 << 8;
        }
    }
}


/*
 * Slide a layer by `frac` pixels, 0-15.
 *
 * The leftmost column ends up at a negative X, which is exactly what is
 * wanted: X is nine bits and wraps at 512, and pixels at X >= 320 are off
 * screen, so a column placed just below 512 has its tail appear at the left
 * edge. That is what covers the seam.
 */
static void layer_set_x(u16 first, s16 frac) {
    for (u16 i = 0; i <= STAGE_COLS; i++) {
        *REG_VRAMMOD = 0;
        *REG_VRAMADDR = ADDR_SCB4 + first + i;
        *REG_VRAMRW = ((((s16)(i * 16)) - frac) & 0x1ff) << 7;
    }
}


static void scroll_layer(u16 first, u16 tile_base, u16 rows,
                         s16 scroll, s16 *last_tile_scroll) {
    /* Keep the scroll inside one repeat of the artwork, staying positive so
       the tile index and the pixel offset are both easy to reason about. */
    s16 s = scroll % (STAGE_COLS * 16);
    if (s < 0) {
        s += STAGE_COLS * 16;
    }

    s16 tile_scroll = s >> 4;
    if (tile_scroll != *last_tile_scroll) {
        layer_set_tiles(first, tile_base, rows, (u16)tile_scroll);
        *last_tile_scroll = tile_scroll;
    }
    layer_set_x(first, s & 15);
}


/*
 * Load one animation frame into the character's sprites.
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
        *REG_VRAMRW = 0xfff;

        if (col > 0) {
            *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE + col;
            *REG_VRAMRW = 1 << 6;           /* sticky: follow the previous one */
        }
    }
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
        if (pad & CNT_LEFT) { hero_world_x -= WALK_SPEED; facing = FACING_LEFT; }
        if (pad & CNT_RIGHT) { hero_world_x += WALK_SPEED; facing = FACING_RIGHT; }

        hero_y += hero_vy;
        hero_vy += GRAVITY;
        if (hero_y >= GROUND_LEVEL) {
            hero_y = GROUND_LEVEL;
            hero_vy = 0;
            set_state(ST_IDLE);
        } else {
            /* Map the arc onto the animation: rising uses the early frames,
               falling the later ones. */
            u8 f = (hero_vy < 0) ? 2 : 3;
            frame = (hero_y >= GROUND_LEVEL - 8) ? 4 : f;
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
        if (pad & CNT_LEFT) { hero_world_x -= WALK_SPEED; facing = FACING_LEFT; }
        else { hero_world_x += WALK_SPEED; facing = FACING_RIGHT; }
        advance_loop(HERO_WALK_FRAMES, WALK_RATE);
    } else {
        set_state(ST_IDLE);
        frame = 0;
    }

    /* Follow the character once it leaves the dead zone. */
    s16 screen_x = hero_world_x - camera_x;
    if (screen_x > CAM_RIGHT) {
        camera_x = hero_world_x - CAM_RIGHT;
    } else if (screen_x < CAM_LEFT) {
        camera_x = hero_world_x - CAM_LEFT;
    }

    /* Wrap the world rather than let the coordinates run away. Both the
       character and the camera move together, so nothing shifts on screen. */
    if (hero_world_x >= WORLD_WRAP) {
        hero_world_x -= WORLD_WRAP;
        camera_x -= WORLD_WRAP;
    } else if (hero_world_x < 0) {
        hero_world_x += WORLD_WRAP;
        camera_x += WORLD_WRAP;
    }

    u16 row;
    switch (state) {
    case ST_ATTACK: row = HERO_ATTACK_ROW; break;
    case ST_JUMP:   row = HERO_JUMP_ROW;   break;
    case ST_CROUCH: row = HERO_CROUCH_ROW; break;
    default:        row = HERO_WALK_ROW;   break;   /* idle rests on walk[0] */
    }

    set_frame(row, frame, facing);
    move_hero_to(hero_world_x - camera_x, hero_y);
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
    init_sky();
    init_scrolling_layer(HILLS_SPRITE, STAGE_HILLS_ROWS, STAGE_HILLS_Y);
    init_scrolling_layer(GROUND_SPRITE, STAGE_GROUND_ROWS, STAGE_GROUND_Y);
    init_hero();

    ng_center_text(2, 0, "A D WALK  W JUMP  S CROUCH  J HIT");

    for (;;) {
        update_hero();

        /* The far hills move at a quarter of the floor's speed, which is what
           makes them read as distant. */
        scroll_layer(HILLS_SPRITE, HILLS_TILE, STAGE_HILLS_ROWS,
                     camera_x >> 2, &hills_tile_scroll);
        scroll_layer(GROUND_SPRITE, GROUND_TILE, STAGE_GROUND_ROWS,
                     camera_x, &ground_tile_scroll);

        wait_vblank();
    }
    return 0;
}
