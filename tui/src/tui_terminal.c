/*
 * TUI Terminal Implementation
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>
#endif

/* ============================================================================
 * Platform-specific state
 * ============================================================================ */

#ifndef _WIN32
static volatile sig_atomic_t g_resize_pending = 0;
static void handle_sigwinch(int sig) {
    (void)sig;
    g_resize_pending = 1;
}
#endif

struct tui_terminal {
    bool            initialized;
    int             width;
    int             height;
    tui_color_mode_t color_mode;
    
    tui_buffer_t*   front;      /* What's on screen */
    tui_buffer_t*   back;       /* What we're drawing */
    
    char*           output;     /* Escape sequence buffer */
    size_t          output_cap;
    size_t          output_len;
    
    bool            mouse_enabled;
    
#ifdef _WIN32
    HANDLE          h_out;
    HANDLE          h_in;
    DWORD           orig_out_mode;
    DWORD           orig_in_mode;
#else
    struct termios  orig_termios;
#endif
};

/* ============================================================================
 * Output buffer helpers
 * ============================================================================ */

static void output_clear(tui_terminal_t* term) {
    term->output_len = 0;
}

static void output_ensure(tui_terminal_t* term, size_t need) {
    size_t required = term->output_len + need;
    if (required > term->output_cap) {
        size_t new_cap = term->output_cap * 2;
        if (new_cap < required) new_cap = required;
        char* new_buf = (char*)realloc(term->output, new_cap);
        if (new_buf) {
            term->output = new_buf;
            term->output_cap = new_cap;
        }
    }
}

static void output_append(tui_terminal_t* term, const char* str) {
    size_t len = strlen(str);
    output_ensure(term, len);
    memcpy(term->output + term->output_len, str, len);
    term->output_len += len;
}

static void output_append_char(tui_terminal_t* term, char c) {
    output_ensure(term, 1);
    term->output[term->output_len++] = c;
}

/* UTF-8 encode */
static void output_append_utf8(tui_terminal_t* term, uint32_t cp) {
    output_ensure(term, 4);
    char* p = term->output + term->output_len;
    
    if (cp < 0x80) {
        *p++ = (char)cp;
        term->output_len += 1;
    } else if (cp < 0x800) {
        *p++ = (char)(0xC0 | (cp >> 6));
        *p++ = (char)(0x80 | (cp & 0x3F));
        term->output_len += 2;
    } else if (cp < 0x10000) {
        *p++ = (char)(0xE0 | (cp >> 12));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
        term->output_len += 3;
    } else {
        *p++ = (char)(0xF0 | (cp >> 18));
        *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
        term->output_len += 4;
    }
}

/* ============================================================================
 * Raw I/O
 * ============================================================================ */

static void write_raw(tui_terminal_t* term, const char* data, size_t len) {
#ifdef _WIN32
    DWORD written;
    WriteConsoleA(term->h_out, data, (DWORD)len, &written, NULL);
#else
    (void)write(STDOUT_FILENO, data, len);
#endif
}

static void write_raw_str(tui_terminal_t* term, const char* str) {
    write_raw(term, str, strlen(str));
}

static void flush_output(tui_terminal_t* term) {
    if (term->output_len > 0) {
        write_raw(term, term->output, term->output_len);
        term->output_len = 0;
    }
    fflush(stdout);
}

/* ============================================================================
 * Color quantization (for 256-color mode)
 * ============================================================================ */

static int quantize_256(tui_color_t c) {
    int q_r = c.r, q_g = c.g, q_b = c.b;
    
    /* Snap to 6x6x6 cube */
    int snap_r = (q_r < 48) ? 0 : (q_r < 115) ? 95 : (q_r < 155) ? 135 : (q_r < 195) ? 175 : (q_r < 235) ? 215 : 255;
    int snap_g = (q_g < 48) ? 0 : (q_g < 115) ? 95 : (q_g < 155) ? 135 : (q_g < 195) ? 175 : (q_g < 235) ? 215 : 255;
    int snap_b = (q_b < 48) ? 0 : (q_b < 115) ? 95 : (q_b < 155) ? 135 : (q_b < 195) ? 175 : (q_b < 235) ? 215 : 255;
    
    int map_val_r = (snap_r == 0) ? 0 : (snap_r - 55) / 40;
    int map_val_g = (snap_g == 0) ? 0 : (snap_g - 55) / 40;
    int map_val_b = (snap_b == 0) ? 0 : (snap_b - 55) / 40;
    
    return 16 + 36 * map_val_r + 6 * map_val_g + map_val_b;
}

/* ============================================================================
 * Terminal Lifecycle
 * ============================================================================ */

