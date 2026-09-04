/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"
#include "casi/jsonl.h"

#include "casi_test.h"

static void test_escape_and_read_roundtrip(void)
{
    const char value[] = "casi://odd/a\"b\\c\n\t\x01";
    casi_buf escaped = CASI_BUF_INIT, json = CASI_BUF_INIT, decoded = CASI_BUF_INIT;

    ASSERT_OK(casi_json_escape_string(value, &escaped));
    ASSERT_EQ_STR(casi_buf_cstr(&escaped),
                  "casi://odd/a\\\"b\\\\c\\n\\t\\u0001");
    ASSERT_OK(casi_buf_printf(&json, "{\"projectPath\":\"%s\"}",
                              casi_buf_cstr(&escaped)));
    ASSERT_TRUE(casi_json_find_string(casi_buf_cstr(&json), json.len,
                                      "projectPath", &decoded));
    ASSERT_EQ_STR(casi_buf_cstr(&decoded), value);

    casi_buf_dispose(&escaped);
    casi_buf_dispose(&json);
    casi_buf_dispose(&decoded);
}

static void test_escape_replaces_existing_output(void)
{
    casi_buf escaped = CASI_BUF_INIT;

    ASSERT_OK(casi_buf_puts(&escaped, "stale"));
    ASSERT_OK(casi_json_escape_string("plain", &escaped));
    ASSERT_EQ_STR(casi_buf_cstr(&escaped), "plain");

    casi_buf_dispose(&escaped);
}

int main(void)
{
    int status;

    if (casi_init() != CASI_OK) {
        fprintf(stderr, "casi_init: %s\n", casi_error_last());
        return 1;
    }

    RUN_TEST(test_escape_and_read_roundtrip);
    RUN_TEST(test_escape_replaces_existing_output);

    status = casi_test_report("jsonl");
    casi_shutdown();
    return status;
}
