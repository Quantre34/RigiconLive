/*
 * Rigicon Live - main entry point.
 *
 * Design:
 *   - Every packet is ChaCha20-Poly1305 AEAD sealed before it hits the wire.
 *   - No files, no logs, no history. Everything lives in RAM until quit.
 *   - Everyone tuned to the same port hears every transmission on that port.
 *   - Change port = change channel (private group chat).
 */

#include "rgcn.h"
#include "crypto.h"
#include "net.h"
#include "term.h"
#include "notify.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

#ifndef S_ISREG
  #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  #define RGCN_THREAD_RET unsigned __stdcall
#else
  #include <pthread.h>
  #include <sys/time.h>
  #include <unistd.h>
  #define RGCN_THREAD_RET void *
#endif

/* -------------------------------------------------------------------------- */
/* Globals                                                                     */
/* -------------------------------------------------------------------------- */

static volatile int g_running = 1;

static char       g_nick[RGCN_MAX_NICK];
static int        g_port    = RGCN_DEFAULT_PORT;
static uint64_t   g_station = 0;
static rgcn_net_t *g_net = NULL;

static char       g_input[RGCN_MAX_TEXT];
static size_t     g_input_len = 0;
static int        g_notify_on = 0;
static uint32_t   g_timer_ms  = 0;   /* /timer session TTL; 0 = off */

/* Scrollback that we control. Every visible line is stored here as a fully
 * formatted ANSI string. When a burn expires, we drop its entry from the log
 * and repaint the whole visible area from what's left. This is what gives
 * us actual "silme" without cursor-based overwrite hacks (which corrupted
 * ordering once the terminal scrolled). */
#define LOG_CAP 512
struct log_entry {
    char     line[RGCN_MAX_TEXT + 256];
    uint64_t expire_ms;   /* 0 = permanent */
    int      alive;
};
static struct log_entry g_log[LOG_CAP];
static int g_log_count = 0;   /* how many slots filled (<= LOG_CAP) */
static int g_log_head  = 0;   /* index of oldest entry when the ring is full */

/* Pending inbound file offers (someone offered a file, we haven't decided). */
#define OFFER_SLOTS 16
static struct {
    uint64_t offer_id;
    char     sender_nick[RGCN_MAX_NICK];
    uint32_t sender_ip_be;
    uint16_t sender_port_be;   /* TCP port to fetch from */
    char     filename[RGCN_FILE_MAX_NAME];
    uint64_t size;
    uint8_t  sha256[32];
    uint64_t received_at_ms;
} g_offers[OFFER_SLOTS];
static int g_offer_count = 0;

/* Outbound offers we've broadcast, waiting for accept. */
#define SENT_OFFER_SLOTS 4
static struct {
    uint64_t offer_id;
    int      listen_socket;
    uint16_t listen_port;
    char     path[RGCN_FILE_MAX_PATH];
    uint64_t size;
    char     filename[RGCN_FILE_MAX_NAME];
} g_sent_offers[SENT_OFFER_SLOTS];
static int g_sent_offer_count = 0;

static uint64_t hex_to_u64(const char *s) {
    uint64_t v = 0;
    while (*s) {
        v <<= 4;
        char c = *s++;
        if      (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    }
    return v;
}

static void hex_encode(const uint8_t *b, size_t n, char *out) {
    static const char *hx = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = hx[b[i] >> 4];
        out[i*2+1] = hx[b[i] & 0x0f];
    }
    out[n * 2] = 0;
}

static int hex_decode(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char h = hex[i*2], l = hex[i*2+1];
        int hv = (h >= '0' && h <= '9') ? h - '0'
               : (h >= 'a' && h <= 'f') ? h - 'a' + 10
               : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        int lv = (l >= '0' && l <= '9') ? l - '0'
               : (l >= 'a' && l <= 'f') ? l - 'a' + 10
               : (l >= 'A' && l <= 'F') ? l - 'A' + 10 : -1;
        if (hv < 0 || lv < 0) return -1;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return 0;
}

#define SEEN_CAP 512
static uint64_t g_seen[SEEN_CAP];
static int      g_seen_pos = 0;

#define PEERS_CAP    64
#define PEER_STALE_MS   90000    /* 90s without a packet = presume gone */
#define HEARTBEAT_MS    30000    /* re-announce ourselves every 30s */

static struct {
    char     nick[RGCN_MAX_NICK];
    uint64_t station;
    uint64_t last_ms;
    uint32_t ip_be;
    uint16_t port_be;
} g_peers[PEERS_CAP];
static int g_peer_count = 0;

#ifdef _WIN32
  static CRITICAL_SECTION g_peer_lock;
  static int              g_peer_lock_ready = 0;
  #define PEER_LOCK()   do { if (!g_peer_lock_ready) { InitializeCriticalSection(&g_peer_lock); g_peer_lock_ready = 1; } EnterCriticalSection(&g_peer_lock); } while (0)
  #define PEER_UNLOCK() LeaveCriticalSection(&g_peer_lock)
#else
  static pthread_mutex_t g_peer_lock = PTHREAD_MUTEX_INITIALIZER;
  #define PEER_LOCK()   pthread_mutex_lock(&g_peer_lock)
  #define PEER_UNLOCK() pthread_mutex_unlock(&g_peer_lock)
#endif

/* -------------------------------------------------------------------------- */
/* Utilities                                                                   */
/* -------------------------------------------------------------------------- */

uint64_t rgcn_now_ms(void) {
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int already_seen(uint64_t id) {
    for (int i = 0; i < SEEN_CAP; i++) if (g_seen[i] == id) return 1;
    g_seen[g_seen_pos] = id;
    g_seen_pos = (g_seen_pos + 1) % SEEN_CAP;
    return 0;
}

/* Returns 1 if this is a newly-seen station (new nick, new session, or new
 * address), 0 if refresh of the same session we've been talking to. Used to
 * decide whether to echo-back JOIN so both sides converge on each other's IP. */
static int peer_touch(const char *nick, uint64_t station,
                      uint32_t ip_be, uint16_t port_be) {
    PEER_LOCK();
    uint64_t now = rgcn_now_ms();
    for (int i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].nick, nick) == 0) {
            int changed = (g_peers[i].station != station)
                       || (ip_be   && g_peers[i].ip_be   != ip_be)
                       || (port_be && g_peers[i].port_be != port_be);
            g_peers[i].station = station;
            g_peers[i].last_ms = now;
            if (ip_be)   g_peers[i].ip_be   = ip_be;
            if (port_be) g_peers[i].port_be = port_be;
            PEER_UNLOCK();
            return changed ? 1 : 0;
        }
    }
    if (g_peer_count < PEERS_CAP) {
        strncpy(g_peers[g_peer_count].nick, nick, RGCN_MAX_NICK - 1);
        g_peers[g_peer_count].nick[RGCN_MAX_NICK - 1] = 0;
        g_peers[g_peer_count].station = station;
        g_peers[g_peer_count].last_ms = now;
        g_peers[g_peer_count].ip_be   = ip_be;
        g_peers[g_peer_count].port_be = port_be;
        g_peer_count++;
        PEER_UNLOCK();
        return 1;
    }
    PEER_UNLOCK();
    return 0;
}

static void peer_drop(const char *nick) {
    PEER_LOCK();
    for (int i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].nick, nick) == 0) {
            g_peers[i] = g_peers[g_peer_count - 1];
            g_peer_count--;
            break;
        }
    }
    PEER_UNLOCK();
}

