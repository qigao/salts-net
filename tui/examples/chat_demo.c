/*
 * TUI Chat Demo - AI Chat Interface like Clawdbot
 * Features: Message history, input area, sidebar, slash commands, autocomplete
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_MESSAGES 100
#define MAX_MSG_LEN 1024
#define MAX_COMMANDS 20

typedef enum {
    MSG_USER,
    MSG_ASSISTANT,
    MSG_SYSTEM
} msg_role_t;

typedef struct {
    char text[MAX_MSG_LEN];
    msg_role_t role;
    time_t timestamp;
} chat_message_t;

typedef struct {
    const char* name;
    const char* description;
    const char* args;
} slash_command_t;

typedef struct {
    chat_message_t messages[MAX_MESSAGES];
    int msg_count;
    int scroll;
    tui_textarea_t input;
    bool sidebar_visible;
    int selected_chat;
    bool running;
    bool input_focused;
    tui_notification_t notify;
    clock_t last_tick;
    /* Autocomplete */
    bool autocomplete_visible;
    int autocomplete_selected;
    int autocomplete_count;
    int autocomplete_matches[MAX_COMMANDS];
} model_t;

static const char* chat_list[] = {
    "General",
    "Code Review", 
    "Debug Help",
    "Documentation",
    "Ideas"
};
static const int chat_count = 5;

static slash_command_t commands[] = {
    { "/help",      "Show available commands",      NULL },
    { "/clear",     "Clear chat history",           NULL },
    { "/new",       "Start a new conversation",     NULL },
    { "/model",     "Switch AI model",              "<model_name>" },
    { "/system",    "Set system prompt",            "<prompt>" },
    { "/export",    "Export chat to file",          "<filename>" },
    { "/image",     "Generate an image",            "<description>" },
    { "/code",      "Generate code",                "<language> <task>" },
    { "/explain",   "Explain code or concept",      "<topic>" },
    { "/summarize", "Summarize text",               NULL },
    { "/translate", "Translate text",               "<language>" },
    { "/search",    "Search the web",               "<query>" },
    { "/file",      "Attach a file",                "<path>" },
    { "/voice",     "Toggle voice mode",            NULL },
    { "/settings",  "Open settings",                NULL },
};
static const int command_count = 15;

static void update_autocomplete(model_t* m) {
    const char* text = m->input.lines[0];
    m->autocomplete_count = 0;
    m->autocomplete_visible = false;
    
    if (text[0] != '/') return;
    if (strchr(text, ' ')) return; /* Already has args */
    
    int len = (int)strlen(text);
    for (int i = 0; i < command_count; i++) {
        if (strncmp(commands[i].name, text, len) == 0) {
            m->autocomplete_matches[m->autocomplete_count++] = i;
        }
    }
    
    if (m->autocomplete_count > 0) {
        m->autocomplete_visible = true;
        if (m->autocomplete_selected >= m->autocomplete_count) {
            m->autocomplete_selected = 0;
        }
    }
}

static void apply_autocomplete(model_t* m) {
    if (!m->autocomplete_visible || m->autocomplete_count == 0) return;
    
    int idx = m->autocomplete_matches[m->autocomplete_selected];
    strcpy(m->input.lines[0], commands[idx].name);
    if (commands[idx].args) {
        strcat(m->input.lines[0], " ");
    }
    m->input.cursor_x = (int)strlen(m->input.lines[0]);
    m->autocomplete_visible = false;
}

static void add_message(model_t* m, const char* text, msg_role_t role) {
    if (m->msg_count >= MAX_MESSAGES) {
        memmove(&m->messages[0], &m->messages[1], (MAX_MESSAGES - 1) * sizeof(chat_message_t));
        m->msg_count = MAX_MESSAGES - 1;
    }
    strncpy(m->messages[m->msg_count].text, text, MAX_MSG_LEN - 1);
    m->messages[m->msg_count].role = role;
    m->messages[m->msg_count].timestamp = time(NULL);
    m->msg_count++;
}

