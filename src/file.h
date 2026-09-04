#ifndef RGCN_FILE_H
#define RGCN_FILE_H

#include <stdint.h>
#include <stddef.h>

#define RGCN_FILE_MAX_NAME    128
#define RGCN_FILE_MAX_PATH    512
#define RGCN_FILE_MAX_SIZE    (50ULL * 1024ULL * 1024ULL)   /* 50 MB hard cap */
#define RGCN_FILE_MAGIC       "RGCF"
#define RGCN_FILE_MAGIC_LEN   4

/* Open an ephemeral TCP listener for one incoming download.
 * On success, *out_port is set. Caller must close the returned socket after
 * `rgcn_file_serve_once`. */
int rgcn_file_open_listener(int *out_socket, uint16_t *out_port);

/* Blocks until one client connects, does HMAC handshake, streams the file,
 * then closes. Returns 0 on success. progress() called every ~64KB.
 *
 * Handshake protocol on connect:
 *   client -> server:  RGCF magic(4) | offer_id(8 LE) | hmac(32)
 *                      hmac = HMAC-SHA256(APP_KEY, magic || offer_id)
 *   server -> client:  status(1)  0=ok, 1=bad-hmac, 2=wrong-offer
 *   server -> client:  <file bytes...>
 */
int rgcn_file_serve_once(int listen_sock, const char *path,
                         uint64_t offer_id, uint64_t size,
                         int timeout_seconds,
                         void (*progress)(uint64_t sent, uint64_t total, void *ud),
                         void *ud);

/* Connect to the sender, do handshake, download to path, verify SHA-256. */
int rgcn_file_download(uint32_t sender_ip_be, uint16_t sender_port_be,
                       uint64_t offer_id, uint64_t expected_size,
                       const uint8_t expected_sha256[32],
                       const char *dest_path,
                       void (*progress)(uint64_t got, uint64_t total, void *ud),
                       void *ud);

/* Basename + strip path components + reject dangerous characters. Returns 0
 * on success with sanitized name in `out`. */
int rgcn_file_sanitize_name(const char *raw, char *out, size_t out_cap);

/* True if the extension looks like an executable that deserves a warning. */
int rgcn_file_ext_is_risky(const char *filename);

/* Convert bytes count to human-readable ("1.2 MB") into `out`. */
void rgcn_file_size_str(uint64_t bytes, char *out, size_t cap);

/* Get or create ~/Downloads/RigiconLive/ path. Returns 0 on success. */
int rgcn_file_download_dir(char *out, size_t cap);

#endif
