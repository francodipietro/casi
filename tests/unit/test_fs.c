/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/error.h"
#include "casi/fs.h"

#include "casi_test.h"

#include <stdlib.h>
#include <unistd.h>

static char g_tmp[] = "/tmp/casi-test-fs-XXXXXX";

static int tmp_path(casi_buf *out, const char *rest)
{
    casi_buf_clear(out);
    if (casi_buf_puts(out, g_tmp) != CASI_OK)
        return CASI_ENOMEM;
    return casi_fs_join(out, "", rest);
}

static void test_join(void)
{
    casi_buf b = CASI_BUF_INIT;

    ASSERT_OK(casi_fs_join(&b, "/a/b", "c/d"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/a/b/c/d");

    casi_buf_clear(&b);
    ASSERT_OK(casi_fs_join(&b, "/a/b/", "/c"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/a/b/c");

    casi_buf_clear(&b);
    ASSERT_OK(casi_fs_join(&b, "/a///", "///c"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/a/c");

    /* Empty tail leaves the base alone rather than adding a dangling slash. */
    casi_buf_clear(&b);
    ASSERT_OK(casi_fs_join(&b, "/a/b", ""));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/a/b");

    /* Empty base appends to whatever the buffer already holds. */
    casi_buf_clear(&b);
    ASSERT_OK(casi_buf_puts(&b, "/home/franco"));
    ASSERT_OK(casi_fs_join(&b, "", ".claude"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/home/franco/.claude");

    casi_buf_dispose(&b);
}

static void test_dirname(void)
{
    casi_buf b = CASI_BUF_INIT;

    ASSERT_OK(casi_fs_dirname(&b, "/a/b/c.jsonl"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/a/b");

    ASSERT_OK(casi_fs_dirname(&b, "/top"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), "/");

    ASSERT_OK(casi_fs_dirname(&b, "bare"));
    ASSERT_EQ_STR(casi_buf_cstr(&b), ".");

    casi_buf_dispose(&b);
}

static void test_realpath_replaces_output(void)
{
    casi_buf got = CASI_BUF_INIT;

    ASSERT_OK(casi_buf_puts(&got, "stale"));
    ASSERT_OK(casi_fs_realpath(g_tmp, &got));
    ASSERT_TRUE(strcmp(casi_buf_cstr(&got), "stale") != 0);
    ASSERT_TRUE(casi_buf_cstr(&got)[0] == '/');

    casi_buf_dispose(&got);
}

static void test_hostname_and_known_host_input(void)
{
    casi_buf hostname = CASI_BUF_INIT;
    char long_host[600];
    char *first;

    ASSERT_OK(casi_fs_hostname(&hostname));
    ASSERT_TRUE(hostname.len > 0);
    first = casi_strdup(casi_buf_cstr(&hostname));
    ASSERT_TRUE(first != NULL);

    /* Reusing the buffer replaces the earlier hostname instead of appending
     * a second copy. */
    ASSERT_OK(casi_fs_hostname(&hostname));
    ASSERT_EQ_STR(casi_buf_cstr(&hostname), first);

    /* Unsafe and option-looking host text must be rejected before it can
     * reach ssh-keygen's shell command. */
    ASSERT_FALSE(casi_fs_ssh_hostkey_is_known("host;echo unsafe", "SHA256:x"));
    ASSERT_FALSE(casi_fs_ssh_hostkey_is_known("-Ftrusted.example", "SHA256:x"));
    ASSERT_FALSE(casi_fs_ssh_hostkey_is_known(NULL, "SHA256:x"));
    ASSERT_FALSE(casi_fs_ssh_hostkey_is_known("trusted.example", NULL));
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = '\0';
    ASSERT_FALSE(casi_fs_ssh_hostkey_is_known(long_host, "SHA256:x"));

    free(first);
    casi_buf_dispose(&hostname);
}

static void test_mkdir_p_is_idempotent(void)
{
    casi_buf p = CASI_BUF_INIT;

    ASSERT_OK(tmp_path(&p, "deep/nested//tree/"));
    ASSERT_OK(casi_fs_mkdir_p(casi_buf_cstr(&p)));
    ASSERT_TRUE(casi_fs_is_dir(casi_buf_cstr(&p)));

    /* Running it again on an existing tree must succeed, not EEXIST. */
    ASSERT_OK(casi_fs_mkdir_p(casi_buf_cstr(&p)));

    casi_buf_dispose(&p);
}

static void test_write_read_roundtrip(void)
{
    casi_buf p = CASI_BUF_INIT, got = CASI_BUF_INIT;
    /* Embedded NUL and CR: a session transcript must survive both. */
    const char payload[] = { '{', '"', 'a', '"', '}', '\n', '\0', '\r', '\n', 'z' };

    ASSERT_OK(tmp_path(&p, "sub/dir/session.jsonl"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&p), payload, sizeof(payload)));

    ASSERT_OK(casi_fs_read_file(casi_buf_cstr(&p), &got));
    ASSERT_EQ_INT(got.len, sizeof(payload));
    ASSERT_EQ_MEM(got.ptr, payload, sizeof(payload));

    casi_buf_dispose(&p);
    casi_buf_dispose(&got);
}

static void test_write_replaces_and_leaves_no_temp(void)
{
    casi_buf p = CASI_BUF_INIT, got = CASI_BUF_INIT;
    casi_strvec entries = CASI_STRVEC_INIT;
    casi_buf dir = CASI_BUF_INIT;
    size_t i;

    ASSERT_OK(tmp_path(&p, "replace/file.txt"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&p), "old", 3));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&p), "new content", 11));

    ASSERT_OK(casi_fs_read_file(casi_buf_cstr(&p), &got));
    ASSERT_EQ_STR(casi_buf_cstr(&got), "new content");

    ASSERT_OK(tmp_path(&dir, "replace"));
    ASSERT_OK(casi_fs_listdir(casi_buf_cstr(&dir), &entries));
    /* The atomic write must leave the directory holding exactly the target:
     * no ".casi-tmp-<pid>" sibling survives a successful replace. */
    ASSERT_EQ_INT(entries.len, 1);
    for (i = 0; i < entries.len; i++)
        ASSERT_TRUE(strstr(entries.items[i], ".casi-tmp-") == NULL);

    casi_strvec_dispose(&entries);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&p);
    casi_buf_dispose(&got);
}

static void test_empty_file(void)
{
    casi_buf p = CASI_BUF_INIT, got = CASI_BUF_INIT;

    ASSERT_OK(tmp_path(&p, "empty.jsonl"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&p), "", 0));
    ASSERT_OK(casi_fs_read_file(casi_buf_cstr(&p), &got));
    ASSERT_EQ_INT(got.len, 0);

    casi_buf_dispose(&p);
    casi_buf_dispose(&got);
}

static void test_missing_paths_report_notfound(void)
{
    casi_buf p = CASI_BUF_INIT, got = CASI_BUF_INIT, loop = CASI_BUF_INIT;
    casi_strvec entries = CASI_STRVEC_INIT;
    casi_stat st;

    ASSERT_OK(tmp_path(&p, "does/not/exist"));

    ASSERT_RC(casi_fs_stat(casi_buf_cstr(&p), &st), CASI_ENOTFOUND);
    ASSERT_RC(casi_fs_realpath(casi_buf_cstr(&p), &got), CASI_ENOTFOUND);
    ASSERT_RC(casi_fs_read_file(casi_buf_cstr(&p), &got), CASI_ENOTFOUND);
    ASSERT_RC(casi_fs_listdir(casi_buf_cstr(&p), &entries), CASI_ENOTFOUND);
    ASSERT_FALSE(casi_fs_exists(casi_buf_cstr(&p)));
    ASSERT_FALSE(casi_fs_is_dir(casi_buf_cstr(&p)));

    /* realpath failures other than a missing component are I/O errors. */
    ASSERT_OK(tmp_path(&loop, "loop"));
    ASSERT_OK(casi_fs_symlink("loop", casi_buf_cstr(&loop)));
    ASSERT_RC(casi_fs_realpath(casi_buf_cstr(&loop), &got), CASI_EIO);

    /* Removing something already absent is success, not an error. */
    ASSERT_OK(casi_fs_remove_file(casi_buf_cstr(&p)));

    casi_strvec_dispose(&entries);
    casi_buf_dispose(&loop);
    casi_buf_dispose(&p);
    casi_buf_dispose(&got);
}

static void test_mkdir_p_rejects_a_file_in_the_way(void)
{
    casi_buf blocker = CASI_BUF_INIT, under = CASI_BUF_INIT;

    /* A regular file where a directory component should be must fail loudly at
     * that component, not be swallowed as EEXIST and blow up later. */
    ASSERT_OK(tmp_path(&blocker, "blocked"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&blocker), "x", 1));

    ASSERT_RC(casi_fs_mkdir_p(casi_buf_cstr(&blocker)), CASI_EEXISTS);

    ASSERT_OK(tmp_path(&under, "blocked/deeper/still"));
    ASSERT_RC(casi_fs_mkdir_p(casi_buf_cstr(&under)), CASI_EEXISTS);

    casi_buf_dispose(&blocker);
    casi_buf_dispose(&under);
}

static void test_listdir_is_sorted_and_skips_dots(void)
{
    casi_buf dir = CASI_BUF_INIT, f = CASI_BUF_INIT;
    casi_strvec entries = CASI_STRVEC_INIT;

    ASSERT_OK(tmp_path(&dir, "listing"));
    ASSERT_OK(casi_fs_mkdir_p(casi_buf_cstr(&dir)));

    ASSERT_OK(tmp_path(&f, "listing/zebra"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&f), "", 0));
    ASSERT_OK(tmp_path(&f, "listing/alpha"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&f), "", 0));
    ASSERT_OK(tmp_path(&f, "listing/.hidden"));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&f), "", 0));

    ASSERT_OK(casi_fs_listdir(casi_buf_cstr(&dir), &entries));

    ASSERT_EQ_INT(entries.len, 3);
    ASSERT_EQ_STR(entries.items[0], ".hidden");
    ASSERT_EQ_STR(entries.items[1], "alpha");
    ASSERT_EQ_STR(entries.items[2], "zebra");

    casi_strvec_dispose(&entries);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&f);
}

