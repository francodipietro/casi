/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"

#include "casi_test.h"

#include <stdlib.h>
#include <unistd.h>

static char g_tmp[] = "/tmp/casi-test-config-XXXXXX";
static casi_buf g_cfg_path = CASI_BUF_INIT;

static casi_config *open_fresh(void)
{
    casi_config *cfg = NULL;

    casi_fs_remove_file(casi_buf_cstr(&g_cfg_path));
    if (casi_config_open_path(&cfg, casi_buf_cstr(&g_cfg_path)) != CASI_OK)
        return NULL;

    return cfg;
}

static void test_open_creates_missing_file(void)
{
    casi_config *cfg;

    casi_fs_remove_file(casi_buf_cstr(&g_cfg_path));
    ASSERT_FALSE(casi_fs_exists(casi_buf_cstr(&g_cfg_path)));

    ASSERT_OK(casi_config_open_path(&cfg, casi_buf_cstr(&g_cfg_path)));
    ASSERT_TRUE(casi_fs_exists(casi_buf_cstr(&g_cfg_path)));

    casi_config_free(cfg);
}

static void test_set_get_roundtrip(void)
{
    casi_config *cfg = open_fresh();
    casi_buf value = CASI_BUF_INIT;

    ASSERT_TRUE(cfg != NULL);

    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "mac-air"));
    ASSERT_OK(casi_config_get_string(cfg, "core.machine", &value));
    ASSERT_EQ_STR(casi_buf_cstr(&value), "mac-air");

    /* Subsections, which is how remotes and roots are spelled. */
    ASSERT_OK(casi_config_set_string(cfg, "remote.origin.url",
                                     "git@github.com:me/sessions.git"));
    ASSERT_OK(casi_config_get_string(cfg, "remote.origin.url", &value));
    ASSERT_EQ_STR(casi_buf_cstr(&value), "git@github.com:me/sessions.git");

    /* Subsection names are case-sensitive and may contain dots and dashes:
     * root."src/work" style keys have to survive verbatim. */
    ASSERT_OK(casi_config_set_string(cfg, "root.bookit.path",
                                     "/Users/fdipietro/src/bookit"));
    ASSERT_OK(casi_config_get_string(cfg, "root.bookit.path", &value));
    ASSERT_EQ_STR(casi_buf_cstr(&value), "/Users/fdipietro/src/bookit");

    casi_buf_dispose(&value);
    casi_config_free(cfg);
}

static void test_values_persist_across_reopen(void)
{
    casi_config *cfg = open_fresh();
    casi_buf value = CASI_BUF_INIT;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "nemo-pc"));
    casi_config_free(cfg);

    ASSERT_OK(casi_config_open_path(&cfg, casi_buf_cstr(&g_cfg_path)));
    ASSERT_OK(casi_config_get_string(cfg, "core.machine", &value));
    ASSERT_EQ_STR(casi_buf_cstr(&value), "nemo-pc");

    casi_buf_dispose(&value);
    casi_config_free(cfg);
}

static void test_missing_key_is_notfound(void)
{
    casi_config *cfg = open_fresh();
    casi_buf value = CASI_BUF_INIT;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_RC(casi_config_get_string(cfg, "core.nothing", &value), CASI_ENOTFOUND);
    ASSERT_RC(casi_config_unset(cfg, "core.nothing"), CASI_ENOTFOUND);

    casi_buf_dispose(&value);
    casi_config_free(cfg);
}

static void test_bool_falls_back(void)
{
    casi_config *cfg = open_fresh();
    bool value = false;

    ASSERT_TRUE(cfg != NULL);

    ASSERT_OK(casi_config_get_bool(cfg, "sync.memory", true, &value));
    ASSERT_TRUE(value);
    ASSERT_OK(casi_config_get_bool(cfg, "sync.memory", false, &value));
    ASSERT_FALSE(value);

    ASSERT_OK(casi_config_set_string(cfg, "sync.memory", "false"));
    ASSERT_OK(casi_config_get_bool(cfg, "sync.memory", true, &value));
    ASSERT_FALSE(value);

    ASSERT_OK(casi_config_set_string(cfg, "sync.memory", "true"));
    ASSERT_OK(casi_config_get_bool(cfg, "sync.memory", false, &value));
    ASSERT_TRUE(value);

    casi_config_free(cfg);
}

static void test_non_bool_is_rejected(void)
{
    casi_config *cfg = open_fresh();
    bool value = false;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_set_string(cfg, "sync.memory", "sometimes"));
    ASSERT_RC(casi_config_get_bool(cfg, "sync.memory", true, &value), CASI_EINVAL);

    casi_config_free(cfg);
}

