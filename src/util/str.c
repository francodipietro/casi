/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/str.h"
#include "casi/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *casi_strdup(const char *s)
{
    return casi_strndup(s, strlen(s));
}

char *casi_strndup(const char *s, size_t n)
{
    char *p = malloc(n + 1);

    if (p == NULL) {
        casi_error_set(CASI_ENOMEM, "out of memory duplicating %zu bytes", n);
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

bool casi_str_has_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

bool casi_str_has_suffix(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);

    if (fl > sl)
        return false;
    return memcmp(s + sl - fl, suffix, fl) == 0;
}

static int strvec_grow(casi_strvec *v)
{
    size_t cap;
    char **p;

    if (v->len < v->cap)
        return CASI_OK;

    if (v->cap == 0) {
        cap = 8;
    } else {
        if (v->cap > SIZE_MAX / 2 / sizeof(char *))
            return casi_error_set(CASI_ENOMEM, "string vector too large");
        cap = v->cap * 2;
    }

    p = realloc(v->items, cap * sizeof(char *));
    if (p == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory growing string vector");

    v->items = p;
    v->cap = cap;
    return CASI_OK;
}

int casi_strvec_push(casi_strvec *v, const char *s)
{
    char *copy = casi_strdup(s);
    int rc;

    if (copy == NULL)
        return CASI_ENOMEM;

    if ((rc = casi_strvec_push_owned(v, copy)) != CASI_OK)
        free(copy);

    return rc;
}

int casi_strvec_push_owned(casi_strvec *v, char *s)
{
    int rc;

    if ((rc = strvec_grow(v)) != CASI_OK)
        return rc;

    v->items[v->len++] = s;
    return CASI_OK;
}

void casi_strvec_dispose(casi_strvec *v)
{
    size_t i;

    for (i = 0; i < v->len; i++)
        free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static int strvec_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void casi_strvec_sort(casi_strvec *v)
{
    if (v->len > 1)
        qsort(v->items, v->len, sizeof(char *), strvec_cmp);
}
