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
 * Hardening (docs/features/ETHHUB.md, docs/FINDINGS.md F43..F46):
 *   - client and MAC tables are chained hash tables with an LRU list: O(1)
 *     lookup, O(1) eviction, ageing cost proportional to what expires;
 *   - a MAC owned by a live client cannot be taken over by another client
 *     (--mac-move=deny, the default), see mac_move_denied();
 *   - per-client token buckets (frames/s, bytes/s) and a cap on the bytes a
 *     client can make the hub flood (broadcast amplification);
 *   - a full client table evicts an idle client instead of refusing a new one,
 *     and one source IP can only hold --max-per-ip clients;
 *   - optional shared-secret authentication (--key-file): a keyed hello
 *     replaces the empty hello; datagrams from addresses that have not sent
 *     one are dropped.  Without a key nothing changes on the wire.
 *
 * Security: without --key-file ANY datagram from ANY source that reaches the
 * socket makes that source a client, and broadcasts are flooded to every
 * client.  A hub that listens beyond loopback can therefore be used for UDP
 * reflection and lets anyone on the network read the LAN's broadcast traffic.
 * So the default listen address is 127.0.0.1; anything wider needs an explicit
 * --listen ADDR:PORT, prints a warning, and should be limited with --allow
 * CIDR and --key-file.  The key stops casual LAN abuse, not an attacker who
 * can read or spoof the client's traffic, and nothing is encrypted: do not
 * expose the hub to the internet.
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
#include <sys/stat.h>
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
#define DEF_LIVE_WINDOW 15 /* three 5 s keepalives */
#define DEF_MAX_CLIENTS 64
#define DEF_MAX_MACS 1024
#define DEF_MAX_PER_IP 16 /* non-loopback sources; loopback is unlimited unless --max-per-ip is given */
#define DEF_MAX_MACS_PER_CLIENT 8
#define MAC_EVICT_SCAN 32 /* how far from the LRU end to look for an evictable MAC */
#define DEF_HINT_AFTER 30
#define HARD_MAX_CLIENTS 4096
#define HARD_MAX_MACS 4096
#define MAX_ALLOW 16

#define DEF_RATE_FPS 10000
#define DEF_RATE_BPS 12500000 /* 100 Mbit/s */
#define DEF_FLOOD_BPS 2000000 /* egress bytes/s one client may cause by flooding */
#define GLOBAL_FLOOD_MULT 4   /* the whole hub: this many times the per-client rate */

/* Keyed hello: 13 bytes (inside the 1..13 range no Ethernet frame can use, so it
 * can never be mistaken for a frame): magic, uint32 BE unix seconds, then the
 * first 8 bytes of HMAC-SHA256(key, "ethhub-hello-v1" || magic || seconds). */
#define AUTH_MAGIC 0xE7
#define AUTH_HELLO_LEN 13
#define AUTH_TAG_LEN 8
#define AUTH_CTX "ethhub-hello-v1"
#define DEF_AUTH_WINDOW 60
#define KEY_MIN 16
#define KEY_MAX 1024

typedef struct {
    double tok;
    int64_t t_ns;
} Bucket;

typedef struct {
    int prev, next;
} Link;

typedef struct {
    int head, tail;
} Lru;

typedef struct {
    bool used;
    bool is_tap;
    bool pinned; /* TAP / uplink: never ages out, not in the LRU list */
    bool trunk;  /* a source listed with --trunk (another hub): treated like a pinned port, but ages out */
    struct sockaddr_in addr;
    time_t last_seen; /* any datagram; with a key, only a valid keyed hello */
    Bucket frames, bytes, flood;
    Lru mlist; /* this client's MAC entries, most recently seen first */
    int nmacs;
    uint32_t auth_ts; /* timestamp of the newest accepted keyed hello */
    bool has_ts;
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
    uint64_t dropped_unauth, dropped_rate, dropped_flood, dropped_macmove, dropped_ip_cap;
    uint64_t clients_evicted, dropped_macfull, dropped_replay;
} Stats;

typedef struct {
    uint32_t net, mask; /* host order */
    uint16_t port;      /* --trunk only: 0 = any port */
} AllowNet;

static AllowNet g_allow[MAX_ALLOW];
static int g_nallow;
static AllowNet g_trunk[MAX_ALLOW];
static int g_ntrunk;
static int g_max_clients = DEF_MAX_CLIENTS;
static int g_max_macs = DEF_MAX_MACS;
static int g_max_per_ip = -1; /* -1: DEF_MAX_PER_IP, but loopback unlimited; 0 = unlimited; N = N for everyone */
static int g_max_macs_per_client = DEF_MAX_MACS_PER_CLIENT; /* 0 = unlimited; TAP/uplink are exempt */
static int g_hint_after = DEF_HINT_AFTER;
static int g_client_timeout = DEF_CLIENT_TIMEOUT;
static int g_mac_timeout = DEF_MAC_TIMEOUT;
static int g_live_window = DEF_LIVE_WINDOW;
static bool g_mac_move_allow; /* --mac-move=allow */

static bool g_rate_on = true;
static double g_rate_fps = DEF_RATE_FPS, g_rate_bps = DEF_RATE_BPS, g_flood_bps = DEF_FLOOD_BPS;
static double g_burst_f, g_burst_b, g_burst_flood; /* derived by rate_setup() */
static Bucket g_gflood;
static int g_opt_flood = DEF_FLOOD_BPS; /* --flood-rate, bytes/s */

static size_t g_key_len; /* 0 = no authentication */
static int g_auth_window = DEF_AUTH_WINDOW;

static Client *g_clients;
static MacEntry *g_macs;
static Link *g_clru, *g_mlru, *g_mclru; /* client LRU, MAC LRU, per-client MAC lists */
static Lru g_clist = {-1, -1}, g_mlist = {-1, -1};
static int *g_chnext, *g_mhnext; /* hash chain links */
static int *g_cbuck, *g_mbuck;   /* hash buckets (-1 = empty) */
static uint32_t g_cmask, g_mmask;
static int *g_cfree, *g_mfree; /* free slot stacks */
static int g_ncfree, g_nmfree;
static uint32_t g_seed;
static uint8_t g_buf[BUF_SIZE]; /* receive buffer */

static int g_sock = -1;
static int g_tap_fd = -1;
static int g_tap_slot = -1;
static int g_uplink = -1;
static bool g_uplink_rx, g_uplink_hinted;
static time_t g_uplink_t0;
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

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

/* ---- SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104) -------------------------- */

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
} Sha256;

