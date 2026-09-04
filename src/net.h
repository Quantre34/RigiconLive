#ifndef RGCN_NET_H
#define RGCN_NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct rgcn_net rgcn_net_t;

rgcn_net_t *rgcn_net_open(int port, char *err, size_t err_cap);
void        rgcn_net_close(rgcn_net_t *n);

/* Blocks up to timeout_ms. Returns bytes received, 0 on timeout, -1 on error.
 * If from_ip/from_port are non-null, they receive the sender's IPv4 addr
 * in network byte order. */
int         rgcn_net_recv(rgcn_net_t *n, uint8_t *buf, size_t buf_cap,
                          int timeout_ms,
                          uint32_t *from_ip_be, uint16_t *from_port_be);

/* Multicast to 239.74.44.44 + limited broadcast + per-interface subnet
 * broadcast. Best-effort delivery for discovery. */
int         rgcn_net_broadcast(rgcn_net_t *n, const uint8_t *pkt, size_t pkt_len);

/* Reliable UDP-unicast to a known peer. Use this for CHAT messages once the
 * peer has been discovered - avoids WiFi multicast packet loss. */
int         rgcn_net_unicast(rgcn_net_t *n, uint32_t ip_be, uint16_t port_be,
                             const uint8_t *pkt, size_t pkt_len);

/* Returns true if the given IPv4 address (network byte order) is one of
 * this machine's own interfaces - used to skip self in peer table. */
int         rgcn_net_is_self(rgcn_net_t *n, uint32_t ip_be);

#endif
