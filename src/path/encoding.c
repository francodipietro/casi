/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/encoding.h"
#include "casi/error.h"

#include <string.h>

/* Deliberately not isalnum(): that is locale-dependent, and two machines with
 * different locales have to derive byte-identical directory names. */
static int is_ascii_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

/* Length of the UTF-8 sequence starting at `s`, or 1 for anything malformed --
 * an invalid byte becomes its own dash rather than derailing the walk. */
static size_t utf8_seq_len(const unsigned char *s, size_t remaining)
{
    size_t len, i;

    if (s[0] < 0x80)
        return 1;
    else if ((s[0] & 0xE0) == 0xC0)
        len = 2;
    else if ((s[0] & 0xF0) == 0xE0)
        len = 3;
    else if ((s[0] & 0xF8) == 0xF0)
        len = 4;
    else
        return 1;

    if (len > remaining)
        return 1;

    for (i = 1; i < len; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 1;

    return len;
}

int casi_encode_project_dir(const char *path, casi_buf *out)
{
    const unsigned char *p = (const unsigned char *)path;
    size_t len = strlen(path);
    size_t i = 0;
    int rc;

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, len)) != CASI_OK)
        return rc;

    while (i < len) {
        if (is_ascii_alnum(p[i])) {
            /* Copy the whole alphanumeric run in one go. */
            size_t start = i;

            while (i < len && is_ascii_alnum(p[i]))
                i++;
            if ((rc = casi_buf_put(out, p + start, i - start)) != CASI_OK)
                return rc;
        } else {
            i += utf8_seq_len(p + i, len - i);
            if ((rc = casi_buf_putc(out, '-')) != CASI_OK)
                return rc;
        }
    }

    return CASI_OK;
}
