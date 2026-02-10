/*
 * TUI - Terminal User Interface Library (C99)
 *
 * A lightweight, zero-dependency TUI library for modern terminals.
 * Inspired by notcurses, bubbletea, and the Elm Architecture.
 *
 * Features:
 * - Cell-based rendering (no pixel rasterization)
 * - Double-buffered differential updates (no flicker)
 * - True color (24-bit RGB)
 * - Unicode support (emoji, CJK, box drawing)
 * - Mouse input (SGR 1006 protocol)
 * - Cross-platform (Windows, Linux, macOS)
 * - Elm Architecture (TEA) application framework
 * - Built-in widgets
 */

#ifndef TURBO_TUI_H
#define TURBO_TUI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>



#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Color
 * ============================================================================ */

typedef struct {
    uint8_t r, g, b;
} tui_color_t;

#define TUI_COLOR(r, g, b)    ((tui_color_t){(r), (g), (b)})
#define TUI_COLOR_HEX(hex)    ((tui_color_t){((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF})

#define TUI_BLACK             TUI_COLOR(0, 0, 0)
#define TUI_WHITE             TUI_COLOR(255, 255, 255)
#define TUI_RED               TUI_COLOR(255, 0, 0)
#define TUI_GREEN             TUI_COLOR(0, 255, 0)
#define TUI_BLUE              TUI_COLOR(0, 0, 255)
#define TUI_YELLOW            TUI_COLOR(255, 255, 0)
#define TUI_CYAN              TUI_COLOR(0, 255, 255)
#define TUI_MAGENTA           TUI_COLOR(255, 0, 255)
#define TUI_GRAY              TUI_COLOR(128, 128, 128)

