#ifndef RGCN_GAME_INTERNAL_H
#define RGCN_GAME_INTERNAL_H

/* Shared between game.c and the individual game modules (tetris.c, xox.c,
 * pong.c). Not used by main.c - main.c only sees game.h. */

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Per-game entry points - called only from game.c's /game dispatcher.       */
/* -------------------------------------------------------------------------- */

void rgcn_tetris_run(void);
void rgcn_pong_run(void);

/* my_turn = 1: we sent the offer, we are X and move first.
 * my_turn = 0: we accepted, we are O and wait for the opponent's first move. */
void rgcn_xox_run(const char *opponent_nick, uint64_t offer_id, int my_turn);

/* -------------------------------------------------------------------------- */
/* Shared input                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    RGCN_KEY_NONE = 0,   /* timeout - no key pressed, caller's tick fires */
    RGCN_KEY_UP,
    RGCN_KEY_DOWN,
    RGCN_KEY_LEFT,
    RGCN_KEY_RIGHT,
    RGCN_KEY_ENTER,
    RGCN_KEY_SPACE,
    RGCN_KEY_QUIT,       /* 'q'/'Q' or a lone ESC */
    RGCN_KEY_CHAR        /* any other printable ASCII byte - see *out_ch */
} rgcn_key_t;

/* Blocks up to timeout_ms for one logical keypress, recognizing arrow keys
 * (ESC [ A/B/C/D) as a tiny state machine so callers never see raw escape
 * bytes. Measure elapsed wall-clock time around calls that must hit a fixed
 * tick rate - an arrow key can internally consume a bit more than
 * timeout_ms while it waits out the CSI follow-up bytes.
 *
 * 'c'/'C' is intercepted here to toggle the shared chat strip on/off (see
 * rgcn_game_frame_sync) and reported back as RGCN_KEY_NONE - every game
 * loop's "no key" tick path already does the right thing, so no game needs
 * its own case for it, and no game should bind 'c' to anything else. */
rgcn_key_t rgcn_game_read_key(int timeout_ms, char *out_ch);

/* -------------------------------------------------------------------------- */
/* Shared split-screen frame: chat strip (top) + separator + board (rest)    */
/* -------------------------------------------------------------------------- */

typedef struct {
    int board_top;    /* first usable row for the game's own board (1-based) */
    int board_rows;   /* usable rows at/below board_top */
    int cols;         /* terminal width, clamped */
} rgcn_frame_t;

/* Call once, right before a game's loop starts (forces a clean full clear +
 * re-layout on the first rgcn_game_frame_sync()). */
void rgcn_game_frame_reset(void);

/* Call once per loop iteration, before drawing the board. Handles resize
 * and chat-toggle re-layout (full clear + redraw separator) and incoming
 * chat (repaint just the strip), then reports where the board may draw.
 * The board itself should still be fully repainted every call - this only
 * governs the chat strip above it.
 *
 * min_board_rows is the game's own minimum playable height (e.g. Tetris's
 * fixed 20-row field + header + border). The chat strip is only given
 * screen space when there is still room left for at least that many rows
 * below it - a toggled-on strip never shrinks the board below playable;
 * it just stops claiming space it can't afford. */
void rgcn_game_frame_sync(rgcn_frame_t *out, int min_board_rows);

/* -------------------------------------------------------------------------- */
/* Shared networking (used by xox.c; available to any future turn-based      */
/* game that wants the same full-state-resync pattern)                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    RGCN_GEV_NONE = 0,
    RGCN_GEV_ACCEPT,
    RGCN_GEV_REJECT,
    RGCN_GEV_MOVE,
    RGCN_GEV_RESIGN
} rgcn_game_event_t;

typedef struct {
    rgcn_game_event_t type;
    uint64_t          turn_no;    /* valid when type == RGCN_GEV_MOVE */
    char              board[16];  /* valid when type == RGCN_GEV_MOVE */
} rgcn_game_net_msg_t;

/* Non-blocking. Returns 1 and fills *out (clearing the slot) if a network
 * event for `offer_id` arrived since the last poll, else returns 0. */
int  rgcn_game_poll(uint64_t offer_id, rgcn_game_net_msg_t *out);

void rgcn_game_send_offer (const char *target_nick, uint64_t offer_id, const char *game_name);
void rgcn_game_send_accept(uint64_t offer_id);
void rgcn_game_send_reject(uint64_t offer_id);
void rgcn_game_send_move  (uint64_t offer_id, uint64_t turn_no, const char *board9);
void rgcn_game_send_resign(uint64_t offer_id);

#endif