static Sha256 g_hin, g_hout; /* HMAC state after the key pads, set once by hmac_key_setup() */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        const uint8_t *q = p + 4 * i;
        w[i] = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | (uint32_t)q[3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6],
             h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha256_init(Sha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(Sha256 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n > 0) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_final(Sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        sha256_update(s, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lb, 8);
    for (size_t i = 0; i < 8; i++) {
        uint8_t *q = out + 4 * i;
        q[0] = (uint8_t)(s->h[i] >> 24);
        q[1] = (uint8_t)(s->h[i] >> 16);
        q[2] = (uint8_t)(s->h[i] >> 8);
        q[3] = (uint8_t)s->h[i];
    }
}

static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

/* Absorb the HMAC key pads once; per hello only the message is hashed. */
static void hmac_key_setup(const uint8_t *key, size_t klen)
{
    uint8_t k[64] = {0}, pad[64];
    if (klen > 64) {
        Sha256 s;
        sha256_init(&s);
        sha256_update(&s, key, klen);
        sha256_final(&s, k);
        secure_zero(&s, sizeof s);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&g_hin);
    sha256_update(&g_hin, pad, 64);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha256_init(&g_hout);
    sha256_update(&g_hout, pad, 64);
    secure_zero(k, sizeof k);
    secure_zero(pad, sizeof pad);
}

static void hmac_fast(const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t inner[32];
    Sha256 s = g_hin;
    sha256_update(&s, msg, mlen);
    sha256_final(&s, inner);
    s = g_hout;
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
    secure_zero(inner, sizeof inner);
}

/* ---- authentication (keyed hello) ------------------------------------------ */

static void auth_tag(const uint8_t hdr[5], uint8_t tag[AUTH_TAG_LEN])
{
    uint8_t msg[sizeof AUTH_CTX - 1 + 5], full[32];
    memcpy(msg, AUTH_CTX, sizeof AUTH_CTX - 1);
    memcpy(msg + sizeof AUTH_CTX - 1, hdr, 5);
    hmac_fast(msg, sizeof msg, full);
    memcpy(tag, full, AUTH_TAG_LEN);
    secure_zero(full, sizeof full);
}

static void auth_hello_make(uint8_t out[AUTH_HELLO_LEN])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t t = (uint32_t)ts.tv_sec;
    out[0] = AUTH_MAGIC;
    out[1] = (uint8_t)(t >> 24);
    out[2] = (uint8_t)(t >> 16);
    out[3] = (uint8_t)(t >> 8);
    out[4] = (uint8_t)t;
    auth_tag(out, out + 5);
}

static bool auth_hello_ok(const uint8_t *p)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t t = (int64_t)((uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4]);
    int64_t d = (int64_t)(uint32_t)ts.tv_sec - t; /* 32-bit seconds: fine until 2106 */
    if (d < -g_auth_window || d > g_auth_window)
        return false;
    uint8_t tag[AUTH_TAG_LEN];
    auth_tag(p, tag);
    uint8_t diff = 0;
    for (int i = 0; i < AUTH_TAG_LEN; i++)
        diff |= (uint8_t)(tag[i] ^ p[5 + i]);
    return diff == 0;
}

static bool is_auth_hello_shape(const uint8_t *p, ssize_t n)
{
    return n == AUTH_HELLO_LEN && p[0] == AUTH_MAGIC;
}

/* The key file must be private to its owner (like an ssh key) and owned by the
 * user running the hub: the secret is all there is, so refuse to run with one
 * that others can read or write.  Opened non-blocking so a FIFO cannot hang us. */
static int load_key(const char *path)
{
    uint8_t buf[KEY_MAX + 1];
    int rc = -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        fprintf(stderr, "--key-file %s: not a regular file\n", path);
    else if (st.st_uid != geteuid())
        fprintf(stderr, "--key-file %s: owned by uid %u, not by the user running ethhub (uid %u)\n", path,
                (unsigned)st.st_uid, (unsigned)geteuid());
    else if (st.st_mode & 077)
        fprintf(stderr, "--key-file %s: permissions %04o are too open (group/other can access it); chmod 600\n",
                path, (unsigned)(st.st_mode & 0777));
    else {
        ssize_t n = read(fd, buf, sizeof buf);
        size_t len = n < 0 ? 0 : (size_t)n;
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            len--;
        if (n < 0)
            perror(path);
        else if (len > KEY_MAX)
            fprintf(stderr, "--key-file %s: longer than %d bytes\n", path, KEY_MAX);
        else if (len < KEY_MIN)
            fprintf(stderr, "--key-file %s: key is %zu bytes, need at least %d\n", path, len, KEY_MIN);
        else {
            if (memcmp(buf, buf + 1, len - 1) == 0)
                fprintf(stderr, "ethhub: WARNING: --key-file %s is the same byte repeated: anyone can guess it\n",
                        path);
            hmac_key_setup(buf, len);
            g_key_len = len;
            rc = 0;
        }
    }
    close(fd);
    secure_zero(buf, sizeof buf);
    return rc;
}

/* ---- hashing, LRU lists ---------------------------------------------------- */

static uint32_t hash_bytes(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u ^ g_seed;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

static void seed_init(void)
{
    uint32_t s = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(&s, sizeof s, 1, f) != 1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        s = (uint32_t)ts.tv_nsec ^ (uint32_t)getpid() * 2654435761u;
    }
    if (f)
        fclose(f);
    g_seed = s;
}

static uint32_t pow2_buckets(int n)
{
    uint32_t b = 16;
    while (b < (uint32_t)n * 2)
        b <<= 1;
    return b;
}

static void lru_unlink(Link *l, Lru *h, int i)
{
    if (l[i].prev >= 0)
        l[l[i].prev].next = l[i].next;
    else
        h->head = l[i].next;
    if (l[i].next >= 0)
        l[l[i].next].prev = l[i].prev;
    else
        h->tail = l[i].prev;
}

static void lru_push_front(Link *l, Lru *h, int i)
{
    l[i].prev = -1;
    l[i].next = h->head;
    if (h->head >= 0)
        l[h->head].prev = i;
    h->head = i;
    if (h->tail < 0)
        h->tail = i;
}

static void lru_touch(Link *l, Lru *h, int i)
{
    if (h->head != i) {
        lru_unlink(l, h, i);
        lru_push_front(l, h, i);
    }
}

/* A port that carries a whole LAN rather than one host: the TAP, the uplink, and
 * any client listed with --trunk.  It is not rate limited, holds any number of
 * MACs, may take a MAC from a guest that moved, and its MACs count as live. */
