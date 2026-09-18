/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/crypto.h"

#include "casi_test.h"

#include <string.h>

static void test_deterministic_authenticated_encryption(void)
{
    unsigned char key[CASI_CRYPTO_KEY_BYTES] = { 1 };
    casi_crypto crypto;
    casi_buf first = CASI_BUF_INIT, second = CASI_BUF_INIT, plain = CASI_BUF_INIT;

    ASSERT_OK(casi_crypto_from_key(&crypto, key));
    ASSERT_OK(casi_crypto_encrypt(&crypto, "opaque/path", "secret", 6, &first));
    ASSERT_OK(casi_crypto_encrypt(&crypto, "opaque/path", "secret", 6, &second));
    ASSERT_EQ_MEM(first.ptr, second.ptr, first.len);
    ASSERT_OK(casi_crypto_decrypt(&crypto, "opaque/path", first.ptr, first.len, &plain));
    ASSERT_EQ_STR(casi_buf_cstr(&plain), "secret");
    ASSERT_TRUE(casi_crypto_decrypt(&crypto, "other/path", first.ptr, first.len, &plain) !=
                CASI_OK);

    casi_buf_dispose(&first);
    casi_buf_dispose(&second);
    casi_buf_dispose(&plain);
    casi_crypto_dispose(&crypto);
}

static void test_path_components_are_keyed_and_stable(void)
{
    unsigned char first_key[CASI_CRYPTO_KEY_BYTES] = { 2 };
    unsigned char second_key[CASI_CRYPTO_KEY_BYTES] = { 3 };
    casi_crypto first, second;
    casi_buf one = CASI_BUF_INIT, again = CASI_BUF_INIT, changed = CASI_BUF_INIT;

    ASSERT_OK(casi_crypto_from_key(&first, first_key));
    ASSERT_OK(casi_crypto_from_key(&second, second_key));
    ASSERT_OK(casi_crypto_path_component(&first, "project-name", &one));
    ASSERT_OK(casi_crypto_path_component(&first, "project-name", &again));
    ASSERT_OK(casi_crypto_path_component(&second, "project-name", &changed));
    ASSERT_EQ_STR(casi_buf_cstr(&one), casi_buf_cstr(&again));
    ASSERT_TRUE(strcmp(casi_buf_cstr(&one), casi_buf_cstr(&changed)) != 0);
    ASSERT_EQ_INT(one.len, 64);

    casi_buf_dispose(&one);
    casi_buf_dispose(&again);
    casi_buf_dispose(&changed);
    casi_crypto_dispose(&first);
    casi_crypto_dispose(&second);
}

int main(void)
{
    int status;

    if (casi_init() != CASI_OK)
        return 1;
    RUN_TEST(test_deterministic_authenticated_encryption);
    RUN_TEST(test_path_components_are_keyed_and_stable);
    status = casi_test_report("crypto");
    casi_shutdown();
    return status;
}
