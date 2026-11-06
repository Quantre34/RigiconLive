#ifndef RGCN_POLL_H
#define RGCN_POLL_H

#include <stdint.h>

/* Rigicon Live - simple multiple-choice polls over the existing LAN
 * channel. One active poll per channel at a time. Results show up as
 * normal system lines in the chat log - no separate screen, no takeover
 * of input (unlike src/game.c's /game, which does take the terminal). */

/* Entry points for "/poll ..." and "/vote ..." (see main.c's handle_command). */
void rgcn_poll_cmd(const char *args);
void rgcn_vote_cmd(const char *args);

/* Called by main.c's handle_decoded() for each POLL_* wire kind, fields
 * already split past "kind|station|id|ts|nick|". An old peer that doesn't
 * understand these kinds never reaches this code - unknown kinds are
 * silently ignored by the existing if/else-if chain. */
void rgcn_poll_on_open (const char *creator_nick, uint64_t poll_id, int duration_s,
                        const char *question, const char * const *opts, int n_opts);
void rgcn_poll_on_vote (const char *voter_nick, uint64_t poll_id, int choice_idx);
void rgcn_poll_on_close(const char *closer_nick, uint64_t poll_id,
                        const int *final_counts, int n_counts);

/* Call periodically (same cadence as expire_burns()) from the receiver
 * loop - drives the creator's POLL_OPEN/result rebroadcast, each voter's
 * own POLL_VOTE resend, and the non-creator fallback auto-close. All of
 * this exists because the transport is best-effort UDP: without resends,
 * one dropped packet on WiFi silently loses a peer's view of the poll or
 * their vote, the exact failure mode src/net.c's own doc comment warns
 * about for chat. */
void rgcn_poll_tick(void);

#endif
