/*
 * A character on a parallax stage that scrolls forever, with a title screen.
 *
 * On the Neo Geo a sprite is a vertical strip of tiles, and there is no
 * background layer at all: the stage is sprites too, numbered below the
 * character because higher-numbered sprites are drawn in front.
 *
 * The 68000 cannot reach the sound chip either. It writes a command byte to
 * REG_SOUND, the Z80 takes an NMI and plays the sample; src/user_commands.s
 * is the other half of that conversation.
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

/* Sound commands. These must stay in step with the jump table in
   src/user_commands.s; 0 to 3 are reserved by the sound driver. */
#define SND_RESET 3
#define SND_COIN 4
#define SND_JUMP 5
#define SND_PUNCH 6

static inline void play_sound(u8 command) {
    *REG_SOUND = command;
}

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


/* Defined with the rest of the frame handling, further down. */
static void wait_vblank(void);


/* Palette 0 draws the fix layer (the text), 1 the character, 2 the stage. */
static const u16 text_palette[16] = {
    0x8000, 0x0fff, 0x0666, 0x8000, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};      /*                  ^ colour 3: black, what the transition paints */


/*
 * Dim one colour to num/den of its brightness.
 *
 * A palette entry packs six bits per channel awkwardly: the top four bits of
 * each are together in the low half of the word, each channel's next bit is in
 * bits 14-12, and the lowest bit of all three is shared in bit 15 and stored
 * inverted. So the channels have to be unpacked, scaled and packed again.
 */
static u16 dim_color(u16 c, u16 num, u16 den) {
    u16 dark = ((c >> 15) & 1) ^ 1;
    u16 r = (u16)((((c >> 8) & 0xf) << 2) | (((c >> 14) & 1) << 1) | dark);
    u16 g = (u16)((((c >> 4) & 0xf) << 2) | (((c >> 13) & 1) << 1) | dark);
    u16 b = (u16)(((c & 0xf) << 2) | (((c >> 12) & 1) << 1) | dark);

    r = (u16)((u32)r * num / den);
    g = (u16)((u32)g * num / den);
    b = (u16)((u32)b * num / den);

    u16 nd = (((r & 1) + (g & 1) + (b & 1)) >= 2) ? 1 : 0;
    return (u16)(((nd ^ 1) << 15)
                 | (((r >> 1) & 1) << 14) | (((g >> 1) & 1) << 13)
                 | (((b >> 1) & 1) << 12)
                 | (((r >> 2) & 0xf) << 8) | (((g >> 2) & 0xf) << 4)
                 | ((b >> 2) & 0xf));
}


/// Write all three palettes at `level`/FADE_STEPS of their brightness.
#define FADE_STEPS 9

static void set_brightness(u16 level) {
    for (u16 i = 0; i < 16; i++) {
        MMAP_PALBANK1[i] = dim_color(text_palette[i], level, FADE_STEPS);
        MMAP_PALBANK1[16 + i] = dim_color(hero_palette[i], level, FADE_STEPS);
        MMAP_PALBANK1[32 + i] = dim_color(stage_palette[i], level, FADE_STEPS);
    }
}


/*
 * Screen transition: a block dissolve.
 *
 * The fix layer is a 40x32 grid of 8x8 tiles drawn on top of every sprite, so
 * filling its cells with a solid tile hides the screen and clearing them
 * reveals it again a block at a time. 8x8 is as fine as this gets - the fix
 * grid is the hardware's, and nothing smaller exists.
 *
 * Each cell is given a pseudo-random step at which it flips, so the screen
 * breaks up in a scatter rather than a sweep. One step per frame, so the whole
 * thing takes DISSOLVE_STEPS frames, and each frame only touches the cells
 * belonging to that step.
 *
 * The fix layer also holds the text, so a dissolve wipes any text with it.
 * Screens therefore reveal first and draw their text afterwards.
 */
#define FIX_MAP 0x7000
#define FIX_COLS 40
#define FIX_ROWS 32
#define SOLID_TILE 1280                 /* straight after ngdevkit's font */
#define EMPTY_TILE 255                  /* transparent */
#define BLOCK_PALETTE 0                 /* colour 3 of it is black */
#define DISSOLVE_STEPS 16

/// Which step a cell flips on. Deliberately scrambled, not a sweep.
static u8 cell_step(u16 col, u16 row) {
    u16 h = (u16)(col * 37u + row * 101u + ((col ^ row) << 3));
    h ^= (u16)(h >> 5);
    return (u8)(h % DISSOLVE_STEPS);
}

static void fix_put(u16 col, u16 row, u16 entry) {
    *REG_VRAMADDR = FIX_MAP + col * 32 + row;
    *REG_VRAMRW = entry;
}

