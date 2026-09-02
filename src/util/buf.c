/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/buf.h"
#include "casi/error.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int casi_buf_grow(casi_buf *buf, size_t extra)
{
    size_t need, cap;
    char *p;

    /* +1 for the terminator we always keep. */
    if (extra > SIZE_MAX - 1 || buf->len > SIZE_MAX - extra - 1)
        return casi_error_set(CASI_ENOMEM, "buffer size overflow");

    need = buf->len + extra + 1;
    if (need <= buf->cap)
        return CASI_OK;

    cap = buf->cap ? buf->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            return casi_error_set(CASI_ENOMEM, "buffer size overflow");
        cap *= 2;
    }

    p = realloc(buf->ptr, cap);
    if (p == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory growing buffer to %zu bytes", cap);

    buf->ptr = p;
    buf->cap = cap;
    return CASI_OK;
}

int casi_buf_put(casi_buf *buf, const void *data, size_t len)
{
    int rc;

    if (len == 0)
        return CASI_OK;

    if ((rc = casi_buf_grow(buf, len)) != CASI_OK)
        return rc;

    memcpy(buf->ptr + buf->len, data, len);
    buf->len += len;
    buf->ptr[buf->len] = '\0';
    return CASI_OK;
}

int casi_buf_puts(casi_buf *buf, const char *str)
{
    return casi_buf_put(buf, str, strlen(str));
}

int casi_buf_putc(casi_buf *buf, char c)
{
    return casi_buf_put(buf, &c, 1);
}

int casi_buf_printf(casi_buf *buf, const char *fmt, ...)
{
    va_list ap;
    int want, rc;

    va_start(ap, fmt);
    want = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (want < 0)
        return casi_error_set(CASI_ERROR, "formatting failed");

    if ((rc = casi_buf_grow(buf, (size_t)want)) != CASI_OK)
        return rc;

    va_start(ap, fmt);
    vsnprintf(buf->ptr + buf->len, (size_t)want + 1, fmt, ap);
    va_end(ap);

    buf->len += (size_t)want;
    return CASI_OK;
}

int casi_buf_set(casi_buf *buf, const void *data, size_t len)
{
    casi_buf_clear(buf);
    return casi_buf_put(buf, data, len);
}

void casi_buf_clear(casi_buf *buf)
{
    buf->len = 0;
    if (buf->ptr != NULL)
        buf->ptr[0] = '\0';
}

void casi_buf_dispose(casi_buf *buf)
{
    free(buf->ptr);
    buf->ptr = NULL;
    buf->len = 0;
    buf->cap = 0;
}

char *casi_buf_detach(casi_buf *buf)
{
    char *p = buf->ptr;

    buf->ptr = NULL;
    buf->len = 0;
    buf->cap = 0;
    return p;
}

const char *casi_buf_cstr(const casi_buf *buf)
{
    return buf->ptr != NULL ? buf->ptr : "";
}
