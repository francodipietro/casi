#!/bin/sh
# `doctor` must refresh the authoritative root list, then turn a missing local
# mapping into the documented exit 5 with an actionable config command.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

# A publishes the logical root name.
mkdir -p "$work/a/src" "$work/a/.claude"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "publish shared root"

# B can contact the remote but has no local mapping yet.
mkdir -p "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
set +e
out=$("$casi" doctor 2>&1)
rc=$?
set -e
[ "$rc" = 5 ] || fail "unmapped root: exit $rc, want 5"
echo "$out" | grep -q 'root "src" is not mapped locally' ||
    fail "unmapped root: missing diagnosis"
echo "$out" | grep -q 'casi config root.src.path <local path>' ||
    fail "unmapped root: missing remediation"
echo "  ok   unmapped shared root is diagnosed"

# Once declared, doctor reports a healthy remote and root mapping.
mkdir -p "$work/b/code"
"$casi" config root.src.path "$work/b/code" >/dev/null
"$casi" doctor >/dev/null || fail "mapped root"
echo "  ok   mapped shared root and reachable remote are healthy"

echo "doctor: 2 passed"
