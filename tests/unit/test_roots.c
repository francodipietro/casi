/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/roots.h"

#include "casi_test.h"

#include <stdlib.h>
#include <unistd.h>

/* The three real machines from the project's own sync setup: one nested
 * layout, two flattened. This is the case a single prefix substitution
 * cannot express, and the reason roots exist at all. */
static casi_roots *laptop_roots(void)
{
    casi_roots *r = NULL;

    if (casi_roots_new(&r) != CASI_OK)
        return NULL;
    casi_roots_add(r, "src", "/home/fdipietro/src/work");
    casi_roots_add(r, "bookit", "/home/fdipietro/src/work/bookit");
    casi_roots_add(r, CASI_HOME_ROOT, "/home/fdipietro");
    return r;
}

static casi_roots *mac_roots(void)
{
    casi_roots *r = NULL;

    if (casi_roots_new(&r) != CASI_OK)
        return NULL;
    casi_roots_add(r, "src", "/Users/fdipietro/src");
    casi_roots_add(r, "bookit", "/Users/fdipietro/src/bookit");
    casi_roots_add(r, CASI_HOME_ROOT, "/Users/fdipietro");
    return r;
}

static void check_norm(const casi_roots *r, const char *local, const char *want)
{
    casi_buf got = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_normalize_path(r, local, &got));
    ASSERT_EQ_STR(casi_buf_cstr(&got), want);
    casi_buf_dispose(&got);
}

static void check_denorm(const casi_roots *r, const char *canonical, const char *want)
{
    casi_buf got = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_denormalize_path(r, canonical, &got));
    ASSERT_EQ_STR(casi_buf_cstr(&got), want);
    casi_buf_dispose(&got);
}

static void test_longest_prefix_wins(void)
{
    casi_roots *r = mac_roots();

    /* "bookit" is nested inside "src"; the deeper root must win. */
    check_norm(r, "/Users/fdipietro/src/bookit/hotels-engine",
               "casi://bookit/hotels-engine");
    check_norm(r, "/Users/fdipietro/src/gastotrack", "casi://src/gastotrack");
    check_norm(r, "/Users/fdipietro/Documents/x", "casi://~/Documents/x");

    casi_roots_free(r);
}

static void test_match_respects_component_boundaries(void)
{
    casi_roots *r = mac_roots();

    /* "/Users/fdipietro/srcfoo" must NOT match the "src" root. */
    check_norm(r, "/Users/fdipietro/srcfoo/x", "casi://~/srcfoo/x");
    /* An exact match with nothing after it is still a match. */
    check_norm(r, "/Users/fdipietro/src", "casi://src");

    casi_roots_free(r);
}

static void test_unmatched_path_passes_through(void)
{
    casi_roots *r = NULL;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_add(r, "src", "/opt/src"));
    check_norm(r, "/var/log/x", "/var/log/x");

    casi_roots_free(r);
}

/* The end-to-end point of the whole module: a path produced on the nested
 * Linux laptop has to land correctly on the flattened Mac. */
static void test_translation_between_differing_layouts(void)
{
    casi_roots *laptop = laptop_roots(), *mac = mac_roots();
    casi_buf canonical = CASI_BUF_INIT, local = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_normalize_path(laptop,
        "/home/fdipietro/src/work/bookit/hotels-engine/pom.xml", &canonical));
    ASSERT_EQ_STR(casi_buf_cstr(&canonical), "casi://bookit/hotels-engine/pom.xml");

    ASSERT_OK(casi_roots_denormalize_path(mac, casi_buf_cstr(&canonical), &local));
    ASSERT_EQ_STR(casi_buf_cstr(&local),
                  "/Users/fdipietro/src/bookit/hotels-engine/pom.xml");

    casi_buf_dispose(&canonical);
    casi_buf_dispose(&local);
    casi_roots_free(laptop);
    casi_roots_free(mac);
}

