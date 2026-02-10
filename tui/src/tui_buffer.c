/*
 * TUI Buffer Implementation
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Buffer Lifecycle
 * ============================================================================ */

tui_buffer_t* tui_buffer_create(int width, int height) {
    tui_buffer_t* buf = (tui_buffer_t*)calloc(1, sizeof(tui_buffer_t));
    if (!buf) return NULL;
    
    size_t count = (size_t)width * (size_t)height;
    buf->cells = (tui_cell_t*)malloc(count * sizeof(tui_cell_t));
    if (!buf->cells) {
        free(buf);
        return NULL;
    }
    
    buf->width = width;
    buf->height = height;
    buf->capacity = count;
    
    tui_buffer_clear(buf);
    tui_buffer_reset_dirty(buf);
    
    return buf;
}

void tui_buffer_destroy(tui_buffer_t* buf) {
    if (!buf) return;
    free(buf->cells);
    free(buf);
}

void tui_buffer_resize(tui_buffer_t* buf, int width, int height) {
    if (!buf) return;
    
    size_t count = (size_t)width * (size_t)height;
    if (count > buf->capacity) {
        tui_cell_t* new_cells = (tui_cell_t*)realloc(buf->cells, count * sizeof(tui_cell_t));
        if (!new_cells) return;
        buf->cells = new_cells;
        buf->capacity = count;
    }
    
    buf->width = width;
    buf->height = height;
    tui_buffer_clear(buf);
    tui_buffer_reset_dirty(buf);
}

/* ============================================================================
 * Cell Access
 * ============================================================================ */

bool tui_buffer_in_bounds(const tui_buffer_t* buf, int x, int y) {
    return x >= 0 && x < buf->width && y >= 0 && y < buf->height;
}

tui_cell_t* tui_buffer_at(tui_buffer_t* buf, int x, int y) {
    return &buf->cells[y * buf->width + x];
}

const tui_cell_t* tui_buffer_at_const(const tui_buffer_t* buf, int x, int y) {
    return &buf->cells[y * buf->width + x];
}

/* ============================================================================
 * Dirty Tracking
 * ============================================================================ */

void tui_buffer_reset_dirty(tui_buffer_t* buf) {
    buf->dirty_min_x = buf->width;
    buf->dirty_min_y = buf->height;
    buf->dirty_max_x = -1;
    buf->dirty_max_y = -1;
}

void tui_buffer_mark_dirty(tui_buffer_t* buf, int x, int y) {
    if (!tui_buffer_in_bounds(buf, x, y)) return;
    if (x < buf->dirty_min_x) buf->dirty_min_x = x;
    if (y < buf->dirty_min_y) buf->dirty_min_y = y;
    if (x > buf->dirty_max_x) buf->dirty_max_x = x;
    if (y > buf->dirty_max_y) buf->dirty_max_y = y;
}

void tui_buffer_mark_all_dirty(tui_buffer_t* buf) {
    if (buf->width > 0 && buf->height > 0) {
        buf->dirty_min_x = 0;
        buf->dirty_min_y = 0;
        buf->dirty_max_x = buf->width - 1;
        buf->dirty_max_y = buf->height - 1;
    }
}

bool tui_buffer_is_dirty(const tui_buffer_t* buf) {
    return buf->dirty_max_x >= buf->dirty_min_x && buf->dirty_max_y >= buf->dirty_min_y;
}

/* ============================================================================
 * Drawing Primitives
 * ============================================================================ */

void tui_buffer_clear(tui_buffer_t* buf) {
    tui_cell_t def = TUI_CELL_DEFAULT;
    size_t count = (size_t)buf->width * (size_t)buf->height;
    for (size_t i = 0; i < count; i++) {
        buf->cells[i] = def;
    }
    tui_buffer_mark_all_dirty(buf);
}

void tui_buffer_clear_color(tui_buffer_t* buf, tui_color_t bg) {
    tui_cell_t cell = {' ', TUI_WHITE, bg, TUI_STYLE_NONE};
    size_t count = (size_t)buf->width * (size_t)buf->height;
    for (size_t i = 0; i < count; i++) {
        buf->cells[i] = cell;
    }
    tui_buffer_mark_all_dirty(buf);
}

void tui_buffer_set(tui_buffer_t* buf, int x, int y, tui_cell_t cell) {
    if (tui_buffer_in_bounds(buf, x, y)) {
        *tui_buffer_at(buf, x, y) = cell;
        tui_buffer_mark_dirty(buf, x, y);
    }
}

void tui_buffer_set_char(tui_buffer_t* buf, int x, int y, uint32_t ch) {
    if (tui_buffer_in_bounds(buf, x, y)) {
        tui_buffer_at(buf, x, y)->ch = ch;
        tui_buffer_mark_dirty(buf, x, y);
    }
}

void tui_buffer_text(tui_buffer_t* buf, int x, int y, const char* str, tui_color_t fg, tui_color_t bg) {
    int start_x = x;
    while (*str && x < buf->width) {
        if (tui_buffer_in_bounds(buf, x, y)) {
            tui_cell_t* cell = tui_buffer_at(buf, x, y);
            cell->ch = (uint32_t)(unsigned char)*str;
            cell->fg = fg;
            cell->bg = bg;
            cell->style = TUI_STYLE_NONE;
        }
        str++;
        x++;
    }
    if (x > start_x) {
        tui_buffer_mark_dirty(buf, start_x, y);
        tui_buffer_mark_dirty(buf, x - 1, y);
    }
}

