/*
 * supersprite_vdp.c -- See supersprite_vdp.h.
 *
 * Software TMS9918A renderer. Reads the VDP registers + VRAM out of the PL
 * front end through the card-control register window and draws the picture
 * into a 256x192 BGRA32 buffer for the compositor to black-key overlay.
 *
 * VRAM is fetched through the CARD_CTRL_SS_VRAM_ADDR / _VRAM_DATA window: one
 * AXI write (address) + one AXI read (data) per byte, so it is not free. We
 * read only the tables the active mode needs and only when the VDP frame
 * counter advances. Heavy full-screen Graphics II updates can render below
 * 50/60 Hz because every VRAM byte requires two AXI transactions.
 */

#include <string.h>

#include "supersprite_vdp.h"

#include "xiltimer.h"

#include "../lib/common.h"
/* The VDP frame stays BGRA32: the compositor's overlay keys on the
 * 24-bit RGB field (black = transparent) and narrows to 565 only at
 * the output write. */
#define SS_BGRA(r, g, b)     (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#include "card_control_regs.h"

/* ---- TMS9918A palette (index -> BGRA32). Index 0 = transparent. ---- */
static const uint32_t k_tms_palette[16] = {
    SS_BGRA(0x00, 0x00, 0x00), /* 0  transparent (drawn as backdrop) */
    SS_BGRA(0x00, 0x00, 0x00), /* 1  black */
    SS_BGRA(0x21, 0xC8, 0x42), /* 2  medium green */
    SS_BGRA(0x5E, 0xDC, 0x78), /* 3  light green */
    SS_BGRA(0x54, 0x55, 0xED), /* 4  dark blue */
    SS_BGRA(0x7D, 0x76, 0xFC), /* 5  light blue */
    SS_BGRA(0xD4, 0x52, 0x4D), /* 6  dark red */
    SS_BGRA(0x42, 0xEB, 0xF5), /* 7  cyan */
    SS_BGRA(0xFC, 0x55, 0x54), /* 8  medium red */
    SS_BGRA(0xFF, 0x79, 0x78), /* 9  light red */
    SS_BGRA(0xD4, 0xC1, 0x54), /* 10 dark yellow */
    SS_BGRA(0xE6, 0xCE, 0x80), /* 11 light yellow */
    SS_BGRA(0x21, 0xB0, 0x3B), /* 12 dark green */
    SS_BGRA(0xC9, 0x5B, 0xBA), /* 13 magenta */
    SS_BGRA(0xCC, 0xCC, 0xCC), /* 14 gray */
    SS_BGRA(0xFF, 0xFF, 0xFF), /* 15 white */
};

static uint32_t s_pixels[SS_VDP_WIDTH * SS_VDP_HEIGHT];
static uint8_t  s_opaque[SS_VDP_WIDTH * SS_VDP_HEIGHT]; /* 1 = sprite/tile pixel */
static ss_vdp_frame_t s_frame;
static XTime    s_last_render_time = 0;
static uint8_t  s_have_rendered = 0u;
static uint8_t  s_force_active = 0u;

/* Cap the VDP render rate: the VRAM read window is slow, so re-rendering on
 * every compositor tick would monopolize the main loop. ~20 Hz is plenty for
 * the overlay and leaves the loop responsive. */
#define SS_RENDER_HZ 20u

void supersprite_vdp_set_force_active(uint8_t on) { s_force_active = on ? 1u : 0u; }
uint8_t supersprite_vdp_get_force_active(void) { return s_force_active; }

/* ---- Register access via the card-control window ---- */

static void ss_read_regs(uint8_t r[8])
{
    uint32_t lo = REG_READ(CARD_CTRL_SS_REGS_LO_REG);
    uint32_t hi = REG_READ(CARD_CTRL_SS_REGS_HI_REG);
    r[0] = (uint8_t)(lo);       r[1] = (uint8_t)(lo >> 8);
    r[2] = (uint8_t)(lo >> 16); r[3] = (uint8_t)(lo >> 24);
    r[4] = (uint8_t)(hi);       r[5] = (uint8_t)(hi >> 8);
    r[6] = (uint8_t)(hi >> 16); r[7] = (uint8_t)(hi >> 24);
}

/* Read `len` bytes of VRAM starting at `addr` into `dst`. One write + read
 * per byte (the PL window does not auto-increment). */
static void ss_vram_read(uint8_t *dst, uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        REG_WRITE(CARD_CTRL_SS_VRAM_ADDR_REG, (addr + i) & 0x3FFFu);
        dst[i] = (uint8_t)(REG_READ(CARD_CTRL_SS_VRAM_DATA_REG) & 0xFFu);
    }
}

