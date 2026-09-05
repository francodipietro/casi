/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_JSONL_H
#define CASI_JSONL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "casi/buf.h"
#include "casi/str.h"

/*
 * Just enough JSON to read a couple of string fields out of a transcript.
 *
 * casi deliberately does not have a JSON parser. It never rewrites a record
 * by parsing and re-serialising it -- that could reorder keys or reshape
 * numbers and break resuming the session -- so the only thing it ever needs
 * is to *read* a named string field. That is a scan, not a parse.
 */

/* Finds "<key>":"<value>" in one JSON object and unescapes the value.
 * false when the key is absent. */
bool casi_json_find_string(const char *data, size_t len, const char *key, casi_buf *out);

/* Reads an array whose members are all JSON strings. `out` is replaced only
 * on success; false means either that the key is absent or that its value is
 * not a well-formed string array. This intentionally remains a small scanner
 * rather than growing a general-purpose JSON parser into the transcript path. */
bool casi_json_find_string_array(const char *data, size_t len, const char *key,
                                 casi_strvec *out);

/* Same, but scans line by line and returns the first line that has the key --
 * how the canonical project path is recovered, since `cwd` moves as a session
 * runs and only the first record reflects where it started. */
bool casi_jsonl_first_string(const char *data, size_t len, const char *key, casi_buf *out);

/* Replaces `out` with `value` escaped for use inside a JSON string. */
int casi_json_escape_string(const char *value, casi_buf *out);

/* Reads a non-negative integer field; 0 when absent. Sizes only -- casi never
 * needs to interpret a number it did not write. */
uint64_t casi_json_find_uint(const char *data, size_t len, const char *key);

/* Cheap structural sanity check on one line: quotes closed, braces balanced.
 * Used after a path substitution to catch a rewrite that broke the record,
 * without paying for a real parse. */
bool casi_json_looks_balanced(const char *data, size_t len);

#endif /* CASI_JSONL_H */
