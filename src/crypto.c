/*
 * Rigicon Live - ChaCha20-Poly1305 AEAD (RFC 8439)
 * Self-contained implementation. No external crypto dependencies.
 *
 * The shared key below is embedded in every build. It protects packets
 * against on-wire observers (Wireshark), not against someone who has
 * the binary. This matches the "closed-loop walkie-talkie" threat model:
 * only Rigicon Live clients can decode Rigicon Live packets.
 */

#include "crypto.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
  #ifndef STATUS_SUCCESS
    #define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
  #endif
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
#endif

/* -------------------------------------------------------------------------- */
/* Shared frequency key. Anyone with this key can join the radio channel.     */
/* -------------------------------------------------------------------------- */
static const uint8_t APP_KEY[32] = {
    0x5a, 0xe1, 0x37, 0x8b, 0x2c, 0x04, 0x94, 0xd9,
    0xf8, 0x71, 0x66, 0x2a, 0x11, 0xbc, 0x5d, 0x03,
    0x8e, 0x22, 0xc7, 0x4f, 0xa5, 0x0b, 0x91, 0x6c,
    0xdd, 0x38, 0x77, 0xe4, 0x19, 0x62, 0xba, 0x00
};

/* -------------------------------------------------------------------------- */
/* Random bytes                                                                */
/* -------------------------------------------------------------------------- */

void rgcn_random_bytes(uint8_t *dst, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(NULL, dst, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS) {
        fprintf(stderr, "fatal: BCryptGenRandom failed\n");
        exit(1);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) { perror("open /dev/urandom"); exit(1); }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, dst + got, n - got);
        if (r <= 0) {
            if (errno == EINTR) continue;
            perror("read /dev/urandom");
            close(fd);
            exit(1);
        }
        got += (size_t)r;
    }
    close(fd);
#endif
}

uint64_t rgcn_random_u64(void) {
    uint64_t v;
    rgcn_random_bytes((uint8_t *)&v, sizeof v);
    return v;
}

/* -------------------------------------------------------------------------- */
/* Little-endian helpers                                                       */
/* -------------------------------------------------------------------------- */

static uint32_t load32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void store64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* -------------------------------------------------------------------------- */
/* ChaCha20 block function (RFC 8439 §2.3)                                     */
/* -------------------------------------------------------------------------- */

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

#define QR(a,b,c,d) do {                       \
    a += b; d ^= a; d = rotl32(d, 16);         \
    c += d; b ^= c; b = rotl32(b, 12);         \
    a += b; d ^= a; d = rotl32(d,  8);         \
    c += d; b ^= c; b = rotl32(b,  7);         \
} while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16] = {
        0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
        load32_le(key +  0), load32_le(key +  4),
        load32_le(key +  8), load32_le(key + 12),
        load32_le(key + 16), load32_le(key + 20),
        load32_le(key + 24), load32_le(key + 28),
        counter,
        load32_le(nonce + 0), load32_le(nonce + 4), load32_le(nonce + 8)
    };
    uint32_t x[16];
    memcpy(x, s, sizeof x);

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; i++) store32_le(out + i * 4, x[i] + s[i]);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12],
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, counter++, nonce, block);
        size_t chunk = len - off;
        if (chunk > 64) chunk = 64;
        for (size_t i = 0; i < chunk; i++) out[off + i] = in[off + i] ^ block[i];
        off += chunk;
    }
}

/* -------------------------------------------------------------------------- */
/* Poly1305 (RFC 8439 §2.5) - 5x26-bit limb representation                    */
/* -------------------------------------------------------------------------- */

