# casi design notes

Rationale for the decisions that are not obvious from the code. The user-facing
story is in the README; this is the part that matters when changing things.

## The internal repository is bare, and there is never a worktree

casi writes blobs with `git_blob_create_from_buffer`, builds trees with
`git_treebuilder`, and pushes with `git_remote_push`. It never calls
`git_checkout_*` and never touches an index. Materialising a session writes
straight into `~/.claude/projects/...` through `src/util/fs.c`.

This removes four whole categories of problem at once: `core.autocrlf` line
ending translation, the Unix file mode bit, symlinks, and libgit2's checkout
semantics. Those are also the four things that make a Windows port painful, so
the decision pays twice.

## Session files are chunked, not stored whole

Claude Code transcripts are append-only JSONL and can reach hundreds of
megabytes. Git stores blobs whole, so a 137 MB session that grows by 100 KB
would produce a second 137 MB blob on every push.

casi slices each transcript into ~1 MiB chunks, always cutting at a line
boundary, deterministically from byte 0. Because the file only ever gains bytes
at the end, every earlier chunk is byte-identical and hashes to the same object
id, so a push adds exactly one new blob.

**Normalise first, then chunk.** Path rewriting happens before slicing, so two
machines with different local layouts produce *identical* blobs for the same
logical session. Deduplication and the prefix rule below both depend on that.

## The stat cache is local and disposable

Scanning every Claude transcript on every `status` is needlessly expensive:
the scan normalises every byte before it can calculate chunk object ids. casi
therefore keeps a private binary cache at `<data dir>/index`. It never travels
with the git store: its keys contain absolute local paths and local root paths.

An entry is keyed by provider plus transcript path and records the source
`size`, nanosecond `mtime`, inode, normalized length, ordered chunk OIDs, and
the raw and normalized offsets where the final chunk begins. The file header
also carries an exact snapshot of the configured roots. A different root
table, cache format, structurally malformed cache, missing or non-blob OID,
inode change, size change, or mtime change at an equal size is a cache miss
and causes a full scan. The cache is rewritten atomically only after the file
has the same stat tuple both
before and after scanning, so a transcript changing underneath casi never
creates a trusted record for a mixed read.

For a larger file on the same inode, casi reuses all chunks before the former
final chunk and reads from that final chunk's source-line boundary through EOF.
It normalises and chunks that tail again; this is necessary because the former
tail is the one mutable chunk. Normalisation never changes newline bytes, so
counting newlines maps that new normalized tail boundary back to the correct
raw byte offset without holding a per-byte translation table.

Like Git's stat cache, the growth fast path trusts the filesystem metadata to
mean "the old bytes stayed put and new bytes were appended". A program that
rewrites an earlier prefix while also growing the same inode can defeat any
metadata-only cache; detecting that case requires rereading the prefix and
removes the performance win. Claude Code's observed transcript writer is
append-only, while ordinary rewrites with the same or smaller size continue to
fall back to a full scan and reach the normal prefix-conflict logic.

## Conflicts are decided by prefix, not by timestamp

Comparing the two chunk-id lists for a session answers everything, with no
clocks involved:

| relation | meaning | action |
|---|---|---|
| identical | up to date | nothing |
| remote is a prefix of local | local is ahead | push |
| local is a prefix of remote | remote is ahead | pull |
| neither | genuine divergence | conflict |

Only the final chunk can be partial, so the comparison is exact.

On divergence casi keeps the local file untouched and parks the remote copy
under `<data dir>/conflicts/`, reporting exit status 3. Nothing is ever
destroyed, and nothing prompts, so `casi sync` stays safe to run unattended at
login. When the user explicitly chooses `casi pull --theirs`, casi parks the
divergent local copy before replacing it with the remote one.

The design does not *assume* strict append-only. A rewritten prefix (a rewind,
a compaction) simply fails the prefix test and surfaces as a conflict.

## Path rewriting is byte substitution, never parse-and-reserialise

Round-tripping a record through a JSON parser risks reordering keys, reshaping
numbers or changing escapes — any of which could break resuming the session.
casi substitutes byte ranges in the raw line and then re-validates that the line
is still well-formed JSON. On Unix, paths contain no characters JSON escapes, so
literal substitution is safe. On Windows they do (`"C:\\Users\\..."`), which is
why the substituter is written to take the escaped variant as well.

## Shared configuration has one authoritative ref

Machine branches are intentionally independently owned: that is what keeps a
session push free of non-fast-forward races. Configuration is different. An
exclude must already exist before a newly configured machine makes its first
push, and an include must revoke that exclude everywhere. Neither operation is
correct if every machine keeps an independent copy.