static inline bool tui_color_eq(tui_color_t a, tui_color_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* ============================================================================
 * Style
 * ============================================================================ */

typedef enum {
    TUI_STYLE_NONE      = 0,
    TUI_STYLE_BOLD      = 1 << 0,
    TUI_STYLE_DIM       = 1 << 1,
    TUI_STYLE_ITALIC    = 1 << 2,
    TUI_STYLE_UNDERLINE = 1 << 3,
    TUI_STYLE_BLINK     = 1 << 4,
    TUI_STYLE_REVERSE   = 1 << 5,
    TUI_STYLE_STRIKE    = 1 << 6,
} tui_style_t;

/* ============================================================================
 * Cell - The fundamental unit of terminal display
 * ============================================================================ */

typedef struct {
    uint32_t    ch;         /* Unicode codepoint */
    tui_color_t fg;
    tui_color_t bg;
    uint8_t     style;      /* tui_style_t flags */
} tui_cell_t;

#define TUI_CELL_DEFAULT      ((tui_cell_t){' ', TUI_WHITE, TUI_BLACK, TUI_STYLE_NONE})

static inline tui_cell_t tui_cell(uint32_t ch, tui_color_t fg, tui_color_t bg) {
    return (tui_cell_t){ch, fg, bg, TUI_STYLE_NONE};
}

static inline tui_cell_t tui_cell_styled(uint32_t ch, tui_color_t fg, tui_color_t bg, uint8_t style) {
    return (tui_cell_t){ch, fg, bg, style};
}

static inline bool tui_cell_eq(const tui_cell_t* a, const tui_cell_t* b) {
    return a->ch == b->ch && tui_color_eq(a->fg, b->fg) && 
           tui_color_eq(a->bg, b->bg) && a->style == b->style;
}

/* ============================================================================
 * Buffer - 2D grid of cells
 * ============================================================================ */

typedef struct {
    int         width;
    int         height;
    tui_cell_t* cells;
    size_t      capacity;
    /* Dirty rect tracking */
    int         dirty_min_x;
    int         dirty_min_y;
    int         dirty_max_x;
    int         dirty_max_y;
} tui_buffer_t;

/* Buffer lifecycle */
tui_buffer_t*   tui_buffer_create(int width, int height);
void            tui_buffer_destroy(tui_buffer_t* buf);
void            tui_buffer_resize(tui_buffer_t* buf, int width, int height);

/* Cell access */
tui_cell_t*     tui_buffer_at(tui_buffer_t* buf, int x, int y);
const tui_cell_t* tui_buffer_at_const(const tui_buffer_t* buf, int x, int y);
bool            tui_buffer_in_bounds(const tui_buffer_t* buf, int x, int y);

/* Drawing primitives */
void            tui_buffer_clear(tui_buffer_t* buf);
void            tui_buffer_clear_color(tui_buffer_t* buf, tui_color_t bg);
void            tui_buffer_set(tui_buffer_t* buf, int x, int y, tui_cell_t cell);
void            tui_buffer_set_char(tui_buffer_t* buf, int x, int y, uint32_t ch);
void            tui_buffer_text(tui_buffer_t* buf, int x, int y, const char* str, tui_color_t fg, tui_color_t bg);
void            tui_buffer_fill(tui_buffer_t* buf, int x, int y, int w, int h, tui_cell_t cell);
void            tui_buffer_fill_color(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t bg);

/* Lines */
void            tui_buffer_hline(tui_buffer_t* buf, int x, int y, int len, uint32_t ch, tui_color_t fg, tui_color_t bg);
void            tui_buffer_vline(tui_buffer_t* buf, int x, int y, int len, uint32_t ch, tui_color_t fg, tui_color_t bg);

/* Boxes */
void            tui_buffer_box(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg);
void            tui_buffer_box_double(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg);
void            tui_buffer_box_round(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg);
void            tui_buffer_box_heavy(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg);

/* Gradients and bars */
void            tui_buffer_gradient_v(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t top, tui_color_t bottom);
void            tui_buffer_gradient_h(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t left, tui_color_t right);
void            tui_buffer_bar_h(tui_buffer_t* buf, int x, int y, int w, float progress, tui_color_t fg, tui_color_t bg);
void            tui_buffer_bar_v(tui_buffer_t* buf, int x, int y, int h, float progress, tui_color_t fg, tui_color_t bg);
void            tui_buffer_sparkline(tui_buffer_t* buf, int x, int y, const float* values, int count, tui_color_t fg, tui_color_t bg);

/* Braille drawing (2x4 dots per cell = 2x horizontal, 4x vertical resolution) */
void            tui_buffer_braille_set(tui_buffer_t* buf, int px, int py, tui_color_t fg, tui_color_t bg);
void            tui_buffer_braille_clear(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t bg);
void            tui_buffer_braille_line(tui_buffer_t* buf, int x0, int y0, int x1, int y1, tui_color_t fg, tui_color_t bg);
void            tui_buffer_braille_plot(tui_buffer_t* buf, int x, int y, int w, int h, const float* values, int count, tui_color_t fg, tui_color_t bg);

/* Block elements */
void            tui_buffer_shade(tui_buffer_t* buf, int x, int y, int w, int h, int level, tui_color_t fg, tui_color_t bg);
void            tui_buffer_block(tui_buffer_t* buf, int x, int y, uint8_t pattern, tui_color_t fg, tui_color_t bg);

/* Symbols and arrows */
void            tui_buffer_arrow(tui_buffer_t* buf, int x, int y, int dir, tui_color_t fg, tui_color_t bg);
void            tui_buffer_symbol(tui_buffer_t* buf, int x, int y, int sym, tui_color_t fg, tui_color_t bg);

/* Box styles */
void            tui_buffer_box_ascii(tui_buffer_t* buf, int x, int y, int w, int h, tui_color_t fg, tui_color_t bg);

/* Symbol constants */
#define TUI_SYM_CHECK       0
#define TUI_SYM_CROSS       1
#define TUI_SYM_BULLET      2
#define TUI_SYM_CIRCLE      3
#define TUI_SYM_CIRCLE_O    4
#define TUI_SYM_SQUARE      5
#define TUI_SYM_SQUARE_O    6
#define TUI_SYM_STAR        7
#define TUI_SYM_STAR_O      8
#define TUI_SYM_HEART       9
#define TUI_SYM_NOTE        10
#define TUI_SYM_LIGHTNING   11
#define TUI_SYM_HOUSE       12
#define TUI_SYM_HOURGLASS   13
#define TUI_SYM_KEYBOARD    14
#define TUI_SYM_GEAR        15

/* Dirty tracking */
void            tui_buffer_mark_dirty(tui_buffer_t* buf, int x, int y);
void            tui_buffer_mark_all_dirty(tui_buffer_t* buf);
void            tui_buffer_reset_dirty(tui_buffer_t* buf);
bool            tui_buffer_is_dirty(const tui_buffer_t* buf);

/* ============================================================================
 * Input - Keyboard and mouse events
 * ============================================================================ */

typedef enum {
    TUI_KEY_NONE = 0,
    /* ASCII printable (32-126) use their values directly */
    TUI_KEY_ENTER = 13,
    TUI_KEY_TAB = 9,
    TUI_KEY_BACKSPACE = 127,
    TUI_KEY_ESCAPE = 27,
    TUI_KEY_SPACE = 32,
    /* Arrow keys (256+) */
    TUI_KEY_UP = 256,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    /* Navigation */
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PAGE_UP,
    TUI_KEY_PAGE_DOWN,
    TUI_KEY_INSERT,
    TUI_KEY_DELETE,
    /* Function keys */
    TUI_KEY_F1, TUI_KEY_F2, TUI_KEY_F3, TUI_KEY_F4,
    TUI_KEY_F5, TUI_KEY_F6, TUI_KEY_F7, TUI_KEY_F8,
    TUI_KEY_F9, TUI_KEY_F10, TUI_KEY_F11, TUI_KEY_F12,
    /* Mouse */
    TUI_KEY_MOUSE_LEFT,
    TUI_KEY_MOUSE_RIGHT,
    TUI_KEY_MOUSE_MIDDLE,
    TUI_KEY_MOUSE_RELEASE,
    TUI_KEY_MOUSE_WHEEL_UP,
    TUI_KEY_MOUSE_WHEEL_DOWN,
} tui_key_t;

typedef enum {
    TUI_MOD_NONE  = 0,
    TUI_MOD_SHIFT = 1 << 0,
    TUI_MOD_CTRL  = 1 << 1,
    TUI_MOD_ALT   = 1 << 2,
} tui_mod_t;

typedef enum {
    TUI_EVENT_KEY,
    TUI_EVENT_MOUSE_PRESS,
    TUI_EVENT_MOUSE_RELEASE,
    TUI_EVENT_MOUSE_MOVE,
    TUI_EVENT_RESIZE,
} tui_event_type_t;

typedef struct {
    tui_event_type_t type;
    tui_key_t        key;
    uint8_t          mod;       /* tui_mod_t flags */
    uint32_t         ch;        /* Unicode character if printable */
    int              x, y;      /* Mouse position */
} tui_event_t;

/* Event helpers */
static inline bool tui_event_is_key(const tui_event_t* e) { return e->type == TUI_EVENT_KEY; }
static inline bool tui_event_is_mouse(const tui_event_t* e) { 
    return e->type == TUI_EVENT_MOUSE_PRESS || e->type == TUI_EVENT_MOUSE_RELEASE || e->type == TUI_EVENT_MOUSE_MOVE; 
}
static inline bool tui_event_is_resize(const tui_event_t* e) { return e->type == TUI_EVENT_RESIZE; }
static inline bool tui_event_is(const tui_event_t* e, tui_key_t k) { return e->type == TUI_EVENT_KEY && e->key == k; }
static inline bool tui_event_is_char(const tui_event_t* e, char c) { return e->type == TUI_EVENT_KEY && e->ch == (uint32_t)c; }
static inline bool tui_event_is_ctrl(const tui_event_t* e, char c) {
    return e->type == TUI_EVENT_KEY && (e->mod & TUI_MOD_CTRL) && 
           (e->ch == (uint32_t)c || e->ch == (uint32_t)(c - 'a' + 1));
}

/* ============================================================================
 * Terminal - Low-level terminal control
 * ============================================================================ */

typedef struct tui_terminal tui_terminal_t;

tui_terminal_t* tui_terminal_create(void);
void            tui_terminal_destroy(tui_terminal_t* term);
bool            tui_terminal_init(tui_terminal_t* term);
void            tui_terminal_cleanup(tui_terminal_t* term);

int             tui_terminal_width(const tui_terminal_t* term);
int             tui_terminal_height(const tui_terminal_t* term);
void            tui_terminal_query_size(tui_terminal_t* term);

/* Color mode */
typedef enum {
    TUI_COLOR_AUTO,         /* Auto-detect (default) */
    TUI_COLOR_256,          /* Force 256 colors */
    TUI_COLOR_TRUECOLOR     /* Force 24-bit RGB */
} tui_color_mode_t;

void            tui_terminal_set_color_mode(tui_terminal_t* term, tui_color_mode_t mode);
tui_color_mode_t tui_terminal_get_color_mode(const tui_terminal_t* term);

tui_buffer_t*   tui_terminal_buffer(tui_terminal_t* term);
void            tui_terminal_render(tui_terminal_t* term);
void            tui_terminal_refresh(tui_terminal_t* term);
void            tui_terminal_clear(tui_terminal_t* term);
void            tui_terminal_clear_color(tui_terminal_t* term, tui_color_t bg);

void            tui_terminal_show_cursor(tui_terminal_t* term);
void            tui_terminal_hide_cursor(tui_terminal_t* term);
void            tui_terminal_move_cursor(tui_terminal_t* term, int x, int y);

/* Input */
void            tui_terminal_enable_mouse(tui_terminal_t* term);
void            tui_terminal_disable_mouse(tui_terminal_t* term);
bool            tui_terminal_poll(tui_terminal_t* term, tui_event_t* event);
void            tui_terminal_wait(tui_terminal_t* term, tui_event_t* event);

/* ============================================================================
 * App - Elm Architecture (TEA) framework
 * ============================================================================ */

/*
 * TEA pattern in C:
 *   - Model: user-defined state (void*)
 *   - Msg: user-defined message type (int or enum cast to int)
 *   - update(msg, model) -> bool (false to quit)
 *   - view(model, buffer)
 *   - event_map(event) -> msg (-1 for no message)
 */

typedef bool (*tui_update_fn)(int msg, void* model);
typedef void (*tui_view_fn)(const void* model, tui_buffer_t* buf);
typedef int  (*tui_event_map_fn)(const tui_event_t* event);

typedef struct {
    void*               model;
    tui_update_fn       update;
    tui_view_fn         view;
    tui_event_map_fn    event_map;
    tui_terminal_t*     terminal;
    bool                owns_terminal;
} tui_app_t;

tui_app_t*      tui_app_create(void* model, tui_update_fn update, tui_view_fn view, tui_event_map_fn event_map);
void            tui_app_destroy(tui_app_t* app);
void            tui_app_run(tui_app_t* app);

/* Access */
void*           tui_app_model(tui_app_t* app);
tui_terminal_t* tui_app_terminal(tui_app_t* app);

/* ============================================================================
 * Widgets
 * ============================================================================ */

typedef struct {
    tui_color_t bg;
    tui_color_t fg;
    tui_color_t primary;
    tui_color_t secondary;
    tui_color_t success;
    tui_color_t warning;
    tui_color_t error;
    tui_color_t border;
    tui_color_t highlight;
} tui_theme_t;

tui_theme_t     tui_theme_default(void);

/* Widgets */
void            tui_widget_progress(tui_buffer_t* buf, int x, int y, int width, float progress, const tui_theme_t* theme);
void            tui_widget_progress_label(tui_buffer_t* buf, int x, int y, int width, float progress, const char* label, const tui_theme_t* theme);
void            tui_widget_button(tui_buffer_t* buf, int x, int y, int width, const char* label, bool focused, bool pressed, const tui_theme_t* theme);
void            tui_widget_checkbox(tui_buffer_t* buf, int x, int y, bool checked, const char* label, const tui_theme_t* theme);
void            tui_widget_radio(tui_buffer_t* buf, int x, int y, bool selected, const char* label, const tui_theme_t* theme);
void            tui_widget_toggle(tui_buffer_t* buf, int x, int y, bool on, const tui_theme_t* theme);
void            tui_widget_slider(tui_buffer_t* buf, int x, int y, int width, float value, const tui_theme_t* theme);
void            tui_widget_spinner(tui_buffer_t* buf, int x, int y, int frame, const tui_theme_t* theme);
void            tui_widget_divider(tui_buffer_t* buf, int x, int y, int width, const tui_theme_t* theme);
void            tui_widget_panel(tui_buffer_t* buf, int x, int y, int width, int height, const char* title, const tui_theme_t* theme);
void            tui_widget_input(tui_buffer_t* buf, int x, int y, int width, const char* text, int cursor, bool focused, const tui_theme_t* theme);
void            tui_widget_badge(tui_buffer_t* buf, int x, int y, const char* text, tui_color_t bg_color, const tui_theme_t* theme);
void            tui_widget_scrollbar_v(tui_buffer_t* buf, int x, int y, int height, float position, float visible_ratio, const tui_theme_t* theme);

/* ============================================================================
 * Interaction Helpers
 * ============================================================================ */

/* Hit testing */
static inline bool tui_rect_contains(int rx, int ry, int rw, int rh, int px, int py) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* Input field state */
typedef struct {
    char    text[256];
    int     cursor;
    int     len;
} tui_input_state_t;

void            tui_input_init(tui_input_state_t* state);
void            tui_input_set(tui_input_state_t* state, const char* text);
bool            tui_input_handle(tui_input_state_t* state, const tui_event_t* event);

/* Slider/scrollbar drag state */
typedef struct {
    float   value;
    bool    dragging;
} tui_drag_state_t;

void            tui_drag_init(tui_drag_state_t* state, float initial);
bool            tui_drag_handle_h(tui_drag_state_t* state, const tui_event_t* event, int x, int y, int width);
bool            tui_drag_handle_v(tui_drag_state_t* state, const tui_event_t* event, int x, int y, int height);

/* List/menu selection */
typedef struct {
    int     selected;
    int     count;
    int     scroll;
    int     visible;
} tui_list_state_t;

void            tui_list_init(tui_list_state_t* state, int count, int visible);
bool            tui_list_handle(tui_list_state_t* state, const tui_event_t* event, int x, int y, int width, int height);
void            tui_list_ensure_visible(tui_list_state_t* state);

/* ============================================================================
 * Flexbox Layout System
 * ============================================================================ */

/* Direction */
#define TUI_LAY_COLUMN          0x00
#define TUI_LAY_ROW             0x01

/* Wrap */
#define TUI_LAY_WRAP            0x02

/* Justify (main axis): bits 2-3 */
#define TUI_LAY_JUSTIFY_START   (0 << 2)
#define TUI_LAY_JUSTIFY_END     (1 << 2)
#define TUI_LAY_JUSTIFY_CENTER  (2 << 2)
#define TUI_LAY_JUSTIFY_BETWEEN (3 << 2)

/* Align (cross axis): bits 4-5 */
#define TUI_LAY_ALIGN_START     (0 << 4)
#define TUI_LAY_ALIGN_END       (1 << 4)
#define TUI_LAY_ALIGN_CENTER    (2 << 4)

/* Gap helper */
#define TUI_LAY_GAP(n)          ((uint32_t)(n) << 16)

typedef struct tui_layout tui_layout_t;

tui_layout_t*   tui_layout_create(void);
void            tui_layout_destroy(tui_layout_t* lay);
void            tui_layout_reset(tui_layout_t* lay);

int             tui_layout_item(tui_layout_t* lay);
void            tui_layout_insert(tui_layout_t* lay, int parent, int child);

void            tui_layout_set_size(tui_layout_t* lay, int id, int w, int h);
void            tui_layout_set_min_size(tui_layout_t* lay, int id, int w, int h);
void            tui_layout_set_grow(tui_layout_t* lay, int id, int grow);
void            tui_layout_set_shrink(tui_layout_t* lay, int id, int shrink);
void            tui_layout_set_flags(tui_layout_t* lay, int id, uint32_t flags);
void            tui_layout_set_margins(tui_layout_t* lay, int id, int l, int t, int r, int b);

void            tui_layout_compute(tui_layout_t* lay, int root, int w, int h);
void            tui_layout_get_rect(tui_layout_t* lay, int id, int* x, int* y, int* w, int* h);

/* ============================================================================
 * Text Utilities
 * ============================================================================ */

typedef enum {
    TUI_ALIGN_LEFT,
    TUI_ALIGN_CENTER,
    TUI_ALIGN_RIGHT
} tui_align_t;

/* Text with alignment and truncation */
void            tui_text_aligned(tui_buffer_t* buf, int x, int y, int width, const char* text, tui_align_t align, tui_color_t fg, tui_color_t bg);
void            tui_text_ellipsis(tui_buffer_t* buf, int x, int y, int width, const char* text, tui_color_t fg, tui_color_t bg);
void            tui_text_wrap(tui_buffer_t* buf, int x, int y, int width, int height, const char* text, tui_color_t fg, tui_color_t bg);
int             tui_text_wrap_height(const char* text, int width);

/* ============================================================================
 * Scrollable Viewport
 * ============================================================================ */

typedef struct {
    int     scroll_x;
    int     scroll_y;
    int     content_w;
    int     content_h;
    int     view_w;
    int     view_h;
} tui_viewport_t;

void            tui_viewport_init(tui_viewport_t* vp, int content_w, int content_h);
void            tui_viewport_set_size(tui_viewport_t* vp, int view_w, int view_h);
void            tui_viewport_scroll_to(tui_viewport_t* vp, int x, int y);
void            tui_viewport_scroll_by(tui_viewport_t* vp, int dx, int dy);
void            tui_viewport_ensure_visible(tui_viewport_t* vp, int x, int y, int w, int h);
bool            tui_viewport_handle(tui_viewport_t* vp, const tui_event_t* event, int rx, int ry);
void            tui_viewport_draw_scrollbars(tui_buffer_t* buf, int x, int y, const tui_viewport_t* vp, const tui_theme_t* theme);

/* ============================================================================
 * Table Widget
 * ============================================================================ */

typedef struct {
    int         width;
    tui_align_t align;
} tui_table_col_t;

typedef struct {
    tui_table_col_t* cols;
    int              col_count;
    int              row_count;
    int              selected_row;
    int              scroll;
    int              visible_rows;
    bool             show_header;
    bool             show_border;
} tui_table_t;

void            tui_table_init(tui_table_t* table, tui_table_col_t* cols, int col_count);
void            tui_table_set_data(tui_table_t* table, int row_count, int visible_rows);
bool            tui_table_handle(tui_table_t* table, const tui_event_t* event, int x, int y, int width, int height);
void            tui_table_draw(tui_buffer_t* buf, int x, int y, int width, int height, 
                               const tui_table_t* table, const char* const* headers,
                               const char* const* const* data, const tui_theme_t* theme);

/* ============================================================================
 * Tab Widget
 * ============================================================================ */

typedef struct {
    int     selected;
    int     count;
    int     scroll;
} tui_tabs_t;

void            tui_tabs_init(tui_tabs_t* tabs, int count);
bool            tui_tabs_handle(tui_tabs_t* tabs, const tui_event_t* event, int x, int y, int width);
void            tui_tabs_draw(tui_buffer_t* buf, int x, int y, int width, const tui_tabs_t* tabs, 
                              const char* const* labels, const tui_theme_t* theme);

/* ============================================================================
 * Tree Widget
 * ============================================================================ */

typedef struct tui_tree_node {
    const char*             label;
    bool                    expanded;
    bool                    is_leaf;
    struct tui_tree_node*   children;
    int                     child_count;
    void*                   user_data;
} tui_tree_node_t;

typedef struct {
    tui_tree_node_t*    root;
    int                 selected;
    int                 scroll;
    int                 visible;
    int                 total_visible;
} tui_tree_t;

void            tui_tree_init(tui_tree_t* tree, tui_tree_node_t* root);
bool            tui_tree_handle(tui_tree_t* tree, const tui_event_t* event, int x, int y, int width, int height);
void            tui_tree_draw(tui_buffer_t* buf, int x, int y, int width, int height, 
                              const tui_tree_t* tree, const tui_theme_t* theme);
tui_tree_node_t* tui_tree_get_selected(tui_tree_t* tree);

/* ============================================================================
 * Modal/Dialog
 * ============================================================================ */

typedef enum {
    TUI_DIALOG_OK,
    TUI_DIALOG_OK_CANCEL,
    TUI_DIALOG_YES_NO,
    TUI_DIALOG_YES_NO_CANCEL
} tui_dialog_type_t;

typedef enum {
    TUI_DIALOG_RESULT_NONE,
    TUI_DIALOG_RESULT_OK,
    TUI_DIALOG_RESULT_CANCEL,
    TUI_DIALOG_RESULT_YES,
    TUI_DIALOG_RESULT_NO
} tui_dialog_result_t;

typedef struct {
    const char*         title;
    const char*         message;
    tui_dialog_type_t   type;
    int                 selected_button;
    bool                visible;
} tui_dialog_t;

void            tui_dialog_show(tui_dialog_t* dlg, const char* title, const char* message, tui_dialog_type_t type);
void            tui_dialog_hide(tui_dialog_t* dlg);
tui_dialog_result_t tui_dialog_handle(tui_dialog_t* dlg, const tui_event_t* event);
void            tui_dialog_draw(tui_buffer_t* buf, int screen_w, int screen_h, const tui_dialog_t* dlg, const tui_theme_t* theme);

/* ============================================================================
 * Charts
 * ============================================================================ */

void            tui_chart_bar_h(tui_buffer_t* buf, int x, int y, int width, int height,
                                const float* values, int count, const char* const* labels,
                                tui_color_t bar_color, const tui_theme_t* theme);
void            tui_chart_bar_v(tui_buffer_t* buf, int x, int y, int width, int height,
                                const float* values, int count, const char* const* labels,
                                tui_color_t bar_color, const tui_theme_t* theme);
void            tui_chart_pie(tui_buffer_t* buf, int x, int y, int radius,
                              const float* values, int count, const tui_color_t* colors);

/* ============================================================================
 * Animation
 * ============================================================================ */

typedef struct {
    float   start;
    float   end;
    float   current;
    float   duration;
    float   elapsed;
    bool    running;
} tui_anim_t;

void            tui_anim_start(tui_anim_t* anim, float start, float end, float duration_ms);
bool            tui_anim_update(tui_anim_t* anim, float delta_ms);
float           tui_anim_value(const tui_anim_t* anim);

/* Easing functions */
float           tui_ease_linear(float t);
float           tui_ease_in_quad(float t);
float           tui_ease_out_quad(float t);
float           tui_ease_in_out_quad(float t);
float           tui_ease_in_cubic(float t);
float           tui_ease_out_cubic(float t);

/* ============================================================================
 * Clipboard (platform-specific)
 * ============================================================================ */

bool            tui_clipboard_set(const char* text);
char*           tui_clipboard_get(void);  /* Returns malloc'd string, caller must free */

/* ============================================================================
 * Menu (Dropdown/Context Menu)
 * ============================================================================ */

#define TUI_MENU_MAX_ITEMS  32

typedef struct {
    const char*     label;
    int             id;
    bool            disabled;
    bool            separator;
} tui_menu_item_t;

typedef struct {
    tui_menu_item_t items[TUI_MENU_MAX_ITEMS];
    int             count;
    int             selected;
    int             x, y;
    int             width;
    bool            visible;
} tui_menu_t;

void            tui_menu_init(tui_menu_t* menu);
void            tui_menu_add(tui_menu_t* menu, const char* label, int id);
void            tui_menu_add_separator(tui_menu_t* menu);
void            tui_menu_show(tui_menu_t* menu, int x, int y);
void            tui_menu_hide(tui_menu_t* menu);
int             tui_menu_handle(tui_menu_t* menu, const tui_event_t* event);  /* Returns item id or -1 */
void            tui_menu_draw(tui_buffer_t* buf, const tui_menu_t* menu, const tui_theme_t* theme);

/* ============================================================================
 * Textarea (Multi-line Text Editor)
 * ============================================================================ */

#define TUI_TEXTAREA_MAX_LINES  1024
#define TUI_TEXTAREA_LINE_LEN   256

typedef struct {
    char            lines[TUI_TEXTAREA_MAX_LINES][TUI_TEXTAREA_LINE_LEN];
    int             line_count;
    int             cursor_x;
    int             cursor_y;
    int             scroll_x;
    int             scroll_y;
    int             sel_start_x, sel_start_y;
    int             sel_end_x, sel_end_y;
    bool            has_selection;
    bool            focused;
} tui_textarea_t;

void            tui_textarea_init(tui_textarea_t* ta);
void            tui_textarea_set_text(tui_textarea_t* ta, const char* text);
char*           tui_textarea_get_text(const tui_textarea_t* ta);  /* Returns malloc'd string */
bool            tui_textarea_handle(tui_textarea_t* ta, const tui_event_t* event);
void            tui_textarea_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                                  const tui_textarea_t* ta, const tui_theme_t* theme);

