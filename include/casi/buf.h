/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_BUF_H
#define CASI_BUF_H

#include <stddef.h>

/*
 * Growable byte buffer. Content is binary-safe (len is authoritative), but a
 * NUL is always kept one past the end so the buffer can be handed to C string
 * APIs without copying.
 *
 * A zeroed casi_buf is a valid empty buffer; CASI_BUF_INIT spells that out.
 */
typedef struct {
    char  *ptr;
    size_t len;  /* bytes of content, not counting the trailing NUL */
    size_t cap;  /* bytes allocated, including room for that NUL    */
} casi_buf;

#define CASI_BUF_INIT { NULL, 0, 0 }

/* Ensures room for `extra` more bytes plus the terminator. */
int  casi_buf_grow(casi_buf *buf, size_t extra);
int  casi_buf_put(casi_buf *buf, const void *data, size_t len);
int  casi_buf_puts(casi_buf *buf, const char *str);
int  casi_buf_putc(casi_buf *buf, char c);
int  casi_buf_printf(casi_buf *buf, const char *fmt, ...);
int  casi_buf_set(casi_buf *buf, const void *data, size_t len);

/* Drops the content but keeps the allocation, for reuse in a loop. */
void casi_buf_clear(casi_buf *buf);
void casi_buf_dispose(casi_buf *buf);

/* Hands ownership of the (NUL-terminated) bytes to the caller and resets the
 * buffer. Returns NULL if the buffer never allocated. */
char *casi_buf_detach(casi_buf *buf);

/* Never NULL: an empty buffer reads back as "". */
const char *casi_buf_cstr(const casi_buf *buf);

#endif /* CASI_BUF_H */
