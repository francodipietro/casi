/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/crypto.h"

#include "casi/casi.h"

#include <limits.h>
#include <sodium.h>
#include <string.h>

#define CASI_CRYPTO_MAGIC "CASIENC1"
#define CASI_CRYPTO_MAGIC_LEN (sizeof(CASI_CRYPTO_MAGIC) - 1)

static int derive_key(unsigned char out[CASI_CRYPTO_KEY_BYTES],
                      const unsigned char master[CASI_CRYPTO_KEY_BYTES],
                      const char *label)
{
    if (crypto_generichash(out, CASI_CRYPTO_KEY_BYTES,
                           (const unsigned char *)label, strlen(label),
                           master, CASI_CRYPTO_KEY_BYTES) != 0)
        return casi_error_set(CASI_ERROR, "cannot derive encryption subkey");
    return CASI_OK;
}

static int put_hex(casi_buf *out, const unsigned char *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    int rc;

    casi_buf_clear(out);
    for (i = 0; i < len; i++)
        if ((rc = casi_buf_putc(out, hex[data[i] >> 4])) != CASI_OK ||
            (rc = casi_buf_putc(out, hex[data[i] & 15])) != CASI_OK)
            return rc;
    return CASI_OK;
}

int casi_crypto_from_key(casi_crypto *crypto,
                         const unsigned char key[CASI_CRYPTO_KEY_BYTES])
{
    int rc;

    if (crypto == NULL || key == NULL)
        return casi_error_set(CASI_EINVAL, "missing encryption key");
    memset(crypto, 0, sizeof(*crypto));
    if ((rc = derive_key(crypto->content_key, key, "casi content key v1")) != CASI_OK ||
        (rc = derive_key(crypto->nonce_key, key, "casi nonce key v1")) != CASI_OK ||
        (rc = derive_key(crypto->path_key, key, "casi path key v1")) != CASI_OK) {
        casi_crypto_dispose(crypto);
        return rc;
    }
    crypto->enabled = true;
    return CASI_OK;
}

void casi_crypto_dispose(casi_crypto *crypto)
{
    if (crypto != NULL)
        sodium_memzero(crypto, sizeof(*crypto));
}

int casi_crypto_encrypt(const casi_crypto *crypto, const char *ad,
                        const void *plain, size_t plain_len, casi_buf *out)
{
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    unsigned long long cipher_len;
    size_t ad_len;
    int rc;

    if (crypto == NULL || !crypto->enabled || ad == NULL ||
        (plain == NULL && plain_len != 0) || plain_len > ULLONG_MAX)
        return casi_error_set(CASI_EINVAL, "invalid encryption input");
    ad_len = strlen(ad);
    if (ad_len > ULLONG_MAX)
        return casi_error_set(CASI_EINVAL, "encryption path is too long");
    if (crypto_generichash(nonce, sizeof(nonce), plain, plain_len,
                           crypto->nonce_key, sizeof(crypto->nonce_key)) != 0)
        return casi_error_set(CASI_ERROR, "cannot derive encryption nonce");

    casi_buf_clear(out);
    if ((rc = casi_buf_put(out, CASI_CRYPTO_MAGIC, CASI_CRYPTO_MAGIC_LEN)) != CASI_OK ||
        (rc = casi_buf_put(out, nonce, sizeof(nonce))) != CASI_OK ||
        (rc = casi_buf_grow(out, plain_len + crypto_aead_xchacha20poly1305_ietf_ABYTES)) != CASI_OK)
        goto done;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            (unsigned char *)out->ptr + out->len, &cipher_len,
            plain, plain_len, (const unsigned char *)ad, ad_len, NULL,
            nonce, crypto->content_key) != 0) {
        rc = casi_error_set(CASI_ERROR, "cannot encrypt content");
        goto done;
    }
    out->len += (size_t)cipher_len;
    out->ptr[out->len] = '\0';
    rc = CASI_OK;

done:
    sodium_memzero(nonce, sizeof(nonce));
    if (rc != CASI_OK)
        casi_buf_clear(out);
    return rc;
}

int casi_crypto_decrypt(const casi_crypto *crypto, const char *ad,
                        const void *cipher, size_t cipher_len, casi_buf *out)
{
    const unsigned char *data = cipher;
    const unsigned char *nonce;
    const unsigned char *encrypted;
    unsigned long long plain_len;
    size_t ad_len, encrypted_len, header_len;
    int rc;

    if (crypto == NULL || !crypto->enabled || ad == NULL || cipher == NULL)
        return casi_error_set(CASI_EINVAL, "invalid encrypted content");
    header_len = CASI_CRYPTO_MAGIC_LEN + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (cipher_len < header_len + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        memcmp(data, CASI_CRYPTO_MAGIC, CASI_CRYPTO_MAGIC_LEN) != 0)
        return casi_error_set(CASI_EINVAL, "unsupported encrypted content format");
    ad_len = strlen(ad);
    encrypted_len = cipher_len - header_len;
    if (ad_len > ULLONG_MAX || encrypted_len > ULLONG_MAX)
        return casi_error_set(CASI_EINVAL, "encrypted content is too large");
    nonce = data + CASI_CRYPTO_MAGIC_LEN;
    encrypted = data + header_len;

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, encrypted_len - crypto_aead_xchacha20poly1305_ietf_ABYTES)) != CASI_OK)
        return rc;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            (unsigned char *)out->ptr, &plain_len, NULL, encrypted, encrypted_len,
            (const unsigned char *)ad, ad_len, nonce, crypto->content_key) != 0) {
        casi_buf_clear(out);
        return casi_error_set(CASI_EINVAL, "encrypted content cannot be authenticated");
    }
    out->len = (size_t)plain_len;
    out->ptr[out->len] = '\0';
    return CASI_OK;
}

int casi_crypto_path_component(const casi_crypto *crypto, const char *component,
                               casi_buf *out)
{
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    int rc;

    if (crypto == NULL || !crypto->enabled || component == NULL)
        return casi_error_set(CASI_EINVAL, "invalid encrypted path component");
    if (crypto_auth_hmacsha256(mac, (const unsigned char *)component,
                               strlen(component), crypto->path_key) != 0)
        return casi_error_set(CASI_ERROR, "cannot authenticate path component");
    rc = put_hex(out, mac, sizeof(mac));
    sodium_memzero(mac, sizeof(mac));
    return rc;
}

int casi_crypto_key_id(const casi_crypto *crypto, casi_buf *out)
{
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    int rc;

    if (crypto == NULL || !crypto->enabled)
        return casi_error_set(CASI_EINVAL, "encryption is not enabled");
    if (crypto_auth_hmacsha256(mac, (const unsigned char *)"casi key id v1",
                               sizeof("casi key id v1") - 1, crypto->path_key) != 0)
        return casi_error_set(CASI_ERROR, "cannot derive key identifier");
    rc = put_hex(out, mac, sizeof(mac));
    sodium_memzero(mac, sizeof(mac));
    return rc;
}