/* ============================================================================
 * Split Pane
 * ============================================================================ */

typedef enum {
    TUI_SPLIT_HORIZONTAL,
    TUI_SPLIT_VERTICAL
} tui_split_dir_t;

typedef struct {
    tui_split_dir_t dir;
    float           ratio;      /* 0.0 - 1.0, position of splitter */
    int             min_size;   /* Minimum size for each pane */
    bool            dragging;
} tui_split_t;

void            tui_split_init(tui_split_t* split, tui_split_dir_t dir, float ratio);
bool            tui_split_handle(tui_split_t* split, const tui_event_t* event, int x, int y, int width, int height);
void            tui_split_get_rects(const tui_split_t* split, int x, int y, int width, int height,
                                    int* x1, int* y1, int* w1, int* h1,
                                    int* x2, int* y2, int* w2, int* h2);
void            tui_split_draw_divider(tui_buffer_t* buf, const tui_split_t* split, 
                                       int x, int y, int width, int height, const tui_theme_t* theme);

/* ============================================================================
 * Notification Toast
 * ============================================================================ */

typedef enum {
    TUI_NOTIFY_INFO,
    TUI_NOTIFY_SUCCESS,
    TUI_NOTIFY_WARNING,
    TUI_NOTIFY_ERROR
} tui_notify_type_t;

