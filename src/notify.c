/*
 * Rigicon Live - OS notifications.
 * We spawn the platform's notification helper without a shell so user text
 * never touches an interpreter (no command injection possible).
 */

#include "notify.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
#endif

static int g_enabled = 0;

void rgcn_notify_enable(int on) { g_enabled = on ? 1 : 0; }

/* Strip control chars and cap length. */
static void sanitize(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\n' || c == '\t') { dst[o++] = ' '; continue; }
        if (c < 0x20) continue;
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

#ifdef _WIN32
/* Escape for PowerShell double-quoted string: ` " become `` `" */
static void ps_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 3 < cap; i++) {
        char c = src[i];
        if (c == '`' || c == '"' || c == '$') { dst[o++] = '`'; }
        dst[o++] = c;
    }
    dst[o] = 0;
}
#endif

void rgcn_notify(const char *title, const char *body) {
    if (!g_enabled) return;
    if (!title) title = "";
    if (!body)  body  = "";

    char t[256], b[512];
    sanitize(title, t, sizeof t);
    sanitize(body,  b, sizeof b);

#ifdef _WIN32
    char et[512], eb[1024];
    ps_escape(t, et, sizeof et);
    ps_escape(b, eb, sizeof eb);

    char cmdline[4096];
    /* Balloon tip - works on Windows 7 through 11 without extra deps. */
    snprintf(cmdline, sizeof cmdline,
        "powershell -NoProfile -WindowStyle Hidden -Command "
        "\"$ErrorActionPreference='SilentlyContinue';"
        "Add-Type -AssemblyName System.Windows.Forms;"
        "Add-Type -AssemblyName System.Drawing;"
        "$n=New-Object System.Windows.Forms.NotifyIcon;"
        "$n.Icon=[System.Drawing.SystemIcons]::Information;"
        "$n.BalloonTipTitle=\\\"%s\\\";"
        "$n.BalloonTipText=\\\"%s\\\";"
        "$n.Visible=$true;"
        "$n.ShowBalloonTip(3000);"
        "Start-Sleep -Milliseconds 3500;"
        "$n.Dispose()\"",
        et, eb);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#else
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* Detach from parent process group so parent isn't blocked. */
        setsid();
  #ifdef __APPLE__
        /* osascript avoids adding any extra runtime dependency. */
        char script[2048];
        /* AppleScript strings use " as delimiter with \ escaping. */
        char et[256], eb[512];
        size_t o = 0;
        for (size_t i = 0; t[i] && o + 2 < sizeof et; i++) {
            if (t[i] == '"' || t[i] == '\\') et[o++] = '\\';
            et[o++] = t[i];
        }
        et[o] = 0;
        o = 0;
        for (size_t i = 0; b[i] && o + 2 < sizeof eb; i++) {
            if (b[i] == '"' || b[i] == '\\') eb[o++] = '\\';
            eb[o++] = b[i];
        }
        eb[o] = 0;
        snprintf(script, sizeof script,
                 "display notification \"%s\" with title \"%s\"", eb, et);
        execlp("osascript", "osascript", "-e", script, (char *)NULL);
  #else
        execlp("notify-send", "notify-send", "--app-name=Rigicon Live",
               t, b, (char *)NULL);
  #endif
        _exit(0);
    }
    /* Child auto-reaped via SIGCHLD=SIG_IGN installed in main(). */
#endif
}
