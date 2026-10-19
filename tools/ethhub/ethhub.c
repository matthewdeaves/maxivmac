/*
 * ethhub - a tiny virtual Ethernet hub/switch over UDP.
 *
 * Wire format: exactly one raw Ethernet frame (14..1514 bytes, no FCS, no
 * extra header) per UDP datagram, in both directions.  A zero-length
 * datagram is a hello/keepalive that registers the sender as a client.
 * This is the same framing QEMU's UDP/dgram netdev uses.
 *
 * Behaviour: learns source MAC -> port, forwards unicast to a learned port
 * only, floods broadcast/multicast/unknown unicast to every other port,
 * never echoes a frame back to the port it came from.  Idle clients age out
 * after --client-timeout seconds, MAC entries after --mac-timeout seconds.
 * Optional Linux-only TAP port (--tap) puts a local IP stack on the LAN.
 *
 * Security: ANY datagram from ANY source that reaches the socket makes that
 * source a client, and broadcasts are flooded to every client.  A hub that
 * listens beyond loopback can therefore be used for UDP reflection and
 * amplification (spoofed sources) and lets anyone on the network read the
 * LAN's broadcast traffic and take over MACs.  So the default listen address
 * is 127.0.0.1; anything wider needs an explicit --listen ADDR:PORT, prints a
 * warning, and should be limited with --allow CIDR.  There is no
 * authentication and no rate limit: do not expose the hub to the internet.
 *
 * Plain C11 + POSIX sockets, no dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * Part of maxivmac (GPL v2, see LICENSE).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#define ETHHUB_HAVE_TAP 1
#else
#define ETHHUB_HAVE_TAP 0
#endif

#define FRAME_MIN 14
#define FRAME_MAX 1514
#define BUF_SIZE 65536 /* larger than any datagram so oversize is detectable */

#define DEF_PORT 5555
#define DEF_CLIENT_TIMEOUT 60
#define DEF_MAC_TIMEOUT 300
#define DEF_MAX_CLIENTS 64
#define DEF_MAX_MACS 1024
#define HARD_MAX_CLIENTS 4096
#define HARD_MAX_MACS 4096 /* lookups are linear scans, twice per frame */
#define MAX_ALLOW 16

typedef struct {
    bool used;
    bool is_tap;
    bool pinned; /* TAP / uplink: never ages out */
    struct sockaddr_in addr;
    time_t last_seen;
} Client;

typedef struct {
    bool used;
    uint8_t mac[6];
    int client;
    time_t last_seen;
} MacEntry;

typedef struct {
    uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes;
    uint64_t hellos, dropped_short, dropped_long, dropped_full, dropped_denied;
    uint64_t unicast, flooded, unknown_unicast, send_errors;
    uint64_t clients_added, clients_expired, macs_learned, macs_evicted, macs_expired;
} Stats;

typedef struct {
    uint32_t net, mask; /* host order */
} AllowNet;

static AllowNet g_allow[MAX_ALLOW];
static int g_nallow;
static Client *g_clients;
static MacEntry *g_macs;
static int g_max_clients = DEF_MAX_CLIENTS;
static int g_max_macs = DEF_MAX_MACS;
static int g_client_timeout = DEF_CLIENT_TIMEOUT;
static int g_mac_timeout = DEF_MAC_TIMEOUT;
static int g_sock = -1;
static int g_tap_fd = -1;
static int g_tap_slot = -1;
static bool g_verbose;
static FILE *g_pcap;
static Stats g_st;
static volatile sig_atomic_t g_quit, g_dump;

static time_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void fmt_mac(const uint8_t *m, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void fmt_client(const Client *c, char *out, size_t n)
{
    if (c->is_tap) {
        snprintf(out, n, "tap");
    } else {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &c->addr.sin_addr, ip, sizeof ip);
        snprintf(out, n, "%s:%u", ip, ntohs(c->addr.sin_port));
    }
}

static void pcap_open(const char *path)
{
    g_pcap = fopen(path, "wb");
    if (!g_pcap) {
        perror(path);
        exit(1);
    }
    uint32_t magic = 0xa1b2c3d4u, zero = 0, snaplen = 65535, linktype = 1; /* Ethernet */
    uint16_t ver[2] = {2, 4};
    fwrite(&magic, 4, 1, g_pcap);
    fwrite(ver, 2, 2, g_pcap);
    fwrite(&zero, 4, 1, g_pcap); /* thiszone */
    fwrite(&zero, 4, 1, g_pcap); /* sigfigs */
    fwrite(&snaplen, 4, 1, g_pcap);
    fwrite(&linktype, 4, 1, g_pcap);
    fflush(g_pcap);
}