typedef struct {
    char            message[128];
    tui_notify_type_t type;
    float           duration;
    float           elapsed;
    bool            visible;
} tui_notification_t;

void            tui_notify_show(tui_notification_t* n, const char* message, tui_notify_type_t type, float duration_ms);
bool            tui_notify_update(tui_notification_t* n, float delta_ms);  /* Returns false when done */
void            tui_notify_draw(tui_buffer_t* buf, int screen_w, const tui_notification_t* n, const tui_theme_t* theme);

/* ============================================================================
 * Command Palette (Fuzzy Search List)
 * ============================================================================ */

#define TUI_PALETTE_MAX_ITEMS  256

typedef struct {
    const char*     label;
    int             id;
    int             score;      /* Match score for sorting */
} tui_palette_item_t;

typedef struct {
    tui_palette_item_t  items[TUI_PALETTE_MAX_ITEMS];
    tui_palette_item_t* filtered[TUI_PALETTE_MAX_ITEMS];
    int                 total_count;
    int                 filtered_count;
    char                query[64];
    int                 query_len;
    int                 selected;
    int                 scroll;
    bool                visible;
} tui_palette_t;

void            tui_palette_init(tui_palette_t* p);
void            tui_palette_add(tui_palette_t* p, const char* label, int id);
void            tui_palette_show(tui_palette_t* p);
void            tui_palette_hide(tui_palette_t* p);
int             tui_palette_handle(tui_palette_t* p, const tui_event_t* event);  /* Returns item id or -1 */
void            tui_palette_draw(tui_buffer_t* buf, int screen_w, int screen_h, 
                                 const tui_palette_t* p, const tui_theme_t* theme);

