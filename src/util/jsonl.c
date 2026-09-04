/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/jsonl.h"
#include "casi/error.h"

#include <string.h>

/* Copies a JSON string body into `out`, resolving the escapes that can appear
 * in a path. Stops at the closing quote. Returns bytes consumed, or 0. */
static size_t unescape_into(const char *p, size_t len, casi_buf *out)
{
    size_t i = 0;

    casi_buf_clear(out);

    while (i < len) {
        char c = p[i];

        if (c == '"')
            return i + 1;

        if (c != '\\') {
            if (casi_buf_putc(out, c) != CASI_OK)
                return 0;
            i++;
            continue;
        }

        if (i + 1 >= len)
            return 0;

        switch (p[i + 1]) {
        case '"':  casi_buf_putc(out, '"');  break;
        case '\\': casi_buf_putc(out, '\\'); break;
        case '/':  casi_buf_putc(out, '/');  break;
        case 'n':  casi_buf_putc(out, '\n'); break;
        case 't':  casi_buf_putc(out, '\t'); break;
        case 'r':  casi_buf_putc(out, '\r'); break;
        case 'b':  casi_buf_putc(out, '\b'); break;
        case 'f':  casi_buf_putc(out, '\f'); break;
        case 'u':
            /* A \u escape cannot appear inside a POSIX path that casi cares
             * about; keep it verbatim rather than half-decoding it. */
            if (i + 5 < len && casi_buf_put(out, p + i, 6) == CASI_OK) {
                i += 6;
                continue;
            }
            return 0;
        default:
            return 0;
        }
        i += 2;
    }

    return 0;
}

bool casi_json_find_string(const char *data, size_t len, const char *key, casi_buf *out)
{
    size_t key_len = strlen(key);
    size_t i = 0;

    /* Looking for the literal "<key>": followed by a quoted value. Scanning
     * rather than parsing means a key of the same name nested deeper could in
     * principle match first; for the fields casi reads (cwd, sessionId) the
     * top-level occurrence comes first in practice. */
    while (i + key_len + 4 < len) {
        const char *q = memchr(data + i, '"', len - i);
        size_t at;

        if (q == NULL)
            return false;

        at = (size_t)(q - data);
        if (at + key_len + 3 < len &&
            memcmp(data + at + 1, key, key_len) == 0 &&
            data[at + 1 + key_len] == '"' &&
            data[at + 2 + key_len] == ':') {
            size_t v = at + 3 + key_len;

            /* Tolerate a space after the colon; reject a non-string value. */
            while (v < len && (data[v] == ' ' || data[v] == '\t'))
                v++;
            if (v < len && data[v] == '"')
                return unescape_into(data + v + 1, len - v - 1, out) > 0;
        }

        i = at + 1;
    }

    return false;
}

bool casi_jsonl_first_string(const char *data, size_t len, const char *key, casi_buf *out)
{
    size_t start = 0;

    while (start < len) {
        const char *nl = memchr(data + start, '\n', len - start);
        size_t line_len = nl != NULL ? (size_t)(nl - data) - start : len - start;

        if (line_len > 0 &&
            casi_json_find_string(data + start, line_len, key, out))
            return true;

        if (nl == NULL)
            break;
        start = (size_t)(nl - data) + 1;
    }

    return false;
}

uint64_t casi_json_find_uint(const char *data, size_t len, const char *key)
{
    size_t key_len = strlen(key);
    size_t i = 0;

    while (i + key_len + 3 < len) {
        const char *q = memchr(data + i, '"', len - i);
        size_t at;

        if (q == NULL)
            return 0;

        at = (size_t)(q - data);
        if (at + key_len + 2 < len &&
            memcmp(data + at + 1, key, key_len) == 0 &&
            data[at + 1 + key_len] == '"' &&
            data[at + 2 + key_len] == ':') {
            size_t v = at + 3 + key_len;
            uint64_t value = 0;

            while (v < len && (data[v] == ' ' || data[v] == '\t'))
                v++;
            if (v >= len || data[v] < '0' || data[v] > '9')
                return 0;
            while (v < len && data[v] >= '0' && data[v] <= '9')
                value = value * 10 + (uint64_t)(data[v++] - '0');
            return value;
        }

        i = at + 1;
    }

    return 0;
}

bool casi_json_looks_balanced(const char *data, size_t len)
{
    size_t i;
    int depth = 0;
    bool in_string = false;

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (in_string) {
            if (c == '\\')
                i++;             /* skip whatever it escapes */
            else if (c == '"')
                in_string = false;
            continue;
        }

        switch (c) {
        case '"': in_string = true; break;
        case '{':
        case '[': depth++; break;
        case '}':
        case ']':
            if (--depth < 0)
                return false;
            break;
        default:
            break;
        }
    }

    return depth == 0 && !in_string;
}