/// Cover every cell at once, with no animation.
static void cover_screen(void) {
    *REG_VRAMMOD = 1;
    for (u16 col = 0; col < FIX_COLS; col++) {
        *REG_VRAMADDR = FIX_MAP + col * 32;
        for (u16 row = 0; row < FIX_ROWS; row++) {
            *REG_VRAMRW = (BLOCK_PALETTE << 12) | SOLID_TILE;
        }
    }
}

/// Flip every cell to `entry`, a step per frame, scattered.
static void dissolve(u16 entry) {
    for (u8 step = 0; step < DISSOLVE_STEPS; step++) {
        *REG_VRAMMOD = 0;
        for (u16 col = 0; col < FIX_COLS; col++) {
            for (u16 row = 0; row < FIX_ROWS; row++) {
                if (cell_step(col, row) == step) {
                    fix_put(col, row, entry);
                }
            }
        }
        wait_vblank();
    }
}

/// Break the screen up into blocks until it is covered.
static void dissolve_out(void) {
    dissolve((BLOCK_PALETTE << 12) | SOLID_TILE);
}

/// Clear the blocks away to reveal whatever the sprites are showing.
static void dissolve_in(void) {
    dissolve((BLOCK_PALETTE << 12) | EMPTY_TILE);
}


/*
 * Fade the screen, about 150 ms each way.
 *
 * This replaces an earlier attempt that scaled every sprite with the
 * hardware's shrink. That grows the scene out of a point, but the hardware can
 * only shrink and never grow past full size, so a full-screen image scaled
 * down always leaves the screen's edges empty around it. A fade has nothing to
 * leave empty, and it dims the text too, which no sprite effect can.
 */
static void fade_to_black(void) {
    for (s16 l = FADE_STEPS; l >= 0; l--) {
        set_brightness((u16)l);
        wait_vblank();
    }
}

static void fade_from_black(void) {
    for (u16 l = 0; l <= FADE_STEPS; l++) {
        set_brightness(l);
        wait_vblank();
    }
}


