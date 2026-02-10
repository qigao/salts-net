/*
 * TUI Interactive Demo - Layout + Widgets with MVC Pattern
 */

#include "tui.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * MODEL
 * ============================================================================ */

typedef struct {
    int x, y, w, h;
} rect_t;

typedef struct {
    /* Application State */
    bool running;
    int spinner_frame;
    bool toggle_on;
    bool checkboxes[3];
    int radio_selected;
    
    /* Widget States (Complex) */
    tui_drag_state_t slider;
    tui_input_state_t input;
    tui_list_state_t menu;
    
    /* View State (Geometry for interaction) */
    struct {
        rect_t slider;
        rect_t input;
        rect_t list;
        rect_t checkboxes[3];
        rect_t radios[3];
        rect_t toggle;
    } layout;
} model_t;

typedef enum {
    MSG_QUIT = 0,
    MSG_TICK,
    MSG_REDRAW,
    MSG_INPUT_EVENT,    /* Generic event to be handled by widgets */
} msg_t;

/* Global pointer for event_map hit testing (limitation of current tui_app API) */
static model_t* g_ptr = NULL;

void model_init(model_t* m) {
    memset(m, 0, sizeof(model_t));
    m->running = true;
    m->toggle_on = true;
    m->checkboxes[0] = true;
    m->radio_selected = 0;
    
    tui_drag_init(&m->slider, 0.5f);
    tui_input_init(&m->input);
    tui_input_set(&m->input, "Hello TUI!");
    tui_list_init(&m->menu, 7, 5);
    
    g_ptr = m;
}

/* ============================================================================
 * VIEW (Layout & Rendering)
 * ============================================================================ */

static void view_render(const void* model_ptr, tui_buffer_t* buf) {
    model_t* m = (model_t*)model_ptr; /* Cast to non-const to cache layout rects */
    tui_theme_t theme = tui_theme_default();
    
    tui_buffer_clear_color(buf, theme.bg);
    
    int W = buf->width;
    int H = buf->height;
    
    tui_layout_t* lay = tui_layout_create();
    
    /* Define Layout Structure */
    int root = tui_layout_item(lay);
    tui_layout_set_flags(lay, root, TUI_LAY_COLUMN | TUI_LAY_GAP(1));
    tui_layout_set_margins(lay, root, 1, 1, 1, 1);
    
    int header = tui_layout_item(lay);
    tui_layout_insert(lay, root, header);
    tui_layout_set_size(lay, header, 0, 3);
    
    int content = tui_layout_item(lay);
    tui_layout_insert(lay, root, content);
    tui_layout_set_flags(lay, content, TUI_LAY_ROW | TUI_LAY_GAP(1));
    
    int sidebar = tui_layout_item(lay);
    tui_layout_insert(lay, content, sidebar);
    tui_layout_set_size(lay, sidebar, 22, 0);
    
    int main_area = tui_layout_item(lay);
    tui_layout_insert(lay, content, main_area);
    
    int footer = tui_layout_item(lay);
    tui_layout_insert(lay, root, footer);
    tui_layout_set_size(lay, footer, 0, 3);
    
    tui_layout_compute(lay, root, W, H);
    
    /* Render Components */
    int x, y, w, h;
    
    /* 1. Header */
    tui_layout_get_rect(lay, header, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "TurboNet TUI MVC Demo", &theme);
    tui_buffer_text(buf, x + 2, y + 1, "Separated Model, View, and Controller", theme.fg, theme.bg);
    
    /* 2. Sidebar */
    tui_layout_get_rect(lay, sidebar, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "Sidebar", &theme);
    
    int sx = x + 2, sy = y + 2;
    for (int i = 0; i < 3; i++) {
        char label[16]; snprintf(label, sizeof(label), "Option %c", 'A' + i);
        tui_widget_checkbox(buf, sx, sy + i, m->checkboxes[i], label, &theme);
        m->layout.checkboxes[i] = (rect_t){sx, sy + i, 12, 1};
    }
    
    sy += 4;
    for (int i = 0; i < 3; i++) {
        char label[16]; snprintf(label, sizeof(label), "Choice %d", i + 1);
        tui_widget_radio(buf, sx, sy + i, m->radio_selected == i, label, &theme);
        m->layout.radios[i] = (rect_t){sx, sy + i, 12, 1};
    }
    
    sy += 4;
    tui_buffer_text(buf, sx, sy, "Toggle:", theme.fg, theme.bg);
    tui_widget_toggle(buf, sx + 8, sy, m->toggle_on, &theme);
    m->layout.toggle = (rect_t){sx + 8, sy, 4, 1};
    
    /* 3. Main Area */
    tui_layout_get_rect(lay, main_area, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "Dashboard", &theme);
    
    if (w > 12 && h > 12) {
        int mx = x + 2, my = y + 2, mw = w - 4;
        
        tui_buffer_text(buf, mx, my, "Interactive Slider:", theme.fg, theme.bg);
        tui_widget_slider(buf, mx, my + 1, mw, m->slider.value, &theme);
        m->layout.slider = (rect_t){mx, my + 1, mw, 1};
        
        tui_buffer_text(buf, mx, my + 3, "Visual Progress (linked):", theme.fg, theme.bg);
        tui_widget_progress(buf, mx, my + 4, mw, m->slider.value, &theme);
        
        tui_buffer_text(buf, mx, my + 6, "Text Input:", theme.fg, theme.bg);
        tui_widget_input(buf, mx, my + 7, mw, m->input.text, m->input.cursor, true, &theme);
        m->layout.input = (rect_t){mx, my + 7, mw, 1};
        
        int list_h = h - 14;
        if (list_h > 0) {
            tui_buffer_text(buf, mx, my + 9, "Fruit List:", theme.fg, theme.bg);
            m->layout.list = (rect_t){mx, my + 10, mw, list_h};
            m->menu.visible = list_h;
            
            static const char* items[] = {"Apple", "Banana", "Cherry", "Date", "Elderberry", "Fig", "Grape"};
            for (int i = 0; i < list_h && i + m->menu.scroll < 7; i++) {
                int idx = i + m->menu.scroll;
                bool sel = (idx == m->menu.selected);
                tui_color_t bg = sel ? theme.highlight : theme.bg;
                tui_buffer_fill_color(buf, mx, my + 10 + i, mw, 1, bg);
                tui_buffer_text(buf, mx + 1, my + 10 + i, items[idx], theme.fg, bg);
            }
        }
        
        tui_widget_spinner(buf, mx + mw - 2, my, m->spinner_frame, &theme);
    }
    
    /* 4. Footer */
    tui_layout_get_rect(lay, footer, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "System Status", &theme);
    
    char status[128];
    snprintf(status, sizeof(status), "Value: %.2f | Input: \"%s\" | 'q' to Exit", 
             m->slider.value, m->input.text);
    tui_buffer_text(buf, x + 2, y + 1, status, theme.secondary, theme.bg);
    
    tui_layout_destroy(lay);
}

