/*
 * TUI Advanced Features Demo
 * Showcases: Dock, Window, Canvas, Border Styles, Menu, Palette, Log Viewer
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef struct {
    tui_dock_t dock;
    tui_window_t window;
    tui_menu_t menu;
    tui_palette_t palette;
    tui_logview_t logview;
    tui_canvas_t* canvas;
    tui_notification_t notify;
    tui_statusbar_t statusbar;
    int border_style;
    int frame;
    clock_t last_tick;
    bool running;
} model_t;

static model_t* g_model = NULL;

static void add_log(model_t* m, const char* msg, tui_log_level_t level) {
    static char buf[128];
    strncpy(buf, msg, sizeof(buf) - 1);
    tui_logview_add(&m->logview, buf, level);
}

static void draw_canvas_demo(tui_canvas_t* c, int frame) {
    tui_canvas_clear(c);
    
    /* Rotating line */
    float angle = frame * 0.05f;
    int cx = c->width / 2;
    int cy = c->height / 2;
    int r = c->height / 2 - 2;
    int x1 = cx + (int)(cosf(angle) * r);
    int y1 = cy + (int)(sinf(angle) * r);
    tui_canvas_line(c, cx, cy, x1, y1);
    
    /* Circle */
    tui_canvas_circle(c, cx, cy, r / 2, false);
    
    /* Rectangle */
    tui_canvas_rect(c, 2, 2, 10, 8, false);
}

static void view(const model_t* m, tui_buffer_t* buf) {
    tui_theme_t theme = tui_theme_default();
    tui_buffer_clear(buf);
    
    /* Dock panels */
    int rects[5][4];
    tui_dock_get_rects(&m->dock, buf->width, buf->height - 1, rects);
    tui_dock_draw_frames(buf, &m->dock, buf->width, buf->height - 1, &theme);
    
    /* Left panel: Border styles */
    if (m->dock.panels[TUI_DOCK_LEFT].visible) {
        int x = rects[TUI_DOCK_LEFT][0] + 1;
        int y = rects[TUI_DOCK_LEFT][1] + 2;
        tui_buffer_text(buf, x, y, "Border Styles:", TUI_WHITE, TUI_BLACK);
        
        static const char* names[] = {"Single","Double","Round","Heavy","ASCII","Dashed","Dotted","Block"};
        for (int i = 0; i < 8; i++) {
            int by = y + 2 + i * 3;
            if (by + 2 >= rects[TUI_DOCK_LEFT][1] + rects[TUI_DOCK_LEFT][3]) break;
            tui_buffer_box_styled(buf, x, by, 12, 3, (tui_border_style_t)i, 
                                  i == m->border_style ? TUI_CYAN : TUI_GRAY, TUI_BLACK);
            tui_buffer_text(buf, x + 1, by + 1, names[i], TUI_WHITE, TUI_BLACK);
        }
    }
    
    /* Right panel: Canvas */
    if (m->dock.panels[TUI_DOCK_RIGHT].visible && m->canvas) {
        int x = rects[TUI_DOCK_RIGHT][0] + 2;
        int y = rects[TUI_DOCK_RIGHT][1] + 2;
        tui_buffer_text(buf, x, y, "Canvas (Braille):", TUI_WHITE, TUI_BLACK);
        tui_canvas_render(buf, x, y + 1, m->canvas, TUI_GREEN, TUI_BLACK);
    }
    
    /* Bottom panel: Log viewer */
    if (m->dock.panels[TUI_DOCK_BOTTOM].visible) {
        int x = rects[TUI_DOCK_BOTTOM][0] + 1;
        int y = rects[TUI_DOCK_BOTTOM][1] + 1;
        int w = rects[TUI_DOCK_BOTTOM][2] - 2;
        int h = rects[TUI_DOCK_BOTTOM][3] - 2;
        tui_logview_draw(buf, x, y, w, h, &m->logview, &theme);
    }
    
    /* Center: Instructions */
    {
        int x = rects[TUI_DOCK_CENTER][0] + 2;
        int y = rects[TUI_DOCK_CENTER][1] + 1;
        tui_buffer_text(buf, x, y, "TUI Advanced Demo", TUI_CYAN, TUI_BLACK);
        tui_buffer_text(buf, x, y + 2, "M - Open Menu", TUI_GRAY, TUI_BLACK);
        tui_buffer_text(buf, x, y + 3, "P - Command Palette", TUI_GRAY, TUI_BLACK);
        tui_buffer_text(buf, x, y + 4, "W - Toggle Window", TUI_GRAY, TUI_BLACK);
        tui_buffer_text(buf, x, y + 5, "1-4 - Toggle Dock Panels", TUI_GRAY, TUI_BLACK);
        tui_buffer_text(buf, x, y + 6, "Q - Quit", TUI_GRAY, TUI_BLACK);
    }
    
    /* Floating window */
    tui_window_draw_frame(buf, &m->window, &theme);
    if (m->window.visible) {
        int wx, wy, ww, wh;
        tui_window_get_content_rect(&m->window, &wx, &wy, &ww, &wh);
        tui_buffer_text(buf, wx + 1, wy + 1, "Floating Window!", TUI_WHITE, TUI_BLACK);
        tui_buffer_text(buf, wx + 1, wy + 2, "Drag title to move", TUI_GRAY, TUI_BLACK);
        tui_buffer_text(buf, wx + 1, wy + 3, "Drag corner to resize", TUI_GRAY, TUI_BLACK);
    }
    
    /* Menu overlay */
    tui_menu_draw(buf, &m->menu, &theme);
    
    /* Palette overlay */
    tui_palette_draw(buf, buf->width, buf->height, &m->palette, &theme);
    
    /* Notification */
    tui_notify_draw(buf, buf->width, &m->notify, &theme);
    
    /* Statusbar */
    tui_statusbar_draw(buf, buf->height - 1, buf->width, &m->statusbar, &theme);
}