/* ============================================================================
 * Hex Viewer
 * ============================================================================ */

typedef struct {
    const uint8_t*  data;
    size_t          size;
    size_t          offset;
    int             bytes_per_row;
    int             cursor;
    bool            show_ascii;
} tui_hexview_t;

void            tui_hexview_init(tui_hexview_t* hv, const uint8_t* data, size_t size);
bool            tui_hexview_handle(tui_hexview_t* hv, const tui_event_t* event, int height);
void            tui_hexview_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                                 const tui_hexview_t* hv, const tui_theme_t* theme);

/* ============================================================================
 * Log Viewer (with filtering)
 * ============================================================================ */

typedef enum {
    TUI_LOG_DEBUG,
    TUI_LOG_INFO,
    TUI_LOG_WARN,
    TUI_LOG_ERROR
} tui_log_level_t;

typedef struct {
    const char*     message;
    tui_log_level_t level;
    uint32_t        timestamp;
} tui_log_entry_t;

typedef struct {
    tui_log_entry_t*    entries;
    int                 count;
    int                 capacity;
    int                 scroll;
    tui_log_level_t     min_level;
    char                filter[64];
    bool                auto_scroll;
} tui_logview_t;

void            tui_logview_init(tui_logview_t* lv, int capacity);
void            tui_logview_destroy(tui_logview_t* lv);
void            tui_logview_add(tui_logview_t* lv, const char* message, tui_log_level_t level);
void            tui_logview_clear(tui_logview_t* lv);
bool            tui_logview_handle(tui_logview_t* lv, const tui_event_t* event, int height);
void            tui_logview_draw(tui_buffer_t* buf, int x, int y, int width, int height,
                                 const tui_logview_t* lv, const tui_theme_t* theme);

