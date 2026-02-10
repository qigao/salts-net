/*
 * TUI Buffer Extended Drawing Functions
 */

#include "tui.h"

static inline int tui_min(int a, int b) { return a < b ? a : b; }
static inline int tui_max(int a, int b) { return a > b ? a : b; }
static inline float tui_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ============================================================================
 * Gradients
 * ============================================================================ */

void tui_buffer_gradient_v(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t top, tui_color_t bottom) {
    for (int j = 0; j < h; j++) {
        float t = (h > 1) ? (float)j / (float)(h - 1) : 0.0f;
        tui_color_t c = {
            (uint8_t)(top.r + t * (bottom.r - top.r)),
            (uint8_t)(top.g + t * (bottom.g - top.g)),
            (uint8_t)(top.b + t * (bottom.b - top.b))
        };
        for (int i = 0; i < w; i++) {
            tui_buffer_set(buf, x + i, y + j, tui_cell(0x2588, c, c)); /* █ */
        }
    }
}

void tui_buffer_gradient_h(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t left, tui_color_t right) {
    for (int i = 0; i < w; i++) {
        float t = (w > 1) ? (float)i / (float)(w - 1) : 0.0f;
        tui_color_t c = {
            (uint8_t)(left.r + t * (right.r - left.r)),
            (uint8_t)(left.g + t * (right.g - left.g)),
            (uint8_t)(left.b + t * (right.b - left.b))
        };
        for (int j = 0; j < h; j++) {
            tui_buffer_set(buf, x + i, y + j, tui_cell(0x2588, c, c));
        }
    }
}

/* ============================================================================
 * Bars
 * ============================================================================ */

void tui_buffer_bar_h(tui_buffer_t* buf, int x, int y, int w, float progress, tui_color_t fg, tui_color_t bg) {
    /* Horizontal bar using eighths: ▏▎▍▌▋▊▉█ */
    static const uint32_t eighths[] = {' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589};
    
    progress = tui_clampf(progress, 0.0f, 1.0f);
    float filled = progress * (float)w;
    int full_blocks = (int)filled;
    float frac = filled - (float)full_blocks;
    
    /* Full blocks */
    for (int i = 0; i < full_blocks && i < w; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(0x2588, fg, bg)); /* █ */
    }
    
    /* Partial block */
    if (full_blocks < w && frac > 0.0f) {
        int idx = (int)(frac * 8.0f);
        tui_buffer_set(buf, x + full_blocks, y, tui_cell(eighths[idx], fg, bg));
        full_blocks++;
    }
    
    /* Empty space */
    for (int i = full_blocks; i < w; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(' ', fg, bg));
    }
}

void tui_buffer_bar_v(tui_buffer_t* buf, int x, int y, int h, float progress, tui_color_t fg, tui_color_t bg) {
    /* Vertical bar using eighths: ▁▂▃▄▅▆▇█ */
    static const uint32_t eighths[] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587};
    
    progress = tui_clampf(progress, 0.0f, 1.0f);
    float filled = progress * (float)h;
    int full_blocks = (int)filled;
    float frac = filled - (float)full_blocks;
    
    /* Empty space (top) */
    for (int i = 0; i < h - full_blocks - 1; i++) {
        tui_buffer_set(buf, x, y + i, tui_cell(' ', fg, bg));
    }
    
    /* Partial block */
    if (full_blocks < h && frac > 0.0f) {
        int idx = (int)(frac * 8.0f);
        tui_buffer_set(buf, x, y + h - full_blocks - 1, tui_cell(eighths[idx], fg, bg));
    }
    
    /* Full blocks (bottom) */
    for (int i = 0; i < full_blocks && i < h; i++) {
        tui_buffer_set(buf, x, y + h - 1 - i, tui_cell(0x2588, fg, bg));
    }
}

