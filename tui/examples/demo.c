/*
 * TUI Demo - Simple counter using TEA pattern
 */

#include "tui.h"
#include <stdio.h>

/* Model */
typedef struct {
    int count;
    int spinner_frame;
} model_t;

/* Messages */
enum {
    MSG_QUIT = 0,
    MSG_INC,
    MSG_DEC,
    MSG_TICK,
};

/* Update */
static bool update(int msg, void* m) {
    model_t* model = (model_t*)m;
    
    switch (msg) {
        case MSG_QUIT:
            return false;
        case MSG_INC:
            model->count++;
            break;
        case MSG_DEC:
            model->count--;
            break;
        case MSG_TICK:
            model->spinner_frame++;
            break;
    }
    return true;
}

/* View */
static void view(const void* m, tui_buffer_t* buf) {
    const model_t* model = (const model_t*)m;
    tui_theme_t theme = tui_theme_default();
    
    tui_buffer_clear_color(buf, theme.bg);
    
    /* Title */
    tui_widget_panel(buf, 2, 1, 40, 12, "TUI Demo", &theme);
    
    /* Counter */
    char text[64];
    snprintf(text, sizeof(text), "Count: %d", model->count);
    tui_buffer_text(buf, 4, 3, text, theme.fg, theme.bg);
    
    /* Progress bar */
    float progress = (float)(model->count % 101) / 100.0f;
    if (progress < 0) progress = -progress;
    tui_widget_progress_label(buf, 4, 5, 36, progress, "Progress", &theme);
    
    /* Spinner */
    tui_buffer_text(buf, 4, 7, "Loading: ", theme.fg, theme.bg);
    tui_widget_spinner(buf, 13, 7, model->spinner_frame, &theme);
    
    /* Slider */
    tui_buffer_text(buf, 4, 9, "Slider:", theme.fg, theme.bg);
    tui_widget_slider(buf, 12, 9, 26, progress, &theme);
    
    /* Instructions */
    tui_buffer_text(buf, 4, 11, "+/- to change, q to quit", theme.secondary, theme.bg);
}

/* Event mapping */
static int event_map(const tui_event_t* e) {
    if (tui_event_is_char(e, 'q') || tui_event_is_char(e, 'Q')) return MSG_QUIT;
    if (tui_event_is_char(e, '+') || tui_event_is_char(e, '=')) return MSG_INC;
    if (tui_event_is_char(e, '-') || tui_event_is_char(e, '_')) return MSG_DEC;
    if (tui_event_is(e, TUI_KEY_UP)) return MSG_INC;
    if (tui_event_is(e, TUI_KEY_DOWN)) return MSG_DEC;
    return MSG_TICK;  /* Default: tick spinner */
}

int main(void) {
    model_t model = {0, 0};
    
    tui_app_t* app = tui_app_create(&model, update, view, event_map);
    if (!app) {
        fprintf(stderr, "Failed to create app\n");
        return 1;
    }
    
    tui_app_run(app);
    tui_app_destroy(app);
    
    return 0;
}
