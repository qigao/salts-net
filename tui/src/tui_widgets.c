/*
 * TUI Widgets Implementation
 */

#include "tui.h"
#include <string.h>

static inline int tui_min_i(int a, int b) { return a < b ? a : b; }
static inline float tui_clamp_f(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ============================================================================
 * Theme
 * ============================================================================ */

tui_theme_t tui_theme_default(void) {
    tui_theme_t t = {
        .bg        = TUI_COLOR(30, 30, 30),
        .fg        = TUI_COLOR(220, 220, 220),
        .primary   = TUI_COLOR(100, 149, 237),   /* Cornflower blue */
        .secondary = TUI_COLOR(128, 128, 128),
        .success   = TUI_COLOR(50, 205, 50),
        .warning   = TUI_COLOR(255, 165, 0),
        .error     = TUI_COLOR(220, 20, 60),
        .border    = TUI_COLOR(80, 80, 80),
        .highlight = TUI_COLOR(70, 130, 180),
    };
    return t;
}

/* ============================================================================
 * Progress Bar
 * ============================================================================ */

void tui_widget_progress(tui_buffer_t* buf, int x, int y, int width, float progress, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    progress = tui_clamp_f(progress, 0.0f, 1.0f);
    int filled = (int)(progress * (float)(width - 2));
    
    /* Border */
    tui_buffer_set(buf, x, y, tui_cell('[', t.fg, t.bg));
    tui_buffer_set(buf, x + width - 1, y, tui_cell(']', t.fg, t.bg));
    
    /* Fill */
    for (int i = 1; i < width - 1; i++) {
        uint32_t ch = (i - 1 < filled) ? 0x2588 : 0x2591;  /* █ or ░ */
        tui_color_t fg = (i - 1 < filled) ? t.primary : t.secondary;
        tui_buffer_set(buf, x + i, y, tui_cell(ch, fg, t.bg));
    }
}

void tui_widget_progress_label(tui_buffer_t* buf, int x, int y, int width, float progress, const char* label, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    tui_widget_progress(buf, x, y, width, progress, &t);
    
    /* Center label */
    int len = (int)strlen(label);
    int label_x = x + (width - len) / 2;
    tui_buffer_text(buf, label_x, y, label, t.fg, t.bg);
}

/* ============================================================================
 * Button
 * ============================================================================ */

void tui_widget_button(tui_buffer_t* buf, int x, int y, int width, const char* label, bool focused, bool pressed, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    tui_color_t bg = pressed ? t.highlight : (focused ? t.primary : t.secondary);
    
    /* Background */
    for (int i = 0; i < width; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(' ', t.fg, bg));
    }
    
    /* Center label */
    int len = (int)strlen(label);
    int label_x = x + (width - len) / 2;
    tui_buffer_text(buf, label_x, y, label, t.fg, bg);
}

/* ============================================================================
 * Checkbox
 * ============================================================================ */

void tui_widget_checkbox(tui_buffer_t* buf, int x, int y, bool checked, const char* label, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    uint32_t box = checked ? 0x2611 : 0x2610;  /* ☑ or ☐ */
    tui_buffer_set(buf, x, y, tui_cell(box, t.primary, t.bg));
    tui_buffer_text(buf, x + 2, y, label, t.fg, t.bg);
}

/* ============================================================================
 * Radio
 * ============================================================================ */

void tui_widget_radio(tui_buffer_t* buf, int x, int y, bool selected, const char* label, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    uint32_t dot = selected ? 0x25C9 : 0x25CB;  /* ◉ or ○ */
    tui_buffer_set(buf, x, y, tui_cell(dot, t.primary, t.bg));
    tui_buffer_text(buf, x + 2, y, label, t.fg, t.bg);
}

/* ============================================================================
 * Toggle
 * ============================================================================ */

void tui_widget_toggle(tui_buffer_t* buf, int x, int y, bool on, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    tui_color_t bg = on ? t.success : t.secondary;
    
    /* Track (4 chars wide) */
    tui_buffer_set(buf, x, y, tui_cell('(', t.fg, t.bg));
    tui_buffer_set(buf, x + 1, y, tui_cell(on ? 0x25CF : ' ', t.fg, bg));   /* ● */
    tui_buffer_set(buf, x + 2, y, tui_cell(on ? ' ' : 0x25CF, t.fg, bg));
    tui_buffer_set(buf, x + 3, y, tui_cell(')', t.fg, t.bg));
}

/* ============================================================================
 * Slider
 * ============================================================================ */

void tui_widget_slider(tui_buffer_t* buf, int x, int y, int width, float value, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    value = tui_clamp_f(value, 0.0f, 1.0f);
    int pos = (int)(value * (float)(width - 1));
    
    for (int i = 0; i < width; i++) {
        uint32_t ch = (i == pos) ? 0x25CF : 0x2500;  /* ● or ─ */
        tui_color_t fg = (i <= pos) ? t.primary : t.secondary;
        tui_buffer_set(buf, x + i, y, tui_cell(ch, fg, t.bg));
    }
}

/* ============================================================================
 * Spinner
 * ============================================================================ */