static bool is_trunk(const Client *c)
{
    return c->pinned || c->trunk;
}

static bool trunk_match(const struct sockaddr_in *a)
{
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    for (int i = 0; i < g_ntrunk; i++)
        if ((ip & g_trunk[i].mask) == g_trunk[i].net && (g_trunk[i].port == 0 || g_trunk[i].port == ntohs(a->sin_port)))
            return true;
    return false;
}

static void bucket_init(Bucket *b, double burst, int64_t t)
{
    b->tok = burst;
    b->t_ns = t;
}

/* Refill by elapsed time, then take `cost` if available. */
static bool bucket_take(Bucket *b, double rate, double burst, double cost, int64_t t)
{
    double dt = (double)(t - b->t_ns) / 1e9;
    if (dt < 0)
        dt = 0;
    b->t_ns = t;
    b->tok += dt * rate;
    if (b->tok > burst)
        b->tok = burst;
    if (b->tok < cost)
        return false;
    b->tok -= cost;
    return true;
}

static void rate_setup(void)
{
    g_flood_bps = g_opt_flood;
    g_burst_f = g_rate_fps / 4 < 8 ? 8 : g_rate_fps / 4;
    g_burst_b = g_rate_bps / 4 < FRAME_MAX ? FRAME_MAX : g_rate_bps / 4;
    g_burst_flood = g_flood_bps / 2 < FRAME_MAX ? FRAME_MAX : g_flood_bps / 2;
    bucket_init(&g_gflood, g_burst_flood * GLOBAL_FLOOD_MULT, now_ns());
}

/* ---- MAC table ------------------------------------------------------------- */

static int mac_find(const uint8_t *mac)
{
    for (int i = g_mbuck[hash_bytes(mac, 6) & g_mmask]; i >= 0; i = g_mhnext[i])
        if (memcmp(g_macs[i].mac, mac, 6) == 0)
            return i;
    return -1;
}

static void mac_release(int i)
{
    int *pp = &g_mbuck[hash_bytes(g_macs[i].mac, 6) & g_mmask];
    while (*pp != i)
        pp = &g_mhnext[*pp];
    *pp = g_mhnext[i];
    Client *o = &g_clients[g_macs[i].client];
    lru_unlink(g_mclru, &o->mlist, i);
    o->nmacs--;
    lru_unlink(g_mlru, &g_mlist, i);
    g_macs[i].used = false;
    g_mfree[g_nmfree++] = i;
}

static void macs_purge_client(int idx)
{
    while (g_clients[idx].mlist.head >= 0)
        mac_release(g_clients[idx].mlist.head);
}

static void mac_attach(int i, int client)
{
    Client *c = &g_clients[client];
    g_macs[i].client = client;
    lru_push_front(g_mclru, &c->mlist, i);
    c->nmacs++;
}

/* Is the owner of a MAC still there?  A client is live while it keeps talking to
 * the hub (any datagram, a keepalive hello included; with a key, a keyed hello)
 * within --live-window.  The TAP and the uplink are trunks: a quiet host stack
 * or a quiet remote hub is still there, so they always count as live and their
 * entries only go away with --mac-timeout. */
static bool owner_live(int owner, time_t now)
{
    const Client *o = &g_clients[owner];
    return o->used && (is_trunk(o) || now - o->last_seen < g_live_window);
}

/* A full table gives up a MAC only if its owner is gone, or it is the claimant's
 * own, or the claimant is a trunk.  Otherwise a client sending many source MACs
 * could push a live client's entries out and then take them.  Looks a bounded
 * distance from the LRU end; -1 = nothing may go. */
static int mac_victim(int claimant, time_t now)
{
    int steps = 0;
    for (int m = g_mlist.tail; m >= 0 && steps < MAC_EVICT_SCAN; m = g_mlru[m].prev, steps++) {
        int ow = g_macs[m].client;
        if (is_trunk(&g_clients[claimant]) || ow == claimant || !owner_live(ow, now))
            return m;
    }
    return -1;
}

static int mac_alloc(int client, time_t now)
{
    if (g_nmfree == 0) {
        int v = mac_victim(client, now);
        if (v < 0)
            return -1;
        mac_release(v);
        g_st.macs_evicted++;
    }
    return g_mfree[--g_nmfree];
}

/* A client (not a trunk) holds at most --max-macs-per-client entries; the next
 * one replaces its own least recently seen. */
static void mac_make_room_for(int client)
{
    Client *c = &g_clients[client];
    if (!is_trunk(c) && g_max_macs_per_client > 0 && c->nmacs >= g_max_macs_per_client) {
        mac_release(c->mlist.tail);
        g_st.macs_evicted++;
    }
}

/* false: the table is full of live clients' MACs and this frame must be dropped. */
static bool mac_learn(const uint8_t *mac, int client, time_t now)
{
    if (mac[0] & 1) /* never learn multicast/broadcast sources */
        return true;
    int i = mac_find(mac);
    if (i < 0) {
        mac_make_room_for(client);
        i = mac_alloc(client, now);
        if (i < 0) {
            g_st.dropped_macfull++;
            return false;
        }
        memcpy(g_macs[i].mac, mac, 6);
        g_macs[i].used = true;
        uint32_t b = hash_bytes(mac, 6) & g_mmask;
        g_mhnext[i] = g_mbuck[b];
        g_mbuck[b] = i;
        lru_push_front(g_mlru, &g_mlist, i);
        mac_attach(i, client);
        g_st.macs_learned++;
    } else {
        lru_touch(g_mlru, &g_mlist, i);
        if (g_macs[i].client != client) {
            mac_make_room_for(client);
            Client *old = &g_clients[g_macs[i].client];
            lru_unlink(g_mclru, &old->mlist, i);
            old->nmacs--;
            mac_attach(i, client);
        } else {
            lru_touch(g_mclru, &g_clients[client].mlist, i);
        }
    }
    g_macs[i].last_seen = now;
    return true;
}

/* A MAC learned on a live client cannot move to another client (see owner_live()).
 * A guest that restarts on a new UDP port is locked out for at most the live
 * window after its old process stopped; an attacker is locked out for as long as
 * the owner is up.  A trunk (TAP, uplink) may always take a MAC: it is the way
 * back to a guest that moved to the other side. */
