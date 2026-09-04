#ifndef RGCN_NET_H
#define RGCN_NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct rgcn_net rgcn_net_t;

rgcn_net_t *rgcn_net_open(int port, char *err, size_t err_cap);
void        rgcn_net_close(rgcn_net_t *n);

/* Blocks up to timeout_ms. Returns bytes received, 0 on timeout, -1 on error. */
int         rgcn_net_recv(rgcn_net_t *n, uint8_t *buf, size_t buf_cap, int timeout_ms);

/* Broadcast a packet to every attached interface + limited broadcast. */
int         rgcn_net_broadcast(rgcn_net_t *n, const uint8_t *pkt, size_t pkt_len);

#endif
