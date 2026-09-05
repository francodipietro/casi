#!/bin/sh
# Sidecars and project memory are separate blobs, but must follow transcripts
# across different root layouts and receive the same path translation.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

sid=aaaaaaaa-1111-2222-3333-444444444444

# A creates a transcript, its two normal subagent files, and project memory.
mkdir -p "$work/a/src/proj" "$work/a/.claude"
enc_a=$(echo "$work/a/src/proj" | sed 's/[^a-zA-Z0-9]/-/g')
base_a="$work/a/.claude/projects/$enc_a"
mkdir -p "$base_a/$sid/subagents" "$base_a/memory"
printf '{"type":"user","cwd":"%s","message":"parent"}\n' "$work/a/src/proj" \
    > "$base_a/$sid.jsonl"
printf '{"cwd":"%s","message":"subagent"}\n' "$work/a/src/proj" \
    > "$base_a/$sid/subagents/agent-one.jsonl"
printf '{"description":"works in %s"}\n' "$work/a/src/proj" \
    > "$base_a/$sid/subagents/agent-one.meta.json"
printf '# Notes\nPath: %s/docs\n' "$work/a/src/proj" > "$base_a/memory/MEMORY.md"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "push from A"
git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/machine-a |
    grep -q "subagents/agent-one.jsonl" || fail "subagent missing from remote tree"
git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/machine-a |
    grep -q "memory/MEMORY.md" || fail "memory missing from remote tree"

# B receives all three kinds under its own encoded project directory.
mkdir -p "$work/b/code/proj" "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/b/code" >/dev/null
"$casi" doctor >/dev/null || fail "fetch remote state for B status"
out=$("$casi" status --porcelain)
echo "$out" | awk -F '\t' '$1 == "aux-pull" && $2 == 3 && $3 > 0 { found = 1 } END { exit !found }' ||
    fail "status does not report remote auxiliary bytes"
"$casi" pull >/dev/null || fail "pull into B"
enc_b=$(echo "$work/b/code/proj" | sed 's/[^a-zA-Z0-9]/-/g')
base_b="$work/b/.claude/projects/$enc_b"
[ -f "$base_b/$sid/subagents/agent-one.jsonl" ] || fail "subagent jsonl not materialised"
[ -f "$base_b/$sid/subagents/agent-one.meta.json" ] || fail "subagent metadata not materialised"
[ -f "$base_b/memory/MEMORY.md" ] || fail "memory not materialised"
grep -q "$work/b/code/proj" "$base_b/$sid/subagents/agent-one.jsonl" ||
    fail "subagent path was not translated"
grep -q "$work/b/code/proj/docs" "$base_b/memory/MEMORY.md" ||
    fail "memory path was not translated"
grep -q "$work/a/src/proj" "$base_b/memory/MEMORY.md" &&
    fail "A path leaked into B memory"
echo "  ok   pull translates sidecars and project memory"

# A later pull receives auxiliary files B added after receiving the session.
printf '{"cwd":"%s","message":"second subagent"}\n' "$work/b/code/proj" \
    > "$base_b/$sid/subagents/agent-two.jsonl"
printf 'Second note: %s/readme\n' "$work/b/code/proj" > "$base_b/memory/second.md"
out=$("$casi" status --porcelain)
echo "$out" | awk -F '\t' '$1 == "aux-push" && $2 == 2 { found = 1 } END { exit !found }' ||
    fail "status does not report pending auxiliary files"
"$casi" push >/dev/null || fail "push additions from B"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" pull >/dev/null || fail "pull additions into A"
[ -f "$base_a/$sid/subagents/agent-two.jsonl" ] || fail "new subagent not materialised"
[ -f "$base_a/memory/second.md" ] || fail "new memory file not materialised"
grep -q "$work/a/src/proj/readme" "$base_a/memory/second.md" ||
    fail "B memory path was not translated back to A"
echo "  ok   later auxiliary additions round-trip"

# Auxiliary files do not have the transcript append rule. A conflicting local
# edit must therefore remain in place, never be silently overwritten.
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
printf 'Conflicting B note: %s\n' "$work/b/code/proj" > "$base_b/memory/MEMORY.md"
"$casi" push >/dev/null || fail "push conflicting memory from B"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
set +e
out=$("$casi" pull 2>&1)
rc=$?
set -e
[ "$rc" = 3 ] || fail "auxiliary conflict: exit $rc, want 3"
echo "$out" | grep -q 'auxiliary file differs and was left untouched' ||
    fail "auxiliary conflict: missing warning"
grep -q '# Notes' "$base_a/memory/MEMORY.md" ||
    fail "auxiliary conflict: local memory was overwritten"
find "$work/a/casi/conflicts" -type f | grep -q 'aux-.*MEMORY.md-remote' ||
    fail "auxiliary conflict: remote copy was not parked"
echo "  ok   conflicting auxiliary edit is left untouched"

echo "auxiliary_files: 3 passed"