static bool mac_move_denied(const uint8_t *src, int from, time_t now)
{
    if (g_mac_move_allow || (src[0] & 1) || is_trunk(&g_clients[from]))
        return false;
    int m = mac_find(src);
    if (m < 0)
        return false;
    int owner = g_macs[m].client;
    return owner != from && owner_live(owner, now);
}

/* ---- client table ---------------------------------------------------------- */

static void client_key(const struct sockaddr_in *a, uint8_t k[6])
{
    memcpy(k, &a->sin_addr.s_addr, 4);
    memcpy(k + 4, &a->sin_port, 2);
}

static uint32_t client_bucket(const struct sockaddr_in *a)
{
    uint8_t k[6];
    client_key(a, k);
    return hash_bytes(k, 6) & g_cmask;
}

static int client_find(const struct sockaddr_in *a)
{
    for (int i = g_cbuck[client_bucket(a)]; i >= 0; i = g_chnext[i]) {
        const Client *c = &g_clients[i];
        if (c->addr.sin_addr.s_addr == a->sin_addr.s_addr && c->addr.sin_port == a->sin_port)
            return i;
    }
    return -1;
}

static void client_release(int i)
{
    Client *c = &g_clients[i];
    macs_purge_client(i);
    if (!c->is_tap) {
        int *pp = &g_cbuck[client_bucket(&c->addr)];
        while (*pp != i)
            pp = &g_chnext[*pp];
        *pp = g_chnext[i];
    }
    if (!c->pinned)
        lru_unlink(g_clru, &g_clist, i);
    c->used = false;
    g_cfree[g_ncfree++] = i;
}

/* Take a free slot; the caller has checked there is one. */
static int client_new(const struct sockaddr_in *a, bool is_tap, bool pinned, time_t now)
{
    int i = g_cfree[--g_ncfree];
    Client *c = &g_clients[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->is_tap = is_tap;
    c->pinned = pinned;
    c->trunk = a && trunk_match(a);
    c->mlist.head = c->mlist.tail = -1;
    c->last_seen = now;
    if (a)
        c->addr = *a;
    int64_t t = now_ns();
    bucket_init(&c->frames, g_burst_f, t);
    bucket_init(&c->bytes, g_burst_b, t);
    bucket_init(&c->flood, g_burst_flood, t);
    if (!is_tap) {
        uint32_t b = client_bucket(&c->addr);
        g_chnext[i] = g_cbuck[b];
        g_cbuck[b] = i;
    }
    if (!pinned)
        lru_push_front(g_clru, &g_clist, i);
    return i;
}

static int clients_from_ip(const struct sockaddr_in *a)
{
    int n = 0;
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].used && !g_clients[i].is_tap &&
            g_clients[i].addr.sin_addr.s_addr == a->sin_addr.s_addr)
            n++;
    return n;
}

/* Clients allowed per source IP: --max-per-ip when given, else 16, except that
 * loopback (every local emulator, test rig and Docker-published port seen from
 * the host) is unlimited.  0 = no limit. */
static int per_ip_limit(const struct sockaddr_in *a)
{
    if (g_max_per_ip >= 0)
        return g_max_per_ip;
    return (ntohl(a->sin_addr.s_addr) >> 24) == 127 ? 0 : DEF_MAX_PER_IP;
}

/* One line per kind per 10 s: a flood must not become a log flood. */
static void log_refusal(time_t *last, const char *fmt, const struct sockaddr_in *a, int n)
{
    time_t t = now_s();
    if (*last != 0 && t - *last < 10)
        return;
    *last = t ? t : 1;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    fprintf(stderr, fmt, ip, n);
}

#define CLIENT_FULL (-1)
#define CLIENT_IP_CAP (-2)

/* Find or create.  A full table evicts the least recently seen client, but only
 * one that has been idle for the live window: a client that keeps its hellos
 * going is never pushed out by newcomers.  Otherwise CLIENT_FULL. */
static int client_touch(const struct sockaddr_in *a, time_t now)
{
    int idx = client_find(a);
    if (idx >= 0) {
        g_clients[idx].last_seen = now;
        if (!g_clients[idx].pinned)
            lru_touch(g_clru, &g_clist, idx);
        return idx;
    }
    static time_t last_ip_log, last_full_log;
    int lim = per_ip_limit(a);
    if (lim > 0 && clients_from_ip(a) >= lim) {
        log_refusal(&last_ip_log,
                    "ethhub: refusing a client from %s: it already has %d clients (--max-per-ip; 0 = no limit)\n", a,
                    lim);
        return CLIENT_IP_CAP;
    }
    if (g_ncfree == 0) {
        int v = g_clist.tail;
        if (v < 0 || now - g_clients[v].last_seen < g_live_window) {
            log_refusal(&last_full_log, "ethhub: client table full, refusing %s (--max-clients %d)\n", a,
                        g_max_clients);
            return CLIENT_FULL;
        }
        if (g_verbose) {
            char s[64];
            fmt_client(&g_clients[v], s, sizeof s);
            fprintf(stderr, "client - %s (evicted, idle)\n", s);
        }
        client_release(v);
        g_st.clients_evicted++;
    }
    idx = client_new(a, false, false, now);
    g_st.clients_added++;
    if (g_verbose) {
        char s[64];
        fmt_client(&g_clients[idx], s, sizeof s);
        fprintf(stderr, "client + %s\n", s);
    }
    return idx;
}

/* ---- ageing ---------------------------------------------------------------- */

/* Both lists are ordered by last_seen, so ageing stops at the first live entry. */
static void age(time_t now)
{
    while (g_clist.tail >= 0 && now - g_clients[g_clist.tail].last_seen >= g_client_timeout) {
        int i = g_clist.tail;
        if (g_verbose) {
            char s[64];
            fmt_client(&g_clients[i], s, sizeof s);
            fprintf(stderr, "client - %s (timeout)\n", s);
        }
        client_release(i);
        g_st.clients_expired++;
    }
    /* An entry lives while its owner keeps talking to the hub: its own age or its
     * owner's, whichever is newer (a trunk has no such clock, only the entry). */
    int steps = 0;
    while (g_mlist.tail >= 0 && steps++ <= g_max_macs) {
        int m = g_mlist.tail;
        MacEntry *e = &g_macs[m];
        if (now - e->last_seen < g_mac_timeout)
            break;
        const Client *o = &g_clients[e->client];
        if (!is_trunk(o) && o->last_seen > e->last_seen && now - o->last_seen < g_mac_timeout) {
            e->last_seen = o->last_seen;
            lru_touch(g_mlru, &g_mlist, m);
            continue;
        }
        mac_release(m);
        g_st.macs_expired++;
    }
}

