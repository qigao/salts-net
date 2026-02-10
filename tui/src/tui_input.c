/*
 * TUI Input Implementation
 */

#include "tui.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/select.h>
#include <signal.h>
#endif

/* Forward declaration of terminal struct */
struct tui_terminal {
    bool            initialized;
    int             width;
    int             height;
    int             color_mode;
    
    tui_buffer_t*   front;
    tui_buffer_t*   back;
    
    char*           output;
    size_t          output_cap;
    size_t          output_len;
    
    bool            mouse_enabled;
    
#ifdef _WIN32
    HANDLE          h_out;
    HANDLE          h_in;
    DWORD           orig_out_mode;
    DWORD           orig_in_mode;
#else
    char            orig_termios[60]; /* struct termios */
#endif
};

#ifndef _WIN32
extern volatile sig_atomic_t g_resize_pending;
#endif

/* ============================================================================
 * Mouse Control
 * ============================================================================ */

void tui_terminal_enable_mouse(tui_terminal_t* term) {
    if (!term) return;
    term->mouse_enabled = true;
#ifndef _WIN32
    write(STDOUT_FILENO, "\033[?1006h", 8);  /* SGR extended */
    write(STDOUT_FILENO, "\033[?1003h", 8);  /* All motion */
#endif
}

void tui_terminal_disable_mouse(tui_terminal_t* term) {
    if (!term) return;
    term->mouse_enabled = false;
#ifndef _WIN32
    write(STDOUT_FILENO, "\033[?1003l", 8);
    write(STDOUT_FILENO, "\033[?1006l", 8);
#endif
}

/* ============================================================================
 * Input Polling
 * ============================================================================ */

#ifdef _WIN32

bool tui_terminal_poll(tui_terminal_t* term, tui_event_t* event) {
    if (!term || !event) return false;
    
    memset(event, 0, sizeof(*event));
    
    DWORD num_events = 0;
    GetNumberOfConsoleInputEvents(term->h_in, &num_events);
    if (num_events == 0) return false;
    
    INPUT_RECORD rec;
    DWORD read_count;
    if (!ReadConsoleInputW(term->h_in, &rec, 1, &read_count) || read_count == 0) {
        return false;
    }
    
    if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
        event->type = TUI_EVENT_KEY;
        event->ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        
        DWORD state = rec.Event.KeyEvent.dwControlKeyState;
        if (state & SHIFT_PRESSED) event->mod |= TUI_MOD_SHIFT;
        if (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) event->mod |= TUI_MOD_CTRL;
        if (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) event->mod |= TUI_MOD_ALT;
        
        switch (rec.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP: event->key = TUI_KEY_UP; break;
            case VK_DOWN: event->key = TUI_KEY_DOWN; break;
            case VK_LEFT: event->key = TUI_KEY_LEFT; break;
            case VK_RIGHT: event->key = TUI_KEY_RIGHT; break;
            case VK_HOME: event->key = TUI_KEY_HOME; break;
            case VK_END: event->key = TUI_KEY_END; break;
            case VK_PRIOR: event->key = TUI_KEY_PAGE_UP; break;
            case VK_NEXT: event->key = TUI_KEY_PAGE_DOWN; break;
            case VK_INSERT: event->key = TUI_KEY_INSERT; break;
            case VK_DELETE: event->key = TUI_KEY_DELETE; break;
            case VK_F1: event->key = TUI_KEY_F1; break;
            case VK_F2: event->key = TUI_KEY_F2; break;
            case VK_F3: event->key = TUI_KEY_F3; break;
            case VK_F4: event->key = TUI_KEY_F4; break;
            case VK_F5: event->key = TUI_KEY_F5; break;
            case VK_F6: event->key = TUI_KEY_F6; break;
            case VK_F7: event->key = TUI_KEY_F7; break;
            case VK_F8: event->key = TUI_KEY_F8; break;
            case VK_F9: event->key = TUI_KEY_F9; break;
            case VK_F10: event->key = TUI_KEY_F10; break;
            case VK_F11: event->key = TUI_KEY_F11; break;
            case VK_F12: event->key = TUI_KEY_F12; break;
            case VK_RETURN: event->key = TUI_KEY_ENTER; event->ch = '\n'; break;
            case VK_TAB: event->key = TUI_KEY_TAB; event->ch = '\t'; break;
            case VK_BACK: event->key = TUI_KEY_BACKSPACE; break;
            case VK_ESCAPE: event->key = TUI_KEY_ESCAPE; break;
            default:
                if (event->ch >= 32 && event->ch < 127) {
                    event->key = (tui_key_t)event->ch;
                }
                break;
        }
        return true;
    }
    
    if (rec.EventType == MOUSE_EVENT && term->mouse_enabled) {
        MOUSE_EVENT_RECORD* me = &rec.Event.MouseEvent;
        event->x = me->dwMousePosition.X;
        event->y = me->dwMousePosition.Y;
        
        if (me->dwEventFlags == 0) {
            if (me->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
                event->type = TUI_EVENT_MOUSE_PRESS;
                event->key = TUI_KEY_MOUSE_LEFT;
            } else if (me->dwButtonState & RIGHTMOST_BUTTON_PRESSED) {
                event->type = TUI_EVENT_MOUSE_PRESS;
                event->key = TUI_KEY_MOUSE_RIGHT;
            } else {
                event->type = TUI_EVENT_MOUSE_RELEASE;
                event->key = TUI_KEY_MOUSE_RELEASE;
            }
            return true;
        } else if (me->dwEventFlags == MOUSE_MOVED) {
            event->type = TUI_EVENT_MOUSE_MOVE;
            return true;
        } else if (me->dwEventFlags == MOUSE_WHEELED) {
            event->type = TUI_EVENT_MOUSE_PRESS;
            event->key = ((short)HIWORD(me->dwButtonState) > 0) ? TUI_KEY_MOUSE_WHEEL_UP : TUI_KEY_MOUSE_WHEEL_DOWN;
            return true;
        }
    }
    
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        event->type = TUI_EVENT_RESIZE;
        return true;
    }
    
    return false;
}

