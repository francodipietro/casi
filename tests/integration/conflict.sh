#!/bin/sh
# The prefix rule under a real divergence: both the "preserve local, park
# remote" outcome and the two output-quality bugs found testing it by hand --
# the duplicate "error: unknown error" line, and warnings printed before the
# report they refer to when output is not a tty (which is every pipe, log
# file, or `casi sync` run from a cron/launchd job).
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/a/src/p" "$work/a/.claude"
enc_a=$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$enc_a"
sess_a="$work/a/.claude/projects/$enc_a/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
printf '{"type":"user","cwd":"%s","message":"base"}\n' "$work/a/src/p" > "$sess_a"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" push >/dev/null

# B has the same project "p" in use (registers its basename -> B's path).
mkdir -p "$work/b/src/p" "$work/b/.claude"
enc_b=$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/b/.claude/projects/$enc_b"
printf '{"type":"user","cwd":"%s","message":"B own"}\n' "$work/b/src/p" \
    > "$work/b/.claude/projects/$enc_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl"

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" pull >/dev/null
sess_b="$work/b/.claude/projects/$enc_b/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
[ -f "$sess_b" ] || fail "A's session was not materialised on B"

# Both sides grow the SAME session with DIFFERENT content -> real divergence.
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
printf '{"type":"user","cwd":"%s","message":"edited on A"}\n' "$work/a/src/p" >> "$sess_a"
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
printf '{"type":"user","cwd":"%s","message":"edited on B, differently"}\n' "$work/b/src/p" >> "$sess_b"
before_b=$(cat "$sess_b")

# status deliberately works from the last fetch. Refresh B's tracking refs
# without materialising A's edit, so the comparison below sees the genuine
# A-versus-B divergence rather than the common base B pulled earlier.
git -C "$work/b/casi/repo.git" fetch -q origin \
    '+refs/heads/casi/*:refs/remotes/origin/casi/*'

# status also exits 3 when a conflict is pending -- that is the outcome under
# test, so errexit has to step aside for exactly this call or the script would
# die right here on the assignment, before any check runs.
set +e; out=$("$casi" status 2>&1); set -e
echo "$out" | grep -q '^machine:' || fail "status: missing machine header"

# The conflict report must carry enough context to choose without decoding a
# session id: the project, a short id, which machine the remote copy came from,
# and how far the two sides still agree.
echo "$out" | grep -q 'casi://p' || fail "status: conflict missing project path"
echo "$out" | grep -q 'aaaaaaaa' || fail "status: conflict missing short id"
echo "$out" | grep -q 'remote (machine-a)' || fail "status: conflict missing origin machine"
echo "$out" | grep -q 'share 0 of 1 chunks' || fail "status: conflict missing common-prefix info"
echo "  ok   status: conflict report carries project, id, origin, prefix"

set +e; porc=$("$casi" status --porcelain 2>&1); set -e
echo "$porc" | grep -q '^diverge	' || fail "status --porcelain: missing diverge line"
echo "$porc" | grep -q '^diverge	aaaaaaaa' || fail "status --porcelain: diverge line missing short id"
echo "$porc" | grep -q 'machine-a' || fail "status --porcelain: diverge line missing origin machine"
echo "  ok   status --porcelain: diverge line is machine-readable"

# Regression: warnings must not appear before the report they explain, even
# when stdout is not a tty (which it never is inside `$(...)`).
first_info=$(echo "$out" | grep -n '^machine:' | head -1 | cut -d: -f1)
first_warn=$(echo "$out" | grep -n '^warning:' | head -1 | cut -d: -f1)
[ -n "$first_warn" ] || fail "status: no conflict warning printed"
[ "$first_info" -lt "$first_warn" ] || fail "status: warning printed before the report (stdout buffering regression)"
echo "  ok   status: conflict reported, in program order"

set +e; out=$("$casi" pull 2>&1); rc=$?; set -e
[ "$rc" = 3 ] || fail "pull with a real divergence: exit $rc, want 3"
echo "$out" | grep -qi 'unknown error' && fail "pull: leaked the 'unknown error' placeholder"
echo "$out" | grep -q 'remote (machine-a)' || fail "pull: conflict missing origin machine"
echo "  ok   pull: exit 3, no spurious error line"

[ "$(cat "$sess_b")" = "$before_b" ] || fail "pull: local file was modified during a conflict"
echo "  ok   pull: local copy untouched"

parked=$(find "$work/b/casi/conflicts" -name '*-remote*.jsonl' 2>/dev/null | head -1)
[ -n "$parked" ] || fail "pull: no conflicting copy was parked"
grep -q 'edited on A' "$parked" || fail "parked copy does not contain the remote content"
echo "  ok   pull: remote copy parked at $parked"

out=$("$casi" conflicts) || fail "conflicts command"
echo "$out" | grep -q 'casi://p' || fail "conflicts: parked copy missing project"
echo "$out" | grep -q '\[remote\]' || fail "conflicts: parked copy missing side"
echo "$out" | grep -q 'machine-a' || fail "conflicts: parked copy missing origin machine"
echo "$out" | grep -q 'aaaaaaaa' || fail "conflicts: parked copy missing short id"
echo "  ok   conflicts: parked copy is described, not just a path"

"$casi" pull --theirs aaaaaaaa-0000-0000-0000-000000000000 >/dev/null || fail "pull --theirs"
local_parked=$(find "$work/b/casi/conflicts" -name '*-local*.jsonl' 2>/dev/null | head -1)
[ -n "$local_parked" ] || fail "pull --theirs: local divergent copy was not parked"
grep -q 'edited on B, differently' "$local_parked" ||
    fail "pull --theirs: parked local copy does not contain the local edits"
echo "  ok   pull --theirs: local copy parked at $local_parked"
grep -q 'edited on A' "$sess_b" || fail "--theirs did not take the remote copy"
echo "  ok   pull --theirs: remote copy adopted"

echo "conflict: 9 passed"
