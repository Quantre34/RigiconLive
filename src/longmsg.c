/*
 * Rigicon Live - "/longmsg" mode + chunked long-message delivery.
 * See longmsg.h for the overall design.
 */

#include "longmsg.h"
#include "rgcn.h"
#include "crypto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Implemented in main.c - generic UI/network bridges (see src/game.h for
 * the authoritative doc comments on the first three). */
extern void        rgcn_main_system(const char *text);
extern void        rgcn_main_send(const char *kind, const char *text);
extern const char *rgcn_main_nick(void);
extern void        rgcn_main_chat_send(const char *text);
extern void        rgcn_main_chat_header(const char *nick, uint64_t ts);
extern void        rgcn_main_write_raw_line(const char *line);

#define LONGMSG_CHUNK_MAX 1000
#define LONGMSG_MAX_BYTES RGCN_LONGMSG_MAX_BYTES
#define LONGMSG_MAX_CHUNKS 32
#define LONGMSG_SLOTS 4
#define LONGMSG_TIMEOUT_MS 6000

static int g_enabled = 0;

void rgcn_longmsg_cmd(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        char buf[64];
        snprintf(buf, sizeof buf, "Uzun mesaj modu: %s", g_enabled ? "açık" : "kapalı");
        rgcn_main_system(buf);
        rgcn_main_system("Aç: /longmsg on   |   Kapat: /longmsg off");
        return;
    }
    if (strcmp(args, "on") == 0) {
        g_enabled = 1;
        rgcn_main_system("Uzun mesaj modu açık: çok satırlı yapıştırmalar tek mesaj olarak gönderilir (gerekirse parçalanıp birleştirilir).");
    } else if (strcmp(args, "off") == 0) {
        g_enabled = 0;
        rgcn_main_system("Uzun mesaj modu kapalı: çok satırlı yapıştırmalar satır satır ayrı mesaj olarak gönderilir (varsayılan).");
    } else {
        rgcn_main_system("Kullanım: /longmsg on | /longmsg off");
    }
}

/* ---- sending ------------------------------------------------------------ */

static void send_per_line(const char *text, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > start) {
                char line[RGCN_MAX_TEXT];
                size_t n = i - start;
                if (n > sizeof line - 1) n = sizeof line - 1;
                memcpy(line, text + start, n);
                line[n] = 0;
                /* rgcn_main_chat_send() (not on_enter()'s command path) -
                 * a pasted line that happens to start with '/' must never
                 * execute as a command, only ever be sent as literal
                 * chat text, rendered exactly like any normal message. */
                rgcn_main_chat_send(line);
            }
            start = i + 1;
        }
    }
}

static void echo_multiline_local(const char *text, size_t len) {
    rgcn_main_chat_header(rgcn_main_nick(), rgcn_now_ms());
    char line[RGCN_MAX_TEXT];
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t n = i - start;
            if (n > sizeof line - 1) n = sizeof line - 1;
            memcpy(line, text + start, n);
            line[n] = 0;
            rgcn_main_write_raw_line(line);
            start = i + 1;
        }
    }
}

void rgcn_longmsg_send_paste(const char *text, size_t len) {
    if (len == 0) return;

    if (!g_enabled) {
        send_per_line(text, len);
        return;
    }

    if (len > LONGMSG_MAX_BYTES) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "Yapıştırılan içerik çok büyük (%zu bayt, sınır %d). "
                 "Büyük içerik için /send kullan.", len, LONGMSG_MAX_BYTES);
        rgcn_main_system(buf);
        return;
    }

    if (len <= (size_t)(RGCN_MAX_TEXT - 1)) {
        /* Fits one packet - ride the plain CHAT kind with real '\n' bytes
         * inside the text field (main.c's CHAT sanitizer now lets '\n'
         * through for exactly this). */
        char text_buf[RGCN_MAX_TEXT];
        memcpy(text_buf, text, len);
        text_buf[len] = 0;
        rgcn_main_send("CHAT", text_buf);
        echo_multiline_local(text, len);
        return;
    }

    /* Too big for one packet - split along line boundaries (never mid-
     * line) into CHATL_PART chunks under one shared msg_id. */
    int total = 0;
    size_t starts[LONGMSG_MAX_CHUNKS], lens[LONGMSG_MAX_CHUNKS];
    size_t pos = 0;
    while (pos < len && total < LONGMSG_MAX_CHUNKS) {
        size_t chunk_start = pos;
        size_t chunk_end = pos;
        for (;;) {
            size_t next_nl = chunk_end;
            while (next_nl < len && text[next_nl] != '\n') next_nl++;
            size_t candidate_end = (next_nl < len) ? next_nl + 1 : len;
            if (candidate_end - chunk_start > LONGMSG_CHUNK_MAX) {
                if (chunk_end == chunk_start) chunk_end = candidate_end; /* one line alone exceeds the cap - take it anyway */
                break;
            }
            chunk_end = candidate_end;
            if (chunk_end == len) break;
        }
        starts[total] = chunk_start;
        lens[total]   = chunk_end - chunk_start;
        total++;
        pos = chunk_end;
    }
    if (pos < len) {
        rgcn_main_system("Yapıştırılan içerik çok parçalı, gönderilemedi.");
        return;
    }

    uint64_t msg_id = rgcn_random_u64();
    for (int i = 0; i < total; i++) {
        char chunk_buf[LONGMSG_CHUNK_MAX + 8];
        size_t n = lens[i];
        if (n > sizeof chunk_buf - 1) n = sizeof chunk_buf - 1;
        memcpy(chunk_buf, text + starts[i], n);
        chunk_buf[n] = 0;
        char field[LONGMSG_CHUNK_MAX + 64];
        snprintf(field, sizeof field, "%016llX|%d|%d|%s",
                 (unsigned long long)msg_id, i, total, chunk_buf);
        rgcn_main_send("CHATL_PART", field);
    }
    echo_multiline_local(text, len);
}

