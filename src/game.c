/*
 * Rigicon Live - game mode dispatcher.
 *
 * Owns the "is a game currently in control of the terminal" flag and the
 * tiny invite/mailbox state machine that lets XOX ride the existing
 * chat wire (GAME_OFFER/ACCEPT/REJECT/MOVE/RESIGN, same
 * "kind|station|id|ts|nick|..." framing as CHAT/JOIN/FILE_*).
 *
 * Every /game sub-command, and every GAME_* packet, funnels through here.
 * The individual games (tetris.c, xox.c, pong.c) never touch main.c or the
 * network directly - they go through game_internal.h.
 */

#include "game.h"
#include "game_internal.h"
#include "rgcn.h"
#include "crypto.h"
#include "term.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
  static CRITICAL_SECTION g_lock;
  static int g_lock_ready = 0;
  #define GLOCK()   do { if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; } EnterCriticalSection(&g_lock); } while (0)
  #define GUNLOCK() LeaveCriticalSection(&g_lock)
#else
  #include <pthread.h>
  static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
  #define GLOCK()   pthread_mutex_lock(&g_lock)
  #define GUNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* True while any /game ... run is in control of the terminal: a local game
 * (Tetris/Pong), an XOX session, or a challenger's "waiting for accept"
 * sub-loop. main.c checks this to suppress direct chat rendering. */
static volatile int g_active = 0;

/* The offer_id we currently care about (valid only while g_active). Network
 * events for any other offer_id are stale/unrelated and dropped. */
static uint64_t g_current_offer_id = 0;

/* One pending inbound invite, while idle. A second offer overwrites it -
 * kept deliberately simple, same spirit as the single-slot file offer UX. */
static struct {
    int      valid;
    uint64_t offer_id;
    char     from_nick[RGCN_MAX_NICK];
    char     game_name[16];
} g_invite;

/* One pending network event for g_current_offer_id, drained by the active
 * game's loop. Lock-protected because it is written from the receiver
 * thread and read from the main thread's game loop. */
static rgcn_game_net_msg_t g_inbox;
static int                 g_inbox_pending = 0;

/* -------------------------------------------------------------------------- */
/* Mailbox plumbing                                                            */
/* -------------------------------------------------------------------------- */

static void deliver(rgcn_game_event_t type, uint64_t offer_id,
                     uint64_t turn_no, const char *board) {
    GLOCK();
    if (g_active && offer_id == g_current_offer_id) {
        g_inbox.type    = type;
        g_inbox.turn_no = turn_no;
        if (board) {
            strncpy(g_inbox.board, board, sizeof g_inbox.board - 1);
            g_inbox.board[sizeof g_inbox.board - 1] = 0;
        } else {
            g_inbox.board[0] = 0;
        }
        g_inbox_pending = 1;
    }
    GUNLOCK();
}

int rgcn_game_poll(uint64_t offer_id, rgcn_game_net_msg_t *out) {
    int got = 0;
    GLOCK();
    if (g_inbox_pending && g_active && offer_id == g_current_offer_id) {
        *out = g_inbox;
        g_inbox_pending = 0;
        got = 1;
    }
    GUNLOCK();
    return got;
}

/* -------------------------------------------------------------------------- */
/* Wire-in (called from main.c's handle_decoded, on the receiver thread)      */
/* -------------------------------------------------------------------------- */

void rgcn_game_on_offer(const char *from_nick, uint64_t offer_id, const char *game_name) {
    GLOCK();
    if (g_active) { GUNLOCK(); return; }   /* busy - silence, peer will time out */
    g_invite.valid     = 1;
    g_invite.offer_id  = offer_id;
    strncpy(g_invite.from_nick, from_nick, sizeof g_invite.from_nick - 1);
    g_invite.from_nick[sizeof g_invite.from_nick - 1] = 0;
    strncpy(g_invite.game_name, game_name, sizeof g_invite.game_name - 1);
    g_invite.game_name[sizeof g_invite.game_name - 1] = 0;
    GUNLOCK();

    char line[160];
    snprintf(line, sizeof line,
             "%s seninle %s oynamak istiyor. Kabul: /game accept   Red: /game reject",
             from_nick, game_name);
    rgcn_main_system(line);
}