int main(void)
{
    int status;

    if (mkdtemp(g_tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    RUN_TEST(test_join);
    RUN_TEST(test_dirname);
    RUN_TEST(test_realpath_replaces_output);
    RUN_TEST(test_hostname_and_known_host_input);
    RUN_TEST(test_mkdir_p_is_idempotent);
    RUN_TEST(test_write_read_roundtrip);
    RUN_TEST(test_write_replaces_and_leaves_no_temp);
    RUN_TEST(test_empty_file);
    RUN_TEST(test_missing_paths_report_notfound);
    RUN_TEST(test_mkdir_p_rejects_a_file_in_the_way);
    RUN_TEST(test_listdir_is_sorted_and_skips_dots);

    status = casi_test_report("fs");

    /* Leave the tree behind on failure so it can be inspected. */
    if (status == 0) {
        casi_buf cmd = CASI_BUF_INIT;
        if (casi_buf_printf(&cmd, "rm -rf '%s'", g_tmp) == CASI_OK &&
            system(casi_buf_cstr(&cmd)) != 0)
            fprintf(stderr, "warning: could not clean up %s\n", g_tmp);
        casi_buf_dispose(&cmd);
    } else {
        fprintf(stderr, "scratch tree left at %s\n", g_tmp);
    }

    return status;
}
