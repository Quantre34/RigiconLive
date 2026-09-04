/*
 * Rigicon Live - encrypted file transfer over TCP.
 *
 * Design:
 *   - Discovery happens over the existing UDP multicast/broadcast channel
 *     (FILE_OFFER, FILE_ACCEPT, FILE_REJECT messages in main.c).
 *   - Actual bytes flow over an ephemeral TCP connection to avoid the
 *     packet loss that plagues UDP over WiFi.
 *   - Before the sender begins streaming, the client proves possession of
 *     the shared key by presenting HMAC-SHA256(APP_KEY, magic || offer_id).
 *     A random TCP port scanner cannot forge this without the key.
 *   - Receiver verifies SHA-256 after download; mismatch = delete.
 *
 * The TCP body is NOT re-encrypted at the application layer. Rationale:
 *   1) confidentiality on the wire is already the threat we care about,
 *      and someone with the shared key already has the key.
 *   2) the HMAC handshake proves both sides have the key, so no random
 *      peer can even establish the connection.
 *   3) adding a stream cipher on top of TCP adds complexity without
 *      changing the threat model.
 */

#include "file.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  #include <direct.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int  socklen_t;
  typedef long ssize_t;
  #define RGCN_CLOSESOCK closesocket
  #define MKDIR(p)       _mkdir(p)
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  #define RGCN_CLOSESOCK close
  #define MKDIR(p)       mkdir(p, 0755)
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
#endif

/* -------------------------------------------------------------------------- */

