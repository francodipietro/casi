#!/bin/sh
# An encrypted remote exposes neither logical tree names nor plaintext Git
# blobs. A second machine recovers its private key from the authenticated
# recovery envelope and must still materialise data byte-for-byte after roots map.
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
printf 'spec002 passphrase\nspec002 passphrase\n' | "$casi" init "file://$work/remote.git" >/dev/null
machine_a=$(git config -f "$CASI_HOME/config" --get core.machine)
[ "$(stat -c %a "$CASI_HOME/crypto.key" 2>/dev/null || stat -f %Lp "$CASI_HOME/crypto.key")" = 600 ] ||
    fail "generated key is not private"
"$casi" push >/dev/null || fail "encrypted push from A"

names=$(git -C "$work/remote.git" ls-tree -r --name-only "refs/heads/casi/$machine_a")
echo "$names" | grep -q 'private-project\|agent.jsonl\|MEMORY.md' &&
    fail "logical names leaked into encrypted tree"
git -C "$work/remote.git" cat-file --batch-all-objects --batch-check='%(objectname)' |
    while read -r oid; do git -C "$work/remote.git" cat-file -p "$oid"; done |
    grep -q 'secret transcript\|private sidecar\|private memory' &&
    fail "plaintext leaked into encrypted objects"
echo "  ok   remote tree and blobs hide private names and content"

mkdir -p "$work/b/code/private-project" "$work/b/.claude" "$work/b/casi"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
printf 'spec002 passphrase\n' | "$casi" init "file://$work/remote.git" >/dev/null ||
    fail "encrypted init with recovery on B"
# A local session registers the "private-project" basename -> B's path.
enc_b=$(echo "$work/b/code/private-project" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/b/.claude/projects/$enc_b"
printf '{"cwd":"%s","message":"B own"}\n' "$work/b/code/private-project" \
    > "$work/b/.claude/projects/$enc_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
"$casi" pull >/dev/null || fail "encrypted pull into B"
base_b="$work/b/.claude/projects/$enc_b"
[ -f "$base_b/$sid.jsonl" ] || fail "transcript was not materialised"
[ -f "$base_b/$sid/subagents/agent-one.jsonl" ] || fail "sidecar was not materialised"
[ -f "$base_b/memory/MEMORY.md" ] || fail "memory was not materialised"
grep -q "$work/b/code/private-project" "$base_b/$sid.jsonl" || fail "transcript path did not map"
grep -q "$work/b/code/private-project/docs" "$base_b/memory/MEMORY.md" || fail "memory path did not map"
echo "  ok   copied key round-trips transcript, sidecar, and memory"

echo "encryption: 2 passed"
