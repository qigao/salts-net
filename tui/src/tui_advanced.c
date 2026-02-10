/*
 * TUI Advanced Components
 * Text utilities, viewport, table, tabs, tree, dialog, charts, animation, clipboard
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }
static inline float clamp_f(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ============================================================================
 * Text Utilities
 * ============================================================================ */

static int utf8_strlen(const char* s) {
    int len = 0;
    while (*s) {
        if ((*s & 0xC0) != 0x80) len++;
        s++;
    }
    return len;
}

void tui_text_aligned(tui_buffer_t* buf, int x, int y, int width, const char* text, tui_align_t align, tui_color_t fg, tui_color_t bg) {
    int len = utf8_strlen(text);
    int pad = 0;
    
    if (len >= width) {
        tui_text_ellipsis(buf, x, y, width, text, fg, bg);
        return;
    }
    
    switch (align) {
        case TUI_ALIGN_CENTER: pad = (width - len) / 2; break;
        case TUI_ALIGN_RIGHT:  pad = width - len; break;
        default: break;
    }
    
    for (int i = 0; i < pad; i++)
        tui_buffer_set(buf, x + i, y, tui_cell(' ', fg, bg));
    tui_buffer_text(buf, x + pad, y, text, fg, bg);
    for (int i = pad + len; i < width; i++)
        tui_buffer_set(buf, x + i, y, tui_cell(' ', fg, bg));
}

void tui_text_ellipsis(tui_buffer_t* buf, int x, int y, int width, const char* text, tui_color_t fg, tui_color_t bg) {
    if (width <= 0) return;
    
    int len = utf8_strlen(text);
    if (len <= width) {
        tui_buffer_text(buf, x, y, text, fg, bg);
        return;
    }
    
    const char* p = text;
    int col = 0;
    int max_col = width - 1;
    
    while (*p && col < max_col) {
        uint32_t ch = (uint8_t)*p;
        if ((ch & 0x80) == 0) {
            tui_buffer_set(buf, x + col, y, tui_cell(ch, fg, bg));
            p++;
        } else if ((ch & 0xE0) == 0xC0) {
            ch = ((ch & 0x1F) << 6) | (p[1] & 0x3F);
            tui_buffer_set(buf, x + col, y, tui_cell(ch, fg, bg));
            p += 2;
        } else if ((ch & 0xF0) == 0xE0) {
            ch = ((ch & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            tui_buffer_set(buf, x + col, y, tui_cell(ch, fg, bg));
            p += 3;
        } else {
            ch = ((ch & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            tui_buffer_set(buf, x + col, y, tui_cell(ch, fg, bg));
            p += 4;
        }
        col++;
    }
    tui_buffer_set(buf, x + width - 1, y, tui_cell(0x2026, fg, bg)); /* … */
}

void tui_text_wrap(tui_buffer_t* buf, int x, int y, int width, int height, const char* text, tui_color_t fg, tui_color_t bg) {
    int row = 0, col = 0;
    const char* p = text;
    
    while (*p && row < height) {
        if (*p == '\n') {
            row++;
            col = 0;
            p++;
            continue;
        }
        if (col >= width) {
            row++;
            col = 0;
        }
        if (row >= height) break;
        
        tui_buffer_set(buf, x + col, y + row, tui_cell((uint8_t)*p, fg, bg));
        col++;
        p++;
    }
}

int tui_text_wrap_height(const char* text, int width) {
    int rows = 1, col = 0;
    const char* p = text;
    
    while (*p) {
        if (*p == '\n') { rows++; col = 0; }
        else if (++col > width) { rows++; col = 1; }
        p++;
    }
    return rows;
}

/* ============================================================================
 * Viewport
 * ============================================================================ */

void tui_viewport_init(tui_viewport_t* vp, int content_w, int content_h) {
    vp->scroll_x = 0;
    vp->scroll_y = 0;
    vp->content_w = content_w;
    vp->content_h = content_h;
    vp->view_w = content_w;
    vp->view_h = content_h;
}

void tui_viewport_set_size(tui_viewport_t* vp, int view_w, int view_h) {
    vp->view_w = view_w;
    vp->view_h = view_h;
    vp->scroll_x = min_i(vp->scroll_x, max_i(0, vp->content_w - view_w));
    vp->scroll_y = min_i(vp->scroll_y, max_i(0, vp->content_h - view_h));
}

void tui_viewport_scroll_to(tui_viewport_t* vp, int x, int y) {
    vp->scroll_x = max_i(0, min_i(x, vp->content_w - vp->view_w));
    vp->scroll_y = max_i(0, min_i(y, vp->content_h - vp->view_h));
}

void tui_viewport_scroll_by(tui_viewport_t* vp, int dx, int dy) {
    tui_viewport_scroll_to(vp, vp->scroll_x + dx, vp->scroll_y + dy);
}

void tui_viewport_ensure_visible(tui_viewport_t* vp, int x, int y, int w, int h) {
    if (x < vp->scroll_x) vp->scroll_x = x;
    if (y < vp->scroll_y) vp->scroll_y = y;
    if (x + w > vp->scroll_x + vp->view_w) vp->scroll_x = x + w - vp->view_w;
    if (y + h > vp->scroll_y + vp->view_h) vp->scroll_y = y + h - vp->view_h;
}

bool tui_viewport_handle(tui_viewport_t* vp, const tui_event_t* event, int rx, int ry) {
    if (event->type == TUI_EVENT_KEY) {
        switch (event->key) {
            case TUI_KEY_UP:    tui_viewport_scroll_by(vp, 0, -1); return true;
            case TUI_KEY_DOWN:  tui_viewport_scroll_by(vp, 0, 1); return true;
            case TUI_KEY_LEFT:  tui_viewport_scroll_by(vp, -1, 0); return true;
            case TUI_KEY_RIGHT: tui_viewport_scroll_by(vp, 1, 0); return true;
            case TUI_KEY_PAGE_UP:   tui_viewport_scroll_by(vp, 0, -vp->view_h); return true;
            case TUI_KEY_PAGE_DOWN: tui_viewport_scroll_by(vp, 0, vp->view_h); return true;
            default: break;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS) {
        if (tui_rect_contains(rx, ry, vp->view_w, vp->view_h, event->x, event->y)) {
            if (event->key == TUI_KEY_MOUSE_WHEEL_UP) { tui_viewport_scroll_by(vp, 0, -3); return true; }
            if (event->key == TUI_KEY_MOUSE_WHEEL_DOWN) { tui_viewport_scroll_by(vp, 0, 3); return true; }
        }
    }
    return false;
}

void tui_viewport_draw_scrollbars(tui_buffer_t* buf, int x, int y, const tui_viewport_t* vp, const tui_theme_t* theme) {
    if (vp->content_h > vp->view_h) {
        float pos = (float)vp->scroll_y / (float)(vp->content_h - vp->view_h);
        float ratio = (float)vp->view_h / (float)vp->content_h;
        tui_widget_scrollbar_v(buf, x + vp->view_w, y, vp->view_h, pos, ratio, theme);
    }
}

/* ============================================================================
 * Table
 * ============================================================================ */

void tui_table_init(tui_table_t* table, tui_table_col_t* cols, int col_count) {
    table->cols = cols;
    table->col_count = col_count;
    table->row_count = 0;
    table->selected_row = 0;
    table->scroll = 0;
    table->visible_rows = 10;
    table->show_header = true;
    table->show_border = true;
}

void tui_table_set_data(tui_table_t* table, int row_count, int visible_rows) {
    table->row_count = row_count;
    table->visible_rows = visible_rows;
}

bool tui_table_handle(tui_table_t* table, const tui_event_t* event, int x, int y, int width, int height) {
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_UP && table->selected_row > 0) {
            table->selected_row--;
            if (table->selected_row < table->scroll) table->scroll = table->selected_row;
            return true;
        }
        if (event->key == TUI_KEY_DOWN && table->selected_row < table->row_count - 1) {
            table->selected_row++;
            if (table->selected_row >= table->scroll + table->visible_rows)
                table->scroll = table->selected_row - table->visible_rows + 1;
            return true;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        int header_offset = table->show_header ? 2 : 0;
        if (tui_rect_contains(x, y + header_offset, width, table->visible_rows, event->x, event->y)) {
            int row = table->scroll + (event->y - y - header_offset);
            if (row >= 0 && row < table->row_count) {
                table->selected_row = row;
                return true;
            }
        }
    }
    return false;
}

void tui_table_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                    const tui_table_t* table, const char* const* headers,
                    const char* const* const* data, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    int row_y = y;
    
    /* Header */
    if (table->show_header && headers) {
        int col_x = x;
        for (int c = 0; c < table->col_count; c++) {
            int cw = table->cols[c].width;
            tui_text_aligned(buf, col_x, row_y, cw, headers[c], table->cols[c].align, t.fg, t.highlight);
            col_x += cw + 1;
        }
        row_y++;
        tui_widget_divider(buf, x, row_y, width, &t);
        row_y++;
    }
    
    /* Rows */
    for (int r = 0; r < table->visible_rows && r + table->scroll < table->row_count; r++) {
        int data_row = r + table->scroll;
        bool selected = (data_row == table->selected_row);
        tui_color_t bg = selected ? t.highlight : t.bg;
        
        tui_buffer_fill_color(buf, x, row_y, width, 1, bg);
        
        int col_x = x;
        for (int c = 0; c < table->col_count; c++) {
            int cw = table->cols[c].width;
            if (data && data[data_row] && data[data_row][c]) {
                tui_text_aligned(buf, col_x, row_y, cw, data[data_row][c], table->cols[c].align, t.fg, bg);
            }
            col_x += cw + 1;
        }
        row_y++;
    }
}


/* ============================================================================
 * Tabs
 * ============================================================================ */

void tui_tabs_init(tui_tabs_t* tabs, int count) {
    tabs->selected = 0;
    tabs->count = count;
    tabs->scroll = 0;
}

bool tui_tabs_handle(tui_tabs_t* tabs, const tui_event_t* event, int x, int y, int width) {
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_LEFT && tabs->selected > 0) {
            tabs->selected--;
            return true;
        }
        if (event->key == TUI_KEY_RIGHT && tabs->selected < tabs->count - 1) {
            tabs->selected++;
            return true;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        if (event->y == y && event->x >= x && event->x < x + width) {
            /* Simple: divide width equally */
            int tab_w = width / tabs->count;
            int clicked = (event->x - x) / tab_w;
            if (clicked >= 0 && clicked < tabs->count) {
                tabs->selected = clicked;
                return true;
            }
        }
    }
    return false;
}

void tui_tabs_draw(tui_buffer_t* buf, int x, int y, int width, const tui_tabs_t* tabs,
                   const char* const* labels, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    int tab_w = width / tabs->count;
    
    for (int i = 0; i < tabs->count; i++) {
        int tx = x + i * tab_w;
        bool sel = (i == tabs->selected);
        tui_color_t bg = sel ? t.primary : t.secondary;
        
        tui_buffer_fill_color(buf, tx, y, tab_w, 1, bg);
        if (labels && labels[i]) {
            tui_text_aligned(buf, tx, y, tab_w, labels[i], TUI_ALIGN_CENTER, t.fg, bg);
        }
    }
}

/* ============================================================================
 * Tree
 * ============================================================================ */

static int tree_count_visible(tui_tree_node_t* node, int depth) {
    if (!node) return 0;
    int count = 1;
    if (node->expanded && node->children) {
        for (int i = 0; i < node->child_count; i++) {
            count += tree_count_visible(&node->children[i], depth + 1);
        }
    }
    return count;
}

static tui_tree_node_t* tree_get_at_index(tui_tree_node_t* node, int* index, int target) {
    if (!node) return NULL;
    if (*index == target) return node;
    (*index)++;
    
    if (node->expanded && node->children) {
        for (int i = 0; i < node->child_count; i++) {
            tui_tree_node_t* found = tree_get_at_index(&node->children[i], index, target);
            if (found) return found;
        }
    }
    return NULL;
}

static void tree_draw_node(tui_buffer_t* buf, int x, int y, int width, int* row,
                           tui_tree_node_t* node, int depth, int selected, int scroll,
                           int visible, const tui_theme_t* t) {
    if (!node || *row >= scroll + visible) return;
    
    if (*row >= scroll) {
        int draw_y = y + (*row - scroll);
        bool sel = (*row == selected);
        tui_color_t bg = sel ? t->highlight : t->bg;
        
        tui_buffer_fill_color(buf, x, draw_y, width, 1, bg);
        
        int indent = depth * 2;
        if (!node->is_leaf) {
            uint32_t arrow = node->expanded ? 0x25BC : 0x25B6; /* ▼ or ▶ */
            tui_buffer_set(buf, x + indent, draw_y, tui_cell(arrow, t->secondary, bg));
        }
        tui_buffer_text(buf, x + indent + 2, draw_y, node->label, t->fg, bg);
    }
    (*row)++;
    
    if (node->expanded && node->children) {
        for (int i = 0; i < node->child_count; i++) {
            tree_draw_node(buf, x, y, width, row, &node->children[i], depth + 1, selected, scroll, visible, t);
        }
    }
}

void tui_tree_init(tui_tree_t* tree, tui_tree_node_t* root) {
    tree->root = root;
    tree->selected = 0;
    tree->scroll = 0;
    tree->visible = 10;
    tree->total_visible = tree_count_visible(root, 0);
}

bool tui_tree_handle(tui_tree_t* tree, const tui_event_t* event, int x, int y, int width, int height) {
    tree->visible = height;
    tree->total_visible = tree_count_visible(tree->root, 0);
    
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_UP && tree->selected > 0) {
            tree->selected--;
            if (tree->selected < tree->scroll) tree->scroll = tree->selected;
            return true;
        }
        if (event->key == TUI_KEY_DOWN && tree->selected < tree->total_visible - 1) {
            tree->selected++;
            if (tree->selected >= tree->scroll + tree->visible)
                tree->scroll = tree->selected - tree->visible + 1;
            return true;
        }
        if (event->key == TUI_KEY_ENTER || event->key == TUI_KEY_RIGHT) {
            tui_tree_node_t* node = tui_tree_get_selected(tree);
            if (node && !node->is_leaf) {
                node->expanded = !node->expanded;
                tree->total_visible = tree_count_visible(tree->root, 0);
                return true;
            }
        }
        if (event->key == TUI_KEY_LEFT) {
            tui_tree_node_t* node = tui_tree_get_selected(tree);
            if (node && node->expanded) {
                node->expanded = false;
                tree->total_visible = tree_count_visible(tree->root, 0);
                return true;
            }
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        if (tui_rect_contains(x, y, width, height, event->x, event->y)) {
            int clicked = tree->scroll + (event->y - y);
            if (clicked >= 0 && clicked < tree->total_visible) {
                if (clicked == tree->selected) {
                    tui_tree_node_t* node = tui_tree_get_selected(tree);
                    if (node && !node->is_leaf) node->expanded = !node->expanded;
                    tree->total_visible = tree_count_visible(tree->root, 0);
                }
                tree->selected = clicked;
                return true;
            }
        }
    }
    return false;
}

void tui_tree_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                   const tui_tree_t* tree, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    int row = 0;
    tree_draw_node(buf, x, y, width, &row, tree->root, 0, tree->selected, tree->scroll, height, &t);
}

