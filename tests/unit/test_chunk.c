/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/buf.h"
#include "casi/chunk.h"
#include "casi/error.h"

#include "casi_test.h"

/* Collects each chunk as "len:bytes;" so a whole split is one comparable string. */
struct collector {
    casi_buf out;
    size_t   count;
    size_t   total;
    int      out_of_order;
};

static int collect(const void *data, size_t len, size_t index, void *payload)
{
    struct collector *c = payload;
    int rc;

    /* The assertion macros return from a void test, so they cannot be used in
     * a callback with a return value -- record it and check after the walk. */
    if (index != c->count)
        c->out_of_order = 1;

    if ((rc = casi_buf_printf(&c->out, "[%zu]", len)) != CASI_OK)
        return rc;
    if ((rc = casi_buf_put(&c->out, data, len)) != CASI_OK)
        return rc;

    c->count++;
    c->total += len;
    return CASI_OK;
}

static void split_into(struct collector *c, const char *data, size_t target)
{
    memset(c, 0, sizeof(*c));
    ASSERT_OK(casi_chunk_split(data, strlen(data), target, collect, c));
    ASSERT_FALSE(c->out_of_order);
}

static void test_empty_input_yields_no_chunks(void)
{
    struct collector c;

    split_into(&c, "", 4);
    ASSERT_EQ_INT(c.count, 0);
    ASSERT_EQ_INT(casi_chunk_count("", 0, 4), 0);
    casi_buf_dispose(&c.out);
}

static void test_input_below_target_is_one_chunk(void)
{
    struct collector c;

    split_into(&c, "a\nb\n", 1024);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_STR(casi_buf_cstr(&c.out), "[4]a\nb\n");
    casi_buf_dispose(&c.out);
}

static void test_cuts_land_just_after_a_newline(void)
{
    struct collector c;

    /* target 4: first chunk reaches 4 bytes mid-line and runs on to the
     * newline that closes it. */
    split_into(&c, "aaa\nbbb\nccc\n", 4);
    ASSERT_EQ_INT(c.count, 3);
    ASSERT_EQ_STR(casi_buf_cstr(&c.out), "[4]aaa\n[4]bbb\n[4]ccc\n");
    casi_buf_dispose(&c.out);
}

static void test_a_line_longer_than_the_target_is_its_own_chunk(void)
{
    struct collector c;

    split_into(&c, "aaaaaaaaaaaaaaaaaaaa\nb\n", 4);
    ASSERT_EQ_INT(c.count, 2);
    ASSERT_EQ_STR(casi_buf_cstr(&c.out), "[21]aaaaaaaaaaaaaaaaaaaa\n[2]b\n");
    casi_buf_dispose(&c.out);
}

static void test_missing_final_newline_still_terminates(void)
{
    struct collector c;

    split_into(&c, "aaa\nbbb", 4);
    ASSERT_EQ_INT(c.count, 2);
    ASSERT_EQ_STR(casi_buf_cstr(&c.out), "[4]aaa\n[3]bbb");
    casi_buf_dispose(&c.out);
}

static void test_no_newline_at_all_is_a_single_chunk(void)
{
    struct collector c;

    split_into(&c, "aaaaaaaaaa", 2);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_EQ_STR(casi_buf_cstr(&c.out), "[10]aaaaaaaaaa");
    casi_buf_dispose(&c.out);
}

/*
 * The property the entire storage design rests on: appending to a transcript
 * must leave every earlier chunk byte-identical, so only the tail is a new
 * git object.
 */
static void test_appending_leaves_earlier_chunks_untouched(void)
{
    casi_buf before = CASI_BUF_INIT, after = CASI_BUF_INIT;
    struct collector c1, c2;
    const char *original = "line one\nline two\nline three\n";
    const char *grown    = "line one\nline two\nline three\nline four\nline five\n";

    split_into(&c1, original, 10);
    split_into(&c2, grown, 10);

    ASSERT_TRUE(c2.count > c1.count);

    /* The rendering of the first c1.count chunks must be a byte-exact prefix
     * of the rendering of the grown file. */
    ASSERT_OK(casi_buf_put(&before, c1.out.ptr, c1.out.len));
    ASSERT_OK(casi_buf_put(&after, c2.out.ptr, c1.out.len));
    ASSERT_EQ_STR(casi_buf_cstr(&before), casi_buf_cstr(&after));

    casi_buf_dispose(&before);
    casi_buf_dispose(&after);
    casi_buf_dispose(&c1.out);
    casi_buf_dispose(&c2.out);
}

