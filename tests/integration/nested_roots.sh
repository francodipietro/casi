#!/bin/sh
# The Phase 2 proof case: a nested root on one machine maps to a flat root on
# another. The longest local prefix must choose `bookit`, not its parent `src`.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/laptop/src/bookit/knowledge" "$work/laptop/.claude"
enc_a=$(echo "$work/laptop/src/bookit/knowledge" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/laptop/.claude/projects/$enc_a"
session="$work/laptop/.claude/projects/$enc_a/cccccccc-1111-2222-3333-444444444444.jsonl"
printf '{"type":"user","cwd":"%s","message":"nested %s/note.md"}\n' \
    "$work/laptop/src/bookit/knowledge" "$work/laptop/src/bookit/knowledge" > "$session"

export HOME="$work/laptop" CASI_HOME="$work/laptop/casi" CASI_CLAUDE_HOME="$work/laptop/.claude"
"$casi" init --remote "file://$work/remote.git" --machine laptop >/dev/null
"$casi" config root.src.path "$work/laptop/src" >/dev/null
"$casi" config root.bookit.path "$work/laptop/src/bookit" >/dev/null
"$casi" push >/dev/null || fail "push nested-root session"
git -C "$work/remote.git" show refs/heads/casi/laptop:$(git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/laptop | grep '/meta.json$' | head -1) |
    grep -q 'casi://bookit/knowledge' || fail "longest root was not selected"

mkdir -p "$work/desktop/code/knowledge" "$work/desktop/.claude"
export HOME="$work/desktop" CASI_HOME="$work/desktop/casi" CASI_CLAUDE_HOME="$work/desktop/.claude"
"$casi" init --remote "file://$work/remote.git" --machine desktop >/dev/null
"$casi" config root.bookit.path "$work/desktop/code" >/dev/null
"$casi" pull >/dev/null || fail "pull into flat layout"
enc_b=$(echo "$work/desktop/code/knowledge" | sed 's/[^a-zA-Z0-9]/-/g')
got="$work/desktop/.claude/projects/$enc_b/cccccccc-1111-2222-3333-444444444444.jsonl"
[ -f "$got" ] || fail "session was not re-encoded for the flat layout"
grep -q "$work/desktop/code/knowledge/note.md" "$got" ||
    fail "embedded path was not denormalised through bookit"
grep -q "$work/laptop/src/bookit" "$got" && fail "laptop path leaked into desktop copy"
echo "  ok   nested root maps to a flat layout through its shared name"

echo "nested_roots: 1 passed"