/* ============================================================================
 * Statusbar
 * ============================================================================ */

typedef struct {
    char    left[128];
    char    center[128];
    char    right[128];
} tui_statusbar_t;

void            tui_statusbar_set(tui_statusbar_t* sb, const char* left, const char* center, const char* right);
void            tui_statusbar_draw(tui_buffer_t* buf, int y, int width, 
                                   const tui_statusbar_t* sb, const tui_theme_t* theme);

/* ============================================================================
 * Floating Window
 * ============================================================================ */

typedef struct {
    char            title[64];
    int             x, y;
    int             width, height;
    int             min_w, min_h;
    bool            visible;
    bool            focused;
    bool            dragging;
    bool            resizing;
    int             drag_offset_x, drag_offset_y;
} tui_window_t;

void            tui_window_init(tui_window_t* win, const char* title, int x, int y, int w, int h);
void            tui_window_show(tui_window_t* win);
void            tui_window_hide(tui_window_t* win);
bool            tui_window_handle(tui_window_t* win, const tui_event_t* event, int screen_w, int screen_h);
void            tui_window_draw_frame(tui_buffer_t* buf, const tui_window_t* win, const tui_theme_t* theme);
void            tui_window_get_content_rect(const tui_window_t* win, int* x, int* y, int* w, int* h);

