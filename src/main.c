// Odyssey GBC — Tile-as-Framebuffer Raycaster (v5)
// 20 rays, proper texture mapping: reads full 16px metatile row & shifts per column

#include <gb/gb.h>
#include <gb/cgb.h>
#include <stdint.h>
#include <string.h>

#include "../include/bg_tiles.h"
#include "../include/lut.h"
#include "../include/sprites_data.h"

// CGB DMA registers
#define HDMA1 (*(volatile uint8_t *)0xFF51)
#define HDMA2 (*(volatile uint8_t *)0xFF52)
#define HDMA3 (*(volatile uint8_t *)0xFF53)
#define HDMA4 (*(volatile uint8_t *)0xFF54)
#define HDMA5 (*(volatile uint8_t *)0xFF55)

#define MAP_W 16
#define MAP_H 16

const uint8_t world_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,0,0,1,1,1,1,0,0,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,0,0,1,1,1,1,1,1,1,1,0,0,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,1,0,0,1,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

#define SCR_W       20
#define VIEW_ROWS   10
#define VIEW_H      80
#define FB_SIZE     3200
#define HALF_FOV    20
#define MAX_DDA     24
#define DDA_CELL    16
#define MOVE_SPD    64
#define TURN_SPD    16

// wall_height_px[d] = min(480/d, 80) — WALL_PROJ=480, VIEW_H=80
const uint8_t wall_height_px[81] = {
     80,  80,  80,  80,  80,  80,  80,  68,  60,  53,  48,  43,  40,  36,  34,  32,
     30,  28,  26,  25,  24,  22,  21,  20,  20,  19,  18,  17,  17,  16,  16,  15,
     15,  14,  14,  13,  13,  12,  12,  12,  12,  11,  11,  11,  11,  10,  10,  10,
     10,   9,   9,   9,   9,   9,   9,   8,   8,   8,   8,   8,   8,   7,   7,   7,
      7,   7,   7,   7,   7,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
      6
};

uint8_t fb[FB_SIZE];
int16_t px, py;
uint8_t pa;

// Nibble-doubler: each bit becomes 2 bits (4 texels → 8 screen pixels)
const uint8_t dbl[16] = {
    0x00,0x03,0x0C,0x0F,0x30,0x33,0x3C,0x3F,
    0xC0,0xC3,0xCC,0xCF,0xF0,0xF3,0xFC,0xFF
};

// ================================================================
// Read 8 screen pixels at 2x scale from the 16px metatile row
// texX = horizontal texel offset (0-15), texY = row (0-15)
// Reads 4 consecutive texels starting at texX and doubles each to 2 pixels
// ================================================================
void read_wall_2x(uint8_t texY, uint8_t texX, uint8_t *lo, uint8_t *hi) {
    uint8_t local_y = texY & 7;
    uint16_t tile_L, tile_R;

    if (texY < 8) { tile_L = WALL_TL; tile_R = WALL_TR; }
    else          { tile_L = WALL_BL; tile_R = WALL_BR; }

    uint16_t off_L = tile_L * 16 + local_y * 2;
    uint16_t off_R = tile_R * 16 + local_y * 2;
    uint8_t lo_L = bg_tile_data[off_L];
    uint8_t hi_L = bg_tile_data[off_L + 1];
    uint8_t lo_R = bg_tile_data[off_R];
    uint8_t hi_R = bg_tile_data[off_R + 1];

    // Shift 16px row to start at texX, then take high nibble (4 texels)
    uint8_t raw_lo, raw_hi;
    if (texX == 0)       { raw_lo = lo_L; raw_hi = hi_L; }
    else if (texX == 8)  { raw_lo = lo_R; raw_hi = hi_R; }
    else if (texX < 8)   { raw_lo = (lo_L << texX) | (lo_R >> (8-texX));
                           raw_hi = (hi_L << texX) | (hi_R >> (8-texX)); }
    else                 { uint8_t s = texX-8;
                           raw_lo = (lo_R << s) | (lo_L >> (8-s));
                           raw_hi = (hi_R << s) | (hi_L >> (8-s)); }

    // 4x: take top 2 bits (2 texels), quadruple each to 4 screen pixels
    uint8_t b0 = (raw_lo >> 7) & 1;
    uint8_t b1 = (raw_lo >> 6) & 1;
    *lo = (b0 ? 0xF0 : 0x00) | (b1 ? 0x0F : 0x00);
    b0 = (raw_hi >> 7) & 1;
    b1 = (raw_hi >> 6) & 1;
    *hi = (b0 ? 0xF0 : 0x00) | (b1 ? 0x0F : 0x00);
}

