#ifndef RGCN_LONGMSG_H
#define RGCN_LONGMSG_H

#include <stddef.h>
#include <stdint.h>

/* Hard cap on one long-message-mode paste (see longmsg.c) - exposed so
 * main.c's paste accumulation buffer can be sized to actually hold a
 * paste up to this size, rather than silently truncating it before
 * rgcn_longmsg_send_paste() ever sees the true length. */
#define RGCN_LONGMSG_MAX_BYTES 16384

/* Rigicon Live - "/longmsg" mode + chunked long-message delivery.
 *
 * Default (mode off): a multi-line paste (anything containing '\n') is
 * fanned out as one ordinary CHAT message per line, sent immediately -
 * never merged into g_input, never run through on_enter()'s space
 * collapsing, and never interpreted as a command even if a line happens
 * to start with '/'. Each line survives or is lost independently, same
 * best-effort behavior as any other chat message.
 *
 * Mode on (per-session, local only - never sent over the wire, same as
 * /timer): a multi-line paste is sent as ONE logical message instead.
 * If it fits in a single packet's safe budget it rides the existing CHAT
 * kind with real '\n' bytes inside the text field. If it doesn't, it's
 * split along line boundaries (never mid-line) into CHATL_PART packets
 * under a shared msg_id and reassembled on the other end - capped at
 * LONGMSG_MAX_BYTES so a malicious "this message has 32 chunks" claim
 * can't be used to exhaust memory with a single real packet. An old peer
 * that doesn't understand CHATL_PART just ignores it (same fallthrough
 * every other new kind in this project has relied on). */

/* "/longmsg [on|off]" - bare shows current state. */
void rgcn_longmsg_cmd(const char *args);

/* Entry point for main.c's paste-end handler: text/len is the already
 * control-char-sanitized multi-line block (real '\n' line separators),
 * called only when the paste contains at least one '\n' - single-line
 * pastes keep their existing merge-into-g_input behavior untouched. */
void rgcn_longmsg_send_paste(const char *text, size_t len);

/* main.c's handle_decoded() CHATL_PART branch. */
void rgcn_longmsg_on_part(const char *nick, uint64_t msg_id, uint64_t ts,
                          int idx, int total, const char *chunk_text);

/* Call periodically (same cadence as expire_burns()/rgcn_poll_tick()) -
 * flushes any reassembly slot that's been incomplete too long, rendering
 * whatever chunks did arrive (with a placeholder for the missing ones)
 * instead of staying silent forever if one chunk never shows up. */
void rgcn_longmsg_tick(void);

#endif