tui_terminal_t* tui_terminal_create(void) {
    tui_terminal_t* term = (tui_terminal_t*)calloc(1, sizeof(tui_terminal_t));
    if (!term) return NULL;
    
    term->width = 80;
    term->height = 24;
    term->output_cap = 8192;
    term->output = (char*)malloc(term->output_cap);
    
    if (!term->output) {
        free(term);
        return NULL;
    }
    
    return term;
}

void tui_terminal_destroy(tui_terminal_t* term) {
    if (!term) return;
    tui_terminal_cleanup(term);
    tui_buffer_destroy(term->front);
    tui_buffer_destroy(term->back);
    free(term->output);
    free(term);
}

bool tui_terminal_init(tui_terminal_t* term) {
    if (!term || term->initialized) return term && term->initialized;
    
    /* Auto-detect color mode if not set */
    if (term->color_mode == TUI_COLOR_AUTO) {
        const char* ct = getenv("COLORTERM");
        if (ct && (strcmp(ct, "truecolor") == 0 || strcmp(ct, "24bit") == 0)) {
            term->color_mode = TUI_COLOR_TRUECOLOR;
        } else {
            term->color_mode = TUI_COLOR_256;
        }
    }
    
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    term->h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    term->h_in = GetStdHandle(STD_INPUT_HANDLE);
    
    GetConsoleMode(term->h_out, &term->orig_out_mode);
    GetConsoleMode(term->h_in, &term->orig_in_mode);
    
    DWORD out_mode = term->orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    DWORD in_mode = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    
    SetConsoleMode(term->h_out, out_mode);
    SetConsoleMode(term->h_in, in_mode);
#else
    tcgetattr(STDIN_FILENO, &term->orig_termios);
    struct termios raw = term->orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    
    /* SIGWINCH handler */
    struct sigaction sa;
    sa.sa_handler = handle_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);
#endif
    
    tui_terminal_query_size(term);
    
    term->front = tui_buffer_create(term->width, term->height);
    term->back = tui_buffer_create(term->width, term->height);
    
    if (!term->front || !term->back) {
        tui_terminal_cleanup(term);
        return false;
    }
    
    /* Mark front buffer as "invalid" to force first frame full render */
    {
        tui_cell_t invalid = {0xFFFFFFFF, {255,255,255}, {255,255,255}, 0xFF};
        size_t count = (size_t)term->front->width * (size_t)term->front->height;
        for (size_t i = 0; i < count; i++) {
            term->front->cells[i] = invalid;
        }
    }
    
    /* Enter alternate screen, hide cursor */
    write_raw_str(term, "\033[?1049h");  /* Alternate screen */
    write_raw_str(term, "\033[?25l");    /* Hide cursor */
    write_raw_str(term, "\033[2J");      /* Clear screen */
    fflush(stdout);
    
    term->initialized = true;
    return true;
}

void tui_terminal_cleanup(tui_terminal_t* term) {
    if (!term || !term->initialized) return;
    
    tui_terminal_disable_mouse(term);
    
    write_raw_str(term, "\033[?25h");    /* Show cursor */
    write_raw_str(term, "\033[?1049l");  /* Exit alternate screen */
    write_raw_str(term, "\033[0m");      /* Reset attributes */
    fflush(stdout);
    
#ifdef _WIN32
    SetConsoleMode(term->h_out, term->orig_out_mode);
    SetConsoleMode(term->h_in, term->orig_in_mode);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->orig_termios);
#endif
    
    term->initialized = false;
}

/* ============================================================================
 * Size
 * ============================================================================ */

int tui_terminal_width(const tui_terminal_t* term) { return term->width; }
int tui_terminal_height(const tui_terminal_t* term) { return term->height; }

void tui_terminal_set_color_mode(tui_terminal_t* term, tui_color_mode_t mode) {
    if (term) term->color_mode = mode;
}

tui_color_mode_t tui_terminal_get_color_mode(const tui_terminal_t* term) {
    return term ? term->color_mode : TUI_COLOR_AUTO;
}

void tui_terminal_query_size(tui_terminal_t* term) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(term->h_out, &csbi)) {
        term->width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        term->height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term->width = ws.ws_col;
        term->height = ws.ws_row;
    }
#endif
    if (term->width <= 0) term->width = 80;
    if (term->height <= 0) term->height = 24;
}

/* ============================================================================
 * Buffer Access
 * ============================================================================ */

