/*
 * Rigicon Live - Pong. Local only: human (left paddle) vs a capped-speed AI
 * (right paddle). No network - real-time Pong over best-effort LAN UDP
 * would need an authoritative-server model this project doesn't have yet.
 */

#include "game_internal.h"
#include "game.h"
#include "rgcn.h"

#include <stdio.h>
#include <string.h>

#define PONG_W 60
#define PONG_H 16
#define PADDLE_H 4
#define WIN_SCORE 5

static void respawn_ball(double *bx, double *by, double *dx, double *dy, int serve_left) {
    *bx = PONG_W / 2.0;
    *by = PONG_H / 2.0;
    *dx = serve_left ? -18.0 : 18.0;
    *dy = 6.0;
}

static void draw(const rgcn_frame_t *f, int left_y, int right_y,
                  double bx, double by, int left_score, int right_score) {
    int top = f->board_top;
    printf("\x1b[%d;1H\x1b[KPONG   %d - %d   (w/s veya ↑/↓: sol paday, c: sohbet, q: çık)",
           top, left_score, right_score);

    int ball_row = (int)(by + 0.5);
    int ball_col = (int)(bx + 0.5);
    char line[PONG_W + 1];
    for (int r = 0; r < PONG_H; r++) {
        memset(line, ' ', PONG_W);
        line[PONG_W] = 0;
        if (r >= left_y && r < left_y + PADDLE_H)   line[0] = '#';
        if (r >= right_y && r < right_y + PADDLE_H) line[PONG_W - 1] = '#';
        if (r == ball_row && ball_col >= 0 && ball_col < PONG_W) line[ball_col] = 'O';
        printf("\x1b[%d;1H\x1b[K%s", top + 1 + r, line);
    }
    fflush(stdout);
}

void rgcn_pong_run(void) {
    int left_y  = (PONG_H - PADDLE_H) / 2;
    int right_y = (PONG_H - PADDLE_H) / 2;
    double bx, by, dx, dy;
    respawn_ball(&bx, &by, &dx, &dy, 1);
    int left_score = 0, right_score = 0;
    uint64_t last = rgcn_now_ms();
    int quit = 0;

    while (!quit && left_score < WIN_SCORE && right_score < WIN_SCORE) {
        rgcn_frame_t f;
        rgcn_game_frame_sync(&f, PONG_H + 2);
        if (f.board_rows < PONG_H + 2 || f.cols < PONG_W + 1) {
            printf("\x1b[%d;1H\x1b[KTerminal çok küçük (en az %d satır, %d sütun gerekli). "
                   "'c': sohbeti gizle, pencereyi büyüt, ya da 'q' ile çık.",
                   f.board_top, PONG_H + 2, PONG_W + 1);
            fflush(stdout);
            char ch = 0;
            if (rgcn_game_read_key(200, &ch) == RGCN_KEY_QUIT) quit = 1;
            last = rgcn_now_ms();
            continue;
        }
        draw(&f, left_y, right_y, bx, by, left_score, right_score);

        char ch = 0;
        rgcn_key_t k = rgcn_game_read_key(40, &ch);
        if (k == RGCN_KEY_QUIT) { quit = 1; break; }
        if (k == RGCN_KEY_UP   || (k == RGCN_KEY_CHAR && ch == 'w')) left_y--;
        if (k == RGCN_KEY_DOWN || (k == RGCN_KEY_CHAR && ch == 's')) left_y++;
        if (left_y < 0) left_y = 0;
        if (left_y > PONG_H - PADDLE_H) left_y = PONG_H - PADDLE_H;

        uint64_t now = rgcn_now_ms();
        double dt = (double)(now - last) / 1000.0;
        if (dt > 0.2) dt = 0.2;
        last = now;

        /* AI: chase the ball's row with a capped speed and a dead zone, so
         * it's beatable rather than a wall. */
        double target_center = by - (PADDLE_H / 2.0 - 0.5);
        double right_center  = right_y;
        if (target_center > right_center + 0.6)      right_y++;
        else if (target_center < right_center - 0.6) right_y--;
        if (right_y < 0) right_y = 0;
        if (right_y > PONG_H - PADDLE_H) right_y = PONG_H - PADDLE_H;

        bx += dx * dt;
        by += dy * dt;
        if (by <= 0)            { by = 0;            dy = -dy; }
        if (by >= PONG_H - 1)   { by = PONG_H - 1;   dy = -dy; }

        if (dx < 0 && bx <= 1.0) {
            if (by >= left_y && by < left_y + PADDLE_H) {
                dx = -dx;
                bx = 1.0;
                dy += (by - (left_y + PADDLE_H / 2.0 - 0.5)) * 2.0;
                if (dy > 20.0) dy = 20.0;
                if (dy < -20.0) dy = -20.0;
            } else if (bx < 0) {
                right_score++;
                respawn_ball(&bx, &by, &dx, &dy, 1);
            }
        }
        if (dx > 0 && bx >= PONG_W - 2.0) {
            if (by >= right_y && by < right_y + PADDLE_H) {
                dx = -dx;
                bx = PONG_W - 2.0;
                dy += (by - (right_y + PADDLE_H / 2.0 - 0.5)) * 2.0;
                if (dy > 20.0) dy = 20.0;
                if (dy < -20.0) dy = -20.0;
            } else if (bx > PONG_W - 1.0) {
                left_score++;
                respawn_ball(&bx, &by, &dx, &dy, 0);
            }
        }
    }

    rgcn_frame_t f;
    rgcn_game_frame_sync(&f, PONG_H + 2);
    if (quit) {
        printf("\x1b[%d;1H\x1b[KÇıkıldı. Skor: %d - %d", f.board_top, left_score, right_score);
    } else {
        printf("\x1b[%d;1H\x1b[K%s   (%d - %d)",
               f.board_top, left_score > right_score ? "Kazandın!" : "Bilgisayar kazandı.",
               left_score, right_score);
    }
    fflush(stdout);
    rgcn_game_read_key(1200, NULL);
}
