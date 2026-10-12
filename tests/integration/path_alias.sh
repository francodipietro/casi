#!/bin/sh
# Two operating-system layouts can describe one history even when old paths
# are still embedded in both local transcripts.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"
fail() { echo "FAIL: $*" >&2; exit 1; }

ap="$work/linux/alice/src/work/project"
bp="$work/mac/bob/code/project"
mkdir -p "$ap" "$bp"
ae=$(echo "$ap" | sed 's/[^a-zA-Z0-9]/-/g')
be=$(echo "$bp" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/linux/alice/.claude/projects/$ae"
mkdir -p "$work/mac/bob/.claude/projects/$be"
sid="aaaaaaaa-0000-0000-0000-000000000000"
af="$work/linux/alice/.claude/projects/$ae/$sid.jsonl"
bf="$work/mac/bob/.claude/projects/$be/$sid.jsonl"
printf '{"cwd":"%s","message":"read %s/file"}\n' "$ap" "$work/linux/alice/src/project" > "$af"
printf '{"cwd":"%s","message":"read %s/file"}\n' "$bp" "$work/mac/bob/src/project" > "$bf"

export HOME="$work/linux/alice" CASI_HOME="$work/linux/alice/casi"
export CASI_CLAUDE_HOME="$work/linux/alice/.claude"
"$casi" init --remote "file://$work/remote.git" --machine linux >/dev/null
"$casi" config --add root.project.alias "$work/linux/alice/older/project"
"$casi" config --add root.project.alias "$work/linux/alice/src/project"
"$casi" push >/dev/null

export HOME="$work/mac/bob" CASI_HOME="$work/mac/bob/casi"
export CASI_CLAUDE_HOME="$work/mac/bob/.claude"
"$casi" init --remote "file://$work/remote.git" --machine mac >/dev/null
"$casi" config --add root.project.alias "$work/mac/bob/older/project"
"$casi" config --add root.project.alias "$work/mac/bob/src/project"
out=$("$casi" status --porcelain)
echo "$out" | grep -q '^current	1$' ||
    fail "identical sessions with historical aliases were reported divergent: $out"
echo "$out" | grep -q '^conflict	0$' || fail "unexpected conflict: $out"

echo "path_alias: 1 passed"