/* ---- forwarding ------------------------------------------------------------ */

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

/* Per-client frame and byte budget for a datagram of `len` bytes. */
static bool rate_admit(int idx, size_t len)
{
    if (!g_rate_on || is_trunk(&g_clients[idx])) /* trunks carry a whole LAN: not limited */
        return true;
    Client *c = &g_clients[idx];
    int64_t t = now_ns();
    return bucket_take(&c->frames, g_rate_fps, g_burst_f, 1, t) &&
           bucket_take(&c->bytes, g_rate_bps, g_burst_b, (double)len, t);
}

/* Charge a flood (len bytes to `copies` ports) to the sender and to the hub as a
 * whole.  The cost is capped at the bucket depth so a big fan-out of a legit
 * frame is delayed, never impossible. */
static bool flood_admit(int from, size_t len, int copies)
{
    if (!g_rate_on || g_flood_bps <= 0 || is_trunk(&g_clients[from]))
        return true;
    double cost = (double)len * (double)copies;
    double cc = cost > g_burst_flood ? g_burst_flood : cost;
    double gc = cost > g_burst_flood * GLOBAL_FLOOD_MULT ? g_burst_flood * GLOBAL_FLOOD_MULT : cost;
    int64_t t = now_ns();
    Bucket own = g_clients[from].flood;
    if (!bucket_take(&own, g_flood_bps, g_burst_flood, cc, t))
        return false;
    /* The hub-wide bucket is only charged to senders that have used more than
     * half of their own budget, so heavy senders can run it dry but a quiet
     * client's ARP or name lookup is never refused because of them. */
    if (own.tok < g_burst_flood / 2) {
        Bucket glob = g_gflood;
        if (!bucket_take(&glob, g_flood_bps * GLOBAL_FLOOD_MULT, g_burst_flood * GLOBAL_FLOOD_MULT, gc, t))
            return false;
        g_gflood = glob;
    }
    g_clients[from].flood = own;
    return true;
}

static void flood(int from, const uint8_t *f, size_t len)
{
    int copies = 0;
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].used && i != from)
            copies++;
    if (!flood_admit(from, len, copies)) {
        g_st.dropped_flood++;
        return;
    }
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i].used && i != from)
            send_to(i, f, len);
}

static void forward(int from, const uint8_t *f, size_t len, time_t now)
{
    g_st.rx_frames++;
    g_st.rx_bytes += len;
    pcap_write(f, len);
    if (mac_move_denied(f + 6, from, now)) {
        g_st.dropped_macmove++;
        if (g_verbose) {
            char s[18];
            fmt_mac(f + 6, s);
            fprintf(stderr, "frame from %s dropped: MAC owned by a live client\n", s);
        }
        return;
    }
    if (!mac_learn(f + 6, from, now))
        return;

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
    flood(from, f, len);
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
    out->port = 0;
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

/* A keyed hello is the only thing that makes or refreshes a client on a keyed
 * hub.  It must carry a timestamp newer than the last one accepted from that
 * address, so replaying a captured hello cannot keep anybody alive, and it is
 * answered with an empty datagram so the client knows the key and clock are
 * right. */
static int admit_keyed_hello(const struct sockaddr_in *from, const uint8_t *buf, time_t now)
{
    if (!auth_hello_ok(buf)) {
        g_st.dropped_unauth++;
        return -3;
    }
    uint32_t ts = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 8 | buf[4];
    int idx = client_find(from);
    if (idx >= 0 && g_clients[idx].has_ts && ts <= g_clients[idx].auth_ts) {
        g_st.dropped_replay++;
        return -3;
    }
    idx = client_touch(from, now);
    if (idx >= 0) {
        g_clients[idx].auth_ts = ts;
        g_clients[idx].has_ts = true;
        (void)sendto(g_sock, "", 0, 0, (const struct sockaddr *)from, sizeof *from);
    }
    return idx;
}

/* Which client does this datagram belong to?  >= 0 a client; < 0 dropped and
 * counted.  Without a key any source may join; with one only a valid keyed
 * hello can create a client, and anything else must come from a known address
 * without refreshing it. */
static int admit_source(const struct sockaddr_in *from, const uint8_t *buf, ssize_t n, time_t now)
{
    if (g_key_len == 0)
        return client_touch(from, now);
    if (is_auth_hello_shape(buf, n))
        return admit_keyed_hello(from, buf, now);
    int idx = client_find(from);
    if (idx < 0)
        g_st.dropped_unauth++;
    return idx >= 0 ? idx : -3;
}

static bool from_uplink(const struct sockaddr_in *a)
{
    return g_uplink >= 0 && g_clients[g_uplink].addr.sin_addr.s_addr == a->sin_addr.s_addr &&
           g_clients[g_uplink].addr.sin_port == a->sin_port;
}

/* Size, allow-list and uplink bookkeeping before a datagram is looked at any
 * further; false = dropped (and counted). */
static bool prefilter(const struct sockaddr_in *from, const uint8_t *buf, ssize_t n)
{
    if (!allowed(from)) {
        g_st.dropped_denied++;
        return false;
    }
    if (from_uplink(from))
        g_uplink_rx = true;
    if (n > FRAME_MAX) {
        g_st.dropped_long++;
        return false;
    }
    if (n > 0 && n < FRAME_MIN && !(g_key_len && is_auth_hello_shape(buf, n))) {
        g_st.dropped_short++;
        return false;
    }
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
    if (!prefilter(&from, buf, n))
        return;
    bool keyed_hello = g_key_len && is_auth_hello_shape(buf, n);
    int idx = admit_source(&from, buf, n, now);
    if (idx == CLIENT_FULL) {
        g_st.dropped_full++;
        return;
    }
    if (idx == CLIENT_IP_CAP) {
        g_st.dropped_ip_cap++;
        return;
    }
    if (idx < 0)
        return;
    if (n == 0 && g_key_len) /* a keyed hub is kept alive by keyed hellos only (acks from an uplink land here) */
        return;
    if (n == 0 || keyed_hello) {
        g_st.hellos++;
        return;
    }
    if (!rate_admit(idx, (size_t)n)) {
        g_st.dropped_rate++;
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
    if (!rate_admit(g_tap_slot, (size_t)n)) {
        g_st.dropped_rate++;
        return;
    }
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

/* ---- misc ------------------------------------------------------------------ */

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
            "clients_expired=%llu macs_learned=%llu macs_evicted=%llu macs_expired=%llu "
            "dropped_unauth=%llu dropped_rate=%llu dropped_flood=%llu dropped_macmove=%llu "
            "dropped_ip_cap=%llu clients_evicted=%llu dropped_macfull=%llu dropped_replay=%llu\n",
            nc, nm, (unsigned long long)g_st.rx_frames, (unsigned long long)g_st.rx_bytes,
            (unsigned long long)g_st.tx_frames, (unsigned long long)g_st.tx_bytes,
            (unsigned long long)g_st.hellos, (unsigned long long)g_st.dropped_short,
            (unsigned long long)g_st.dropped_long, (unsigned long long)g_st.dropped_full,
            (unsigned long long)g_st.dropped_denied,
            (unsigned long long)g_st.unicast, (unsigned long long)g_st.flooded,
            (unsigned long long)g_st.unknown_unicast, (unsigned long long)g_st.send_errors,
            (unsigned long long)g_st.clients_added, (unsigned long long)g_st.clients_expired,
            (unsigned long long)g_st.macs_learned, (unsigned long long)g_st.macs_evicted,
            (unsigned long long)g_st.macs_expired, (unsigned long long)g_st.dropped_unauth,
            (unsigned long long)g_st.dropped_rate, (unsigned long long)g_st.dropped_flood,
            (unsigned long long)g_st.dropped_macmove, (unsigned long long)g_st.dropped_ip_cap,
            (unsigned long long)g_st.clients_evicted, (unsigned long long)g_st.dropped_macfull,
            (unsigned long long)g_st.dropped_replay);
    fflush(o);
}

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
    if (g_ncfree == 0) {
        freeaddrinfo(res);
        fprintf(stderr, "no free client slot for uplink\n");
        return -1;
    }
    g_uplink = client_new((struct sockaddr_in *)res->ai_addr, false, true, now_s());
    g_uplink_t0 = now_s();
    freeaddrinfo(res);
    return 0;
}