static void test_roundtrip_is_lossless(void)
{
    casi_roots *r = mac_roots();
    const char *cases[] = {
        "/Users/fdipietro/src/bookit/x/y.json",
        "/Users/fdipietro/src",
        "/Users/fdipietro/Documents/notes.md",
        "/etc/hosts",
        NULL
    };
    size_t i;

    for (i = 0; cases[i] != NULL; i++) {
        casi_buf canonical = CASI_BUF_INIT, back = CASI_BUF_INIT;

        ASSERT_OK(casi_roots_normalize_path(r, cases[i], &canonical));
        ASSERT_OK(casi_roots_denormalize_path(r, casi_buf_cstr(&canonical), &back));
        ASSERT_EQ_STR(casi_buf_cstr(&back), cases[i]);

        casi_buf_dispose(&canonical);
        casi_buf_dispose(&back);
    }

    casi_roots_free(r);
}

static void test_unknown_root_is_reported_not_guessed(void)
{
    casi_roots *r = NULL;
    casi_buf out = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_add(r, "src", "/Users/f/src"));

    ASSERT_RC(casi_roots_denormalize_path(r, "casi://vendor/thing", &out),
              CASI_EUNMAPPED);
    /* The message has to name the missing root, or the user cannot fix it. */
    ASSERT_TRUE(strstr(casi_error_last(), "vendor") != NULL);

    casi_buf_dispose(&out);
    casi_roots_free(r);
}

static void test_trailing_slashes_are_normalised_away(void)
{
    casi_roots *r = NULL;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_add(r, "src", "/Users/f/src///"));
    check_norm(r, "/Users/f/src/a", "casi://src/a");
    check_denorm(r, "casi://src/a", "/Users/f/src/a");

    casi_roots_free(r);
}

static void test_re_adding_a_name_replaces_it(void)
{
    casi_roots *r = NULL;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_add(r, "src", "/old/path"));
    ASSERT_OK(casi_roots_add(r, "src", "/new/path"));

    ASSERT_EQ_INT(casi_roots_count(r), 1);
    check_norm(r, "/new/path/x", "casi://src/x");
    check_norm(r, "/old/path/x", "/old/path/x");

    casi_roots_free(r);
}

static void test_empty_name_or_path_rejected(void)
{
    casi_roots *r = NULL;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_RC(casi_roots_add(r, "", "/a"), CASI_EINVAL);
    ASSERT_RC(casi_roots_add(r, "a", ""), CASI_EINVAL);
    ASSERT_RC(casi_roots_add(r, "bad)", "/a"), CASI_EINVAL);
    ASSERT_EQ_INT(casi_roots_count(r), 0);

    casi_roots_free(r);
}

/* --- text-level rewriting, which is what actually touches transcripts --- */

static void check_text_roundtrip(const casi_roots *from, const casi_roots *to,
                                 const char *input, const char *want_canonical,
                                 const char *want_local)
{
    casi_buf in = CASI_BUF_INIT, canonical = CASI_BUF_INIT, out = CASI_BUF_INIT;

    ASSERT_OK(casi_buf_puts(&in, input));
    ASSERT_OK(casi_roots_normalize_text(from, &in, &canonical));
    ASSERT_EQ_STR(casi_buf_cstr(&canonical), want_canonical);

    ASSERT_OK(casi_roots_denormalize_text(to, &canonical, &out, NULL));
    ASSERT_EQ_STR(casi_buf_cstr(&out), want_local);

    casi_buf_dispose(&in);
    casi_buf_dispose(&canonical);
    casi_buf_dispose(&out);
}

static void test_text_rewrites_every_occurrence(void)
{
    casi_roots *laptop = laptop_roots(), *mac = mac_roots();

    check_text_roundtrip(laptop, mac,
        "{\"cwd\":\"/home/fdipietro/src/work/bookit\","
        "\"text\":\"see /home/fdipietro/src/work/bookit/pom.xml and /home/fdipietro/notes\"}",
        "{\"cwd\":\"casi://bookit\","
        "\"text\":\"see casi://bookit/pom.xml and casi://~/notes\"}",
        "{\"cwd\":\"/Users/fdipietro/src/bookit\","
        "\"text\":\"see /Users/fdipietro/src/bookit/pom.xml and /Users/fdipietro/notes\"}");

    casi_roots_free(laptop);
    casi_roots_free(mac);
}