static void handle_command(model_t* m, const char* cmd) {
    static char response[MAX_MSG_LEN];
    
    if (strcmp(cmd, "/help") == 0) {
        strcpy(response, "Available slash commands:\n\n");
        for (int i = 0; i < command_count; i++) {
            char line[128];
            if (commands[i].args) {
                snprintf(line, sizeof(line), "- %s %s - %s\n", 
                         commands[i].name, commands[i].args, commands[i].description);
            } else {
                snprintf(line, sizeof(line), "- %s - %s\n", 
                         commands[i].name, commands[i].description);
            }
            strcat(response, line);
        }
        add_message(m, response, MSG_SYSTEM);
    } else if (strcmp(cmd, "/clear") == 0) {
        m->msg_count = 0;
        tui_notify_show(&m->notify, "Chat cleared", TUI_NOTIFY_INFO, 2000);
    } else if (strcmp(cmd, "/new") == 0) {
        m->msg_count = 0;
        add_message(m, "Started a new conversation. How can I help you?", MSG_ASSISTANT);
    } else if (strncmp(cmd, "/model ", 7) == 0) {
        snprintf(response, sizeof(response), "Switched to model: %s", cmd + 7);
        tui_notify_show(&m->notify, response, TUI_NOTIFY_SUCCESS, 2000);
    } else if (strncmp(cmd, "/code ", 6) == 0) {
        snprintf(response, sizeof(response), 
            "Generating code for: %s\n\n"
            "```\n"
            "// Generated code would appear here\n"
            "// In a real app, this calls the AI API\n"
            "```", cmd + 6);
        add_message(m, response, MSG_ASSISTANT);
    } else if (strncmp(cmd, "/search ", 8) == 0) {
        snprintf(response, sizeof(response), 
            "Searching for: %s\n\n"
            "Results would appear here in a real implementation.", cmd + 8);
        add_message(m, response, MSG_ASSISTANT);
    } else if (strcmp(cmd, "/settings") == 0) {
        add_message(m, "Settings panel would open here.\n\n"
                       "- Model: Claude 3.5 Sonnet\n"
                       "- Temperature: 0.7\n"
                       "- Max tokens: 4096\n"
                       "- Stream: enabled", MSG_SYSTEM);
    } else {
        snprintf(response, sizeof(response), "Unknown command: %s\nType /help for available commands.", cmd);
        add_message(m, response, MSG_SYSTEM);
    }
}

static void simulate_response(model_t* m, const char* user_msg) {
    static char response[MAX_MSG_LEN];
    
    if (user_msg[0] == '/') {
        /* Extract command (first word) */
        char cmd[64];
        const char* space = strchr(user_msg, ' ');
        if (space) {
            strncpy(cmd, user_msg, space - user_msg);
            cmd[space - user_msg] = '\0';
        } else {
            strncpy(cmd, user_msg, sizeof(cmd) - 1);
        }
        handle_command(m, user_msg);
        return;
    }
    
    if (strstr(user_msg, "hello") || strstr(user_msg, "Hello")) {
        snprintf(response, sizeof(response), "Hello! How can I help you today?\n\nTip: Type / to see available commands.");
    } else if (strstr(user_msg, "help")) {
        snprintf(response, sizeof(response), 
            "I can help you with:\n"
            "- Code review and suggestions\n"
            "- Debugging assistance\n"
            "- Documentation writing\n"
            "- General questions\n\n"
            "Try slash commands like /code, /search, /help");
    } else {
        snprintf(response, sizeof(response), 
            "I received: \"%s\"\n\n"
            "This is a demo. Try typing / to see slash commands!", user_msg);
    }
    
    add_message(m, response, MSG_ASSISTANT);
}

