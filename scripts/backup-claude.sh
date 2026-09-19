#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Create a space-efficient, local snapshot of a Claude Code data directory.
#
# Each successful invocation creates a complete snapshot.  Files that did not
# change since the preceding snapshot are hard-linked, so they consume no
# additional data blocks when the destination filesystem supports hard links.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: backup-claude.sh --destination DIRECTORY [--name MACHINE]

Create an incremental snapshot of ~/.claude (or $CLAUDE_CONFIG_DIR) under:
  DIRECTORY/casi-claude-backups/MACHINE/snapshots/

The destination must not be ~/.claude or a directory inside it.  Repeated
runs create complete snapshots and hard-link files unchanged from the previous
one. No previous snapshot is deleted automatically.

Options:
  -d, --destination DIRECTORY  Backup volume or directory (required)
  -n, --name MACHINE           Name used to separate this machine's backups
  -h, --help                   Show this help
EOF
}

destination=''
machine=''

while (($#)); do
    case "$1" in
        -d|--destination)
            (($# >= 2)) || { usage >&2; exit 2; }
            destination="$2"
            shift 2
            ;;
        -n|--name)
            (($# >= 2)) || { usage >&2; exit 2; }
            machine="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'backup-claude: unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "$destination" ]] || { usage >&2; exit 2; }
command -v rsync >/dev/null 2>&1 || {
    printf 'backup-claude: rsync is required (Ubuntu: sudo apt install rsync).\n' >&2
    exit 1
}

source_dir="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
[[ -d "$source_dir" ]] || {
    printf 'backup-claude: Claude data directory does not exist: %s\n' "$source_dir" >&2
    exit 1
}

mkdir -p "$destination"
source_dir="$(cd "$source_dir" && pwd -P)"
destination="$(cd "$destination" && pwd -P)"

if [[ -z "$machine" ]]; then
    machine="$(hostname -s 2>/dev/null || hostname)"
fi
machine="$(LC_ALL=C printf '%s' "$machine" | tr -cs '[:alnum:]._-' '_')"
[[ -n "$machine" ]] || machine='unknown-machine'
if [[ "$machine" == '.' || "$machine" == '..' ]]; then
    printf 'backup-claude: machine name must not be . or ..\n' >&2
    exit 2
fi

backup_root="$destination/casi-claude-backups/$machine"
case "$backup_root/" in
    "$source_dir/"*)
        printf 'backup-claude: destination must not be inside %s\n' "$source_dir" >&2
        exit 1
        ;;
esac

snapshots="$backup_root/snapshots"
mkdir -p "$snapshots"

lock_dir="$backup_root/.backup-lock"
if ! mkdir "$lock_dir" 2>/dev/null; then
    printf 'backup-claude: another backup for %s is already running\n' "$machine" >&2
    exit 1
fi

staging=''
cleanup() {
    if [[ -n "$staging" && -d "$staging" ]]; then
        rm -rf -- "$staging"
    fi
    rmdir "$lock_dir" 2>/dev/null || true
}
trap cleanup EXIT

snapshot_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
snapshot="$snapshots/$snapshot_id"
staging="$snapshots/.in-progress-$snapshot_id"
mkdir -m 700 "$staging"

previous=''
if [[ -d "$backup_root/latest" ]]; then
    previous="$(cd "$backup_root/latest" && pwd -P)"
fi

rsync_args=(-a --human-readable --itemize-changes)
case "$(uname -s)" in
    Darwin)
        # Apple's rsync uses -E for extended attributes and resource forks.
        rsync_args+=(-E)
        ;;
    Linux)
        rsync_args+=(-A -X)
        ;;
esac
if [[ -n "$previous" ]]; then
    rsync_args+=("--link-dest=$previous")
fi

printf 'Backing up %s to %s\n' "$source_dir" "$snapshot"
printf 'For the most coherent snapshot, close Claude Code while this runs.\n'
rsync "${rsync_args[@]}" "$source_dir/" "$staging/"

# A rename within snapshots is atomic.  An interrupted run therefore never
# appears as a completed snapshot or becomes the `latest` target.
mv "$staging" "$snapshot"
staging=''
temporary_latest="$backup_root/.latest-$snapshot_id"
ln -s "snapshots/$snapshot_id" "$temporary_latest"
mv -f "$temporary_latest" "$backup_root/latest"

printf '\nBackup complete.\n'
printf 'Snapshot: %s\n' "$snapshot"
printf 'Latest:   %s/latest\n' "$backup_root"
printf 'Existing snapshots are retained; unchanged files share storage with the previous snapshot.\n'
