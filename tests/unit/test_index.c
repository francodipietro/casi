/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/index.h"
#include "casi/paths.h"

#include "casi_test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[] = "/tmp/casi-test-index-XXXXXX";

static int make_roots(casi_roots **out, const char *src)
{
    int rc;

    if ((rc = casi_roots_new(out)) != CASI_OK)
        return rc;
    return casi_roots_add(*out, "src", src);
}

static void test_roundtrip_and_stat_invalidation(void)
{
    casi_roots *roots = NULL;
    casi_index *index = NULL;
    const casi_index_entry *entry;
    casi_stat before, grown;
    git_oid chunks[2];
    casi_buf path = CASI_BUF_INIT, corrupt = CASI_BUF_INIT;

    ASSERT_OK(make_roots(&roots, "/tmp/src"));
    ASSERT_OK(casi_buf_printf(&path, "%s/session.jsonl", g_home));
    ASSERT_OK(casi_fs_write_file_atomic(casi_buf_cstr(&path), "one\n", 4));
    ASSERT_OK(casi_fs_stat(casi_buf_cstr(&path), &before));
    ASSERT_EQ_INT(git_oid_fromstr(&chunks[0], "0123456789012345678901234567890123456789"), 0);
    ASSERT_EQ_INT(git_oid_fromstr(&chunks[1], "abcdefabcdefabcdefabcdefabcdefabcdefabcd"), 0);

    ASSERT_OK(casi_index_open(roots, &index));
    ASSERT_TRUE(casi_index_find(index, "claude-code", casi_buf_cstr(&path)) == NULL);
    ASSERT_OK(casi_index_update(index, "claude-code", casi_buf_cstr(&path), &before,
                                0, 0, 4, chunks, 2));
    ASSERT_OK(casi_index_flush(index));
    casi_index_free(index);
    index = NULL;

    ASSERT_OK(casi_index_open(roots, &index));
    entry = casi_index_find(index, "claude-code", casi_buf_cstr(&path));
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(casi_index_entry_matches(entry, &before));
    ASSERT_EQ_INT(entry->chunk_count, 2);
    ASSERT_EQ_MEM(entry->chunks, chunks, sizeof(chunks));
    ASSERT_TRUE(casi_index_entry_is_well_formed(entry));

    {
        casi_index_entry malformed = *entry;

        /* A corrupt count of zero must never turn a non-empty transcript into
         * a trusted empty session on the next push. */
        malformed.chunk_count = 0;
        ASSERT_FALSE(casi_index_entry_is_well_formed(&malformed));
    }

    grown = before;
    grown.size++;
    ASSERT_FALSE(casi_index_entry_matches(entry, &grown));
    ASSERT_TRUE(casi_index_entry_can_resume(entry, &grown));
    grown.ino++;
    ASSERT_FALSE(casi_index_entry_can_resume(entry, &grown));

    casi_index_free(index);
    index = NULL;

    /* An altered offset or even a checksum byte makes the whole local cache a
     * miss.  It is cheaper and safer to scan than to reuse partial state. */
    ASSERT_OK(casi_fs_read_file(casi_paths_index(), &corrupt));
    corrupt.ptr[corrupt.len - 1] ^= 1;
    ASSERT_OK(casi_fs_write_file_atomic(casi_paths_index(), corrupt.ptr, corrupt.len));
    ASSERT_OK(casi_index_open(roots, &index));
    ASSERT_TRUE(casi_index_find(index, "claude-code", casi_buf_cstr(&path)) == NULL);
    casi_index_free(index);
    casi_roots_free(roots);
    casi_buf_dispose(&corrupt);
    casi_buf_dispose(&path);
}

static void test_root_snapshot_invalidates_every_entry(void)
{
    casi_roots *roots = NULL;
    casi_index *index = NULL;
    casi_buf path = CASI_BUF_INIT;

    ASSERT_OK(make_roots(&roots, "/tmp/other-src"));
    ASSERT_OK(casi_buf_printf(&path, "%s/session.jsonl", g_home));
    ASSERT_OK(casi_index_open(roots, &index));
    ASSERT_TRUE(casi_index_find(index, "claude-code", casi_buf_cstr(&path)) == NULL);

    casi_index_free(index);
    casi_roots_free(roots);
    casi_buf_dispose(&path);
}

int main(void)
{
    int status;

    if (mkdtemp(g_home) == NULL)
        return 1;
    if (setenv("CASI_HOME", g_home, 1) != 0)
        return 1;
    casi_paths_reset();
    if (casi_init() != CASI_OK)
        return 1;

    RUN_TEST(test_roundtrip_and_stat_invalidation);
    RUN_TEST(test_root_snapshot_invalidates_every_entry);

    status = casi_test_report("index");
    casi_shutdown();
    unsetenv("CASI_HOME");
    casi_paths_reset();
    return status;
}