// Same as read_wall_2x but for floor metatile
void read_floor_2x(uint8_t texY, uint8_t texX, uint8_t *lo, uint8_t *hi) {
    uint8_t local_y = texY & 7;
    uint16_t tile_L, tile_R;

    if (texY < 8) { tile_L = FLOOR_TL; tile_R = FLOOR_TR; }
    else          { tile_L = FLOOR_BL; tile_R = FLOOR_BR; }

    uint16_t off_L = tile_L * 16 + local_y * 2;
    uint16_t off_R = tile_R * 16 + local_y * 2;
    uint8_t lo_L = bg_tile_data[off_L];
    uint8_t hi_L = bg_tile_data[off_L + 1];
    uint8_t lo_R = bg_tile_data[off_R];
    uint8_t hi_R = bg_tile_data[off_R + 1];

    uint8_t raw_lo, raw_hi;
    if (texX == 0)       { raw_lo = lo_L; raw_hi = hi_L; }
    else if (texX == 8)  { raw_lo = lo_R; raw_hi = hi_R; }
    else if (texX < 8)   { raw_lo = (lo_L << texX) | (lo_R >> (8-texX));
                           raw_hi = (hi_L << texX) | (hi_R >> (8-texX)); }
    else                 { uint8_t s = texX-8;
                           raw_lo = (lo_R << s) | (lo_L >> (8-s));
                           raw_hi = (hi_R << s) | (hi_L >> (8-s)); }

    uint8_t b0 = (raw_lo >> 7) & 1;
    uint8_t b1 = (raw_lo >> 6) & 1;
    *lo = (b0 ? 0xF0 : 0x00) | (b1 ? 0x0F : 0x00);
    b0 = (raw_hi >> 7) & 1;
    b1 = (raw_hi >> 6) & 1;
    *hi = (b0 ? 0xF0 : 0x00) | (b1 ? 0x0F : 0x00);
}

