/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/shared_config.h"

#include "casi/error.h"
#include "casi/jsonl.h"
#include "casi/roots.h"

#include <stdlib.h>
#include <string.h>

static bool contains(const casi_strvec *items, const char *value)
{
    size_t i;

    if (value == NULL)
        return false;
    for (i = 0; i < items->len; i++)
        if (strcmp(items->items[i], value) == 0)
            return true;
    return false;
}

static int add_unique(casi_strvec *items, const char *value)
{
    int rc;

    if (contains(items, value))
        return CASI_OK;
    if ((rc = casi_strvec_push(items, value)) != CASI_OK)
        return rc;
    casi_strvec_sort(items);
    return CASI_OK;
}

void casi_shared_config_dispose(casi_shared_config *cfg)
{
    casi_strvec_dispose(&cfg->roots);
    casi_strvec_dispose(&cfg->exclude);
}

int casi_shared_config_add_root(casi_shared_config *cfg, const char *name)
{
    if (name == NULL || name[0] == '\0')
        return casi_error_set(CASI_EINVAL, "shared root name cannot be empty");
    if (!casi_root_name_is_valid(name))
        return casi_error_set(CASI_EINVAL,
                              "shared root name \"%s\" contains an invalid character", name);
    if (strcmp(name, CASI_HOME_ROOT) == 0)
        return casi_error_set(CASI_EINVAL, "the implicit ~ root is not shared");
    return add_unique(&cfg->roots, name);
}

int casi_shared_config_add_exclude(casi_shared_config *cfg, const char *project_path)
{
    if (project_path == NULL || project_path[0] == '\0')
        return casi_error_set(CASI_EINVAL, "shared exclusion cannot be empty");
    if (!casi_str_has_prefix(project_path, CASI_CANONICAL_SCHEME))
        return casi_error_set(CASI_EINVAL,
                              "shared exclusion is not canonical: %s", project_path);
    return add_unique(&cfg->exclude, project_path);
}

int casi_shared_config_remove_exclude(casi_shared_config *cfg, const char *project_path)
{
    size_t i;

    if (project_path == NULL || project_path[0] == '\0')
        return casi_error_set(CASI_EINVAL, "shared exclusion cannot be empty");
    for (i = 0; i < cfg->exclude.len; i++) {
        if (strcmp(cfg->exclude.items[i], project_path) == 0) {
            free(cfg->exclude.items[i]);
            memmove(&cfg->exclude.items[i], &cfg->exclude.items[i + 1],
                    (cfg->exclude.len - i - 1) * sizeof(*cfg->exclude.items));
            cfg->exclude.len--;
            return CASI_OK;
        }
    }

    return casi_error_set(CASI_ENOTFOUND, "not excluded: %s", project_path);
}

bool casi_shared_config_is_excluded(const casi_shared_config *cfg,
                                    const char *project_path)
{
    return contains(&cfg->exclude, project_path);
}

static int parse_array_or_empty(const char *data, size_t len, const char *key,
                                const char *field_name, casi_strvec *out)
{
    if (casi_json_find_string_array(data, len, key, out)) {
        casi_strvec_sort(out);
        return CASI_OK;
    }

    /* The two fields are optional only for the old Phase 1 {"format":1}
     * shape. If a named field is present but cannot be read as a string array,
     * fail closed instead of silently dropping an exclusion. */
    if (casi_json_has_key(data, len, key))
        return casi_error_set(CASI_EINVAL, "malformed %s array in casi.json", field_name);
    return CASI_OK;
}

int casi_shared_config_parse(casi_shared_config *cfg, const char *data, size_t len)
{
    casi_shared_config parsed = { 0 };
    casi_strvec roots = CASI_STRVEC_INIT, exclude = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    if (len == 0) {
        casi_shared_config_dispose(cfg);
        return CASI_OK;
    }
    if (casi_json_find_uint(data, len, "format") != CASI_SHARED_CONFIG_FORMAT)
        return casi_error_set(CASI_EINVAL, "unsupported or missing casi.json format");
    if ((rc = parse_array_or_empty(data, len, "roots", "roots", &roots)) != CASI_OK ||
        (rc = parse_array_or_empty(data, len, "exclude", "sync.exclude", &exclude)) != CASI_OK)
        goto done;
    for (i = 0; i < roots.len; i++)
        if ((rc = casi_shared_config_add_root(&parsed, roots.items[i])) != CASI_OK)
            goto done;
    for (i = 0; i < exclude.len; i++)
        if ((rc = casi_shared_config_add_exclude(&parsed, exclude.items[i])) != CASI_OK)
            goto done;

    casi_shared_config_dispose(cfg);
    *cfg = parsed;
    casi_strvec_dispose(&roots);
    casi_strvec_dispose(&exclude);
    return CASI_OK;

done:
    casi_strvec_dispose(&roots);
    casi_strvec_dispose(&exclude);
    casi_shared_config_dispose(&parsed);
    return rc;
}

