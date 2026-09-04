/*
 * Rigicon Live - terminal I/O.
 * Raw mode (char-by-char), ANSI colors with modulo-cycled palette.
 */

#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  static DWORD  g_in_mode  = 0;
  static DWORD  g_out_mode = 0;
  static HANDLE g_hin  = NULL;
  static HANDLE g_hout = NULL;
  static CRITICAL_SECTION g_lock;
  static int    g_lock_ready = 0;
#else
  #include <termios.h>
  #include <unistd.h>
  #include <pthread.h>
  static struct termios g_saved_termios;
  static int g_termios_saved = 0;
  static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* -------------------------------------------------------------------------- */
/* ANSI palette                                                                */
/* -------------------------------------------------------------------------- */

static const char *PALETTE[] = {
    "\x1b[38;5;39m",   /* dodger blue */
    "\x1b[38;5;208m",  /* orange */
    "\x1b[38;5;46m",   /* bright green */
    "\x1b[38;5;201m",  /* magenta */
    "\x1b[38;5;226m",  /* yellow */
    "\x1b[38;5;51m",   /* cyan */
    "\x1b[38;5;196m",  /* red */
    "\x1b[38;5;118m",  /* lime */
    "\x1b[38;5;219m",  /* pink */
    "\x1b[38;5;123m",  /* aqua */
    "\x1b[38;5;213m",  /* hot pink */
    "\x1b[38;5;154m",  /* chartreuse */
    "\x1b[38;5;166m",  /* dark orange */
    "\x1b[38;5;99m",   /* purple */
    "\x1b[38;5;87m",   /* pale cyan */
    "\x1b[38;5;220m",  /* gold */
};
#define PALETTE_LEN (sizeof(PALETTE) / sizeof(PALETTE[0]))

#define NICK_CACHE_CAP 64
static struct {
    char        nick[64];
    int         idx;
} g_cache[NICK_CACHE_CAP];
static int g_cache_count = 0;

const char *rgcn_color_reset(void)  { return "\x1b[0m"; }
const char *rgcn_color_gray(void)   { return "\x1b[38;5;244m"; }
const char *rgcn_color_system(void) { return "\x1b[38;5;250m"; }

const char *rgcn_color_for(const char *nick) {
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i].nick, nick) == 0) {
            return PALETTE[g_cache[i].idx % PALETTE_LEN];
        }
    }
    int idx = g_cache_count;
    if (g_cache_count < NICK_CACHE_CAP) {
        strncpy(g_cache[g_cache_count].nick, nick, sizeof g_cache[0].nick - 1);
        g_cache[g_cache_count].nick[sizeof g_cache[0].nick - 1] = 0;
        g_cache[g_cache_count].idx = idx;
        g_cache_count++;
    }
    /* Modulo cycle - never overruns the palette. */
    return PALETTE[idx % PALETTE_LEN];
}

/* -------------------------------------------------------------------------- */
/* Lock                                                                        */
/* -------------------------------------------------------------------------- */

void rgcn_term_lock(void) {
#ifdef _WIN32
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    EnterCriticalSection(&g_lock);
#else
    pthread_mutex_lock(&g_lock);
#endif
}

void rgcn_term_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_lock);
#else
    pthread_mutex_unlock(&g_lock);
#endif
}

/* -------------------------------------------------------------------------- */
/* Raw mode                                                                    */
/* -------------------------------------------------------------------------- */

static void enable_bracketed_paste(void)  { printf("\x1b[?2004h"); fflush(stdout); }
static void disable_bracketed_paste(void) { printf("\x1b[?2004l"); fflush(stdout); }

/* Alternate screen buffer - all rendering happens on a fresh screen that
 * disappears entirely on exit. Fits the "zero log / kapatınca her şey gider"
 * design and also gives us a controlled canvas we can redraw without
 * polluting the parent terminal's scrollback. */
static void enter_alt_screen(void)  { printf("\x1b[?1049h"); fflush(stdout); }
static void leave_alt_screen(void)  { printf("\x1b[?1049l"); fflush(stdout); }

void rgcn_term_init(void) {
#ifdef _WIN32
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hin && GetConsoleMode(g_hin, &g_in_mode)) {
        DWORD m = g_in_mode;
        m &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        m |=  ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(g_hin, m);
    }
    if (g_hout && GetConsoleMode(g_hout, &g_out_mode)) {
        DWORD m = g_out_mode;
        m |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(g_hout, m);
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);          /* Turkish input bytes as UTF-8, not CP-1254 */
    enter_alt_screen();
    enable_bracketed_paste();
#else
    if (tcgetattr(0, &g_saved_termios) == 0) {
        g_termios_saved = 1;
        struct termios t = g_saved_termios;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_iflag &= ~(ICRNL);
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
    enter_alt_screen();
    enable_bracketed_paste();
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
}

void rgcn_term_restore(void) {
    disable_bracketed_paste();
    leave_alt_screen();
#ifdef _WIN32
    if (g_hin  && g_in_mode)  SetConsoleMode(g_hin,  g_in_mode);
    if (g_hout && g_out_mode) SetConsoleMode(g_hout, g_out_mode);
#else
    if (g_termios_saved) tcsetattr(0, TCSANOW, &g_saved_termios);
#endif
    printf("%s", rgcn_color_reset());
    fflush(stdout);
}

int rgcn_term_read_char(uint8_t *buf, size_t buf_cap) {
    if (buf_cap == 0) return 0;
#ifdef _WIN32
    /* ReadConsoleW would give us wide chars, but we operate in UTF-8 mode.
     * Use ReadFile so paste/Cyrillic/Turkish input flows through as UTF-8. */
    DWORD got = 0;
    if (!ReadFile(g_hin, buf, 1, &got, NULL) || got == 0) return 0;
    /* If it's a UTF-8 lead byte, pull the continuation. */
    int extra = 0;
    if      ((buf[0] & 0xE0) == 0xC0) extra = 1;
    else if ((buf[0] & 0xF0) == 0xE0) extra = 2;
    else if ((buf[0] & 0xF8) == 0xF0) extra = 3;
    for (int i = 0; i < extra && (size_t)(1 + i) < buf_cap; i++) {
        DWORD g = 0;
        if (!ReadFile(g_hin, buf + 1 + i, 1, &g, NULL) || g == 0) break;
    }
    return 1 + extra;
#else
    ssize_t r = read(0, buf, 1);
    if (r <= 0) return 0;
    int extra = 0;
    if      ((buf[0] & 0xE0) == 0xC0) extra = 1;
    else if ((buf[0] & 0xF0) == 0xE0) extra = 2;
    else if ((buf[0] & 0xF8) == 0xF0) extra = 3;
    for (int i = 0; i < extra && (size_t)(1 + i) < buf_cap; i++) {
        if (read(0, buf + 1 + i, 1) <= 0) break;
    }
    return 1 + extra;
#endif
}

void rgcn_term_clear_screen(void)      { printf("\x1b[2J\x1b[H"); fflush(stdout); }
void rgcn_term_clear_current_line(void){ printf("\r\x1b[K");     fflush(stdout); }
void rgcn_term_show_cursor(int on)     { printf(on ? "\x1b[?25h" : "\x1b[?25l"); fflush(stdout); }
