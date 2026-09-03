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
login.

The design does not *assume* strict append-only. A rewritten prefix (a rewind,
a compaction) simply fails the prefix test and surfaces as a conflict.

## Path rewriting is byte substitution, never parse-and-reserialise

Round-tripping a record through a JSON parser risks reordering keys, reshaping
numbers or changing escapes — any of which could break resuming the session.
casi substitutes byte ranges in the raw line and then re-validates that the line
is still well-formed JSON. On Unix, paths contain no characters JSON escapes, so
literal substitution is safe. On Windows they do (`"C:\\Users\\..."`), which is
why the substituter is written to take the escaped variant as well.

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
`casi doctor` report the backend in play, and the SSH credential and host-key
callbacks exist to make the `libssh2` case work for plain hostnames.

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
