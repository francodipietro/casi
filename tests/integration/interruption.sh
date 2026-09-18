#!/bin/sh
# Atomic materialisation can leave a sibling .casi-tmp-* file if the process
# is interrupted before rename. Those bytes are deliberately not transcripts:
# a later scan must ignore them rather than publishing a half-written session.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

project="$work/a/src/p"
mkdir -p "$project" "$work/a/.claude"
encoded=$(echo "$project" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$encoded"
session="$work/a/.claude/projects/$encoded/cccccccc-0000-0000-0000-000000000000.jsonl"
printf '{"type":"user","cwd":"%s","message":"complete record"}\n' "$project" > "$session"
printf 'not a complete JSONL transcript\n' > "$session.casi-tmp-interrupted"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "push with interrupted temporary sibling"

mkdir -p "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" pull >/dev/null || fail "pull"
restored=$(find "$work/b/.claude/projects" -name '*.jsonl')
[ -n "$restored" ] || fail "no transcript materialised"
grep -q 'complete record' "$restored" || fail "complete transcript missing"
grep -q 'not a complete' "$restored" && fail "interrupted temporary file was synced"

# A stale target-side temporary is also ignored when the recipient later
# scans and pushes its own branch.
printf 'still not a transcript\n' > "$restored.casi-tmp-interrupted"
"$casi" push >/dev/null || fail "push with local interrupted temporary sibling"
git -C "$work/remote.git" grep -q 'still not a transcript' refs/heads/casi/machine-b &&
    fail "target-side interrupted temporary file reached the remote"
echo "interruption: 1 passed"
