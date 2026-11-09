#ifndef RGCN_ASCII_ART_H
#define RGCN_ASCII_ART_H

/* Rigicon Live - small curated library of built-in ASCII drawings.
 *
 * Freely typed multi-line ASCII art breaks in this app: on_enter() (see
 * main.c) collapses runs of spaces as an anti-paste-spam defense, which
 * destroys the alignment any ASCII drawing depends on. Baking a fixed set
 * of drawings into the binary sidesteps that entirely - the network only
 * ever carries a numeric id ("ASCII_SEND|station|id|ts|nick|art_id"), each
 * peer renders its own local copy of the same table, so alignment can
 * never be mangled in transit, there is no decoder, and no new dependency
 * is introduced. */

/* "/ascii ..." command: bare lists names, "/ascii <n>" previews locally
 * only, "/ascii <n> gonder" broadcasts it to the channel. */
void rgcn_ascii_cmd(const char *args);

/* Called by main.c's handle_decoded() for the ASCII_SEND wire kind. */
void rgcn_ascii_on_send(const char *nick, int id);

#endif
