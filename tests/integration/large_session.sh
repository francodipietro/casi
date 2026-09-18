#!/bin/sh
# A 200 MiB synthetic transcript catches two regressions the small fixtures
# cannot: a push after a short append must send only the mutable chunk tail,
# and the stat cache must reconstruct that same tail byte-for-byte.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

project="$work/a/src/large"
mkdir -p "$project" "$work/a/.claude"
encoded=$(echo "$project" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$encoded"
session="$work/a/.claude/projects/$encoded/99999999-0000-0000-0000-000000000000.jsonl"

# 150 MiB of random input becomes approximately 200 MiB after base64.  Fold
# it before wrapping records so every JSONL line remains far below a chunk.
dd if=/dev/urandom bs=1048576 count=150 2>/dev/null |
    base64 | tr -d '\n' | fold -w 65500 |
    awk -v cwd="$project" '{printf "{\"type\":\"user\",\"cwd\":\"%s\",\"message\":\"%s\"}\n", cwd, $0}' \
    > "$session"

size=$(wc -c < "$session" | tr -d ' ')
[ "$size" -ge $((200 * 1024 * 1024)) ] || fail "fixture is only $size bytes"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "initial large push"
[ -s "$work/a/casi/index" ] || fail "initial scan did not write the stat cache"
before=$(git -C "$work/remote.git" count-objects -v | awk '/^size-pack:/{print $2}')

# A 75 KiB random append is deliberately much smaller than one chunk.
dd if=/dev/urandom bs=1024 count=75 2>/dev/null |
    base64 | tr -d '\n' | fold -w 65500 |
    awk -v cwd="$project" '{printf "{\"type\":\"assistant\",\"cwd\":\"%s\",\"message\":\"%s\"}\n", cwd, $0}' \
    >> "$session"
"$casi" push >/dev/null || fail "append large push"
after=$(git -C "$work/remote.git" count-objects -v | awk '/^size-pack:/{print $2}')
delta=$((after - before))
[ "$delta" -lt 1024 ] || fail "append transferred ${delta} KiB, want less than 1024 KiB"
echo "  ok   append transferred ${delta} KiB (< 1 MiB)"

# Pull through a second, isolated Claude home while deliberately mapping its
# root to A's path.  That keeps normalized content and the original bytes
# identical, so cmp proves the cached tail was not truncated or shifted.
mkdir -p "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" pull >/dev/null || fail "pull large transcript"
restored=$(find "$work/b/.claude/projects" -name '*.jsonl')
[ -n "$restored" ] || fail "no large transcript materialised"
cmp -s "$session" "$restored" || fail "remote transcript differs after cached append"
echo "  ok   cached append round-trips byte-for-byte"

echo "large_session: 2 passed"
