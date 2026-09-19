# Handoff: casi

Originally written 2026-09-04 at the end of Phase 1, when Franco moved this
side project from Claude Code to Codex for ongoing work (Claude Code stays for
his day job). It preserves the empirical findings that a later agent must not
re-derive or assume — several came from live experimentation rather than
documentation.

> **Phase 4 closeout (2026-09-18).** Phase 1 was squash-merged as PR #2
> (`a3f0e41`), its follow-up as PR #3 (`d333228`), CI scoping as PR #4
> (`d869551`), Phase 2 as PR #5 (`c9a19b5`), and Phase 3 as PR #7
> (`6554208`), and Phase 4 as PR #9 (`ad7ba54`). The Phase 1 snapshot and
> pre-Phase-3 checklist are
> historical. Sections 2, 4, 6 and 7 give the current baseline; retain the
> rest for its empirical evidence and rationale.

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
handoff evidence plus the Phase 4 closeout baseline; use it alongside the
current git state rather than as a replacement for the durable docs.

## 2. Current state — Phase 5 baseline

- Repo: `github.com/francodipietro/casi` (public). Remote `origin` uses SSH.
- `main` includes Phase 0, Phase 1 and its SSH/roundtrip follow-up, scoped CI,
  Phases 2 through 4, the Phase 5 distribution merge (PR #11), and the first
  release-workflow repair (PR #12).
- Phase 2 made `refs/heads/casi/config` the authoritative shared
  configuration. It publishes named roots and `sync.exclude`, migrates legacy
  local exclusions on first publish, diagnoses shared root mappings with
  `casi doctor`, and syncs Claude Code subagents and project memory.
- Phase 3 added the local disposable stat cache, append-tail rescanning, the
  200 MiB transfer-bound integration test, hidden `casi conflicts` and
  `casi gc` plumbing commands, and an interruption audit with regressions.
  The cache is `CASIIDX3`: it is atomically replaced, tied to the exact roots
  table and source stat tuple, verifies cached OIDs are blobs, and has a
  checksum over its private serialized form. See `docs/DESIGN.md` rather than
  duplicating its validity contract.
- Phase 4 added opt-in encrypted stores. `casi init --encrypt` uses a private
  local `0600` keyfile; the key is never written to Git and a pre-existing
  remote must be joined with an explicitly copied matching key. Remote content
  is deterministically encrypted and remote path components are HMACed, while
  the shared-config header stays clear only to advertise encryption mode and a
  non-secret key identifier. `docs/DESIGN.md` records the security and trust
  boundaries, including authenticated session manifests and auxiliary scopes.
- PR #9 received independent local-agent reviews. The initial reviews found
  missing authenticated bindings for session chunks and auxiliary scopes; both
  were fixed and the final review was clean. No GitHub reviewer was requested.
  CI was green on the full Linux/macOS matrix, valgrind,
  vendored-libgit2/exec-SSH, GitGuardian, and the final `CI` gate.

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

## 3. What's built (through Phase 4)

Phase 2 added the shared-configuration module, `casi doctor`, shared exclusion
mutation, and provider/store support for auxiliary assets. The relevant test
coverage is `test_shared_config` plus `shared_config`, `doctor`,
`auxiliary_files`, and `nested_roots` integration tests. Its detailed design
is deliberately recorded in `docs/DESIGN.md`; do not recreate a second source
of truth here.

Phase 3 added `core/index.c` and `include/casi/index.h` for the local stat
cache; `store.c` reuses unchanged OIDs and, on an append, only rereads the
former final chunk plus the new tail. The hidden `casi conflicts` command lists
parked copies without changing them. `casi gc` repacks/prunes only the local
bare store through fixed-argv `git gc --prune=now`; ordinary sync remains
libgit2-only. `large_session`, `gc`, and `interruption` are integration tests;
`test_index` covers cache structure, invalidation, and checksum rejection.

Phase 4 adds `core/crypto.c` and `include/casi/crypto.h`, with separated
content, nonce, and path keys derived from the local master key. The repository
layer transparently encrypts ordinary blobs; shared configuration retains a
small clear header and encrypts its payload. Encrypted store paths use HMAC
components, and session metadata authenticates the chunk tree it references.
`test_crypto` and `encryption` cover the primitives and two-machine workflow.

The inventory below is the original Phase 1 snapshot. Read it as background
for the stable MVP machinery, not as a complete current file tree.

```
src/
  main.c                 dispatch; global -v/-q/--no-color parsed before the verb
  cmd/                   one file per verb: init, push, pull, sync, status,
                         exclude (also serves `include`), config, version, help,
                         plus hidden doctor, conflicts, gc
  cmd/sync_ops.{c,h}     shared push/pull machinery (fetch, build tree, walk
                         entries, apply the prefix rule) both commands call
  core/
    ctx.{c → include/casi/ctx.h}   everything a command needs, opened once:
                         config, roots, repo, machine name, exclude list
    chunk.c              the ~1 MiB line-boundary splitter
    repo.c               bare repo, blobs/trees/commits, fetch/push, SSH
                         credential + host-key callbacks (see §5.3)
    index.c              private local stat cache for transcript scans
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
  unit/                  test_{buf,str,fs,paths,config,jsonl,encoding,roots,
                         chunk,shared_config,index,crypto} (Phases 0–4)
  integration/           two_machines.sh, conflict.sh, shared_config.sh,
                         doctor.sh, auxiliary_files.sh, nested_roots.sh,
                         encryption.sh, large_session.sh, gc.sh, interruption.sh
                         — real casi binary, real
                         (temporary) git repos, `file://` remote, no network
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

The hidden plumbing commands are `casi doctor` (active SSH backend, remote
reachability, shared-root mappings), `casi conflicts` (parked copies), and
`casi gc` (local store maintenance).

Exit codes match PLAN.md §4: `0` ok, `1` error, `2` usage, `3` conflict, `4`
network, `5` an unmapped root.

## 4. What's actually verified, and how

- The Phase 4 suite is **26/26 green**, split to avoid rerunning the expensive
  `large_session` test: the other 25 passed under the development preset, and
  `ctest --test-dir build/dev -R 'large_session$' --output-on-failure` passed
  independently in 76.34 seconds. `large_session` generates an approximately
  200 MiB JSONL transcript, corrupts the cached raw-tail offset, appends a
  short record batch, requires less than 1 MiB of new remote pack data, and
  verifies a second machine restores the exact bytes. The observed transfer
  was 168 KiB.
- CI confirmed Phase 4 on Linux/macOS debug and sanitizer jobs, valgrind,
  vendored libgit2 with the exec SSH transport, GitGuardian, and the required
  `CI` gate.
- The encrypted two-machine integration test verifies a `0600` keyfile, that
  a remote exposes neither readable content nor project names, rejection of a
  join without a copied key (without leaving a stray default key), and that a
  copied key materializes the transcript, sidecar, and memory under the second
  machine's layout. It also checks path rewriting in the transcript and memory.
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
- `gc.sh` proves that `casi gc` leaves the store valid (`git fsck
  --no-dangling`) and that a subsequent append reaches the remote. The
  interruption regression keeps malformed `.casi-tmp-*` siblings on both
  source and recipient machines and proves they never get materialized or
  uploaded.

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
6. **A local stat cache can corrupt a future append.** A cache record that
   claimed zero chunks for a non-empty session, named a missing/non-blob OID,
   or held an altered tail offset could make the scanner reuse an invalid
   prefix. The fix is layered: reject structurally impossible entries, verify
   every cached OID is a blob and that its sizes add up, and checksum the
   complete private `CASIIDX3` serialization. The 200 MiB integration test
   modifies the *actual* `tail_raw_offset` field before an append; an earlier
   version of that test was nine bytes off and merely invalidated inode/mtime,
   a false positive caught in independent review. The final test proves the
   altered cache is a miss and the restored transcript is byte-exact. This is
   still a Git-index-style local performance cache, not a security boundary
   against someone who can rewrite both the cache and its checksum.
7. **A successful GC is not enough if the next push breaks.** The first GC
   regression only ran `git fsck`, which says the local object database is
   valid but not that casi can continue writing after repack/prune. `gc.sh`
   now appends a record, pushes it, and reads that record from the remote.
   `casi_fs_git_gc()` also retries `waitpid` when interrupted by `EINTR`.

## 6. Known gaps — Phase 5 baseline

1. **SSH coverage is not autonomous.** The SSH fallback was hardened in PR #3
   and CI exercises the vendored `exec` transport, but PLAN.md's local-`sshd`
   integration job is still missing.
2. **The real-dataset latency target remains unrecorded.** The cache and the
   synthetic 200 MiB transfer bound are automated, but the original plan's
   explicit `casi status` sub-second measurement on the real 390 MB dataset
   has not yet been repeated and captured as evidence.
3. **The first public release is still unexecuted.** The Phase 5 machinery
   builds self-contained prebuilt binaries and a Debian package, but it cannot
   publish them until the repository is public and a version-matching tag is
   pushed. Only then will immutable archive checksums exist for the owner to
   pin the external Homebrew tap and AUR package; see `docs/RELEASING.md`.
   The first two `v0.1.0` attempts published neither a GitHub Release nor
   attestations: first Alpine could not run `apk` as the runner UID, then
   libgit2 could not find its static OpenSSL backend. The release job now
   installs Alpine's separate `openssl-libs-static`, `zlib-static`, and
   `libsodium-static` packages, and ordinary CI exercises that same musl build
   before a tag can be the first execution. The third attempt built every
   artifact and created attestations, but `gh release create` failed because
   the publish job had no checkout. That attestation makes `v0.1.0` immutable;
   the repair must ship as `v0.1.1` rather than retargeting it.

Deliberately later: deletions (Phase 6), Windows (Phase 7), and a second
provider (Phase 8).

## 7. Phase 4 closeout evidence

- [x] Define local-key lifecycle and reject unsafe migration of an existing
      remote without a copied key.
- [x] Encrypt content deterministically, conceal remote path names, and retain
      a minimal clear configuration header for safe joins.
- [x] Authenticate the session chunk manifest and every auxiliary asset's
      scope; keep cache domains separate by key identity.
- [x] Run the 26-test local suite including `large_session`, resolve the
      independent local review findings, verify the full CI matrix and final
      `CI` gate, then squash-merge PR #9 with Franco's authorization.