Phase 2 therefore stores the canonical `casi.json` in `refs/heads/casi/config`.
It contains the shared root-name namespace and `sync.exclude`; local config
still maps those names to machine-specific paths. A writer fetches that ref,
parents a config commit on it, and pushes normally. A non-fast-forward means
another machine won the race, so casi refetches, replays the small set-like
mutation, and retries. It never force-pushes configuration and never treats
that race as a transcript conflict.

The first Phase 2 push migrates a legacy local `sync.exclude` list before it
scans or uploads sessions. This preserves the safety boundary during upgrade:
an excluded client project cannot leak in the gap between installing the new
binary and teaching a second machine about the policy.

## Sidecars and memory are blobs, not transcript chunks

Claude Code's `subagents/` files and project `memory/` are ordinary small text
files. casi normalises their embedded paths like a transcript, but stores each
as one blob: they have no append-only contract, so applying the transcript
prefix rule would be fictional. Their identity is their project, optional
session, and basename.

An equal blob is already current. A missing one materialises atomically. If a
local auxiliary file and the remote one differ, casi leaves the local file
untouched, parks the translated remote copy under `conflicts/`, reports exit 3,
and never selects either version by timestamp or iteration order. Two distinct
remote versions likewise report a conflict rather than silently choosing one.

## The project directory name is never decoded

Claude Code derives `~/.claude/projects/<name>` from the startup working
directory by replacing every non-alphanumeric character with `-`. That is lossy
and not injective:

| cwd | directory |
|---|---|
| `/Users/f/src/etl_fsearch` | `-Users-f-src-etl-fsearch` |
| `/Users/f/src/bookit/bookit-knowledge` | `-Users-f-src-bookit-bookit-knowledge` |
| `/Users/f/src/bookit-knowledge` | `-Users-f-src-bookit-knowledge` |

So casi reads the real path from the `cwd` field of the transcript, stores it
explicitly, and re-encodes it for the target machine on the way out. Inverting
the directory name would be guessing.

Note also that the directory reflects the *startup* cwd while each record's
`cwd` field tracks the current one, which moves as the session runs. The
project's canonical path is the first `cwd` seen.

## Which SSH backend libgit2 was built with matters

libgit2 can be built with one of two SSH transports, chosen at compile time:

- **`exec`** shells out to the system `ssh`. `~/.ssh/config` host aliases,
  `known_hosts`, `ProxyCommand` and agent forwarding all work exactly as the
  user already has them configured.
- **`libssh2`** speaks the protocol itself. It reads none of that: an alias-only
  host does not resolve, and host-key verification and credential selection have
  to be reimplemented by the caller.

casi therefore prefers `exec`, and release binaries vendor libgit2 built that
way (`--preset static`). Distribution packages that link a system libgit2 may
well get `libssh2` instead — Homebrew's build does — so `casi --version` and
`casi doctor` report the backend in play. For a plain hostname, the libssh2
callbacks verify `known_hosts` and try the conventional local key files
(`id_ed25519`, `id_ecdsa`, `id_rsa`) before the SSH agent; they never prompt
for or store a passphrase.

### Version floor

The floor is libgit2 **1.7** — what the code actually needs, and what
Debian/Ubuntu ship (24.04 has 1.7.2).

The security caveat is narrower than a blanket floor would imply. The `exec`
transport was introduced in 1.8.0 and its arbitrary command execution bug was
fixed in 1.9.2, so exactly `[1.8.0, 1.9.2)` is dangerous — anything older has
no `exec` transport at all and cannot be affected. The build rejects precisely
that window, with `-DCASI_ALLOW_UNSAFE_LIBGIT2=ON` as the documented escape for
a packager who knows their build links libssh2.

`git_libgit2_feature_backend()` is a 1.9 API, so it sits behind a
`LIBGIT2_VERSION_*` guard. Below 1.9 there was only one backend, so casi reports
`libssh2` without asking.

## Windows is not supported, but is not designed out

Decisions taken now that keep the port cheap:

| decision | why it helps |
|---|---|
| bare repo, no checkout | no autocrlf, mode bits, symlinks or checkout semantics |
| all I/O explicitly binary | no CRT text-mode newline translation |
| canonical paths always use `/` | one canonical form on every platform |
| project ids are SHA-256 hex | no illegal characters, no `CON`/`NUL`, no 260-char limit |
| every syscall behind `util/fs.c` | the port touches one file |
| `casi_fs_home()` rather than `getenv("HOME")` | `%USERPROFILE%` fallback is a one-function change |
| atomic replace wrapped from day one | `rename()` vs `MoveFileExW` never reaches a call site |
| UTF-8 throughout the core | UTF-16 conversion stays at the syscall edge |

What will still cost real work: case-insensitive path comparison on Windows and
macOS versus case-sensitive on Linux; the unknown shape of Claude Code's
directory encoding on Windows; backslash escaping in JSON; and long-path
prefixes.
