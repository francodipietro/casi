#!/bin/sh
# Default pull chooses the newest transcript event, archives the losing copy,
# and returns success. --strict retains the older conflict-only behaviour.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }
sid=aaaaaaaa-0000-0000-0000-000000000000
sid_time=bbbbbbbb-0000-0000-0000-000000000000

for machine in a b; do
    mkdir -p "$work/$machine/src/p" "$work/$machine/.claude"
    encoded=$(echo "$work/$machine/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
    mkdir -p "$work/$machine/.claude/projects/$encoded/$sid/subagents"
done

sa="$work/a/.claude/projects/$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid.jsonl"
sb="$work/b/.claude/projects/$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid.jsonl"
sa_time="$work/a/.claude/projects/$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid_time.jsonl"
sb_time="$work/b/.claude/projects/$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid_time.jsonl"
printf '{"type":"user","cwd":"%s","timestamp":"2026-01-01T00:00:00Z","message":"base"}\n' \
    "$work/a/src/p" > "$sa"
printf '{"type":"user","cwd":"%s","timestamp":"2026-01-01T00:00:00Z","message":"unchanged parent"}\n' \
    "$work/a/src/p" > "$sa_time"
agent_a="$work/a/.claude/projects/$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid/subagents/agent-one.jsonl"
agent_b="$work/b/.claude/projects/$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid/subagents/agent-one.jsonl"
agent_a_time="$work/a/.claude/projects/$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid_time/subagents/agent-time.jsonl"
agent_b_time="$work/b/.claude/projects/$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid_time/subagents/agent-time.jsonl"
mkdir -p "$(dirname "$agent_a_time")"
printf '{"cwd":"%s","message":"base sidecar"}\n' "$work/a/src/p" > "$agent_a"
printf '{"cwd":"%s","timestamp":"2026-01-01T00:00:00Z","message":"base timestamp sidecar"}\n' \
    "$work/a/src/p" > "$agent_a_time"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" pull >/dev/null

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
printf '{"type":"assistant","cwd":"%s","timestamp":"2026-01-03T00:00:00Z","message":"newest remote"}\n' \
    "$work/a/src/p" >> "$sa"
printf '{"cwd":"%s","message":"newest remote sidecar"}\n' "$work/a/src/p" > "$agent_a"
printf '{"cwd":"%s","timestamp":"2026-01-03T00:00:00Z","message":"newest independent sidecar"}\n' \
    "$work/a/src/p" > "$agent_a_time"
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
printf '{"type":"assistant","cwd":"%s","timestamp":"2026-01-02T00:00:00Z","message":"older local"}\n' \
    "$work/b/src/p" >> "$sb"
printf '{"cwd":"%s","message":"older local sidecar"}\n' "$work/b/src/p" > "$agent_b"
printf '{"cwd":"%s","timestamp":"2026-01-02T00:00:00Z","message":"older independent sidecar"}\n' \
    "$work/b/src/p" > "$agent_b_time"

# A preview deliberately does not fetch (fetching changes tracking refs), so
# make B's already-fetched view include A's independent edit before checking
# that the planner itself is fully read-only.
git -C "$work/b/casi/repo.git" fetch --quiet origin

# Planning a divergence must inspect the current local edit without writing
# its chunks or a refreshed stat cache into B's real store.
objects_before=$(find "$work/b/casi/repo.git/objects" -type f | wc -l | tr -d ' ')
if [ -e "$work/b/casi/index" ]; then
    index_before=$(cksum "$work/b/casi/index")
else
    index_before=missing
fi
"$casi" pull --dry-run >"$work/dry-run" || fail "read-only automatic preview failed"
objects_after=$(find "$work/b/casi/repo.git/objects" -type f | wc -l | tr -d ' ')
if [ -e "$work/b/casi/index" ]; then
    index_after=$(cksum "$work/b/casi/index")
else
    index_after=missing
fi
[ "$objects_before" = "$objects_after" ] || fail "dry-run wrote an object into the real store"
[ "$index_before" = "$index_after" ] || fail "dry-run changed the real stat cache"
grep -q 'would auto-resolve' "$work/dry-run" || fail "dry-run did not report its choice"
grep -q 'older local' "$sb" || fail "dry-run changed the local transcript"
[ ! -e "$work/b/casi/conflicts" ] || fail "dry-run created an archive"

"$casi" sync >"$work/out" || fail "automatic sync failed"
grep -q 'auto-resolved' "$work/out" || fail "automatic resolution was not reported"
grep -q 'newest remote' "$sb" || fail "newest session was not selected"
! grep -q 'older local' "$sb" || fail "older local session survived"
parked=$(find "$work/b/casi/conflicts" -name '*archived-local*.jsonl' -print -quit)
[ -n "$parked" ] || fail "older local copy was not archived"
grep -q 'older local' "$parked" || fail "archive does not contain older local copy"
grep -q 'newest remote sidecar' "$agent_b" ||
    fail "subagent did not follow the winning parent session"
! grep -q 'older local sidecar' "$agent_b" ||
    fail "older subagent survived beside the winning session"
parked_agent=$(find "$work/b/casi/conflicts" -name '*agent-one.jsonl-archived-local*' -print -quit)
[ -n "$parked_agent" ] || fail "older subagent copy was not archived"
grep -q 'older local sidecar' "$parked_agent" ||
    fail "subagent archive does not contain the losing copy"
grep -q 'newest independent sidecar' "$agent_b_time" ||
    fail "subagent timestamp did not choose the newest independent sidecar"
! grep -q 'older independent sidecar' "$agent_b_time" ||
    fail "older independent sidecar survived"

# The winner is pushed as part of sync, so a fresh third machine converges
# without knowing anything about the original conflict.
mkdir -p "$work/c/src/p" "$work/c/.claude"
export HOME="$work/c" CASI_HOME="$work/c/casi" CASI_CLAUDE_HOME="$work/c/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-c >/dev/null
"$casi" pull >/dev/null
sc="$work/c/.claude/projects/$(echo "$work/c/src/p" | sed 's/[^a-zA-Z0-9]/-/g')/$sid.jsonl"
grep -q 'newest remote' "$sc" || fail "automatic winner was not published for a new machine"
echo "auto_resolve: automatic newest-wins archive passed"
