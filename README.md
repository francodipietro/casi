# casi

**C**oding **A**ssistant **S**ession **I**nterchange — carry your coding-assistant
sessions between machines.

Start a Claude Code session on your laptop, `casi push`, then `casi pull` on your
desktop and pick up exactly where you left off.

> Status: early development. Phase 4 is complete — see [Roadmap](#roadmap).

## What it does

casi is to your assistant sessions what git is to your code, minus everything you
do not want to think about. There are no branches, no commits, no merges, no
rebases. There are five commands:

```
casi init <url>            # connect and securely initialize or join a remote
casi push                  # send this machine's sessions
casi pull                  # bring in the other machines'
casi sync                  # pull, resolve safely, then push
casi status                # what is out of date, and where
```

Under the hood it is a git repository, so you get content integrity,
deduplication, compression and incremental transfer for free. You never see any
of that. The versioned remote-store contract is documented in
[docs/FORMAT.md](docs/FORMAT.md).

## Releases

Release assets are produced from version tags, with SHA-256 checksums and
GitHub build provenance. The artifact layout, verification command, and the
owner-controlled publication procedure are in [docs/RELEASING.md](docs/RELEASING.md).

## Backup before syncing

Before using casi with an existing Claude Code history, make a local backup on
each machine. The included script works on macOS and Ubuntu and uses `rsync` to
make complete, incremental snapshots: unchanged files are hard-linked to the
previous snapshot (when the backup filesystem supports them), so a second run
does not duplicate their contents. It never deletes an earlier snapshot
automatically.

Close Claude Code first for the most coherent result, then choose a backup
volume or directory which is **not** `~/.claude` or inside it:

```sh
scripts/backup-claude.sh --destination /Volumes/Claude-backups   # macOS
scripts/backup-claude.sh --destination /media/$USER/Claude-backups # Ubuntu
```

Ubuntu may need `sudo apt install rsync`. Snapshots live below
`<destination>/casi-claude-backups/<machine>/snapshots/`; `latest` points at
the most recent one. Re-run the exact command before a later casi session to
renew the backup. The backup destination should be on a separate volume (and,
ideally, backed up or encrypted itself): a local snapshot does not protect
against loss of the laptop or disk.

To restore, stop Claude Code and casi, first move the live directory somewhere
safe, then copy one snapshot back. Start with the dry run and omit it only
after checking the proposed changes:

```sh
mv ~/.claude ~/.claude.before-restore
mkdir ~/.claude
# macOS
rsync -aE --dry-run /path/to/snapshots/<timestamp>/ ~/.claude/
rsync -aE /path/to/snapshots/<timestamp>/ ~/.claude/

# Ubuntu
rsync -aAX --dry-run /path/to/snapshots/<timestamp>/ ~/.claude/
rsync -aAX /path/to/snapshots/<timestamp>/ ~/.claude/
```

The moved `~/.claude.before-restore` directory lets you undo the restore
without relying on the backup.

## Finding projects on another machine

casi identifies a project from the first `cwd` in its transcript and uses the
final directory name as its portable name. When the other machine has already
used that project in Claude Code, its local transcript supplies the destination
path automatically. If it has a checkout but no Claude Code history, casi
searches common code locations under that machine's home directory for a
unique matching project directory. Neither case needs a configured path, alias, matching username, or
matching parent-directory layout.

If there is no matching directory, or there are two with the same name, casi
does not guess and does not write the incoming session. The remote copy remains
available; moving into or creating the intended checkout and retrying is safe.
Manual `root.<name>.path` settings remain an escape hatch for genuinely
ambiguous names, not part of normal setup.

Older remote branches may contain paths normalized by a previous casi version.
`casi` migrates known historical layouts while materialising them, without
adding aliases or storing a machine-specific path in shared state.

## Automatic conflict handling

The normal `casi sync` path converges automatically. Equivalent copies and
append-only continuations are merged as usual. For a genuine session fork,
casi compares the last internal JSONL event timestamp and keeps the newest
activity; the other copy is archived under Casi's data directory. Subagent
files follow their parent session's choice, so one conversation never receives
a mixture of both branches. Files without a parent session or event timestamp,
such as project memory, use a deterministic object-id tie-breaker so every
machine makes the same choice. Nothing is deleted, and a repeat sync reuses the
same archived copy rather than adding numbered duplicates.

Use `casi conflicts` to inspect archived copies. `casi pull --strict` or
`casi sync --strict` disables automatic selection and leaves a genuine fork
pending, which is useful when deliberately comparing two conversations.

`casi pull --dry-run` is a read-only preview: it does not fetch, write a
session, archive a copy, update Casi's object store, or refresh its cache. It
reports the choices it would make from the last fetched remote view.

## Why not just rsync / Dropbox / a git repo of ~/.claude

- **Sessions embed absolute paths.** `/home/franco/dev/app` on Linux is
  `/Users/franco/dev/app` on a Mac, and the directory names Claude Code derives
  from them are a lossy, non-invertible encoding. casi maps them across machines
  through a table of named roots, so a synced session actually opens on the
  other side.
- **Session files are append-only and can be enormous** — a long-running one
  reaches hundreds of megabytes. casi slices them at line boundaries so a push
  after an hour of work transfers the tail, not the file.
- **The same session can grow on two machines.** casi keeps the newest recorded
  activity by default and archives the other complete copy; strict mode leaves
  it pending instead. Neither copy is destroyed.
- **No always-on network required.** Any git remote works as the hub: GitHub,
  GitLab, Bitbucket, self-hosted Gitea/Forgejo, a bare repo over plain SSH, or a
  path on a USB stick. The machines never have to be online at the same time.

## Remote-agnostic, by design

casi speaks git, not a vendor API. Anything git can push to, casi can push to:

```
casi init git@github.com:you/my-sessions.git
casi init https://gitea.example.org/you/my-sessions.git
casi init ssh://you@nas.local/srv/git/my-sessions.git
casi init file:///Volumes/usb/my-sessions.git
```

Your sessions are yours. casi has no server, no account and no telemetry.

## Building from source

Requires a C11 compiler, CMake ≥ 3.20, pkg-config, libgit2 ≥ 1.7 and
libsodium ≥ 1.0.12.

```sh
brew install cmake pkg-config libgit2 libsodium             # macOS
sudo apt install cmake pkg-config libgit2-dev libsodium-dev # Debian/Ubuntu

cmake --preset release
cmake --build --preset release
sudo cmake --install build/release
```

For development, `--preset dev` adds ASan/UBSan and `-Werror`:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

`--preset static` vendors libgit2, selects the static libsodium archive, and
builds libgit2 with the OpenSSH `exec` transport. The release workflow adds
full static linking for Linux and produces macOS binaries with no Homebrew
dependencies. Reach for this preset if your distro's libgit2 links libssh2 and
you need `~/.ssh/config` host aliases to resolve — `casi --version` tells you
which backend you have.

That preset produces an artifact to copy, not one to `cmake --install`: libgit2's
own install rules come along with it and would drop its headers and static
library into your prefix. Take `build/static/casi` directly, or install from
`--preset release` against a system libgit2.

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 0 | Scaffolding: build, CI, util layer, `casi --version` | done |
| 1 | MVP: `init`/`status`/`push`/`pull`/`sync` against any git remote | done |
| 2 | Shared named roots, full path normalization, `doctor`, memory and subagents | done |
| 3 | Stat-cache, large-session coverage, `casi conflicts`, GC/repack and interruption audit | done |
| 4 | Opt-in encryption (libsodium, convergent per chunk) | done |
| 5 | Prebuilt binaries, Homebrew tap, packages | |
| 6 | Deletions and `casi forget` | |
| 7 | Windows support | |
| 8 | A second assistant (Cursor, Codex CLI) | |

Claude Code is the first supported assistant. The provider layer is a vtable, so
others slot in without touching the sync core.

Linux and macOS are the supported platforms. Windows is not supported yet, but
the design avoids the decisions that would make porting painful — see
`docs/DESIGN.md`.

## License

GNU Affero General Public License v3.0 only. See [LICENSE](LICENSE).