void rgcn_game_on_accept(const char *from_nick, uint64_t offer_id) {
    (void)from_nick;
    deliver(RGCN_GEV_ACCEPT, offer_id, 0, NULL);
}

void rgcn_game_on_reject(const char *from_nick, uint64_t offer_id) {
    (void)from_nick;
    deliver(RGCN_GEV_REJECT, offer_id, 0, NULL);
}

void rgcn_game_on_move(uint64_t offer_id, uint64_t turn_no, const char *board) {
    deliver(RGCN_GEV_MOVE, offer_id, turn_no, board);
}

void rgcn_game_on_resign(uint64_t offer_id) {
    deliver(RGCN_GEV_RESIGN, offer_id, 0, NULL);
}

int rgcn_game_is_active(void) { return g_active; }

/* -------------------------------------------------------------------------- */
/* Split-screen frame: chat strip (default on) + separator + board           */
/* -------------------------------------------------------------------------- */

static volatile int g_chat_visible = 1;
static volatile int g_layout_dirty = 1;   /* forces a full clear + separator redraw */

static void toggle_chat_visible(void) {
    g_chat_visible = !g_chat_visible;
    g_layout_dirty = 1;
}

/* Rows given to the chat strip for a given terminal height. 0 = hidden
 * (toggled off, or the terminal is too short to spare any rows for it). */
static int chat_strip_rows(int term_rows) {
    if (!g_chat_visible)      return 0;
    if (term_rows >= 24)      return 6;
    if (term_rows >= 16)      return 3;
    return 0;
}

void rgcn_game_frame_reset(void) {
    g_layout_dirty = 1;
}

void rgcn_game_frame_sync(rgcn_frame_t *out, int min_board_rows) {
    int cols = 80, rows = 24;
    rgcn_main_term_size(&cols, &rows);

    if (rgcn_main_needs_redraw_consume()) g_layout_dirty = 1;

    int chat_rows = chat_strip_rows(rows);
    /* Only spend rows on the strip if the board still gets its minimum
     * after paying for it - the board always wins the space fight. */
    if (chat_rows > 0 && rows - chat_rows - 1 < min_board_rows) chat_rows = 0;
    int board_top = chat_rows > 0 ? chat_rows + 2 : 1;   /* +1 separator, +1 past it */

    if (g_layout_dirty) {
        printf("\x1b[2J");
        if (chat_rows > 0) {
            printf("\x1b[%d;1H", chat_rows + 1);
            int dashes = cols;
            for (int c = 0; c < dashes; c++) putchar('-');
        }
        fflush(stdout);
        g_layout_dirty = 0;
        if (chat_rows > 0) rgcn_main_draw_chat_strip(1, chat_rows);
        rgcn_main_chat_dirty_consume();   /* already fresh, don't redraw it again below */
    } else if (chat_rows > 0 && rgcn_main_chat_dirty_consume()) {
        rgcn_main_draw_chat_strip(1, chat_rows);
    }

    out->board_top  = board_top;
    out->board_rows = rows - board_top + 1 > 1 ? rows - board_top + 1 : 1;
    out->cols       = cols;
}

/* -------------------------------------------------------------------------- */
/* Wire-out                                                                    */
/* -------------------------------------------------------------------------- */

void rgcn_game_send_offer(const char *target_nick, uint64_t offer_id, const char *game_name) {
    char text[96];
    snprintf(text, sizeof text, "%016llX|%s|%s",
             (unsigned long long)offer_id, game_name, target_nick ? target_nick : "");
    rgcn_main_send("GAME_OFFER", text);
}

void rgcn_game_send_accept(uint64_t offer_id) {
    char text[32];
    snprintf(text, sizeof text, "%016llX", (unsigned long long)offer_id);
    rgcn_main_send("GAME_ACCEPT", text);
}

void rgcn_game_send_reject(uint64_t offer_id) {
    char text[32];
    snprintf(text, sizeof text, "%016llX", (unsigned long long)offer_id);
    rgcn_main_send("GAME_REJECT", text);
}

