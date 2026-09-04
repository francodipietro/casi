/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/provider.h"
#include "casi/error.h"

#include <git2.h>

#include <stdlib.h>
#include <string.h>

extern const casi_provider casi_provider_claude_code;

static const casi_provider *const providers[] = {
    &casi_provider_claude_code,
    NULL
};

const casi_provider *casi_provider_default(void)
{
    return providers[0];
}

const casi_provider *casi_provider_lookup(const char *name)
{
    size_t i;

    for (i = 0; providers[i] != NULL; i++)
        if (strcmp(providers[i]->name, name) == 0)
            return providers[i];

    return NULL;
}

int casi_project_id(const char *project_path, casi_buf *out)
{
    git_oid oid;
    char hex[GIT_OID_MAX_HEXSIZE + 1];

    if (git_odb_hash(&oid, project_path, strlen(project_path), GIT_OBJECT_BLOB) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot derive id for %s", project_path);

    git_oid_tostr(hex, sizeof(hex), &oid);
    return casi_buf_set(out, hex, strlen(hex));
}

int casi_session_list_push(casi_session_list *list, const casi_session *session)
{
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        casi_session *p = realloc(list->items, cap * sizeof(*p));

        if (p == NULL)
            return casi_error_set(CASI_ENOMEM, "out of memory listing sessions");
        list->items = p;
        list->cap = cap;
    }

    list->items[list->len++] = *session;
    return CASI_OK;
}

void casi_session_list_dispose(casi_session_list *list)
{
    size_t i;

    for (i = 0; i < list->len; i++) {
        free(list->items[i].session_id);
        free(list->items[i].local_path);
        free(list->items[i].project_path);
        free(list->items[i].project_id);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}