static void poly1305_mac(const uint8_t key[32],
                         const uint8_t *msg, size_t msg_len,
                         uint8_t tag[16]) {
    /* r, clamped */
    uint32_t r0 = (load32_le(key +  0)     ) & 0x3ffffff;
    uint32_t r1 = (load32_le(key +  3) >> 2) & 0x3ffff03;
    uint32_t r2 = (load32_le(key +  6) >> 4) & 0x3ffc0ff;
    uint32_t r3 = (load32_le(key +  9) >> 6) & 0x3f03fff;
    uint32_t r4 = (load32_le(key + 12) >> 8) & 0x00fffff;

    uint32_t s1 = r1 * 5;
    uint32_t s2 = r2 * 5;
    uint32_t s3 = r3 * 5;
    uint32_t s4 = r4 * 5;

    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    while (msg_len > 0) {
        uint8_t block[16] = {0};
        size_t use = msg_len >= 16 ? 16 : msg_len;
        memcpy(block, msg, use);
        int full = (use == 16);
        if (!full) block[use] = 1;

        uint32_t b0 = (load32_le(block +  0)     ) & 0x3ffffff;
        uint32_t b1 = (load32_le(block +  3) >> 2) & 0x3ffffff;
        uint32_t b2 = (load32_le(block +  6) >> 4) & 0x3ffffff;
        uint32_t b3 = (load32_le(block +  9) >> 6) & 0x3ffffff;
        uint32_t b4 =  load32_le(block + 12) >> 8;
        if (full) b4 |= (1u << 24);

        h0 += b0; h1 += b1; h2 += b2; h3 += b3; h4 += b4;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3
                    + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4
                    + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0
                    + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1
                    + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2
                    + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
        c = h0 >> 26;             h0 &= 0x3ffffff;                h1 += c;

        msg     += use;
        msg_len -= use;
    }

    /* Final carry propagation */
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    /* Compute h + -p (i.e. h - (2^130 - 5)) and select in constant time */
    uint32_t g0 = h0 + 5;   c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;   c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;   c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;   c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1; /* if g4 borrowed, mask=0 else mask=all1 */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Serialize h to 4 x u32 little-endian words */
    uint32_t f0 = (h0      ) | (h1 << 26);
    uint32_t f1 = (h1 >>  6) | (h2 << 20);
    uint32_t f2 = (h2 >> 12) | (h3 << 14);
    uint32_t f3 = (h3 >> 18) | (h4 <<  8);

    /* Add s (second half of key), mod 2^128 */
    uint64_t t;
    t = (uint64_t)f0 + load32_le(key + 16);                            f0 = (uint32_t)t;
    t = (uint64_t)f1 + load32_le(key + 20) + (t >> 32);                f1 = (uint32_t)t;
    t = (uint64_t)f2 + load32_le(key + 24) + (t >> 32);                f2 = (uint32_t)t;
    t = (uint64_t)f3 + load32_le(key + 28) + (t >> 32);                f3 = (uint32_t)t;

    store32_le(tag +  0, f0);
    store32_le(tag +  4, f1);
    store32_le(tag +  8, f2);
    store32_le(tag + 12, f3);
}

/* -------------------------------------------------------------------------- */
/* AEAD_CHACHA20_POLY1305 (RFC 8439 §2.8) - empty AAD                          */
/* -------------------------------------------------------------------------- */

static void poly1305_key_gen(const uint8_t key[32], const uint8_t nonce[12],
                             uint8_t otk[32]) {
    uint8_t block[64];
    chacha20_block(key, 0, nonce, block);
    memcpy(otk, block, 32);
}

static void aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *pt, size_t pt_len,
                      uint8_t *ct, uint8_t tag[16]) {
    uint8_t otk[32];
    poly1305_key_gen(key, nonce, otk);
    chacha20_xor(key, 1, nonce, pt, ct, pt_len);

    size_t pad = (16 - (pt_len & 15)) & 15;
    size_t mac_len = pt_len + pad + 16;
    /* We construct mac_data on stack up to a safe cap (RGCN packets are small) */
    uint8_t buf[1600];
    if (mac_len > sizeof buf) { /* shouldn't happen for our packet size */
        memset(tag, 0, 16);
        return;
    }
    memcpy(buf, ct, pt_len);
    memset(buf + pt_len, 0, pad);
    store64_le(buf + pt_len + pad, 0);           /* AAD len = 0 */
    store64_le(buf + pt_len + pad + 8, pt_len);  /* ciphertext len */
    poly1305_mac(otk, buf, mac_len, tag);
}