/* ============================================================================
 * Dock Panel System
 * ============================================================================ */

typedef enum {
    TUI_DOCK_LEFT,
    TUI_DOCK_RIGHT,
    TUI_DOCK_TOP,
    TUI_DOCK_BOTTOM,
    TUI_DOCK_CENTER
} tui_dock_pos_t;

typedef struct {
    const char*     title;
    tui_dock_pos_t  position;
    int             size;       /* Width for left/right, height for top/bottom */
    bool            visible;
    bool            collapsed;
} tui_dock_panel_t;

typedef struct {
    tui_dock_panel_t    panels[5];  /* One for each position */
    int                 drag_panel; /* -1 if not dragging */
    int                 drag_start;
} tui_dock_t;

void            tui_dock_init(tui_dock_t* dock);
void            tui_dock_set_panel(tui_dock_t* dock, tui_dock_pos_t pos, const char* title, int size);
void            tui_dock_toggle(tui_dock_t* dock, tui_dock_pos_t pos);
bool            tui_dock_handle(tui_dock_t* dock, const tui_event_t* event, int screen_w, int screen_h);
void            tui_dock_get_rects(const tui_dock_t* dock, int screen_w, int screen_h,
                                   int rects[5][4]);  /* [pos][x,y,w,h] */
void            tui_dock_draw_frames(tui_buffer_t* buf, const tui_dock_t* dock, 
                                     int screen_w, int screen_h, const tui_theme_t* theme);

