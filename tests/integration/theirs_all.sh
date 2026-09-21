#!/bin/sh
# `casi pull --theirs-all` resolves every divergence -- sessions and auxiliary
# files -- in a single pass, parking each local copy before adopting the remote.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mk() { # home, cwd, sid, msg
    local h="$1" cwd="$2" sid="$3" msg="$4"
    local enc

    enc=$(echo "$cwd" | sed 's/[^a-zA-Z0-9]/-/g')
    mkdir -p "$h/.claude/projects/$enc"
    printf '{"cwd":"%s","message":"%s"}\n' "$cwd" "$msg" \
        > "$h/.claude/projects/$enc/$sid.jsonl"
}

mkdir -p "$work/a/src/p"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
mk "$work/a" "$work/a/src/p" "aaaaaaaa-0000-0000-0000-000000000000" "base A"
mk "$work/a" "$work/a/src/p" "bbbbbbbb-0000-0000-0000-000000000000" "base B"
enc_a=$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$enc_a/aaaaaaaa-0000-0000-0000-000000000000/subagents"
printf '{"cwd":"%s","message":"subagent A"}\n' "$work/a/src/p" \
    > "$work/a/.claude/projects/$enc_a/aaaaaaaa-0000-0000-0000-000000000000/subagents/agent-one.jsonl"
"$casi" push >/dev/null

mkdir -p "$work/b/src/p"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
mk "$work/b" "$work/b/src/p" "aaaaaaaa-0000-0000-0000-000000000000" "base A"
mk "$work/b" "$work/b/src/p" "bbbbbbbb-0000-0000-0000-000000000000" "base B"
"$casi" pull >/dev/null
enc_b=$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
printf '{"cwd":"%s","message":"B edit"}\n' "$work/b/src/p" \
    >> "$work/b/.claude/projects/$enc_b/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
printf '{"cwd":"%s","message":"B edit"}\n' "$work/b/src/p" \
    >> "$work/b/.claude/projects/$enc_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
printf '{"cwd":"%s","message":"subagent B"}\n' "$work/b/src/p" \
    > "$work/b/.claude/projects/$enc_b/aaaaaaaa-0000-0000-0000-000000000000/subagents/agent-one.jsonl"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
printf '{"cwd":"%s","message":"A edit"}\n' "$work/a/src/p" \
    >> "$work/a/.claude/projects/$enc_a/aaaaaaaa-0000-0000-0000-000000000000.jsonl"
printf '{"cwd":"%s","message":"A edit"}\n' "$work/a/src/p" \
    >> "$work/a/.claude/projects/$enc_a/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
"$casi" push >/dev/null

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
git -C "$work/b/casi/repo.git" fetch -q origin \
    '+refs/heads/casi/*:refs/remotes/origin/casi/*'

set +e; out=$("$casi" pull --theirs-all 2>&1); rc=$?; set -e
[ "$rc" = 0 ] || fail "pull --theirs-all: exit $rc, want 0"
echo "$out" | grep -q 'updated 2 session(s)' || fail "pull --theirs-all: sessions not updated"
echo "$out" | grep -q 'updated 1 auxiliary file(s)' || fail "pull --theirs-all: auxiliary not updated"

out=$("$casi" diagnose)
echo "$out" | grep -q '^session	same	aaaaaaaa' || fail "aaaaaaaa still diverged after --theirs-all"
echo "$out" | grep -q '^session	same	bbbbbbbb' || fail "bbbbbbbb still diverged after --theirs-all"
echo "$out" | grep -q 'diverged' && fail "--theirs-all left divergences behind"
echo "  ok   --theirs-all resolves sessions and auxiliary files in one pass"

echo "theirs_all: 1 passed"
