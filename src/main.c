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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>

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

#define SEEN_CAP 512
static uint64_t g_seen[SEEN_CAP];
static int      g_seen_pos = 0;

#define PEERS_CAP 64
static struct { char nick[RGCN_MAX_NICK]; uint64_t last_ms; } g_peers[PEERS_CAP];
static int g_peer_count = 0;

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

static void peer_touch(const char *nick) {
    uint64_t now = rgcn_now_ms();
    for (int i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].nick, nick) == 0) {
            g_peers[i].last_ms = now;
            return;
        }
    }
    if (g_peer_count < PEERS_CAP) {
        strncpy(g_peers[g_peer_count].nick, nick, RGCN_MAX_NICK - 1);
        g_peers[g_peer_count].nick[RGCN_MAX_NICK - 1] = 0;
        g_peers[g_peer_count].last_ms = now;
        g_peer_count++;
    }
}

static void peer_drop(const char *nick) {
    for (int i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].nick, nick) == 0) {
            g_peers[i] = g_peers[g_peer_count - 1];
            g_peer_count--;
            return;
        }
    }
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
    const char *r = rgcn_color_reset();
    printf("\r\x1b[K%s%s%s ▸ %s", c, g_nick, rgcn_color_gray(), r);
    /* Reprint any input the user was typing. */
    if (g_input_len > 0) fwrite(g_input, 1, g_input_len, stdout);
    fflush(stdout);
}

static void write_line(const char *line) {
    rgcn_term_lock();
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

static void render_system(const char *text) {
    char tbuf[16]; time_hms(tbuf, sizeof tbuf, rgcn_now_ms());
    char line[512];
    snprintf(line, sizeof line, "%s[%s]%s %s* %s%s",
             rgcn_color_gray(), tbuf, rgcn_color_reset(),
             rgcn_color_system(), text, rgcn_color_reset());
    write_line(line);
}

static void render_banner(int notify_on) {
    const char *gray = rgcn_color_gray();
    const char *rst  = rgcn_color_reset();
    const char *me   = rgcn_color_for(g_nick);
    rgcn_term_clear_screen();
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  \x1b[1m\x1b[38;5;39mR I G I C O N   L I V E\x1b[0m   %s· Rigicon Inc.%s\n", gray, rst);
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  %sRumuz    :%s %s%s%s\n", gray, rst, me, g_nick, rst);
    printf("  %sKanal    :%s %d\n", gray, rst, g_port);
    printf("  %sBildirim :%s %s\n", gray, rst, notify_on ? "Açık" : "Kapalı");
    printf("  %sŞifreleme:%s ChaCha20-Poly1305 (AEAD, RFC 8439)\n", gray, rst);
    printf("  %sİz       :%s Sıfır. Kapanınca her şey gider.\n", gray, rst);
    printf("%s%s%s\n", gray, "═══════════════════════════════════════════════════════════", rst);
    printf("  %sKomutlar: /quit  /clear  /who  /help    Enter ile gönder%s\n", gray, rst);
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

static void send_msg(const char *kind, const char *text) {
    uint64_t id = 0;
    rgcn_random_bytes((uint8_t *)&id, sizeof id);
    uint64_t ts = rgcn_now_ms();

    char plain[RGCN_MAX_TEXT + 128];
    int n;
    if (text) {
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
    rgcn_net_broadcast(g_net, pkt, pkt_len);
}

static char *split(char **s) {
    if (!*s) return NULL;
    char *start = *s;
    char *bar = strchr(start, '|');
    if (bar) { *bar = 0; *s = bar + 1; }
    else     { *s = NULL; }
    return start;
}

static void handle_decoded(const char *raw, size_t raw_len) {
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

    if (strcmp(kind, "CHAT") == 0) {
        const char *text = p ? p : "";
        peer_touch(nick);
        render_chat(nick, text, (uint64_t)ts);
        char nbody[RGCN_MAX_TEXT + 64];
        snprintf(nbody, sizeof nbody, "%s: %s", nick, text);
        rgcn_notify(RGCN_APP_NAME, nbody);
    } else if (strcmp(kind, "JOIN") == 0) {
        peer_touch(nick);
        char line[128];
        snprintf(line, sizeof line, "%s kanala katıldı", nick);
        render_system(line);
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

static RGCN_THREAD_RET receiver_thread(void *arg) {
    (void)arg;
    uint8_t pkt[RGCN_MAX_PACKET];
    uint8_t plain[RGCN_MAX_PACKET];
    while (g_running) {
        int r = rgcn_net_recv(g_net, pkt, sizeof pkt, 500);
        if (r <= 0) continue;
        size_t plain_len = 0;
        if (rgcn_open(pkt, (size_t)r, plain, sizeof plain, &plain_len) != 0) continue;
        handle_decoded((const char *)plain, plain_len);
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

static void handle_command(const char *cmd) {
    if (strcmp(cmd, "/quit") == 0) {
        g_running = 0;
    } else if (strcmp(cmd, "/clear") == 0) {
        rgcn_term_clear_screen();
        render_banner(g_notify_on);
        rgcn_term_lock();
        print_prompt_unlocked();
        rgcn_term_unlock();
    } else if (strcmp(cmd, "/who") == 0) {
        uint64_t now = rgcn_now_ms();
        char line[512]; line[0] = 0;
        int off = 0;
        int count = 0;
        for (int i = 0; i < g_peer_count; i++) {
            if (now - g_peers[i].last_ms > 60000) continue;
            const char *c = rgcn_color_for(g_peers[i].nick);
            off += snprintf(line + off, sizeof line - off,
                            "%s%s%s%s", count ? ", " : "", c,
                            g_peers[i].nick, rgcn_color_reset());
            count++;
            if (off >= (int)sizeof line - 64) break;
        }
        if (count == 0) render_system("Kanalda başka kimse yok.");
        else {
            char full[600];
            snprintf(full, sizeof full, "Kanaldakiler (%d): %s", count, line);
            render_system(full);
        }
    } else if (strcmp(cmd, "/help") == 0) {
        render_system("Komutlar: /quit  /clear  /who  /help");
    } else {
        render_system("Bilinmeyen komut. /help yaz.");
    }
}

static void on_enter(void) {
    if (g_input_len == 0) return;

    char text[RGCN_MAX_TEXT];
    memcpy(text, g_input, g_input_len);
    text[g_input_len] = 0;
    g_input_len = 0;
    g_input[0]  = 0;

    if (text[0] == '/') {
        rgcn_term_lock();
        printf("\r\x1b[K");
        fflush(stdout);
        rgcn_term_unlock();
        handle_command(text);
        return;
    }

    uint64_t ts = rgcn_now_ms();
    send_msg("CHAT", text);
    render_chat(g_nick, text, ts);
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

    /* Main input loop. */
    while (g_running) {
        uint8_t buf[8];
        int n = rgcn_term_read_char(buf, sizeof buf);
        if (n <= 0) continue;

        rgcn_term_lock();
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