// ================================================================
// RAYCASTER + RENDERER
// ================================================================
void raycast_and_render(void) {
    uint8_t col;

    for (col = 0; col < SCR_W; col++) {
        uint8_t ray_a = pa - HALF_FOV + (col << 1);
        int8_t rdx = cos_table[ray_a];
        int8_t rdy = sin_table[ray_a];
        uint8_t adx = (rdx < 0) ? (uint8_t)(-rdx) : (uint8_t)rdx;
        uint8_t ady = (rdy < 0) ? (uint8_t)(-rdy) : (uint8_t)rdy;
        if (adx == 0) adx = 1;
        if (ady == 0) ady = 1;

        uint8_t mapX = (uint8_t)(px >> 8);
        uint8_t mapY = (uint8_t)(py >> 8);
        int8_t stepX = (rdx >= 0) ? 1 : -1;
        int8_t stepY = (rdy >= 0) ? 1 : -1;

        uint8_t fracX = (uint8_t)((px >> 4) & 0x0F);
        uint8_t fracY = (uint8_t)((py >> 4) & 0x0F);
        uint16_t sX = (uint16_t)((stepX > 0) ? (DDA_CELL - fracX) : fracX) * ady;
        uint16_t sY = (uint16_t)((stepY > 0) ? (DDA_CELL - fracY) : fracY) * adx;
        uint16_t ddx = (uint16_t)DDA_CELL * ady;
        uint16_t ddy = (uint16_t)DDA_CELL * adx;

        uint8_t side = 0, hit = 0, d;
        for (d = 0; d < MAX_DDA; d++) {
            if (sX < sY) { sX += ddx; mapX += stepX; side = 0; }
            else          { sY += ddy; mapY += stepY; side = 1; }
            if (mapX >= MAP_W || mapY >= MAP_H) { hit = 1; break; }
            if (world_map[mapY][mapX])           { hit = 1; break; }
        }

        uint8_t drawStart = VIEW_H >> 1, drawEnd = VIEW_H >> 1;
        uint8_t texX = 0;
        uint8_t do_darken = 0;

        if (hit) {
            uint16_t wf, rawDist;
            uint8_t rc;
            if (side == 0) {
                wf = (stepX > 0) ? ((uint16_t)mapX << 8) : (((uint16_t)mapX+1) << 8);
                rawDist = (wf > (uint16_t)px) ? wf-(uint16_t)px : (uint16_t)px-wf;
                rc = adx;
            } else {
                wf = (stepY > 0) ? ((uint16_t)mapY << 8) : (((uint16_t)mapY+1) << 8);
                rawDist = (wf > (uint16_t)py) ? wf-(uint16_t)py : (uint16_t)py-wf;
                rc = ady;
            }

            uint8_t cf = (uint8_t)cos_table[(uint8_t)(ray_a - pa)];
            uint16_t pq = (uint16_t)(((uint32_t)rawDist * (uint32_t)cf) / ((uint16_t)rc << 6));
            if (pq < 1) pq = 1;
            if (pq > 80) pq = 80;

            uint8_t lh = wall_height_px[(uint8_t)pq];
            drawStart = (VIEW_H - lh) >> 1;
            drawEnd = drawStart + lh;
            do_darken = (side == 0);

            // texX: where on the wall surface this column hits (0-15)
            int16_t ho;
            if (side == 0)
                ho = py + (int16_t)((int16_t)pq * (int16_t)rdy);
            else
                ho = px + (int16_t)((int16_t)pq * (int16_t)rdx);
            texX = (uint8_t)((ho >> 4) & 0x0F);
        }

        // Stretched vertical: 16 texels mapped across full wall height
        uint8_t wh = drawEnd - drawStart;
        uint16_t texStep = wh ? (((uint16_t)16 << 8) / (uint16_t)wh) : 0;
        uint16_t texPos = 0;

        uint8_t ty;
        for (ty = 0; ty < VIEW_ROWS; ty++) {
            uint16_t toff = ((uint16_t)ty * SCR_W + col) << 4;
            uint8_t by = ty << 3;

            if (by + 8 <= drawStart) {
                memset(&fb[toff], 0xFF, 16);
                continue;
            }
            if (by >= drawEnd) {
                // Floor gradient: dark near horizon, light near bottom (3D depth)
                uint8_t *p = &fb[toff];
                uint8_t j;
                for (j = 0; j < 8; j++) {
                    uint8_t sy = by + j;
                    uint8_t dist = sy - (VIEW_H >> 1); // distance from horizon
                    // color 3 (darkest) near horizon, 2 mid, 1 near bottom
                    if (dist < 8)       { p[j*2]=0xFF; p[j*2+1]=0xFF; } // color 3
                    else if (dist < 20) { p[j*2]=0x00; p[j*2+1]=0xFF; } // color 2
                    else                { p[j*2]=0xFF; p[j*2+1]=0x00; } // color 1
                }
                continue;
            }

            uint8_t pr;
            for (pr = 0; pr < 8; pr++) {
                uint8_t sy = by + pr;
                uint8_t lo, hi;

                if (sy < drawStart) {
                    lo = 0xFF; hi = 0xFF;
                } else if (sy < drawEnd) {
                    uint8_t tY = (uint8_t)(texPos >> 8);
                    if (tY > 15) tY = 15;
                    read_wall_2x(tY, texX, &lo, &hi);
                    texPos += texStep;
                    if (do_darken) {
                        uint8_t nlo = hi & ~lo;
                        uint8_t nhi = hi & lo;
                        lo = nlo; hi = nhi;
                    }
                } else {
                    // Floor gradient in mixed tiles
                    uint8_t dist = sy - (VIEW_H >> 1);
                    if (dist < 8)       { lo=0xFF; hi=0xFF; }
                    else if (dist < 20) { lo=0x00; hi=0xFF; }
                    else                { lo=0xFF; hi=0x00; }
                }

                fb[toff + pr*2]   = lo;
                fb[toff + pr*2+1] = hi;
            }
        }
    }
}

