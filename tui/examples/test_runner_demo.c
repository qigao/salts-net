/*
 * TUI Test Runner Demo
 * Interactive test selection and execution display
 */

#include "tui.h"
#include <stdio.h>
#include <string.h>

#define MAX_TESTS 32

typedef enum {
    TEST_PENDING,
    TEST_RUNNING,
    TEST_PASSED,
    TEST_FAILED,
    TEST_SKIPPED
} test_status_t;

typedef struct {
    const char* name;
    const char* group;
    test_status_t status;
    const char* error;
} test_item_t;

typedef struct {
    test_item_t tests[MAX_TESTS];
    int test_count;
    tui_list_state_t list;
    int running_index;
    bool is_running;
    int passed;
    int failed;
    int spinner_frame;
} model_t;

enum { MSG_QUIT = 0, MSG_TICK, MSG_RUN, MSG_RUN_ALL, MSG_TOGGLE };

static model_t* g_model = NULL;

/* Fake test execution - in real use, this would run actual tests */
static void simulate_test_step(model_t* m) {
    if (!m->is_running) return;
    if (m->running_index >= m->test_count) {
        m->is_running = false;
        return;
    }
    
    test_item_t* t = &m->tests[m->running_index];
    if (t->status == TEST_RUNNING) {
        /* Simulate random pass/fail */
        if ((m->spinner_frame % 7) == 0) {
            t->status = TEST_FAILED;
            t->error = "assertion failed: expected 42, got 0";
            m->failed++;
        } else {
            t->status = TEST_PASSED;
            m->passed++;
        }
        m->running_index++;
        if (m->running_index < m->test_count) {
            m->tests[m->running_index].status = TEST_RUNNING;
        } else {
            m->is_running = false;
        }
    }
}

static bool update(int msg, void* data) {
    model_t* m = (model_t*)data;
    
    switch (msg) {
        case MSG_QUIT: return false;
        case MSG_RUN:
            if (!m->is_running && m->list.selected < m->test_count) {
                m->tests[m->list.selected].status = TEST_RUNNING;
                m->running_index = m->list.selected;
                m->is_running = true;
            }
            break;
        case MSG_RUN_ALL:
            if (!m->is_running) {
                m->passed = 0;
                m->failed = 0;
                for (int i = 0; i < m->test_count; i++) {
                    m->tests[i].status = TEST_PENDING;
                    m->tests[i].error = NULL;
                }
                m->tests[0].status = TEST_RUNNING;
                m->running_index = 0;
                m->is_running = true;
            }
            break;
    }
    
    m->spinner_frame++;
    if (m->spinner_frame % 5 == 0) {
        simulate_test_step(m);
    }
    return true;
}