static void draw_message(tui_buffer_t* buf, int x, int y, int width, const chat_message_t* msg, const tui_theme_t* theme, int max_y) {
    tui_color_t name_color;
    const char* name;
    
    switch (msg->role) {
        case MSG_USER:
            name = "You";
            name_color = TUI_COLOR(100, 200, 255);
            break;
        case MSG_ASSISTANT:
            name = "Assistant";
            name_color = TUI_COLOR(200, 150, 255);
            break;
        default:
            name = "System";
            name_color = TUI_COLOR(255, 200, 100);
            break;
    }
    
    if (y < max_y) tui_buffer_text(buf, x, y, name, name_color, TUI_BLACK);
    
    struct tm* tm = localtime(&msg->timestamp);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm->tm_hour, tm->tm_min);
    if (y < max_y) tui_buffer_text(buf, x + width - 6, y, time_str, TUI_GRAY, TUI_BLACK);
    
    int line = 1;
    const char* p = msg->text;
    int col = 0;
    bool in_code = false;
    
    while (*p && line < 20 && (y + line) < max_y) {
        if (*p == '`' && *(p+1) == '`' && *(p+2) == '`') {
            in_code = !in_code;
            p += 3;
            if (*p == '\n') p++;
            line++;
            col = 0;
            continue;
        }
        
        if (*p == '\n') { line++; col = 0; p++; continue; }
        if (col >= width - 2) { line++; col = 0; }
        
        tui_color_t fg = in_code ? TUI_COLOR(150, 255, 150) : theme->fg;
        tui_color_t cbg = in_code ? TUI_COLOR(40, 40, 40) : TUI_BLACK;
        
        if (*p == '*' && *(p+1) != '*') {
            tui_buffer_set(buf, x + col, y + line, tui_cell('-', theme->primary, cbg));
            col++; p++;
            continue;
        }
        
        tui_buffer_set(buf, x + col, y + line, tui_cell((uint8_t)*p, fg, cbg));
        col++; p++;
    }
}

static int get_message_height(const chat_message_t* msg, int width) {
    int lines = 2;
    int col = 0;
    const char* p = msg->text;
    
    while (*p && lines < 20) {
        if (*p == '\n') { lines++; col = 0; }
        else if (++col >= width - 2) { lines++; col = 0; }
        p++;
    }
    return (lines < 20 ? lines : 20) + 1;
}

static void draw_autocomplete(tui_buffer_t* buf, int x, int y, const model_t* m, const tui_theme_t* theme) {
    if (!m->autocomplete_visible || m->autocomplete_count == 0) return;
    
    int width = 45;
    int height = m->autocomplete_count + 2;
    int popup_y = y - height;
    
    /* Shadow */
    tui_buffer_fill_color(buf, x + 1, popup_y + 1, width, height, TUI_COLOR(20, 20, 20));
    
    /* Background */
    tui_buffer_fill_color(buf, x, popup_y, width, height, TUI_COLOR(35, 35, 45));
    tui_buffer_box_styled(buf, x, popup_y, width, height, TUI_BORDER_ROUND, theme->border, TUI_COLOR(35, 35, 45));
    
    /* Title */
    tui_buffer_text(buf, x + 2, popup_y, " Commands ", TUI_GRAY, TUI_COLOR(35, 35, 45));
    
    /* Items */
    for (int i = 0; i < m->autocomplete_count; i++) {
        int idx = m->autocomplete_matches[i];
        bool sel = (i == m->autocomplete_selected);
        tui_color_t bg = sel ? theme->primary : TUI_COLOR(35, 35, 45);
        
        tui_buffer_fill_color(buf, x + 1, popup_y + 1 + i, width - 2, 1, bg);
        
        /* Command name */
        tui_buffer_text(buf, x + 2, popup_y + 1 + i, commands[idx].name, TUI_CYAN, bg);
        
        /* Description */
        int name_len = (int)strlen(commands[idx].name);
        tui_text_ellipsis(buf, x + 2 + name_len + 1, popup_y + 1 + i, 
                          width - name_len - 5, commands[idx].description, TUI_GRAY, bg);
    }
}