void rgcn_game_send_move(uint64_t offer_id, uint64_t turn_no, const char *board9) {
    char text[64];
    snprintf(text, sizeof text, "%016llX|%llu|%s",
             (unsigned long long)offer_id, (unsigned long long)turn_no, board9);
    rgcn_main_send("GAME_MOVE", text);
}

void rgcn_game_send_resign(uint64_t offer_id) {
    char text[32];
    snprintf(text, sizeof text, "%016llX", (unsigned long long)offer_id);
    rgcn_main_send("GAME_RESIGN", text);
}

/* -------------------------------------------------------------------------- */
/* Shared key reader                                                           */
/* -------------------------------------------------------------------------- */

rgcn_key_t rgcn_game_read_key(int timeout_ms, char *out_ch) {
    uint8_t buf[4];
    int n = rgcn_term_read_char_timeout(buf, sizeof buf, timeout_ms);
    if (n <= 0) return RGCN_KEY_NONE;

    if (n == 1 && buf[0] == 0x1B) {
        /* Might be an arrow key (ESC [ A/B/C/D). Give it a short window to
         * complete; a lone ESC with nothing following means quit. */
        uint8_t b2[4];
        int n2 = rgcn_term_read_char_timeout(b2, sizeof b2, 50);
        if (n2 == 1 && b2[0] == '[') {
            uint8_t b3[4];
            int n3 = rgcn_term_read_char_timeout(b3, sizeof b3, 50);
            if (n3 == 1) {
                switch (b3[0]) {
                    case 'A': return RGCN_KEY_UP;
                    case 'B': return RGCN_KEY_DOWN;
                    case 'C': return RGCN_KEY_RIGHT;
                    case 'D': return RGCN_KEY_LEFT;
                    default:  return RGCN_KEY_NONE;   /* unrecognized CSI - swallow */
                }
            }
        }
        return RGCN_KEY_QUIT;
    }

    if (n == 1) {
        uint8_t c = buf[0];
        if (c == 'q' || c == 'Q')        return RGCN_KEY_QUIT;
        if (c == '\r' || c == '\n')      return RGCN_KEY_ENTER;
        if (c == ' ')                    return RGCN_KEY_SPACE;
        if (c == 'c' || c == 'C')        { toggle_chat_visible(); return RGCN_KEY_NONE; }
        if (out_ch) *out_ch = (char)c;
        return RGCN_KEY_CHAR;
    }
    return RGCN_KEY_NONE;   /* multi-byte UTF-8 - no game uses these as controls */
}

/* -------------------------------------------------------------------------- */
/* /game dispatcher                                                            */
/* -------------------------------------------------------------------------- */

static void run_local_game(void (*fn)(void)) {
    if (g_active) { rgcn_main_system("Zaten bir oyundasın."); return; }
    g_active = 1;
    rgcn_term_set_autowrap(0);
    rgcn_game_frame_reset();
    fn();
    rgcn_term_set_autowrap(1);
    g_active = 0;
    rgcn_main_redraw();
}

