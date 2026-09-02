/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/error.h"
#include "casi/paths.h"

#include "casi_test.h"

#include <stdlib.h>

/* Each case owns the environment: reset the cache so the next lookup
 * re-reads it. */
static void use_env(const char *home, const char *xdg_config,
                    const char *xdg_data, const char *casi_home,
                    const char *claude_home)
{
    casi_paths_reset();

    unsetenv("HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("CASI_HOME");
    unsetenv("CASI_CLAUDE_HOME");

    if (home)         setenv("HOME", home, 1);
    if (xdg_config)   setenv("XDG_CONFIG_HOME", xdg_config, 1);
    if (xdg_data)     setenv("XDG_DATA_HOME", xdg_data, 1);
    if (casi_home)    setenv("CASI_HOME", casi_home, 1);
    if (claude_home)  setenv("CASI_CLAUDE_HOME", claude_home, 1);
}

static void test_defaults_from_home(void)
{
    use_env("/home/franco", NULL, NULL, NULL, NULL);

    ASSERT_EQ_STR(casi_paths_config_file(), "/home/franco/.config/casi/config");
    ASSERT_EQ_STR(casi_paths_data_dir(),    "/home/franco/.local/share/casi");
    ASSERT_EQ_STR(casi_paths_repo(),        "/home/franco/.local/share/casi/repo.git");
    ASSERT_EQ_STR(casi_paths_index(),       "/home/franco/.local/share/casi/index");
    ASSERT_EQ_STR(casi_paths_conflicts_dir(),
                  "/home/franco/.local/share/casi/conflicts");
    ASSERT_EQ_STR(casi_paths_claude_home(), "/home/franco/.claude");
}

static void test_xdg_overrides_home(void)
{
    use_env("/home/franco", "/cfg", "/data", NULL, NULL);

    ASSERT_EQ_STR(casi_paths_config_file(), "/cfg/casi/config");
    ASSERT_EQ_STR(casi_paths_data_dir(),    "/data/casi");
    ASSERT_EQ_STR(casi_paths_repo(),        "/data/casi/repo.git");
    /* CASI_CLAUDE_HOME is unset, so ~/.claude still wins here. */
    ASSERT_EQ_STR(casi_paths_claude_home(), "/home/franco/.claude");
}

static void test_casi_home_collapses_config_and_data(void)
{
    /* The integration-test override: one directory holds everything, so
     * several isolated "machines" can run side by side. */
    use_env("/home/franco", "/cfg", "/data", "/tmp/machine-a", NULL);

    ASSERT_EQ_STR(casi_paths_config_file(), "/tmp/machine-a/config");
    ASSERT_EQ_STR(casi_paths_data_dir(),    "/tmp/machine-a");
    ASSERT_EQ_STR(casi_paths_repo(),        "/tmp/machine-a/repo.git");
    ASSERT_EQ_STR(casi_paths_conflicts_dir(), "/tmp/machine-a/conflicts");
}

static void test_claude_home_override(void)
{
    use_env("/home/franco", NULL, NULL, NULL, "/tmp/machine-a/.claude");

    ASSERT_EQ_STR(casi_paths_claude_home(), "/tmp/machine-a/.claude");
    /* Overriding the assistant's home must not move casi's own state. */
    ASSERT_EQ_STR(casi_paths_data_dir(), "/home/franco/.local/share/casi");
}

static void test_empty_env_var_is_treated_as_unset(void)
{
    use_env("/home/franco", "", "", "", "");

    ASSERT_EQ_STR(casi_paths_config_file(), "/home/franco/.config/casi/config");
    ASSERT_EQ_STR(casi_paths_claude_home(), "/home/franco/.claude");
}

static void test_no_home_reports_notfound(void)
{
    use_env(NULL, NULL, NULL, NULL, NULL);

    ASSERT_TRUE(casi_paths_config_file() == NULL);
    ASSERT_EQ_INT(casi_error_last_code(), CASI_ENOTFOUND);
}

static void test_repeated_calls_are_stable(void)
{
    const char *first, *second;

    use_env("/home/franco", NULL, NULL, NULL, NULL);

    first = casi_paths_repo();
    second = casi_paths_repo();

    /* Cached: same pointer, and crucially not a path that grew a second
     * "/repo.git" on the way through. */
    ASSERT_TRUE(first == second);
    ASSERT_EQ_STR(second, "/home/franco/.local/share/casi/repo.git");
}

int main(void)
{
    int status;

    RUN_TEST(test_defaults_from_home);
    RUN_TEST(test_xdg_overrides_home);
    RUN_TEST(test_casi_home_collapses_config_and_data);
    RUN_TEST(test_claude_home_override);
    RUN_TEST(test_empty_env_var_is_treated_as_unset);
    RUN_TEST(test_no_home_reports_notfound);
    RUN_TEST(test_repeated_calls_are_stable);

    status = casi_test_report("paths");
    casi_paths_reset();
    return status;
}