static void test_text_without_paths_is_untouched(void)
{
    casi_roots *r = mac_roots();
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT;
    const char *plain = "{\"type\":\"assistant\",\"n\":42,\"s\":\"nothing here\"}\n";

    ASSERT_OK(casi_buf_puts(&in, plain));
    ASSERT_OK(casi_roots_normalize_text(r, &in, &out));
    ASSERT_EQ_STR(casi_buf_cstr(&out), plain);

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_roots_free(r);
}

static void test_text_preserves_binary_bytes(void)
{
    casi_roots *r = mac_roots();
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT;
    const char raw[] = { 'a', '\0', '\r', '\n', 'b' };

    ASSERT_OK(casi_buf_put(&in, raw, sizeof(raw)));
    ASSERT_OK(casi_roots_normalize_text(r, &in, &out));
    ASSERT_EQ_INT(out.len, sizeof(raw));
    ASSERT_EQ_MEM(out.ptr, raw, sizeof(raw));

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_roots_free(r);
}

static void test_text_denormalize_names_the_missing_root(void)
{
    casi_roots *r = NULL;
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_add(r, "src", "/Users/f/src"));
    ASSERT_OK(casi_buf_puts(&in, "{\"cwd\":\"casi://vendor/x\"}"));
    ASSERT_OK(casi_buf_puts(&unmapped, "stale-root"));

    ASSERT_RC(casi_roots_denormalize_text(r, &in, &out, &unmapped), CASI_EUNMAPPED);
    ASSERT_EQ_STR(casi_buf_cstr(&unmapped), "vendor");

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_buf_dispose(&unmapped);
    casi_roots_free(r);
}

static void test_text_root_name_stops_at_json_delimiters(void)
{
    casi_roots *r = mac_roots();
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT;

    /* A bare root with no trailing path, closed by the JSON quote. */
    ASSERT_OK(casi_buf_puts(&in, "{\"cwd\":\"casi://src\"}"));
    ASSERT_OK(casi_roots_denormalize_text(r, &in, &out, NULL));
    ASSERT_EQ_STR(casi_buf_cstr(&out), "{\"cwd\":\"/Users/fdipietro/src\"}");

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_roots_free(r);
}

static void test_text_root_name_stops_at_path_punctuation(void)
{
    casi_roots *mac = mac_roots(), *laptop = laptop_roots();

    check_text_roundtrip(mac, laptop,
        "paths: (/Users/fdipietro/src), [/Users/fdipietro/src/bookit]!",
        "paths: (casi://src), [casi://bookit]!",
        "paths: (/home/fdipietro/src/work), "
        "[/home/fdipietro/src/work/bookit]!");

    casi_roots_free(mac);
    casi_roots_free(laptop);
}

static void test_text_bare_scheme_is_literal(void)
{
    casi_roots *r = mac_roots();
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    const char *text = "{\"message\":\"the scheme casi:// has no root\"}";

    ASSERT_OK(casi_buf_puts(&in, text));
    ASSERT_OK(casi_buf_puts(&unmapped, "stale-root"));
    ASSERT_OK(casi_roots_denormalize_text(r, &in, &out, &unmapped));
    ASSERT_EQ_STR(casi_buf_cstr(&out), text);
    ASSERT_EQ_INT(unmapped.len, 0);

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_buf_dispose(&unmapped);
    casi_roots_free(r);
}