static void pcap_write(const uint8_t *f, size_t len)
{
    if (!g_pcap)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t rec[4] = {(uint32_t)ts.tv_sec, (uint32_t)(ts.tv_nsec / 1000), (uint32_t)len, (uint32_t)len};
    fwrite(rec, 4, 4, g_pcap);
    fwrite(f, 1, len, g_pcap);
    fflush(g_pcap);
}

/* ---- client table ---------------------------------------------------- */

static int client_find(const struct sockaddr_in *a)
{
    for (int i = 0; i < g_max_clients; i++) {
        const Client *c = &g_clients[i];
        if (c->used && !c->is_tap && c->addr.sin_addr.s_addr == a->sin_addr.s_addr &&
            c->addr.sin_port == a->sin_port)
            return i;
    }
    return -1;
}

static void macs_purge_client(int idx)
{
    for (int i = 0; i < g_max_macs; i++)
        if (g_macs[i].used && g_macs[i].client == idx)
            g_macs[i].used = false;
}

/* Find or create; -1 if table full. */
static int client_touch(const struct sockaddr_in *a, time_t now)
{
    int idx = client_find(a);
    if (idx >= 0) {
        g_clients[idx].last_seen = now;
        return idx;
    }
    for (int i = 0; i < g_max_clients; i++) {
        if (!g_clients[i].used) {
            g_clients[i].used = true;
            g_clients[i].is_tap = false;
            g_clients[i].addr = *a;
            g_clients[i].last_seen = now;
            g_st.clients_added++;
            if (g_verbose) {
                char s[64];
                fmt_client(&g_clients[i], s, sizeof s);
                fprintf(stderr, "client + %s\n", s);
            }
            return i;
        }
    }
    return -1;
}

/* ---- MAC table ------------------------------------------------------- */

static int mac_find(const uint8_t *mac)
{
    for (int i = 0; i < g_max_macs; i++)
        if (g_macs[i].used && memcmp(g_macs[i].mac, mac, 6) == 0)
            return i;
    return -1;
}

static void mac_learn(const uint8_t *mac, int client, time_t now)
{
    if (mac[0] & 1) /* never learn multicast/broadcast sources */
        return;
    int i = mac_find(mac);
    if (i < 0) {
        int victim = -1;
        for (int k = 0; k < g_max_macs; k++) {
            if (!g_macs[k].used) {
                victim = k;
                break;
            }
            if (victim < 0 || g_macs[k].last_seen < g_macs[victim].last_seen)
                victim = k;
        }
        if (g_macs[victim].used)
            g_st.macs_evicted++; /* table full: evict least recently seen */
        i = victim;
        memcpy(g_macs[i].mac, mac, 6);
        g_macs[i].used = true;
        g_st.macs_learned++;
    }
    g_macs[i].client = client;
    g_macs[i].last_seen = now;
}

/* ---- ageing ---------------------------------------------------------- */

static void age(time_t now)
{
    for (int i = 0; i < g_max_clients; i++) {
        Client *c = &g_clients[i];
        if (c->used && !c->pinned && now - c->last_seen >= g_client_timeout) {
            if (g_verbose) {
                char s[64];
                fmt_client(c, s, sizeof s);
                fprintf(stderr, "client - %s (timeout)\n", s);
            }
            macs_purge_client(i);
            c->used = false;
            g_st.clients_expired++;
        }
    }
    for (int i = 0; i < g_max_macs; i++) {
        if (g_macs[i].used && now - g_macs[i].last_seen >= g_mac_timeout) {
            g_macs[i].used = false;
            g_st.macs_expired++;
        }
    }
}

/* ---- forwarding ------------------------------------------------------ */

static void send_to(int idx, const uint8_t *f, size_t len)
{
    Client *c = &g_clients[idx];
    ssize_t r;
    if (c->is_tap)
        r = write(g_tap_fd, f, len);
    else
        r = sendto(g_sock, f, len, 0, (const struct sockaddr *)&c->addr, sizeof c->addr);
    if (r < 0) {
        g_st.send_errors++;
    } else {
        g_st.tx_frames++;
        g_st.tx_bytes += (uint64_t)len;
    }
}

