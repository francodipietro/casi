# Handoff: casi

Written 2026-09-04, at the end of Phase 1 implementation. Franco is moving
this side project from Claude Code to Codex for ongoing work (Claude Code
stays for his day job). This document is the full state transfer: what casi
is, what exists, what's verified, what's deliberately not done yet, and the
facts a fresh agent should not re-derive or assume — several of these were
only established by live experimentation, not by reading documentation that
doesn't exist.

> **Historical snapshot.** Phase 1 was subsequently squash-merged as PR #2
> (`a3f0e41`). Its branch state in §2 and the pre-PR checklist in §7 are no
> longer current instructions. Keep this file for the empirical evidence and
> rationale it records; use the durable docs plus the current git state when
> starting later work.

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

Those four files are the durable record. This file is a snapshot of *state*
at handoff time — once Phase 1 lands and this gets folded into the above, it
can be deleted.

## 2. Current state — read before touching git

- Repo: `github.com/francodipietro/casi` (private). Remote `origin` over SSH.
- `main`: Phase 0 only. PR #1, squashed as `976c7ed "chore: phase 0 project
  scaffolding"`. Build system, portability layer (`util/`), config on top of
  `git_config_*`, `casi --version`/`config`. CI green on push/PR (Linux
  gcc+clang, macOS clang, valgrind, a job that vendors libgit2 with the exec
  SSH transport).
- Branch `phase-1-mvp`: checked out locally, branched from and up to date
  with `main` (no divergence — `git merge-base --is-ancestor main
  phase-1-mvp` is true). Contains the **entire Phase 1 MVP**, see §3.

**The Phase 1 work is committed** as `bd23ee3 "feat: phase 1 MVP --
push/pull/sync against any git remote"` (46 files, 5,495 insertions) on
`phase-1-mvp`, on top of `main`. **It is not pushed** — `origin` has no idea
this branch exists yet. Nothing else is staged or modified; `git status`
should read clean.

Before doing anything else:

```sh
git log --oneline -3   # bd23ee3, then 976c7ed (main), then the initial commit
git status --short     # should print nothing
```

If that doesn't match, something already happened to this checkout since
this was written — stop and figure out what before writing new code on top
of an assumption that isn't true anymore.

Next step is §7: push the branch, open the PR, decide gap #1 first.

Commit message convention Franco wants going forward (given mid-project,
after the phase-0 PR title was renamed to match): [Conventional
Commits](https://gist.github.com/joshbuchea/6f47e86d2510bce28f8e7f42ae84c716)
— `type(scope): summary`, body in prose explaining *why*. No AI co-author
trailer in commits (`Co-Authored-By: ...`) — Franco rejected that explicitly
for this project. One PR per phase, squash-merged, PR title itself in
Conventional Commits form since squash makes it the commit message on `main`.

## 3. What's built (Phase 0 + Phase 1)

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

Exit codes match PLAN.md §4: `0` ok, `1` error, `2` usage, `3` conflict, `4`
network, `5` an unmapped root.

## 4. What's actually verified, and how

- `ctest --preset dev` (ASan+UBSan): **14/14 green.** Also clean under plain
  `debug` and under `static` (vendored libgit2, exec SSH transport, 1.1 MB
  binary, zero non-system dynamic deps).
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

## 6. Known gaps — deliberately deferred, not forgotten

1. **`sync.exclude` is local-only, not shared via the remote.** Before Phase 1
   began, it was explicitly decided (see conversation history / the decision
   is not yet written into PLAN.md's original text, only into this handoff
   and the supersession note at the top of PLAN.md) that the exclude list
   should live in the remote's `casi.json`, so a brand-new machine respects
   existing exclusions from its very first push — otherwise a machine that
   has never run `casi exclude bookit` will push a client's sessions before
   anyone remembers to configure it there too. **What's implemented instead**
   reads/writes `sync.exclude` purely from local
   `~/.config/casi/config` (`casi_ctx_exclude_list()` in `src/core/ctx.c`,
   backed by `casi_config_get_multivar()`). This is the most important open
   item — treat it as a correctness gap, not a nice-to-have, before pointing
   casi at any machine that also holds client work mixed with personal
   projects (Franco's actual situation: `bookit`/`nemogroup` alongside
   personal repos).
2. **`casi doctor` does not exist.** Referenced only in comments (grep for
   "doctor" in `src/` — it's all prose, no `cmd_doctor.c`). Per PLAN.md §4,
   it should diagnose unmapped roots, an unreachable remote, and repo
   corruption.
3. **No SSH remote has ever been exercised.** Every test above uses
   `file://`. `credential_cb`/`certificate_cb`/`hostkey_is_known()` in
   `src/core/repo.c` implement agent-based auth and known_hosts verification
   for the libssh2 backend (the one Homebrew actually ships — see
   `docs/DESIGN.md`), but none of it has run against a real `git@host:...`
   remote yet. This is the natural next verification step.
4. **Three tests from PLAN.md §8 don't exist yet:**
   - `large_session` — an automated synthetic-200MB push/append/push/assert-
     small-transfer test. The equivalent was done by hand against the real
     137 MB file (§4), not automated.
   - `roundtrip` — A→B→A byte-exact, as a standalone assertion.
     `two_machines.sh` covers one-way translation plus idempotence, not the
     full round trip back to the origin machine.
   - `ssh_remote` — a CI job with a local `sshd`, blocked on gap #3 above.
5. **Full named-roots UX from PLAN.md §5.1 is partly Phase-2 scope.** What
   exists (`src/path/roots.c`) is the real longest-prefix matcher, config
   loading (`root.<name>.path`), and text substitution — fully working, unit
   tested (`test_roots.c`). What's *not* built: roots being discovered from
   `casi.json` automatically (so a new machine sees what root names exist
   without being told), and the `casi doctor`-driven unmapped-root workflow.
   This was always Phase 2 per the plan; noted here only so it isn't mistaken
   for a Phase 1 miss.

## 7. Before opening the Phase 1 PR

- [ ] Decide gap #1 (§6) — at minimum, decide explicitly to leave it local-only
      for the MVP and say so in the PR description, rather than let it pass
      silently.
- [ ] `git commit` the staged work (§2) with a Conventional Commits message,
      push `phase-1-mvp`, open a PR against `main`.
- [ ] Request the same Copilot code review Phase 0 got (`gh api --method POST
      repos/francodipietro/casi/pulls/<n>/requested_reviewers -f
      "reviewers[]=copilot-pull-request-reviewer[bot]"` — it shows up as a
      check run, not in the reviewers list, that's normal).
- [ ] Confirm CI is green on the PR itself, not just locally — see §5, point 5.
