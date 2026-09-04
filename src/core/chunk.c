/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/chunk.h"
#include "casi/error.h"

#include <string.h>

/*
 * End offset of the chunk starting at `start`: advance to the target size,
 * then run to just past the newline that closes the line we landed in. Falls
 * to end-of-input when the data runs out first, which is what makes the last
 * chunk the only short one.
 *
 * The search begins at `target - 1`, not `target`. Landing exactly on a line
 * start means the preceding byte was already the newline that closes the
 * chunk, and searching from `target` would skip past it and swallow a second
 * line -- doubling every chunk whenever the target happens to align with a
 * line boundary.
 */
static size_t chunk_end(const char *data, size_t len, size_t start, size_t target)
{
    const char *newline;
    size_t at;

    if (target == 0)
        target = 1;

    if (len - start <= target)
        return len;

    at = start + target - 1;
    newline = memchr(data + at, '\n', len - at);
    if (newline == NULL)
        return len;

    return (size_t)(newline - data) + 1;
}

int casi_chunk_split(const void *data, size_t len, size_t target,
                     casi_chunk_cb cb, void *payload)
{
    const char *p = data;
    size_t start = 0, index = 0;

    if (len == 0)
        return CASI_OK;

    if (p == NULL)
        return casi_error_set(CASI_EINVAL, "chunking a null buffer of %zu bytes", len);

    while (start < len) {
        size_t end = chunk_end(p, len, start, target);
        int rc = cb(p + start, end - start, index, payload);

        if (rc != CASI_OK)
            return rc;

        start = end;
        index++;
    }

    return CASI_OK;
}

size_t casi_chunk_count(const void *data, size_t len, size_t target)
{
    const char *p = data;
    size_t start = 0, count = 0;

    if (len == 0 || p == NULL)
        return 0;

    while (start < len) {
        start = chunk_end(p, len, start, target);
        count++;
    }

    return count;
}
