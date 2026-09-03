/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/error.h"
#include "casi/str.h"

#include "casi_test.h"

static void test_strdup_and_strndup(void)
{
    char *a = casi_strdup("abcdef");
    char *b = casi_strndup("abcdef", 3);

    ASSERT_EQ_STR(a, "abcdef");
    ASSERT_EQ_STR(b, "abc");

    free(a);
    free(b);
}

static void test_prefix_and_suffix(void)
{
    ASSERT_TRUE(casi_str_has_prefix("/Users/franco/src", "/Users"));
    ASSERT_TRUE(casi_str_has_prefix("abc", "abc"));
    ASSERT_TRUE(casi_str_has_prefix("abc", ""));
    ASSERT_FALSE(casi_str_has_prefix("abc", "abcd"));
    ASSERT_FALSE(casi_str_has_prefix("/Users", "/User/"));

    ASSERT_TRUE(casi_str_has_suffix("session.jsonl", ".jsonl"));
    ASSERT_TRUE(casi_str_has_suffix("abc", "abc"));
    ASSERT_TRUE(casi_str_has_suffix("abc", ""));
    ASSERT_FALSE(casi_str_has_suffix("abc", "abcd"));
    ASSERT_FALSE(casi_str_has_suffix("a.jsonlx", ".jsonl"));
}

static void test_strvec_push_and_dispose(void)
{
    casi_strvec v = CASI_STRVEC_INIT;
    size_t i;

    for (i = 0; i < 100; i++)
        ASSERT_OK(casi_strvec_push(&v, "entry"));

    ASSERT_EQ_INT(v.len, 100);
    ASSERT_EQ_STR(v.items[0], "entry");
    ASSERT_EQ_STR(v.items[99], "entry");

    casi_strvec_dispose(&v);
    ASSERT_EQ_INT(v.len, 0);
    ASSERT_TRUE(v.items == NULL);
}

static void test_strvec_initial_capacity(void)
{
    casi_strvec v = CASI_STRVEC_INIT;
    size_t i;

    ASSERT_OK(casi_strvec_push(&v, "one"));
    /* The first allocation is 8, not 16: growth doubles an existing capacity
     * rather than doubling the base. */
    ASSERT_EQ_INT(v.cap, 8);

    for (i = 1; i < 8; i++)
        ASSERT_OK(casi_strvec_push(&v, "filler"));
    ASSERT_EQ_INT(v.cap, 8);

    ASSERT_OK(casi_strvec_push(&v, "ninth"));
    ASSERT_EQ_INT(v.cap, 16);
    ASSERT_EQ_INT(v.len, 9);
    ASSERT_EQ_STR(v.items[8], "ninth");

    casi_strvec_dispose(&v);
}

static void test_strvec_push_owned(void)
{
    casi_strvec v = CASI_STRVEC_INIT;
    char *owned = casi_strdup("taken");

    ASSERT_OK(casi_strvec_push_owned(&v, owned));
    ASSERT_EQ_STR(v.items[0], "taken");

    /* dispose frees it: no double free below. */
    casi_strvec_dispose(&v);
}

static void test_strvec_sort_is_byte_order(void)
{
    casi_strvec v = CASI_STRVEC_INIT;

    ASSERT_OK(casi_strvec_push(&v, "memory"));
    ASSERT_OK(casi_strvec_push(&v, "Zeta"));
    ASSERT_OK(casi_strvec_push(&v, "000001"));
    ASSERT_OK(casi_strvec_push(&v, "000000"));

    casi_strvec_sort(&v);

    /* Byte order, not locale order: uppercase sorts before lowercase. That is
     * what keeps two machines walking a tree in the same sequence. */
    ASSERT_EQ_STR(v.items[0], "000000");
    ASSERT_EQ_STR(v.items[1], "000001");
    ASSERT_EQ_STR(v.items[2], "Zeta");
    ASSERT_EQ_STR(v.items[3], "memory");

    casi_strvec_dispose(&v);
}

static void test_strvec_sort_empty(void)
{
    casi_strvec v = CASI_STRVEC_INIT;

    casi_strvec_sort(&v);
    ASSERT_EQ_INT(v.len, 0);
    casi_strvec_dispose(&v);
}

int main(void)
{
    RUN_TEST(test_strdup_and_strndup);
    RUN_TEST(test_prefix_and_suffix);
    RUN_TEST(test_strvec_push_and_dispose);
    RUN_TEST(test_strvec_initial_capacity);
    RUN_TEST(test_strvec_push_owned);
    RUN_TEST(test_strvec_sort_is_byte_order);
    RUN_TEST(test_strvec_sort_empty);
    return casi_test_report("str");
}
