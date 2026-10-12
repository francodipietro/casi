#!/bin/sh
# A second snapshot must update latest, not move its temporary symlink into
# the directory named by the old latest link.
set -eu
script="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work/claude/projects"
printf 'first\n' > "$work/claude/projects/session.jsonl"
CLAUDE_CONFIG_DIR="$work/claude" bash "$script" \
    --destination "$work/backups" --name test >/dev/null
root="$work/backups/casi-claude-backups/test"
first=$(readlink "$root/latest")
[ -f "$root/$first/projects/session.jsonl" ] || exit 1

printf 'second\n' >> "$work/claude/projects/session.jsonl"
CLAUDE_CONFIG_DIR="$work/claude" bash "$script" \
    --destination "$work/backups" --name test >/dev/null
second=$(readlink "$root/latest")
[ "$first" != "$second" ] || exit 1
printf 'first\n' | cmp - "$root/$first/projects/session.jsonl"
cmp "$work/claude/projects/session.jsonl" "$root/$second/projects/session.jsonl"

echo 'backup_script: 1 passed'
