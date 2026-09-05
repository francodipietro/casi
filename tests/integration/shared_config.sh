#!/bin/sh
# The Phase 2 safety property: exclusions belong to the remote before a newly
# configured machine can push, and an explicit include revokes them everywhere.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

# Machine A represents an upgraded installation: it has a Phase 1 local-only
# exclusion before its first Phase 2 push. Publishing config must migrate that
# entry before any other machine is allowed to contribute sessions.
mkdir -p "$work/a/src/client" "$work/a/.claude"
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" config sync.exclude casi://src/client >/dev/null
"$casi" push >/dev/null || fail "initial config migration"
git -C "$work/remote.git" rev-parse -q --verify refs/heads/casi/config >/dev/null ||
    fail "shared config ref was not pushed"
git -C "$work/remote.git" show refs/heads/casi/config:casi.json |
    grep -q '"casi://src/client"' || fail "legacy exclusion was not migrated"
echo "  ok   first push migrates local exclusions into shared config"

# B has the same canonical project under a different local layout. Its first
# push fetches the shared exclusion and must leave its transcript out.
mkdir -p "$work/b/code/client" "$work/b/.claude"
enc_b=$(echo "$work/b/code/client" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/b/.claude/projects/$enc_b"
sess_b="$work/b/.claude/projects/$enc_b/bbbbbbbb-0000-0000-0000-000000000000.jsonl"
printf '{"type":"user","cwd":"%s","message":"client work"}\n' \
    "$work/b/code/client" > "$sess_b"

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/b/code" >/dev/null
"$casi" push >/dev/null || fail "excluded push from B"

git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/machine-b |
    grep -q 'bbbbbbbb-0000-0000-0000-000000000000' &&
    fail "new machine pushed a project excluded in shared config"
echo "  ok   a new machine inherits exclusions before its first session push"

# A revokes the exclusion. B's next push must observe that exact remote state
# and publish the session without manual configuration on B.
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" include "$work/a/src/client" >/dev/null || fail "shared include on A"

export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" push >/dev/null || fail "included push from B"
git -C "$work/remote.git" ls-tree -r --name-only refs/heads/casi/machine-b |
    grep -q 'bbbbbbbb-0000-0000-0000-000000000000' ||
    fail "include on A did not revoke the exclusion for B"
echo "  ok   include revokes the shared exclusion for every machine"

# An exclusion applies on pull too: already-published material must not be
# restored after a machine decides to exclude that project again.
export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" exclude "$work/a/src/client" >/dev/null || fail "shared exclude on A"
"$casi" pull >/dev/null || fail "excluded pull on A"
find "$work/a/.claude/projects" -name 'bbbbbbbb-0000-0000-0000-000000000000.jsonl' |
    grep -q . && fail "excluded project was materialised by pull"
echo "  ok   shared exclusion prevents pull materialisation too"

echo "shared_config: 4 passed"