tui_tree_node_t* tui_tree_get_selected(tui_tree_t* tree) {
    int index = 0;
    return tree_get_at_index(tree->root, &index, tree->selected);
}

/* ============================================================================
 * Dialog
 * ============================================================================ */

void tui_dialog_show(tui_dialog_t* dlg, const char* title, const char* message, tui_dialog_type_t type) {
    dlg->title = title;
    dlg->message = message;
    dlg->type = type;
    dlg->selected_button = 0;
    dlg->visible = true;
}

void tui_dialog_hide(tui_dialog_t* dlg) {
    dlg->visible = false;
}

tui_dialog_result_t tui_dialog_handle(tui_dialog_t* dlg, const tui_event_t* event) {
    if (!dlg->visible) return TUI_DIALOG_RESULT_NONE;
    
    int btn_count = (dlg->type == TUI_DIALOG_OK) ? 1 : 
                    (dlg->type == TUI_DIALOG_YES_NO_CANCEL) ? 3 : 2;
    
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_LEFT && dlg->selected_button > 0) {
            dlg->selected_button--;
            return TUI_DIALOG_RESULT_NONE;
        }
        if (event->key == TUI_KEY_RIGHT && dlg->selected_button < btn_count - 1) {
            dlg->selected_button++;
            return TUI_DIALOG_RESULT_NONE;
        }
        if (event->key == TUI_KEY_ENTER) {
            dlg->visible = false;
            switch (dlg->type) {
                case TUI_DIALOG_OK: return TUI_DIALOG_RESULT_OK;
                case TUI_DIALOG_OK_CANCEL:
                    return dlg->selected_button == 0 ? TUI_DIALOG_RESULT_OK : TUI_DIALOG_RESULT_CANCEL;
                case TUI_DIALOG_YES_NO:
                    return dlg->selected_button == 0 ? TUI_DIALOG_RESULT_YES : TUI_DIALOG_RESULT_NO;
                case TUI_DIALOG_YES_NO_CANCEL:
                    if (dlg->selected_button == 0) return TUI_DIALOG_RESULT_YES;
                    if (dlg->selected_button == 1) return TUI_DIALOG_RESULT_NO;
                    return TUI_DIALOG_RESULT_CANCEL;
            }
        }
        if (event->key == TUI_KEY_ESCAPE) {
            dlg->visible = false;
            return TUI_DIALOG_RESULT_CANCEL;
        }
    }
    return TUI_DIALOG_RESULT_NONE;
}