static void view(const void* data, tui_buffer_t* buf) {
    model_t* m = g_model;
    tui_theme_t theme = tui_theme_default();
    
    tui_buffer_clear_color(buf, theme.bg);
    
    int W = buf->width, H = buf->height;
    
    tui_layout_t* lay = tui_layout_create();
    
    int root = tui_layout_item(lay);
    tui_layout_set_flags(lay, root, TUI_LAY_COLUMN | TUI_LAY_GAP(1));
    tui_layout_set_margins(lay, root, 1, 1, 1, 1);
    
    int header = tui_layout_item(lay);
    tui_layout_insert(lay, root, header);
    tui_layout_set_size(lay, header, 0, 3);
    
    int content = tui_layout_item(lay);
    tui_layout_insert(lay, root, content);
    tui_layout_set_flags(lay, content, TUI_LAY_ROW | TUI_LAY_GAP(1));
    
    int test_list = tui_layout_item(lay);
    tui_layout_insert(lay, content, test_list);
    tui_layout_set_grow(lay, test_list, 2);
    
    int details = tui_layout_item(lay);
    tui_layout_insert(lay, content, details);
    tui_layout_set_grow(lay, details, 1);
    
    int footer = tui_layout_item(lay);
    tui_layout_insert(lay, root, footer);
    tui_layout_set_size(lay, footer, 0, 3);
    
    tui_layout_compute(lay, root, W, H);
    
    int x, y, w, h;
    
    /* Header */
    tui_layout_get_rect(lay, header, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "TUI Test Runner", &theme);
    
    char header_text[64];
    snprintf(header_text, sizeof(header_text), "Tests: %d | ", m->test_count);
    tui_buffer_text(buf, x + 2, y + 1, header_text, theme.fg, theme.bg);
    
    int hx = x + 2 + (int)strlen(header_text);
    tui_buffer_text(buf, hx, y + 1, "Passed: ", theme.fg, theme.bg);
    hx += 8;
    char num[8];
    snprintf(num, sizeof(num), "%d", m->passed);
    tui_buffer_text(buf, hx, y + 1, num, theme.success, theme.bg);
    hx += (int)strlen(num) + 2;
    
    tui_buffer_text(buf, hx, y + 1, "Failed: ", theme.fg, theme.bg);
    hx += 8;
    snprintf(num, sizeof(num), "%d", m->failed);
    tui_buffer_text(buf, hx, y + 1, num, m->failed > 0 ? theme.error : theme.fg, theme.bg);
    
    if (m->is_running) {
        tui_widget_spinner(buf, x + w - 3, y + 1, m->spinner_frame, &theme);
    }
    
    /* Test list */
    tui_layout_get_rect(lay, test_list, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "Tests", &theme);
    
    int list_h = h - 2;
    m->list.visible = list_h;
    
    for (int i = 0; i < list_h && i + m->list.scroll < m->test_count; i++) {
        int idx = i + m->list.scroll;
        test_item_t* t = &m->tests[idx];
        bool sel = (idx == m->list.selected);
        
        tui_color_t bg = sel ? theme.highlight : theme.bg;
        tui_buffer_fill_color(buf, x + 1, y + 1 + i, w - 2, 1, bg);
        
        /* Status icon */
        uint32_t icon;
        tui_color_t icon_color;
        switch (t->status) {
            case TEST_PASSED:  icon = 0x2713; icon_color = theme.success; break;  /* ✓ */
            case TEST_FAILED:  icon = 0x2717; icon_color = theme.error; break;    /* ✗ */
            case TEST_RUNNING: icon = 0x25CF; icon_color = theme.warning; break;  /* ● */
            case TEST_SKIPPED: icon = 0x25CB; icon_color = theme.secondary; break;/* ○ */
            default:           icon = 0x25CB; icon_color = theme.secondary; break;/* ○ */
        }
        tui_buffer_set(buf, x + 2, y + 1 + i, tui_cell(icon, icon_color, bg));
        
        /* Test name */
        tui_buffer_text(buf, x + 4, y + 1 + i, t->name, theme.fg, bg);
    }
    
    /* Details panel */
    tui_layout_get_rect(lay, details, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "Details", &theme);
    
    if (m->list.selected < m->test_count) {
        test_item_t* t = &m->tests[m->list.selected];
        
        tui_buffer_text(buf, x + 2, y + 2, "Name:", theme.secondary, theme.bg);
        tui_buffer_text(buf, x + 8, y + 2, t->name, theme.fg, theme.bg);
        
        tui_buffer_text(buf, x + 2, y + 3, "Group:", theme.secondary, theme.bg);
        tui_buffer_text(buf, x + 9, y + 3, t->group, theme.fg, theme.bg);
        
        tui_buffer_text(buf, x + 2, y + 4, "Status:", theme.secondary, theme.bg);
        const char* status_str;
        tui_color_t status_color;
        switch (t->status) {
            case TEST_PASSED:  status_str = "PASSED";  status_color = theme.success; break;
            case TEST_FAILED:  status_str = "FAILED";  status_color = theme.error; break;
            case TEST_RUNNING: status_str = "RUNNING"; status_color = theme.warning; break;
            case TEST_SKIPPED: status_str = "SKIPPED"; status_color = theme.secondary; break;
            default:           status_str = "PENDING"; status_color = theme.secondary; break;
        }
        tui_buffer_text(buf, x + 10, y + 4, status_str, status_color, theme.bg);
        
        if (t->error) {
            tui_buffer_text(buf, x + 2, y + 6, "Error:", theme.error, theme.bg);
            /* Word wrap error message */
            int max_w = w - 4;
            int ey = y + 7;
            const char* p = t->error;
            while (*p && ey < y + h - 1) {
                int len = (int)strlen(p);
                if (len > max_w) len = max_w;
                char line[128];
                strncpy(line, p, len);
                line[len] = '\0';
                tui_buffer_text(buf, x + 2, ey, line, theme.fg, theme.bg);
                p += len;
                ey++;
            }
        }
    }
    
    /* Footer */
    tui_layout_get_rect(lay, footer, &x, &y, &w, &h);
    tui_widget_panel(buf, x, y, w, h, "Controls", &theme);
    tui_buffer_text(buf, x + 2, y + 1, "[Enter] Run | [a] Run All | [q] Quit", theme.secondary, theme.bg);
    
    tui_layout_destroy(lay);
}

static int event_map(const tui_event_t* e) {
    if (!g_model) return MSG_TICK;
    
    if (tui_event_is_char(e, 'q')) return MSG_QUIT;
    if (tui_event_is_char(e, 'a')) return MSG_RUN_ALL;
    if (tui_event_is(e, TUI_KEY_ENTER)) return MSG_RUN;
    
    if (tui_list_handle(&g_model->list, e, 0, 0, 40, 20)) return MSG_TICK;
    
    return MSG_TICK;
}

int main(void) {
    model_t model = {0};
    g_model = &model;
    
    /* Sample tests */
    const char* test_names[] = {
        "should create buffer",
        "should resize buffer",
        "should clear buffer",
        "should draw text",
        "should handle UTF-8",
        "should render box",
        "should compute layout",
        "should handle mouse",
        "should process input",
        "should update theme"
    };
    const char* groups[] = {
        "buffer", "buffer", "buffer", "buffer", "buffer",
        "widgets", "layout", "input", "input", "theme"
    };
    
    model.test_count = 10;
    for (int i = 0; i < model.test_count; i++) {
        model.tests[i].name = test_names[i];
        model.tests[i].group = groups[i];
        model.tests[i].status = TEST_PENDING;
        model.tests[i].error = NULL;
    }
    
    tui_list_init(&model.list, model.test_count, 10);
    
    tui_app_t* app = tui_app_create(&model, update, view, event_map);
    if (!app) {
        fprintf(stderr, "Failed to create app\n");
        return 1;
    }
    
    tui_app_run(app);
    tui_app_destroy(app);
    
    return 0;
}
