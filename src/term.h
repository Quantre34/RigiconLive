#ifndef RGCN_TERM_H
#define RGCN_TERM_H

#include <stddef.h>
#include <stdint.h>

void rgcn_term_init(void);
void rgcn_term_restore(void);

/* Reads one code point unit (bytes) from stdin into buf. Returns bytes read
 * (1..4) or 0 on EOF/interrupt. Blocking. */
int  rgcn_term_read_char(uint8_t *buf, size_t buf_cap);

/* Print helpers - thread-safe wrt each other. */
void rgcn_term_lock(void);
void rgcn_term_unlock(void);

/* ANSI color for a given nickname (stable per-nick, modulo palette). */
const char *rgcn_color_for(const char *nick);
const char *rgcn_color_reset(void);
const char *rgcn_color_gray(void);
const char *rgcn_color_system(void);

/* Override a nick's palette slot. idx = -1 clears the override and reverts
 * to auto assignment. Returns the total palette size for range checking. */
int         rgcn_color_palette_size(void);
void        rgcn_color_override(const char *nick, int idx);
int         rgcn_color_current_index(const char *nick);
const char *rgcn_color_by_index(int idx);
const char *rgcn_color_name(int idx);

void rgcn_term_clear_screen(void);
void rgcn_term_clear_current_line(void);
void rgcn_term_show_cursor(int on);

#endif
