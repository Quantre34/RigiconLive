#ifndef RGCN_GAME_H
#define RGCN_GAME_H

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* game.c -> main.c                                                           */
/* -------------------------------------------------------------------------- */

/* True while a game loop currently owns the terminal/input (main.c uses this
 * to suppress direct chat/system rendering - log-append only - and to let
 * the active game's own tick loop consume SIGWINCH instead of main's
 * chat redraw). */
int  rgcn_game_is_active(void);

/* Entry point for the "/game ..." command. `args` is the text after
 * "/game" (leading/trailing spaces not guaranteed trimmed by the caller). */
void rgcn_game_cmd(const char *args);

/* Called by main.c's handle_decoded() for each GAME_* wire kind, with the
 * fields already split past "kind|station|id|ts|nick|". Delivers into the
 * single-slot mailbox a waiting game loop drains, or (idle + GAME_OFFER)
 * posts an invite prompt via rgcn_main_system(). Unrelated/stale packets
 * (unknown offer_id, busy, etc.) are ignored silently - an old peer that
 * doesn't understand these kinds never reaches this code at all. */
void rgcn_game_on_offer (const char *from_nick, uint64_t offer_id, const char *game_name);
void rgcn_game_on_accept(const char *from_nick, uint64_t offer_id);
void rgcn_game_on_reject(const char *from_nick, uint64_t offer_id);
void rgcn_game_on_move  (uint64_t offer_id, uint64_t turn_no, const char *board);
void rgcn_game_on_resign(uint64_t offer_id);

/* -------------------------------------------------------------------------- */
/* main.c -> game.c  (thin bridges - implemented in main.c)                   */
/* -------------------------------------------------------------------------- */

/* Post a system line into the chat log (same look as the rest of the UI). */
void        rgcn_main_system(const char *text);

/* Send a "kind|station|id|ts|nick|text" wire message the same way chat/JOIN
 * do - broadcast + unicast to known peers, auto-sealed. */
void        rgcn_main_send(const char *kind, const char *text);

/* Sends `text` as a normal CHAT message AND renders it locally, exactly
 * like typing it at the prompt would - unlike rgcn_main_send, which only
 * puts the packet on the wire (fine for GAME_* kinds nobody else echoes
 * back to us, wrong for a message we want to see in our own chat too). */
void        rgcn_main_chat_send(const char *text);

const char *rgcn_main_nick(void);

/* Full chat-log redraw, for when a game loop hands the terminal back. */
void        rgcn_main_redraw(void);

/* If a resize (or other async trigger) happened since the last call, clears
 * the flag and returns 1 so the caller's game loop can redraw its own
 * board; returns 0 otherwise. */
int         rgcn_main_needs_redraw_consume(void);

/* Current terminal size, clamped to sane bounds. */
void        rgcn_main_term_size(int *out_cols, int *out_rows);

/* True (and clears the flag) if chat/system lines were appended to the log
 * since the last call - the game's chat strip should repaint. Only ever
 * set while a game is active (write_line()/render_burn() check that). */
int         rgcn_main_chat_dirty_consume(void);

/* Paints the last `rows` alive log lines into a fixed screen region, oldest
 * at top_row, newest at the bottom row (top_row + rows - 1). Each line is
 * absolutely positioned and cleared with \x1b[K first; never emits a
 * trailing newline, so a line wider than the terminal can't wrap into the
 * next region (pair with rgcn_term_set_autowrap(0) while a game is active). */
void        rgcn_main_draw_chat_strip(int top_row, int rows);

#endif
