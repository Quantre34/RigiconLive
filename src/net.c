/*
 * Rigicon Live - UDP multicast layer.
 * Every station on the same port/frequency joins the same multicast group.
 * "Frequency" is the port; the group address is fixed (239.74.44.44).
 * IP_MULTICAST_LOOP is on so two clients on the same host also hear each other.
 * Cross-platform: Winsock2 on Windows, POSIX sockets elsewhere.
 */

#include "net.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define RGCN_CLOSESOCK closesocket
  #define RGCN_SOCKERR() WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <net/if.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define RGCN_CLOSESOCK close
  #define RGCN_SOCKERR() errno
#endif

/* Admin-scoped local group (239.0.0.0/8). Never routed off-site. */
#define RGCN_MCAST_ADDR "239.74.44.44"
#define RGCN_MCAST_TTL  4

struct rgcn_net {
    SOCKET             sock;
    int                port;
    struct sockaddr_in mcast_dst;
};

rgcn_net_t *rgcn_net_open(int port, char *err, size_t err_cap) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(err, err_cap, "WSAStartup failed");
        return NULL;
    }
#endif

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        snprintf(err, err_cap, "socket() failed (err=%d)", RGCN_SOCKERR());
        return NULL;
    }

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof yes);
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons((uint16_t)port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof bind_addr) == SOCKET_ERROR) {
        snprintf(err, err_cap, "bind() failed on port %d (err=%d)", port, RGCN_SOCKERR());
        RGCN_CLOSESOCK(s);
        return NULL;
    }

    /* Join the multicast group on all interfaces (INADDR_ANY). */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.imr_multiaddr.s_addr = inet_addr(RGCN_MCAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *)&mreq, sizeof mreq) != 0) {
        snprintf(err, err_cap, "IP_ADD_MEMBERSHIP failed (err=%d)", RGCN_SOCKERR());
        RGCN_CLOSESOCK(s);
        return NULL;
    }

    /* Send options: local network TTL, loop back to same host so a second
     * instance on the same machine also receives our transmissions. */
    unsigned char ttl = RGCN_MCAST_TTL;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    unsigned char loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof loop);

    rgcn_net_t *n = (rgcn_net_t *)calloc(1, sizeof *n);
    if (!n) {
        snprintf(err, err_cap, "out of memory");
        RGCN_CLOSESOCK(s);
        return NULL;
    }
    n->sock = s;
    n->port = port;
    memset(&n->mcast_dst, 0, sizeof n->mcast_dst);
    n->mcast_dst.sin_family = AF_INET;
    n->mcast_dst.sin_port   = htons((uint16_t)port);
    n->mcast_dst.sin_addr.s_addr = inet_addr(RGCN_MCAST_ADDR);
    return n;
}

void rgcn_net_close(rgcn_net_t *n) {
    if (!n) return;
    if (n->sock != INVALID_SOCKET) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof mreq);
        mreq.imr_multiaddr.s_addr = inet_addr(RGCN_MCAST_ADDR);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(n->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   (const char *)&mreq, sizeof mreq);
        RGCN_CLOSESOCK(n->sock);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    free(n);
}

int rgcn_net_recv(rgcn_net_t *n, uint8_t *buf, size_t buf_cap, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(n->sock, &rfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int nfds = (int)n->sock + 1;
    int rc = select(nfds, &rfds, NULL, NULL, &tv);
    if (rc == 0) return 0;
    if (rc < 0) return -1;

    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    int r = recvfrom(n->sock, (char *)buf, (int)buf_cap, 0,
                     (struct sockaddr *)&from, &flen);
    if (r < 0) return -1;
    return r;
}

int rgcn_net_broadcast(rgcn_net_t *n, const uint8_t *pkt, size_t pkt_len) {
    int r = sendto(n->sock, (const char *)pkt, (int)pkt_len, 0,
                   (struct sockaddr *)&n->mcast_dst, sizeof n->mcast_dst);
    return r == (int)pkt_len ? 0 : -1;
}