/* ============================================================================
 * CONTROLLER (Logic & Handling)
 * ============================================================================ */

static bool controller_update(int msg, void* model_ptr) {
    model_t* m = (model_t*)model_ptr;
    
    if (msg == MSG_QUIT) return false;
    
    if (msg == MSG_TICK) {
        m->spinner_frame++;
    }
    
    /* The MSG_REDRAW and MSG_INPUT_EVENT cases basically just acknowledge 
       that something changed in the model during the event_map process. */
    
    return m->running;
}

static int controller_event_map(const tui_event_t* e) {
    if (!g_ptr) return MSG_TICK;
    model_t* m = g_ptr;
    
    if (tui_event_is_char(e, 'q')) return MSG_QUIT;
    
    /* Handle widget-specific logic and return REDRAW if state changed */
    
    /* 1. Standard Widget Handlers */
    if (tui_input_handle(&m->input, e)) return MSG_REDRAW;
    
    rect_t* sr = &m->layout.slider;
    if (tui_drag_handle_h(&m->slider, e, sr->x, sr->y, sr->w)) return MSG_REDRAW;
    
    rect_t* lr = &m->layout.list;
    if (tui_list_handle(&m->menu, e, lr->x, lr->y, lr->w, lr->h)) return MSG_REDRAW;
    
    /* 2. Custom Click Handlers */
    if (e->type == TUI_EVENT_MOUSE_PRESS && e->key == TUI_KEY_MOUSE_LEFT) {
        /* Checkboxes */
        for (int i = 0; i < 3; i++) {
            rect_t* r = &m->layout.checkboxes[i];
            if (tui_rect_contains(r->x, r->y, r->w, r->h, e->x, e->y)) {
                m->checkboxes[i] = !m->checkboxes[i];
                return MSG_REDRAW;
            }
        }
        
        /* Radios */
        for (int i = 0; i < 3; i++) {
            rect_t* r = &m->layout.radios[i];
            if (tui_rect_contains(r->x, r->y, r->w, r->h, e->x, e->y)) {
                m->radio_selected = i;
                return MSG_REDRAW;
            }
        }
        
        /* Toggle */
        rect_t* tr = &m->layout.toggle;
        if (tui_rect_contains(tr->x, tr->y, tr->w, tr->h, e->x, e->y)) {
            m->toggle_on = !m->toggle_on;
            return MSG_REDRAW;
        }
    }
    
    return MSG_TICK;
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(void) {
    model_t model;
    model_init(&model);
    
    tui_app_t* app = tui_app_create(&model, controller_update, view_render, controller_event_map);
    if (!app) {
        fprintf(stderr, "Fatal: Could not initialize TUI application.\n");
        return 1;
    }
    
    tui_app_run(app);
    tui_app_destroy(app);
    
    return 0;
}