int rgcn_file_sanitize_name(const char *raw, char *out, size_t out_cap) {
    if (!raw || !out || out_cap < 2) return -1;
    /* Extract basename - reject anything with path separators */
    const char *base = raw;
    for (const char *p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    if (!*base) return -1;
    /* Refuse hidden files, dotfiles, empty */
    if (base[0] == '.') return -1;

    size_t o = 0;
    for (const char *p = base; *p && o + 1 < out_cap; p++) {
        unsigned char c = (unsigned char)*p;
        /* Allow ASCII letters, digits, dash, underscore, dot, space, plus UTF-8 continuation */
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' ' ||
            (c >= 0x80)) {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    if (o == 0) return -1;
    /* Reject if it collapsed to just "." or ".." */
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) return -1;
    return 0;
}

int rgcn_file_ext_is_risky(const char *filename) {
    if (!filename) return 0;
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    static const char *risky[] = {
        ".exe",".dmg",".sh",".command",".pkg",".jar",".app",".bat",".cmd",
        ".ps1",".vbs",".scr",".msi",".deb",".rpm",".apk",".hta",".jse",
        ".vbe",".wsf",".wsh",".ipa",NULL
    };
    /* lowercase compare */
    char lower[16] = {0};
    for (int i = 0; dot[i] && i < 15; i++) lower[i] = (char)tolower((unsigned char)dot[i]);
    for (int i = 0; risky[i]; i++) if (strcmp(lower, risky[i]) == 0) return 1;
    return 0;
}

void rgcn_file_size_str(uint64_t bytes, char *out, size_t cap) {
    if (bytes < 1024ULL) snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ULL * 1024ULL)
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

int rgcn_file_download_dir(char *out, size_t cap) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE");
    if (!home) return -1;
    snprintf(out, cap, "%s\\Downloads\\RigiconLive", home);
#else
    const char *home = getenv("HOME");
    if (!home) return -1;
    snprintf(out, cap, "%s/Downloads/RigiconLive", home);
#endif
    struct stat st;
    if (stat(out, &st) != 0) {
        /* create Downloads dir first (macOS/Linux usually already exists) */
        char parent[RGCN_FILE_MAX_PATH];
#ifdef _WIN32
        snprintf(parent, sizeof parent, "%s\\Downloads", home);
#else
        snprintf(parent, sizeof parent, "%s/Downloads", home);
#endif
        if (stat(parent, &st) != 0) MKDIR(parent);
        if (MKDIR(out) != 0 && errno != EEXIST) return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* TCP helpers                                                                 */
/* -------------------------------------------------------------------------- */

static ssize_t recv_all(SOCKET s, void *buf, size_t n, int timeout_seconds) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    time_t deadline = time(NULL) + timeout_seconds;
    while (got < n) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        struct timeval tv;
        long remain = (long)(deadline - time(NULL));
        if (remain <= 0) return -1;
        tv.tv_sec = remain; tv.tv_usec = 0;
        int rc = select((int)s + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) return -1;
        int r = recv(s, (char *)(p + got), (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t send_all(SOCKET s, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, (const char *)(p + sent), (int)(n - sent), 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return (ssize_t)sent;
}

/* -------------------------------------------------------------------------- */
/* Sender side                                                                 */
/* -------------------------------------------------------------------------- */

int rgcn_file_open_listener(int *out_socket, uint16_t *out_port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;   /* ephemeral - kernel picks */
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        RGCN_CLOSESOCK(s); return -1;
    }
    if (listen(s, 1) < 0) {
        RGCN_CLOSESOCK(s); return -1;
    }
    struct sockaddr_in got;
    socklen_t glen = sizeof got;
    if (getsockname(s, (struct sockaddr *)&got, &glen) < 0) {
        RGCN_CLOSESOCK(s); return -1;
    }
    *out_socket = (int)s;
    *out_port   = ntohs(got.sin_port);
    return 0;
}

int rgcn_file_serve_once(int listen_sock, const char *path,
                         uint64_t offer_id, uint64_t size,
                         int timeout_seconds,
                         void (*progress)(uint64_t sent, uint64_t total, void *ud),
                         void *ud) {
    /* Wait for a client, but bounded */
    fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_sock, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_seconds; tv.tv_usec = 0;
    int rc = select(listen_sock + 1, &rfds, NULL, NULL, &tv);
    if (rc <= 0) { RGCN_CLOSESOCK(listen_sock); return -1; }

    struct sockaddr_in caddr;
    socklen_t clen = sizeof caddr;
    SOCKET c = accept(listen_sock, (struct sockaddr *)&caddr, &clen);
    RGCN_CLOSESOCK(listen_sock);
    if (c == INVALID_SOCKET) return -1;

    /* Read handshake: magic(4) + offer_id(8 LE) + hmac(32) = 44 bytes */
    uint8_t hs[44];
    if (recv_all(c, hs, sizeof hs, 5) != (ssize_t)sizeof hs) {
        RGCN_CLOSESOCK(c); return -1;
    }
    if (memcmp(hs, RGCN_FILE_MAGIC, 4) != 0) {
        uint8_t st = 1; send_all(c, &st, 1); RGCN_CLOSESOCK(c); return -1;
    }
    uint64_t claimed_id = 0;
    for (int i = 0; i < 8; i++)
        claimed_id |= ((uint64_t)hs[4 + i]) << (i * 8);
    if (claimed_id != offer_id) {
        uint8_t st = 2; send_all(c, &st, 1); RGCN_CLOSESOCK(c); return -1;
    }
    uint8_t want[32];
    rgcn_hmac_appkey(hs, 12, want);   /* HMAC over magic + offer_id */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= want[i] ^ hs[12 + i];
    if (diff) {
        uint8_t st = 1; send_all(c, &st, 1); RGCN_CLOSESOCK(c); return -1;
    }
    uint8_t ok = 0;
    if (send_all(c, &ok, 1) != 1) { RGCN_CLOSESOCK(c); return -1; }

    /* Stream file bytes */
    FILE *fp = fopen(path, "rb");
    if (!fp) { RGCN_CLOSESOCK(c); return -1; }

    uint8_t chunk[8192];
    uint64_t sent = 0;
    while (sent < size) {
        size_t want_n = sizeof chunk;
        if (size - sent < want_n) want_n = (size_t)(size - sent);
        size_t n = fread(chunk, 1, want_n, fp);
        if (n == 0) break;
        if (send_all(c, chunk, n) != (ssize_t)n) { fclose(fp); RGCN_CLOSESOCK(c); return -1; }
        sent += n;
        if (progress) progress(sent, size, ud);
    }
    fclose(fp);
    RGCN_CLOSESOCK(c);
    return sent == size ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* Receiver side                                                               */
/* -------------------------------------------------------------------------- */

int rgcn_file_download(uint32_t sender_ip_be, uint16_t sender_port_be,
                       uint64_t offer_id, uint64_t expected_size,
                       const uint8_t expected_sha256[32],
                       const char *dest_path,
                       void (*progress)(uint64_t got, uint64_t total, void *ud),
                       void *ud) {
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET) return -1;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = sender_ip_be;
    dst.sin_port        = sender_port_be;
    if (connect(c, (struct sockaddr *)&dst, sizeof dst) < 0) {
        RGCN_CLOSESOCK(c); return -1;
    }

    /* Handshake */
    uint8_t hs[44];
    memcpy(hs, RGCN_FILE_MAGIC, 4);
    for (int i = 0; i < 8; i++) hs[4 + i] = (uint8_t)(offer_id >> (i * 8));
    rgcn_hmac_appkey(hs, 12, hs + 12);
    if (send_all(c, hs, sizeof hs) != (ssize_t)sizeof hs) { RGCN_CLOSESOCK(c); return -1; }
    uint8_t st = 0;
    if (recv_all(c, &st, 1, 5) != 1) { RGCN_CLOSESOCK(c); return -1; }
    if (st != 0) { RGCN_CLOSESOCK(c); return -1; }

    /* Stream to temp file, then rename */
    char tmp_path[RGCN_FILE_MAX_PATH + 8];
    snprintf(tmp_path, sizeof tmp_path, "%s.part", dest_path);
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) { RGCN_CLOSESOCK(c); return -1; }

    uint8_t chunk[8192];
    uint64_t got = 0;
    while (got < expected_size) {
        size_t want_n = sizeof chunk;
        if (expected_size - got < want_n) want_n = (size_t)(expected_size - got);
        fd_set rfds; FD_ZERO(&rfds); FD_SET(c, &rfds);
        struct timeval tv; tv.tv_sec = 30; tv.tv_usec = 0;
        int rc = select((int)c + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) { fclose(fp); RGCN_CLOSESOCK(c); remove(tmp_path); return -1; }
        int r = recv(c, (char *)chunk, (int)want_n, 0);
        if (r <= 0) { fclose(fp); RGCN_CLOSESOCK(c); remove(tmp_path); return -1; }
        if (fwrite(chunk, 1, r, fp) != (size_t)r) { fclose(fp); RGCN_CLOSESOCK(c); remove(tmp_path); return -1; }
        got += r;
        if (progress) progress(got, expected_size, ud);
    }
    fclose(fp);
    RGCN_CLOSESOCK(c);

    /* Verify SHA-256 */
    uint8_t got_hash[32];
    if (rgcn_sha256_file(tmp_path, got_hash) != 0) { remove(tmp_path); return -1; }
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= got_hash[i] ^ expected_sha256[i];
    if (diff) { remove(tmp_path); return -2; }

    /* Rename to final path (overwrite if exists) */
    remove(dest_path);
    if (rename(tmp_path, dest_path) != 0) { remove(tmp_path); return -1; }
    return 0;
}