/* ---- Table scratch (sized for the largest mode that uses each) ---- */
static uint8_t s_name[960];       /* name table (text uses 960) */
static uint8_t s_pattern[0x2000]; /* pattern generator (GII: 6 KB used) */
static uint8_t s_color[0x2000];   /* color table (GII: 6 KB used) */
static uint8_t s_spr_attr[128];   /* sprite attribute table */
static uint8_t s_spr_patt[0x800]; /* sprite pattern generator (2 KB) */

static inline void put_px(int x, int y, uint8_t color, uint8_t backdrop)
{
    if ((unsigned)x >= SS_VDP_WIDTH || (unsigned)y >= SS_VDP_HEIGHT) return;
    uint8_t c = (color == 0u) ? backdrop : color;
    s_pixels[y * SS_VDP_WIDTH + x] = k_tms_palette[c & 0x0Fu];
}

static void fill_backdrop(uint8_t backdrop)
{
    uint32_t v = k_tms_palette[backdrop & 0x0Fu];
    for (int i = 0; i < SS_VDP_WIDTH * SS_VDP_HEIGHT; ++i) s_pixels[i] = v;
    memset(s_opaque, 0, sizeof(s_opaque));
}

/* ---- Tile modes ---- */

static void render_graphics1(const uint8_t r[8], uint8_t backdrop)
{
    uint32_t name_base = (uint32_t)(r[2] & 0x0F) << 10;
    uint32_t patt_base = (uint32_t)(r[4] & 0x07) << 11;
    uint32_t color_base = (uint32_t)r[3] << 6;

    ss_vram_read(s_name, name_base, 768);
    ss_vram_read(s_pattern, patt_base, 0x800);
    ss_vram_read(s_color, color_base, 32);

    for (int row = 0; row < 24; ++row) {
        for (int col = 0; col < 32; ++col) {
            uint8_t name = s_name[row * 32 + col];
            uint8_t clr = s_color[name >> 3];
            uint8_t fg = clr >> 4, bg = clr & 0x0F;
            for (int py = 0; py < 8; ++py) {
                uint8_t bits = s_pattern[name * 8 + py];
                for (int px = 0; px < 8; ++px) {
                    uint8_t on = (bits & (0x80 >> px)) != 0;
                    put_px(col * 8 + px, row * 8 + py, on ? fg : bg, backdrop);
                }
            }
        }
    }
}

static void render_graphics2(const uint8_t r[8], uint8_t backdrop)
{
    uint32_t name_base = (uint32_t)(r[2] & 0x0F) << 10;
    uint32_t patt_base = (r[4] & 0x04) ? 0x2000u : 0x0000u;
    uint32_t color_base = (r[3] & 0x80) ? 0x2000u : 0x0000u;

    ss_vram_read(s_name, name_base, 768);
    ss_vram_read(s_pattern, patt_base, 0x1800); /* 3 x 2 KB banks */
    ss_vram_read(s_color, color_base, 0x1800);

    for (int row = 0; row < 24; ++row) {
        int third = row >> 3;
        for (int col = 0; col < 32; ++col) {
            uint8_t name = s_name[row * 32 + col];
            uint32_t idx = ((uint32_t)third * 256u + name) * 8u;
            for (int py = 0; py < 8; ++py) {
                uint8_t bits = s_pattern[idx + py];
                uint8_t clr = s_color[idx + py];
                uint8_t fg = clr >> 4, bg = clr & 0x0F;
                for (int px = 0; px < 8; ++px) {
                    uint8_t on = (bits & (0x80 >> px)) != 0;
                    put_px(col * 8 + px, row * 8 + py, on ? fg : bg, backdrop);
                }
            }
        }
    }
}

static void render_text(const uint8_t r[8], uint8_t backdrop)
{
    uint32_t name_base = (uint32_t)(r[2] & 0x0F) << 10;
    uint32_t patt_base = (uint32_t)(r[4] & 0x07) << 11;
    uint8_t fg = r[7] >> 4, bg = r[7] & 0x0F;

    ss_vram_read(s_name, name_base, 960);
    ss_vram_read(s_pattern, patt_base, 0x800);

    for (int row = 0; row < 24; ++row) {
        for (int col = 0; col < 40; ++col) {
            uint8_t name = s_name[row * 40 + col];
            for (int py = 0; py < 8; ++py) {
                uint8_t bits = s_pattern[name * 8 + py];
                for (int px = 0; px < 6; ++px) { /* 6 px wide glyphs */
                    uint8_t on = (bits & (0x80 >> px)) != 0;
                    put_px(col * 6 + px, row * 8 + py, on ? fg : bg, backdrop);
                }
            }
        }
    }
    (void)backdrop;
}