void handle_input(void) {
    uint8_t keys = joypad();
    int16_t mdx, mdy, nx, ny;
    uint8_t cx, cy;
    if (keys & J_LEFT)  pa -= TURN_SPD;
    if (keys & J_RIGHT) pa += TURN_SPD;
    if (keys & (J_UP | J_DOWN)) {
        mdx = ((int16_t)cos_table[pa] * MOVE_SPD) >> 6;
        mdy = ((int16_t)sin_table[pa] * MOVE_SPD) >> 6;
        if (keys & J_DOWN) { mdx = -mdx; mdy = -mdy; }
        nx = px + mdx;
        cx = (uint8_t)((uint16_t)nx >> 8);
        cy = (uint8_t)((uint16_t)py >> 8);
        if (cx < MAP_W && cy < MAP_H && !world_map[cy][cx]) px = nx;
        ny = py + mdy;
        cx = (uint8_t)((uint16_t)px >> 8);
        cy = (uint8_t)((uint16_t)ny >> 8);
        if (cx < MAP_W && cy < MAP_H && !world_map[cy][cx]) py = ny;
    }
}

// ================================================================
// OBJECTS — world items scattered across the map
// ================================================================
#define MAX_OBJECTS 8

typedef struct {
    int16_t x, y;   // 8.8 world position
    uint8_t type;   // sprite type (0-3)
    uint8_t active;
} Object;

Object objects[MAX_OBJECTS] = {
    { (4 << 8)|0x80, (4 << 8)|0x80, 0, 1 },   // type 0 in corridor
    { (8 << 8)|0x80, (4 << 8)|0x80, 1, 1 },   // type 1
    { (12<< 8)|0x80, (4 << 8)|0x80, 2, 1 },   // type 2
    { (5 << 8)|0x80, (7 << 8)|0x80, 3, 1 },   // type 3 in south room
    { (10<< 8)|0x80, (7 << 8)|0x80, 0, 1 },
    { (3 << 8)|0x80, (11<< 8)|0x80, 1, 1 },
    { (8 << 8)|0x80, (11<< 8)|0x80, 2, 1 },
    { (12<< 8)|0x80, (13<< 8)|0x80, 3, 1 },
};

