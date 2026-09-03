/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_STR_H
#define CASI_STR_H

#include <stdbool.h>
#include <stddef.h>

/* strdup that reports through the casi error channel. Returns NULL on OOM. */
char *casi_strdup(const char *s);
char *casi_strndup(const char *s, size_t n);

bool casi_str_has_prefix(const char *s, const char *prefix);
bool casi_str_has_suffix(const char *s, const char *suffix);

/*
 * Owning, growable vector of strings. Used for directory listings, root
 * names, and the like. A zeroed casi_strvec is a valid empty vector.
 */
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} casi_strvec;

#define CASI_STRVEC_INIT { NULL, 0, 0 }

/* Takes a copy of `s`. */
int  casi_strvec_push(casi_strvec *v, const char *s);
/* Takes ownership of `s` (which must come from malloc). */
int  casi_strvec_push_owned(casi_strvec *v, char *s);
void casi_strvec_dispose(casi_strvec *v);
/* Sorts in place, byte order (strcmp), so listings are deterministic. */
void casi_strvec_sort(casi_strvec *v);

#endif /* CASI_STR_H */
