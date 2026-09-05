/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/shared_config.h"

#include "casi_test.h"

#include <string.h>

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

static void test_rejects_bad_shared_values(void)
{
    casi_shared_config cfg = { 0 };

    ASSERT_RC(casi_shared_config_add_root(&cfg, "~"), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_add_exclude(&cfg, "/Users/me/private"), CASI_EINVAL);
    ASSERT_RC(casi_shared_config_parse(&cfg,
                                       "{\"format\":1,\"roots\":{}}", 24),
              CASI_EINVAL);

    casi_shared_config_dispose(&cfg);
}

int main(void)
{
    int status;

    if (casi_init() != CASI_OK)
        return 1;

    RUN_TEST(test_serialization_is_deterministic);
    RUN_TEST(test_roundtrip_and_revoke);
    RUN_TEST(test_phase_one_config_is_accepted);
    RUN_TEST(test_rejects_bad_shared_values);

    status = casi_test_report("shared_config");
    casi_shutdown();
    return status;
}
