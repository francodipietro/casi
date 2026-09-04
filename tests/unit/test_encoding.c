/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/encoding.h"
#include "casi/error.h"

#include "casi_test.h"

static void check(const char *path, const char *want)
{
    casi_buf got = CASI_BUF_INIT;
    int rc;

    rc = casi_encode_project_dir(path, &got);
    if (rc != CASI_OK) {
        casi_buf_dispose(&got);
        ASSERT_OK(rc);
    }
    ASSERT_EQ_STR(casi_buf_cstr(&got), want);
    casi_buf_dispose(&got);
}

/*
 * Golden cases taken verbatim from ~/.claude/projects on the development
 * machine (Claude Code 2.1.258). These are observations, not invented
 * examples: if Claude Code ever changes its rule, this is what catches it.
 */
static void test_real_project_directories(void)
{
    check("/Users/fdipietro/src",
          "-Users-fdipietro-src");
    check("/Users/fdipietro/src/bookit",
          "-Users-fdipietro-src-bookit");
    check("/Users/fdipietro/src/data-analysis",
          "-Users-fdipietro-src-data-analysis");
    check("/Users/fdipietro/src/bookit/poligonos-destinos",
          "-Users-fdipietro-src-bookit-poligonos-destinos");
}

static void test_underscore_becomes_dash(void)
{
    /* /Users/fdipietro/src/etl_fsearch on disk. */
    check("/Users/fdipietro/src/etl_fsearch", "-Users-fdipietro-src-etl-fsearch");
}

static void test_dot_becomes_dash(void)
{
    /*
     * The case no existing directory could settle, so it was probed live:
     * a directory literally named "casi.probe.v2" came back encoded with
     * dashes, ruling out the narrower "only / and _" rule.
     */
    check("/tmp/casi.probe.v2", "-tmp-casi-probe-v2");
    check("/Users/f/.config/app", "-Users-f--config-app");
}

static void test_existing_dashes_survive_and_can_double(void)
{
    /* "claude-501/-Users" -> "claude-501--Users": the '/' adds a dash next to
     * the one already there. Straight from the live probe. */
    check("/private/tmp/claude-501/-Users-f", "-private-tmp-claude-501--Users-f");
}

static void test_the_mapping_is_not_injective(void)
{
    /* Three different paths, one directory name. This is precisely why casi
     * stores the real path in metadata instead of decoding the name. */
    casi_buf a = CASI_BUF_INIT, b = CASI_BUF_INIT, c = CASI_BUF_INIT;

    ASSERT_OK(casi_encode_project_dir("/x/a/b", &a));
    ASSERT_OK(casi_encode_project_dir("/x/a-b", &b));
    ASSERT_OK(casi_encode_project_dir("/x/a_b", &c));

    ASSERT_EQ_STR(casi_buf_cstr(&a), "-x-a-b");
    ASSERT_EQ_STR(casi_buf_cstr(&b), "-x-a-b");
    ASSERT_EQ_STR(casi_buf_cstr(&c), "-x-a-b");

    casi_buf_dispose(&a);
    casi_buf_dispose(&b);
    casi_buf_dispose(&c);
}

static void test_non_ascii_is_one_dash_per_code_point(void)
{
    /* JavaScript's regex runs over UTF-16 code units, so "ñ" is a single
     * replacement -- not the two a byte-wise pass over UTF-8 would emit. */
    check("/Users/f/dise\xc3\xb1o", "-Users-f-dise-o");
    /* 3-byte sequence (CJK) is still one dash. */
    check("/srv/\xe6\x97\xa5", "-srv--");
}

static void test_malformed_utf8_does_not_derail(void)
{
    /* A stray continuation byte becomes its own dash; the rest still encodes. */
    check("/a/\xff/b", "-a---b");
}

static void test_edges(void)
{
    check("", "");
    check("/", "-");
    check("plain", "plain");
    check("///", "---");
}

static void test_output_buffer_is_reset_not_appended(void)
{
    casi_buf out = CASI_BUF_INIT;

    ASSERT_OK(casi_encode_project_dir("/a", &out));
    ASSERT_OK(casi_encode_project_dir("/b", &out));
    ASSERT_EQ_STR(casi_buf_cstr(&out), "-b");

    casi_buf_dispose(&out);
}

int main(void)
{
    RUN_TEST(test_real_project_directories);
    RUN_TEST(test_underscore_becomes_dash);
    RUN_TEST(test_dot_becomes_dash);
    RUN_TEST(test_existing_dashes_survive_and_can_double);
    RUN_TEST(test_the_mapping_is_not_injective);
    RUN_TEST(test_non_ascii_is_one_dash_per_code_point);
    RUN_TEST(test_malformed_utf8_does_not_derail);
    RUN_TEST(test_edges);
    RUN_TEST(test_output_buffer_is_reset_not_appended);
    return casi_test_report("encoding");
}