/* ---- receiving / reassembly ---------------------------------------------- */

typedef struct {
    int      active;
    uint64_t msg_id;
    char     nick[RGCN_MAX_NICK];
    uint64_t ts;
    int      total;
    uint32_t got_mask;
    uint16_t chunk_len[LONGMSG_MAX_CHUNKS];
    char     chunk_data[LONGMSG_MAX_CHUNKS][LONGMSG_CHUNK_MAX + 1];
    uint64_t first_seen_ms;
} rx_slot_t;

static rx_slot_t g_slots[LONGMSG_SLOTS];

static uint32_t full_mask(int total) {
    return total >= 32 ? 0xFFFFFFFFu : ((1u << total) - 1u);
}

/* Concatenate every chunk (missing ones replaced by a marker) into one
 * flat buffer first, THEN split that on '\n' a single time. Splitting
 * chunk-by-chunk independently looks equivalent but isn't: a chunk's data
 * always ends exactly at a line boundary (see rgcn_longmsg_send_paste's
 * sender-side chunking), so its last byte is usually '\n' - treating each
 * chunk as its own self-contained string then makes the "k == len" case
 * fire an extra time right after that trailing '\n', emitting a spurious
 * empty line at every chunk seam. One global split has only one true end
 * of buffer, so no seam-artifact is possible. */
static void flush_slot(rx_slot_t *s) {
    static char flat[LONGMSG_MAX_CHUNKS * (LONGMSG_CHUNK_MAX + 1) + 64];
    static const char MISSING[] = "[eksik satır - paket kaybı]\n";
    size_t pos = 0;
    for (int i = 0; i < s->total; i++) {
        const char *piece = (s->got_mask & (1u << i)) ? s->chunk_data[i] : MISSING;
        size_t plen = (s->got_mask & (1u << i)) ? s->chunk_len[i] : sizeof(MISSING) - 1;
        if (pos + plen >= sizeof flat) plen = sizeof flat - 1 - pos;
        memcpy(flat + pos, piece, plen);
        pos += plen;
        if (pos >= sizeof flat - 1) break;
    }
    flat[pos] = 0;

    rgcn_main_chat_header(s->nick, s->ts);
    char line[LONGMSG_CHUNK_MAX + 64];
    size_t start = 0;
    for (size_t k = 0; k <= pos; k++) {
        if (k == pos || flat[k] == '\n') {
            size_t n = k - start;
            if (n > sizeof line - 1) n = sizeof line - 1;
            memcpy(line, flat + start, n);
            line[n] = 0;
            rgcn_main_write_raw_line(line);
            start = k + 1;
        }
    }
    s->active = 0;
}

void rgcn_longmsg_on_part(const char *nick, uint64_t msg_id, uint64_t ts,
                          int idx, int total, const char *chunk_text) {
    if (total <= 0 || total > LONGMSG_MAX_CHUNKS || idx < 0 || idx >= total) return;

    int slot = -1;
    for (int i = 0; i < LONGMSG_SLOTS; i++) {
        if (g_slots[i].active && g_slots[i].msg_id == msg_id) { slot = i; break; }
    }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX; int oldest_i = 0;
        for (int i = 0; i < LONGMSG_SLOTS; i++) {
            if (!g_slots[i].active) { slot = i; break; }
            if (g_slots[i].first_seen_ms < oldest) { oldest = g_slots[i].first_seen_ms; oldest_i = i; }
        }
        if (slot < 0) {
            /* No free slot - evict the oldest in-flight reassembly rather
             * than grow unbounded; it'll show its own gaps when it would
             * otherwise have timed out anyway. */
            flush_slot(&g_slots[oldest_i]);
            slot = oldest_i;
        }
        memset(&g_slots[slot], 0, sizeof g_slots[slot]);
        g_slots[slot].active = 1;
        g_slots[slot].msg_id = msg_id;
        strncpy(g_slots[slot].nick, nick, sizeof g_slots[slot].nick - 1);
        g_slots[slot].ts = ts;
        g_slots[slot].total = total;
        g_slots[slot].first_seen_ms = rgcn_now_ms();
    }

    rx_slot_t *s = &g_slots[slot];
    size_t n = strlen(chunk_text);
    if (n > LONGMSG_CHUNK_MAX) n = LONGMSG_CHUNK_MAX;
    memcpy(s->chunk_data[idx], chunk_text, n);
    s->chunk_data[idx][n] = 0;
    s->chunk_len[idx] = (uint16_t)n;
    s->got_mask |= (1u << idx);

    if (s->got_mask == full_mask(s->total)) flush_slot(s);
}

void rgcn_longmsg_tick(void) {
    uint64_t now = rgcn_now_ms();
    for (int i = 0; i < LONGMSG_SLOTS; i++) {
        if (g_slots[i].active && now - g_slots[i].first_seen_ms > LONGMSG_TIMEOUT_MS) {
            flush_slot(&g_slots[i]);
        }
    }
}
