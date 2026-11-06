/*
 * Rigicon Live - simple multiple-choice polls.
 *
 * Same best-effort UDP transport as chat/games, so the same resend
 * discipline applies: the creator rebroadcasts POLL_OPEN every ~5s while
 * the poll is live (also lets a peer who joins mid-poll discover it), and
 * each voter resends its own last POLL_VOTE every ~2s. The creator's
 * timer broadcasts the authoritative final tally in POLL_CLOSE so every
 * peer agrees on the same winner regardless of clock skew or which votes
 * it personally received; everyone else's own timer is only a fallback
 * (duration + 3s) in case the creator vanished.
 *
 * One active poll per channel. If two peers open one at the same moment,
 * every peer keeps whichever poll_id is numerically lower - deterministic
 * regardless of packet arrival order, so nobody's screen disagrees.
 */

#include "poll.h"
#include "rgcn.h"
#include "crypto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Implemented in main.c (see src/game.h for the authoritative doc
 * comments - these three are generic UI bridges, not game-specific). */
extern void        rgcn_main_system(const char *text);
extern void        rgcn_main_send(const char *kind, const char *text);
extern const char *rgcn_main_nick(void);

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

#define POLL_MAX_OPTIONS       9
#define POLL_MAX_VOTERS        64
#define POLL_QUESTION_CAP      100
#define POLL_OPTION_CAP        24
#define POLL_DURATION_S        60
#define POLL_OPEN_RESEND_MS    5000
#define POLL_VOTE_RESEND_MS    2000
#define POLL_CLOSE_FALLBACK_MS 3000

typedef struct {
    char nick[RGCN_MAX_NICK];
    int  choice;
} poll_voter_t;