static void forward(int from, const uint8_t *f, size_t len, time_t now)
{
    g_st.rx_frames++;
    g_st.rx_bytes += len;
    pcap_write(f, len);
    mac_learn(f + 6, from, now);

    const uint8_t *dst = f;
    int target = -1;
    if (!(dst[0] & 1)) {
        int m = mac_find(dst);
        if (m >= 0 && g_clients[g_macs[m].client].used)
            target = g_macs[m].client;
    }
    if (g_verbose) {
        char s[18], d[18];
        fmt_mac(f + 6, s);
        fmt_mac(dst, d);
        fprintf(stderr, "frame %s -> %s len=%zu %s\n", s, d, len,
                target >= 0 ? "unicast" : (dst[0] & 1) ? "flood(bcast/mcast)" : "flood(unknown)");
    }
    if (target >= 0) {
        g_st.unicast++;
        if (target != from) /* never echo (also covers dst learned on same port) */
            send_to(target, f, len);
        return;
    }
    if (dst[0] & 1)
        g_st.flooded++;
    else
        g_st.unknown_unicast++;
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].used && i != from)
            send_to(i, f, len);
}

/* No --allow means everybody the socket can hear. */
static bool allowed(const struct sockaddr_in *a)
{
    if (g_nallow == 0)
        return true;
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    for (int i = 0; i < g_nallow; i++)
        if ((ip & g_allow[i].mask) == g_allow[i].net)
            return true;
    return false;
}

static bool parse_cidr(const char *s, AllowNet *out)
{
    char buf[64];
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl == 0 || hl >= sizeof buf)
        return false;
    memcpy(buf, s, hl);
    buf[hl] = '\0';
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1)
        return false;
    long bits = 32;
    if (slash) {
        char *e;
        bits = strtol(slash + 1, &e, 10);
        if (slash[1] == '\0' || *e != '\0' || bits < 0 || bits > 32)
            return false;
    }
    out->mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    out->net = ntohl(a.s_addr) & out->mask;
    return true;
}

static void handle_udp(uint8_t *buf)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(g_sock, buf, BUF_SIZE, 0, (struct sockaddr *)&from, &fl);
    if (n < 0)
        return;
    time_t now = now_s();
    if (!allowed(&from)) {
        g_st.dropped_denied++;
        return;
    }
    if (n > FRAME_MAX) {
        g_st.dropped_long++;
        return;
    }
    if (n > 0 && n < FRAME_MIN) {
        g_st.dropped_short++;
        return;
    }
    int idx = client_touch(&from, now);
    if (idx < 0) {
        g_st.dropped_full++;
        return;
    }
    if (n == 0) {
        g_st.hellos++;
        return;
    }
    forward(idx, buf, (size_t)n, now);
}

static void handle_tap(uint8_t *buf)
{
    ssize_t n = read(g_tap_fd, buf, BUF_SIZE);
    if (n < 0)
        return;
    if (n > FRAME_MAX) {
        g_st.dropped_long++;
        return;
    }
    if (n < FRAME_MIN) {
        g_st.dropped_short++;
        return;
    }
    time_t now = now_s();
    g_clients[g_tap_slot].last_seen = now;
    forward(g_tap_slot, buf, (size_t)n, now);
}