static void render_multicolor(const uint8_t r[8], uint8_t backdrop)
{
    uint32_t name_base = (uint32_t)(r[2] & 0x0F) << 10;
    uint32_t patt_base = (uint32_t)(r[4] & 0x07) << 11;

    ss_vram_read(s_name, name_base, 768);
    ss_vram_read(s_pattern, patt_base, 0x800);

    /* 64x48 grid of 4x4 blocks. Each 8x8 cell draws two color nibbles per
     * 4-row half, selected by the cell's screen row within the third. */
    for (int row = 0; row < 24; ++row) {
        for (int col = 0; col < 32; ++col) {
            uint8_t name = s_name[row * 32 + col];
            for (int half = 0; half < 2; ++half) {
                uint8_t b = s_pattern[name * 8 + ((row & 3) * 2) + half];
                uint8_t left = b >> 4, right = b & 0x0F;
                for (int py = 0; py < 4; ++py) {
                    int y = row * 8 + half * 4 + py;
                    for (int px = 0; px < 4; ++px) {
                        put_px(col * 8 + px, y, left, backdrop);
                        put_px(col * 8 + 4 + px, y, right, backdrop);
                    }
                }
            }
        }
    }
}

/* ---- Sprites (modes 0/2/3) ---- */

static void render_sprites(const uint8_t r[8], uint8_t *coincidence,
                           uint8_t *five_flag, uint8_t *fifth_num)
{
    uint32_t attr_base = (uint32_t)(r[5] & 0x7F) << 7;
    uint32_t patt_base = (uint32_t)(r[6] & 0x07) << 11;
    int size16 = (r[1] & 0x02) != 0;
    int mag = (r[1] & 0x01) ? 2 : 1;
    int dim = (size16 ? 16 : 8) * mag;

    ss_vram_read(s_spr_attr, attr_base, 128);
    ss_vram_read(s_spr_patt, patt_base, 0x800);

    uint8_t line_count[SS_VDP_HEIGHT];
    memset(line_count, 0, sizeof(line_count));
    *coincidence = 0; *five_flag = 0; *fifth_num = 31;

    /* Sprite-list termination: scanning 0->31, the FIRST sprite whose Y == 0xD0
     * (208) ends the list -- that sprite AND every higher-numbered one are not
     * displayed. This is how software makes sprites vanish, so it must gate the
     * whole draw, not merely skip the terminator entry. */
    int last = 32;
    for (int i = 0; i < 32; ++i) {
        if (s_spr_attr[i * 4 + 0] == 0xD0) { last = i; break; }
    }

    /* Pass 1 -- sprite-number order (0..last-1): apply the 4-sprites-per-
     * scanline limit. Only the first four sprites (by number) covering a line
     * are displayed; the fifth and beyond on that line are dropped and set the
     * 5S / fifth-sprite status. spr_draw[i] bit dy = sprite i may draw row dy
     * (also implies that row is on-screen). */
    uint32_t spr_draw[32];
    for (int i = 0; i < last; ++i) {
        int y = s_spr_attr[i * 4 + 0];
        int sy = ((y >= 0xE0) ? (y - 256) : y) + 1;
        uint32_t mask = 0;
        for (int dy = 0; dy < dim; ++dy) {
            int line = sy + dy;
            if (line < 0 || line >= SS_VDP_HEIGHT) continue;
            int n = ++line_count[line];
            if (n <= 4) {
                mask |= ((uint32_t)1u << dy);
            } else if (n == 5 && !*five_flag) {
                *five_flag = 1; *fifth_num = (uint8_t)i;
            }
        }
        spr_draw[i] = mask;
    }

    /* Pass 2 -- reverse for priority (sprite 0 lands on top). Draw only the
     * rows pass 1 cleared; a dropped or off-screen row has its bit clear. */
    for (int i = last - 1; i >= 0; --i) {
        int y = s_spr_attr[i * 4 + 0];
        int sy = ((y >= 0xE0) ? (y - 256) : y) + 1;
        int x = s_spr_attr[i * 4 + 1];
        uint8_t patt = s_spr_attr[i * 4 + 2];
        uint8_t c = s_spr_attr[i * 4 + 3];
        int sx = x - ((c & 0x80) ? 32 : 0);   /* early-clock */
        uint8_t color = c & 0x0F;
        if (size16) patt &= 0xFC;

        for (int dy = 0; dy < dim; ++dy) {
            if ((spr_draw[i] & ((uint32_t)1u << dy)) == 0u) {
                continue;                     /* dropped (5th+) or off-screen */
            }
            int line = sy + dy;
            int srow = dy / mag;              /* 0..15 or 0..7 */
            for (int dx = 0; dx < dim; ++dx) {
                int col = sx + dx;
                if ((unsigned)col >= SS_VDP_WIDTH) continue;
                int scol = dx / mag;          /* 0..15 or 0..7 */
                uint8_t bits, on;
                if (size16) {
                    int quad = (scol >= 8 ? 2 : 0);        /* left/right half */
                    bits = s_spr_patt[patt * 8 + quad * 8 + srow];
                    on = (bits & (0x80 >> (scol & 7))) != 0;
                } else {
                    bits = s_spr_patt[patt * 8 + srow];
                    on = (bits & (0x80 >> scol)) != 0;
                }
                if (!on) continue;
                int idx = line * SS_VDP_WIDTH + col;
                if (s_opaque[idx]) *coincidence = 1;   /* sprite/sprite hit */
                if (color != 0) {
                    s_pixels[idx] = k_tms_palette[color];
                }
                s_opaque[idx] = 1;
            }
        }
    }
}