static void view(const model_t* m, tui_buffer_t* buf) {
    tui_theme_t theme = tui_theme_default();
    tui_buffer_clear(buf);
    
    int sidebar_w = m->sidebar_visible ? 20 : 0;
    int main_x = sidebar_w;
    int main_w = buf->width - sidebar_w;
    int input_h = 5;
    int chat_h = buf->height - input_h - 2;
    
    /* Sidebar */
    if (m->sidebar_visible) {
        tui_buffer_fill_color(buf, 0, 0, sidebar_w, buf->height, TUI_COLOR(25, 25, 30));
        tui_buffer_vline(buf, sidebar_w - 1, 0, buf->height, 0x2502, TUI_COLOR(60, 60, 70), TUI_COLOR(25, 25, 30));
        
        tui_buffer_text(buf, 2, 1, "Chats", TUI_WHITE, TUI_COLOR(25, 25, 30));
        tui_buffer_hline(buf, 1, 2, sidebar_w - 2, 0x2500, TUI_COLOR(60, 60, 70), TUI_COLOR(25, 25, 30));
        
        for (int i = 0; i < chat_count; i++) {
            bool sel = (i == m->selected_chat);
            tui_color_t bg = sel ? theme.primary : TUI_COLOR(25, 25, 30);
            tui_buffer_fill_color(buf, 1, 4 + i, sidebar_w - 2, 1, bg);
            tui_buffer_text(buf, 2, 4 + i, chat_list[i], TUI_WHITE, bg);
        }
        
        tui_buffer_text(buf, 2, buf->height - 2, "+ New Chat", theme.primary, TUI_COLOR(25, 25, 30));
    }
    
    /* Header */
    tui_buffer_fill_color(buf, main_x, 0, main_w, 1, TUI_COLOR(35, 35, 45));
    char header[64];
    snprintf(header, sizeof(header), " %s - AI Assistant", chat_list[m->selected_chat]);
    tui_buffer_text(buf, main_x + 1, 0, header, TUI_WHITE, TUI_COLOR(35, 35, 45));
    tui_buffer_text(buf, main_x + main_w - 12, 0, "[Tab] Menu", TUI_GRAY, TUI_COLOR(35, 35, 45));
    
    /* Chat area */
    int y = 2;
    int start_msg = 0;
    int total_h = 0;
    for (int i = m->msg_count - 1; i >= 0; i--) {
        total_h += get_message_height(&m->messages[i], main_w - 4);
        if (total_h > chat_h) { start_msg = i + 1; break; }
    }
    
    y = 2;
    for (int i = start_msg; i < m->msg_count && y < chat_h; i++) {
        draw_message(buf, main_x + 2, y, main_w - 4, &m->messages[i], &theme, chat_h + 1);
        y += get_message_height(&m->messages[i], main_w - 4);
    }
    
    /* Input area */
    int input_y = buf->height - input_h;
    tui_buffer_hline(buf, main_x, input_y - 1, main_w, 0x2500, TUI_COLOR(60, 60, 70), TUI_BLACK);
    
    tui_buffer_fill_color(buf, main_x + 1, input_y, main_w - 2, input_h - 1, TUI_COLOR(30, 30, 35));
    tui_buffer_box_styled(buf, main_x + 1, input_y, main_w - 2, input_h - 1, 
                          TUI_BORDER_ROUND, m->input_focused ? theme.primary : TUI_GRAY, TUI_COLOR(30, 30, 35));
    
    /* Slash indicator */
    if (m->input.lines[0][0] == '/') {
        tui_buffer_set(buf, main_x + 3, input_y + 1, tui_cell('/', TUI_CYAN, TUI_COLOR(30, 30, 35)));
        tui_buffer_text(buf, main_x + 4, input_y + 1, m->input.lines[0] + 1, TUI_WHITE, TUI_COLOR(30, 30, 35));
    } else {
        for (int i = 0; i < m->input.line_count && i < input_h - 2; i++) {
            tui_buffer_text(buf, main_x + 3, input_y + 1 + i, m->input.lines[i], TUI_WHITE, TUI_COLOR(30, 30, 35));
        }
    }
    
    /* Cursor */
    if (m->input_focused) {
        int cx = main_x + 3 + m->input.cursor_x;
        int cy = input_y + 1 + m->input.cursor_y;
        tui_cell_t* cell = tui_buffer_at(buf, cx, cy);
        if (cell) cell->style |= TUI_STYLE_REVERSE;
    }
    
    /* Autocomplete popup */
    draw_autocomplete(buf, main_x + 3, input_y, m, &theme);
    
    /* Hint */
    const char* hint = m->autocomplete_visible 
        ? "Tab: Select | Enter: Apply | Esc: Close"
        : "Type / for commands | Enter: Send | Ctrl+C: Quit";
    tui_buffer_text(buf, main_x + 3, buf->height - 1, hint, TUI_GRAY, TUI_BLACK);
    
    /* Notification */
    tui_notify_draw(buf, buf->width, &m->notify, &theme);
}

