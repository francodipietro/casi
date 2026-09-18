#!/bin/sh
# An encrypted remote exposes neither logical tree names nor plaintext Git
# blobs. A second machine, given the same explicitly copied private key, must
# still recover transcripts and auxiliary files byte-for-byte after roots map.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }
sid=aaaaaaaa-1111-2222-3333-444444444444

mkdir -p "$work/a/src/private-project" "$work/a/.claude"
enc_a=$(echo "$work/a/src/private-project" | sed 's/[^a-zA-Z0-9]/-/g')
base_a="$work/a/.claude/projects/$enc_a"
mkdir -p "$base_a/$sid/subagents" "$base_a/memory"
printf '{"cwd":"%s","message":"secret transcript"}\n' "$work/a/src/private-project" > "$base_a/$sid.jsonl"
printf '{"cwd":"%s","message":"private sidecar"}\n' "$work/a/src/private-project" > "$base_a/$sid/subagents/agent-one.jsonl"
printf 'private memory: %s/docs\n' "$work/a/src/private-project" > "$base_a/memory/MEMORY.md"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a --encrypt >/dev/null
[ "$(stat -c %a "$CASI_HOME/crypto.key" 2>/dev/null || stat -f %Lp "$CASI_HOME/crypto.key")" = 600 ] ||
    fail "generated key is not private"
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "encrypted push from A"

names=$(git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/machine-a)
echo "$names" | grep -q 'private-project\|agent.jsonl\|MEMORY.md' &&
    fail "logical names leaked into encrypted tree"
git -C "$work/remote.git" cat-file --batch-all-objects --batch-check='%(objectname)' |
    while read -r oid; do git -C "$work/remote.git" cat-file -p "$oid"; done |
    grep -q 'secret transcript\|private sidecar\|private memory' &&
    fail "plaintext leaked into encrypted objects"
echo "  ok   remote tree and blobs hide private names and content"

mkdir -p "$work/b/code/private-project" "$work/b/.claude" "$work/b/casi"
cp "$work/a/casi/crypto.key" "$work/b/casi/crypto.key"
chmod 600 "$work/b/casi/crypto.key"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b --encrypt >/dev/null ||
    fail "encrypted init with copied key on B"
"$casi" config root.src.path "$work/b/code" >/dev/null
"$casi" pull >/dev/null || fail "encrypted pull into B"
enc_b=$(echo "$work/b/code/private-project" | sed 's/[^a-zA-Z0-9]/-/g')
base_b="$work/b/.claude/projects/$enc_b"
[ -f "$base_b/$sid.jsonl" ] || fail "transcript was not materialised"
[ -f "$base_b/$sid/subagents/agent-one.jsonl" ] || fail "sidecar was not materialised"
[ -f "$base_b/memory/MEMORY.md" ] || fail "memory was not materialised"
grep -q "$work/b/code/private-project" "$base_b/$sid.jsonl" || fail "transcript path did not map"
grep -q "$work/b/code/private-project/docs" "$base_b/memory/MEMORY.md" || fail "memory path did not map"
echo "  ok   copied key round-trips transcript, sidecar, and memory"

echo "encryption: 2 passed"