void tui_buffer_fill(tui_buffer_t* buf, int x, int y, int w, int h, tui_cell_t cell) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (tui_buffer_in_bounds(buf, x + i, y + j)) {
                *tui_buffer_at(buf, x + i, y + j) = cell;
            }
        }
    }
    tui_buffer_mark_dirty(buf, x, y);
    tui_buffer_mark_dirty(buf, x + w - 1, y + h - 1);
}

void tui_buffer_fill_color(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t bg) {
    tui_cell_t cell = {' ', TUI_WHITE, bg, TUI_STYLE_NONE};
    tui_buffer_fill(buf, x, y, w, h, cell);
}

/* ============================================================================
 * Lines
 * ============================================================================ */

void tui_buffer_hline(tui_buffer_t* buf, int x, int y, int len, uint32_t ch, tui_color_t fg, tui_color_t bg) {
    tui_cell_t cell = {ch, fg, bg, TUI_STYLE_NONE};
    for (int i = 0; i < len && x + i < buf->width; i++) {
        tui_buffer_set(buf, x + i, y, cell);
    }
}

void tui_buffer_vline(tui_buffer_t* buf, int x, int y, int len, uint32_t ch, tui_color_t fg, tui_color_t bg) {
    tui_cell_t cell = {ch, fg, bg, TUI_STYLE_NONE};
    for (int i = 0; i < len && y + i < buf->height; i++) {
        tui_buffer_set(buf, x, y + i, cell);
    }
}

/* ============================================================================
 * Boxes
 * ============================================================================ */

void tui_buffer_box(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    /* Corners */
    tui_buffer_set(buf, x, y, tui_cell(0x250C, fg, bg));             /* ┌ */
    tui_buffer_set(buf, x + w - 1, y, tui_cell(0x2510, fg, bg));     /* ┐ */
    tui_buffer_set(buf, x, y + h - 1, tui_cell(0x2514, fg, bg));     /* └ */
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell(0x2518, fg, bg)); /* ┘ */
    
    /* Edges */
    tui_buffer_hline(buf, x + 1, y, w - 2, 0x2500, fg, bg);          /* ─ */
    tui_buffer_hline(buf, x + 1, y + h - 1, w - 2, 0x2500, fg, bg);
    tui_buffer_vline(buf, x, y + 1, h - 2, 0x2502, fg, bg);          /* │ */
    tui_buffer_vline(buf, x + w - 1, y + 1, h - 2, 0x2502, fg, bg);
}

void tui_buffer_box_double(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    tui_buffer_set(buf, x, y, tui_cell(0x2554, fg, bg));             /* ╔ */
    tui_buffer_set(buf, x + w - 1, y, tui_cell(0x2557, fg, bg));     /* ╗ */
    tui_buffer_set(buf, x, y + h - 1, tui_cell(0x255A, fg, bg));     /* ╚ */
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell(0x255D, fg, bg)); /* ╝ */
    
    tui_buffer_hline(buf, x + 1, y, w - 2, 0x2550, fg, bg);          /* ═ */
    tui_buffer_hline(buf, x + 1, y + h - 1, w - 2, 0x2550, fg, bg);
    tui_buffer_vline(buf, x, y + 1, h - 2, 0x2551, fg, bg);          /* ║ */
    tui_buffer_vline(buf, x + w - 1, y + 1, h - 2, 0x2551, fg, bg);
}

void tui_buffer_box_round(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    tui_buffer_set(buf, x, y, tui_cell(0x256D, fg, bg));             /* ╭ */
    tui_buffer_set(buf, x + w - 1, y, tui_cell(0x256E, fg, bg));     /* ╮ */
    tui_buffer_set(buf, x, y + h - 1, tui_cell(0x2570, fg, bg));     /* ╰ */
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell(0x256F, fg, bg)); /* ╯ */
    
    tui_buffer_hline(buf, x + 1, y, w - 2, 0x2500, fg, bg);
    tui_buffer_hline(buf, x + 1, y + h - 1, w - 2, 0x2500, fg, bg);
    tui_buffer_vline(buf, x, y + 1, h - 2, 0x2502, fg, bg);
    tui_buffer_vline(buf, x + w - 1, y + 1, h - 2, 0x2502, fg, bg);
}

void tui_buffer_box_heavy(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    tui_buffer_set(buf, x, y, tui_cell(0x250F, fg, bg));             /* ┏ */
    tui_buffer_set(buf, x + w - 1, y, tui_cell(0x2513, fg, bg));     /* ┓ */
    tui_buffer_set(buf, x, y + h - 1, tui_cell(0x2517, fg, bg));     /* ┗ */
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell(0x251B, fg, bg)); /* ┛ */
    
    tui_buffer_hline(buf, x + 1, y, w - 2, 0x2501, fg, bg);          /* ━ */
    tui_buffer_hline(buf, x + 1, y + h - 1, w - 2, 0x2501, fg, bg);
    tui_buffer_vline(buf, x, y + 1, h - 2, 0x2503, fg, bg);          /* ┃ */
    tui_buffer_vline(buf, x + w - 1, y + 1, h - 2, 0x2503, fg, bg);
}