static void uplink_hello(void)
{
    if (g_uplink < 0)
        return;
    uint8_t h[AUTH_HELLO_LEN] = {0};
    size_t n = 0;
    if (g_key_len) {
        auth_hello_make(h);
        n = sizeof h;
    }
    (void)sendto(g_sock, h, n, 0, (const struct sockaddr *)&g_clients[g_uplink].addr,
                 sizeof g_clients[g_uplink].addr);
}

/* One hint, once: hellos have gone out and nothing has come back, which is what a
 * key mismatch (one side keyed, or a different key, or clocks apart by more than
 * --auth-window) looks like.  A quiet unkeyed LAN looks the same, hence "if". */
static void uplink_check(time_t now)
{
    if (g_uplink < 0 || g_uplink_rx || g_uplink_hinted || now - g_uplink_t0 < g_hint_after)
        return;
    g_uplink_hinted = true;
    fprintf(stderr,
            "ethhub: no traffic from the uplink in %d s.  If it is up, check that both hubs use the same\n"
            "ethhub: --key-file (or neither) and that their clocks agree within --auth-window.\n",
            g_hint_after);
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
            "usage: %s [options]   (options also as --name=value)\n"
            "  --listen [addr:]port   UDP listen address (default 127.0.0.1:%d).  Anything but\n"
            "                         loopback lets the network join, read broadcasts and\n"
            "                         reflect traffic off the hub: say so explicitly, e.g.\n"
            "                         --listen 0.0.0.0:%d, and add --allow and --key-file\n"
            "  --allow CIDR           accept clients only from CIDR (a.b.c.d[/n]); repeatable\n"
            "  --trunk ADDR           treat clients from a.b.c.d[/n] or a.b.c.d:port as trunks (another hub\n"
            "                         that carries a LAN): no rate limit, any number of MACs, may take\n"
            "                         MACs from guests; repeatable.  The TAP and --connect are always trunks\n"
            "  --key-file FILE        shared secret (%d..%d bytes, mode 0600): only addresses that\n"
            "                         sent a keyed hello may join; see docs/features/ETHHUB.md\n"
            "  --auth-window SEC      accepted clock difference of a keyed hello (default %d)\n"
#if ETHHUB_HAVE_TAP
            "  --tap NAME             attach Linux TAP device NAME as a port\n"
#endif
            "  --connect host:port    join another ethhub as a client (uplink; use a tree,\n"
            "                         never a loop - there is no spanning tree)\n"
            "  --client-timeout SEC   drop idle clients after SEC (default %d)\n"
            "  --mac-timeout SEC      forget MACs after SEC (default %d)\n"
            "  --max-clients N        client table size (default %d, max %d)\n"
            "  --max-macs N           MAC table size (default %d, max %d)\n"
            "  --max-per-ip N         clients per source IP, 0 = no limit (default %d for other hosts,\n"
            "                         none for loopback unless given)\n"
            "  --max-macs-per-client N  MACs one client may hold, 0 = no limit (default %d; TAP and\n"
            "                         uplink are exempt)\n"
            "  --live-window SEC      a client that sent anything within SEC is live: its MACs\n"
            "                         are pinned and it is not evicted (default %d)\n"
            "  --mac-move allow|deny  may a MAC move to another client while its owner is live?\n"
            "                         (default deny)\n"
            "  --hint-after SEC       say so once if an uplink is silent for SEC (default %d)\n"
            "  --rate FPS[,BPS]       per-client limit in frames/s and bytes/s (default %d,%d)\n"
            "  --flood-rate BPS       egress bytes/s one client may cause by broadcast/multicast/\n"
            "                         unknown-unicast flooding; 0 = no cap (default %d)\n"
            "  --no-rate              turn off --rate and --flood-rate\n"
            "  --pcap FILE            write all frames received by the hub to a pcap file\n"
            "  --verbose              log clients and frames to stderr\n"
            "Wire format: one raw Ethernet frame (14..1514 bytes) per UDP datagram;\n"
            "empty datagram = hello/keepalive (13-byte keyed hello with --key-file).\n"
            "SIGUSR1 prints counters; SIGINT/SIGTERM prints them and exits.\n",
            p, DEF_PORT, DEF_PORT, KEY_MIN, KEY_MAX, DEF_AUTH_WINDOW, DEF_CLIENT_TIMEOUT, DEF_MAC_TIMEOUT,
            DEF_MAX_CLIENTS, HARD_MAX_CLIENTS, DEF_MAX_MACS, HARD_MAX_MACS, DEF_MAX_PER_IP, DEF_MAX_MACS_PER_CLIENT,
            DEF_LIVE_WINDOW,
            DEF_HINT_AFTER, DEF_RATE_FPS, DEF_RATE_BPS, DEF_FLOOD_BPS);
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