static void run_app(model_t* m) {
    tui_terminal_t* term = tui_terminal_create();
    tui_terminal_init(term);
    tui_terminal_enable_mouse(term);
    
    tui_buffer_t* buf = tui_terminal_buffer(term);
    
    while (m->running) {
        /* Update */
        clock_t now = clock();
        float delta = (float)(now - m->last_tick) * 1000.0f / CLOCKS_PER_SEC;
        m->last_tick = now;
        m->frame++;
        
        tui_notify_update(&m->notify, delta);
        draw_canvas_demo(m->canvas, m->frame);
        
        /* Render */
        tui_terminal_query_size(term);
        view(m, buf);
        tui_terminal_render(term);
        
        /* Input */
        tui_event_t event;
        if (!tui_terminal_poll(term, &event)) continue;
        
        /* Palette has priority */
        if (m->palette.visible) {
            int id = tui_palette_handle(&m->palette, &event);
            if (id >= 0) {
                static char msg[64];
                snprintf(msg, sizeof(msg), "Selected command: %d", id);
                tui_notify_show(&m->notify, msg, TUI_NOTIFY_SUCCESS, 2000);
                add_log(m, msg, TUI_LOG_INFO);
            }
            continue;
        }
        
        /* Menu */
        if (m->menu.visible) {
            int id = tui_menu_handle(&m->menu, &event);
            if (id >= 0) {
                static char msg[64];
                snprintf(msg, sizeof(msg), "Menu action: %d", id);
                tui_notify_show(&m->notify, msg, TUI_NOTIFY_INFO, 2000);
                add_log(m, msg, TUI_LOG_INFO);
            }
            continue;
        }
        
        /* Window */
        if (tui_window_handle(&m->window, &event, buf->width, buf->height)) continue;
        
        /* Dock */
        if (tui_dock_handle(&m->dock, &event, buf->width, buf->height - 1)) continue;
        
        /* Log viewer */
        if (m->dock.panels[TUI_DOCK_BOTTOM].visible) {
            int rects[5][4];
            tui_dock_get_rects(&m->dock, buf->width, buf->height - 1, rects);
            if (tui_logview_handle(&m->logview, &event, rects[TUI_DOCK_BOTTOM][3] - 2)) continue;
        }
        
        /* Global keys */
        if (event.type == TUI_EVENT_KEY) {
            switch (event.ch) {
                case 'q': case 'Q': m->running = false; break;
                case 'm': case 'M': tui_menu_show(&m->menu, 10, 5); break;
                case 'p': case 'P': tui_palette_show(&m->palette); break;
                case 'w': case 'W': 
                    if (m->window.visible) tui_window_hide(&m->window);
                    else tui_window_show(&m->window);
                    break;
                case '1': tui_dock_toggle(&m->dock, TUI_DOCK_LEFT); tui_terminal_refresh(term); break;
                case '2': tui_dock_toggle(&m->dock, TUI_DOCK_RIGHT); tui_terminal_refresh(term); break;
                case '3': tui_dock_toggle(&m->dock, TUI_DOCK_TOP); tui_terminal_refresh(term); break;
                case '4': tui_dock_toggle(&m->dock, TUI_DOCK_BOTTOM); tui_terminal_refresh(term); break;
            }
            if (event.ch >= '0' && event.ch <= '7') {
                m->border_style = event.ch - '0';
            }
        }
        
        /* Right click menu */
        if (event.type == TUI_EVENT_MOUSE_PRESS && event.key == TUI_KEY_MOUSE_RIGHT) {
            tui_menu_show(&m->menu, event.x, event.y);
        }
    }
    
    tui_terminal_disable_mouse(term);
    tui_terminal_cleanup(term);
    tui_terminal_destroy(term);
}