void tui_dialog_draw(tui_buffer_t* buf, int screen_w, int screen_h, const tui_dialog_t* dlg, const tui_theme_t* theme) {
    if (!dlg->visible) return;
    
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    int dlg_w = 40, dlg_h = 9;
    int dlg_x = (screen_w - dlg_w) / 2;
    int dlg_y = (screen_h - dlg_h) / 2;
    
    /* Shadow */
    tui_buffer_fill_color(buf, dlg_x + 1, dlg_y + 1, dlg_w, dlg_h, TUI_COLOR(20, 20, 20));
    
    /* Panel */
    tui_widget_panel(buf, dlg_x, dlg_y, dlg_w, dlg_h, dlg->title, &t);
    
    /* Message */
    tui_text_wrap(buf, dlg_x + 2, dlg_y + 2, dlg_w - 4, 3, dlg->message, t.fg, t.bg);
    
    /* Buttons */
    const char* buttons[3];
    int btn_count;
    switch (dlg->type) {
        case TUI_DIALOG_OK:
            buttons[0] = "  OK  "; btn_count = 1; break;
        case TUI_DIALOG_OK_CANCEL:
            buttons[0] = "  OK  "; buttons[1] = "Cancel"; btn_count = 2; break;
        case TUI_DIALOG_YES_NO:
            buttons[0] = " Yes  "; buttons[1] = "  No  "; btn_count = 2; break;
        case TUI_DIALOG_YES_NO_CANCEL:
            buttons[0] = " Yes  "; buttons[1] = "  No  "; buttons[2] = "Cancel"; btn_count = 3; break;
        default: btn_count = 0; break;
    }
    
    int btn_total_w = btn_count * 8;
    int btn_x = dlg_x + (dlg_w - btn_total_w) / 2;
    int btn_y = dlg_y + dlg_h - 2;
    
    for (int i = 0; i < btn_count; i++) {
        bool focused = (i == dlg->selected_button);
        tui_widget_button(buf, btn_x + i * 8, btn_y, 7, buttons[i], focused, false, &t);
    }
}


/* ============================================================================
 * Charts
 * ============================================================================ */

void tui_chart_bar_h(tui_buffer_t* buf, int x, int y, int width, int height,
                     const float* values, int count, const char* const* labels,
                     tui_color_t bar_color, const tui_theme_t* theme) {
    if (count <= 0 || height <= 0) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    /* Find max value */
    float max_val = 0.001f;
    for (int i = 0; i < count; i++) {
        if (values[i] > max_val) max_val = values[i];
    }
    
    int label_w = 0;
    if (labels) {
        for (int i = 0; i < count; i++) {
            int len = labels[i] ? utf8_strlen(labels[i]) : 0;
            if (len > label_w) label_w = len;
        }
        label_w = min_i(label_w + 1, width / 3);
    }
    
    int bar_area = width - label_w;
    int row_h = height / count;
    if (row_h < 1) row_h = 1;
    
    for (int i = 0; i < count && i * row_h < height; i++) {
        int row_y = y + i * row_h;
        
        /* Label */
        if (labels && labels[i]) {
            tui_text_ellipsis(buf, x, row_y, label_w - 1, labels[i], t.fg, t.bg);
        }
        
        /* Bar */
        float ratio = values[i] / max_val;
        int bar_len = (int)(ratio * bar_area);
        if (bar_len < 1 && values[i] > 0) bar_len = 1;
        
        for (int bx = 0; bx < bar_len; bx++) {
            tui_buffer_set(buf, x + label_w + bx, row_y, tui_cell(0x2588, bar_color, t.bg));
        }
    }
}

void tui_chart_bar_v(tui_buffer_t* buf, int x, int y, int width, int height,
                     const float* values, int count, const char* const* labels,
                     tui_color_t bar_color, const tui_theme_t* theme) {
    if (count <= 0 || width <= 0) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    /* Find max value */
    float max_val = 0.001f;
    for (int i = 0; i < count; i++) {
        if (values[i] > max_val) max_val = values[i];
    }
    
    int label_h = labels ? 1 : 0;
    int bar_area = height - label_h;
    int col_w = width / count;
    if (col_w < 1) col_w = 1;
    
    for (int i = 0; i < count && i * col_w < width; i++) {
        int col_x = x + i * col_w;
        
        /* Bar (bottom-up) */
        float ratio = values[i] / max_val;
        int bar_h = (int)(ratio * bar_area);
        if (bar_h < 1 && values[i] > 0) bar_h = 1;
        
        for (int by = 0; by < bar_h; by++) {
            int bar_y = y + bar_area - 1 - by;
            for (int bx = 0; bx < col_w - 1; bx++) {
                tui_buffer_set(buf, col_x + bx, bar_y, tui_cell(0x2588, bar_color, t.bg));
            }
        }
        
        /* Label */
        if (labels && labels[i]) {
            tui_text_ellipsis(buf, col_x, y + height - 1, col_w - 1, labels[i], t.fg, t.bg);
        }
    }
}

void tui_chart_pie(tui_buffer_t* buf, int cx, int cy, int radius,
                   const float* values, int count, const tui_color_t* colors) {
    if (count <= 0 || radius <= 0) return;
    
    /* Calculate total */
    float total = 0;
    for (int i = 0; i < count; i++) total += values[i];
    if (total <= 0) return;
    
    /* Braille-based pie chart (2x4 resolution per cell) */
    int px_w = radius * 2 * 2;  /* pixel width */
    int px_h = radius * 2 * 4;  /* pixel height */
    int pcx = px_w / 2;
    int pcy = px_h / 2;
    
    /* Draw each pixel */
    for (int py = 0; py < px_h; py++) {
        for (int px = 0; px < px_w; px++) {
            int dx = px - pcx;
            int dy = py - pcy;
            float dist = sqrtf((float)(dx * dx) + (float)(dy * dy) * 0.25f);
            
            if (dist <= pcx) {
                /* Calculate angle (0 to 2*PI, starting from top) */
                float angle = atan2f((float)dx, (float)(-dy));
                if (angle < 0) angle += 6.28318f;
                float ratio = angle / 6.28318f;
                
                /* Find which segment */
                float cumulative = 0;
                int seg = 0;
                for (int i = 0; i < count; i++) {
                    cumulative += values[i] / total;
                    if (ratio <= cumulative) { seg = i; break; }
                }
                
                /* Set braille dot */
                int cell_x = cx - radius + px / 2;
                int cell_y = cy - radius + py / 4;
                tui_buffer_braille_set(buf, cx * 2 - radius * 2 + px, cy * 4 - radius * 4 + py, 
                                       colors[seg % count], TUI_BLACK);
            }
        }
    }
}

/* ============================================================================
 * Animation
 * ============================================================================ */

void tui_anim_start(tui_anim_t* anim, float start, float end, float duration_ms) {
    anim->start = start;
    anim->end = end;
    anim->current = start;
    anim->duration = duration_ms;
    anim->elapsed = 0;
    anim->running = true;
}

bool tui_anim_update(tui_anim_t* anim, float delta_ms) {
    if (!anim->running) return false;
    
    anim->elapsed += delta_ms;
    if (anim->elapsed >= anim->duration) {
        anim->elapsed = anim->duration;
        anim->current = anim->end;
        anim->running = false;
        return false;
    }
    
    float t = anim->elapsed / anim->duration;
    anim->current = anim->start + (anim->end - anim->start) * t;
    return true;
}

float tui_anim_value(const tui_anim_t* anim) {
    return anim->current;
}

/* Easing functions */
float tui_ease_linear(float t) {
    return t;
}

float tui_ease_in_quad(float t) {
    return t * t;
}

float tui_ease_out_quad(float t) {
    return t * (2 - t);
}

float tui_ease_in_out_quad(float t) {
    return t < 0.5f ? 2 * t * t : -1 + (4 - 2 * t) * t;
}

float tui_ease_in_cubic(float t) {
    return t * t * t;
}

float tui_ease_out_cubic(float t) {
    float t1 = t - 1;
    return t1 * t1 * t1 + 1;
}

/* ============================================================================
 * Clipboard (Platform-specific)
 * ============================================================================ */

#ifdef _WIN32
#include <windows.h>

bool tui_clipboard_set(const char* text) {
    if (!text) return false;
    
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) return false;
    
    char* pMem = (char*)GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        return false;
    }
    memcpy(pMem, text, len);
    GlobalUnlock(hMem);
    
    if (!OpenClipboard(NULL)) {
        GlobalFree(hMem);
        return false;
    }
    
    EmptyClipboard();
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
    return true;
}

char* tui_clipboard_get(void) {
    if (!OpenClipboard(NULL)) return NULL;
    
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) {
        CloseClipboard();
        return NULL;
    }
    
    char* pData = (char*)GlobalLock(hData);
    if (!pData) {
        CloseClipboard();
        return NULL;
    }
    
    size_t len = strlen(pData) + 1;
    char* result = (char*)malloc(len);
    if (result) memcpy(result, pData, len);
    
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
}

#else
/* Unix: Use OSC 52 escape sequence (works in most modern terminals) */
#include <stdio.h>
#include <unistd.h>

static char* base64_encode(const char* data, size_t len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    char* out = (char*)malloc(out_len);
    if (!out) return NULL;
    
    char* p = out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        
        *p++ = b64[(n >> 18) & 0x3F];
        *p++ = b64[(n >> 12) & 0x3F];
        *p++ = (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
        *p++ = (i + 2 < len) ? b64[n & 0x3F] : '=';
    }
    *p = '\0';
    return out;
}

bool tui_clipboard_set(const char* text) {
    if (!text) return false;
    
    char* encoded = base64_encode(text, strlen(text));
    if (!encoded) return false;
    
    /* OSC 52: \033]52;c;<base64>\007 */
    printf("\033]52;c;%s\007", encoded);
    fflush(stdout);
    
    free(encoded);
    return true;
}

