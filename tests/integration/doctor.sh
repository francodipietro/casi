#!/bin/sh
# `doctor` must explain the empty/pre-push state before anyone has pushed, and
# report "ready" once the remote is reachable. Projects match by basename, so
# there is no root to map or declare.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/a/src" "$work/a/.claude"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null

# Before anyone pushes, doctor explains the empty state instead of sounding
# like a mandatory setup step is missing.
out=$("$casi" doctor 2>&1)
echo "$out" | grep -q 'reachable (empty' || fail "empty remote: missing empty state"
echo "$out" | grep -q 'no shared configuration yet' || fail "no shared config: missing explanation"
echo "$out" | grep -q 'expected before your first' || fail "no shared config: missing 'expected' wording"
echo "$out" | grep -q 'none found yet' || fail "no sessions: missing inventory"
echo "  ok   pre-push doctor explains the empty state"

"$casi" push >/dev/null || fail "publish shared config"

# B, with no sessions of its own, still reports a reachable remote and ready.
mkdir -p "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
out=$("$casi" doctor 2>&1) || fail "doctor on B"
echo "$out" | grep -q 'reachable (1 machine branch' || fail "doctor: missing remote branch count"
echo "$out" | grep -q 'state: ready' || fail "doctor: missing ready state"
echo "  ok   reachable remote with shared config reports ready"

echo "doctor: 2 passed"