static void test_loading_roots_from_config(void)
{
    casi_config *cfg = NULL;
    casi_roots *r = NULL;
    char tmpl[] = "/tmp/casi-test-roots-XXXXXX";
    casi_buf path = CASI_BUF_INIT;

    ASSERT_TRUE(mkdtemp(tmpl) != NULL);
    ASSERT_OK(casi_buf_printf(&path, "%s/config", tmpl));
    ASSERT_OK(casi_config_open_path(&cfg, casi_buf_cstr(&path)));

    ASSERT_OK(casi_config_set_string(cfg, "root.src.path", "/Users/f/src"));
    ASSERT_OK(casi_config_set_string(cfg, "root.bookit.path", "/Users/f/src/bookit"));
    /* Must be ignored: right section, wrong variable. */
    ASSERT_OK(casi_config_set_string(cfg, "root.src.comment", "not a path"));
    /* Must be ignored: not a root at all. */
    ASSERT_OK(casi_config_set_string(cfg, "core.machine", "mac-air"));

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_load(r, cfg));

    /* Two configured roots plus the implicit home fallback. */
    ASSERT_EQ_INT(casi_roots_count(r), 3);
    check_norm(r, "/Users/f/src/bookit/x", "casi://bookit/x");
    check_norm(r, "/Users/f/src/other", "casi://src/other");

    casi_roots_free(r);
    casi_config_free(cfg);
    casi_buf_dispose(&path);
}

static void test_auto_project_names_encode_invalid_bytes(void)
{
    casi_roots *r = NULL;
    casi_buf in = CASI_BUF_INIT, out = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_canonicalize_project(r, "/work/My Project", &out));
    ASSERT_EQ_STR(casi_buf_cstr(&out), "casi://My~20Project");
    ASSERT_OK(casi_buf_puts(&in, "cwd /work/My Project/file"));
    ASSERT_OK(casi_roots_normalize_text(r, &in, &out));
    ASSERT_EQ_STR(casi_buf_cstr(&out), "cwd casi://My~20Project/file");

    casi_buf_dispose(&in);
    casi_buf_dispose(&out);
    casi_roots_free(r);
}

static void test_auto_project_name_collision_is_rejected(void)
{
    casi_roots *r = NULL;
    casi_buf out = CASI_BUF_INIT;

    ASSERT_OK(casi_roots_new(&r));
    ASSERT_OK(casi_roots_canonicalize_project(r, "/one/app", &out));
    ASSERT_RC(casi_roots_canonicalize_project(r, "/two/app", &out), CASI_EEXISTS);

    casi_buf_dispose(&out);
    casi_roots_free(r);
}

int main(void)
{
    int status;

    if (casi_init() != CASI_OK) {
        fprintf(stderr, "casi_init: %s\n", casi_error_last());
        return 1;
    }

    RUN_TEST(test_longest_prefix_wins);
    RUN_TEST(test_match_respects_component_boundaries);
    RUN_TEST(test_unmatched_path_passes_through);
    RUN_TEST(test_translation_between_differing_layouts);
    RUN_TEST(test_roundtrip_is_lossless);
    RUN_TEST(test_unknown_root_is_reported_not_guessed);
    RUN_TEST(test_trailing_slashes_are_normalised_away);
    RUN_TEST(test_re_adding_a_name_replaces_it);
    RUN_TEST(test_empty_name_or_path_rejected);
    RUN_TEST(test_text_rewrites_every_occurrence);
    RUN_TEST(test_text_without_paths_is_untouched);
    RUN_TEST(test_text_preserves_binary_bytes);
    RUN_TEST(test_text_denormalize_names_the_missing_root);
    RUN_TEST(test_text_root_name_stops_at_json_delimiters);
    RUN_TEST(test_text_root_name_stops_at_path_punctuation);
    RUN_TEST(test_text_bare_scheme_is_literal);
    RUN_TEST(test_loading_roots_from_config);
    RUN_TEST(test_auto_project_names_encode_invalid_bytes);
    RUN_TEST(test_auto_project_name_collision_is_rejected);

    status = casi_test_report("roots");
    casi_shutdown();
    return status;
}