char* tui_clipboard_get(void) {
    /* OSC 52 paste requires terminal support and is async - not easily implemented */
    /* Fallback: try xclip/xsel */
    FILE* fp = popen("xclip -selection clipboard -o 2>/dev/null || xsel -b -o 2>/dev/null", "r");
    if (!fp) return NULL;
    
    char* result = NULL;
    size_t cap = 0, len = 0;
    char buf[256];
    
    while (fgets(buf, sizeof(buf), fp)) {
        size_t chunk = strlen(buf);
        if (len + chunk + 1 > cap) {
            cap = (len + chunk + 1) * 2;
            char* tmp = (char*)realloc(result, cap);
            if (!tmp) { free(result); pclose(fp); return NULL; }
            result = tmp;
        }
        memcpy(result + len, buf, chunk);
        len += chunk;
    }
    if (result) result[len] = '\0';
    
    pclose(fp);
    return result;
}

#endif


/* ============================================================================
 * Menu
 * ============================================================================ */

void tui_menu_init(tui_menu_t* menu) {
    memset(menu, 0, sizeof(*menu));
}

void tui_menu_add(tui_menu_t* menu, const char* label, int id) {
    if (menu->count >= TUI_MENU_MAX_ITEMS) return;
    menu->items[menu->count].label = label;
    menu->items[menu->count].id = id;
    menu->items[menu->count].disabled = false;
    menu->items[menu->count].separator = false;
    
    int len = label ? (int)strlen(label) : 0;
    if (len + 4 > menu->width) menu->width = len + 4;
    menu->count++;
}

void tui_menu_add_separator(tui_menu_t* menu) {
    if (menu->count >= TUI_MENU_MAX_ITEMS) return;
    menu->items[menu->count].separator = true;
    menu->count++;
}

void tui_menu_show(tui_menu_t* menu, int x, int y) {
    menu->x = x;
    menu->y = y;
    menu->selected = 0;
    menu->visible = true;
    while (menu->selected < menu->count && 
           (menu->items[menu->selected].separator || menu->items[menu->selected].disabled)) {
        menu->selected++;
    }
}

void tui_menu_hide(tui_menu_t* menu) {
    menu->visible = false;
}

int tui_menu_handle(tui_menu_t* menu, const tui_event_t* event) {
    if (!menu->visible) return -1;
    
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_ESCAPE) {
            menu->visible = false;
            return -1;
        }
        if (event->key == TUI_KEY_UP) {
            do {
                menu->selected = (menu->selected - 1 + menu->count) % menu->count;
            } while (menu->items[menu->selected].separator || menu->items[menu->selected].disabled);
            return -1;
        }
        if (event->key == TUI_KEY_DOWN) {
            do {
                menu->selected = (menu->selected + 1) % menu->count;
            } while (menu->items[menu->selected].separator || menu->items[menu->selected].disabled);
            return -1;
        }
        if (event->key == TUI_KEY_ENTER) {
            menu->visible = false;
            return menu->items[menu->selected].id;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS) {
        if (event->key == TUI_KEY_MOUSE_LEFT) {
            if (tui_rect_contains(menu->x, menu->y, menu->width, menu->count, event->x, event->y)) {
                int idx = event->y - menu->y;
                if (idx >= 0 && idx < menu->count && 
                    !menu->items[idx].separator && !menu->items[idx].disabled) {
                    menu->visible = false;
                    return menu->items[idx].id;
                }
            } else {
                menu->visible = false;
            }
        }
    }
    if (event->type == TUI_EVENT_MOUSE_MOVE) {
        if (tui_rect_contains(menu->x, menu->y, menu->width, menu->count, event->x, event->y)) {
            int idx = event->y - menu->y;
            if (idx >= 0 && idx < menu->count && 
                !menu->items[idx].separator && !menu->items[idx].disabled) {
                menu->selected = idx;
            }
        }
    }
    return -1;
}

void tui_menu_draw(tui_buffer_t* buf, const tui_menu_t* menu, const tui_theme_t* theme) {
    if (!menu->visible) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    /* Shadow */
    tui_buffer_fill_color(buf, menu->x + 1, menu->y + 1, menu->width, menu->count, TUI_COLOR(20, 20, 20));
    
    /* Background */
    tui_buffer_fill_color(buf, menu->x, menu->y, menu->width, menu->count, t.bg);
    
    for (int i = 0; i < menu->count; i++) {
        int y = menu->y + i;
        if (menu->items[i].separator) {
            tui_widget_divider(buf, menu->x, y, menu->width, &t);
        } else {
            bool sel = (i == menu->selected);
            tui_color_t bg = sel ? t.highlight : t.bg;
            tui_color_t fg = menu->items[i].disabled ? t.secondary : t.fg;
            
            tui_buffer_fill_color(buf, menu->x, y, menu->width, 1, bg);
            tui_buffer_text(buf, menu->x + 2, y, menu->items[i].label, fg, bg);
        }
    }
    
    /* Border */
    tui_buffer_box(buf, menu->x - 1, menu->y - 1, menu->width + 2, menu->count + 2, t.border, t.bg);
}

/* ============================================================================
 * Textarea
 * ============================================================================ */

void tui_textarea_init(tui_textarea_t* ta) {
    memset(ta, 0, sizeof(*ta));
    ta->line_count = 1;
}

void tui_textarea_set_text(tui_textarea_t* ta, const char* text) {
    memset(ta->lines, 0, sizeof(ta->lines));
    ta->line_count = 0;
    ta->cursor_x = 0;
    ta->cursor_y = 0;
    
    if (!text) { ta->line_count = 1; return; }
    
    int line = 0, col = 0;
    while (*text && line < TUI_TEXTAREA_MAX_LINES) {
        if (*text == '\n') {
            line++;
            col = 0;
        } else if (col < TUI_TEXTAREA_LINE_LEN - 1) {
            ta->lines[line][col++] = *text;
        }
        text++;
    }
    ta->line_count = line + 1;
}

char* tui_textarea_get_text(const tui_textarea_t* ta) {
    size_t total = 0;
    for (int i = 0; i < ta->line_count; i++) {
        total += strlen(ta->lines[i]) + 1;
    }
    
    char* result = (char*)malloc(total + 1);
    if (!result) return NULL;
    
    char* p = result;
    for (int i = 0; i < ta->line_count; i++) {
        size_t len = strlen(ta->lines[i]);
        memcpy(p, ta->lines[i], len);
        p += len;
        if (i < ta->line_count - 1) *p++ = '\n';
    }
    *p = '\0';
    return result;
}

bool tui_textarea_handle(tui_textarea_t* ta, const tui_event_t* event) {
    if (!ta->focused) return false;
    if (event->type != TUI_EVENT_KEY) return false;
    
    int line_len = (int)strlen(ta->lines[ta->cursor_y]);
    
    switch (event->key) {
        case TUI_KEY_UP:
            if (ta->cursor_y > 0) {
                ta->cursor_y--;
                int new_len = (int)strlen(ta->lines[ta->cursor_y]);
                if (ta->cursor_x > new_len) ta->cursor_x = new_len;
            }
            return true;
        case TUI_KEY_DOWN:
            if (ta->cursor_y < ta->line_count - 1) {
                ta->cursor_y++;
                int new_len = (int)strlen(ta->lines[ta->cursor_y]);
                if (ta->cursor_x > new_len) ta->cursor_x = new_len;
            }
            return true;
        case TUI_KEY_LEFT:
            if (ta->cursor_x > 0) ta->cursor_x--;
            else if (ta->cursor_y > 0) {
                ta->cursor_y--;
                ta->cursor_x = (int)strlen(ta->lines[ta->cursor_y]);
            }
            return true;
        case TUI_KEY_RIGHT:
            if (ta->cursor_x < line_len) ta->cursor_x++;
            else if (ta->cursor_y < ta->line_count - 1) {
                ta->cursor_y++;
                ta->cursor_x = 0;
            }
            return true;
        case TUI_KEY_HOME:
            ta->cursor_x = 0;
            return true;
        case TUI_KEY_END:
            ta->cursor_x = line_len;
            return true;
        case TUI_KEY_ENTER:
            if (ta->line_count < TUI_TEXTAREA_MAX_LINES) {
                for (int i = ta->line_count; i > ta->cursor_y + 1; i--) {
                    memcpy(ta->lines[i], ta->lines[i-1], TUI_TEXTAREA_LINE_LEN);
                }
                strcpy(ta->lines[ta->cursor_y + 1], &ta->lines[ta->cursor_y][ta->cursor_x]);
                ta->lines[ta->cursor_y][ta->cursor_x] = '\0';
                ta->cursor_y++;
                ta->cursor_x = 0;
                ta->line_count++;
            }
            return true;
        case TUI_KEY_BACKSPACE:
            if (ta->cursor_x > 0) {
                memmove(&ta->lines[ta->cursor_y][ta->cursor_x - 1],
                        &ta->lines[ta->cursor_y][ta->cursor_x],
                        line_len - ta->cursor_x + 1);
                ta->cursor_x--;
            } else if (ta->cursor_y > 0) {
                int prev_len = (int)strlen(ta->lines[ta->cursor_y - 1]);
                strcat(ta->lines[ta->cursor_y - 1], ta->lines[ta->cursor_y]);
                for (int i = ta->cursor_y; i < ta->line_count - 1; i++) {
                    memcpy(ta->lines[i], ta->lines[i+1], TUI_TEXTAREA_LINE_LEN);
                }
                ta->line_count--;
                ta->cursor_y--;
                ta->cursor_x = prev_len;
            }
            return true;
        case TUI_KEY_DELETE:
            if (ta->cursor_x < line_len) {
                memmove(&ta->lines[ta->cursor_y][ta->cursor_x],
                        &ta->lines[ta->cursor_y][ta->cursor_x + 1],
                        line_len - ta->cursor_x);
            } else if (ta->cursor_y < ta->line_count - 1) {
                strcat(ta->lines[ta->cursor_y], ta->lines[ta->cursor_y + 1]);
                for (int i = ta->cursor_y + 1; i < ta->line_count - 1; i++) {
                    memcpy(ta->lines[i], ta->lines[i+1], TUI_TEXTAREA_LINE_LEN);
                }
                ta->line_count--;
            }
            return true;
        default:
            if (event->ch >= 32 && event->ch < 127 && line_len < TUI_TEXTAREA_LINE_LEN - 1) {
                memmove(&ta->lines[ta->cursor_y][ta->cursor_x + 1],
                        &ta->lines[ta->cursor_y][ta->cursor_x],
                        line_len - ta->cursor_x + 1);
                ta->lines[ta->cursor_y][ta->cursor_x] = (char)event->ch;
                ta->cursor_x++;
                return true;
            }
    }
    return false;
}