static void parse_rate(const char *s)
{
    char buf[64];
    const char *comma = strchr(s, ',');
    size_t fl = comma ? (size_t)(comma - s) : strlen(s);
    if (fl >= sizeof buf) {
        fprintf(stderr, "bad --rate: %s\n", s);
        exit(2);
    }
    memcpy(buf, s, fl);
    buf[fl] = '\0';
    g_rate_fps = parse_int(buf, 1, 100000000, "rate frames/s");
    if (comma)
        g_rate_bps = parse_int(comma + 1, 1, 2000000000, "rate bytes/s");
}

typedef struct {
    const char *listen_spec, *tap_name, *pcap_path, *connect_spec, *key_path;
} Opts;

typedef struct {
    const char *name;
    int *dst;
    int lo, hi;
} IntOpt;

typedef struct {
    const char *name;
    const char **dst;
} StrOpt;

/* Match "--name value" or "--name=value".  On a match sets *v; a missing value
 * ends the program. */
static bool opt_val(int argc, char **argv, int *i, const char *name, const char **v)
{
    const char *a = argv[*i];
    size_t nl = strlen(name);
    if (!a || strncmp(a, name, nl) != 0)
        return false;
    if (a[nl] == '=') {
        *v = a + nl + 1;
        return true;
    }
    if (a[nl] != '\0')
        return false;
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", name);
        exit(2);
    }
    *v = argv[++*i];
    return true;
}

static bool opt_table(int argc, char **argv, int *i, const IntOpt *io, size_t ni, const StrOpt *so, size_t ns)
{
    const char *v;
    for (size_t k = 0; k < ni; k++)
        if (opt_val(argc, argv, i, io[k].name, &v)) {
            *io[k].dst = parse_int(v, io[k].lo, io[k].hi, io[k].name + 2);
            return true;
        }
    for (size_t k = 0; k < ns; k++)
        if (opt_val(argc, argv, i, so[k].name, &v)) {
            *so[k].dst = v;
            return true;
        }
    return false;
}

/* a.b.c.d[/n] (any port) or a.b.c.d:port (exactly that socket). */
static bool parse_trunk(const char *s, AllowNet *out)
{
    const char *colon = strchr(s, ':');
    if (!colon)
        return parse_cidr(s, out);
    char host[64];
    size_t hl = (size_t)(colon - s);
    char *e;
    long port = strtol(colon + 1, &e, 10);
    if (hl == 0 || hl >= sizeof host || colon[1] == '\0' || *e != '\0' || port < 1 || port > 65535)
        return false;
    memcpy(host, s, hl);
    host[hl] = '\0';
    if (!parse_cidr(host, out))
        return false;
    out->port = (uint16_t)port;
    return true;
}

static void parse_mac_move(const char *v)
{
    if (!strcmp(v, "allow"))
        g_mac_move_allow = true;
    else if (!strcmp(v, "deny"))
        g_mac_move_allow = false;
    else {
        fprintf(stderr, "bad --mac-move: %s (allow or deny)\n", v);
        exit(2);
    }
}

/* Options that are not a plain integer or string. */
static bool opt_special(int argc, char **argv, int *i)
{
    const char *v;
    if (opt_val(argc, argv, i, "--allow", &v)) {
        if (g_nallow >= MAX_ALLOW || !parse_cidr(v, &g_allow[g_nallow])) {
            fprintf(stderr, "bad --allow %s (want a.b.c.d[/n], at most %d)\n", v, MAX_ALLOW);
            exit(2);
        }
        g_nallow++;
    } else if (opt_val(argc, argv, i, "--trunk", &v)) {
        if (g_ntrunk >= MAX_ALLOW || !parse_trunk(v, &g_trunk[g_ntrunk])) {
            fprintf(stderr, "bad --trunk %s (want a.b.c.d[/n] or a.b.c.d:port, at most %d)\n", v, MAX_ALLOW);
            exit(2);
        }
        g_ntrunk++;
    } else if (opt_val(argc, argv, i, "--mac-move", &v))
        parse_mac_move(v);
    else if (opt_val(argc, argv, i, "--rate", &v))
        parse_rate(v);
    else if (!strcmp(argv[*i], "--verbose"))
        g_verbose = true;
    else if (!strcmp(argv[*i], "--no-rate"))
        g_rate_on = false;
    else
        return false;
    return true;
}

static void parse_args(int argc, char **argv, Opts *o)
{
    const IntOpt ints[] = {
        {"--client-timeout", &g_client_timeout, 1, 86400},
        {"--mac-timeout", &g_mac_timeout, 1, 86400},
        {"--live-window", &g_live_window, 1, 86400},
        {"--auth-window", &g_auth_window, 1, 86400},
        {"--max-clients", &g_max_clients, 1, HARD_MAX_CLIENTS},
        {"--max-macs", &g_max_macs, 1, HARD_MAX_MACS},
        {"--max-per-ip", &g_max_per_ip, 0, HARD_MAX_CLIENTS},
        {"--max-macs-per-client", &g_max_macs_per_client, 0, HARD_MAX_MACS},
        {"--hint-after", &g_hint_after, 1, 3600},
        {"--flood-rate", &g_opt_flood, 0, 2000000000},
    };
    const StrOpt strs[] = {
        {"--listen", &o->listen_spec}, {"--tap", &o->tap_name},   {"--connect", &o->connect_spec},
        {"--pcap", &o->pcap_path},     {"--key-file", &o->key_path},
    };
    for (int i = 1; i < argc; i++) {
        if (opt_table(argc, argv, &i, ints, sizeof ints / sizeof ints[0], strs, sizeof strs / sizeof strs[0]))
            continue;
        if (!opt_special(argc, argv, &i)) {
            usage(argv[0]);
            exit(2);
        }
    }
}

static bool parse_listen(const char *listen_spec, struct sockaddr_in *la)
{
    char host[128] = "";
    const char *colon = strrchr(listen_spec, ':');
    const char *port = listen_spec;
    if (colon) {
        size_t hl = (size_t)(colon - listen_spec);
        if (hl >= sizeof host) {
            fprintf(stderr, "bad --listen\n");
            return false;
        }
        memcpy(host, listen_spec, hl);
        host[hl] = '\0';
        port = colon + 1;
    }
    la->sin_port = htons((uint16_t)parse_int(port, 0, 65535, "port"));
    if (host[0] && inet_pton(AF_INET, host, &la->sin_addr) != 1) {
        fprintf(stderr, "bad listen address: %s (IPv4 only)\n", host);
        return false;
    }
    return true;
}

