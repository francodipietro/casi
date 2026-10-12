#!/bin/sh
# A later unknown root must abort before an earlier conflict is parked.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/a/src/p" "$work/a/other" "$work/b/src/p"
enc_a=$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
enc_b=$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$enc_a" "$work/b/.claude/projects/$enc_b"
sa="$work/a/.claude/projects/$enc_a/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
sb="$work/b/.claude/projects/$enc_b/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
seed="$work/b/.claude/projects/$enc_b/cccccccc-0000-0000-0000-000000000000.jsonl"

printf '{"cwd":"%s","message":"base"}\n' "$work/a/src/p" > "$sa"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine a >/dev/null
"$casi" push >/dev/null

printf '{"cwd":"%s","message":"B own"}\n' "$work/b/src/p" > "$seed"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine b >/dev/null
"$casi" pull >/dev/null

printf '{"cwd":"%s","message":"A edit"}\n' "$work/a/src/p" >> "$sa"
printf '{"cwd":"%s","message":"B edit"}\n' "$work/b/src/p" >> "$sb"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" config root.other.path "$work/a/other"
printf '{"cwd":"%s","message":"read %s/file"}\n' "$work/a/src/p" "$work/a/other" > "$work/a/.claude/projects/$enc_a/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
set +e
out=$("$casi" pull 2>&1)
rc=$?
set -e
[ "$rc" -eq 5 ] || fail "missing root: exit $rc, expected 5: $out"
[ ! -e "$work/b/casi/conflicts" ] ||
    fail "a conflict was parked before discovering the missing root"
[ ! -f "$work/b/.claude/projects/$enc_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl" ] ||
    fail "a session was materialized before discovering the missing root"
grep -q 'B edit' "$sb" || fail "local session changed"

echo "preflight: 1 passed"