void tui_textarea_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                       const tui_textarea_t* ta, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    tui_buffer_fill_color(buf, x, y, width, height, t.bg);
    
    for (int row = 0; row < height && row + ta->scroll_y < ta->line_count; row++) {
        int line_idx = row + ta->scroll_y;
        const char* line = ta->lines[line_idx];
        int len = (int)strlen(line);
        
        for (int col = 0; col < width - 1 && col + ta->scroll_x < len; col++) {
            char ch = line[col + ta->scroll_x];
            tui_buffer_set(buf, x + col, y + row, tui_cell(ch, t.fg, t.bg));
        }
    }
    
    /* Cursor */
    if (ta->focused) {
        int cx = x + ta->cursor_x - ta->scroll_x;
        int cy = y + ta->cursor_y - ta->scroll_y;
        if (cx >= x && cx < x + width && cy >= y && cy < y + height) {
            tui_cell_t* cell = tui_buffer_at(buf, cx, cy);
            if (cell) {
                cell->style |= TUI_STYLE_REVERSE;
            }
        }
    }
}

/* ============================================================================
 * Split Pane
 * ============================================================================ */

void tui_split_init(tui_split_t* split, tui_split_dir_t dir, float ratio) {
    split->dir = dir;
    split->ratio = clamp_f(ratio, 0.1f, 0.9f);
    split->min_size = 5;
    split->dragging = false;
}

bool tui_split_handle(tui_split_t* split, const tui_event_t* event, int x, int y, int width, int height) {
    int split_pos;
    if (split->dir == TUI_SPLIT_HORIZONTAL) {
        split_pos = y + (int)(height * split->ratio);
    } else {
        split_pos = x + (int)(width * split->ratio);
    }
    
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        bool on_divider;
        if (split->dir == TUI_SPLIT_HORIZONTAL) {
            on_divider = (event->y == split_pos && event->x >= x && event->x < x + width);
        } else {
            on_divider = (event->x == split_pos && event->y >= y && event->y < y + height);
        }
        if (on_divider) {
            split->dragging = true;
            return true;
        }
    }
    
    if (event->type == TUI_EVENT_MOUSE_RELEASE) {
        split->dragging = false;
    }
    
    if (split->dragging && event->type == TUI_EVENT_MOUSE_MOVE) {
        if (split->dir == TUI_SPLIT_HORIZONTAL) {
            split->ratio = clamp_f((float)(event->y - y) / height, 0.1f, 0.9f);
        } else {
            split->ratio = clamp_f((float)(event->x - x) / width, 0.1f, 0.9f);
        }
        return true;
    }
    
    return false;
}

void tui_split_get_rects(const tui_split_t* split, int x, int y, int width, int height,
                         int* x1, int* y1, int* w1, int* h1,
                         int* x2, int* y2, int* w2, int* h2) {
    if (split->dir == TUI_SPLIT_HORIZONTAL) {
        int split_y = (int)(height * split->ratio);
        *x1 = x; *y1 = y; *w1 = width; *h1 = split_y;
        *x2 = x; *y2 = y + split_y + 1; *w2 = width; *h2 = height - split_y - 1;
    } else {
        int split_x = (int)(width * split->ratio);
        *x1 = x; *y1 = y; *w1 = split_x; *h1 = height;
        *x2 = x + split_x + 1; *y2 = y; *w2 = width - split_x - 1; *h2 = height;
    }
}

void tui_split_draw_divider(tui_buffer_t* buf, const tui_split_t* split,
                            int x, int y, int width, int height, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    tui_color_t color = split->dragging ? t.primary : t.border;
    
    if (split->dir == TUI_SPLIT_HORIZONTAL) {
        int split_y = y + (int)(height * split->ratio);
        tui_buffer_hline(buf, x, split_y, width, 0x2500, color, t.bg);
    } else {
        int split_x = x + (int)(width * split->ratio);
        tui_buffer_vline(buf, split_x, y, height, 0x2502, color, t.bg);
    }
}

/* ============================================================================
 * Notification
 * ============================================================================ */

void tui_notify_show(tui_notification_t* n, const char* message, tui_notify_type_t type, float duration_ms) {
    strncpy(n->message, message, sizeof(n->message) - 1);
    n->message[sizeof(n->message) - 1] = '\0';
    n->type = type;
    n->duration = duration_ms;
    n->elapsed = 0;
    n->visible = true;
}

bool tui_notify_update(tui_notification_t* n, float delta_ms) {
    if (!n->visible) return false;
    n->elapsed += delta_ms;
    if (n->elapsed >= n->duration) {
        n->visible = false;
        return false;
    }
    return true;
}

void tui_notify_draw(tui_buffer_t* buf, int screen_w, const tui_notification_t* n, const tui_theme_t* theme) {
    if (!n->visible) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    tui_color_t bg;
    switch (n->type) {
        case TUI_NOTIFY_SUCCESS: bg = t.success; break;
        case TUI_NOTIFY_WARNING: bg = t.warning; break;
        case TUI_NOTIFY_ERROR:   bg = t.error; break;
        default:                 bg = t.primary; break;
    }
    
    int len = (int)strlen(n->message);
    int width = len + 4;
    int x = screen_w - width - 2;
    int y = 1;
    
    tui_buffer_fill_color(buf, x, y, width, 1, bg);
    tui_buffer_text(buf, x + 2, y, n->message, TUI_WHITE, bg);
}


/* ============================================================================
 * Command Palette (Fuzzy Search)
 * ============================================================================ */

static int fuzzy_match(const char* pattern, const char* str) {
    if (!pattern || !*pattern) return 100;
    if (!str) return 0;
    
    int score = 0;
    const char* p = pattern;
    const char* s = str;
    int consecutive = 0;
    
    while (*p && *s) {
        char pc = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
        char sc = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        
        if (pc == sc) {
            score += 10 + consecutive * 5;
            consecutive++;
            p++;
        } else {
            consecutive = 0;
        }
        s++;
    }
    
    return *p ? 0 : score;
}

static void palette_filter(tui_palette_t* p) {
    p->filtered_count = 0;
    
    for (int i = 0; i < p->total_count; i++) {
        int score = fuzzy_match(p->query, p->items[i].label);
        if (score > 0) {
            p->items[i].score = score;
            p->filtered[p->filtered_count++] = &p->items[i];
        }
    }
    
    /* Sort by score (simple bubble sort, good enough for small lists) */
    for (int i = 0; i < p->filtered_count - 1; i++) {
        for (int j = i + 1; j < p->filtered_count; j++) {
            if (p->filtered[j]->score > p->filtered[i]->score) {
                tui_palette_item_t* tmp = p->filtered[i];
                p->filtered[i] = p->filtered[j];
                p->filtered[j] = tmp;
            }
        }
    }
    
    p->selected = 0;
    p->scroll = 0;
}

void tui_palette_init(tui_palette_t* p) {
    memset(p, 0, sizeof(*p));
}

void tui_palette_add(tui_palette_t* p, const char* label, int id) {
    if (p->total_count >= TUI_PALETTE_MAX_ITEMS) return;
    p->items[p->total_count].label = label;
    p->items[p->total_count].id = id;
    p->total_count++;
}

void tui_palette_show(tui_palette_t* p) {
    p->query[0] = '\0';
    p->query_len = 0;
    p->visible = true;
    palette_filter(p);
}

void tui_palette_hide(tui_palette_t* p) {
    p->visible = false;
}