static void test_appending_a_partial_line_only_rewrites_the_tail(void)
{
    struct collector c1, c2;

    /* A transcript flushed mid-line: the last chunk changes, earlier ones
     * must not. */
    split_into(&c1, "aaa\nbbb\nccc", 4);
    split_into(&c2, "aaa\nbbb\nccc\nddd\n", 4);

    ASSERT_EQ_INT(c1.count, 3);
    ASSERT_EQ_INT(c2.count, 4);
    /* "[4]aaa\n[4]bbb\n" is the shared prefix; chunk 3 differs. */
    ASSERT_EQ_MEM(c1.out.ptr, c2.out.ptr, strlen("[4]aaa\n[4]bbb\n"));

    casi_buf_dispose(&c1.out);
    casi_buf_dispose(&c2.out);
}

static void test_chunks_reassemble_to_the_original(void)
{
    struct collector c;
    const char *original = "one\ntwo\nthree\nfour\nfive\nsix\n";
    casi_buf rebuilt = CASI_BUF_INIT;
    const char *p;

    split_into(&c, original, 5);

    /* Strip the "[len]" markers back out and confirm the bytes concatenate to
     * exactly what went in. */
    p = casi_buf_cstr(&c.out);
    while (*p != '\0') {
        size_t len;
        char *end;

        ASSERT_TRUE(*p == '[');
        len = (size_t)strtoul(p + 1, &end, 10);
        ASSERT_TRUE(*end == ']');
        p = end + 1;
        ASSERT_OK(casi_buf_put(&rebuilt, p, len));
        p += len;
    }

    ASSERT_EQ_STR(casi_buf_cstr(&rebuilt), original);
    ASSERT_EQ_INT(c.total, strlen(original));

    casi_buf_dispose(&rebuilt);
    casi_buf_dispose(&c.out);
}

static void test_count_agrees_with_split(void)
{
    struct collector c;
    const char *data = "alpha\nbeta\ngamma\ndelta\nepsilon\n";

    split_into(&c, data, 7);
    ASSERT_EQ_INT(casi_chunk_count(data, strlen(data), 7), c.count);
    casi_buf_dispose(&c.out);
}

static int fail_on_second(const void *data, size_t len, size_t index, void *payload)
{
    (void)data;
    (void)len;
    (void)payload;
    return index == 1 ? CASI_EIO : CASI_OK;
}

static void test_callback_error_stops_the_walk(void)
{
    ASSERT_RC(casi_chunk_split("aaa\nbbb\nccc\n", 12, 4, fail_on_second, NULL), CASI_EIO);
}

static void test_binary_content_survives(void)
{
    /* Transcripts carry embedded NULs and CRs; chunking must not care. */
    const char raw[] = { 'a', '\0', '\n', '\r', '\n', 'b', '\n' };
    struct collector c;

    memset(&c, 0, sizeof(c));
    ASSERT_OK(casi_chunk_split(raw, sizeof(raw), 2, collect, &c));
    ASSERT_FALSE(c.out_of_order);
    ASSERT_EQ_INT(c.total, sizeof(raw));
    casi_buf_dispose(&c.out);
}

int main(void)
{
    RUN_TEST(test_empty_input_yields_no_chunks);
    RUN_TEST(test_input_below_target_is_one_chunk);
    RUN_TEST(test_cuts_land_just_after_a_newline);
    RUN_TEST(test_a_line_longer_than_the_target_is_its_own_chunk);
    RUN_TEST(test_missing_final_newline_still_terminates);
    RUN_TEST(test_no_newline_at_all_is_a_single_chunk);
    RUN_TEST(test_appending_leaves_earlier_chunks_untouched);
    RUN_TEST(test_appending_a_partial_line_only_rewrites_the_tail);
    RUN_TEST(test_chunks_reassemble_to_the_original);
    RUN_TEST(test_count_agrees_with_split);
    RUN_TEST(test_callback_error_stops_the_walk);
    RUN_TEST(test_binary_content_survives);
    return casi_test_report("chunk");
}
