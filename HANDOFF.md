# Handoff: casi

Originally written 2026-09-04 at the end of Phase 1, when Franco moved this
side project from Claude Code to Codex for ongoing work (Claude Code stays for
his day job). It preserves the empirical findings that a later agent must not
re-derive or assume — several came from live experimentation rather than
documentation.

> **Phase 2 closeout (2026-09-06).** Phase 1 was squash-merged as PR #2
> (`a3f0e41`), its follow-up as PR #3 (`d333228`), CI scoping as PR #4
> (`d869551`), and Phase 2 as PR #5 (`c9a19b5`). The Phase 1 branch snapshot
> and pre-PR checklist are historical. Sections 2, 4, 6 and 7 now give the
> current baseline; retain the rest for its empirical evidence and rationale.

Read this whole file before touching git or writing code. Section 2 in
particular describes a real risk to the actual work product.

## 1. What casi is

A CLI, in C, that syncs coding-assistant sessions (Claude Code first) between
machines over any git remote — the same relationship git has to code, but
exposing only `init`/`push`/`pull`/`sync`/`status`, never branches or commits.

- **User-facing pitch, build instructions, roadmap table:** [README.md](README.md)
- **Full original architecture plan** (the research into Claude Code's session
  format, the reasoning behind chunking/conflict rules/path normalization,
  the phase breakdown): [docs/PLAN.md](docs/PLAN.md) — read this for *why*,
  not this file
- **Design rationale for specific non-obvious decisions**, written as the
  decisions were made: [docs/DESIGN.md](docs/DESIGN.md)
- **House style, testing conventions, the error-reporting contract:**
  [CONTRIBUTING.md](CONTRIBUTING.md)

Those four files are the durable record. This file preserves the empirical
handoff evidence plus the Phase 2 closeout baseline; use it alongside the
current git state rather than as a replacement for the durable docs.

## 2. Current state — Phase 3 baseline

- Repo: `github.com/francodipietro/casi` (private). Remote `origin` uses SSH.
- `main`: `c9a19b5 feat(paths): phase 2 path normalization`, the squash merge
  of PR #5. It includes Phase 0, Phase 1 and its SSH/roundtrip follow-up,
  scoped CI, and Phase 2.
- Phase 2 made `refs/heads/casi/config` the authoritative shared
  configuration. It publishes named roots and `sync.exclude`, migrates legacy
  local exclusions on first publish, diagnoses shared root mappings with
  `casi doctor`, and syncs Claude Code subagents and project memory.
- The last merged PR had a final Copilot review with no actionable comments;
  its CI run was green on Linux/macOS debug and sanitizer jobs, valgrind,
  vendored-libgit2/exec-SSH, and GitGuardian.

Before beginning a new phase, refresh and inspect the actual baseline rather
than relying on a local branch:

```sh
git fetch origin
git log --oneline -3 origin/main
git status --short
```