int tui_palette_handle(tui_palette_t* p, const tui_event_t* event) {
    if (!p->visible) return -1;
    
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_ESCAPE) {
            p->visible = false;
            return -1;
        }
        if (event->key == TUI_KEY_UP && p->selected > 0) {
            p->selected--;
            if (p->selected < p->scroll) p->scroll = p->selected;
            return -1;
        }
        if (event->key == TUI_KEY_DOWN && p->selected < p->filtered_count - 1) {
            p->selected++;
            return -1;
        }
        if (event->key == TUI_KEY_ENTER && p->filtered_count > 0) {
            p->visible = false;
            return p->filtered[p->selected]->id;
        }
        if (event->key == TUI_KEY_BACKSPACE && p->query_len > 0) {
            p->query[--p->query_len] = '\0';
            palette_filter(p);
            return -1;
        }
        if (event->ch >= 32 && event->ch < 127 && p->query_len < 63) {
            p->query[p->query_len++] = (char)event->ch;
            p->query[p->query_len] = '\0';
            palette_filter(p);
            return -1;
        }
    }
    return -1;
}

void tui_palette_draw(tui_buffer_t* buf, int screen_w, int screen_h,
                      const tui_palette_t* p, const tui_theme_t* theme) {
    if (!p->visible) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    int width = 50;
    int height = 12;
    int x = (screen_w - width) / 2;
    int y = 3;
    
    /* Background */
    tui_buffer_fill_color(buf, x, y, width, height, t.bg);
    tui_buffer_box(buf, x - 1, y - 1, width + 2, height + 2, t.border, t.bg);
    
    /* Input field */
    tui_buffer_fill_color(buf, x + 1, y + 1, width - 2, 1, t.secondary);
    tui_buffer_text(buf, x + 2, y + 1, p->query, t.fg, t.secondary);
    tui_buffer_set(buf, x + 2 + p->query_len, y + 1, tui_cell_styled('_', t.fg, t.secondary, TUI_STYLE_BLINK));
    
    /* Results */
    int list_y = y + 3;
    int list_h = height - 4;
    
    for (int i = 0; i < list_h && i + p->scroll < p->filtered_count; i++) {
        int idx = i + p->scroll;
        bool sel = (idx == p->selected);
        tui_color_t bg = sel ? t.highlight : t.bg;
        
        tui_buffer_fill_color(buf, x + 1, list_y + i, width - 2, 1, bg);
        tui_text_ellipsis(buf, x + 2, list_y + i, width - 4, p->filtered[idx]->label, t.fg, bg);
    }
}

/* ============================================================================
 * Hex Viewer
 * ============================================================================ */

void tui_hexview_init(tui_hexview_t* hv, const uint8_t* data, size_t size) {
    hv->data = data;
    hv->size = size;
    hv->offset = 0;
    hv->bytes_per_row = 16;
    hv->cursor = 0;
    hv->show_ascii = true;
}

bool tui_hexview_handle(tui_hexview_t* hv, const tui_event_t* event, int height) {
    size_t visible_bytes = (size_t)height * hv->bytes_per_row;
    
    if (event->type == TUI_EVENT_KEY) {
        switch (event->key) {
            case TUI_KEY_UP:
                if (hv->offset >= (size_t)hv->bytes_per_row)
                    hv->offset -= hv->bytes_per_row;
                return true;
            case TUI_KEY_DOWN:
                if (hv->offset + visible_bytes < hv->size)
                    hv->offset += hv->bytes_per_row;
                return true;
            case TUI_KEY_PAGE_UP:
                if (hv->offset >= visible_bytes)
                    hv->offset -= visible_bytes;
                else
                    hv->offset = 0;
                return true;
            case TUI_KEY_PAGE_DOWN:
                hv->offset += visible_bytes;
                if (hv->offset >= hv->size) 
                    hv->offset = (hv->size / hv->bytes_per_row) * hv->bytes_per_row;
                return true;
            case TUI_KEY_HOME:
                hv->offset = 0;
                return true;
            case TUI_KEY_END:
                hv->offset = (hv->size / hv->bytes_per_row) * hv->bytes_per_row;
                return true;
            default: break;
        }
    }
    return false;
}

void tui_hexview_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                      const tui_hexview_t* hv, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    char line[128];
    
    for (int row = 0; row < height; row++) {
        size_t addr = hv->offset + row * hv->bytes_per_row;
        if (addr >= hv->size) break;
        
        /* Address */
        snprintf(line, sizeof(line), "%08X  ", (unsigned)addr);
        tui_buffer_text(buf, x, y + row, line, t.secondary, t.bg);
        
        /* Hex bytes */
        int hex_x = x + 10;
        for (int col = 0; col < hv->bytes_per_row; col++) {
            size_t idx = addr + col;
            if (idx < hv->size) {
                snprintf(line, sizeof(line), "%02X ", hv->data[idx]);
                tui_buffer_text(buf, hex_x + col * 3, y + row, line, t.fg, t.bg);
            }
            if (col == 7) hex_x++;
        }
        
        /* ASCII */
        if (hv->show_ascii) {
            int ascii_x = x + 10 + hv->bytes_per_row * 3 + 2;
            tui_buffer_set(buf, ascii_x - 1, y + row, tui_cell('|', t.secondary, t.bg));
            
            for (int col = 0; col < hv->bytes_per_row; col++) {
                size_t idx = addr + col;
                if (idx < hv->size) {
                    uint8_t b = hv->data[idx];
                    char ch = (b >= 32 && b < 127) ? b : '.';
                    tui_buffer_set(buf, ascii_x + col, y + row, tui_cell(ch, t.primary, t.bg));
                }
            }
        }
    }
}

/* ============================================================================
 * Log Viewer
 * ============================================================================ */

void tui_logview_init(tui_logview_t* lv, int capacity) {
    lv->entries = (tui_log_entry_t*)calloc(capacity, sizeof(tui_log_entry_t));
    lv->count = 0;
    lv->capacity = capacity;
    lv->scroll = 0;
    lv->min_level = TUI_LOG_DEBUG;
    lv->filter[0] = '\0';
    lv->auto_scroll = true;
}

void tui_logview_destroy(tui_logview_t* lv) {
    free(lv->entries);
    lv->entries = NULL;
}

void tui_logview_add(tui_logview_t* lv, const char* message, tui_log_level_t level) {
    if (lv->count >= lv->capacity) {
        memmove(&lv->entries[0], &lv->entries[1], (lv->capacity - 1) * sizeof(tui_log_entry_t));
        lv->count = lv->capacity - 1;
    }
    lv->entries[lv->count].message = message;
    lv->entries[lv->count].level = level;
    lv->count++;
}

void tui_logview_clear(tui_logview_t* lv) {
    lv->count = 0;
    lv->scroll = 0;
}

bool tui_logview_handle(tui_logview_t* lv, const tui_event_t* event, int height) {
    if (event->type == TUI_EVENT_KEY) {
        switch (event->key) {
            case TUI_KEY_UP:
                if (lv->scroll > 0) { lv->scroll--; lv->auto_scroll = false; }
                return true;
            case TUI_KEY_DOWN:
                lv->scroll++;
                return true;
            case TUI_KEY_PAGE_UP:
                lv->scroll = max_i(0, lv->scroll - height);
                lv->auto_scroll = false;
                return true;
            case TUI_KEY_PAGE_DOWN:
                lv->scroll += height;
                return true;
            case TUI_KEY_END:
                lv->auto_scroll = true;
                return true;
            default: break;
        }
    }
    return false;
}

void tui_logview_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                      const tui_logview_t* lv, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    static const char* level_str[] = { "DBG", "INF", "WRN", "ERR" };
    static const tui_color_t level_colors[] = {
        {128, 128, 128}, {100, 200, 100}, {255, 200, 100}, {255, 100, 100}
    };
    
    int visible = 0;
    int scroll = lv->auto_scroll ? max_i(0, lv->count - height) : lv->scroll;
    
    for (int i = scroll; i < lv->count && visible < height; i++) {
        const tui_log_entry_t* e = &lv->entries[i];
        if (e->level < lv->min_level) continue;
        if (lv->filter[0] && !strstr(e->message, lv->filter)) continue;
        
        int row_y = y + visible;
        tui_color_t lc = level_colors[e->level];
        
        tui_buffer_text(buf, x, row_y, level_str[e->level], lc, t.bg);
        tui_buffer_set(buf, x + 3, row_y, tui_cell(' ', t.fg, t.bg));
        tui_text_ellipsis(buf, x + 4, row_y, width - 4, e->message, t.fg, t.bg);
        
        visible++;
    }
}

/* ============================================================================
 * Statusbar
 * ============================================================================ */

void tui_statusbar_set(tui_statusbar_t* sb, const char* left, const char* center, const char* right) {
    if (left) strncpy(sb->left, left, sizeof(sb->left) - 1);
    if (center) strncpy(sb->center, center, sizeof(sb->center) - 1);
    if (right) strncpy(sb->right, right, sizeof(sb->right) - 1);
}

void tui_statusbar_draw(tui_buffer_t* buf, int y, int width,
                        const tui_statusbar_t* sb, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    tui_buffer_fill_color(buf, 0, y, width, 1, t.primary);
    
    /* Left */
    tui_buffer_text(buf, 1, y, sb->left, t.fg, t.primary);
    
    /* Center */
    int center_len = (int)strlen(sb->center);
    tui_buffer_text(buf, (width - center_len) / 2, y, sb->center, t.fg, t.primary);
    
    /* Right */
    int right_len = (int)strlen(sb->right);
    tui_buffer_text(buf, width - right_len - 1, y, sb->right, t.fg, t.primary);
}


/* ============================================================================
 * Floating Window
 * ============================================================================ */