#else /* Unix */

/* Declared in tui_terminal.c */
extern volatile sig_atomic_t g_resize_pending;

bool tui_terminal_poll(tui_terminal_t* term, tui_event_t* event) {
    if (!term || !event) return false;
    
    memset(event, 0, sizeof(*event));
    
    /* Check resize signal */
    if (g_resize_pending) {
        g_resize_pending = 0;
        event->type = TUI_EVENT_RESIZE;
        return true;
    }
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
        if (g_resize_pending) {
            g_resize_pending = 0;
            event->type = TUI_EVENT_RESIZE;
            return true;
        }
        return false;
    }
    
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return false;
    
    event->type = TUI_EVENT_KEY;
    
    /* Single byte */
    if (n == 1) {
        event->ch = (uint32_t)(unsigned char)buf[0];
        if (event->ch < 32) {
            if (event->ch == 27) event->key = TUI_KEY_ESCAPE;
            else if (event->ch == 13 || event->ch == 10) { event->key = TUI_KEY_ENTER; event->ch = '\n'; }
            else if (event->ch == 9) { event->key = TUI_KEY_TAB; event->ch = '\t'; }
            else {
                event->mod = TUI_MOD_CTRL;
                event->ch = event->ch + 'a' - 1;
            }
        } else if (event->ch == 127) {
            event->key = TUI_KEY_BACKSPACE;
        } else {
            event->key = (tui_key_t)event->ch;
        }
        return true;
    }
    
    /* CSI sequences */
    if (buf[0] == '\033' && buf[1] == '[') {
        /* Arrow keys */
        if (n == 3) {
            switch (buf[2]) {
                case 'A': event->key = TUI_KEY_UP; return true;
                case 'B': event->key = TUI_KEY_DOWN; return true;
                case 'C': event->key = TUI_KEY_RIGHT; return true;
                case 'D': event->key = TUI_KEY_LEFT; return true;
                case 'H': event->key = TUI_KEY_HOME; return true;
                case 'F': event->key = TUI_KEY_END; return true;
            }
        }
        
        /* SGR mouse: \033[<btn;x;y[Mm] */
        if (buf[2] == '<' && term->mouse_enabled) {
            int btn = 0, x = 0, y = 0;
            char type = 0;
            if (sscanf(buf + 3, "%d;%d;%d%c", &btn, &x, &y, &type) == 4) {
                event->x = x - 1;
                event->y = y - 1;
                event->type = (type == 'M') ? TUI_EVENT_MOUSE_PRESS : TUI_EVENT_MOUSE_RELEASE;
                
                int button = btn & 0x03;
                if (btn & 64) {
                    event->key = (btn & 1) ? TUI_KEY_MOUSE_WHEEL_DOWN : TUI_KEY_MOUSE_WHEEL_UP;
                } else if (btn & 32) {
                    event->type = TUI_EVENT_MOUSE_MOVE;
                } else {
                    switch (button) {
                        case 0: event->key = TUI_KEY_MOUSE_LEFT; break;
                        case 1: event->key = TUI_KEY_MOUSE_MIDDLE; break;
                        case 2: event->key = TUI_KEY_MOUSE_RIGHT; break;
                        default: event->key = TUI_KEY_MOUSE_RELEASE; break;
                    }
                }
                return true;
            }
        }
        
        /* Function keys, etc. */
        int num = 0;
        if (sscanf(buf + 2, "%d~", &num) == 1) {
            switch (num) {
                case 1: event->key = TUI_KEY_HOME; return true;
                case 2: event->key = TUI_KEY_INSERT; return true;
                case 3: event->key = TUI_KEY_DELETE; return true;
                case 4: event->key = TUI_KEY_END; return true;
                case 5: event->key = TUI_KEY_PAGE_UP; return true;
                case 6: event->key = TUI_KEY_PAGE_DOWN; return true;
                case 15: event->key = TUI_KEY_F5; return true;
                case 17: event->key = TUI_KEY_F6; return true;
                case 18: event->key = TUI_KEY_F7; return true;
                case 19: event->key = TUI_KEY_F8; return true;
                case 20: event->key = TUI_KEY_F9; return true;
                case 21: event->key = TUI_KEY_F10; return true;
                case 23: event->key = TUI_KEY_F11; return true;
                case 24: event->key = TUI_KEY_F12; return true;
            }
        }
    }
    
    /* SS3 sequences (F1-F4) */
    if (buf[0] == '\033' && buf[1] == 'O' && n == 3) {
        switch (buf[2]) {
            case 'P': event->key = TUI_KEY_F1; return true;
            case 'Q': event->key = TUI_KEY_F2; return true;
            case 'R': event->key = TUI_KEY_F3; return true;
            case 'S': event->key = TUI_KEY_F4; return true;
        }
    }
    
    return false;
}

#endif

void tui_terminal_wait(tui_terminal_t* term, tui_event_t* event) {
    while (!tui_terminal_poll(term, event)) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
}