static struct {
    int           active;
    uint64_t      poll_id;
    char          creator[RGCN_MAX_NICK];
    char          question[POLL_QUESTION_CAP + 1];
    char          options[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    int           n_options;
    poll_voter_t  voters[POLL_MAX_VOTERS];
    int           voter_count;
    uint64_t      closes_at_ms;
    uint64_t      last_open_sent_ms;   /* creator-only bookkeeping */
} g_poll;

static uint64_t g_my_last_vote_sent_ms = 0;

/* -------------------------------------------------------------------------- */
/* Small local helpers (deliberately duplicated from main.c's statics rather */
/* than exposing them - keeps this module self-contained)                    */
/* -------------------------------------------------------------------------- */

static void sanitize(char *s) {
    for (char *c = s; *c; c++) {
        unsigned char b = (unsigned char)*c;
        if (b == '|' || b < 0x20) *c = ' ';
    }
}

static const char *parse_token(const char *in, char *out, size_t out_cap) {
    while (*in == ' ' || *in == '\t') in++;
    if (!*in) { if (out_cap) out[0] = 0; return NULL; }
    size_t o = 0;
    if (*in == '"') {
        in++;
        while (*in && *in != '"' && o + 1 < out_cap) {
            if (*in == '\\' && in[1]) in++;
            out[o++] = *in++;
        }
        if (*in == '"') in++;
    } else {
        while (*in && *in != ' ' && *in != '\t' && o + 1 < out_cap) out[o++] = *in++;
    }
    out[o] = 0;
    return in;
}

/* Caller must hold GLOCK. */
static void tally_locked(int *out_counts, int n) {
    for (int i = 0; i < n; i++) out_counts[i] = 0;
    for (int i = 0; i < g_poll.voter_count; i++) {
        int c = g_poll.voters[i].choice;
        if (c >= 0 && c < n) out_counts[c]++;
    }
}

/* -------------------------------------------------------------------------- */
/* State mutation + announcement (shared by the local command path and the   */
/* network-receive path - mutate under the lock, announce after releasing   */
/* it, same discipline as src/game.c)                                        */
/* -------------------------------------------------------------------------- */

static void apply_poll_open(const char *creator_nick, uint64_t poll_id, int duration_s,
                             const char *question, const char * const *opts, int n) {
    if (n < 2 || n > POLL_MAX_OPTIONS) return;

    int  announce = 0;
    char q_copy[POLL_QUESTION_CAP + 1];
    char opt_copy[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    char creator_copy[RGCN_MAX_NICK];

    GLOCK();
    if (g_poll.active && g_poll.poll_id == poll_id) {
        /* Our own creator's periodic resend, or an echo of it - just keep
         * the clock in sync, never touch voters. */
        g_poll.closes_at_ms = rgcn_now_ms() + (uint64_t)duration_s * 1000;
        GUNLOCK();
        return;
    }
    if (g_poll.active && g_poll.poll_id < poll_id) {
        /* A competing poll with a higher id lost the tie-break. */
        GUNLOCK();
        return;
    }

    g_poll.active  = 1;
    g_poll.poll_id = poll_id;
    strncpy(g_poll.creator, creator_nick, sizeof g_poll.creator - 1);
    g_poll.creator[sizeof g_poll.creator - 1] = 0;
    strncpy(g_poll.question, question, sizeof g_poll.question - 1);
    g_poll.question[sizeof g_poll.question - 1] = 0;
    g_poll.n_options = n;
    for (int i = 0; i < n; i++) {
        strncpy(g_poll.options[i], opts[i], sizeof g_poll.options[i] - 1);
        g_poll.options[i][sizeof g_poll.options[i] - 1] = 0;
    }
    g_poll.voter_count       = 0;
    g_poll.closes_at_ms      = rgcn_now_ms() + (uint64_t)duration_s * 1000;
    g_poll.last_open_sent_ms = rgcn_now_ms();
    g_my_last_vote_sent_ms   = 0;
    announce = 1;

    strncpy(q_copy, g_poll.question, sizeof q_copy);
    strncpy(creator_copy, g_poll.creator, sizeof creator_copy);
    for (int i = 0; i < n; i++) strncpy(opt_copy[i], g_poll.options[i], sizeof opt_copy[i]);
    GUNLOCK();

    if (!announce) return;
    char line[160];
    snprintf(line, sizeof line, "%s bir anket açtı (%ds): \"%s\"", creator_copy, duration_s, q_copy);
    rgcn_main_system(line);
    for (int i = 0; i < n; i++) {
        char opt_line[64];
        snprintf(opt_line, sizeof opt_line, "  %d) %s", i + 1, opt_copy[i]);
        rgcn_main_system(opt_line);
    }
    rgcn_main_system("  Oy ver: /vote <numara>   Durum: /poll   Bitir (sadece açan): /poll end");
}

static void apply_poll_vote(const char *voter_nick, uint64_t poll_id, int idx) {
    int  changed = 0;
    int  n = 0;
    char opt_name[POLL_OPTION_CAP + 1] = {0};
    int  counts[POLL_MAX_OPTIONS];

    GLOCK();
    if (!g_poll.active || g_poll.poll_id != poll_id) { GUNLOCK(); return; }
    if (idx < 0 || idx >= g_poll.n_options)          { GUNLOCK(); return; }
    n = g_poll.n_options;

    int found = -1;
    for (int i = 0; i < g_poll.voter_count; i++) {
        if (strcmp(g_poll.voters[i].nick, voter_nick) == 0) { found = i; break; }
    }
    if (found >= 0) {
        if (g_poll.voters[found].choice != idx) {
            g_poll.voters[found].choice = idx;
            changed = 1;
        }
    } else if (g_poll.voter_count < POLL_MAX_VOTERS) {
        strncpy(g_poll.voters[g_poll.voter_count].nick, voter_nick, sizeof g_poll.voters[0].nick - 1);
        g_poll.voters[g_poll.voter_count].nick[sizeof g_poll.voters[0].nick - 1] = 0;
        g_poll.voters[g_poll.voter_count].choice = idx;
        g_poll.voter_count++;
        changed = 1;
    }
    if (changed) {
        strncpy(opt_name, g_poll.options[idx], sizeof opt_name - 1);
        tally_locked(counts, n);
    }
    GUNLOCK();

    /* Resend echoes of an unchanged vote land here every ~2s per voter -
     * must stay silent or the log floods. */
    if (!changed) return;

    char line[160];
    int off = snprintf(line, sizeof line, "%s oy verdi: %s  (", voter_nick, opt_name);
    for (int i = 0; i < n; i++) {
        if (off <= 0 || off >= (int)sizeof line) break;
        off += snprintf(line + off, sizeof line - off, "%s%d:%d", i ? " " : "", i + 1, counts[i]);
    }
    if (off > 0 && off < (int)sizeof line) snprintf(line + off, sizeof line - off, ")");
    rgcn_main_system(line);
}

static void apply_poll_close(uint64_t poll_id, const int *final_counts, int n_counts) {
    char question[POLL_QUESTION_CAP + 1];
    char opt_copy[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    int  n = 0;
    int  counts[POLL_MAX_OPTIONS];

    GLOCK();
    if (!g_poll.active || g_poll.poll_id != poll_id) { GUNLOCK(); return; }
    n = g_poll.n_options;
    strncpy(question, g_poll.question, sizeof question);
    for (int i = 0; i < n; i++) strncpy(opt_copy[i], g_poll.options[i], sizeof opt_copy[i]);
    if (final_counts && n_counts == n) {
        for (int i = 0; i < n; i++) counts[i] = final_counts[i];
    } else {
        tally_locked(counts, n);
    }
    g_poll.active = 0;
    GUNLOCK();

    int max_count = 0;
    for (int i = 0; i < n; i++) if (counts[i] > max_count) max_count = counts[i];

    char line[160];
    snprintf(line, sizeof line, "Anket bitti: \"%s\"", question);
    rgcn_main_system(line);

    int shown[POLL_MAX_OPTIONS] = {0};
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (shown[i]) continue;
            if (best < 0 || counts[i] > counts[best]) best = i;
        }
        shown[best] = 1;
        char l[96];
        snprintf(l, sizeof l, "  %s%s: %d oy",
                 (max_count > 0 && counts[best] == max_count) ? "* " : "  ",
                 opt_copy[best], counts[best]);
        rgcn_main_system(l);
    }
    if (max_count == 0) rgcn_main_system("  (kimse oy vermedi)");
}

/* -------------------------------------------------------------------------- */
/* Wire helpers                                                                */
/* -------------------------------------------------------------------------- */

static void resend_open_wire(uint64_t poll_id) {
    char question[POLL_QUESTION_CAP + 1];
    char opts[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    int  n;
    int  dur_s;

    GLOCK();
    if (!g_poll.active || g_poll.poll_id != poll_id) { GUNLOCK(); return; }
    strncpy(question, g_poll.question, sizeof question);
    n = g_poll.n_options;
    for (int i = 0; i < n; i++) strncpy(opts[i], g_poll.options[i], sizeof opts[i]);
    uint64_t now = rgcn_now_ms();
    dur_s = (int)((g_poll.closes_at_ms > now) ? (g_poll.closes_at_ms - now) / 1000 : 0);
    GUNLOCK();

    char text[700];
    int off = snprintf(text, sizeof text, "%016llX|%d|%d|%s",
                        (unsigned long long)poll_id, dur_s, n, question);
    for (int i = 0; i < n; i++) {
        if (off <= 0 || off >= (int)sizeof text) break;
        off += snprintf(text + off, sizeof text - off, "|%s", opts[i]);
    }
    rgcn_main_send("POLL_OPEN", text);
}

static void broadcast_close(uint64_t poll_id) {
    int n;
    int counts[POLL_MAX_OPTIONS];

    GLOCK();
    if (!g_poll.active || g_poll.poll_id != poll_id) { GUNLOCK(); return; }
    n = g_poll.n_options;
    tally_locked(counts, n);
    GUNLOCK();

    apply_poll_close(poll_id, counts, n);

    char text[128];
    int off = snprintf(text, sizeof text, "%016llX|%d", (unsigned long long)poll_id, n);
    for (int i = 0; i < n; i++) {
        if (off <= 0 || off >= (int)sizeof text) break;
        off += snprintf(text + off, sizeof text - off, "|%d", counts[i]);
    }
    rgcn_main_send("POLL_CLOSE", text);
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                    */
/* -------------------------------------------------------------------------- */

static void cmd_status(void) {
    char question[POLL_QUESTION_CAP + 1], creator[RGCN_MAX_NICK];
    char opt_copy[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    int  n = 0, counts[POLL_MAX_OPTIONS];
    uint64_t remaining_s = 0;
    int active;

    GLOCK();
    active = g_poll.active;
    if (active) {
        strncpy(question, g_poll.question, sizeof question);
        strncpy(creator, g_poll.creator, sizeof creator);
        n = g_poll.n_options;
        for (int i = 0; i < n; i++) strncpy(opt_copy[i], g_poll.options[i], sizeof opt_copy[i]);
        tally_locked(counts, n);
        uint64_t now = rgcn_now_ms();
        remaining_s = (g_poll.closes_at_ms > now) ? (g_poll.closes_at_ms - now) / 1000 : 0;
    }
    GUNLOCK();

    if (!active) {
        rgcn_main_system("Aktif anket yok. Kullanım: /poll \"Soru\" \"A\" \"B\" ...");
        return;
    }
    char line[200];
    snprintf(line, sizeof line, "Aktif anket (%s açtı, ~%llus kaldı): \"%s\"",
             creator, (unsigned long long)remaining_s, question);
    rgcn_main_system(line);
    for (int i = 0; i < n; i++) {
        char l[80];
        snprintf(l, sizeof l, "  %d) %s  -  %d oy", i + 1, opt_copy[i], counts[i]);
        rgcn_main_system(l);
    }
}

static void cmd_end(void) {
    uint64_t poll_id = 0;
    int active = 0, is_creator = 0;

    GLOCK();
    active = g_poll.active;
    poll_id = g_poll.poll_id;
    is_creator = active && strcmp(g_poll.creator, rgcn_main_nick()) == 0;
    GUNLOCK();

    if (!active)     { rgcn_main_system("Aktif anket yok."); return; }
    if (!is_creator)  { rgcn_main_system("Anketi sadece açan kişi bitirebilir."); return; }
    broadcast_close(poll_id);
}

void rgcn_poll_cmd(const char *args) {
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "end", 3) == 0 && (p[3] == 0 || p[3] == ' ')) { cmd_end(); return; }
    if (!*p) { cmd_status(); return; }

    char question[POLL_QUESTION_CAP + 1];
    const char *rest = parse_token(p, question, sizeof question);
    if (!question[0]) {
        rgcn_main_system("Kullanım: /poll \"Soru\" \"Seçenek A\" \"Seçenek B\" ...");
        return;
    }
    sanitize(question);

    char opts[POLL_MAX_OPTIONS][POLL_OPTION_CAP + 1];
    const char *optp[POLL_MAX_OPTIONS];
    int n = 0;
    while (rest && n < POLL_MAX_OPTIONS) {
        rest = parse_token(rest, opts[n], sizeof opts[n]);
        if (!opts[n][0]) break;
        sanitize(opts[n]);
        optp[n] = opts[n];
        n++;
    }
    if (n < 2) {
        rgcn_main_system("En az 2 seçenek gerekli. Kullanım: /poll \"Soru\" \"A\" \"B\" ...");
        return;
    }

    GLOCK();
    if (g_poll.active) {
        GUNLOCK();
        rgcn_main_system("Zaten aktif bir anket var. Bitirmek için: /poll end");
        return;
    }
    GUNLOCK();

    uint64_t id = rgcn_random_u64();
    apply_poll_open(rgcn_main_nick(), id, POLL_DURATION_S, question, optp, n);

    char text[700];
    int off = snprintf(text, sizeof text, "%016llX|%d|%d|%s",
                        (unsigned long long)id, POLL_DURATION_S, n, question);
    for (int i = 0; i < n; i++) {
        if (off <= 0 || off >= (int)sizeof text) break;
        off += snprintf(text + off, sizeof text - off, "|%s", opts[i]);
    }
    rgcn_main_send("POLL_OPEN", text);
}

void rgcn_vote_cmd(const char *args) {
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { rgcn_main_system("Kullanım: /vote <numara>"); return; }

    int active, n_options;
    uint64_t poll_id;
    GLOCK();
    active     = g_poll.active;
    n_options  = g_poll.n_options;
    poll_id    = g_poll.poll_id;
    GUNLOCK();

    if (!active) { rgcn_main_system("Aktif anket yok."); return; }

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || v < 1 || v > n_options) {
        char b[64];
        snprintf(b, sizeof b, "Kullanım: /vote 1-%d", n_options);
        rgcn_main_system(b);
        return;
    }

    int idx = (int)v - 1;
    apply_poll_vote(rgcn_main_nick(), poll_id, idx);
    g_my_last_vote_sent_ms = rgcn_now_ms();

    char text[48];
    snprintf(text, sizeof text, "%016llX|%d", (unsigned long long)poll_id, idx);
    rgcn_main_send("POLL_VOTE", text);
}

/* -------------------------------------------------------------------------- */
/* Wire-in                                                                     */
/* -------------------------------------------------------------------------- */

void rgcn_poll_on_open(const char *creator_nick, uint64_t poll_id, int duration_s,
                       const char *question, const char * const *opts, int n_opts) {
    apply_poll_open(creator_nick, poll_id, duration_s, question, opts, n_opts);
}

void rgcn_poll_on_vote(const char *voter_nick, uint64_t poll_id, int choice_idx) {
    apply_poll_vote(voter_nick, poll_id, choice_idx);
}

void rgcn_poll_on_close(const char *closer_nick, uint64_t poll_id,
                        const int *final_counts, int n_counts) {
    int ok;
    GLOCK();
    ok = g_poll.active && g_poll.poll_id == poll_id && strcmp(g_poll.creator, closer_nick) == 0;
    GUNLOCK();
    if (!ok) return;
    apply_poll_close(poll_id, final_counts, n_counts);
}

/* -------------------------------------------------------------------------- */
/* Periodic tick                                                              */
/* -------------------------------------------------------------------------- */

void rgcn_poll_tick(void) {
    uint64_t now = rgcn_now_ms();
    int do_creator_close = 0, do_fallback_close = 0, do_resend_open = 0;
    uint64_t poll_id = 0;
    int my_choice = -1;

    GLOCK();
    if (g_poll.active) {
        poll_id = g_poll.poll_id;
        int am_creator = strcmp(g_poll.creator, rgcn_main_nick()) == 0;
        if (am_creator) {
            if (now >= g_poll.closes_at_ms) {
                do_creator_close = 1;
            } else if (now - g_poll.last_open_sent_ms >= POLL_OPEN_RESEND_MS) {
                do_resend_open = 1;
                g_poll.last_open_sent_ms = now;
            }
        } else if (now >= g_poll.closes_at_ms + POLL_CLOSE_FALLBACK_MS) {
            do_fallback_close = 1;
        }
        for (int i = 0; i < g_poll.voter_count; i++) {
            if (strcmp(g_poll.voters[i].nick, rgcn_main_nick()) == 0) {
                my_choice = g_poll.voters[i].choice;
                break;
            }
        }
    }
    GUNLOCK();

    if (do_creator_close)  { broadcast_close(poll_id); return; }
    if (do_fallback_close) { apply_poll_close(poll_id, NULL, 0); return; }
    if (do_resend_open) resend_open_wire(poll_id);

    if (my_choice >= 0 && now - g_my_last_vote_sent_ms >= POLL_VOTE_RESEND_MS) {
        g_my_last_vote_sent_ms = now;
        char text[48];
        snprintf(text, sizeof text, "%016llX|%d", (unsigned long long)poll_id, my_choice);
        rgcn_main_send("POLL_VOTE", text);
    }
}
