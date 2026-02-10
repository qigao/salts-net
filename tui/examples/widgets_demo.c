/*
 * TUI Widgets Demo - Showcases advanced TUI features
 * Charts, Tables, Tabs, Tree, Dialog, Animation
 */

#include "tui.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Messages */
enum {
    MSG_QUIT = -1,
    MSG_NONE = 0,
    MSG_TAB_CHANGE,
    MSG_TABLE_NAV,
    MSG_TREE_NAV,
    MSG_DIALOG_OPEN,
    MSG_DIALOG_CLOSE,
    MSG_TICK
};

/* Model */
typedef struct {
    tui_tabs_t tabs;
    tui_table_t table;
    tui_tree_t tree;
    tui_dialog_t dialog;
    tui_anim_t anim;
    int frame;
    clock_t last_tick;
} model_t;

/* Sample data */
static const char* tab_labels[] = { "Charts", "Table", "Tree" };

static tui_table_col_t table_cols[] = {
    { 12, TUI_ALIGN_LEFT },
    { 8, TUI_ALIGN_RIGHT },
    { 10, TUI_ALIGN_CENTER }
};
static const char* table_headers[] = { "Name", "Value", "Status" };
static const char* table_row0[] = { "Alpha", "1234", "Active" };
static const char* table_row1[] = { "Beta", "5678", "Pending" };
static const char* table_row2[] = { "Gamma", "9012", "Done" };
static const char* table_row3[] = { "Delta", "3456", "Error" };
static const char* table_row4[] = { "Epsilon", "7890", "Active" };
static const char* const* table_data[] = { table_row0, table_row1, table_row2, table_row3, table_row4 };

static tui_tree_node_t tree_children1[] = {
    { "File 1.txt", false, true, NULL, 0, NULL },
    { "File 2.txt", false, true, NULL, 0, NULL }
};
static tui_tree_node_t tree_children2[] = {
    { "Image.png", false, true, NULL, 0, NULL },
    { "Photo.jpg", false, true, NULL, 0, NULL }
};
static tui_tree_node_t tree_root_children[] = {
    { "Documents", true, false, tree_children1, 2, NULL },
    { "Pictures", false, false, tree_children2, 2, NULL },
    { "README.md", false, true, NULL, 0, NULL }
};
static tui_tree_node_t tree_root = { "Root", true, false, tree_root_children, 3, NULL };

/* Chart data */
static float chart_values[] = { 35, 60, 25, 80, 45 };
static const char* chart_labels[] = { "Mon", "Tue", "Wed", "Thu", "Fri" };
static tui_color_t pie_colors[] = {
    { 255, 100, 100 }, { 100, 255, 100 }, { 100, 100, 255 },
    { 255, 255, 100 }, { 255, 100, 255 }
};

static void* init_model(void) {
    model_t* m = calloc(1, sizeof(model_t));
    tui_tabs_init(&m->tabs, 3);
    tui_table_init(&m->table, table_cols, 3);
    tui_table_set_data(&m->table, 5, 5);
    tui_tree_init(&m->tree, &tree_root);
    m->dialog.visible = false;
    tui_anim_start(&m->anim, 0, 100, 2000);
    m->last_tick = clock();
    return m;
}

static bool update(int msg, void* model) {
    model_t* m = model;
    if (msg == MSG_QUIT) return false;
    if (msg == MSG_DIALOG_OPEN) {
        tui_dialog_show(&m->dialog, "Confirm", "Do you want to continue?", TUI_DIALOG_YES_NO);
    }
    if (msg == MSG_TICK) {
        m->frame++;
        clock_t now = clock();
        float delta = (float)(now - m->last_tick) * 1000.0f / CLOCKS_PER_SEC;
        m->last_tick = now;
        if (!tui_anim_update(&m->anim, delta)) {
            tui_anim_start(&m->anim, 0, 100, 2000);
        }
    }
    return true;
}

