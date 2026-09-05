/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/repo.h"
#include "casi/shared_config.h"

#include "casi_test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_serialization_is_deterministic(void)
{
    casi_shared_config cfg = { 0 };
    casi_buf json = CASI_BUF_INIT;

    ASSERT_OK(casi_shared_config_add_root(&cfg, "src"));
    ASSERT_OK(casi_shared_config_add_root(&cfg, "bookit"));
    ASSERT_OK(casi_shared_config_add_root(&cfg, "src"));
    ASSERT_OK(casi_shared_config_add_exclude(&cfg, "casi://src/private"));
    ASSERT_OK(casi_shared_config_add_exclude(&cfg, "casi://bookit/client"));
    ASSERT_OK(casi_shared_config_add_exclude(&cfg, "casi://src/private"));

    ASSERT_OK(casi_shared_config_serialize(&cfg, &json));
    ASSERT_EQ_STR(casi_buf_cstr(&json),
                  "{\"format\":1,\"roots\":[\"bookit\",\"src\"],"
                  "\"sync\":{\"exclude\":[\"casi://bookit/client\","
                  "\"casi://src/private\"]}}\n");

    casi_buf_dispose(&json);
    casi_shared_config_dispose(&cfg);
}

static void test_roundtrip_and_revoke(void)
{
    const char json[] =
        "{\"format\":1,\"roots\":[\"src\",\"bookit\"],"
        "\"sync\":{\"exclude\":[\"casi://bookit/client\"]}}\n";
    casi_shared_config cfg = { 0 };

    ASSERT_OK(casi_shared_config_parse(&cfg, json, strlen(json)));
    ASSERT_EQ_INT(cfg.roots.len, 2);
    ASSERT_EQ_STR(cfg.roots.items[0], "bookit");
    ASSERT_EQ_STR(cfg.roots.items[1], "src");
    ASSERT_TRUE(casi_shared_config_is_excluded(&cfg, "casi://bookit/client"));

    ASSERT_OK(casi_shared_config_remove_exclude(&cfg, "casi://bookit/client"));
    ASSERT_FALSE(casi_shared_config_is_excluded(&cfg, "casi://bookit/client"));
    ASSERT_RC(casi_shared_config_remove_exclude(&cfg, "casi://bookit/client"),
              CASI_ENOTFOUND);

    casi_shared_config_dispose(&cfg);
}

static void test_phase_one_config_is_accepted(void)
{
    const char json[] = "{\"format\":1}\n";
    casi_shared_config cfg = { 0 };

    ASSERT_OK(casi_shared_config_parse(&cfg, json, strlen(json)));
    ASSERT_EQ_INT(cfg.roots.len, 0);
    ASSERT_EQ_INT(cfg.exclude.len, 0);

    casi_shared_config_dispose(&cfg);
}

static void test_parse_does_not_require_a_nul_terminator(void)
{
    const char json[] = { '{', '"', 'f', 'o', 'r', 'm', 'a', 't', '"', ':', '1', '}' };
    casi_shared_config cfg = { 0 };

    ASSERT_OK(casi_shared_config_parse(&cfg, json, sizeof(json)));
    ASSERT_EQ_INT(cfg.roots.len, 0);
    ASSERT_EQ_INT(cfg.exclude.len, 0);

    casi_shared_config_dispose(&cfg);
}

static void test_rejects_bad_shared_values(void)
{
    casi_shared_config cfg = { 0 };

    ASSERT_RC(casi_shared_config_add_root(&cfg, "~"), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_add_root(&cfg, "bad/name"), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_add_exclude(&cfg, NULL), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_add_exclude(&cfg, "/Users/me/private"), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_parse(&cfg,
                                       "{\"format\":1,\"roots\":{}}", 24),
              CASI_EINVAL);

    casi_shared_config_dispose(&cfg);
}

static void test_parse_enforces_shared_config_invariants(void)
{
    const char duplicate[] =
        "{\"format\":1,\"roots\":[\"src\",\"src\"],"
        "\"sync\":{\"exclude\":[\"casi://src/private\","
        "\"casi://src/private\"]}}";
    const char invalid_root[] = "{\"format\":1,\"roots\":[\"bad/name\"]}";
    const char invalid_exclude[] =
        "{\"format\":1,\"sync\":{\"exclude\":[\"/tmp/private\"]}}";
    const char malformed_exclude[] = "{\"format\":1,\"sync\":{\"exclude\":{}}}";
    const char forward_compatible[] =
        "{\"format\":1,\"excluded\":[],\"note\":\"roots remain optional\"}";
    casi_shared_config cfg = { 0 };

    ASSERT_OK(casi_shared_config_parse(&cfg, duplicate, strlen(duplicate)));
    ASSERT_EQ_INT(cfg.roots.len, 1);
    ASSERT_EQ_INT(cfg.exclude.len, 1);
    casi_shared_config_dispose(&cfg);

    ASSERT_RC(casi_shared_config_parse(&cfg, invalid_root, strlen(invalid_root)), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_parse(&cfg, invalid_exclude, strlen(invalid_exclude)),
              CASI_EINVAL);
    ASSERT_RC(casi_shared_config_parse(&cfg, malformed_exclude,
                                       strlen(malformed_exclude)), CASI_EINVAL);
    ASSERT_TRUE(strstr(casi_error_last(), "sync.exclude") != NULL);
    ASSERT_OK(casi_shared_config_parse(&cfg, forward_compatible,
                                       strlen(forward_compatible)));
    ASSERT_EQ_INT(cfg.roots.len, 0);
    ASSERT_EQ_INT(cfg.exclude.len, 0);

    casi_shared_config_dispose(&cfg);
}

static void test_identical_config_retries_an_unpublished_ref(void)
{
    char home[] = "/tmp/casi-shared-config-XXXXXX";
    casi_repo *repo = NULL;
    casi_shared_config cfg = { 0 };

    ASSERT_TRUE(mkdtemp(home) != NULL);
    ASSERT_EQ_INT(setenv("CASI_HOME", home, 1), 0);
    casi_paths_reset();

    ASSERT_OK(casi_repo_init(&repo));
    ASSERT_OK(casi_shared_config_add_root(&cfg, "src"));

    /* The first failure happens after the local commit was written. The
     * second identical call must retry the push, rather than mistaking that
     * local commit for a successfully published configuration. */
    ASSERT_RC(casi_shared_config_commit_push(repo, &cfg, "test-machine"),
              CASI_ENOTFOUND);
    ASSERT_RC(casi_shared_config_commit_push(repo, &cfg, "test-machine"),
              CASI_ENOTFOUND);

    casi_shared_config_dispose(&cfg);
    casi_repo_free(repo);
    unsetenv("CASI_HOME");
    casi_paths_reset();
}

int main(void)
{
    int status;

    if (casi_init() != CASI_OK)
        return 1;

    RUN_TEST(test_serialization_is_deterministic);
    RUN_TEST(test_roundtrip_and_revoke);
    RUN_TEST(test_phase_one_config_is_accepted);
    RUN_TEST(test_parse_does_not_require_a_nul_terminator);
    RUN_TEST(test_rejects_bad_shared_values);
    RUN_TEST(test_parse_enforces_shared_config_invariants);
    RUN_TEST(test_identical_config_retries_an_unpublished_ref);

    status = casi_test_report("shared_config");
    casi_shutdown();
    return status;
}