void tui_buffer_sparkline(tui_buffer_t* buf, int x, int y, const float* values, int count, tui_color_t fg, tui_color_t bg) {
    static const uint32_t bars[] = {0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
    
    if (count <= 0) return;
    
    float min_v = values[0], max_v = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < min_v) min_v = values[i];
        if (values[i] > max_v) max_v = values[i];
    }
    
    float range = max_v - min_v;
    if (range < 0.0001f) range = 1.0f;
    
    for (int i = 0; i < count; i++) {
        float norm = (values[i] - min_v) / range;
        int idx = (int)(norm * 7.99f);
        tui_buffer_set(buf, x + i, y, tui_cell(bars[idx], fg, bg));
    }
}

/* ============================================================================
 * Braille Drawing (2x4 dot matrix per cell = 2x resolution)
 * ============================================================================ */

/*
 * Braille pattern: dots numbered 1-8
 *   1 4
 *   2 5
 *   3 6
 *   7 8
 * Base: U+2800, each dot adds: 1=0x01, 2=0x02, 3=0x04, 4=0x08, 5=0x10, 6=0x20, 7=0x40, 8=0x80
 */

void tui_buffer_braille_set(tui_buffer_t* buf, int px, int py, tui_color_t fg, tui_color_t bg) {
    int cx = px / 2;
    int cy = py / 4;
    if (!tui_buffer_in_bounds(buf, cx, cy)) return;
    
    static const uint8_t dot_bits[4][2] = {
        {0x01, 0x08},  /* row 0: dots 1, 4 */
        {0x02, 0x10},  /* row 1: dots 2, 5 */
        {0x04, 0x20},  /* row 2: dots 3, 6 */
        {0x40, 0x80}   /* row 3: dots 7, 8 */
    };
    
    tui_cell_t* cell = tui_buffer_at(buf, cx, cy);
    uint32_t ch = cell->ch;
    if (ch < 0x2800 || ch > 0x28FF) ch = 0x2800;
    
    int dx = px % 2;
    int dy = py % 4;
    ch |= dot_bits[dy][dx];
    
    cell->ch = ch;
    cell->fg = fg;
    cell->bg = bg;
}

void tui_buffer_braille_clear(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t bg) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            tui_buffer_set(buf, x + i, y + j, tui_cell(0x2800, TUI_WHITE, bg));
        }
    }
}

void tui_buffer_braille_line(tui_buffer_t* buf, int x0, int y0, int x1, int y1, tui_color_t fg, tui_color_t bg) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        tui_buffer_braille_set(buf, x0, y0, fg, bg);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void tui_buffer_braille_plot(tui_buffer_t* buf, int x, int y, int w, int h, 
                             const float* values, int count, tui_color_t fg, tui_color_t bg) {
    if (count < 2) return;
    
    tui_buffer_braille_clear(buf, x, y, w, h, bg);
    
    int pw = w * 2;  /* pixel width */
    int ph = h * 4;  /* pixel height */
    
    float min_v = values[0], max_v = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < min_v) min_v = values[i];
        if (values[i] > max_v) max_v = values[i];
    }
    float range = max_v - min_v;
    if (range < 0.0001f) range = 1.0f;
    
    int prev_px = 0;
    int prev_py = ph - 1 - (int)(((values[0] - min_v) / range) * (ph - 1));
    
    for (int i = 1; i < count; i++) {
        int px = (i * (pw - 1)) / (count - 1);
        int py = ph - 1 - (int)(((values[i] - min_v) / range) * (ph - 1));
        tui_buffer_braille_line(buf, x * 2 + prev_px, y * 4 + prev_py, 
                                x * 2 + px, y * 4 + py, fg, bg);
        prev_px = px;
        prev_py = py;
    }
}

/* ============================================================================
 * Block Elements (shade/fill patterns)
 * ============================================================================ */

void tui_buffer_shade(tui_buffer_t* buf, int x, int y, int w, int h, int level, tui_color_t fg, tui_color_t bg) {
    /* level: 0=empty, 1=light, 2=medium, 3=dark, 4=full */
    static const uint32_t shades[] = {' ', 0x2591, 0x2592, 0x2593, 0x2588};
    uint32_t ch = shades[level > 4 ? 4 : level];
    
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            tui_buffer_set(buf, x + i, y + j, tui_cell(ch, fg, bg));
        }
    }
}