void tui_widget_spinner(tui_buffer_t* buf, int x, int y, int frame, const tui_theme_t* theme) {
    static const uint32_t frames[] = {0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827, 0x2807, 0x280F};
    /* ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏ */
    tui_theme_t t = theme ? *theme : tui_theme_default();
    uint32_t ch = frames[frame % 10];
    tui_buffer_set(buf, x, y, tui_cell(ch, t.primary, t.bg));
}

/* ============================================================================
 * Divider
 * ============================================================================ */

void tui_widget_divider(tui_buffer_t* buf, int x, int y, int width, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    for (int i = 0; i < width; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(0x2500, t.border, t.bg));  /* ─ */
    }
}

/* ============================================================================
 * Panel
 * ============================================================================ */

void tui_widget_panel(tui_buffer_t* buf, int x, int y, int width, int height, const char* title, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    
    /* Fill background */
    tui_buffer_fill_color(buf, x, y, width, height, t.bg);
    
    /* Border */
    tui_buffer_box_round(buf, x, y, width, height, t.border, t.bg);
    
    /* Title */
    if (title && title[0]) {
        int title_x = x + 2;
        tui_buffer_set(buf, title_x - 1, y, tui_cell(' ', t.fg, t.bg));
        tui_buffer_text(buf, title_x, y, title, t.fg, t.bg);
        tui_buffer_set(buf, title_x + (int)strlen(title), y, tui_cell(' ', t.fg, t.bg));
    }
}

/* ============================================================================
 * Input
 * ============================================================================ */

void tui_widget_input(tui_buffer_t* buf, int x, int y, int width, const char* text, int cursor, bool focused, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    tui_color_t bg = focused ? TUI_COLOR(50, 50, 50) : t.bg;
    
    /* Background */
    for (int i = 0; i < width; i++) {
        tui_buffer_set(buf, x + i, y, tui_cell(' ', t.fg, bg));
    }
    
    /* Text */
    int text_len = tui_min_i((int)strlen(text), width - 2);
    for (int i = 0; i < text_len; i++) {
        tui_buffer_set(buf, x + 1 + i, y, tui_cell((uint32_t)(unsigned char)text[i], t.fg, bg));
    }
    
    /* Cursor */
    if (focused && cursor >= 0 && cursor <= text_len) {
        tui_buffer_set(buf, x + 1 + cursor, y, tui_cell(0x258F, t.primary, bg));  /* ▏ */
    }
    
    /* Border */
    tui_buffer_set(buf, x, y, tui_cell(0x2502, t.border, t.bg));  /* │ */
    tui_buffer_set(buf, x + width - 1, y, tui_cell(0x2502, t.border, t.bg));
}

/* ============================================================================
 * Badge
 * ============================================================================ */

void tui_widget_badge(tui_buffer_t* buf, int x, int y, const char* text, tui_color_t bg_color, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    int len = (int)strlen(text);
    
    tui_buffer_set(buf, x, y, tui_cell(' ', t.fg, bg_color));
    tui_buffer_text(buf, x + 1, y, text, TUI_WHITE, bg_color);
    tui_buffer_set(buf, x + len + 1, y, tui_cell(' ', t.fg, bg_color));
}

/* ============================================================================
 * Scrollbar
 * ============================================================================ */

void tui_widget_scrollbar_v(tui_buffer_t* buf, int x, int y, int height, float position, float visible_ratio, const tui_theme_t* theme) {
    tui_theme_t t = theme ? *theme : tui_theme_default();
    position = tui_clamp_f(position, 0.0f, 1.0f);
    visible_ratio = tui_clamp_f(visible_ratio, 0.0f, 1.0f);
    
    int thumb_height = height;
    if (visible_ratio < 1.0f) {
        thumb_height = (int)(height * visible_ratio);
        if (thumb_height < 1) thumb_height = 1;
    }
    int thumb_pos = (int)(position * (float)(height - thumb_height));
    
    for (int i = 0; i < height; i++) {
        bool is_thumb = (i >= thumb_pos && i < thumb_pos + thumb_height);
        uint32_t ch = is_thumb ? 0x2588 : 0x2591;  /* █ or ░ */
        tui_color_t fg = is_thumb ? t.primary : t.secondary;
        tui_buffer_set(buf, x, y + i, tui_cell(ch, fg, t.bg));
    }
}


/* ============================================================================
 * Input State
 * ============================================================================ */

void tui_input_init(tui_input_state_t* state) {
    state->text[0] = '\0';
    state->cursor = 0;
    state->len = 0;
}

void tui_input_set(tui_input_state_t* state, const char* text) {
    int len = (int)strlen(text);
    if (len > 255) len = 255;
    memcpy(state->text, text, len);
    state->text[len] = '\0';
    state->len = len;
    state->cursor = len;
}