static void run_app(model_t* m) {
    tui_terminal_t* term = tui_terminal_create();
    tui_terminal_init(term);
    tui_terminal_enable_mouse(term);
    
    tui_buffer_t* buf = tui_terminal_buffer(term);
    
    while (m->running) {
        clock_t now = clock();
        float delta = (float)(now - m->last_tick) * 1000.0f / CLOCKS_PER_SEC;
        m->last_tick = now;
        tui_notify_update(&m->notify, delta);
        
        tui_terminal_query_size(term);
        view(m, buf);
        tui_terminal_render(term);
        
        tui_event_t event;
        if (!tui_terminal_poll(term, &event)) continue;
        
        /* Global keys */
        if (event.type == TUI_EVENT_KEY) {
            if (tui_event_is_ctrl(&event, 'c')) { m->running = false; continue; }
            if (event.key == TUI_KEY_TAB && !m->autocomplete_visible) {
                m->sidebar_visible = !m->sidebar_visible;
                tui_terminal_refresh(term);
                continue;
            }
        }
        
        /* Autocomplete handling */
        if (m->autocomplete_visible && event.type == TUI_EVENT_KEY) {
            if (event.key == TUI_KEY_UP) {
                if (m->autocomplete_selected > 0) m->autocomplete_selected--;
                continue;
            }
            if (event.key == TUI_KEY_DOWN) {
                if (m->autocomplete_selected < m->autocomplete_count - 1) m->autocomplete_selected++;
                continue;
            }
            if (event.key == TUI_KEY_TAB || event.key == TUI_KEY_ENTER) {
                apply_autocomplete(m);
                continue;
            }
            if (event.key == TUI_KEY_ESCAPE) {
                m->autocomplete_visible = false;
                continue;
            }
        }
        
        /* Sidebar navigation */
        if (m->sidebar_visible && !m->input_focused) {
            if (event.type == TUI_EVENT_KEY) {
                if (event.key == TUI_KEY_UP && m->selected_chat > 0) { m->selected_chat--; continue; }
                if (event.key == TUI_KEY_DOWN && m->selected_chat < chat_count - 1) { m->selected_chat++; continue; }
            }
            if (event.type == TUI_EVENT_MOUSE_PRESS && event.key == TUI_KEY_MOUSE_LEFT) {
                if (event.x < 20 && event.y >= 4 && event.y < 4 + chat_count) {
                    m->selected_chat = event.y - 4;
                    continue;
                }
            }
        }
        
        /* Input handling */
        if (m->input_focused) {
            if (event.type == TUI_EVENT_KEY && event.key == TUI_KEY_ENTER && 
                !(event.mod & TUI_MOD_SHIFT) && !m->autocomplete_visible) {
                char* text = tui_textarea_get_text(&m->input);
                if (text && strlen(text) > 0) {
                    add_message(m, text, MSG_USER);
                    simulate_response(m, text);
                    tui_textarea_init(&m->input);
                    m->input.focused = true;
                    m->autocomplete_visible = false;
                    tui_terminal_refresh(term);
                }
                free(text);
                continue;
            }
            
            if (event.key == TUI_KEY_ESCAPE && !m->autocomplete_visible) {
                m->input_focused = false;
                m->input.focused = false;
                continue;
            }
            
            tui_textarea_handle(&m->input, &event);
            update_autocomplete(m);
        } else {
            if (event.type == TUI_EVENT_MOUSE_PRESS && event.key == TUI_KEY_MOUSE_LEFT) {
                int sidebar_w = m->sidebar_visible ? 20 : 0;
                int input_y = buf->height - 5;
                if (event.x > sidebar_w && event.y >= input_y) {
                    m->input_focused = true;
                    m->input.focused = true;
                }
            }
            if (event.type == TUI_EVENT_KEY) {
                m->input_focused = true;
                m->input.focused = true;
                tui_textarea_handle(&m->input, &event);
                update_autocomplete(m);
            }
        }
    }
    
    tui_terminal_disable_mouse(term);
    tui_terminal_cleanup(term);
    tui_terminal_destroy(term);
}

int main(void) {
    model_t m = {0};
    m.running = true;
    m.sidebar_visible = true;
    m.input_focused = true;
    m.last_tick = clock();
    
    tui_textarea_init(&m.input);
    m.input.focused = true;
    
    add_message(&m, "Welcome to TUI Chat!\n\n"
                    "Features:\n"
                    "- Type / to see slash commands with autocomplete\n"
                    "- Use Tab/Arrow keys to navigate suggestions\n"
                    "- Press Enter to apply selected command\n\n"
                    "Try: /help, /code, /search, /settings",
                MSG_ASSISTANT);
    
    run_app(&m);
    
    return 0;
}