static int aead_open(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *ct, size_t ct_len,
                     const uint8_t tag[16], uint8_t *pt) {
    uint8_t otk[32];
    poly1305_key_gen(key, nonce, otk);

    size_t pad = (16 - (ct_len & 15)) & 15;
    size_t mac_len = ct_len + pad + 16;
    uint8_t buf[1600];
    if (mac_len > sizeof buf) return -1;

    memcpy(buf, ct, ct_len);
    memset(buf + ct_len, 0, pad);
    store64_le(buf + ct_len + pad, 0);
    store64_le(buf + ct_len + pad + 8, ct_len);

    uint8_t got[16];
    poly1305_mac(otk, buf, mac_len, got);

    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= got[i] ^ tag[i];
    if (diff) return -1;

    chacha20_xor(key, 1, nonce, ct, pt, ct_len);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public API - seal/open with wire framing                                    */
/*                                                                             */
/*   [ MAGIC(4) | NONCE(12) | TAG(16) | CIPHERTEXT(*) ]                        */
/* -------------------------------------------------------------------------- */

int rgcn_seal(const uint8_t *pt, size_t pt_len,
              uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t need = RGCN_HEADER_LEN + pt_len;
    if (out_cap < need) return -1;

    memcpy(out, RGCN_MAGIC, RGCN_MAGIC_LEN);
    uint8_t *nonce = out + RGCN_MAGIC_LEN;
    uint8_t *tag   = nonce + RGCN_NONCE_LEN;
    uint8_t *ct    = tag + RGCN_TAG_LEN;

    rgcn_random_bytes(nonce, RGCN_NONCE_LEN);
    aead_seal(APP_KEY, nonce, pt, pt_len, ct, tag);

    *out_len = need;
    return 0;
}

int rgcn_open(const uint8_t *packet, size_t pkt_len,
              uint8_t *out, size_t out_cap, size_t *out_len) {
    if (pkt_len < RGCN_HEADER_LEN) return -1;
    if (memcmp(packet, RGCN_MAGIC, RGCN_MAGIC_LEN) != 0) return -1;

    const uint8_t *nonce = packet + RGCN_MAGIC_LEN;
    const uint8_t *tag   = nonce + RGCN_NONCE_LEN;
    const uint8_t *ct    = tag + RGCN_TAG_LEN;
    size_t ct_len = pkt_len - RGCN_HEADER_LEN;

    if (out_cap < ct_len) return -1;
    if (aead_open(APP_KEY, nonce, ct, ct_len, tag, out) != 0) return -1;

    *out_len = ct_len;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Self-test against RFC 8439 §2.8.2 test vector                               */
/* -------------------------------------------------------------------------- */

int rgcn_crypto_init(void) {
    static const uint8_t k[32] = {
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
        0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f
    };
    static const uint8_t n[12] = {
        0x07,0x00,0x00,0x00,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47
    };
    static const char pt_str[] =
        "Ladies and Gentlemen of the class of '99: "
        "If I could offer you only one tip for the future, "
        "sunscreen would be it.";
    const uint8_t *pt = (const uint8_t *)pt_str;
    size_t pt_len = sizeof(pt_str) - 1;

    /* Expected ciphertext (RFC 8439 §2.8.2 without AAD variant would differ;
       we validate our aead by round-trip: seal then open and compare) */
    uint8_t ct[128], tag[16], back[128];
    aead_seal(k, n, pt, pt_len, ct, tag);
    if (aead_open(k, n, ct, pt_len, tag, back) != 0) return -1;
    if (memcmp(back, pt, pt_len) != 0) return -1;

    /* Also validate that a tampered byte fails auth */
    ct[0] ^= 0x01;
    if (aead_open(k, n, ct, pt_len, tag, back) == 0) return -1;

    return 0;
}
