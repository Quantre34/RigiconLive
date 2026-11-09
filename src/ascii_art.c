/*
 * Rigicon Live - built-in ASCII art library (see ascii_art.h for why this
 * exists instead of letting people type/paste their own).
 */

#include "ascii_art.h"
#include "term.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Implemented in main.c - generic UI/network bridges, not game-specific
 * (see src/game.h for the authoritative doc comments on these three). */
extern void        rgcn_main_system(const char *text);
extern void        rgcn_main_send(const char *kind, const char *text);
extern const char *rgcn_main_nick(void);

#define ASCII_MAX_ROWS 6

typedef struct {
    const char *name;
    const char *rows[ASCII_MAX_ROWS];
} ascii_art_t;

static const ascii_art_t ASCII_LIB[] = {
    { "kalp", {
        " .:::.   .:::. ",
        ":::::::.:::::::",
        "':::::::::::::'",
        "  ':::::::::'  ",
        "    ':::::'    ",
        "      ':'      ",
    }},
    { "kedi", {
        " /\\_/\\  ",
        "( o.o ) ",
        " > ^ <  ",
    }},
    { "kupa", {
        " ___________ ",
        "'._==_==_=_.'",
        " '-\\:    /-' ",
        "    ):  (    ",
        "   (_.__.')  ",
    }},
    { "robot", {
        " [=_=] ",
        " /|##|\\ ",
        "  |  |  ",
        " _|  |_ ",
    }},
    { "kahve", {
        "   ( (   ",
        "    ) )  ",
        " ........",
        " |      |",
        " \\      /",
        "  `----'  ",
    }},
    { "tebrikler", {
        " _____ _____ ___  ___ ",
        "|_   _|  ___|  \\/  | ",
        "  | | | |__ | .  . | ",
        "  | | |  __|| |\\/| | ",
        "  | | | |___| |  | | ",
        "  \\_/ \\____/\\_|  |_/ ",
    }},
};
#define ASCII_LIB_N (int)(sizeof ASCII_LIB / sizeof ASCII_LIB[0])

static int art_row_count(int idx) {
    int n = 0;
    while (n < ASCII_MAX_ROWS && ASCII_LIB[idx].rows[n]) n++;
    return n;
}

static void print_art(const char *header, int idx) {
    rgcn_main_system(header);
    int n = art_row_count(idx);
    for (int r = 0; r < n; r++) rgcn_main_system(ASCII_LIB[idx].rows[r]);
}

void rgcn_ascii_cmd(const char *args) {
    while (*args == ' ') args++;

    if (!*args) {
        rgcn_main_system("ASCII kütüphanesi:");
        char line[256]; int off = 0;
        for (int i = 0; i < ASCII_LIB_N; i++) {
            if (off == 0) off = snprintf(line, sizeof line, "  ");
            off += snprintf(line + off, sizeof line - off, "%d:%s  ", i, ASCII_LIB[i].name);
            if ((i + 1) % 4 == 0 || i + 1 == ASCII_LIB_N) {
                rgcn_main_system(line);
                off = 0;
            }
        }
        rgcn_main_system("Önizle (sadece sende görünür): /ascii <no>   |   Kanala gönder: /ascii <no> gönder");
        return;
    }

    char *end = NULL;
    long v = strtol(args, &end, 10);
    if (end == args || v < 0 || v >= ASCII_LIB_N) {
        char buf[64];
        snprintf(buf, sizeof buf, "Kullanım: /ascii 0-%d  |  /ascii <no> gönder", ASCII_LIB_N - 1);
        rgcn_main_system(buf);
        return;
    }
    while (*end == ' ') end++;

    if (*end) {
        /* "gönder" (or anything else typed after the number) - broadcast. */
        char id_str[8];
        snprintf(id_str, sizeof id_str, "%ld", v);
        rgcn_main_send("ASCII_SEND", id_str);
        char header[96];
        snprintf(header, sizeof header, "%s%s%s bir ASCII çizim gönderdi: %s",
                 rgcn_color_for(rgcn_main_nick()), rgcn_main_nick(), rgcn_color_reset(),
                 ASCII_LIB[v].name);
        print_art(header, (int)v);
    } else {
        print_art("(önizleme, sadece sende görünüyor - göndermek için sona \"gönder\" ekle)", (int)v);
    }
}

void rgcn_ascii_on_send(const char *nick, int id) {
    if (id < 0 || id >= ASCII_LIB_N) return;   /* unknown id - version skew, ignore */
    char header[96];
    snprintf(header, sizeof header, "%s%s%s bir ASCII çizim gönderdi: %s",
             rgcn_color_for(nick), nick, rgcn_color_reset(), ASCII_LIB[id].name);
    print_art(header, id);
}
