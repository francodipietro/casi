#!/bin/sh
# A project checkout without Claude history is enough to locate incoming
# sessions; two equally named checkouts must never be guessed between.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"
fail() { echo "FAIL: $*" >&2; exit 1; }

ap="$work/a/src/work/project"
bp="$work/b/code/nested/project"
mkdir -p "$ap" "$bp"
mkdir -p "$work/b/code/unrelated/one/two/three/four/five/six/seven"
ae=$(echo "$ap" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$ae"
sid=aaaaaaaa-0000-0000-0000-000000000000
printf '{"cwd":"%s","message":"use %s/file"}\n' "$ap" "$ap" > \
    "$work/a/.claude/projects/$ae/$sid.jsonl"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine a >/dev/null
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine b >/dev/null
"$casi" pull >/dev/null || fail "could not find unique local checkout"
be=$(echo "$bp" | sed 's/[^a-zA-Z0-9]/-/g')
got="$work/b/.claude/projects/$be/$sid.jsonl"
[ -f "$got" ] || fail "session did not land in the discovered checkout"
grep -Fq "$bp/file" "$got" || fail "embedded path did not use local checkout"

mkdir -p "$work/c/code/project" "$work/c/src/project"
export HOME="$work/c" CASI_HOME="$work/c/casi" CASI_CLAUDE_HOME="$work/c/.claude"
"$casi" init --remote "file://$work/remote.git" --machine c >/dev/null
set +e
"$casi" pull >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "ambiguous project was guessed"
[ ! -e "$work/c/.claude" ] || fail "ambiguous project wrote a Claude session"

mkdir -p "$work/d"
export HOME="$work/d" CASI_HOME="$work/d/casi" CASI_CLAUDE_HOME="$work/d/.claude"
"$casi" init --remote "file://$work/remote.git" --machine d >/dev/null
set +e
"$casi" pull >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "missing checkout was given an invented destination"
[ ! -e "$work/d/.claude" ] || fail "missing checkout wrote a Claude session"

echo "auto_project: 3 passed"