Commit and PR convention: [Conventional
Commits](https://gist.github.com/joshbuchea/6f47e86d2510bce28f8e7f42ae84c716)
(`type(scope): summary`), no AI co-author trailer, one PR per phase, and
squash merge only with Franco's explicit authorization. The PR title is the
eventual commit message on `main`.

## 3. What's built (through Phase 2)

Phase 2 added the shared-configuration module, `casi doctor`, shared exclusion
mutation, and provider/store support for auxiliary assets. The relevant test
coverage is `test_shared_config` plus `shared_config`, `doctor`,
`auxiliary_files`, and `nested_roots` integration tests. Its detailed design
is deliberately recorded in `docs/DESIGN.md`; do not recreate a second source
of truth here.

The inventory below is the original Phase 1 snapshot. Read it as background
for the stable MVP machinery, not as a complete current file tree.

```
src/
  main.c                 dispatch; global -v/-q/--no-color parsed before the verb
  cmd/                   one file per verb: init, push, pull, sync, status,
                         exclude (also serves `include`), config, version, help
  cmd/sync_ops.{c,h}     shared push/pull machinery (fetch, build tree, walk
                         entries, apply the prefix rule) both commands call
  core/
    ctx.{c → include/casi/ctx.h}   everything a command needs, opened once:
                         config, roots, repo, machine name, exclude list
    chunk.c              the ~1 MiB line-boundary splitter
    repo.c               bare repo, blobs/trees/commits, fetch/push, SSH
                         credential + host-key callbacks (see §5.3)
    store.c              session <-> git tree: scan local sessions into
                         casi_entry_list, read/write the remote tree layout,
                         materialize a tree back into ~/.claude/projects/
    sync.c               the prefix relation from PLAN.md §6.2, as pure
                         comparison over two chunk-id lists
  provider/
    provider.c           the vtable registry (one provider today)
    claude_code.c        discover()/read_item()/materialize()/
                         encode_project_dir() for Claude Code
  path/
    encoding.c           project-directory name encoder (§5, below)
    roots.c              named roots: longest-prefix normalize/denormalize,
                         both for a single path and for paths embedded in text
  util/                  (Phase 0) error, buf, str, fs, log, paths, config
                         (Phase 1 addition) jsonl.c — line scanning + the
                         "still valid JSON after substitution" check
include/casi/            one header per module above, plus casi.h aggregating
tests/
  unit/                  test_{buf,str,fs,paths,config} (Phase 0) +
                         test_{encoding,roots,chunk} (Phase 1)
  integration/           two_machines.sh, conflict.sh — real casi binary,
                         real (temporary) git repos, `file://` remote, no
                         network
  cli_errors.sh          process-level checks (Phase 0)
```

CLI surface (`casi --help`):

```
casi init [--remote <url>] [--machine <name>]
casi push [--dry-run]
casi pull [--dry-run] [--theirs <session-id>]
casi sync
casi status [--porcelain]
casi exclude <path>          # stop syncing a project
casi include <path>          # resume syncing it
casi config <key> [<value>] | --list | --unset <key>
```

Phase 2 also ships the hidden plumbing command `casi doctor`, which reports
the active SSH backend, remote reachability, and shared-root mappings.

Exit codes match PLAN.md §4: `0` ok, `1` error, `2` usage, `3` conflict, `4`
network, `5` an unmapped root.

## 4. What's actually verified, and how

- `ctest --preset dev` (ASan+UBSan): **20/20 green** at the Phase 2 closeout.
  The suite includes `test_shared_config` and the four new Phase 2 integration
  tests. CI confirmed that same phase on Linux/macOS debug and sanitizer jobs,
  valgrind, and vendored libgit2 with the exec SSH transport.
- Phase 2 integration tests prove shared-exclusion migration and enforcement
  on a new machine, `doctor`'s unmapped-root exit and remediation, subagent and
  memory synchronisation/conflict parking, and the nested-root-to-flat-root
  mapping case.
- Manual end-to-end, by hand, redone after every fix until clean:
  - Two isolated "machines" (`$CASI_HOME`/`$CASI_CLAUDE_HOME` pointed at
    scratch dirs) with **different local layouts** (`~/src/...` vs
    `~/code/...`) and a shared `file://` remote.
  - Push from A, pull on B: session materializes under B's own re-encoded
    directory name (never A's), `cwd` field translated, paths embedded in
    message text translated, nothing of A's layout leaks into B's copy.
  - Append on A, push again: only the new chunk transfers (checked via `git
    count-objects -v` on the bare remote before/after).
  - Push again with nothing new: no new commit, ref unchanged (idempotent).
  - Real divergence (both sides append different content to the same
    session): `casi pull` exits 3, A's local file is untouched, B's is
    parked in `conflicts/`, `casi status` reports it, `casi pull --theirs
    <id>` takes the remote copy correctly.
  - All of the above is now automated in `tests/integration/two_machines.sh`
    and `conflict.sh` — the manual runs are what *found* the bugs in §5, the
    scripts are what stop them from coming back.
- The project-directory encoding rule (`[^a-zA-Z0-9]` → `-`, per Unicode code
  point) was **confirmed live against the real Claude Code 2.1.258 binary**,
  not assumed from the original plan's small sample. Method: `claude -p "x"
  --model definitely-not-a-real-model` inside a directory with a `.` in its
  name — it fails model validation *after* creating the session directory, so
  this costs zero API calls and still exercises the real encoder. See
  `src/path/encoding.c`'s header comment and `tests/unit/test_encoding.c` for
  the exact evidence trail. **If this ever needs re-checking (a Claude Code
  update, a new edge case), redo the same probe — don't guess.**
- Chunking's core promise (stable byte-identical prefixes as an append-only
  file grows) was checked against the actual 137 MB / 32,154-line session
  file on the development machine, not just synthetic data.

## 5. Bugs found while verifying, now fixed and covered

Listed because each one is a landmine an agent that hasn't seen the failure
could easily reintroduce elsewhere, and because the reasoning behind the fix
matters more than the diff.

1. **Chunk off-by-one.** Landing exactly on a line boundary looked for the
   *next* newline instead of accepting the boundary already found, silently
   duplicating a line into two chunks. Caught by hand against the real 137 MB
   file, not by a unit test — the unit tests' synthetic inputs happened not
   to hit that exact boundary. `src/core/chunk.c`, covered now in
   `tests/unit/test_chunk.c`.
2. **Double error printing.** `cmd_pull`/`cmd_status` returned `CASI_ECONFLICT`
   without setting an error message; `main()` printed
   `casi_error_last()` unconditionally on any non-OK code, so the user saw a
   perfectly good multi-line conflict report followed by a meaningless
   `error: unknown error`. Root cause and the fix (main() does not auto-print
   for `CASI_ECONFLICT`/`CASI_EUNMAPPED` — the returning command must have
   already said everything) are documented in **CONTRIBUTING.md, "Soft exit
   codes report themselves"**. This is a load-bearing convention for any new
   command: if you introduce a third "soft" exit code, follow the same rule
   or add it to that exemption list in `main.c`.
3. **stdout buffering scrambled output order.** `casi status`'s conflict
   warning (stderr, unbuffered) appeared *before* the status report (stdout,
   fully buffered when not a tty) in any redirected/piped output — exactly
   `casi sync >> log 2>&1`, the real use case this tool exists for. Fixed
   with `setvbuf(stdout, NULL, _IOLBF, 0)` in `casi_init()` (`src/casi.c`).
   Regression-tested in `tests/integration/conflict.sh` by checking line
   numbers in combined `2>&1` output, not just content.
4. **`casi_buf`'s own invariant was violated.** The header promises "a NUL is
   always kept one past the end", but `casi_buf_grow()` alone — with nothing
   written afterward — left freshly `realloc`'d memory uninitialised.
   `casi_encode_project_dir("")` hit exactly this. Found by ASan, not by
   inspection; fixed in the base module (`src/util/buf.c`) rather than
   papering over it in the one caller that happened to trigger it, because
   any future caller doing grow-then-maybe-nothing would hit the same wall.
   Regression test: `test_grow_alone_leaves_a_terminated_empty_buffer` in
   `tests/unit/test_buf.c`.
5. **A CI job can be "added" and still be broken.** Two brand-new CI jobs
   (`vendored`, `valgrind`) and one YAML edit were pushed on faith during
   Phase 0 and failed for three unrelated reasons (a `$<BUILD_INTERFACE:...>`
   generator-expression omission, `ctest -T memcheck` needing
   `include(CTest)` that this project never calls, and a `: ` inside a plain
   YAML scalar being parsed as a mapping). None of the three were visible
   from macOS. **Lesson applied since:** a CI change isn't done until its own
   run is actually green, not until it merges without visible local errors.

## 6. Known gaps — Phase 3 baseline

1. **No stat-cache or append-tail hashing.** `casi_paths_index()` reserves the
   on-disk location, but scans still read and normalise complete transcripts.
   This is the main performance feature left for Phase 3.
2. **The large-session proof is still manual.** The chunking strategy was
   exercised against the real 137 MB session recorded in §4, but the planned
   synthetic 200 MB push/append/push transfer-budget test is still absent.
3. **Conflict recovery is not yet discoverable as a command.** Parking and
   `casi pull --theirs` work; the planned `casi conflicts` command does not.
   GC/repack plumbing is also still absent.
4. **Interruption handling needs an explicit audit.** Transcript and auxiliary
   materialisation use atomic writes, but Phase 3 should check every mutable
   state transition and add regression coverage for interruption boundaries.
5. **SSH coverage is not autonomous.** The SSH fallback was hardened in PR #3
   and CI exercises the vendored `exec` transport, but PLAN.md's local-`sshd`
   integration job is still missing.

Deliberately later: opt-in encryption (Phase 4), distribution (Phase 5),
deletions (Phase 6), Windows (Phase 7), and a second provider (Phase 8).

## 7. Before opening the Phase 3 PR

- [ ] Define the stat-cache record and invalidation contract before modifying
      scan/store code; preserve the existing chunk-prefix correctness rules.
- [ ] Add `large_session` first enough to measure the promised transfer bound,
      then use it to drive append-tail hashing.
- [ ] Decide whether `casi conflicts`, GC/repack, and interruption coverage
      belong in the same Phase 3 PR or whether the phase needs a narrower
      acceptance criterion.
- [ ] Run `ctest --preset dev`, request Copilot review, and confirm PR CI is
      green before asking for an explicitly authorized squash merge.