static bool tables_alloc(void)
{
    g_clients = calloc((size_t)g_max_clients, sizeof *g_clients);
    g_macs = calloc((size_t)g_max_macs, sizeof *g_macs);
    g_clru = calloc((size_t)g_max_clients, sizeof *g_clru);
    g_mlru = calloc((size_t)g_max_macs, sizeof *g_mlru);
    g_mclru = calloc((size_t)g_max_macs, sizeof *g_mclru);
    g_chnext = calloc((size_t)g_max_clients, sizeof *g_chnext);
    g_mhnext = calloc((size_t)g_max_macs, sizeof *g_mhnext);
    g_cfree = calloc((size_t)g_max_clients, sizeof *g_cfree);
    g_mfree = calloc((size_t)g_max_macs, sizeof *g_mfree);
    g_cmask = pow2_buckets(g_max_clients) - 1;
    g_mmask = pow2_buckets(g_max_macs) - 1;
    g_cbuck = malloc((size_t)(g_cmask + 1) * sizeof *g_cbuck);
    g_mbuck = malloc((size_t)(g_mmask + 1) * sizeof *g_mbuck);
    if (!g_clients || !g_macs || !g_clru || !g_mlru || !g_mclru || !g_chnext || !g_mhnext || !g_cfree || !g_mfree ||
        !g_cbuck || !g_mbuck)
        return false;
    memset(g_cbuck, 0xff, (size_t)(g_cmask + 1) * sizeof *g_cbuck); /* all -1 */
    memset(g_mbuck, 0xff, (size_t)(g_mmask + 1) * sizeof *g_mbuck);
    for (int i = 0; i < g_max_clients; i++)
        g_cfree[i] = g_max_clients - 1 - i; /* pop yields slot 0 first */
    g_ncfree = g_max_clients;
    for (int i = 0; i < g_max_macs; i++)
        g_mfree[i] = g_max_macs - 1 - i;
    g_nmfree = g_max_macs;
    return true;
}

static void tables_free(void)
{
    free(g_clients);
    free(g_macs);
    free(g_clru);
    free(g_mlru);
    free(g_mclru);
    free(g_chnext);
    free(g_mhnext);
    free(g_cfree);
    free(g_mfree);
    free(g_cbuck);
    free(g_mbuck);
}

static void announce(const struct sockaddr_in *la, const Opts *o)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &la->sin_addr, ip, sizeof ip);
    fprintf(stderr, "ethhub: bound to %s:%u\n", ip, ntohs(la->sin_port));
    if (g_key_len)
        fprintf(stderr, "ethhub: authentication on (key file %s): unkeyed sources are dropped\n", o->key_path);
    if ((ntohl(la->sin_addr.s_addr) >> 24) != 127)
        fprintf(stderr,
                "ethhub: WARNING: %s is not loopback.  Anyone who can send UDP here %s the\n"
                "ethhub: LAN, sees its broadcasts and can reflect traffic off the hub.%s\n",
                ip, g_key_len ? "and knows the key joins" : "joins", g_nallow ? "" : "  Use --allow CIDR.");
}

static void serve(const struct pollfd *pf, int npf, uint8_t *buf)
{
    time_t last_age = now_s(), last_hello = last_age - 100;
    struct pollfd p[2];
    memcpy(p, pf, (size_t)npf * sizeof p[0]);
    while (!g_quit) {
        int r = poll(p, (nfds_t)npf, 500);
        if (r > 0) {
            if (p[0].revents & POLLIN)
                for (int k = 0; k < 64; k++) { /* drain a burst per wakeup */
                    struct pollfd p1 = {g_sock, POLLIN, 0};
                    handle_udp(buf);
                    if (poll(&p1, 1, 0) <= 0)
                        break;
                }
            if (npf == 2 && (p[1].revents & POLLIN))
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
        uplink_check(now);
        if (g_uplink >= 0 && now - last_hello >= 5) {
            uplink_hello();
            last_hello = now;
        }
        if (g_dump) {
            g_dump = 0;
            print_stats(stderr);
        }
    }
}

static int open_tap_port(const char *tap_name)
{
#if ETHHUB_HAVE_TAP
    g_tap_fd = tap_open(tap_name);
    if (g_tap_fd < 0)
        return 1;
    g_tap_slot = client_new(NULL, true, true, now_s());
    return 0;
#else
    (void)tap_name;
    fprintf(stderr, "--tap is only supported on Linux\n");
    return 2;
#endif
}

static int open_socket(struct sockaddr_in *la)
{
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0 || bind(g_sock, (struct sockaddr *)la, sizeof *la) < 0) {
        perror("bind");
        return -1;
    }
    socklen_t ll = sizeof *la;
    getsockname(g_sock, (struct sockaddr *)la, &ll);
    int big = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    return 0;
}

static int attach_ports(const Opts *o)
{
    if (o->tap_name) {
        int rc = open_tap_port(o->tap_name);
        if (rc)
            return rc;
    }
    if (o->connect_spec && uplink_open(o->connect_spec) < 0)
        return 2;
    if (o->pcap_path)
        pcap_open(o->pcap_path);
    return 0;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig; /* no SA_RESTART: poll() must return */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv)
{
    Opts o;
    memset(&o, 0, sizeof o);
    parse_args(argc, argv, &o);

    struct sockaddr_in la;
    memset(&la, 0, sizeof la);
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(0x7F000001u) /* 127.0.0.1 */;
    la.sin_port = htons(DEF_PORT);
    if (o.listen_spec && !parse_listen(o.listen_spec, &la))
        return 2;
    if (o.key_path && load_key(o.key_path) < 0)
        return 2;

    seed_init();
    rate_setup();
    if (!tables_alloc()) {
        perror("malloc");
        return 1;
    }
    if (open_socket(&la) < 0)
        return 1;
    int rc = attach_ports(&o);
    if (rc)
        return rc;
    install_signals();

    /* Machine-readable line so wrappers/tests can learn an ephemeral port. */
    announce(&la, &o);
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
    serve(pf, npf, g_buf);
    print_stats(stderr);
    secure_zero(&g_hin, sizeof g_hin);
    secure_zero(&g_hout, sizeof g_hout);
    if (g_pcap)
        fclose(g_pcap);
    tables_free();
    close(g_sock);
    return 0;
}