static void init_palette(void) {
    set_brightness(FADE_STEPS);
    /* The backdrop shows wherever no sprite is drawn: the last colour of the
       bank. Black, so a screen with the stage hidden is plain black. */
    MMAP_PALBANK1[4095] = 0x8000;
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


/// Player 1's Start button, which lives in a different register to the stick.
/// Active low as well, so the same inversion applies.
static u8 read_start(void) {
    return (u8)~(*REG_STATUS_B) & CNT_START1;
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
        play_sound(SND_PUNCH);
    } else if (pressed & CNT_UP) {
        set_state(ST_JUMP);
        hero_vy = JUMP_SPEED;
        frame = 1;
        play_sound(SND_JUMP);
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


/*
 * The title screen.
 *
 * The stage stays on screen behind it - it costs nothing, since those sprites
 * are already set up, and an empty parallax backdrop makes a better title card
 * than a black screen. Only the character is hidden.
 */

#define MENU_START 0
#define MENU_QUIT 1
#define MENU_ITEMS 2

static const u8 menu_row[MENU_ITEMS] = { 17, 19 };
static const char *menu_label[MENU_ITEMS] = { "START", "QUIT" };


/*
 * Switch the stage off. A sprite whose height is zero is not drawn, so the
 * screen falls back to the backdrop colour - black. The title screen wants a
 * clean background rather than the game showing through behind it.
 *
 * The sky is a sticky chain and its followers inherit the leader's height, so
 * only the leader needs changing; the scrolling layers each carry their own.
 */
static void show_stage(u8 visible) {
    *REG_VRAMMOD = 0;

    *REG_VRAMADDR = ADDR_SCB3 + SKY_SPRITE;
    *REG_VRAMRW = (((496 - STAGE_SKY_Y) & 0x1ff) << 7)
                  | (visible ? STAGE_SKY_ROWS : 0);

    for (u16 i = 0; i <= STAGE_COLS; i++) {
        *REG_VRAMADDR = ADDR_SCB3 + HILLS_SPRITE + i;
        *REG_VRAMRW = (((496 - STAGE_HILLS_Y) & 0x1ff) << 7)
                      | (visible ? STAGE_HILLS_ROWS : 0);
        *REG_VRAMADDR = ADDR_SCB3 + GROUND_SPRITE + i;
        *REG_VRAMRW = (((496 - STAGE_GROUND_Y) & 0x1ff) << 7)
                      | (visible ? STAGE_GROUND_ROWS : 0);
    }
}


/// A sprite with a height of zero is switched off, which is how the character
/// is kept out of the way on the title screen.
static void show_hero(u8 visible) {
    *REG_VRAMMOD = 0;
    *REG_VRAMADDR = ADDR_SCB3 + FIRST_SPRITE;
    *REG_VRAMRW = (((496 - hero_y) & 0x1ff) << 7) | (visible ? HERO_TILES_H : 0);
}


static void draw_menu(u8 selected) {
    for (u8 i = 0; i < MENU_ITEMS; i++) {
        /* The cursor is part of the string so that clearing it needs no
           separate erase: the same width is always written. */
        char line[16];
        const char *label = menu_label[i];
        u8 n = 0;
        line[n++] = (i == selected) ? '>' : ' ';
        line[n++] = ' ';
        while (*label) { line[n++] = *label++; }
        line[n++] = ' ';
        line[n++] = (i == selected) ? '<' : ' ';
        line[n] = '\0';
        ng_center_text(menu_row[i], 0, line);
    }
}


/// Runs the title screen until the player picks something. Returns the choice.
static u8 title_screen(void) {
    u8 selected = MENU_START;
    u8 prev_start = 0;

    ng_cls();
    cover_screen();
    show_hero(0);
    show_stage(0);
    dissolve_in();

    /* Text goes on after the dissolve: it lives in the same fix layer the
       dissolve paints over, so anything drawn first would be wiped. */
    ng_center_text(8, 0, "T H E   W A N D E R E R");
    ng_center_text(11, 0, "A NEO GEO GAME");
    draw_menu(selected);
    ng_center_text(25, 0, "W S TO CHOOSE   ENTER OR J TO PICK");

    for (;;) {
        u8 pad = read_p1();
        u8 pressed = pad & ~prev_pad;
        prev_pad = pad;

        if (pressed & (CNT_UP | CNT_DOWN)) {
            selected = (selected + 1) % MENU_ITEMS;   /* only two entries */
            draw_menu(selected);
        }
        u8 start = read_start();
        u8 start_pressed = start & ~prev_start;
        prev_start = start;

        if ((pressed & (CNT_A | CNT_B | CNT_C | CNT_D)) || start_pressed) {
            return selected;
        }

        wait_vblank();
    }
}


int main(void) {
    ng_cls();
    init_palette();
    init_sky();
    init_scrolling_layer(HILLS_SPRITE, STAGE_HILLS_ROWS, STAGE_HILLS_Y);
    init_scrolling_layer(GROUND_SPRITE, STAGE_GROUND_ROWS, STAGE_GROUND_Y);
    init_hero();

    /* Lay the scrolling layers out once before anything is drawn. Their
       columns only get an X when they are scrolled, and until then they would
       all sit stacked at the left edge. */
    scroll_layer(HILLS_SPRITE, HILLS_TILE, STAGE_HILLS_ROWS, 0, &hills_tile_scroll);
    scroll_layer(GROUND_SPRITE, GROUND_TILE, STAGE_GROUND_ROWS, 0, &ground_tile_scroll);

    /* Put the sound driver in a known state before asking it for anything. */
    play_sound(SND_RESET);

    /* Start covered, so the first screen reveals like every other one. */
    cover_screen();

    for (;;) {
        if (title_screen() == MENU_QUIT) {
            dissolve_out();

            /* A cartridge has nowhere to quit to, so this is as far as it
               goes: say goodbye, then offer the title screen again. */
            ng_cls();
            cover_screen();
            show_hero(0);
            show_stage(0);
            dissolve_in();
            ng_center_text(13, 0, "THANKS FOR PLAYING");
            for (u16 i = 0; i < 120; i++) {
                wait_vblank();
            }
            dissolve_out();
            continue;
        }

        dissolve_out();
        play_sound(SND_COIN);

        ng_cls();
        cover_screen();
        show_stage(1);

        /* Draw the character where it will actually stand before anything is
           shown. Without this it sits at x=0 for the whole fade and then jumps
           to the middle on the first frame of play. */
        set_frame(HERO_WALK_ROW, 0, facing);
        move_hero_to(hero_world_x - camera_x, hero_y);
        show_hero(1);

        dissolve_in();
        ng_center_text(2, 0, "A D WALK  W JUMP  S CROUCH  J HIT");


        for (;;) {
            update_hero();

            /* The far hills move at a quarter of the floor's speed, which is
               what makes them read as distant. */
            scroll_layer(HILLS_SPRITE, HILLS_TILE, STAGE_HILLS_ROWS,
                         camera_x >> 2, &hills_tile_scroll);
            scroll_layer(GROUND_SPRITE, GROUND_TILE, STAGE_GROUND_ROWS,
                         camera_x, &ground_tile_scroll);


            wait_vblank();
        }
    }
    return 0;
}
