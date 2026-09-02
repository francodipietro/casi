# Contributing to casi

## Building

```sh
cmake --preset dev        # Debug + ASan/UBSan + -Werror
cmake --build --preset dev
ctest --preset dev
```

Presets: `dev` (sanitizers), `debug` (no sanitizers, for valgrind/lldb),
`release`, `static` (vendored libgit2, for release binaries).

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
