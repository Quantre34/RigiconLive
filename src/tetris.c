/*
 * Rigicon Live - Tetris. Single player, no network.
 *
 * Draws into the board region handed to it by game.c's frame sync every
 * tick (full repaint - no incremental cell updates, same lesson as the
 * rest of the terminal UI). Gravity is paced off rgcn_now_ms() deltas, not
 * off "the key read consumed exactly N ms".
 */

#include "game_internal.h"
#include "game.h"
#include "term.h"
#include "rgcn.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define TET_W 10
#define TET_H 20

static const uint16_t SPAWN[7] = {
    /* I */ (1<<4)|(1<<5)|(1<<6)|(1<<7),
    /* O */ (1<<5)|(1<<6)|(1<<9)|(1<<10),
    /* T */ (1<<1)|(1<<4)|(1<<5)|(1<<6),
    /* S */ (1<<1)|(1<<2)|(1<<4)|(1<<5),
    /* Z */ (1<<0)|(1<<1)|(1<<5)|(1<<6),
    /* J */ (1<<0)|(1<<4)|(1<<5)|(1<<6),
    /* L */ (1<<2)|(1<<4)|(1<<5)|(1<<6),
};

static uint16_t rotate_cw(uint16_t m) {
    uint16_t out = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (m & (1 << (r * 4 + c)))
                out |= (uint16_t)(1 << (c * 4 + (3 - r)));
    return out;
}

static int collides(int px, int py, uint16_t mask, uint8_t board[TET_H][TET_W]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(mask & (1 << (r * 4 + c)))) continue;
            int bx = px + c, by = py + r;
            if (bx < 0 || bx >= TET_W || by >= TET_H) return 1;
            if (by < 0) continue;
            if (board[by][bx]) return 1;
        }
    }
    return 0;
}

static void lock_piece(uint8_t board[TET_H][TET_W], int type, uint16_t mask, int px, int py) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(mask & (1 << (r * 4 + c)))) continue;
            int bx = px + c, by = py + r;
            if (by >= 0 && by < TET_H && bx >= 0 && bx < TET_W) board[by][bx] = (uint8_t)(type + 1);
        }
    }
}

static int clear_lines(uint8_t board[TET_H][TET_W]) {
    int cleared = 0;
    for (int r = TET_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TET_W; c++) if (!board[r][c]) { full = 0; break; }
        if (!full) continue;
        cleared++;
        for (int rr = r; rr > 0; rr--) memcpy(board[rr], board[rr - 1], TET_W);
        memset(board[0], 0, TET_W);
        r++;   /* re-check this row index - it now holds what was above */
    }
    return cleared;
}

static void draw_board(const rgcn_frame_t *f, uint8_t board[TET_H][TET_W],
                        int type, uint16_t mask, int px, int py,
                        int score, int level, int lines) {
    uint8_t view[TET_H][TET_W];
    memcpy(view, board, sizeof view);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(mask & (1 << (r * 4 + c)))) continue;
            int bx = px + c, by = py + r;
            if (by >= 0 && by < TET_H && bx >= 0 && bx < TET_W) view[by][bx] = (uint8_t)(type + 1);
        }
    }

    int top = f->board_top;
    printf("\x1b[%d;1H\x1b[KTETRIS   Skor: %d   Seviye: %d   Satır: %d   "
           "(a/d hareket, w döndür, s yumuşak düşür, boşluk sert düşür, c sohbet, q çık)",
           top, score, level, lines);
    for (int r = 0; r < TET_H; r++) {
        printf("\x1b[%d;1H\x1b[K|", top + 1 + r);
        for (int c = 0; c < TET_W; c++) {
            if (view[r][c]) {
                printf("%s██%s", rgcn_color_by_index(view[r][c] - 1), rgcn_color_reset());
            } else {
                printf("  ");
            }
        }
        printf("|");
    }
    printf("\x1b[%d;1H\x1b[K+", top + 1 + TET_H);
    for (int c = 0; c < TET_W; c++) printf("--");
    printf("+");
    fflush(stdout);
}

