/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/paths.h"
#include "casi/sync.h"

#include "casi_test.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static char g_home[] = "/tmp/casi-test-sync-XXXXXX";
static casi_repo *g_repo;
static casi_roots *g_roots;
static char g_session_id[] = "aaaaaaaa-0000-0000-0000-000000000000";
static char g_project[] = "casi://bookit";

static void entry_from_text(casi_entry *entry, const char *project,
                            const char *text)
{
    entry->session_id = g_session_id;
    entry->project_path = (char *)project;
    entry->bytes = strlen(text);
    entry->chunk_count = 1;
    entry->chunks = calloc(1, sizeof(*entry->chunks));
    ASSERT_TRUE(entry->chunks != NULL);
    ASSERT_OK(casi_repo_write_blob(g_repo, text, strlen(text), entry->chunks));
}

static void test_legacy_paths_are_equivalent(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\",\"message\":\"base\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"base\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_local_append_is_safe(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\",\"message\":\"base\"}\n"
                    "{\"message\":\"new local work\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"base\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_LOCAL_AHEAD);
    free(local.chunks);
    free(remote.chunks);
}

static void test_real_difference_stays_diverged(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\",\"message\":\"local edit\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"remote edit\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_DIVERGED);
    free(local.chunks);
    free(remote.chunks);
}

static void test_legacy_remote_append_stays_pending(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\",\"message\":\"base\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"base\"}\n"
                    "{\"message\":\"new remote work\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_DIVERGED);
    free(local.chunks);
    free(remote.chunks);
}

static void test_unknown_other_project_stays_pending(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\",\"message\":\"base\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://src/work/bookit\","
                    "\"message\":\"casi://src/work/other\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_DIVERGED);
    free(local.chunks);
    free(remote.chunks);
}

static void test_auxiliary_paths_but_not_edits_are_equivalent(void)
{
    casi_asset local = { 0 }, remote = { 0 };
    bool equal;
    const char *local_text = "{\"cwd\":\"casi://bookit\",\"message\":\"base\"}\n";
    const char *old_text = "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"base\"}\n";
    const char *edit_text = "{\"cwd\":\"casi://src/work/bookit\",\"message\":\"edit\"}\n";

    local.project_path = remote.project_path = g_project;
    local.kind = remote.kind = CASI_AUX_SUBAGENT;
    ASSERT_OK(casi_repo_write_blob(g_repo, local_text, strlen(local_text), &local.oid));
    ASSERT_OK(casi_repo_write_blob(g_repo, old_text, strlen(old_text), &remote.oid));
    ASSERT_OK(casi_store_asset_equal(g_repo, g_roots, &local, &remote, &equal));
    ASSERT_TRUE(equal);

    ASSERT_OK(casi_repo_write_blob(g_repo, edit_text, strlen(edit_text), &remote.oid));
    ASSERT_OK(casi_store_asset_equal(g_repo, g_roots, &local, &remote, &equal));
    ASSERT_FALSE(equal);
}

