#!/bin/sh
# `casi diagnose` must classify every session without leaking content: the
# whole local-vs-remote verdict in one parseable listing. Covers same,
# diverged, local_only and remote_only; the ahead cases share the compare path
# already exercised by two_machines/conflict.
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

# A publishes: same, remote_only
mkdir -p "$work/a/src/p"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
mk "$work/a" "$work/a/src/p" "11111111-0000-0000-0000-000000000000" "same"
mk "$work/a" "$work/a/src/p" "22222222-0000-0000-0000-000000000000" "remote only"
"$casi" push >/dev/null

# B pulls, adds a local-only session, and diverges on "same".
mkdir -p "$work/b/src/p"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/b/src" >/dev/null
"$casi" pull >/dev/null
mk "$work/b" "$work/b/src/p" "33333333-0000-0000-0000-000000000000" "local only"
enc_b=$(echo "$work/b/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
printf '{"cwd":"%s","message":"B edit"}\n' "$work/b/src/p" \
    >> "$work/b/.claude/projects/$enc_b/11111111-0000-0000-0000-000000000000.jsonl"

# A diverges on "same" and adds a remote-only session.
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
enc_a=$(echo "$work/a/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
printf '{"cwd":"%s","message":"A edit"}\n' "$work/a/src/p" \
    >> "$work/a/.claude/projects/$enc_a/11111111-0000-0000-0000-000000000000.jsonl"
mk "$work/a" "$work/a/src/p" "44444444-0000-0000-0000-000000000000" "remote only 2"
"$casi" push >/dev/null

# B refreshes its view and classifies.
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
git -C "$work/b/casi/repo.git" fetch -q origin \
    '+refs/heads/casi/*:refs/remotes/origin/casi/*'

out=$("$casi" diagnose)

echo "$out" | grep -q '^session	diverged	11111111' || fail "11111111 not diverged"
echo "$out" | grep -q '^session	same	22222222' || fail "22222222 not same"
echo "$out" | grep -q '^session	local_only	33333333' || fail "33333333 not local_only"
echo "$out" | grep -q '^session	remote_only	44444444' || fail "44444444 not remote_only"
echo "$out" | grep -q '^summary	1	0	0	1	1	1' || fail "summary counts wrong: $(echo "$out" | grep '^summary')"
echo "  ok   diagnose classifies same/diverged/local_only/remote_only"

echo "diagnose: 1 passed"