// Project and render objects as hardware sprites
void update_sprites(void) {
    uint8_t i;
    int8_t cos_pa = cos_table[pa];
    int8_t sin_pa = sin_table[pa];
    uint8_t oam_idx = 0;

    // Hide all sprites first
    for (i = 0; i < 40; i++) {
        move_sprite(i, 0, 0);
    }

    for (i = 0; i < MAX_OBJECTS && oam_idx < 36; i++) {
        if (!objects[i].active) continue;

        // Relative position to player
        int16_t dx = objects[i].x - px;
        int16_t dy = objects[i].y - py;

        // Transform to camera space (rotate by -player_angle)
        // camZ = forward depth, camX = sideways
        int16_t camZ = ((int32_t)dx * cos_pa + (int32_t)dy * sin_pa) >> 6;
        int16_t camX = ((int32_t)-dx * sin_pa + (int32_t)dy * cos_pa) >> 6;

        // Behind camera or too far? Skip
        if (camZ < 64 || camZ > 3072) continue;

        // Project to screen
        int16_t proj = (int16_t)(((int32_t)camX * 480) / camZ);
        int16_t scrX = 80 + proj;

        // Anchor sprite to floor: use wall height at this distance
        uint8_t dist_qt = (uint8_t)(camZ >> 6);
        if (dist_qt < 1) dist_qt = 1;
        if (dist_qt > 80) dist_qt = 80;
        uint8_t wh = wall_height_px[dist_qt];
        int16_t floorY = (VIEW_H >> 1) + (wh >> 1);
        int16_t scrY = floorY - 16;  // sprite bottom sits on floor

        // Clamp/skip if off screen
        if (scrX < -8 || scrX > 168 || scrY < -8 || scrY > VIEW_H) continue;

        // Scale: 16x16 at close range, shrink idea — but HW sprites are fixed size
        // Just place the 4 OAM entries (2x2 of 8x8 tiles)
        uint8_t base_tile = SPRITE_TILE_START + objects[i].type * 4;
        uint8_t sx = (uint8_t)(scrX + 8);   // OAM X is offset by 8
        uint8_t sy = (uint8_t)(scrY + 16);  // OAM Y is offset by 16

        move_sprite(oam_idx,     sx,     sy);
        set_sprite_tile(oam_idx, base_tile);
        oam_idx++;

        move_sprite(oam_idx,     sx + 8, sy);
        set_sprite_tile(oam_idx, base_tile + 1);
        oam_idx++;

        move_sprite(oam_idx,     sx,     sy + 8);
        set_sprite_tile(oam_idx, base_tile + 2);
        oam_idx++;

        move_sprite(oam_idx,     sx + 8, sy + 8);
        set_sprite_tile(oam_idx, base_tile + 3);
        oam_idx++;
    }
}

void init(void) {
    if (_cpu == CGB_TYPE) cpu_fast();
    set_bkg_palette(0, 1, bg_palettes);

    // Load sprite tiles into OBJ VRAM at tile 200+
    set_sprite_data(SPRITE_TILE_START, NUM_SPRITE_TYPES * 4, sprite_tile_data);
    // Set OBJ palette 0 (same colors as BG)
    set_sprite_palette(0, 1, sprite_palette);

    {
        uint8_t row[SCR_W]; uint8_t ty, tx;
        for (ty = 0; ty < VIEW_ROWS; ty++) {
            for (tx = 0; tx < SCR_W; tx++) row[tx] = ty * SCR_W + tx;
            set_bkg_tiles(0, ty, SCR_W, 1, row);
        }
        memset(row, 240, SCR_W);
        for (ty = VIEW_ROWS; ty < 18; ty++)
            set_bkg_tiles(0, ty, SCR_W, 1, row);
    }
    { uint8_t ft[16]; uint8_t i;
      for (i = 0; i < 8; i++) { ft[i*2]=0xFF; ft[i*2+1]=0x00; }
      set_bkg_data(240, 1, ft); }
    memset(fb, 0xFF, sizeof(fb));
    set_bkg_data(0, 100, fb);
    set_bkg_data(100, 100, fb + 1600u);

    px = (2 << 8) | 0x80;
    py = (4 << 8) | 0x80;
    pa = 0;
    move_bkg(0, 0);
    SPRITES_8x8;
    SHOW_SPRITES;
    LCDC_REG = LCDCF_ON | LCDCF_BGON | LCDCF_BG8000 | LCDCF_OBJON | LCDCF_OBJ8;
}

void main(void) {
    init();
    while (1) {
        handle_input();
        raycast_and_render();
        update_sprites();
        wait_vbl_done();
        set_bkg_data(0, 100, fb);
        set_bkg_data(100, 100, fb + 1600u);
    }
}