bool tui_input_handle(tui_input_state_t* state, const tui_event_t* event) {
    if (event->type != TUI_EVENT_KEY) return false;
    
    if (event->key == TUI_KEY_LEFT && state->cursor > 0) {
        state->cursor--;
        return true;
    }
    if (event->key == TUI_KEY_RIGHT && state->cursor < state->len) {
        state->cursor++;
        return true;
    }
    if (event->key == TUI_KEY_HOME) {
        state->cursor = 0;
        return true;
    }
    if (event->key == TUI_KEY_END) {
        state->cursor = state->len;
        return true;
    }
    if (event->key == TUI_KEY_BACKSPACE && state->cursor > 0) {
        memmove(&state->text[state->cursor - 1], &state->text[state->cursor], state->len - state->cursor + 1);
        state->cursor--;
        state->len--;
        return true;
    }
    if (event->key == TUI_KEY_DELETE && state->cursor < state->len) {
        memmove(&state->text[state->cursor], &state->text[state->cursor + 1], state->len - state->cursor);
        state->len--;
        return true;
    }
    if (event->ch >= 32 && event->ch < 127 && state->len < 255) {
        memmove(&state->text[state->cursor + 1], &state->text[state->cursor], state->len - state->cursor + 1);
        state->text[state->cursor] = (char)event->ch;
        state->cursor++;
        state->len++;
        return true;
    }
    return false;
}

/* ============================================================================
 * Drag State (Slider/Scrollbar)
 * ============================================================================ */

void tui_drag_init(tui_drag_state_t* state, float initial) {
    state->value = tui_clamp_f(initial, 0.0f, 1.0f);
    state->dragging = false;
}

bool tui_drag_handle_h(tui_drag_state_t* state, const tui_event_t* event, int x, int y, int width) {
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        if (tui_rect_contains(x, y, width, 1, event->x, event->y)) {
            state->dragging = true;
            state->value = tui_clamp_f((float)(event->x - x) / (float)(width - 1), 0.0f, 1.0f);
            return true;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_RELEASE) {
        state->dragging = false;
    }
    if (event->type == TUI_EVENT_MOUSE_MOVE && state->dragging) {
        state->value = tui_clamp_f((float)(event->x - x) / (float)(width - 1), 0.0f, 1.0f);
        return true;
    }
    return false;
}

bool tui_drag_handle_v(tui_drag_state_t* state, const tui_event_t* event, int x, int y, int height) {
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        if (tui_rect_contains(x, y, 1, height, event->x, event->y)) {
            state->dragging = true;
            state->value = tui_clamp_f((float)(event->y - y) / (float)(height - 1), 0.0f, 1.0f);
            return true;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_RELEASE) {
        state->dragging = false;
    }
    if (event->type == TUI_EVENT_MOUSE_MOVE && state->dragging) {
        state->value = tui_clamp_f((float)(event->y - y) / (float)(height - 1), 0.0f, 1.0f);
        return true;
    }
    return false;
}

/* ============================================================================
 * List State
 * ============================================================================ */

void tui_list_init(tui_list_state_t* state, int count, int visible) {
    state->selected = 0;
    state->count = count;
    state->scroll = 0;
    state->visible = visible;
}

void tui_list_ensure_visible(tui_list_state_t* state) {
    if (state->selected < state->scroll) {
        state->scroll = state->selected;
    }
    if (state->selected >= state->scroll + state->visible) {
        state->scroll = state->selected - state->visible + 1;
    }
}

bool tui_list_handle(tui_list_state_t* state, const tui_event_t* event, int x, int y, int width, int height) {
    if (event->type == TUI_EVENT_KEY) {
        if (event->key == TUI_KEY_UP && state->selected > 0) {
            state->selected--;
            tui_list_ensure_visible(state);
            return true;
        }
        if (event->key == TUI_KEY_DOWN && state->selected < state->count - 1) {
            state->selected++;
            tui_list_ensure_visible(state);
            return true;
        }
        if (event->key == TUI_KEY_PAGE_UP) {
            state->selected -= state->visible;
            if (state->selected < 0) state->selected = 0;
            tui_list_ensure_visible(state);
            return true;
        }
        if (event->key == TUI_KEY_PAGE_DOWN) {
            state->selected += state->visible;
            if (state->selected >= state->count) state->selected = state->count - 1;
            tui_list_ensure_visible(state);
            return true;
        }
        if (event->key == TUI_KEY_HOME) {
            state->selected = 0;
            tui_list_ensure_visible(state);
            return true;
        }
        if (event->key == TUI_KEY_END) {
            state->selected = state->count - 1;
            tui_list_ensure_visible(state);
            return true;
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS && event->key == TUI_KEY_MOUSE_LEFT) {
        if (tui_rect_contains(x, y, width, height, event->x, event->y)) {
            int clicked = state->scroll + (event->y - y);
            if (clicked >= 0 && clicked < state->count) {
                state->selected = clicked;
                return true;
            }
        }
    }
    if (event->type == TUI_EVENT_MOUSE_PRESS) {
        if (tui_rect_contains(x, y, width, height, event->x, event->y)) {
            if (event->key == TUI_KEY_MOUSE_WHEEL_UP && state->scroll > 0) {
                state->scroll--;
                return true;
            }
            if (event->key == TUI_KEY_MOUSE_WHEEL_DOWN && state->scroll < state->count - state->visible) {
                state->scroll++;
                return true;
            }
        }
    }
    return false;
}
