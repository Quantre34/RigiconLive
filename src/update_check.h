#ifndef RGCN_UPDATE_CHECK_H
#define RGCN_UPDATE_CHECK_H

#include <stddef.h>

/* Rigicon Live - optional "is a newer release out" check.
 *
 * Runs once, in a background thread, right after startup: shells out to
 * curl against GitHub's public releases API with a short timeout, so a
 * slow or absent internet connection never delays opening the chat. If
 * curl isn't installed, there's no network, or the request times out, it
 * just never reports an update - same graceful-degradation shape as the
 * terminal-notifier integration in notify.c. The only network traffic
 * this ever generates is one plain GET to GitHub's release API; nothing
 * about this app, its channel, peers, or messages is ever sent anywhere. */
void rgcn_update_check_async(void);

/* True once the background check has found a tagged release newer than
 * RGCN_VERSION; *out receives that version string (e.g. "1.7.0", no
 * leading 'v') if out/out_cap are given. Safe to call from any thread at
 * any time - returns 0 (nothing to report, whether because none exists
 * yet, the check is still running, or it already came back clean) until
 * a real update is found. */
int rgcn_update_available(char *out, size_t out_cap);

#endif
