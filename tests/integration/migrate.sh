#!/bin/sh
# `casi migrate <home...>` rewrites a foreign machine's home to THIS machine's
# home inside every transcript and sidecar, backing up the originals first. It
# exists for the one-time cleanup after a sync tool left other machines' paths
# embedded in the sessions.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/home/src/p" "$work/home/.claude"
export HOME="$work/home" CASI_HOME="$work/home/casi" CASI_CLAUDE_HOME="$work/home/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null

set +e
out=$("$casi" migrate '' 2>&1)
rc=$?
set -e
[ "$rc" = 2 ] || fail "empty foreign home: exit $rc, want 2"
echo "$out" | grep -q 'cannot be empty' || fail "empty foreign home: missing error"

enc=$(echo "$work/home/src/p" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/home/.claude/projects/$enc"
f="$work/home/.claude/projects/$enc/11111111-0000-0000-0000-000000000000.jsonl"
printf '{"cwd":"%s","message":"see %s/a.txt, %s/b.txt -type f"}\n' \
    "$work/home/src/p" "$work/foreign" "$work/foreign" > "$f"

"$casi" migrate "$work/foreign" >/dev/null

grep -q "see $work/home/a.txt, $work/home/b.txt -type f" "$f" \
    || fail "foreign home was not rewritten to the local home"
grep -q "$work/foreign" "$f" && fail "foreign home leaked into the transcript"

backup="$work/home/casi/migrate-backup"
[ -d "$backup" ] || fail "no migrate backup directory"
original=$(find "$backup" -type f -print -quit)
[ -n "$original" ] || fail "migrate backup directory is empty"
first=$(cat "$original")
printf '{"message":"again %s/c.txt"}\n' "$work/foreign" >> "$f"
"$casi" migrate "$work/foreign" >/dev/null
[ "$(cat "$original")" = "$first" ] || fail "second migration overwrote first backup"
[ "$(find "$backup" -type f | wc -l)" -eq 2 ] ||
    fail "second migration did not preserve both backup versions"

echo "migrate: 2 passed"