#if ETHHUB_HAVE_TAP
static int tap_open(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/* ---- misc ------------------------------------------------------------ */

static void print_stats(FILE *o)
{
    int nc = 0, nm = 0;
    for (int i = 0; i < g_max_clients; i++)
        nc += g_clients[i].used;
    for (int i = 0; i < g_max_macs; i++)
        nm += g_macs[i].used;
    fprintf(o,
            "ethhub stats: clients=%d macs=%d rx_frames=%llu rx_bytes=%llu tx_frames=%llu "
            "tx_bytes=%llu hellos=%llu dropped_short=%llu dropped_long=%llu dropped_full=%llu "
            "dropped_denied=%llu "
            "unicast=%llu flooded=%llu unknown_unicast=%llu send_errors=%llu clients_added=%llu "
            "clients_expired=%llu macs_learned=%llu macs_evicted=%llu macs_expired=%llu\n",
            nc, nm, (unsigned long long)g_st.rx_frames, (unsigned long long)g_st.rx_bytes,
            (unsigned long long)g_st.tx_frames, (unsigned long long)g_st.tx_bytes,
            (unsigned long long)g_st.hellos, (unsigned long long)g_st.dropped_short,
            (unsigned long long)g_st.dropped_long, (unsigned long long)g_st.dropped_full,
            (unsigned long long)g_st.dropped_denied,
            (unsigned long long)g_st.unicast, (unsigned long long)g_st.flooded,
            (unsigned long long)g_st.unknown_unicast, (unsigned long long)g_st.send_errors,
            (unsigned long long)g_st.clients_added, (unsigned long long)g_st.clients_expired,
            (unsigned long long)g_st.macs_learned, (unsigned long long)g_st.macs_evicted,
            (unsigned long long)g_st.macs_expired);
    fflush(o);
}

static int g_uplink = -1;

/* Register another hub as a pinned client; we hello it periodically. */
static int uplink_open(const char *spec)
{
    char host[256];
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof host) {
        fprintf(stderr, "--connect wants host:port\n");
        return -1;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int gr = getaddrinfo(host, colon + 1, &hints, &res);
    if (gr != 0 || !res) {
        fprintf(stderr, "--connect %s: %s\n", spec, gai_strerror(gr));
        return -1;
    }
    for (int i = 0; i < g_max_clients; i++) {
        if (!g_clients[i].used) {
            g_clients[i].used = true;
            g_clients[i].pinned = true;
            g_clients[i].addr = *(struct sockaddr_in *)res->ai_addr;
            g_clients[i].last_seen = now_s();
            g_uplink = i;
            freeaddrinfo(res);
            return 0;
        }
    }
    freeaddrinfo(res);
    fprintf(stderr, "no free client slot for uplink\n");
    return -1;
}

static void uplink_hello(void)
{
    if (g_uplink >= 0)
        sendto(g_sock, "", 0, 0, (const struct sockaddr *)&g_clients[g_uplink].addr,
               sizeof g_clients[g_uplink].addr);
}

static void on_sig(int s)
{
    if (s == SIGUSR1)
        g_dump = 1;
    else
        g_quit = 1;
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --listen [addr:]port   UDP listen address (default 127.0.0.1:%d).  Anything but\n"
            "                         loopback lets the network join, read broadcasts and\n"
            "                         reflect traffic off the hub: say so explicitly, e.g.\n"
            "                         --listen 0.0.0.0:%d, and add --allow\n"
            "  --allow CIDR           accept clients only from CIDR (a.b.c.d[/n]); repeatable\n"
#if ETHHUB_HAVE_TAP
            "  --tap NAME             attach Linux TAP device NAME as a port\n"
#endif
            "  --connect host:port    join another ethhub as a client (uplink; use a tree,\n"
            "                         never a loop - there is no spanning tree)\n"
            "  --client-timeout SEC   drop idle clients after SEC (default %d)\n"
            "  --mac-timeout SEC      forget MACs after SEC (default %d)\n"
            "  --max-clients N        client table size (default %d, max %d)\n"
            "  --max-macs N           MAC table size (default %d, max %d)\n"
            "  --pcap FILE            write all frames received by the hub to a pcap file\n"
            "  --verbose              log clients and frames to stderr\n"
            "Wire format: one raw Ethernet frame (14..1514 bytes) per UDP datagram;\n"
            "empty datagram = hello/keepalive.  SIGUSR1 prints counters; SIGINT/SIGTERM\n"
            "prints them and exits.\n",
            p, DEF_PORT, DEF_PORT, DEF_CLIENT_TIMEOUT, DEF_MAC_TIMEOUT, DEF_MAX_CLIENTS, HARD_MAX_CLIENTS,
            DEF_MAX_MACS, HARD_MAX_MACS);
}

static int parse_int(const char *s, int lo, int hi, const char *what)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (*s == '\0' || *e != '\0' || v < lo || v > hi) {
        fprintf(stderr, "bad %s: %s (range %d..%d)\n", what, s, lo, hi);
        exit(2);
    }
    return (int)v;
}

