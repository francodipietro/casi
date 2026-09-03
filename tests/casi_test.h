/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_TEST_H
#define CASI_TEST_H

/*
 * A ~100-line test harness, header-only and dependency-free.
 *
 * Deliberately not a vendored third-party framework: the whole surface casi
 * needs is "run these functions, compare these values, exit non-zero if any
 * failed", and CTest already supplies discovery, parallelism and reporting.
 * One less thing to keep in the build.
 *
 * A failing assertion reports and returns from the current test; the rest of
 * the file still runs, so one broken case does not mask the others.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         casi_test_failures;
static int         casi_test_count;
static const char *casi_test_current = "<none>";
static int         casi_test_failed_here;

static void casi_test_fail_header(const char *file, int line)
{
    fprintf(stderr, "  FAIL %s (%s:%d)\n", casi_test_current, file, line);
    casi_test_failures++;
    casi_test_failed_here = 1;
}

#define CASI_TEST_FAIL_(...)                             \
    do {                                                 \
        casi_test_fail_header(__FILE__, __LINE__);       \
        fprintf(stderr, "    " __VA_ARGS__);             \
        fputc('\n', stderr);                             \
        return;                                          \
    } while (0)

#define ASSERT_TRUE(expr)                                                     \
    do {                                                                      \
        if (!(expr))                                                          \
            CASI_TEST_FAIL_("expected true: %s", #expr);                      \
    } while (0)

#define ASSERT_FALSE(expr)                                                    \
    do {                                                                      \
        if ((expr))                                                           \
            CASI_TEST_FAIL_("expected false: %s", #expr);                     \
    } while (0)

#define ASSERT_EQ_INT(actual, expected)                                       \
    do {                                                                      \
        long long a_ = (long long)(actual), e_ = (long long)(expected);       \
        if (a_ != e_)                                                         \
            CASI_TEST_FAIL_("%s: got %lld, want %lld", #actual, a_, e_);      \
    } while (0)

#define ASSERT_EQ_STR(actual, expected)                                       \
    do {                                                                      \
        const char *a_ = (actual), *e_ = (expected);                          \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0)                  \
            CASI_TEST_FAIL_("%s:\n      got  \"%s\"\n      want \"%s\"",      \
                            #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)"); \
    } while (0)

#define ASSERT_EQ_MEM(actual, expected, len)                                  \
    do {                                                                      \
        if (memcmp((actual), (expected), (len)) != 0)                         \
            CASI_TEST_FAIL_("%s: %zu bytes differ", #actual, (size_t)(len));  \
    } while (0)

/* Shorthands for the casi_result convention. */
#define ASSERT_OK(expr)                                                       \
    do {                                                                      \
        int rc_ = (expr);                                                     \
        if (rc_ != CASI_OK)                                                   \
            CASI_TEST_FAIL_("%s: rc %d (%s)", #expr, rc_, casi_error_last()); \
    } while (0)

#define ASSERT_RC(expr, want)                                                 \
    do {                                                                      \
        int rc_ = (expr);                                                     \
        if (rc_ != (want))                                                    \
            CASI_TEST_FAIL_("%s: rc %d, want %d", #expr, rc_, (int)(want));   \
    } while (0)

static void casi_test_run(const char *name, void (*fn)(void))
{
    casi_test_current = name;
    casi_test_failed_here = 0;
    casi_test_count++;
    fn();
    if (!casi_test_failed_here)
        printf("  ok   %s\n", name);
}

#define RUN_TEST(fn) casi_test_run(#fn, fn)

static int casi_test_report(const char *suite)
{
    if (casi_test_failures > 0) {
        fprintf(stderr, "%s: %d of %d failed\n",
                suite, casi_test_failures, casi_test_count);
        return 1;
    }
    printf("%s: %d passed\n", suite, casi_test_count);
    return 0;
}

#endif /* CASI_TEST_H */