static void view(const void* model, tui_buffer_t* buf) {
    const model_t* m = model;
    tui_theme_t theme = tui_theme_default();
    
    tui_buffer_clear(buf);
    
    /* Title */
    tui_text_aligned(buf, 0, 0, buf->width, "TUI Advanced Widgets Demo", TUI_ALIGN_CENTER, TUI_CYAN, TUI_BLACK);
    tui_widget_divider(buf, 0, 1, buf->width, &theme);
    
    /* Tabs */
    tui_tabs_draw(buf, 0, 2, buf->width, &m->tabs, tab_labels, &theme);
    
    /* Content area */
    int content_y = 4;
    int content_h = buf->height - 6;
    
    switch (m->tabs.selected) {
        case 0: { /* Charts */
            /* Horizontal bar chart */
            tui_buffer_text(buf, 1, content_y, "Horizontal Bar Chart:", TUI_WHITE, TUI_BLACK);
            tui_chart_bar_h(buf, 1, content_y + 1, 35, 5, chart_values, 5, chart_labels, TUI_GREEN, &theme);
            
            /* Vertical bar chart */
            tui_buffer_text(buf, 40, content_y, "Vertical Bar Chart:", TUI_WHITE, TUI_BLACK);
            tui_chart_bar_v(buf, 40, content_y + 1, 25, 8, chart_values, 5, chart_labels, TUI_BLUE, &theme);
            
            /* Animation progress */
            float anim_val = tui_anim_value(&m->anim);
            tui_buffer_text(buf, 1, content_y + 8, "Animation:", TUI_WHITE, TUI_BLACK);
            tui_widget_progress(buf, 12, content_y + 8, 30, anim_val / 100.0f, &theme);
            
            /* Spinner */
            tui_buffer_text(buf, 1, content_y + 10, "Spinner:", TUI_WHITE, TUI_BLACK);
            tui_widget_spinner(buf, 10, content_y + 10, m->frame, &theme);
            break;
        }
        case 1: { /* Table */
            tui_table_draw(buf, 1, content_y, 40, content_h, &m->table, table_headers, table_data, &theme);
            
            /* Instructions */
            tui_buffer_text(buf, 45, content_y, "Up/Down: Navigate", TUI_GRAY, TUI_BLACK);
            tui_buffer_text(buf, 45, content_y + 1, "Click: Select row", TUI_GRAY, TUI_BLACK);
            break;
        }
        case 2: { /* Tree */
            tui_tree_draw(buf, 1, content_y, 35, content_h, &m->tree, &theme);
            
            /* Instructions */
            tui_buffer_text(buf, 40, content_y, "Up/Down: Navigate", TUI_GRAY, TUI_BLACK);
            tui_buffer_text(buf, 40, content_y + 1, "Enter/Right: Expand", TUI_GRAY, TUI_BLACK);
            tui_buffer_text(buf, 40, content_y + 2, "Left: Collapse", TUI_GRAY, TUI_BLACK);
            break;
        }
    }
    
    /* Footer */
    tui_widget_divider(buf, 0, buf->height - 2, buf->width, &theme);
    tui_buffer_text(buf, 1, buf->height - 1, "Tab: Switch | D: Dialog | Q: Quit", TUI_GRAY, TUI_BLACK);
    
    /* Dialog overlay */
    if (m->dialog.visible) {
        tui_dialog_draw(buf, buf->width, buf->height, &m->dialog, &theme);
    }
}

static int event_map(const tui_event_t* event) {
    static model_t* m = NULL;
    if (!m) m = NULL; /* Will be set properly in main */
    
    /* Always tick for animation */
    if (event->type == TUI_EVENT_KEY || event->type == TUI_EVENT_MOUSE_PRESS) {
        return MSG_TICK;
    }
    return MSG_NONE;
}

/* Custom run loop for proper event handling */
static void run_app(model_t* m) {
    tui_terminal_t* term = tui_terminal_create();
    tui_terminal_init(term);
    tui_terminal_enable_mouse(term);
    
    tui_buffer_t* buf = tui_terminal_buffer(term);
    tui_theme_t theme = tui_theme_default();
    bool running = true;
    
    while (running) {
        /* Update animation */
        clock_t now = clock();
        float delta = (float)(now - m->last_tick) * 1000.0f / CLOCKS_PER_SEC;
        m->last_tick = now;
        if (!tui_anim_update(&m->anim, delta)) {
            tui_anim_start(&m->anim, 0, 100, 2000);
        }
        m->frame++;
        
        /* Render */
        tui_terminal_query_size(term);
        view(m, buf);
        tui_terminal_render(term);
        
        /* Poll events (non-blocking would be better, but simplified here) */
        tui_event_t event;
        if (tui_terminal_poll(term, &event)) {
            /* Dialog handling */
            if (m->dialog.visible) {
                tui_dialog_result_t result = tui_dialog_handle(&m->dialog, &event);
                if (result != TUI_DIALOG_RESULT_NONE) continue;
            }
            
            /* Global keys */
            if (event.type == TUI_EVENT_KEY) {
                if (event.ch == 'q' || event.ch == 'Q') { running = false; continue; }
                if (event.ch == 'd' || event.ch == 'D') {
                    tui_dialog_show(&m->dialog, "Confirm", "Do you want to continue?", TUI_DIALOG_YES_NO);
                    continue;
                }
                if (event.key == TUI_KEY_TAB) {
                    m->tabs.selected = (m->tabs.selected + 1) % 3;
                    tui_terminal_refresh(term);
                    continue;
                }
            }
            
            /* Tab-specific handling */
            switch (m->tabs.selected) {
                case 1: tui_table_handle(&m->table, &event, 1, 4, 40, buf->height - 6); break;
                case 2: tui_tree_handle(&m->tree, &event, 1, 4, 35, buf->height - 6); break;
            }
            
            /* Tab bar clicks */
            if (tui_tabs_handle(&m->tabs, &event, 0, 2, buf->width)) {
                tui_terminal_refresh(term);
            }
        }
    }
    
    tui_terminal_disable_mouse(term);
    tui_terminal_cleanup(term);
    tui_terminal_destroy(term);
}

int main(void) {
    model_t* m = init_model();
    run_app(m);
    free(m);
    return 0;
}