int main(int argc, char **argv)
{
    const char *listen_spec = NULL, *tap_name = NULL, *pcap_path = NULL,
               *connect_spec = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has = i + 1 < argc;
        if (!strcmp(a, "--listen") && has)
            listen_spec = argv[++i];
        else if (!strcmp(a, "--allow") && has) {
            if (g_nallow >= MAX_ALLOW || !parse_cidr(argv[++i], &g_allow[g_nallow])) {
                fprintf(stderr, "bad --allow %s (want a.b.c.d[/n], at most %d)\n", argv[i], MAX_ALLOW);
                return 2;
            }
            g_nallow++;
        } else if (!strcmp(a, "--tap") && has)
            tap_name = argv[++i];
        else if (!strcmp(a, "--client-timeout") && has)
            g_client_timeout = parse_int(argv[++i], 1, 86400, "client-timeout");
        else if (!strcmp(a, "--mac-timeout") && has)
            g_mac_timeout = parse_int(argv[++i], 1, 86400, "mac-timeout");
        else if (!strcmp(a, "--max-clients") && has)
            g_max_clients = parse_int(argv[++i], 1, HARD_MAX_CLIENTS, "max-clients");
        else if (!strcmp(a, "--max-macs") && has)
            g_max_macs = parse_int(argv[++i], 1, HARD_MAX_MACS, "max-macs");
        else if (!strcmp(a, "--connect") && has)
            connect_spec = argv[++i];
        else if (!strcmp(a, "--pcap") && has)
            pcap_path = argv[++i];
        else if (!strcmp(a, "--verbose"))
            g_verbose = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }

    struct sockaddr_in la;
    memset(&la, 0, sizeof la);
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(0x7F000001u) /* 127.0.0.1 */;
    la.sin_port = htons(DEF_PORT);
    if (listen_spec) {
        char host[128] = "";
        const char *colon = strrchr(listen_spec, ':');
        const char *port = listen_spec;
        if (colon) {
            size_t hl = (size_t)(colon - listen_spec);
            if (hl >= sizeof host) {
                fprintf(stderr, "bad --listen\n");
                return 2;
            }
            memcpy(host, listen_spec, hl);
            host[hl] = '\0';
            port = colon + 1;
        }
        la.sin_port = htons((uint16_t)parse_int(port, 0, 65535, "port"));
        if (host[0] && inet_pton(AF_INET, host, &la.sin_addr) != 1) {
            fprintf(stderr, "bad listen address: %s (IPv4 only)\n", host);
            return 2;
        }
    }

    g_clients = calloc((size_t)g_max_clients, sizeof *g_clients);
    g_macs = calloc((size_t)g_max_macs, sizeof *g_macs);
    uint8_t *buf = malloc(BUF_SIZE);
    if (!g_clients || !g_macs || !buf) {
        perror("malloc");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0 || bind(g_sock, (struct sockaddr *)&la, sizeof la) < 0) {
        perror("bind");
        return 1;
    }
    socklen_t ll = sizeof la;
    getsockname(g_sock, (struct sockaddr *)&la, &ll);
    int big = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);

    if (tap_name) {
#if ETHHUB_HAVE_TAP
        g_tap_fd = tap_open(tap_name);
        if (g_tap_fd < 0)
            return 1;
        g_tap_slot = 0;
        g_clients[0].used = true;
        g_clients[0].is_tap = true;
        g_clients[0].pinned = true;
        g_clients[0].last_seen = now_s();
#else
        fprintf(stderr, "--tap is only supported on Linux\n");
        return 2;
#endif
    }
    if (connect_spec && uplink_open(connect_spec) < 0)
        return 2;
    if (pcap_path)
        pcap_open(pcap_path);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig; /* no SA_RESTART: poll() must return */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Machine-readable line so wrappers/tests can learn an ephemeral port. */
    {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &la.sin_addr, ip, sizeof ip);
        fprintf(stderr, "ethhub: bound to %s:%u\n", ip, ntohs(la.sin_port));
        if ((ntohl(la.sin_addr.s_addr) >> 24) != 127)
            fprintf(stderr,
                    "ethhub: WARNING: %s is not loopback.  Anyone who can send UDP here joins the\n"
                    "ethhub: LAN, sees its broadcasts and can reflect traffic off the hub.%s\n",
                    ip, g_nallow ? "" : "  Use --allow CIDR.");
    }
    printf("ethhub listening on %u\n", ntohs(la.sin_port));
    fflush(stdout);

    struct pollfd pf[2];
    pf[0].fd = g_sock;
    pf[0].events = POLLIN;
    int npf = 1;
    if (g_tap_fd >= 0) {
        pf[1].fd = g_tap_fd;
        pf[1].events = POLLIN;
        npf = 2;
    }
    time_t last_age = now_s(), last_hello = last_age - 100;
    while (!g_quit) {
        int r = poll(pf, (nfds_t)npf, 500);
        if (r > 0) {
            if (pf[0].revents & POLLIN)
                for (int k = 0; k < 64; k++) { /* drain a burst per wakeup */
                    struct pollfd p1 = {g_sock, POLLIN, 0};
                    handle_udp(buf);
                    if (poll(&p1, 1, 0) <= 0)
                        break;
                }
            if (npf == 2 && (pf[1].revents & POLLIN))
                for (int k = 0; k < 64; k++) {
                    struct pollfd p1 = {g_tap_fd, POLLIN, 0};
                    handle_tap(buf);
                    if (poll(&p1, 1, 0) <= 0)
                        break;
                }
        }
        time_t now = now_s();
        if (now != last_age) {
            age(now);
            last_age = now;
        }
        if (g_uplink >= 0 && now - last_hello >= 5) {
            uplink_hello();
            last_hello = now;
        }
        if (g_dump) {
            g_dump = 0;
            print_stats(stderr);
        }
    }
    print_stats(stderr);
    if (g_pcap)
        fclose(g_pcap);
    free(buf);
    free(g_clients);
    free(g_macs);
    close(g_sock);
    return 0;
}
