#!/bin/sh
# A machine tree is a versioned wire format. A familiar-looking tree without
# the supported root marker must fail before pull can interpret or rewrite it.
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
printf '{"cwd":"%s","message":"format fixture"}\n' "$work/a/src/p" \
    > "$work/a/.claude/projects/$enc_a/cccccccc-0000-0000-0000-000000000000.jsonl"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" push >/dev/null

old_commit=$(git -C "$work/remote.git" rev-parse refs/heads/casi/machine-a)
old_tree=$(git -C "$work/remote.git" rev-parse "$old_commit^{tree}")
bad_blob=$(printf '{"format":999}\n' | git -C "$work/remote.git" hash-object -w --stdin)
bad_tree=$(
    {
        git -C "$work/remote.git" ls-tree "$old_tree" | awk -F '\t' '$2 != "casi.json"'
        printf '100644 blob %s\tcasi.json\n' "$bad_blob"
    } | git -C "$work/remote.git" mktree
)
bad_commit=$(git -C "$work/remote.git" -c user.name=casi-test -c user.email=casi@test \
    commit-tree "$bad_tree" -p "$old_commit" -m 'invalid format fixture')
git -C "$work/remote.git" update-ref refs/heads/casi/machine-a "$bad_commit" "$old_commit"

mkdir -p "$work/b/src/p" "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
set +e
out=$("$casi" pull 2>&1)
rc=$?
set -e
[ "$rc" = 2 ] || fail "pull from unsupported tree format: exit $rc, want 2"
echo "$out" | grep -q 'unsupported or missing machine tree format' ||
    fail "pull did not identify the machine tree format"
echo "format: 1 passed"
