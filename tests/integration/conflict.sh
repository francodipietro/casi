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
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null

mkdir -p "$work/b/src/p" "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/b/src" >/dev/null
"$casi" pull >/dev/null
sess_b=$(find "$work/b/.claude/projects" -name '*.jsonl')

# Both sides grow the SAME session with DIFFERENT content -> real divergence.
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
printf '{"type":"user","cwd":"%s","message":"edited on A"}\n' "$work/a/src/p" >> "$sess_a"
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
printf '{"type":"user","cwd":"%s","message":"edited on B, differently"}\n' "$work/b/src/p" >> "$sess_b"
before_b=$(cat "$sess_b")

# status also exits 3 when a conflict is pending -- that is the outcome under
# test, so errexit has to step aside for exactly this call or the script would
# die right here on the assignment, before any check runs.
set +e; out=$("$casi" status 2>&1); set -e
echo "$out" | grep -q '^machine:' || fail "status: missing machine header"

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
echo "  ok   pull: exit 3, no spurious error line"

[ "$(cat "$sess_b")" = "$before_b" ] || fail "pull: local file was modified during a conflict"
echo "  ok   pull: local copy untouched"

parked=$(find "$work/b/casi/conflicts" -name '*.jsonl' 2>/dev/null | head -1)
[ -n "$parked" ] || fail "pull: no conflicting copy was parked"
grep -q 'edited on A' "$parked" || fail "parked copy does not contain the remote content"
echo "  ok   pull: remote copy parked at $parked"

"$casi" pull --theirs aaaaaaaa-0000-0000-0000-000000000000 >/dev/null || fail "pull --theirs"
grep -q 'edited on A' "$sess_b" || fail "--theirs did not take the remote copy"
echo "  ok   pull --theirs: remote copy adopted"

echo "conflict: 5 passed"