tui_buffer_t* tui_terminal_buffer(tui_terminal_t* term) {
    return term->back;
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

void tui_terminal_render(tui_terminal_t* term) {
    if (!term || !term->initialized) return;
    
    tui_buffer_t* back = term->back;
    tui_buffer_t* front = term->front;
    
    /* Resize if needed */
    if (back->width != term->width || back->height != term->height) {
        tui_buffer_resize(back, term->width, term->height);
        tui_buffer_resize(front, term->width, term->height);
        /* Force full redraw */
        tui_buffer_clear(front);
        tui_buffer_mark_all_dirty(back);
    }
    
    if (!tui_buffer_is_dirty(back)) return;
    
    output_clear(term);
    output_append(term, "\033[0m");
    
    tui_color_t last_fg = TUI_WHITE;
    tui_color_t last_bg = TUI_BLACK;
    uint8_t last_style = TUI_STYLE_NONE;
    int last_x = -1000, last_y = -1000;
    
    int min_x = back->dirty_min_x;
    int min_y = back->dirty_min_y;
    int max_x = back->dirty_max_x;
    int max_y = back->dirty_max_y;
    
    /* Clamp */
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= term->width) max_x = term->width - 1;
    if (max_y >= term->height) max_y = term->height - 1;
    
    char buf[64];
    
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            const tui_cell_t* cell = tui_buffer_at_const(back, x, y);
            const tui_cell_t* prev = tui_buffer_at_const(front, x, y);
            
            if (tui_cell_eq(cell, prev)) continue;
            
            /* Move cursor if not sequential */
            if (last_x != x - 1 || last_y != y) {
                snprintf(buf, sizeof(buf), "\033[%d;%dH", y + 1, x + 1);
                output_append(term, buf);
            }
            
            /* Update style */
            if (cell->style != last_style) {
                output_append(term, "\033[0m");
                last_fg = TUI_WHITE;
                last_bg = TUI_BLACK;
                
                if (cell->style & TUI_STYLE_BOLD) output_append(term, "\033[1m");
                if (cell->style & TUI_STYLE_DIM) output_append(term, "\033[2m");
                if (cell->style & TUI_STYLE_ITALIC) output_append(term, "\033[3m");
                if (cell->style & TUI_STYLE_UNDERLINE) output_append(term, "\033[4m");
                if (cell->style & TUI_STYLE_BLINK) output_append(term, "\033[5m");
                if (cell->style & TUI_STYLE_REVERSE) output_append(term, "\033[7m");
                if (cell->style & TUI_STYLE_STRIKE) output_append(term, "\033[9m");
                
                last_style = cell->style;
            }
            
            /* Update colors */
            if (!tui_color_eq(cell->fg, last_fg)) {
                if (term->color_mode == TUI_COLOR_TRUECOLOR) {
                    snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm", cell->fg.r, cell->fg.g, cell->fg.b);
                } else {
                    snprintf(buf, sizeof(buf), "\033[38;5;%dm", quantize_256(cell->fg));
                }
                output_append(term, buf);
                last_fg = cell->fg;
            }
            
            if (!tui_color_eq(cell->bg, last_bg)) {
                if (term->color_mode == TUI_COLOR_TRUECOLOR) {
                    snprintf(buf, sizeof(buf), "\033[48;2;%d;%d;%dm", cell->bg.r, cell->bg.g, cell->bg.b);
                } else {
                    snprintf(buf, sizeof(buf), "\033[48;5;%dm", quantize_256(cell->bg));
                }
                output_append(term, buf);
                last_bg = cell->bg;
            }
            
            /* Output character */
            if (cell->ch != 0) {
                output_append_utf8(term, cell->ch);
            }
            
            last_x = x;
            last_y = y;
            
            /* Update front buffer */
            *tui_buffer_at(front, x, y) = *cell;
        }
    }
    
    flush_output(term);
    tui_buffer_reset_dirty(back);
}

void tui_terminal_refresh(tui_terminal_t* term) {
    if (!term || !term->initialized) return;
    /* Mark front buffer as invalid to force full redraw on next render */
    tui_cell_t invalid = {0xFFFFFFFF, {255,255,255}, {255,255,255}, 0xFF};
    size_t count = (size_t)term->front->width * (size_t)term->front->height;
    for (size_t i = 0; i < count; i++) {
        term->front->cells[i] = invalid;
    }
    tui_buffer_mark_all_dirty(term->back);
    /* Don't render here - let the main loop call render after view() updates back buffer */
}

void tui_terminal_clear(tui_terminal_t* term) {
    if (term && term->back) tui_buffer_clear(term->back);
}

void tui_terminal_clear_color(tui_terminal_t* term, tui_color_t bg) {
    if (term && term->back) tui_buffer_clear_color(term->back, bg);
}

/* ============================================================================
 * Cursor
 * ============================================================================ */

void tui_terminal_show_cursor(tui_terminal_t* term) {
    write_raw_str(term, "\033[?25h");
    fflush(stdout);
}

void tui_terminal_hide_cursor(tui_terminal_t* term) {
    write_raw_str(term, "\033[?25l");
    fflush(stdout);
}

void tui_terminal_move_cursor(tui_terminal_t* term, int x, int y) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\033[%d;%dH", y + 1, x + 1);
    write_raw_str(term, buf);
    fflush(stdout);
}
