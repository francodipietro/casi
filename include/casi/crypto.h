/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_CRYPTO_H
#define CASI_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>

#include "casi/buf.h"

/* A master key is generated locally and never enters a git object. */
#define CASI_CRYPTO_KEY_BYTES 32

typedef struct {
    bool          enabled;
    unsigned char content_key[CASI_CRYPTO_KEY_BYTES];
    unsigned char nonce_key[CASI_CRYPTO_KEY_BYTES];
    unsigned char path_key[CASI_CRYPTO_KEY_BYTES];
} casi_crypto;

/* Derives separated content, nonce, and path keys from a local master key. */
int  casi_crypto_from_key(casi_crypto *crypto,
                          const unsigned char key[CASI_CRYPTO_KEY_BYTES]);
void casi_crypto_dispose(casi_crypto *crypto);

/* Deterministic authenticated encryption. `ad` binds the ciphertext to its
 * opaque tree path, preventing a valid blob from being transplanted elsewhere. */
int casi_crypto_encrypt(const casi_crypto *crypto, const char *ad,
                        const void *plain, size_t plain_len, casi_buf *out);
int casi_crypto_decrypt(const casi_crypto *crypto, const char *ad,
                        const void *cipher, size_t cipher_len, casi_buf *out);

/* Hex HMAC-SHA256 for one logical tree component. It is deterministic but
 * opaque without the local key, so a clone does not disclose project names. */
int casi_crypto_path_component(const casi_crypto *crypto, const char *component,
                               casi_buf *out);

/* Stable non-secret identifier for invalidating local state after a key change. */
int casi_crypto_key_id(const casi_crypto *crypto, casi_buf *out);

#endif /* CASI_CRYPTO_H */
