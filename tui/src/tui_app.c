/*
 * TUI App - Elm Architecture (TEA) Implementation
 */

#include "tui.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ============================================================================
 * App Lifecycle
 * ============================================================================ */

tui_app_t* tui_app_create(void* model, tui_update_fn update, tui_view_fn view, tui_event_map_fn event_map) {
    tui_app_t* app = (tui_app_t*)calloc(1, sizeof(tui_app_t));
    if (!app) return NULL;
    
    app->model = model;
    app->update = update;
    app->view = view;
    app->event_map = event_map;
    app->terminal = tui_terminal_create();
    app->owns_terminal = true;
    
    if (!app->terminal) {
        free(app);
        return NULL;
    }
    
    return app;
}

void tui_app_destroy(tui_app_t* app) {
    if (!app) return;
    if (app->owns_terminal) {
        tui_terminal_destroy(app->terminal);
    }
    free(app);
}

/* ============================================================================
 * App Run Loop
 * ============================================================================ */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

void tui_app_run(tui_app_t* app) {
    if (!app) return;
    
    if (!tui_terminal_init(app->terminal)) return;
    tui_terminal_enable_mouse(app->terminal);
    
    bool running = true;
    bool needs_redraw = true;
    
    int old_w = tui_terminal_width(app->terminal);
    int old_h = tui_terminal_height(app->terminal);
    
    while (running) {
        /* Handle resize */
        tui_terminal_query_size(app->terminal);
        int new_w = tui_terminal_width(app->terminal);
        int new_h = tui_terminal_height(app->terminal);
        if (new_w != old_w || new_h != old_h) {
            needs_redraw = true;
            old_w = new_w;
            old_h = new_h;
        }
        
        /* Process input */
        tui_event_t event;
        while (tui_terminal_poll(app->terminal, &event)) {
            if (event.type == TUI_EVENT_RESIZE) {
                needs_redraw = true;
                continue;
            }
            
            int msg = app->event_map(&event);
            if (msg >= 0) {
                running = app->update(msg, app->model);
                needs_redraw = true;
            }
        }
        
        /* Render */
        if (needs_redraw) {
            tui_buffer_t* buf = tui_terminal_buffer(app->terminal);
            app->view(app->model, buf);
            tui_terminal_render(app->terminal);
            needs_redraw = false;
        }
        
        sleep_ms(16);  /* ~60 FPS */
    }
    
    tui_terminal_disable_mouse(app->terminal);
    tui_terminal_cleanup(app->terminal);
}

/* ============================================================================
 * Accessors
 * ============================================================================ */

void* tui_app_model(tui_app_t* app) {
    return app ? app->model : NULL;
}

tui_terminal_t* tui_app_terminal(tui_app_t* app) {
    return app ? app->terminal : NULL;
}
