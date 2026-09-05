/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/jsonl.h"
#include "casi/error.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

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
        case '"':  c = '"';  break;
        case '\\': c = '\\'; break;
        case '/':  c = '/';  break;
        case 'n':  c = '\n'; break;
        case 't':  c = '\t'; break;
        case 'r':  c = '\r'; break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'u':
            /* casi's writer uses \u00XX for control bytes. Decode exactly
             * those; preserve other Unicode escapes verbatim rather than
             * pretending this tiny scanner is a full JSON parser. */
            if (i + 5 < len && p[i + 2] == '0' && p[i + 3] == '0') {
                int hi = hex_value(p[i + 4]);
                int lo = hex_value(p[i + 5]);

                if (hi >= 0 && lo >= 0 && ((hi << 4) | lo) < 0x20) {
                    if (casi_buf_putc(out, (char)((hi << 4) | lo)) != CASI_OK)
                        return 0;
                    i += 6;
                    continue;
                }
            }
            if (i + 5 < len && casi_buf_put(out, p + i, 6) == CASI_OK) {
                i += 6;
                continue;
            }
            return 0;
        default:
            return 0;
        }
        if (casi_buf_putc(out, c) != CASI_OK)
            return 0;
        i += 2;
    }

    return 0;
}

static bool is_json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Finds a key only at the start of a JSON string token. When a token is not
 * the requested key, skip its whole body (including escaped quotes) so key
 * lookalikes inside a message cannot be mistaken for structure. */
static bool find_value_start(const char *data, size_t len, const char *key,
                             size_t *value_out)
{
    size_t key_len = strlen(key);
    size_t i = 0;

    while (i < len) {
        size_t end, colon, value;

        if (data[i] != '"') {
            i++;
            continue;
        }

        end = i + 1;
        while (end < len && data[end] != '"') {
            if (data[end] == '\\') {
                if (end + 1 >= len)
                    return false;
                end += 2;
            } else {
                end++;
            }
        }
        if (end >= len)
            return false;

        colon = end + 1;
        while (colon < len && is_json_space(data[colon]))
            colon++;
        if (end - i - 1 == key_len &&
            memcmp(data + i + 1, key, key_len) == 0 &&
            colon < len && data[colon] == ':') {
            value = colon + 1;
            while (value < len && is_json_space(data[value]))
                value++;
            *value_out = value;
            return true;
        }

        i = end + 1;
    }

    return false;
}

bool casi_json_find_string(const char *data, size_t len, const char *key, casi_buf *out)
{
    size_t value;

    /* Scanning rather than parsing means a key of the same name nested deeper
     * could in principle match first; for the fields casi reads (cwd,
     * sessionId) the top-level occurrence comes first in practice. */
    if (find_value_start(data, len, key, &value) &&
        value < len && data[value] == '"')
        return unescape_into(data + value + 1, len - value - 1, out) > 0;

    return false;
}

bool casi_json_find_string_array(const char *data, size_t len, const char *key,
                                 casi_strvec *out)
{
    casi_strvec values = CASI_STRVEC_INIT;
    casi_buf value = CASI_BUF_INIT;
    size_t pos;
    bool ok = false;

    if (!find_value_start(data, len, key, &pos) || pos >= len || data[pos] != '[')
        goto done;
    pos++;

    for (;;) {
        size_t used;

        while (pos < len && is_json_space(data[pos]))
            pos++;
        if (pos >= len)
            goto done;
        if (data[pos] == ']') {
            ok = true;
            break;
        }
        if (data[pos] != '"')
            goto done;

        used = unescape_into(data + pos + 1, len - pos - 1, &value);
        if (used == 0 || casi_strvec_push(&values, casi_buf_cstr(&value)) != CASI_OK)
            goto done;
        pos += used + 1;  /* opening quote plus string body/closing quote */

        while (pos < len && is_json_space(data[pos]))
            pos++;
        if (pos < len && data[pos] == ',') {
            pos++;
            continue;
        }
        if (pos < len && data[pos] == ']') {
            ok = true;
            break;
        }
        goto done;
    }

done:
    casi_buf_dispose(&value);
    if (!ok) {
        casi_strvec_dispose(&values);
        return false;
    }

    casi_strvec_dispose(out);
    *out = values;
    return true;
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

int casi_json_escape_string(const char *value, casi_buf *out)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)value;
    int rc;

    casi_buf_clear(out);
    for (; *p != '\0'; p++) {
        const char *escape = NULL;

        switch (*p) {
        case '"':  escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b";  break;
        case '\f': escape = "\\f";  break;
        case '\n': escape = "\\n";  break;
        case '\r': escape = "\\r";  break;
        case '\t': escape = "\\t";  break;
        default: break;
        }

        if (escape != NULL) {
            if ((rc = casi_buf_puts(out, escape)) != CASI_OK)
                return rc;
        } else if (*p < 0x20) {
            char encoded[6] = { '\\', 'u', '0', '0',
                                hex[*p >> 4], hex[*p & 0x0f] };

            if ((rc = casi_buf_put(out, encoded, sizeof(encoded))) != CASI_OK)
                return rc;
        } else if ((rc = casi_buf_putc(out, (char)*p)) != CASI_OK) {
            return rc;
        }
    }

    return CASI_OK;
}

uint64_t casi_json_find_uint(const char *data, size_t len, const char *key)
{
    size_t i;
    uint64_t value = 0;

    if (!find_value_start(data, len, key, &i) ||
        i >= len || data[i] < '0' || data[i] > '9')
        return 0;

    while (i < len && data[i] >= '0' && data[i] <= '9')
        value = value * 10 + (uint64_t)(data[i++] - '0');

    return value;
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
