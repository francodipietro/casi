/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/buf.h"
#include "casi/error.h"

#include "casi_test.h"

static void test_empty_buf_is_usable(void)
{
    casi_buf buf = CASI_BUF_INIT;

    ASSERT_EQ_INT(buf.len, 0);
    ASSERT_EQ_STR(casi_buf_cstr(&buf), "");
    casi_buf_dispose(&buf);
}

static void test_put_and_terminate(void)
{
    casi_buf buf = CASI_BUF_INIT;

    ASSERT_OK(casi_buf_puts(&buf, "hello"));
    ASSERT_OK(casi_buf_putc(&buf, ' '));
    ASSERT_OK(casi_buf_puts(&buf, "world"));

    ASSERT_EQ_INT(buf.len, 11);
    ASSERT_EQ_STR(casi_buf_cstr(&buf), "hello world");
    /* The terminator lives one past len and is not counted. */
    ASSERT_EQ_INT(buf.ptr[buf.len], '\0');

    casi_buf_dispose(&buf);
}

static void test_binary_safe(void)
{
    casi_buf buf = CASI_BUF_INIT;
    const char raw[] = { 'a', '\0', 'b', '\n', '\0', 'c' };

    ASSERT_OK(casi_buf_put(&buf, raw, sizeof(raw)));
    ASSERT_EQ_INT(buf.len, sizeof(raw));
    ASSERT_EQ_MEM(buf.ptr, raw, sizeof(raw));

    casi_buf_dispose(&buf);
}

static void test_printf_grows(void)
{
    casi_buf buf = CASI_BUF_INIT;
    int i;

    for (i = 0; i < 500; i++)
        ASSERT_OK(casi_buf_printf(&buf, "%04d,", i));

    ASSERT_EQ_INT(buf.len, 500 * 5);
    ASSERT_EQ_MEM(buf.ptr, "0000,0001,", 10);
    ASSERT_EQ_STR(buf.ptr + buf.len - 5, "0499,");

    casi_buf_dispose(&buf);
}

static void test_clear_keeps_allocation(void)
{
    casi_buf buf = CASI_BUF_INIT;
    size_t cap;

    ASSERT_OK(casi_buf_puts(&buf, "something reasonably long"));
    cap = buf.cap;

    casi_buf_clear(&buf);
    ASSERT_EQ_INT(buf.len, 0);
    ASSERT_EQ_INT(buf.cap, cap);
    ASSERT_EQ_STR(casi_buf_cstr(&buf), "");

    casi_buf_dispose(&buf);
}

static void test_set_replaces(void)
{
    casi_buf buf = CASI_BUF_INIT;

    ASSERT_OK(casi_buf_puts(&buf, "first"));
    ASSERT_OK(casi_buf_set(&buf, "second", 6));
    ASSERT_EQ_STR(casi_buf_cstr(&buf), "second");

    casi_buf_dispose(&buf);
}

static void test_detach_transfers_ownership(void)
{
    casi_buf buf = CASI_BUF_INIT;
    char *owned;

    ASSERT_OK(casi_buf_puts(&buf, "mine now"));
    owned = casi_buf_detach(&buf);

    ASSERT_EQ_STR(owned, "mine now");
    ASSERT_EQ_INT(buf.len, 0);
    ASSERT_EQ_INT(buf.cap, 0);
    ASSERT_TRUE(buf.ptr == NULL);

    free(owned);
    casi_buf_dispose(&buf);
}

int main(void)
{
    RUN_TEST(test_empty_buf_is_usable);
    RUN_TEST(test_put_and_terminate);
    RUN_TEST(test_binary_safe);
    RUN_TEST(test_printf_grows);
    RUN_TEST(test_clear_keeps_allocation);
    RUN_TEST(test_set_replaces);
    RUN_TEST(test_detach_transfers_ownership);
    return casi_test_report("buf");
}