void tui_window_init(tui_window_t* win, const char* title, int x, int y, int w, int h) {
    memset(win, 0, sizeof(*win));
    if (title) strncpy(win->title, title, sizeof(win->title) - 1);
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->min_w = 10;
    win->min_h = 5;
    win->visible = false;
}

void tui_window_show(tui_window_t* win) {
    win->visible = true;
    win->focused = true;
}

void tui_window_hide(tui_window_t* win) {
    win->visible = false;
    win->focused = false;
}

bool tui_window_handle(tui_window_t* win, const tui_event_t* event, int screen_w, int screen_h) {
    if (!win->visible) return false;
    
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        /* Title bar drag */
        if (tui_rect_contains(win->x, win->y, win->width, 1, event->x, event->y)) {
            win->dragging = true;
            win->drag_offset_x = event->x - win->x;
            win->drag_offset_y = event->y - win->y;
            win->focused = true;
            return true;
        }
        /* Resize handle (bottom-right corner) */
        if (event->x == win->x + win->width - 1 && event->y == win->y + win->height - 1) {
            win->resizing = true;
            win->focused = true;
            return true;
        }
        /* Click inside window */
        if (tui_rect_contains(win->x, win->y, win->width, win->height, event->x, event->y)) {
            win->focused = true;
            return true;
        }
    }
    
    if (event->type == TUI_EVENT_MOUSE_RELEASE) {
        win->dragging = false;
        win->resizing = false;
    }
    
    if (event->type == TUI_EVENT_MOUSE_MOVE) {
        if (win->dragging) {
            win->x = max_i(0, min_i(event->x - win->drag_offset_x, screen_w - win->width));
            win->y = max_i(0, min_i(event->y - win->drag_offset_y, screen_h - win->height));
            return true;
        }
        if (win->resizing) {
            win->width = max_i(win->min_w, event->x - win->x + 1);
            win->height = max_i(win->min_h, event->y - win->y + 1);
            return true;
        }
    }
    
    if (event->type == TUI_EVENT_KEY && win->focused) {
        if (event->key == TUI_KEY_ESCAPE) {
            win->visible = false;
            return true;
        }
    }
    
    return false;
}

void tui_window_draw_frame(tui_buffer_t* buf, const tui_window_t* win, const tui_theme_t* theme) {
    if (!win->visible) return;
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    tui_color_t border = win->focused ? t.primary : t.border;
    
    /* Shadow */
    tui_buffer_fill_color(buf, win->x + 1, win->y + 1, win->width, win->height, TUI_COLOR(20, 20, 20));
    
    /* Background */
    tui_buffer_fill_color(buf, win->x, win->y, win->width, win->height, t.bg);
    
    /* Border */
    tui_buffer_box(buf, win->x, win->y, win->width, win->height, border, t.bg);
    
    /* Title bar */
    tui_buffer_fill_color(buf, win->x + 1, win->y, win->width - 2, 1, win->focused ? t.primary : t.secondary);
    tui_text_ellipsis(buf, win->x + 2, win->y, win->width - 6, win->title, TUI_WHITE, win->focused ? t.primary : t.secondary);
    
    /* Close button */
    tui_buffer_set(buf, win->x + win->width - 2, win->y, tui_cell('x', TUI_WHITE, win->focused ? t.primary : t.secondary));
    
    /* Resize handle */
    tui_buffer_set(buf, win->x + win->width - 1, win->y + win->height - 1, tui_cell(0x25E2, t.secondary, t.bg));
}

void tui_window_get_content_rect(const tui_window_t* win, int* x, int* y, int* w, int* h) {
    *x = win->x + 1;
    *y = win->y + 1;
    *w = win->width - 2;
    *h = win->height - 2;
}

/* ============================================================================
 * Dock Panel System
 * ============================================================================ */

void tui_dock_init(tui_dock_t* dock) {
    memset(dock, 0, sizeof(*dock));
    dock->drag_panel = -1;
    for (int i = 0; i < 5; i++) {
        dock->panels[i].position = (tui_dock_pos_t)i;
        dock->panels[i].size = 20;
    }
}

void tui_dock_set_panel(tui_dock_t* dock, tui_dock_pos_t pos, const char* title, int size) {
    dock->panels[pos].title = title;
    dock->panels[pos].size = size;
    dock->panels[pos].visible = true;
}

void tui_dock_toggle(tui_dock_t* dock, tui_dock_pos_t pos) {
    dock->panels[pos].collapsed = !dock->panels[pos].collapsed;
}

bool tui_dock_handle(tui_dock_t* dock, const tui_event_t* event, int screen_w, int screen_h) {
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        /* Check divider hits */
        int rects[5][4];
        tui_dock_get_rects(dock, screen_w, screen_h, rects);
        
        tui_dock_panel_t* left = &dock->panels[TUI_DOCK_LEFT];
        tui_dock_panel_t* right = &dock->panels[TUI_DOCK_RIGHT];
        tui_dock_panel_t* top = &dock->panels[TUI_DOCK_TOP];
        tui_dock_panel_t* bottom = &dock->panels[TUI_DOCK_BOTTOM];
        
        if (left->visible && !left->collapsed && event->x == rects[TUI_DOCK_LEFT][0] + rects[TUI_DOCK_LEFT][2]) {
            dock->drag_panel = TUI_DOCK_LEFT;
            dock->drag_start = event->x;
            return true;
        }
        if (right->visible && !right->collapsed && event->x == rects[TUI_DOCK_RIGHT][0] - 1) {
            dock->drag_panel = TUI_DOCK_RIGHT;
            dock->drag_start = event->x;
            return true;
        }
        if (top->visible && !top->collapsed && event->y == rects[TUI_DOCK_TOP][1] + rects[TUI_DOCK_TOP][3]) {
            dock->drag_panel = TUI_DOCK_TOP;
            dock->drag_start = event->y;
            return true;
        }
        if (bottom->visible && !bottom->collapsed && event->y == rects[TUI_DOCK_BOTTOM][1] - 1) {
            dock->drag_panel = TUI_DOCK_BOTTOM;
            dock->drag_start = event->y;
            return true;
        }
    }
    
    if (event->type == TUI_EVENT_MOUSE_RELEASE) {
        dock->drag_panel = -1;
    }
    
    if (event->type == TUI_EVENT_MOUSE_MOVE && dock->drag_panel >= 0) {
        tui_dock_panel_t* p = &dock->panels[dock->drag_panel];
        switch (dock->drag_panel) {
            case TUI_DOCK_LEFT:
                p->size = max_i(5, event->x);
                break;
            case TUI_DOCK_RIGHT:
                p->size = max_i(5, screen_w - event->x);
                break;
            case TUI_DOCK_TOP:
                p->size = max_i(3, event->y);
                break;
            case TUI_DOCK_BOTTOM:
                p->size = max_i(3, screen_h - event->y);
                break;
        }
        return true;
    }
    
    return false;
}

void tui_dock_get_rects(const tui_dock_t* dock, int screen_w, int screen_h, int rects[5][4]) {
    int left_w = (dock->panels[TUI_DOCK_LEFT].visible && !dock->panels[TUI_DOCK_LEFT].collapsed) 
                 ? dock->panels[TUI_DOCK_LEFT].size : 0;
    int right_w = (dock->panels[TUI_DOCK_RIGHT].visible && !dock->panels[TUI_DOCK_RIGHT].collapsed)
                  ? dock->panels[TUI_DOCK_RIGHT].size : 0;
    int top_h = (dock->panels[TUI_DOCK_TOP].visible && !dock->panels[TUI_DOCK_TOP].collapsed)
                ? dock->panels[TUI_DOCK_TOP].size : 0;
    int bottom_h = (dock->panels[TUI_DOCK_BOTTOM].visible && !dock->panels[TUI_DOCK_BOTTOM].collapsed)
                   ? dock->panels[TUI_DOCK_BOTTOM].size : 0;
    
    /* Left */
    rects[TUI_DOCK_LEFT][0] = 0;
    rects[TUI_DOCK_LEFT][1] = top_h;
    rects[TUI_DOCK_LEFT][2] = left_w;
    rects[TUI_DOCK_LEFT][3] = screen_h - top_h - bottom_h;
    
    /* Right */
    rects[TUI_DOCK_RIGHT][0] = screen_w - right_w;
    rects[TUI_DOCK_RIGHT][1] = top_h;
    rects[TUI_DOCK_RIGHT][2] = right_w;
    rects[TUI_DOCK_RIGHT][3] = screen_h - top_h - bottom_h;
    
    /* Top */
    rects[TUI_DOCK_TOP][0] = 0;
    rects[TUI_DOCK_TOP][1] = 0;
    rects[TUI_DOCK_TOP][2] = screen_w;
    rects[TUI_DOCK_TOP][3] = top_h;
    
    /* Bottom */
    rects[TUI_DOCK_BOTTOM][0] = 0;
    rects[TUI_DOCK_BOTTOM][1] = screen_h - bottom_h;
    rects[TUI_DOCK_BOTTOM][2] = screen_w;
    rects[TUI_DOCK_BOTTOM][3] = bottom_h;
    
    /* Center */
    rects[TUI_DOCK_CENTER][0] = left_w;
    rects[TUI_DOCK_CENTER][1] = top_h;
    rects[TUI_DOCK_CENTER][2] = screen_w - left_w - right_w;
    rects[TUI_DOCK_CENTER][3] = screen_h - top_h - bottom_h;
}

