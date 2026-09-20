#!/bin/sh
# `casi init` is onboarding, not infrastructure setup: a pasted Markdown link or
# a URL it never reaches must fail, and success must follow a real connection.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

export HOME="$work/home"

# A pasted Markdown link is rejected before a store is created.
set +e
out=$(CASI_HOME="$work/casi-md" CASI_CLAUDE_HOME="$work/claude" \
    "$casi" init --remote '[https://github.com/u/repo](https://github.com/u/repo)' 2>&1)
rc=$?
set -e
[ "$rc" = 2 ] || fail "markdown url: exit $rc, want 2"
echo "$out" | grep -qi 'markdown' || fail "markdown url: missing guidance"
[ ! -d "$work/casi-md/repo.git" ] || fail "markdown url: store was created anyway"
echo "  ok   init: pasted Markdown link is rejected"

# Whitespace in the URL is rejected the same way.
set +e
out=$(CASI_HOME="$work/casi-space" CASI_CLAUDE_HOME="$work/claude" \
    "$casi" init --remote 'file:///a path with spaces' 2>&1)
rc=$?
set -e
[ "$rc" = 2 ] || fail "space url: exit $rc, want 2"
echo "  ok   init: URL with spaces is rejected"

# A remote it cannot reach fails with the network code, not a success message.
set +e
out=$(CASI_HOME="$work/casi-bad" CASI_CLAUDE_HOME="$work/claude" \
    "$casi" init --remote "file://$work/does-not-exist.git" 2>&1)
rc=$?
set -e
[ "$rc" = 4 ] || fail "unreachable remote: exit $rc, want 4"
echo "$out" | grep -q 'ready' && fail "unreachable remote: announced success"
echo "  ok   init: unreachable remote fails with exit 4"

# A reachable remote verifies and reports success.
out=$(CASI_HOME="$work/casi-ok" CASI_CLAUDE_HOME="$work/claude" \
    "$casi" init --remote "file://$work/remote.git" --machine machine-a 2>&1)
echo "$out" | grep -q 'remote verified' || fail "reachable remote: missing verification"
echo "  ok   init: reachable remote reports verification"

echo "init: 4 passed"