/* ============================================================================
 * Border Styles
 * ============================================================================ */

typedef enum {
    TUI_BORDER_SINGLE,      /* ─│┌┐└┘ */
    TUI_BORDER_DOUBLE,      /* ═║╔╗╚╝ */
    TUI_BORDER_ROUND,       /* ─│╭╮╰╯ */
    TUI_BORDER_HEAVY,       /* ━┃┏┓┗┛ */
    TUI_BORDER_ASCII,       /* -|++++ */
    TUI_BORDER_DASHED,      /* ┄┆┌┐└┘ */
    TUI_BORDER_DOTTED,      /* ┈┊┌┐└┘ */
    TUI_BORDER_BLOCK        /* ▀▄█ */
} tui_border_style_t;

void            tui_buffer_box_styled(tui_buffer_t* buf, int x, int y, int w, int h, 
                                      tui_border_style_t style, tui_color_t fg, tui_color_t bg);

/* ============================================================================
 * Canvas (High-resolution Braille Drawing)
 * ============================================================================ */

typedef struct {
    uint8_t*    pixels;     /* 1 bit per pixel */
    int         width;      /* Pixel width (2x cell width) */
    int         height;     /* Pixel height (4x cell height) */
    int         cell_w;     /* Cell width */
    int         cell_h;     /* Cell height */
} tui_canvas_t;

tui_canvas_t*   tui_canvas_create(int cell_w, int cell_h);
void            tui_canvas_destroy(tui_canvas_t* canvas);
void            tui_canvas_clear(tui_canvas_t* canvas);
void            tui_canvas_set(tui_canvas_t* canvas, int px, int py, bool on);
bool            tui_canvas_get(const tui_canvas_t* canvas, int px, int py);
void            tui_canvas_line(tui_canvas_t* canvas, int x0, int y0, int x1, int y1);
void            tui_canvas_rect(tui_canvas_t* canvas, int x, int y, int w, int h, bool fill);
void            tui_canvas_circle(tui_canvas_t* canvas, int cx, int cy, int r, bool fill);
void            tui_canvas_render(tui_buffer_t* buf, int x, int y, const tui_canvas_t* canvas,
                                  tui_color_t fg, tui_color_t bg);

/* ============================================================================
 * Image Rendering (Sixel/Kitty protocol detection)
 * ============================================================================ */

typedef enum {
    TUI_IMAGE_NONE,         /* No image support */
    TUI_IMAGE_SIXEL,        /* Sixel graphics */
    TUI_IMAGE_KITTY,        /* Kitty graphics protocol */
    TUI_IMAGE_ITERM,        /* iTerm2 inline images */
    TUI_IMAGE_BRAILLE       /* Fallback: braille dithering */
} tui_image_protocol_t;

tui_image_protocol_t tui_image_detect_protocol(void);
bool            tui_image_render_file(tui_terminal_t* term, int x, int y, int w, int h, const char* path);
bool            tui_image_render_data(tui_terminal_t* term, int x, int y, int w, int h,
                                      const uint8_t* rgba, int img_w, int img_h);
void            tui_image_render_braille(tui_buffer_t* buf, int x, int y, int w, int h,
                                         const uint8_t* gray, int img_w, int img_h,
                                         tui_color_t fg, tui_color_t bg);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TUI_H */