static int append_array(casi_buf *out, const casi_strvec *items)
{
    casi_buf escaped = CASI_BUF_INIT;
    size_t i;
    int rc;

    if ((rc = casi_buf_putc(out, '[')) != CASI_OK)
        goto done;
    for (i = 0; i < items->len; i++) {
        if (i > 0 && (rc = casi_buf_putc(out, ',')) != CASI_OK)
            goto done;
        if ((rc = casi_json_escape_string(items->items[i], &escaped)) != CASI_OK)
            goto done;
        if ((rc = casi_buf_printf(out, "\"%s\"", casi_buf_cstr(&escaped))) != CASI_OK)
            goto done;
    }
    rc = casi_buf_putc(out, ']');

done:
    casi_buf_dispose(&escaped);
    return rc;
}

int casi_shared_config_serialize(const casi_shared_config *cfg, casi_buf *out)
{
    int rc;

    casi_buf_clear(out);
    if ((rc = casi_buf_printf(out, "{\"format\":%d,\"roots\":",
                              CASI_SHARED_CONFIG_FORMAT)) != CASI_OK ||
        (rc = append_array(out, &cfg->roots)) != CASI_OK ||
        (rc = casi_buf_puts(out, ",\"sync\":{\"exclude\":")) != CASI_OK ||
        (rc = append_array(out, &cfg->exclude)) != CASI_OK ||
        (rc = casi_buf_puts(out, "}}\n")) != CASI_OK)
        return rc;
    return CASI_OK;
}

int casi_shared_config_load_remote(casi_repo *repo, casi_shared_config *cfg,
                                   bool *present_out)
{
    git_oid tree;
    casi_buf json = CASI_BUF_INIT;
    int rc;

    if (present_out != NULL)
        *present_out = false;

    rc = casi_repo_ref_tree(repo, CASI_SHARED_CONFIG_REMOTE_REF, &tree);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = casi_shared_config_parse(cfg, "", 0);
        goto done;
    }
    if (rc != CASI_OK)
        goto done;
    if ((rc = casi_repo_tree_entry_blob(repo, &tree, "casi.json", &json)) != CASI_OK)
        goto done;
    if ((rc = casi_shared_config_parse(cfg, casi_buf_cstr(&json), json.len)) != CASI_OK)
        goto done;

    if (present_out != NULL)
        *present_out = true;
    rc = CASI_OK;

done:
    casi_buf_dispose(&json);
    return rc;
}

int casi_shared_config_commit_push(casi_repo *repo, const casi_shared_config *cfg,
                                   const char *machine)
{
    casi_tree *tree = NULL;
    casi_buf json = CASI_BUF_INIT;
    git_oid tree_oid, existing_tree, commit;
    int rc;

    if ((rc = casi_repo_reset_ref_from(repo, CASI_SHARED_CONFIG_REF,
                                       CASI_SHARED_CONFIG_REMOTE_REF)) != CASI_OK ||
        (rc = casi_shared_config_serialize(cfg, &json)) != CASI_OK ||
        (rc = casi_tree_new(repo, &tree)) != CASI_OK ||
        (rc = casi_tree_add_text(tree, "casi.json", casi_buf_cstr(&json))) != CASI_OK ||
        (rc = casi_tree_write(tree, &tree_oid)) != CASI_OK)
        goto done;

    if (casi_repo_ref_tree(repo, CASI_SHARED_CONFIG_REF, &existing_tree) == CASI_OK &&
        git_oid_equal(&tree_oid, &existing_tree)) {
        rc = CASI_OK;
        goto done;
    }
    casi_error_clear();

    if ((rc = casi_repo_commit(repo, CASI_SHARED_CONFIG_REF, &tree_oid, machine,
                               "casi: update shared configuration", &commit)) != CASI_OK)
        goto done;
    rc = casi_repo_push(repo, CASI_SHARED_CONFIG_REF);

done:
    casi_tree_free(tree);
    casi_buf_dispose(&json);
    return rc;
}
