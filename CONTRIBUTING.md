# Contributing to casi

## Building

```sh
cmake --preset dev        # Debug + ASan/UBSan + -Werror
cmake --build --preset dev
ctest --preset dev
```

Presets: `dev` (sanitizers), `debug` (no sanitizers, for valgrind/lldb),
`release`, `static` (vendored libgit2 plus static libsodium, used for release
artifacts; Linux releases also set `CASI_FULLY_STATIC=ON`).

## House style

C11, four-space indent, no tabs. Broadly kernel-ish, but not dogmatic:

- Functions return `0` on success and a negative `casi_result` on failure.
  Set the message first: `return casi_error_set(CASI_EIO, "cannot read %s", path);`
- Every allocation has one owner, and the `dispose` sits in the same function
  that created it. `goto out;` for multi-step cleanup.
- Declare at the top of a block. Static where it need not be exported.
- No syscalls outside `src/util/fs.c`. That file is the whole Windows port.
- Comments say *why*. The code already says what.

## Tests

Unit tests live in `tests/unit/`, one file per module, using the header-only
harness in `tests/casi_test.h`. Adding a test file means adding its stem to
`CASI_UNIT_TESTS` in `tests/CMakeLists.txt`.

Integration tests run two isolated "machines" against a `file://` remote by
setting `CASI_HOME` and `CASI_CLAUDE_HOME`. They must not touch the real
`~/.claude` or need a network.

Never commit a real session transcript as a fixture: they contain whatever the
author was working on. Generate synthetic ones instead.

## Licensing

AGPL-3.0-only. Every source file starts with:

```c
/* SPDX-License-Identifier: AGPL-3.0-only */
```

By contributing you agree your work ships under that license.

## Soft exit codes report themselves

`CASI_ECONFLICT` (exit 3) and `CASI_EUNMAPPED` (exit 5) are documented
outcomes, not failures `main()` narrates for you. A command returning either
one must have already told the user everything via `casi_warn()`/`casi_info()`
before returning -- `main()` deliberately does not print `casi_error_last()`
for these two codes, to avoid duplicating that report or, if something cleared
the error state in the meantime, printing a meaningless "unknown error" over a
perfectly good one. Both failure modes were caught by hand while testing
`casi pull`'s conflict path, not invented.

Every other non-OK code still goes through the single `main()` print, so
`casi_error_set()` is still the right (and only) way to report those.