void tui_dock_draw_frames(tui_buffer_t* buf, const tui_dock_t* dock,
                          int screen_w, int screen_h, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    int rects[5][4];
    tui_dock_get_rects(dock, screen_w, screen_h, rects);
    
    for (int i = 0; i < 4; i++) {
        if (!dock->panels[i].visible || dock->panels[i].collapsed) continue;
        
        int x = rects[i][0], y = rects[i][1], w = rects[i][2], h = rects[i][3];
        if (w <= 0 || h <= 0) continue;
        
        tui_buffer_fill_color(buf, x, y, w, h, t.bg);
        tui_buffer_box(buf, x, y, w, h, t.border, t.bg);
        
        if (dock->panels[i].title) {
            tui_text_ellipsis(buf, x + 2, y, w - 4, dock->panels[i].title, t.fg, t.bg);
        }
    }
}

/* ============================================================================
 * Border Styles
 * ============================================================================ */

static const uint32_t border_chars[8][6] = {
    /* SINGLE:  h,    v,    tl,   tr,   bl,   br */
    { 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518 },
    /* DOUBLE */
    { 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D },
    /* ROUND */
    { 0x2500, 0x2502, 0x256D, 0x256E, 0x2570, 0x256F },
    /* HEAVY */
    { 0x2501, 0x2503, 0x250F, 0x2513, 0x2517, 0x251B },
    /* ASCII */
    { '-', '|', '+', '+', '+', '+' },
    /* DASHED */
    { 0x2504, 0x2506, 0x250C, 0x2510, 0x2514, 0x2518 },
    /* DOTTED */
    { 0x2508, 0x250A, 0x250C, 0x2510, 0x2514, 0x2518 },
    /* BLOCK */
    { 0x2580, 0x2588, 0x2588, 0x2588, 0x2588, 0x2588 }
};

void tui_buffer_box_styled(tui_buffer_t* buf, int x, int y, int w, int h,
                           tui_border_style_t style, tui_color_t fg, tui_color_t bg) {
    if (w < 2 || h < 2) return;
    
    const uint32_t* chars = border_chars[style];
    
    /* Corners */
    tui_buffer_set(buf, x, y, tui_cell(chars[2], fg, bg));
    tui_buffer_set(buf, x + w - 1, y, tui_cell(chars[3], fg, bg));
    tui_buffer_set(buf, x, y + h - 1, tui_cell(chars[4], fg, bg));
    tui_buffer_set(buf, x + w - 1, y + h - 1, tui_cell(chars[5], fg, bg));
    
    /* Horizontal lines */
    for (int i = 1; i < w - 1; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(chars[0], fg, bg));
        tui_buffer_set(buf, x + i, y + h - 1, tui_cell(chars[0], fg, bg));
    }
    
    /* Vertical lines */
    for (int i = 1; i < h - 1; i++) {
        tui_buffer_set(buf, x, y + i, tui_cell(chars[1], fg, bg));
        tui_buffer_set(buf, x + w - 1, y + i, tui_cell(chars[1], fg, bg));
    }
}

/* ============================================================================
 * Canvas (Braille Drawing)
 * ============================================================================ */

tui_canvas_t* tui_canvas_create(int cell_w, int cell_h) {
    tui_canvas_t* c = (tui_canvas_t*)calloc(1, sizeof(tui_canvas_t));
    if (!c) return NULL;
    
    c->cell_w = cell_w;
    c->cell_h = cell_h;
    c->width = cell_w * 2;
    c->height = cell_h * 4;
    
    size_t bytes = ((size_t)c->width * c->height + 7) / 8;
    c->pixels = (uint8_t*)calloc(bytes, 1);
    if (!c->pixels) { free(c); return NULL; }
    
    return c;
}

void tui_canvas_destroy(tui_canvas_t* canvas) {
    if (canvas) {
        free(canvas->pixels);
        free(canvas);
    }
}

void tui_canvas_clear(tui_canvas_t* canvas) {
    size_t bytes = ((size_t)canvas->width * canvas->height + 7) / 8;
    memset(canvas->pixels, 0, bytes);
}

void tui_canvas_set(tui_canvas_t* canvas, int px, int py, bool on) {
    if (px < 0 || px >= canvas->width || py < 0 || py >= canvas->height) return;
    size_t idx = (size_t)py * canvas->width + px;
    size_t byte = idx / 8;
    uint8_t bit = 1 << (idx % 8);
    if (on) canvas->pixels[byte] |= bit;
    else canvas->pixels[byte] &= ~bit;
}

bool tui_canvas_get(const tui_canvas_t* canvas, int px, int py) {
    if (px < 0 || px >= canvas->width || py < 0 || py >= canvas->height) return false;
    size_t idx = (size_t)py * canvas->width + px;
    return (canvas->pixels[idx / 8] & (1 << (idx % 8))) != 0;
}

void tui_canvas_line(tui_canvas_t* canvas, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    
    while (1) {
        tui_canvas_set(canvas, x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void tui_canvas_rect(tui_canvas_t* canvas, int x, int y, int w, int h, bool fill) {
    if (fill) {
        for (int py = y; py < y + h; py++)
            for (int px = x; px < x + w; px++)
                tui_canvas_set(canvas, px, py, true);
    } else {
        for (int px = x; px < x + w; px++) {
            tui_canvas_set(canvas, px, y, true);
            tui_canvas_set(canvas, px, y + h - 1, true);
        }
        for (int py = y; py < y + h; py++) {
            tui_canvas_set(canvas, x, py, true);
            tui_canvas_set(canvas, x + w - 1, py, true);
        }
    }
}

void tui_canvas_circle(tui_canvas_t* canvas, int cx, int cy, int r, bool fill) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (fill ? (d <= r * r) : (d >= (r-1) * (r-1) && d <= r * r)) {
                tui_canvas_set(canvas, cx + x, cy + y, true);
            }
        }
    }
}

void tui_canvas_render(tui_buffer_t* buf, int x, int y, const tui_canvas_t* canvas,
                       tui_color_t fg, tui_color_t bg) {
    /* Braille dot positions: 0,3  1,4  2,5  6,7 (column-major in 2x4 grid) */
    static const int dot_map[8][2] = {
        {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}
    };
    
    for (int cy = 0; cy < canvas->cell_h; cy++) {
        for (int cx = 0; cx < canvas->cell_w; cx++) {
            uint32_t braille = 0x2800;
            
            for (int dot = 0; dot < 8; dot++) {
                int px = cx * 2 + dot_map[dot][0];
                int py = cy * 4 + dot_map[dot][1];
                if (tui_canvas_get(canvas, px, py)) {
                    braille |= (1 << dot);
                }
            }
            
            tui_buffer_set(buf, x + cx, y + cy, tui_cell(braille, fg, bg));
        }
    }
}

/* ============================================================================
 * Image Rendering
 * ============================================================================ */

tui_image_protocol_t tui_image_detect_protocol(void) {
    const char* term = getenv("TERM");
    const char* term_program = getenv("TERM_PROGRAM");
    
    if (term_program) {
        if (strstr(term_program, "iTerm")) return TUI_IMAGE_ITERM;
        if (strstr(term_program, "kitty")) return TUI_IMAGE_KITTY;
    }
    if (term && strstr(term, "kitty")) return TUI_IMAGE_KITTY;
    
    /* Check for sixel support via TERM */
    if (term) {
        if (strstr(term, "xterm") || strstr(term, "mlterm") || strstr(term, "foot")) {
            return TUI_IMAGE_SIXEL;
        }
    }
    
    return TUI_IMAGE_BRAILLE;
}

bool tui_image_render_file(tui_terminal_t* term, int x, int y, int w, int h, const char* path) {
    (void)term; (void)x; (void)y; (void)w; (void)h; (void)path;
    /* Full implementation would require image loading library */
    return false;
}

bool tui_image_render_data(tui_terminal_t* term, int x, int y, int w, int h,
                           const uint8_t* rgba, int img_w, int img_h) {
    (void)term; (void)x; (void)y; (void)w; (void)h; (void)rgba; (void)img_w; (void)img_h;
    /* Full implementation would require protocol-specific encoding */
    return false;
}

void tui_image_render_braille(tui_buffer_t* buf, int x, int y, int w, int h,
                              const uint8_t* gray, int img_w, int img_h,
                              tui_color_t fg, tui_color_t bg) {
    /* Simple threshold dithering to braille */
    int px_w = w * 2;
    int px_h = h * 4;
    
    for (int cy = 0; cy < h; cy++) {
        for (int cx = 0; cx < w; cx++) {
            uint32_t braille = 0x2800;
            
            static const int dot_map[8][2] = {
                {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}
            };
            
            for (int dot = 0; dot < 8; dot++) {
                int px = cx * 2 + dot_map[dot][0];
                int py = cy * 4 + dot_map[dot][1];
                
                /* Map to image coordinates */
                int ix = px * img_w / px_w;
                int iy = py * img_h / px_h;
                
                if (ix < img_w && iy < img_h) {
                    uint8_t val = gray[iy * img_w + ix];
                    if (val > 128) braille |= (1 << dot);
                }
            }
            
            tui_buffer_set(buf, x + cx, y + cy, tui_cell(braille, fg, bg));
        }
    }
}