static void start_xox_challenge(const char *rest) {
    if (g_active) { rgcn_main_system("Zaten bir oyundasın/beklemedesin."); return; }

    char nick[RGCN_MAX_NICK] = {0};
    size_t i = 0;
    while (rest[i] && rest[i] != ' ' && rest[i] != '\t' && i < sizeof nick - 1) {
        nick[i] = rest[i]; i++;
    }
    nick[i] = 0;
    if (!nick[0]) { rgcn_main_system("Kullanım: /game xox <rumuz>"); return; }
    if (strcmp(nick, rgcn_main_nick()) == 0) { rgcn_main_system("Kendinle oynayamazsın."); return; }

    uint64_t offer_id = rgcn_random_u64();
    GLOCK();
    g_current_offer_id = offer_id;
    g_inbox_pending     = 0;
    g_active            = 1;
    GUNLOCK();
    rgcn_term_set_autowrap(0);
    rgcn_game_frame_reset();

    rgcn_game_send_offer(nick, offer_id, "xox");
    char line[96];
    snprintf(line, sizeof line,
             "%s'e XOX daveti gönderildi, yanıt bekleniyor... (q: vazgeç)", nick);
    rgcn_main_system(line);

    uint64_t deadline = rgcn_now_ms() + 60000;
    int accepted = 0, rejected = 0, cancelled = 0;
    while (rgcn_now_ms() < deadline) {
        rgcn_frame_t f;
        rgcn_game_frame_sync(&f, 3);
        printf("\x1b[%d;1H\x1b[K%s (c: sohbeti gizle/göster)", f.board_top, line);
        fflush(stdout);

        char ch = 0;
        if (rgcn_game_read_key(200, &ch) == RGCN_KEY_QUIT) { cancelled = 1; break; }
        rgcn_game_net_msg_t ev;
        if (rgcn_game_poll(offer_id, &ev)) {
            if (ev.type == RGCN_GEV_ACCEPT) { accepted = 1; break; }
            if (ev.type == RGCN_GEV_REJECT) { rejected = 1; break; }
        }
    }

    if (accepted) {
        rgcn_xox_run(nick, offer_id, 1);
    } else if (rejected) {
        rgcn_main_system("Davet reddedildi.");
    } else if (cancelled) {
        rgcn_main_system("Davet geri çekildi.");
    } else {
        rgcn_main_system("Davet yanıtsız kaldı (zaman aşımı).");
    }
    rgcn_term_set_autowrap(1);
    g_active = 0;
    rgcn_main_redraw();
}

static void accept_pending_invite(void) {
    GLOCK();
    if (!g_invite.valid) {
        GUNLOCK();
        rgcn_main_system("Bekleyen bir oyun daveti yok.");
        return;
    }
    if (g_active) {
        GUNLOCK();
        rgcn_main_system("Zaten bir oyundasın.");
        return;
    }
    uint64_t offer_id = g_invite.offer_id;
    char from_nick[RGCN_MAX_NICK];
    char game_name[16];
    strncpy(from_nick, g_invite.from_nick, sizeof from_nick - 1); from_nick[sizeof from_nick - 1] = 0;
    strncpy(game_name, g_invite.game_name, sizeof game_name - 1); game_name[sizeof game_name - 1] = 0;
    g_invite.valid      = 0;
    g_current_offer_id  = offer_id;
    g_inbox_pending      = 0;
    g_active            = 1;
    GUNLOCK();
    rgcn_term_set_autowrap(0);
    rgcn_game_frame_reset();

    rgcn_game_send_accept(offer_id);

    if (strcmp(game_name, "xox") == 0) {
        rgcn_xox_run(from_nick, offer_id, 0);
    } else {
        rgcn_main_system("Bilinmeyen oyun türü.");
    }
    rgcn_term_set_autowrap(1);
    g_active = 0;
    rgcn_main_redraw();
}

static void reject_pending_invite(void) {
    GLOCK();
    if (!g_invite.valid) {
        GUNLOCK();
        rgcn_main_system("Bekleyen bir oyun daveti yok.");
        return;
    }
    uint64_t offer_id = g_invite.offer_id;
    g_invite.valid = 0;
    GUNLOCK();

    rgcn_game_send_reject(offer_id);
    rgcn_main_system("Reddedildi.");
}

void rgcn_game_cmd(const char *args) {
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    char cmd1[16] = {0};
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && i < sizeof cmd1 - 1) { cmd1[i] = p[i]; i++; }
    cmd1[i] = 0;
    const char *rest = p + i;
    while (*rest == ' ' || *rest == '\t') rest++;

    if (!cmd1[0]) {
        rgcn_main_system("Kullanım: /game tetris | /game pong | /game xox <rumuz> | /game accept | /game reject");
    } else if (strcmp(cmd1, "tetris") == 0) {
        run_local_game(rgcn_tetris_run);
    } else if (strcmp(cmd1, "pong") == 0) {
        run_local_game(rgcn_pong_run);
    } else if (strcmp(cmd1, "xox") == 0) {
        start_xox_challenge(rest);
    } else if (strcmp(cmd1, "accept") == 0) {
        accept_pending_invite();
    } else if (strcmp(cmd1, "reject") == 0) {
        reject_pending_invite();
    } else {
        rgcn_main_system("Bilinmeyen oyun. Kullanım: /game tetris | /game pong | /game xox <rumuz> | /game accept | /game reject");
    }
}
