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
# spec003:R1-R4 — old archives need explicit confirmation; legacy metadata and
# symlinks remain untouched.
mkdir -p "$work/a/casi/conflicts"
printf 'old archive\n' > "$work/a/casi/conflicts/old.jsonl"
printf '{"parkedAt":1}\n' > "$work/a/casi/conflicts/old.jsonl.meta.json"
printf 'recent archive\n' > "$work/a/casi/conflicts/recent.jsonl"
printf '{"parkedAt":4102444800}\n' > "$work/a/casi/conflicts/recent.jsonl.meta.json"
printf 'legacy archive\n' > "$work/a/casi/conflicts/legacy.jsonl"
printf '{"updatedAt":1}\n' > "$work/a/casi/conflicts/legacy.jsonl.meta.json"
ln -s "$work/a/casi/conflicts/old.jsonl" "$work/a/casi/conflicts/link.jsonl"
out=$("$casi" gc) || fail "gc preview"
echo "$out" | grep -q '1 eligible' || fail "gc did not report eligible archive"
[ -f "$work/a/casi/conflicts/old.jsonl" ] || fail "preview removed archive"
[ -L "$work/a/casi/conflicts/link.jsonl" ] || fail "preview followed symlink"
out=$("$casi" gc --yes) || fail "gc confirmed cleanup"
echo "$out" | grep -q '1 removed' || fail "gc did not report removal"
[ ! -e "$work/a/casi/conflicts/old.jsonl" ] || fail "confirmed cleanup kept old archive"
[ -f "$work/a/casi/conflicts/recent.jsonl" ] || fail "cleanup removed recent archive"
[ -f "$work/a/casi/conflicts/legacy.jsonl" ] || fail "cleanup removed legacy archive"
[ -L "$work/a/casi/conflicts/link.jsonl" ] || fail "cleanup followed symlink"
"$casi" gc --yes | grep -q '0 eligible' || fail "repeat cleanup has candidates"
echo "  ok   gc: spec003 retention preview and confirmed cleanup"
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