static void test_historic_foreign_home_does_not_create_a_conflict(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"attachment\":\"/home/alice/.claude/CLAUDE.md\","
                    "\"path\":\"casi://bookit/README.md\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"attachment\":\"casi://~/.claude/CLAUDE.md\","
                    "\"path\":\"casi://src/work/bookit/README.md\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_moved_parent_does_not_create_a_conflict(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"other\":\"/Users/bob/src/workload\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"other\":\"casi://src/work/workload\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_unmapped_historic_parent_is_only_a_comparison_hint(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;
    casi_buf content = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;

    entry_from_text(&local, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"other\":\"/Users/bob/src/workload\"}\n");
    entry_from_text(&remote, "casi://bookit",
                    "{\"cwd\":\"casi://bookit\","
                    "\"other\":\"casi://work/workload\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    /* A remote-only file still cannot be written using this tentative root. */
    ASSERT_RC(casi_store_materialize_content(g_repo, g_roots, &remote,
                                              &content, &unmapped), CASI_EUNMAPPED);
    casi_buf_dispose(&content);
    casi_buf_dispose(&unmapped);
    free(local.chunks);
    free(remote.chunks);
}

static void test_unmapped_historic_child_is_only_a_comparison_hint(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://src",
                    "{\"cwd\":\"casi://src\","
                    "\"other\":\"/Users/bob/src/work/file\"}\n");
    entry_from_text(&remote, "casi://src",
                    "{\"cwd\":\"casi://src\","
                    "\"other\":\"casi://work/file\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_removed_checkout_child_is_only_a_comparison_hint(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://src",
                    "{\"cwd\":\"/Users/bob/src\"}\n");
    entry_from_text(&remote, "casi://src",
                    "{\"cwd\":\"/Users/bob/src/work\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_legacy_root_name_can_move_without_a_conflict(void)
{
    casi_entry local = { 0 }, remote = { 0 };
    casi_sync_relation relation;

    entry_from_text(&local, "casi://src",
                    "{\"cwd\":\"/Users/bob/src\",\"message\":\"base\"}\n");
    entry_from_text(&remote, "casi://work",
                    "{\"cwd\":\"casi://work\",\"message\":\"base\"}\n");
    ASSERT_OK(casi_sync_compare(g_repo, g_roots, &local, &remote, &relation));
    ASSERT_EQ_INT(relation, CASI_SYNC_SAME);
    free(local.chunks);
    free(remote.chunks);
}

static void test_legacy_work_references_materialize_without_an_alias(void)
{
    casi_entry entry = { 0 };
    casi_buf unmapped = CASI_BUF_INIT;
    char path[sizeof(g_home) + 32];
    char content[256];
    FILE *file;
    size_t n;

    entry_from_text(&entry, "casi://otel-lab",
                    "{\"cwd\":\"casi://work/otel-lab\","
                    "\"instruction\":\"casi://work/CLAUDE.md\"}\n");
    snprintf(path, sizeof(path), "%s/imported.jsonl", g_home);
    ASSERT_OK(casi_store_materialize_to(g_repo, g_roots, &entry,
                                        path, &unmapped));
    file = fopen(path, "rb");
    ASSERT_TRUE(file != NULL);
    n = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    content[n] = '\0';
    ASSERT_EQ_STR(content,
                  "{\"cwd\":\"/Users/bob/src/otel-lab\","
                  "\"instruction\":\"/Users/bob/src/CLAUDE.md\"}\n");
    casi_buf_dispose(&unmapped);
    free(entry.chunks);
}

static void test_legacy_work_memory_uses_the_src_project(void)
{
    casi_asset asset = { 0 };
    casi_buf content = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    const char *raw = "path: casi://work/CLAUDE.md\n";

    asset.project_path = (char *)"casi://work";
    asset.kind = CASI_AUX_MEMORY;
    asset.name = (char *)"legacy.md";
    ASSERT_OK(casi_repo_write_blob(g_repo, raw, strlen(raw), &asset.oid));
    ASSERT_OK(casi_store_materialize_asset_content(g_repo, g_roots, &asset,
                                                   &content, &unmapped));
    ASSERT_EQ_STR(casi_buf_cstr(&content), "path: /Users/bob/src/CLAUDE.md\n");
    casi_buf_dispose(&content);
    casi_buf_dispose(&unmapped);
}

int main(void)
{
    int status;

    if (mkdtemp(g_home) == NULL || setenv("CASI_HOME", g_home, 1) != 0)
        return 1;
    casi_paths_reset();
    if (casi_init() != CASI_OK || casi_repo_init(&g_repo) != CASI_OK ||
        casi_roots_new(&g_roots) != CASI_OK ||
        casi_roots_add(g_roots, "~", "/Users/bob") != CASI_OK ||
        casi_roots_add(g_roots, "src", "/Users/bob/src") != CASI_OK ||
        casi_roots_add(g_roots, "bookit", "/Users/bob/src/bookit") != CASI_OK ||
        casi_roots_add(g_roots, "otel-lab", "/Users/bob/src/otel-lab") != CASI_OK)
        return 1;

    RUN_TEST(test_legacy_paths_are_equivalent);
    RUN_TEST(test_local_append_is_safe);
    RUN_TEST(test_real_difference_stays_diverged);
    RUN_TEST(test_legacy_remote_append_stays_pending);
    RUN_TEST(test_unknown_other_project_stays_pending);
    RUN_TEST(test_auxiliary_paths_but_not_edits_are_equivalent);
    RUN_TEST(test_historic_foreign_home_does_not_create_a_conflict);
    RUN_TEST(test_moved_parent_does_not_create_a_conflict);
    RUN_TEST(test_unmapped_historic_parent_is_only_a_comparison_hint);
    RUN_TEST(test_unmapped_historic_child_is_only_a_comparison_hint);
    RUN_TEST(test_removed_checkout_child_is_only_a_comparison_hint);
    RUN_TEST(test_legacy_root_name_can_move_without_a_conflict);
    RUN_TEST(test_legacy_work_references_materialize_without_an_alias);
    RUN_TEST(test_legacy_work_memory_uses_the_src_project);
    status = casi_test_report("sync");

    casi_roots_free(g_roots);
    casi_repo_free(g_repo);
    casi_shutdown();
    unsetenv("CASI_HOME");
    casi_paths_reset();
    return status;
}
