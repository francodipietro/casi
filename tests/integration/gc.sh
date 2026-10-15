#!/bin/sh
# `casi gc` is intentionally local-only: it may pack and prune the internal
# store, but it must leave every reachable session readable and pushable.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/a/src/p" "$work/a/.claude"
encoded=$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$encoded"
session="$work/a/.claude/projects/$encoded/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
printf '{"type":"user","cwd":"%s","message":"keep me"}\n' "$work/a/src/p" > "$session"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" push >/dev/null || fail "initial push"
"$casi" gc >/dev/null || fail "gc"
git -C "$work/a/casi/repo.git" fsck --no-dangling >/dev/null || fail "gc left an invalid store"
printf '{"type":"assistant","cwd":"%s","message":"after gc"}\n' "$work/a/src/p" >> "$session"
"$casi" push >/dev/null || fail "push after gc"
mkdir -p "$work/b/src/p" "$work/b/.claude"
encoded_b=$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/b/.claude/projects/$encoded_b"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" pull >/dev/null || fail "pull after gc"
grep -q 'after gc' "$work/b/.claude/projects/$encoded_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl" ||
    fail "append was not pushed after gc"
echo "gc: 1 passed"
