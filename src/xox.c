/*
 * Rigicon Live - XOX (tic-tac-toe), two players over the LAN channel.
 *
 * The transport (rgcn_net_*) is best-effort UDP with no reliability layer,
 * so every GAME_MOVE carries the FULL board + a move counter rather than a
 * delta: a lost packet is harmless (the next real move - or the periodic
 * resend below - repairs it), and a stale/duplicate packet is dropped by
 * comparing the counter. The side that just moved resends that same move
 * every ~1s until the opponent's reply arrives, which is the only "ack".
 */

#include "game_internal.h"
#include "game.h"
#include "rgcn.h"

#include <stdio.h>
#include <string.h>

#define CELL_EMPTY '.'

static char check_winner(const char board[9]) {
    static const int lines[8][3] = {
        {0,1,2}, {3,4,5}, {6,7,8},
        {0,3,6}, {1,4,7}, {2,5,8},
        {0,4,8}, {2,4,6},
    };
    for (int i = 0; i < 8; i++) {
        int a = lines[i][0], b = lines[i][1], c = lines[i][2];
        if (board[a] != CELL_EMPTY && board[a] == board[b] && board[b] == board[c]) return board[a];
    }
    return 0;
}

static int board_full(const char board[9]) {
    for (int i = 0; i < 9; i++) if (board[i] == CELL_EMPTY) return 0;
    return 1;
}

static void draw(const rgcn_frame_t *f, const char board[9], const char *opponent,
                  char my_mark, int my_move_now, int finished) {
    int top = f->board_top;
    printf("\x1b[%d;1H\x1b[KXOX vs %s   Sen: %c   (1-9: hücre seç, c: sohbet, q: pes et/çık)",
           top, opponent, my_mark);

    char cell[9];
    for (int i = 0; i < 9; i++) cell[i] = (board[i] == CELL_EMPTY) ? (char)('1' + i) : board[i];

    printf("\x1b[%d;1H\x1b[K  %c | %c | %c", top + 2, cell[0], cell[1], cell[2]);
    printf("\x1b[%d;1H\x1b[K ---+---+---", top + 3);
    printf("\x1b[%d;1H\x1b[K  %c | %c | %c", top + 4, cell[3], cell[4], cell[5]);
    printf("\x1b[%d;1H\x1b[K ---+---+---", top + 5);
    printf("\x1b[%d;1H\x1b[K  %c | %c | %c", top + 6, cell[6], cell[7], cell[8]);

    const char *status;
    switch (finished) {
        case 1: status = "Kazandın!"; break;
        case 2: status = "Kaybettin."; break;
        case 3: status = "Berabere."; break;
        case 4: status = "Pes ettin."; break;
        case 5: status = "Rakip pes etti - kazandın!"; break;
        default: status = my_move_now ? "Sıra sende." : "Rakip bekleniyor..."; break;
    }
    printf("\x1b[%d;1H\x1b[K%s", top + 8, status);
    fflush(stdout);
}

void rgcn_xox_run(const char *opponent_nick, uint64_t offer_id, int my_turn) {
    char board[10];   /* [0..8] = cells, [9] = NUL - board is passed as %s to send_move */
    for (int i = 0; i < 9; i++) board[i] = CELL_EMPTY;
    board[9] = 0;
    char my_mark = my_turn ? 'X' : 'O';

    int turn_no_done   = 0;      /* moves applied so far (mine + opponent's) */
    int last_move_mine = 0;
    int finished        = 0;     /* 0 ongoing; see draw()'s switch for codes */
    uint64_t last_resend = 0;

    for (;;) {
        rgcn_frame_t f;
        rgcn_game_frame_sync(&f, 10);

        int next_is_X     = (turn_no_done % 2) == 0;
        int my_move_now   = my_turn ? next_is_X : !next_is_X;

        if (f.board_rows < 10) {
            printf("\x1b[%d;1H\x1b[KTerminal çok küçük (en az 10 satır gerekli). "
                   "'c': sohbeti gizle, pencereyi büyüt, ya da 'q' ile çık.", f.board_top);
            fflush(stdout);
            char ch = 0;
            if (rgcn_game_read_key(200, &ch) == RGCN_KEY_QUIT) {
                if (!finished) rgcn_game_send_resign(offer_id);
                break;
            }
            continue;
        }

        draw(&f, board, opponent_nick, my_mark, my_move_now, finished);

        if (finished) {
            rgcn_game_read_key(1500, NULL);
            break;
        }

        char ch = 0;
        rgcn_key_t k = rgcn_game_read_key(150, &ch);
        if (k == RGCN_KEY_QUIT) {
            rgcn_game_send_resign(offer_id);
            finished = 4;
            continue;
        }
        if (my_move_now && k == RGCN_KEY_CHAR && ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (board[idx] == CELL_EMPTY) {
                board[idx] = my_mark;
                turn_no_done++;
                last_move_mine = 1;
                rgcn_game_send_move(offer_id, (uint64_t)turn_no_done, board);
                last_resend = rgcn_now_ms();
                char w = check_winner(board);
                if (w)                     finished = (w == my_mark) ? 1 : 2;
                else if (board_full(board)) finished = 3;
            }
        }

        rgcn_game_net_msg_t ev;
        if (!finished && rgcn_game_poll(offer_id, &ev)) {
            if (ev.type == RGCN_GEV_MOVE && ev.turn_no > (uint64_t)turn_no_done) {
                turn_no_done   = (int)ev.turn_no;
                last_move_mine = 0;
                memcpy(board, ev.board, 9);
                char w = check_winner(board);
                if (w)                     finished = (w == my_mark) ? 1 : 2;
                else if (board_full(board)) finished = 3;
            } else if (ev.type == RGCN_GEV_RESIGN) {
                finished = 5;
            }
        }

        if (!finished && last_move_mine) {
            uint64_t now = rgcn_now_ms();
            if (now - last_resend >= 1000) {
                rgcn_game_send_move(offer_id, (uint64_t)turn_no_done, board);
                last_resend = now;
            }
        }
    }
}