/* ---- Top level ---- */

const ss_vdp_frame_t *supersprite_vdp_render(void)
{
    uint32_t features = REG_READ(CARD_CTRL_FEATURE_ENABLE_REG);

    /* When the card is disabled, touch NO SuperSprite-specific register --
     * only the always-present feature-enable reg above. This keeps the
     * compositor's per-frame call harmless when the card is off. */
    if (((features & CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT) == 0u) &&
        (s_force_active == 0u)) {
        s_frame.active = 0u;
        s_frame.pixels = NULL;
        s_frame.changed = 0u;
        return &s_frame;
    }

    uint32_t status = REG_READ(CARD_CTRL_SS_STATUS_REG);
    uint8_t overlay = (uint8_t)((status >> 25) & 1u);
    uint8_t apple_video = (uint8_t)((status >> 24) & 1u);

    s_frame.active = (overlay != 0u || s_force_active != 0u);
    s_frame.apple_video = apple_video;
    s_frame.changed = 0u;

    if (!s_frame.active) {
        s_frame.pixels = NULL;
        return &s_frame;
    }

    /* Rate-limit with a wall clock rather than the PL frame counter (which can
     * stall). Between renders, hold the last frame. Force re-renders always. */
    {
        XTime now = 0;
        XTime_GetTime(&now);
        if (!s_force_active && s_have_rendered &&
            (now - s_last_render_time) < (XTime)(COUNTS_PER_SECOND / SS_RENDER_HZ)) {
            s_frame.pixels = s_pixels;
            return &s_frame;
        }
        s_last_render_time = now;
    }

    uint8_t r[8];
    ss_read_regs(r);

    /* Blanked display (R1 bit6 = 0): hold the last rendered frame. Programs
     * blank transiently while updating VRAM, so clearing the overlay here
     * would make active graphics disappear intermittently. */
    if ((r[1] & 0x40u) == 0u) {
        s_frame.pixels = s_have_rendered ? s_pixels : NULL;
        return &s_frame;
    }

    s_have_rendered = 1u;
    s_frame.changed = 1u;
    uint8_t backdrop = r[7] & 0x0F;

    fill_backdrop(backdrop);

    int m1 = (r[1] & 0x10) != 0;  /* text */
    int m2 = (r[1] & 0x08) != 0;  /* multicolor */
    int m3 = (r[0] & 0x02) != 0;  /* graphics II */

    if (m1) {
        render_text(r, backdrop);          /* no sprites in text mode */
    } else {
        if (m2)       render_multicolor(r, backdrop);
        else if (m3)  render_graphics2(r, backdrop);
        else          render_graphics1(r, backdrop);

        uint8_t coincidence = 0, five = 0, fifth = 31;
        render_sprites(r, &coincidence, &five, &fifth);

        /* Report sprite status back to the PL: {5S, C, fifth_num[4:0]}. */
        uint32_t flags = ((uint32_t)(five ? 1u : 0u) << 6) |
                         ((uint32_t)(coincidence ? 1u : 0u) << 5) |
                         (uint32_t)(fifth & 0x1Fu);
        REG_WRITE(CARD_CTRL_SS_SPR_FLAGS_REG, flags);
    }

    s_frame.pixels = s_pixels;
    return &s_frame;
}
