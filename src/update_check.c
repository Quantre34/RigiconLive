/*
 * Rigicon Live - background "newer release available?" check.
 * See update_check.h for the design rationale.
 */

#include "update_check.h"
#include "rgcn.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  #define popen  _popen
  #define pclose _pclose
#else
  #include <pthread.h>
#endif

/* Implemented in main.c - a generic "something changed, repaint" bridge
 * (see game.h for rgcn_main_redraw's full-redraw cousin; this one is for
 * triggering that redraw from a thread with no other reason to fire). */
extern void rgcn_main_request_redraw(void);

static volatile int g_available = 0;
static char g_latest[32] = {0};

static int parse_version(const char *s, int out[3]) {
    return sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

static int is_newer(const char *latest, const char *current) {
    int a[3], b[3];
    if (!parse_version(latest, a) || !parse_version(current, b)) return 0;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 0;
}

static int extract_tag(const char *json, char *out, size_t out_cap) {
    /* GitHub's API pretty-prints with a space after the colon
     * ("tag_name": "v1.6.3"), not the compact "tag_name":"v1.6.3" - skip
     * any mix of ':'/whitespace rather than assuming either form. */
    const char *p = strstr(json, "\"tag_name\"");
    if (!p) return 0;
    p += strlen("\"tag_name\"");
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    if (*p == 'v' || *p == 'V') p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_cap) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static void run_check(void) {
    /* Short timeout so a dead/slow network never hangs this thread for
     * long. curl is assumed present - install.sh/install.ps1 already
     * require it, and Windows 10 1803+/11 ship curl.exe by default. If
     * it's missing, the read below just comes back empty and this gives
     * up silently, same shape as notify.c's terminal-notifier fallback. */
    FILE *fp = popen(
        "curl -fsSL --max-time 4 "
        "\"https://api.github.com/repos/Quantre34/RigiconLive/releases/latest\" 2>/dev/null",
        "r");
    if (!fp) return;

    char buf[16384];
    size_t total = 0, n;
    while (total + 1 < sizeof buf &&
           (n = fread(buf + total, 1, sizeof buf - 1 - total, fp)) > 0) {
        total += n;
    }
    buf[total] = 0;
    /* pclose()'s own waitpid() can come back -1/ECHILD here because
     * main() sets SIGCHLD=SIG_IGN (for notify.c's fire-and-forget
     * children) - the kernel auto-reaps this child regardless, so
     * there's no zombie/leak, just nothing meaningful to check. The
     * data we already read into buf is unaffected either way. */
    pclose(fp);
    if (total == 0) return;

    char tag[32];
    if (!extract_tag(buf, tag, sizeof tag)) return;
    if (!is_newer(tag, RGCN_VERSION)) return;

    strncpy(g_latest, tag, sizeof g_latest - 1);
    g_latest[sizeof g_latest - 1] = 0;
    g_available = 1;
    rgcn_main_request_redraw();
}

#ifdef _WIN32
static unsigned __stdcall thread_main(void *arg) { (void)arg; run_check(); return 0; }
#else
static void *thread_main(void *arg) { (void)arg; run_check(); return NULL; }
#endif

void rgcn_update_check_async(void) {
#ifdef _WIN32
    _beginthreadex(NULL, 0, thread_main, NULL, 0, NULL);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, thread_main, NULL) == 0) pthread_detach(th);
#endif
}

int rgcn_update_available(char *out, size_t out_cap) {
    if (!g_available) return 0;
    if (out && out_cap) {
        strncpy(out, g_latest, out_cap - 1);
        out[out_cap - 1] = 0;
    }
    return 1;
}