static void test_unset_removes(void)
{
    casi_config *cfg = open_fresh();
    casi_buf value = CASI_BUF_INIT;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "gone-soon"));
    ASSERT_OK(casi_config_unset(cfg, "core.machine"));
    ASSERT_RC(casi_config_get_string(cfg, "core.machine", &value), CASI_ENOTFOUND);

    casi_buf_dispose(&value);
    casi_config_free(cfg);
}

static void test_multivar_replaces_output(void)
{
    casi_config *cfg = open_fresh();
    casi_strvec values = CASI_STRVEC_INIT;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_add_multivar(cfg, "sync.exclude", "casi://src/one"));
    ASSERT_OK(casi_config_add_multivar(cfg, "sync.exclude", "casi://src/two"));
    ASSERT_OK(casi_strvec_push(&values, "stale"));

    ASSERT_OK(casi_config_get_multivar(cfg, "sync.exclude", &values));
    ASSERT_EQ_INT(values.len, 2);
    ASSERT_EQ_STR(values.items[0], "casi://src/one");
    ASSERT_EQ_STR(values.items[1], "casi://src/two");

    /* A second read must replace rather than append the same two values. */
    ASSERT_OK(casi_config_get_multivar(cfg, "sync.exclude", &values));
    ASSERT_EQ_INT(values.len, 2);

    ASSERT_OK(casi_config_get_multivar(cfg, "sync.missing", &values));
    ASSERT_EQ_INT(values.len, 0);

    casi_strvec_dispose(&values);
    casi_config_free(cfg);
}

struct collect {
    casi_buf out;
};

static int collect_entry(const char *key, const char *value, void *payload)
{
    struct collect *c = payload;
    return casi_buf_printf(&c->out, "%s=%s;", key, value);
}

static void test_foreach_sees_every_key(void)
{
    casi_config *cfg = open_fresh();
    struct collect c = { CASI_BUF_INIT };

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "mac-air"));
    ASSERT_OK(casi_config_set_string(cfg, "remote.origin.url", "file:///tmp/r.git"));

    ASSERT_OK(casi_config_foreach(cfg, collect_entry, &c));

    ASSERT_TRUE(strstr(casi_buf_cstr(&c.out), "core.machine=mac-air;") != NULL);
    ASSERT_TRUE(strstr(casi_buf_cstr(&c.out),
                       "remote.origin.url=file:///tmp/r.git;") != NULL);

    casi_buf_dispose(&c.out);
    casi_config_free(cfg);
}

static int stop_immediately(const char *key, const char *value, void *payload)
{
    (void)key;
    (void)value;
    (*(int *)payload)++;
    return CASI_ECONFLICT;  /* an arbitrary non-OK code, to prove it propagates */
}

static void test_foreach_propagates_callback_error(void)
{
    casi_config *cfg = open_fresh();
    int calls = 0;

    ASSERT_TRUE(cfg != NULL);
    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "mac-air"));
    ASSERT_OK(casi_config_set_string(cfg, "core.provider", "claude-code"));

    ASSERT_RC(casi_config_foreach(cfg, stop_immediately, &calls), CASI_ECONFLICT);
    ASSERT_EQ_INT(calls, 1);

    casi_config_free(cfg);
}

int main(void)
{
    int status;

    if (mkdtemp(g_tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    if (casi_init() != CASI_OK) {
        fprintf(stderr, "casi_init: %s\n", casi_error_last());
        return 1;
    }
    if (casi_buf_printf(&g_cfg_path, "%s/config", g_tmp) != CASI_OK)
        return 1;

    RUN_TEST(test_open_creates_missing_file);
    RUN_TEST(test_set_get_roundtrip);
    RUN_TEST(test_values_persist_across_reopen);
    RUN_TEST(test_missing_key_is_notfound);
    RUN_TEST(test_bool_falls_back);
    RUN_TEST(test_non_bool_is_rejected);
    RUN_TEST(test_unset_removes);
    RUN_TEST(test_multivar_replaces_output);
    RUN_TEST(test_foreach_sees_every_key);
    RUN_TEST(test_foreach_propagates_callback_error);

    status = casi_test_report("config");

    if (status == 0) {
        casi_buf cmd = CASI_BUF_INIT;
        if (casi_buf_printf(&cmd, "rm -rf '%s'", g_tmp) == CASI_OK &&
            system(casi_buf_cstr(&cmd)) != 0)
            fprintf(stderr, "warning: could not clean up %s\n", g_tmp);
        casi_buf_dispose(&cmd);
    } else {
        fprintf(stderr, "scratch tree left at %s\n", g_tmp);
    }

    casi_buf_dispose(&g_cfg_path);
    casi_shutdown();
    return status;
}
