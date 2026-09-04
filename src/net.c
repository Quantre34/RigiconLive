/*
 * Rigicon Live - UDP hybrid multicast + broadcast layer.
 *
 * Two delivery paths, both tried for every outbound packet:
 *   1) IPv4 multicast group 239.74.44.44 (works cross-subnet on managed nets
 *      and on the same host via IP_MULTICAST_LOOP)
 *   2) IPv4 broadcast (255.255.255.255 + per-interface subnet broadcast)
 *      (works on cheap WiFi APs that silently drop multicast frames)
 *
 * Inbound: bind a single UDP socket on the port, join the multicast group on
 * every non-loopback interface, and receive both multicast and broadcast
 * traffic on the same socket. Duplicates from the two paths are dropped by
 * the message id dedupe in main.c.
 *
 * Cross-platform: Winsock2 on Windows, POSIX sockets elsewhere.
 */

#include "net.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "iphlpapi.lib")
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
  #include <ifaddrs.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define RGCN_CLOSESOCK close
  #define RGCN_SOCKERR() errno
#endif

#define RGCN_MCAST_ADDR "239.74.44.44"
#define RGCN_MCAST_TTL  4
#define RGCN_MAX_DEST   64
#define RGCN_MAX_LOCAL  32

struct rgcn_net {
    SOCKET             sock;
    int                port;
    struct sockaddr_in dests[RGCN_MAX_DEST];  /* mcast + subnet bcast + limited bcast */
    int                dest_count;
    int                iface_count;
    uint32_t           local_ips[RGCN_MAX_LOCAL];   /* network byte order */
    int                local_count;
};

static void remember_local(struct rgcn_net *n, uint32_t ip_be) {
    if (n->local_count >= RGCN_MAX_LOCAL) return;
    for (int i = 0; i < n->local_count; i++) if (n->local_ips[i] == ip_be) return;
    n->local_ips[n->local_count++] = ip_be;
}

int rgcn_net_is_self(rgcn_net_t *n, uint32_t ip_be) {
    for (int i = 0; i < n->local_count; i++) if (n->local_ips[i] == ip_be) return 1;
    return 0;
}

static void add_dest(struct rgcn_net *n, uint32_t addr_be, uint16_t port_be) {
    if (n->dest_count >= RGCN_MAX_DEST) return;
    struct sockaddr_in *d = &n->dests[n->dest_count++];
    memset(d, 0, sizeof *d);
    d->sin_family      = AF_INET;
    d->sin_port        = port_be;
    d->sin_addr.s_addr = addr_be;
}

static void collect_and_join(struct rgcn_net *n, uint16_t port_be, uint32_t mcast) {
    /* Always include limited broadcast + multicast target */
    add_dest(n, htonl(INADDR_BROADCAST), port_be);
    add_dest(n, mcast,                    port_be);

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.imr_multiaddr.s_addr = mcast;

    /* Join multicast on INADDR_ANY first (system default interface). */
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(n->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               (const char *)&mreq, sizeof mreq);

#ifdef _WIN32
    ULONG buflen = 15 * 1024;
    IP_ADAPTER_ADDRESSES *addrs = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        addrs = (IP_ADAPTER_ADDRESSES *)malloc(buflen);
        if (!addrs) return;
        ULONG rc = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST
            | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, addrs, &buflen);
        if (rc == ERROR_SUCCESS) break;
        free(addrs);
        addrs = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW) return;
    }
    if (!addrs) return;

    for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            uint32_t ip_be = sin->sin_addr.s_addr;
            uint32_t ip = ntohl(ip_be);
            uint32_t mask = u->OnLinkPrefixLength >= 32 ? 0xffffffffu
                          : u->OnLinkPrefixLength == 0  ? 0
                          : (0xffffffffu << (32 - u->OnLinkPrefixLength));
            uint32_t bcast_be = htonl((ip & mask) | (~mask));

            /* Subnet-directed broadcast for this interface */
            add_dest(n, bcast_be, port_be);

            /* Remember this address as one of ours */
            remember_local(n, ip_be);

            /* Join multicast on this specific interface too */
            mreq.imr_interface.s_addr = ip_be;
            setsockopt(n->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       (const char *)&mreq, sizeof mreq);
            n->iface_count++;
        }
    }
    free(addrs);
#else
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) return;
    for (struct ifaddrs *it = ifap; it; it = it->ifa_next) {
        if (!it->ifa_addr) continue;
        if (it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP)) continue;
        if (it->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_in *sin  = (struct sockaddr_in *)it->ifa_addr;
        struct sockaddr_in *mask = (struct sockaddr_in *)it->ifa_netmask;
        if (!mask) continue;

        uint32_t ip_be   = sin->sin_addr.s_addr;
        uint32_t m       = mask->sin_addr.s_addr;
        uint32_t bcast_be = (ip_be & m) | (~m);

        /* Subnet-directed broadcast for this interface */
        add_dest(n, bcast_be, port_be);

        /* Remember this address as one of ours */
        remember_local(n, ip_be);

        /* Join multicast on this specific interface */
        mreq.imr_interface.s_addr = ip_be;
        setsockopt(n->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *)&mreq, sizeof mreq);
        n->iface_count++;
    }
    freeifaddrs(ifap);
#endif
}

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
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof yes);
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons((uint16_t)port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof bind_addr) == SOCKET_ERROR) {
        snprintf(err, err_cap, "bind() failed on port %d (err=%d)", port, RGCN_SOCKERR());
        RGCN_CLOSESOCK(s);
        return NULL;
    }

    /* Multicast send options: local network TTL, loop back to same host. */
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

    uint32_t mcast_be = inet_addr(RGCN_MCAST_ADDR);
    collect_and_join(n, htons((uint16_t)port), mcast_be);

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

int rgcn_net_recv(rgcn_net_t *n, uint8_t *buf, size_t buf_cap, int timeout_ms,
                  uint32_t *from_ip_be, uint16_t *from_port_be) {
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

    if (from_ip_be)   *from_ip_be   = from.sin_addr.s_addr;
    if (from_port_be) *from_port_be = from.sin_port;
    return r;
}

int rgcn_net_broadcast(rgcn_net_t *n, const uint8_t *pkt, size_t pkt_len) {
    int ok = 0;
    for (int i = 0; i < n->dest_count; i++) {
        int r = sendto(n->sock, (const char *)pkt, (int)pkt_len, 0,
                       (struct sockaddr *)&n->dests[i], sizeof n->dests[i]);
        if (r == (int)pkt_len) ok++;
    }
    return ok > 0 ? 0 : -1;
}

int rgcn_net_unicast(rgcn_net_t *n, uint32_t ip_be, uint16_t port_be,
                     const uint8_t *pkt, size_t pkt_len) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = ip_be;
    dst.sin_port        = port_be;
    int r = sendto(n->sock, (const char *)pkt, (int)pkt_len, 0,
                   (struct sockaddr *)&dst, sizeof dst);
    return r == (int)pkt_len ? 0 : -1;
}