void tui_buffer_block(tui_buffer_t* buf, int x, int y, uint8_t pattern, tui_color_t fg, tui_color_t bg) {
    /*
     * 2x2 quadrant pattern (bits: TL=1, TR=2, BL=4, BR=8)
     * 0x0000=empty, 0x2588=full, quadrants: 0x2596-0x259F
     */
    static const uint32_t blocks[16] = {
        ' ',      0x2598, 0x259D, 0x2580,  /* 0000, 0001, 0010, 0011 */
        0x2596,   0x258C, 0x259E, 0x259B,  /* 0100, 0101, 0110, 0111 */
        0x2597,   0x259A, 0x2590, 0x259C,  /* 1000, 1001, 1010, 1011 */
        0x2584,   0x2599, 0x259F, 0x2588   /* 1100, 1101, 1110, 1111 */
    };
    tui_buffer_set(buf, x, y, tui_cell(blocks[pattern & 0x0F], fg, bg));
}

/* ============================================================================
 * Arrows and Symbols
 * ============================================================================ */

void tui_buffer_arrow(tui_buffer_t* buf, int x, int y, int dir, tui_color_t fg, tui_color_t bg) {
    /* dir: 0=up, 1=right, 2=down, 3=left */
    static const uint32_t arrows[] = {0x2191, 0x2192, 0x2193, 0x2190};  /* ↑→↓← */
    static const uint32_t arrows_double[] = {0x21D1, 0x21D2, 0x21D3, 0x21D0};  /* ⇑⇒⇓⇐ */
    static const uint32_t triangles[] = {0x25B2, 0x25B6, 0x25BC, 0x25C0};  /* ▲▶▼◀ */
    tui_buffer_set(buf, x, y, tui_cell(arrows[dir & 3], fg, bg));
}

void tui_buffer_symbol(tui_buffer_t* buf, int x, int y, int sym, tui_color_t fg, tui_color_t bg) {
    static const uint32_t symbols[] = {
        0x2713,  /* 0: ✓ check */
        0x2717,  /* 1: ✗ cross */
        0x2022,  /* 2: • bullet */
        0x25CF,  /* 3: ● circle filled */
        0x25CB,  /* 4: ○ circle empty */
        0x25A0,  /* 5: ■ square filled */
        0x25A1,  /* 6: □ square empty */
        0x2605,  /* 7: ★ star filled */
        0x2606,  /* 8: ☆ star empty */
        0x2665,  /* 9: ♥ heart */
        0x266A,  /* 10: ♪ note */
        0x26A1,  /* 11: ⚡ lightning */
        0x2302,  /* 12: ⌂ house */
        0x231B,  /* 13: ⌛ hourglass */
        0x2328,  /* 14: ⌨ keyboard */
        0x2699,  /* 15: ⚙ gear */
    };
    if (sym >= 0 && sym < 16) {
        tui_buffer_set(buf, x, y, tui_cell(symbols[sym], fg, bg));
    }
}

/* ============================================================================
 * Borders (extended styles)
 * ============================================================================ */

void tui_buffer_box_ascii(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    tui_buffer_set(buf, x, y, tui_cell('+', fg, bg));
    tui_buffer_set(buf, x + w - 1, y, tui_cell('+', fg, bg));
    tui_buffer_set(buf, x, y + h - 1, tui_cell('+', fg, bg));
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell('+', fg, bg));
    
    for (int i = 1; i < w - 1; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell('-', fg, bg));
        tui_buffer_set(buf, x + i, y + h - 1, tui_cell('-', fg, bg));
    }
    for (int j = 1; j < h - 1; j++) {
        tui_buffer_set(buf, x, y + j, tui_cell('|', fg, bg));
        tui_buffer_set(buf, x + w - 1, y + j, tui_cell('|', fg, bg));
    }
}
