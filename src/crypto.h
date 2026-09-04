#ifndef RGCN_CRYPTO_H
#define RGCN_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define RGCN_NONCE_LEN   12
#define RGCN_TAG_LEN     16
#define RGCN_MAGIC       "RGCN"
#define RGCN_MAGIC_LEN   4
#define RGCN_HEADER_LEN  (RGCN_MAGIC_LEN + RGCN_NONCE_LEN + RGCN_TAG_LEN)

int  rgcn_crypto_init(void);

int  rgcn_seal(const uint8_t *plaintext, size_t pt_len,
               uint8_t *out, size_t out_cap, size_t *out_len);

int  rgcn_open(const uint8_t *packet, size_t pkt_len,
               uint8_t *out, size_t out_cap, size_t *out_len);

void rgcn_random_bytes(uint8_t *dst, size_t n);
uint64_t rgcn_random_u64(void);

#endif