void rgcn_tetris_run(void) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }

    uint8_t board[TET_H][TET_W];
    memset(board, 0, sizeof board);

    int score = 0, lines = 0, level = 1, gravity_ms = 500;
    int type = rand() % 7;
    uint16_t mask = SPAWN[type];
    int px = 3, py = 0;
    uint64_t last_tick = rgcn_now_ms();
    int quit = 0, over = 0;

    while (!over && !quit) {
        rgcn_frame_t f;
        rgcn_game_frame_sync(&f, TET_H + 2);
        if (f.board_rows < TET_H + 2) {
            printf("\x1b[%d;1H\x1b[KTerminal çok küçük (en az %d satır gerekli). "
                   "'c': sohbeti gizle, pencereyi büyüt, ya da 'q' ile çık.",
                   f.board_top, TET_H + 2);
            fflush(stdout);
            char ch = 0;
            if (rgcn_game_read_key(200, &ch) == RGCN_KEY_QUIT) quit = 1;
            continue;
        }
        draw_board(&f, board, type, mask, px, py, score, level, lines);

        int remaining = gravity_ms - (int)(rgcn_now_ms() - last_tick);
        if (remaining < 10) remaining = 10;
        if (remaining > 50) remaining = 50;   /* keep input responsive */

        char ch = 0;
        rgcn_key_t k = rgcn_game_read_key(remaining, &ch);
        switch (k) {
            case RGCN_KEY_QUIT:  quit = 1; break;
            case RGCN_KEY_LEFT:  if (!collides(px - 1, py, mask, board)) px--; break;
            case RGCN_KEY_RIGHT: if (!collides(px + 1, py, mask, board)) px++; break;
            case RGCN_KEY_DOWN:  if (!collides(px, py + 1, mask, board)) { py++; score += 1; } break;
            case RGCN_KEY_UP: {
                uint16_t m2 = rotate_cw(mask);
                if (!collides(px, py, m2, board)) mask = m2;
                break;
            }
            case RGCN_KEY_SPACE:
                while (!collides(px, py + 1, mask, board)) { py++; score += 2; }
                break;
            case RGCN_KEY_CHAR:
                if (ch == 'a')      { if (!collides(px - 1, py, mask, board)) px--; }
                else if (ch == 'd') { if (!collides(px + 1, py, mask, board)) px++; }
                else if (ch == 's') { if (!collides(px, py + 1, mask, board)) { py++; score += 1; } }
                else if (ch == 'w') {
                    uint16_t m2 = rotate_cw(mask);
                    if (!collides(px, py, m2, board)) mask = m2;
                }
                break;
            default: break;
        }
        if (quit) break;

        if ((int)(rgcn_now_ms() - last_tick) >= gravity_ms) {
            last_tick = rgcn_now_ms();
            if (!collides(px, py + 1, mask, board)) {
                py++;
            } else {
                lock_piece(board, type, mask, px, py);
                int c = clear_lines(board);
                if (c > 0) {
                    static const int pts[] = {0, 100, 300, 500, 800};
                    score += pts[c] * level;
                    lines += c;
                    level = 1 + lines / 10;
                    gravity_ms = 500 - (level - 1) * 40;
                    if (gravity_ms < 100) gravity_ms = 100;
                }
                type = rand() % 7;
                mask = SPAWN[type];
                px = 3; py = 0;
                if (collides(px, py, mask, board)) over = 1;
            }
        }
    }

    if (over) {
        for (;;) {
            rgcn_frame_t f;
            rgcn_game_frame_sync(&f, TET_H + 2);
            printf("\x1b[%d;1H\x1b[KOyun bitti! Skor: %d   Satır: %d   Seviye: %d",
                   f.board_top, score, lines, level);
            printf("\x1b[%d;1H\x1b[KSkoru kanala paylaş mı? (y/n)", f.board_top + 1);
            fflush(stdout);

            char ch = 0;
            rgcn_key_t k = rgcn_game_read_key(300, &ch);
            if (k == RGCN_KEY_QUIT) break;
            if (k == RGCN_KEY_CHAR && (ch == 'y' || ch == 'Y')) {
                char line[96];
                snprintf(line, sizeof line, "Tetris skorum: %d (satır: %d, seviye: %d)", score, lines, level);
                rgcn_main_chat_send(line);
                break;
            }
            if (k == RGCN_KEY_CHAR && (ch == 'n' || ch == 'N')) break;
        }
    } else {
        /* User quit mid-game. */
        rgcn_frame_t f;
        rgcn_game_frame_sync(&f, TET_H + 2);
        printf("\x1b[%d;1H\x1b[KCıkıldı. Skor: %d   Satır: %d", f.board_top, score, lines);
        fflush(stdout);
        rgcn_game_read_key(800, NULL);
    }
}