/* Expire peers we havent heard from in PEER_STALE_MS and report each one
 * via callback. Called periodically from the receiver thread. */
static void peer_expire_stale(void (*on_drop)(const char *)) {
    char dropped[PEERS_CAP][RGCN_MAX_NICK];
    int  drop_count = 0;
    uint64_t now = rgcn_now_ms();

    PEER_LOCK();
    for (int i = 0; i < g_peer_count; ) {
        if (now - g_peers[i].last_ms > PEER_STALE_MS) {
            if (drop_count < PEERS_CAP) {
                strncpy(dropped[drop_count], g_peers[i].nick, RGCN_MAX_NICK - 1);
                dropped[drop_count][RGCN_MAX_NICK - 1] = 0;
                drop_count++;
            }
            g_peers[i] = g_peers[g_peer_count - 1];
            g_peer_count--;
        } else {
            i++;
        }
    }
    PEER_UNLOCK();

    for (int i = 0; i < drop_count; i++) on_drop(dropped[i]);
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                   */
/* -------------------------------------------------------------------------- */

static void time_hms(char *out, size_t cap, uint64_t ms) {
    time_t s = (time_t)(ms / 1000);
    struct tm tm_;
#ifdef _WIN32
    localtime_s(&tm_, &s);
#else
    localtime_r(&s, &tm_);
#endif
    snprintf(out, cap, "%02d:%02d:%02d", tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
}

static void print_prompt_unlocked(void) {
    const char *c = rgcn_color_for(g_nick);
    const char *gray = rgcn_color_gray();
    const char *r = rgcn_color_reset();
    printf("\r\x1b[K");
    if (g_timer_ms > 0) {
        printf("%s[%us]%s ", gray, g_timer_ms / 1000, r);
    }
    printf("%s%s%s ▸ %s", c, g_nick, gray, r);
    if (g_input_len > 0) fwrite(g_input, 1, g_input_len, stdout);
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/* Message log + redraw                                                        */
/*                                                                             */
/* Anything visible on screen is also an entry in g_log. When a burn expires  */
/* we mark its entry dead (alive=0) and full-redraw the alt screen from what  */
/* is left. This is the actual silme mechanic - not a cursor-tricks patch.    */
/* -------------------------------------------------------------------------- */

static int log_iter_idx(int i) {
    return (g_log_count < LOG_CAP) ? i : (g_log_head + i) % LOG_CAP;
}

/* Append one fully-formatted line to the log. expire_ms > 0 means the entry
 * will be removed from the log at that wall-clock timestamp. Ring wraps. */
static void log_append(const char *line, uint64_t expire_ms) {
    int idx;
    if (g_log_count < LOG_CAP) {
        idx = g_log_count++;
    } else {
        idx = g_log_head;
        g_log_head = (g_log_head + 1) % LOG_CAP;
    }
    strncpy(g_log[idx].line, line, sizeof g_log[idx].line - 1);
    g_log[idx].line[sizeof g_log[idx].line - 1] = 0;
    g_log[idx].expire_ms = expire_ms;
    g_log[idx].alive     = 1;
}

static void log_clear(void) {
    g_log_count = 0;
    g_log_head  = 0;
}

/* Forward decl - defined below after render_banner. */
static void print_banner_body(int notify_on);

static void redraw_from_log_unlocked(void) {
    printf("\x1b[H\x1b[2J");   /* home + clear entire visible screen */
    print_banner_body(g_notify_on);
    for (int i = 0; i < g_log_count; i++) {
        int idx = log_iter_idx(i);
        if (!g_log[idx].alive) continue;
        printf("%s\n", g_log[idx].line);
    }
    print_prompt_unlocked();
}

static void write_line(const char *line) {
    rgcn_term_lock();
    log_append(line, 0);
    printf("\r\x1b[K%s\n", line);
    print_prompt_unlocked();
    rgcn_term_unlock();
}

static void render_chat(const char *nick, const char *text, uint64_t ts) {
    char tbuf[16]; time_hms(tbuf, sizeof tbuf, ts);
    char line[RGCN_MAX_TEXT + 128];
    snprintf(line, sizeof line, "%s[%s]%s %s%s%s: %s",
             rgcn_color_gray(), tbuf, rgcn_color_reset(),
             rgcn_color_for(nick), nick, rgcn_color_reset(),
             text);
    write_line(line);
}

/* Timed message: rendered with a [Ns] tag and scheduled for actual removal
 * from the log at now + ttl_ms. */
static void render_burn(const char *nick, const char *text, uint64_t ts,
                        uint32_t ttl_ms) {
    char tbuf[16]; time_hms(tbuf, sizeof tbuf, ts);
    char line[RGCN_MAX_TEXT + 160];
    snprintf(line, sizeof line,
             "%s[%s]%s %s%s%s: %s   %s[%us]%s",
             rgcn_color_gray(), tbuf, rgcn_color_reset(),
             rgcn_color_for(nick), nick, rgcn_color_reset(),
             text,
             rgcn_color_gray(), ttl_ms / 1000, rgcn_color_reset());
    rgcn_term_lock();
    log_append(line, rgcn_now_ms() + ttl_ms);
    printf("\r\x1b[K%s\n", line);
    print_prompt_unlocked();
    rgcn_term_unlock();
}

/* Called periodically from the receiver thread. Any log entry whose
 * expire_ms has passed is marked dead and the whole screen is repainted
 * from the surviving entries. That is the actual "silme" - the message
 * disappears from the visible screen entirely, and (because we are in
 * the alternate screen buffer) also from the scrollback of this session. */
static void expire_burns(void) {
    uint64_t now = rgcn_now_ms();
    int removed = 0;
    rgcn_term_lock();
    for (int i = 0; i < g_log_count; i++) {
        int idx = log_iter_idx(i);
        if (!g_log[idx].alive) continue;
        if (g_log[idx].expire_ms == 0) continue;
        if (now >= g_log[idx].expire_ms) {
            g_log[idx].alive = 0;
            removed++;
        }
    }
    if (removed) redraw_from_log_unlocked();
    rgcn_term_unlock();
}

/* /clear semantics: forget everything we ever rendered + wipe the screen. */
static void reset_screen(void) {
    rgcn_term_lock();
    log_clear();
    redraw_from_log_unlocked();
    rgcn_term_unlock();
}

static void render_system(const char *text) {
    char tbuf[16]; time_hms(tbuf, sizeof tbuf, rgcn_now_ms());
    char line[512];
    snprintf(line, sizeof line, "%s[%s]%s %s* %s%s",
             rgcn_color_gray(), tbuf, rgcn_color_reset(),
             rgcn_color_system(), text, rgcn_color_reset());
    write_line(line);
}

/* Full screen redraw wrapper for callers that hold no lock. */
static void redraw_screen(void) {
    rgcn_term_lock();
    redraw_from_log_unlocked();
    rgcn_term_unlock();
}

/* Initial banner + subsequent full redraws (Ctrl+L, /clear) both go here.
 * Callers should not hold term_lock. */
static void render_banner(int notify_on) {
    (void)notify_on;
    redraw_screen();
}

static void print_banner_body(int notify_on) {
    const char *gray = rgcn_color_gray();
    const char *rst  = rgcn_color_reset();
    const char *me   = rgcn_color_for(g_nick);
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  \x1b[1m\x1b[38;5;39mR I G I C O N   L I V E\x1b[0m   %s· Rigicon Inc.%s\n", gray, rst);
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  %sRumuz    :%s %s%s%s\n", gray, rst, me, g_nick, rst);
    printf("  %sKanal    :%s %d\n", gray, rst, g_port);
    printf("  %sBildirim :%s %s\n", gray, rst, notify_on ? "Açık" : "Kapalı");
    printf("  %sŞifreleme:%s ChaCha20-Poly1305 (AEAD, RFC 8439)\n", gray, rst);
    printf("  %sİz       :%s Sıfır. Kapanınca her şey gider.\n", gray, rst);
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  %sKomutlar: /quit /clear /who /status /timer /send /accept /reject /help%s\n", gray, rst);
    printf("%s%s%s\n\n", gray, "───────────────────────────────────────────────────────────", rst);
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/* Wire format:                                                                */
/*   "CHAT|<station:016X>|<id:016X>|<ts_ms>|<nick>|<text...>"                  */
/*   "JOIN|<station>|<id>|<ts>|<nick>"                                         */
/*   "LEAVE|<station>|<id>|<ts>|<nick>"                                        */
/* All inside a ChaCha20-Poly1305 sealed envelope on the wire.                 */
/* -------------------------------------------------------------------------- */

/* Sends a raw kind. If ttl_ms > 0 and kind is CHAT, transparently upgrades to
 * BURN wire type with the ttl embedded. */
static void send_kind_ttl(const char *kind, const char *text, uint32_t ttl_ms) {
    uint64_t id = 0;
    rgcn_random_bytes((uint8_t *)&id, sizeof id);
    uint64_t ts = rgcn_now_ms();

    char plain[RGCN_MAX_TEXT + 128];
    int n;
    int is_burn = (ttl_ms > 0 && strcmp(kind, "CHAT") == 0);
    if (is_burn) {
        n = snprintf(plain, sizeof plain,
                     "BURN|%016llX|%016llX|%llu|%s|%u|%s",
                     (unsigned long long)g_station,
                     (unsigned long long)id,
                     (unsigned long long)ts,
                     g_nick, ttl_ms,
                     text ? text : "");
    } else if (text) {
        n = snprintf(plain, sizeof plain,
                     "%s|%016llX|%016llX|%llu|%s|%s",
                     kind,
                     (unsigned long long)g_station,
                     (unsigned long long)id,
                     (unsigned long long)ts,
                     g_nick,
                     text);
    } else {
        n = snprintf(plain, sizeof plain,
                     "%s|%016llX|%016llX|%llu|%s",
                     kind,
                     (unsigned long long)g_station,
                     (unsigned long long)id,
                     (unsigned long long)ts,
                     g_nick);
    }
    if (n < 0 || n >= (int)sizeof plain) return;

    /* Mark our own message as seen so we ignore its echo. */
    already_seen(id);

    uint8_t pkt[RGCN_MAX_PACKET];
    size_t pkt_len = 0;
    if (rgcn_seal((const uint8_t *)plain, (size_t)n, pkt, sizeof pkt, &pkt_len) != 0) return;

    /* Discovery path: multicast + subnet broadcast + limited broadcast.
     * Unreliable on WiFi (packet loss up to 70%) but reaches new peers. */
    rgcn_net_broadcast(g_net, pkt, pkt_len);

    /* Reliable path: unicast to every known peer (ACKed at WiFi layer).
     * Snapshot under lock, send unlocked (no network I/O while holding lock). */
    struct { uint32_t ip; uint16_t port; } snap[PEERS_CAP];
    int snap_n = 0;
    uint64_t now = rgcn_now_ms();
    PEER_LOCK();
    for (int i = 0; i < g_peer_count; i++) {
        if (!g_peers[i].ip_be || !g_peers[i].port_be) continue;
        if (now - g_peers[i].last_ms > PEER_STALE_MS) continue;
        snap[snap_n].ip   = g_peers[i].ip_be;
        snap[snap_n].port = g_peers[i].port_be;
        snap_n++;
    }
    PEER_UNLOCK();
    for (int i = 0; i < snap_n; i++) {
        rgcn_net_unicast(g_net, snap[i].ip, snap[i].port, pkt, pkt_len);
    }
}

/* Convenience wrapper - existing call sites don't use timer. */
static void send_msg(const char *kind, const char *text) {
    send_kind_ttl(kind, text, 0);
}

/* Broadcast a raw pre-formed plaintext message (for FILE_* messages that don't
 * fit the "kind|station|id|ts|nick|text" auto-formatting). */
static void send_raw_plaintext(const char *plain, size_t n) {
    uint8_t pkt[RGCN_MAX_PACKET];
    size_t pkt_len = 0;
    if (rgcn_seal((const uint8_t *)plain, n, pkt, sizeof pkt, &pkt_len) != 0) return;
    rgcn_net_broadcast(g_net, pkt, pkt_len);

    struct { uint32_t ip; uint16_t port; } snap[PEERS_CAP];
    int snap_n = 0;
    uint64_t now = rgcn_now_ms();
    PEER_LOCK();
    for (int i = 0; i < g_peer_count; i++) {
        if (!g_peers[i].ip_be || !g_peers[i].port_be) continue;
        if (now - g_peers[i].last_ms > PEER_STALE_MS) continue;
        snap[snap_n].ip   = g_peers[i].ip_be;
        snap[snap_n].port = g_peers[i].port_be;
        snap_n++;
    }
    PEER_UNLOCK();
    for (int i = 0; i < snap_n; i++)
        rgcn_net_unicast(g_net, snap[i].ip, snap[i].port, pkt, pkt_len);
}

static char *split(char **s) {
    if (!*s) return NULL;
    char *start = *s;
    char *bar = strchr(start, '|');
    if (bar) { *bar = 0; *s = bar + 1; }
    else     { *s = NULL; }
    return start;
}

static void handle_decoded(const char *raw, size_t raw_len,
                           uint32_t from_ip_be, uint16_t from_port_be) {
    char buf[RGCN_MAX_TEXT + 128];
    if (raw_len >= sizeof buf) return;
    memcpy(buf, raw, raw_len);
    buf[raw_len] = 0;

    char *p    = buf;
    char *kind = split(&p);
    char *st_s = split(&p);
    char *id_s = split(&p);
    char *ts_s = split(&p);
    char *nick = split(&p);
    if (!kind || !st_s || !id_s || !ts_s || !nick) return;

    unsigned long long st  = strtoull(st_s, NULL, 16);
    unsigned long long id  = strtoull(id_s, NULL, 16);
    unsigned long long ts  = strtoull(ts_s, NULL, 10);

    if (st == g_station) return;
    if (already_seen((uint64_t)id)) return;

    /* Nickname sanity: strip control chars, cap length. */
    for (char *c = nick; *c; c++) if ((unsigned char)*c < 0x20) *c = '?';
    if (strlen(nick) == 0 || strlen(nick) >= RGCN_MAX_NICK) return;

    /* Only trust unicast addresses (never a broadcast/multicast) for peer
     * cache. The sender's socket is bound to our channel port, so we can
     * unicast back on the same port. */
    if (rgcn_net_is_self(g_net, from_ip_be)) from_ip_be = 0;

    if (strcmp(kind, "CHAT") == 0) {
        char *text = p ? p : (char *)"";
        /* Sanitize text: strip control chars (ANSI escape injection prevention).
         * A peer with the shared key could otherwise craft a packet that
         * moves our cursor, clears our screen, or hides output. */
        for (char *c = text; *c; c++) {
            unsigned char b = (unsigned char)*c;
            if (b < 0x20 && b != '\t') *c = '?';
            if (b == 0x7f) *c = '?';
        }
        int is_new = peer_touch(nick, (uint64_t)st, from_ip_be, from_port_be);
        render_chat(nick, text, (uint64_t)ts);
        char nbody[RGCN_MAX_TEXT + 64];
        snprintf(nbody, sizeof nbody, "%s: %s", nick, text);
        rgcn_notify(RGCN_APP_NAME, nbody);
        /* If a peer starts CHATting without us seeing their JOIN (broadcast
         * was lost), still send them our address unicast. */
        if (is_new && from_ip_be && from_port_be) send_msg("JOIN", NULL);
    } else if (strcmp(kind, "JOIN") == 0 || strcmp(kind, "HELLO") == 0) {
        int is_new = peer_touch(nick, (uint64_t)st, from_ip_be, from_port_be);
        /* Only announce genuinely new sessions - don't render echo-back
         * JOINs, HELLO heartbeats, or re-broadcasts. */
        if (is_new && strcmp(kind, "JOIN") == 0) {
            char line[128];
            snprintf(line, sizeof line, "%s kanala katıldı", nick);
            render_system(line);
        }
        /* Echo-back so the new peer learns our IP too. */
        if (is_new && from_ip_be && from_port_be) send_msg("JOIN", NULL);
    } else if (strcmp(kind, "FILE_OFFER") == 0) {
        /* FILE_OFFER|station|id|ts|sender_nick|offer_id_hex|filename|size|sha256_hex|tcp_port|target_nick */
        char *offer_id_s = split(&p);
        char *filename   = split(&p);
        char *size_s     = split(&p);
        char *sha_hex    = split(&p);
        char *port_s     = split(&p);
        char *target     = p;   /* remainder, possibly empty */
        if (!offer_id_s || !filename || !size_s || !sha_hex || !port_s) return;
        if (target && *target && strcmp(target, g_nick) != 0) return;

        char clean_name[RGCN_FILE_MAX_NAME];
        if (rgcn_file_sanitize_name(filename, clean_name, sizeof clean_name) != 0) return;

        uint64_t off_id = hex_to_u64(offer_id_s);
        uint64_t sz     = strtoull(size_s, NULL, 10);
        uint16_t tcp_p  = (uint16_t)atoi(port_s);
        if (sz > RGCN_FILE_MAX_SIZE) return;
        if (!tcp_p) return;
        uint8_t sha[32];
        if (hex_decode(sha_hex, sha, 32) != 0) return;

        /* Cache the offer */
        if (g_offer_count < OFFER_SLOTS) {
            g_offers[g_offer_count].offer_id       = off_id;
            strncpy(g_offers[g_offer_count].sender_nick, nick, RGCN_MAX_NICK - 1);
            g_offers[g_offer_count].sender_nick[RGCN_MAX_NICK - 1] = 0;
            g_offers[g_offer_count].sender_ip_be   = from_ip_be;
            /* Sender's TCP port is different from our UDP channel port -
             * they told us in the packet. Store in network byte order. */
            g_offers[g_offer_count].sender_port_be = htons(tcp_p);
            strncpy(g_offers[g_offer_count].filename, clean_name, RGCN_FILE_MAX_NAME - 1);
            g_offers[g_offer_count].filename[RGCN_FILE_MAX_NAME - 1] = 0;
            g_offers[g_offer_count].size           = sz;
            memcpy(g_offers[g_offer_count].sha256, sha, 32);
            g_offers[g_offer_count].received_at_ms = rgcn_now_ms();
            g_offer_count++;
        }

        char sbuf[32]; rgcn_file_size_str(sz, sbuf, sizeof sbuf);
        char line[256];
        int risky = rgcn_file_ext_is_risky(clean_name);
        snprintf(line, sizeof line,
                 "%s%s%s dosya paylaşıyor: %s (%s)%s",
                 rgcn_color_for(nick), nick, rgcn_color_reset(),
                 clean_name, sbuf,
                 risky ? "  \x1b[33m[UYARI: çalıştırılabilir uzantı]\x1b[0m" : "");
        render_system(line);
        snprintf(line, sizeof line,
                 "  Kabul: /accept %s   |   Reddet: /reject %s",
                 clean_name, clean_name);
        render_system(line);
    } else if (strcmp(kind, "FILE_ACCEPT") == 0) {
        /* FILE_ACCEPT|station|id|ts|accepter_nick|offer_id_hex */
        char *offer_id_s = p ? p : NULL;
        if (!offer_id_s) return;
        uint64_t off_id = hex_to_u64(offer_id_s);
        /* Is this OUR outbound offer? */
        int idx = -1;
        for (int i = 0; i < g_sent_offer_count; i++) {
            if (g_sent_offers[i].offer_id == off_id) { idx = i; break; }
        }
        if (idx < 0) return;
        char line[192];
        snprintf(line, sizeof line, "%s%s%s kabul etti: %s - gönderiliyor...",
                 rgcn_color_for(nick), nick, rgcn_color_reset(),
                 g_sent_offers[idx].filename);
        render_system(line);
        /* Sender's serve thread will accept and stream. See spawn_serve_thread. */
    } else if (strcmp(kind, "FILE_REJECT") == 0) {
        char *offer_id_s = p ? p : NULL;
        if (!offer_id_s) return;
        char line[192];
        snprintf(line, sizeof line, "%s%s%s dosya isteğini reddetti",
                 rgcn_color_for(nick), nick, rgcn_color_reset());
        render_system(line);
    } else if (strcmp(kind, "BURN") == 0) {
        /* BURN|station|id|ts|nick|expire_ms|text - self-destructing CHAT. */
        char *ttl_s = p ? split(&p) : NULL;
        char *text  = p ? p : (char *)"";
        if (!ttl_s) return;
        for (char *c = text; *c; c++) {
            unsigned char b = (unsigned char)*c;
            if (b < 0x20 && b != '\t') *c = '?';
            if (b == 0x7f) *c = '?';
        }
        unsigned long ttl_ms = strtoul(ttl_s, NULL, 10);
        if (ttl_ms < 1000)      ttl_ms = 1000;      /* min 1s */
        if (ttl_ms > 3600000UL) ttl_ms = 3600000UL; /* max 1h */
        int is_new = peer_touch(nick, (uint64_t)st, from_ip_be, from_port_be);
        render_burn(nick, text, (uint64_t)ts, (uint32_t)ttl_ms);
        char nbody[RGCN_MAX_TEXT + 64];
        snprintf(nbody, sizeof nbody, "%s: %s [%us]", nick, text, (unsigned)(ttl_ms/1000));
        rgcn_notify(RGCN_APP_NAME, nbody);
        if (is_new && from_ip_be && from_port_be) send_msg("JOIN", NULL);
    } else if (strcmp(kind, "LEAVE") == 0) {
        peer_drop(nick);
        char line[128];
        snprintf(line, sizeof line, "%s kanaldan ayrıldı", nick);
        render_system(line);
    }
}

/* -------------------------------------------------------------------------- */
/* Receiver thread                                                             */
/* -------------------------------------------------------------------------- */

static void on_peer_timeout(const char *nick) {
    char line[128];
    snprintf(line, sizeof line, "%s kanaldan ayrıldı (yanıt yok)", nick);
    render_system(line);
}

static RGCN_THREAD_RET receiver_thread(void *arg) {
    (void)arg;
    uint8_t pkt[RGCN_MAX_PACKET];
    uint8_t plain[RGCN_MAX_PACKET];
    uint64_t last_heartbeat = rgcn_now_ms();

    while (g_running) {
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int r = rgcn_net_recv(g_net, pkt, sizeof pkt, 500, &from_ip, &from_port);
        if (r > 0) {
            size_t plain_len = 0;
            if (rgcn_open(pkt, (size_t)r, plain, sizeof plain, &plain_len) == 0)
                handle_decoded((const char *)plain, plain_len, from_ip, from_port);
        }

        /* Every wake-up: check burn expirations (fast, cheap). */
        expire_burns();

        /* Periodic maintenance: expire stale peers + re-announce presence. */
        uint64_t now = rgcn_now_ms();
        if (now - last_heartbeat >= HEARTBEAT_MS) {
            peer_expire_stale(on_peer_timeout);
            send_msg("HELLO", NULL);
            last_heartbeat = now;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* -------------------------------------------------------------------------- */
/* Signal handling                                                             */
/* -------------------------------------------------------------------------- */

static void on_sigint(int sig) { (void)sig; g_running = 0; }

#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD kind) {
    if (kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT ||
        kind == CTRL_CLOSE_EVENT || kind == CTRL_LOGOFF_EVENT ||
        kind == CTRL_SHUTDOWN_EVENT) {
        g_running = 0;
        return TRUE;
    }
    return FALSE;
}
#endif

/* -------------------------------------------------------------------------- */
/* Line editing                                                                */
/* -------------------------------------------------------------------------- */

static void input_backspace(void) {
    while (g_input_len > 0 && (g_input[g_input_len - 1] & 0xC0) == 0x80) g_input_len--;
    if (g_input_len > 0) g_input_len--;
    g_input[g_input_len] = 0;
}

/* -------------------------------------------------------------------------- */
/* File transfer glue - threads used by /send and /accept below                */
/* -------------------------------------------------------------------------- */

struct serve_args {
    int      listen_sock;
    uint64_t offer_id;
    uint64_t size;
    char     path[RGCN_FILE_MAX_PATH];
    char     filename[RGCN_FILE_MAX_NAME];
};

static void serve_progress(uint64_t sent, uint64_t total, void *ud) {
    struct serve_args *a = (struct serve_args *)ud;
    static uint64_t last_report = 0;
    uint64_t now = rgcn_now_ms();
    if (now - last_report < 1000 && sent < total) return;
    last_report = now;
    int pct = (int)((sent * 100) / (total ? total : 1));
    char sbuf[32]; rgcn_file_size_str(sent, sbuf, sizeof sbuf);
    char line[192];
    snprintf(line, sizeof line, "%s gönderiliyor: %s (%d%%)",
             a->filename, sbuf, pct);
    render_system(line);
}

static RGCN_THREAD_RET file_serve_thread(void *ud) {
    struct serve_args *a = (struct serve_args *)ud;
    int rc = rgcn_file_serve_once(a->listen_sock, a->path, a->offer_id,
                                  a->size, 120, serve_progress, a);
    char line[192];
    if (rc == 0) snprintf(line, sizeof line, "%s gönderildi.", a->filename);
    else         snprintf(line, sizeof line, "%s gönderilemedi.", a->filename);
    render_system(line);

    PEER_LOCK();
    for (int i = 0; i < g_sent_offer_count; i++) {
        if (g_sent_offers[i].offer_id == a->offer_id) {
            g_sent_offers[i] = g_sent_offers[--g_sent_offer_count];
            break;
        }
    }
    PEER_UNLOCK();
    free(a);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

struct download_args {
    uint32_t sender_ip_be;
    uint16_t sender_port_be;
    uint64_t offer_id;
    uint64_t size;
    uint8_t  sha256[32];
    char     dest_path[RGCN_FILE_MAX_PATH];
    char     filename[RGCN_FILE_MAX_NAME];
    char     sender_nick[RGCN_MAX_NICK];
};

static void download_progress(uint64_t got, uint64_t total, void *ud) {
    struct download_args *a = (struct download_args *)ud;
    static uint64_t last_report = 0;
    uint64_t now = rgcn_now_ms();
    if (now - last_report < 1000 && got < total) return;
    last_report = now;
    int pct = (int)((got * 100) / (total ? total : 1));
    char sbuf[32]; rgcn_file_size_str(got, sbuf, sizeof sbuf);
    char line[192];
    snprintf(line, sizeof line, "%s indiriliyor: %s (%d%%)",
             a->filename, sbuf, pct);
    render_system(line);
}

static RGCN_THREAD_RET file_download_thread(void *ud) {
    struct download_args *a = (struct download_args *)ud;
    int rc = rgcn_file_download(a->sender_ip_be, a->sender_port_be,
                                a->offer_id, a->size, a->sha256,
                                a->dest_path, download_progress, a);
    char line[512];
    if (rc == 0)      snprintf(line, sizeof line, "%s alındı → %s", a->filename, a->dest_path);
    else if (rc == -2) snprintf(line, sizeof line, "%s: SHA-256 uyuşmadı, reddedildi", a->filename);
    else              snprintf(line, sizeof line, "%s indirilemedi", a->filename);
    render_system(line);
    free(a);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void handle_command(const char *cmd) {
    if (strcmp(cmd, "/quit") == 0) {
        g_running = 0;
    } else if (strcmp(cmd, "/clear") == 0) {
        reset_screen();
    } else if (strncmp(cmd, "/timer", 6) == 0) {
        const char *arg = cmd + 6;
        while (*arg == ' ') arg++;
        if (*arg == 0) {
            if (g_timer_ms == 0) render_system("Timer: kapalı.");
            else {
                char buf[64];
                snprintf(buf, sizeof buf, "Timer: %u saniye (aktif).",
                         g_timer_ms / 1000);
                render_system(buf);
            }
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            g_timer_ms = 0;
            render_system("Timer kapatıldı.");
        } else {
            char *end = NULL;
            long v = strtol(arg, &end, 10);
            if (end == arg || v < 1 || v > 3600) {
                render_system("Kullanım: /timer <1-3600 saniye> | /timer off");
            } else {
                g_timer_ms = (uint32_t)(v * 1000);
                char buf[80];
                snprintf(buf, sizeof buf,
                         "Timer: %ld saniye. Sonraki mesajlar %ld saniyede silinecek.",
                         v, v);
                render_system(buf);
            }
        }
    } else if (strcmp(cmd, "/who") == 0) {
        uint64_t now = rgcn_now_ms();
        char line[512]; line[0] = 0;
        int off = 0;
        int count = 0;
        PEER_LOCK();
        for (int i = 0; i < g_peer_count; i++) {
            if (now - g_peers[i].last_ms > PEER_STALE_MS) continue;
            const char *c = rgcn_color_for(g_peers[i].nick);
            off += snprintf(line + off, sizeof line - off,
                            "%s%s%s%s", count ? ", " : "", c,
                            g_peers[i].nick, rgcn_color_reset());
            count++;
            if (off >= (int)sizeof line - 64) break;
        }
        PEER_UNLOCK();
        if (count == 0) render_system("Kanalda başka kimse yok.");
        else {
            char full[600];
            snprintf(full, sizeof full, "Kanaldakiler (%d): %s", count, line);
            render_system(full);
        }
    } else if (strcmp(cmd, "/status") == 0) {
        char line[128];
        snprintf(line, sizeof line, "Port %d dinleniyor, station=%016llX",
                 g_port, (unsigned long long)g_station);
        render_system(line);
        uint64_t now = rgcn_now_ms();
        int shown = 0;
        PEER_LOCK();
        for (int i = 0; i < g_peer_count; i++) {
            if (now - g_peers[i].last_ms > PEER_STALE_MS) continue;
            uint8_t  *ip_b = (uint8_t *)&g_peers[i].ip_be;
            uint8_t  *pt_b = (uint8_t *)&g_peers[i].port_be;
            uint16_t port_h = ((uint16_t)pt_b[0] << 8) | pt_b[1];
            const char *reach = g_peers[i].ip_be ? "unicast" : "broadcast";
            char buf[256];
            snprintf(buf, sizeof buf,
                     "  %s%s%s  %u.%u.%u.%u:%u  son:%llus  yol:%s",
                     rgcn_color_for(g_peers[i].nick),
                     g_peers[i].nick,
                     rgcn_color_reset(),
                     ip_b[0], ip_b[1], ip_b[2], ip_b[3],
                     port_h,
                     (unsigned long long)((now - g_peers[i].last_ms) / 1000),
                     reach);
            render_system(buf);
            shown++;
        }
        PEER_UNLOCK();
        if (shown == 0) render_system("  (henüz peer yok - HELLO paketi bekleniyor)");
    } else if (strncmp(cmd, "/send", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        const char *arg = cmd + 5;
        while (*arg == ' ') arg++;
        if (!*arg) { render_system("Kullanım: /send <dosya-yolu>  |  /send @rumuz <dosya-yolu>"); return; }
        /* Optional target: /send @nick /path */
        char target[RGCN_MAX_NICK] = {0};
        if (arg[0] == '@') {
            arg++;
            size_t k = 0;
            while (*arg && *arg != ' ' && k < RGCN_MAX_NICK - 1) target[k++] = *arg++;
            target[k] = 0;
            while (*arg == ' ') arg++;
        }
        if (!*arg) { render_system("Kullanım: /send @rumuz <dosya-yolu>"); return; }
        /* Resolve path relative to cwd */
        struct stat st;
        if (stat(arg, &st) != 0 || !S_ISREG(st.st_mode)) {
            render_system("Dosya bulunamadı."); return;
        }
        if ((uint64_t)st.st_size > RGCN_FILE_MAX_SIZE) {
            render_system("Dosya çok büyük (>50 MB)."); return;
        }
        if (g_sent_offer_count >= SENT_OFFER_SLOTS) {
            render_system("Zaten bekleyen çok dosya var, biraz bekle."); return;
        }

        /* SHA-256 the file */
        uint8_t sha[32];
        if (rgcn_sha256_file(arg, sha) != 0) {
            render_system("Dosya okunamadı."); return;
        }
        char sha_hex[65];
        hex_encode(sha, 32, sha_hex);

        /* Sanitize the on-wire filename (just basename, no path) */
        char clean_name[RGCN_FILE_MAX_NAME];
        if (rgcn_file_sanitize_name(arg, clean_name, sizeof clean_name) != 0) {
            render_system("Dosya adı geçersiz."); return;
        }

        /* Open TCP listener */
        int lsock;
        uint16_t lport;
        if (rgcn_file_open_listener(&lsock, &lport) != 0) {
            render_system("TCP portu açılamadı."); return;
        }

        /* Register outbound offer */
        uint64_t off_id = 0;
        rgcn_random_bytes((uint8_t *)&off_id, sizeof off_id);
        int idx = g_sent_offer_count++;
        g_sent_offers[idx].offer_id     = off_id;
        g_sent_offers[idx].listen_socket = lsock;
        g_sent_offers[idx].listen_port  = lport;
        strncpy(g_sent_offers[idx].path, arg, RGCN_FILE_MAX_PATH - 1);
        g_sent_offers[idx].path[RGCN_FILE_MAX_PATH - 1] = 0;
        g_sent_offers[idx].size         = (uint64_t)st.st_size;
        strncpy(g_sent_offers[idx].filename, clean_name, RGCN_FILE_MAX_NAME - 1);
        g_sent_offers[idx].filename[RGCN_FILE_MAX_NAME - 1] = 0;

        /* Spawn serve thread (waits for accept, handshake, streams file) */
        struct serve_args *sa = (struct serve_args *)calloc(1, sizeof *sa);
        sa->listen_sock = lsock;
        sa->offer_id    = off_id;
        sa->size        = (uint64_t)st.st_size;
        strncpy(sa->path, arg, RGCN_FILE_MAX_PATH - 1);
        strncpy(sa->filename, clean_name, RGCN_FILE_MAX_NAME - 1);
#ifdef _WIN32
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, file_serve_thread, sa, 0, NULL);
        if (th) CloseHandle(th);
#else
        pthread_t th; pthread_create(&th, NULL, file_serve_thread, sa);
        pthread_detach(th);
#endif

        /* Broadcast FILE_OFFER */
        uint64_t id = 0; rgcn_random_bytes((uint8_t *)&id, sizeof id);
        uint64_t ts = rgcn_now_ms();
        char plain[1024];
        int n = snprintf(plain, sizeof plain,
                         "FILE_OFFER|%016llX|%016llX|%llu|%s|%016llX|%s|%llu|%s|%u|%s",
                         (unsigned long long)g_station,
                         (unsigned long long)id,
                         (unsigned long long)ts,
                         g_nick,
                         (unsigned long long)off_id,
                         clean_name,
                         (unsigned long long)st.st_size,
                         sha_hex,
                         lport,
                         target);
        if (n > 0 && n < (int)sizeof plain) send_raw_plaintext(plain, n);

        char sbuf[32]; rgcn_file_size_str((uint64_t)st.st_size, sbuf, sizeof sbuf);
        char line[192];
        if (target[0]) snprintf(line, sizeof line, "%s (%s) → @%s teklif edildi.", clean_name, sbuf, target);
        else           snprintf(line, sizeof line, "%s (%s) kanala teklif edildi.", clean_name, sbuf);
        render_system(line);
    } else if (strncmp(cmd, "/accept", 7) == 0 && (cmd[7] == 0 || cmd[7] == ' ')) {
        const char *arg = cmd + 7;
        while (*arg == ' ') arg++;
        if (!*arg) { render_system("Kullanım: /accept <dosya-adı>"); return; }
        /* Find offer by filename */
        int idx = -1;
        for (int i = 0; i < g_offer_count; i++) {
            if (strcmp(g_offers[i].filename, arg) == 0) { idx = i; break; }
        }
        if (idx < 0) { render_system("Böyle bir teklif yok."); return; }

        /* Prepare destination path */
        char dir[RGCN_FILE_MAX_PATH];
        if (rgcn_file_download_dir(dir, sizeof dir) != 0) {
            render_system("~/Downloads/RigiconLive/ oluşturulamadı."); return;
        }
        char dest[RGCN_FILE_MAX_PATH];
#ifdef _WIN32
        snprintf(dest, sizeof dest, "%s\\%s", dir, g_offers[idx].filename);
#else
        snprintf(dest, sizeof dest, "%s/%s", dir, g_offers[idx].filename);
#endif

        struct download_args *da = (struct download_args *)calloc(1, sizeof *da);
        da->sender_ip_be   = g_offers[idx].sender_ip_be;
        da->sender_port_be = g_offers[idx].sender_port_be;
        da->offer_id       = g_offers[idx].offer_id;
        da->size           = g_offers[idx].size;
        memcpy(da->sha256, g_offers[idx].sha256, 32);
        strncpy(da->dest_path, dest, RGCN_FILE_MAX_PATH - 1);
        strncpy(da->filename, g_offers[idx].filename, RGCN_FILE_MAX_NAME - 1);
        strncpy(da->sender_nick, g_offers[idx].sender_nick, RGCN_MAX_NICK - 1);

        /* Send FILE_ACCEPT to sender */
        uint64_t id = 0; rgcn_random_bytes((uint8_t *)&id, sizeof id);
        uint64_t ts = rgcn_now_ms();
        char plain[256];
        int n = snprintf(plain, sizeof plain,
                         "FILE_ACCEPT|%016llX|%016llX|%llu|%s|%016llX",
                         (unsigned long long)g_station,
                         (unsigned long long)id,
                         (unsigned long long)ts,
                         g_nick,
                         (unsigned long long)g_offers[idx].offer_id);
        if (n > 0) send_raw_plaintext(plain, n);

        /* Remove offer from list */
        g_offers[idx] = g_offers[--g_offer_count];

        /* Spawn download thread */
#ifdef _WIN32
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, file_download_thread, da, 0, NULL);
        if (th) CloseHandle(th);
#else
        pthread_t th; pthread_create(&th, NULL, file_download_thread, da);
        pthread_detach(th);
#endif

        char line[192];
        snprintf(line, sizeof line, "%s indirmeye başlanıyor...", da->filename);
        render_system(line);
    } else if (strncmp(cmd, "/reject", 7) == 0 && (cmd[7] == 0 || cmd[7] == ' ')) {
        const char *arg = cmd + 7;
        while (*arg == ' ') arg++;
        if (!*arg) { render_system("Kullanım: /reject <dosya-adı>"); return; }
        int idx = -1;
        for (int i = 0; i < g_offer_count; i++) {
            if (strcmp(g_offers[i].filename, arg) == 0) { idx = i; break; }
        }
        if (idx < 0) { render_system("Böyle bir teklif yok."); return; }
        uint64_t id = 0; rgcn_random_bytes((uint8_t *)&id, sizeof id);
        uint64_t ts = rgcn_now_ms();
        char plain[256];
        int n = snprintf(plain, sizeof plain,
                         "FILE_REJECT|%016llX|%016llX|%llu|%s|%016llX",
                         (unsigned long long)g_station,
                         (unsigned long long)id,
                         (unsigned long long)ts,
                         g_nick,
                         (unsigned long long)g_offers[idx].offer_id);
        if (n > 0) send_raw_plaintext(plain, n);
        g_offers[idx] = g_offers[--g_offer_count];
        render_system("Reddedildi.");
    } else if (strcmp(cmd, "/help") == 0) {
        render_system("Komutlar:");
        render_system("  /quit                   çık");
        render_system("  /clear                  ekranı temizle");
        render_system("  /who                    kanaldakileri listele");
        render_system("  /status                 bağlantı durumu / peer IP'leri");
        render_system("  /timer <sn>             her mesaj sn saniyede silinsin (/timer off kapatır)");
        render_system("  /send <dosya>           dosya paylaş (max 50 MB)");
        render_system("  /send @rumuz <dosya>    sadece belirli kişiye paylaş");
        render_system("  /accept <dosya-adı>     gelen dosya teklifini kabul et");
        render_system("  /reject <dosya-adı>     gelen dosya teklifini reddet");
        render_system("  /help                   bu liste");
    }
    /* Unknown "/..." inputs never reach here - filtered by is_known_command()
     * in on_enter() and sent as regular chat instead. */
}

static int is_known_command(const char *text) {
    /* Multi-word commands: match on the first token only. */
    static const char *known[] = {
        "/quit", "/clear", "/who", "/status", "/help", "/timer",
        "/send", "/accept", "/reject", NULL
    };
    for (int i = 0; known[i]; i++) {
        size_t klen = strlen(known[i]);
        if (strncmp(text, known[i], klen) == 0 &&
            (text[klen] == 0 || text[klen] == ' ')) return 1;
    }
    return 0;
}

static void on_enter(void) {
    if (g_input_len == 0) return;

    char text[RGCN_MAX_TEXT];
    memcpy(text, g_input, g_input_len);
    text[g_input_len] = 0;
    g_input_len = 0;
    g_input[0]  = 0;

    /* Outgoing sanitize: strip control chars (defense in depth - our raw-mode
     * input already skips them, but pasted content can slip in escapes). */
    for (char *c = text; *c; c++) {
        unsigned char b = (unsigned char)*c;
        if (b < 0x20 && b != '\t') *c = ' ';
        if (b == 0x7f) *c = ' ';
    }
    /* Collapse runs of spaces to a single space + trim ends. Prevents
     * mass-pasted content from being wall-of-whitespace noise. */
    size_t r = 0, w = 0;
    int prev_sp = 1;
    while (text[r]) {
        if (text[r] == ' ') {
            if (!prev_sp) text[w++] = ' ';
            prev_sp = 1;
        } else {
            text[w++] = text[r];
            prev_sp = 0;
        }
        r++;
    }
    while (w > 0 && text[w-1] == ' ') w--;
    text[w] = 0;
    if (w == 0) return;   /* nothing to send after sanitize */

    /* Only route to command handler if this exactly matches a real command.
     * Anything else that starts with '/' (like "/idk" or "/whatever") is
     * treated as a regular chat message. */
    if (text[0] == '/' && is_known_command(text)) {
        rgcn_term_lock();
        printf("\r\x1b[K");
        fflush(stdout);
        rgcn_term_unlock();
        handle_command(text);
        return;
    }

    uint64_t ts = rgcn_now_ms();
    if (g_timer_ms > 0) {
        send_kind_ttl("CHAT", text, g_timer_ms);
        render_burn(g_nick, text, ts, g_timer_ms);
    } else {
        send_msg("CHAT", text);
        render_chat(g_nick, text, ts);
    }
}

/* -------------------------------------------------------------------------- */
/* Startup prompts                                                             */
/* -------------------------------------------------------------------------- */

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
}

static int prompt_nick(char *out) {
    while (1) {
        printf("Rumuz / İsim giriniz: ");
        fflush(stdout);
        char line[RGCN_MAX_NICK * 2];
        if (!fgets(line, sizeof line, stdin)) return -1;
        chomp(line);
        /* Nick may not contain '|' (our wire delimiter) or control chars. */
        int ok = 1;
        for (char *c = line; *c; c++) {
            if (*c == '|' || (unsigned char)*c < 0x20) { ok = 0; break; }
        }
        if (line[0] == 0 || !ok || strlen(line) >= RGCN_MAX_NICK) {
            printf("  ! Geçerli bir isim gir (boşluk yok, kontrol karakteri yok, '|' yok).\n");
            continue;
        }
        strncpy(out, line, RGCN_MAX_NICK - 1);
        out[RGCN_MAX_NICK - 1] = 0;
        return 0;
    }
}

static int prompt_port(int *out) {
    printf("Kanal / Port [Varsayılan: %d]: ", RGCN_DEFAULT_PORT);
    fflush(stdout);
    char line[32];
    if (!fgets(line, sizeof line, stdin)) return -1;
    chomp(line);
    if (line[0] == 0) { *out = RGCN_DEFAULT_PORT; return 0; }
    char *end = NULL;
    long v = strtol(line, &end, 10);
    if (end == line || v < RGCN_MIN_PORT || v > RGCN_MAX_PORT) {
        printf("  ! Geçersiz. %d kullanılacak.\n", RGCN_DEFAULT_PORT);
        *out = RGCN_DEFAULT_PORT;
        return 0;
    }
    *out = (int)v;
    return 0;
}

static int prompt_yes_no(const char *q, int def_yes) {
    printf("%s (%s): ", q, def_yes ? "E/h" : "e/H");
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return def_yes;
    chomp(line);
    if (line[0] == 0) return def_yes;
    char c = (char)tolower((unsigned char)line[0]);
    if (c == 'e' || c == 'y') return 1;
    if (c == 'h' || c == 'n') return 0;
    return def_yes;
}

static int parse_cli(int argc, char **argv, int *port_out, char *nick_out) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "--port") || !strcmp(a, "-p")) && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v >= RGCN_MIN_PORT && v <= RGCN_MAX_PORT) *port_out = (int)v;
        } else if ((!strcmp(a, "--nick") || !strcmp(a, "-n")) && i + 1 < argc) {
            strncpy(nick_out, argv[++i], RGCN_MAX_NICK - 1);
            nick_out[RGCN_MAX_NICK - 1] = 0;
        } else if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("%s %s (%s)\n", RGCN_APP_NAME, RGCN_VERSION, RGCN_COMPANY);
            return 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("Kullanım: %s [--nick <isim>] [--port <numara>]\n", argv[0]);
            printf("Varsayılan port: %d\n", RGCN_DEFAULT_PORT);
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* auto-reap notify helpers */
#endif

    /* Verify our AEAD implementation before touching the network. */
    if (rgcn_crypto_init() != 0) {
        fprintf(stderr, "fatal: kripto self-test başarısız\n");
        return 1;
    }

    int   cli_port = -1;
    char  cli_nick[RGCN_MAX_NICK] = {0};
    if (parse_cli(argc, argv, &cli_port, cli_nick)) return 0;

    /* Cooked-mode startup prompts (safe fgets while stdin is still line-buffered). */
    if (cli_nick[0]) {
        strncpy(g_nick, cli_nick, RGCN_MAX_NICK - 1);
    } else {
        if (prompt_nick(g_nick) != 0) return 1;
    }

    if (cli_port > 0) g_port = cli_port;
    else if (prompt_port(&g_port) != 0) return 1;

    g_notify_on = prompt_yes_no("Sistem bildirimleri alınsın mı?", 0);
    rgcn_notify_enable(g_notify_on);

    g_station = 0;
    rgcn_random_bytes((uint8_t *)&g_station, sizeof g_station);

    /* Open the radio channel. */
    char err[256];
    g_net = rgcn_net_open(g_port, err, sizeof err);
    if (!g_net) {
        fprintf(stderr, "Hata: %s\n", err);
        fprintf(stderr, "İpucu: aynı porta başka bir uygulama bağlıysa farklı bir port seç.\n");
        return 1;
    }

    signal(SIGINT, on_sigint);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#endif

    rgcn_term_init();
    render_banner(g_notify_on);
    print_prompt_unlocked();

    /* Announce ourselves. */
    send_msg("JOIN", NULL);

    /* Spawn receiver thread. */
#ifdef _WIN32
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, receiver_thread, NULL, 0, NULL);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, receiver_thread, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        rgcn_term_restore();
        rgcn_net_close(g_net);
        return 1;
    }
#endif

    /* Bracketed-paste state machine. When the terminal wraps a paste with
     * \e[200~ ... \e[201~ we collapse it into a single message (newlines
     * become spaces) so a multi-line paste doesn't turn into N sends. */
    int      esc_state  = 0;       /* 0=normal, 1=saw ESC, 2=in CSI */
    char     esc_buf[16]; int esc_len = 0;
    int      paste_mode = 0;
    static char paste_buf[RGCN_MAX_TEXT]; int paste_len = 0;

    /* Main input loop. */
    while (g_running) {
        uint8_t buf[8];
        int n = rgcn_term_read_char(buf, sizeof buf);
        if (n <= 0) continue;

        rgcn_term_lock();

        /* --- ESC sequence recognizer (for bracketed paste only) --- */
        if (esc_state == 1) {
            if (n == 1 && buf[0] == '[') { esc_state = 2; esc_len = 0; }
            else                          { esc_state = 0; }
            rgcn_term_unlock(); continue;
        }
        if (esc_state == 2) {
            if (n == 1) {
                if (esc_len < (int)sizeof esc_buf - 1) esc_buf[esc_len++] = (char)buf[0];
                /* CSI terminator = 0x40..0x7E */
                if (buf[0] >= 0x40 && buf[0] <= 0x7E) {
                    esc_buf[esc_len] = 0;
                    if (strcmp(esc_buf, "200~") == 0) {
                        paste_mode = 1; paste_len = 0;
                    } else if (strcmp(esc_buf, "201~") == 0 && paste_mode) {
                        paste_mode = 0;
                        if (paste_len > 0) {
                            /* Append pasted chunk to current input, newline→space. */
                            for (int i = 0; i < paste_len; i++) {
                                unsigned char c = (unsigned char)paste_buf[i];
                                if (c == '\r' || c == '\n' || c == '\t') paste_buf[i] = ' ';
                                if (c < 0x20 || c == 0x7f) paste_buf[i] = ' ';
                            }
                            size_t room = sizeof g_input - 1 - g_input_len;
                            size_t take = (size_t)paste_len < room ? (size_t)paste_len : room;
                            memcpy(g_input + g_input_len, paste_buf, take);
                            g_input_len += take;
                            g_input[g_input_len] = 0;
                            print_prompt_unlocked();
                        }
                    }
                    esc_state = 0;
                }
            }
            rgcn_term_unlock(); continue;
        }
        if (n == 1 && buf[0] == 0x1B) {
            esc_state = 1;
            rgcn_term_unlock(); continue;
        }

        /* --- Inside a paste: buffer everything until \e[201~ arrives --- */
        if (paste_mode) {
            if (paste_len + n < (int)sizeof paste_buf) {
                memcpy(paste_buf + paste_len, buf, n);
                paste_len += n;
            }
            rgcn_term_unlock(); continue;
        }

        /* --- Normal typed input --- */
        if (n == 1 && buf[0] == 0x03) {           /* Ctrl+C */
            g_running = 0;
            rgcn_term_unlock();
            break;
        } else if (n == 1 && (buf[0] == '\r' || buf[0] == '\n')) {
            rgcn_term_unlock();
            on_enter();
            rgcn_term_lock();
        } else if (n == 1 && (buf[0] == 0x7f || buf[0] == 0x08)) {
            input_backspace();
            print_prompt_unlocked();
        } else if (n == 1 && buf[0] == 0x0c) {    /* Ctrl+L */
            rgcn_term_clear_screen();
            render_banner(g_notify_on);
            print_prompt_unlocked();
        } else if (n == 1 && buf[0] < 0x20) {
            /* Ignore other control chars. */
        } else {
            if (g_input_len + n < sizeof g_input) {
                memcpy(g_input + g_input_len, buf, n);
                g_input_len += n;
                g_input[g_input_len] = 0;
                fwrite(buf, 1, n, stdout);
                fflush(stdout);
            }
        }
        rgcn_term_unlock();
    }

    /* Broadcast LEAVE, then clean up. */
    send_msg("LEAVE", NULL);

    rgcn_term_lock();
    printf("\r\x1b[K%s* Kanal kapatıldı. Görüşürüz, %s.%s\n",
           rgcn_color_system(), g_nick, rgcn_color_reset());
    rgcn_term_unlock();

    rgcn_term_restore();
    rgcn_net_close(g_net);

#ifdef _WIN32
    WaitForSingleObject(th, 1000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    return 0;
}