int main(void) {
    model_t m = {0};
    g_model = &m;
    m.running = true;
    m.last_tick = clock();
    
    /* Dock */
    tui_dock_init(&m.dock);
    tui_dock_set_panel(&m.dock, TUI_DOCK_LEFT, "Borders", 18);
    tui_dock_set_panel(&m.dock, TUI_DOCK_RIGHT, "Canvas", 25);
    tui_dock_set_panel(&m.dock, TUI_DOCK_BOTTOM, "Logs", 8);
    
    /* Window */
    tui_window_init(&m.window, "Floating Window", 30, 8, 30, 10);
    
    /* Menu */
    tui_menu_init(&m.menu);
    tui_menu_add(&m.menu, "New File", 1);
    tui_menu_add(&m.menu, "Open...", 2);
    tui_menu_add(&m.menu, "Save", 3);
    tui_menu_add_separator(&m.menu);
    tui_menu_add(&m.menu, "Settings", 4);
    tui_menu_add(&m.menu, "Exit", 5);
    
    /* Palette */
    tui_palette_init(&m.palette);
    tui_palette_add(&m.palette, "Toggle Left Panel", 101);
    tui_palette_add(&m.palette, "Toggle Right Panel", 102);
    tui_palette_add(&m.palette, "Toggle Bottom Panel", 103);
    tui_palette_add(&m.palette, "Show Window", 104);
    tui_palette_add(&m.palette, "Clear Logs", 105);
    tui_palette_add(&m.palette, "Add Info Log", 106);
    tui_palette_add(&m.palette, "Add Warning Log", 107);
    tui_palette_add(&m.palette, "Add Error Log", 108);
    
    /* Log viewer */
    tui_logview_init(&m.logview, 100);
    add_log(&m, "Application started", TUI_LOG_INFO);
    add_log(&m, "Press M for menu, P for palette", TUI_LOG_DEBUG);
    
    /* Canvas */
    m.canvas = tui_canvas_create(20, 10);
    
    /* Statusbar */
    tui_statusbar_set(&m.statusbar, " TUI Demo", "Advanced Features", "Q:Quit ");
    
    run_app(&m);
    
    tui_canvas_destroy(m.canvas);
    tui_logview_destroy(&m.logview);
    
    return 0;
}
