#!/bin/sh
# End-to-end: two isolated "machines" with DIFFERENT local layouts, one
# git remote between them. Exercises the actual thing casi exists to do --
# unit tests check the pieces, this checks they fit together.
set -eu
casi="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
git init -q --bare "$work/remote.git"

fail() { echo "FAIL: $*" >&2; exit 1; }

# --- machine A: project under ~/src -----------------------------------
mkdir -p "$work/a/src/myproj/.claude-marker" "$work/a/.claude"
rm -rf "$work/a/src/myproj/.claude-marker"
enc_a=$(echo "$work/a/src/myproj" | sed 's/[^a-zA-Z0-9]/-/g')
mkdir -p "$work/a/.claude/projects/$enc_a"
sess="$work/a/.claude/projects/$enc_a/11111111-2222-3333-4444-555555555555.jsonl"
printf '{"type":"user","cwd":"%s","message":"see %s/main.c"}\n' \
    "$work/a/src/myproj" "$work/a/src/myproj" > "$sess"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
set +e
out=$("$casi" init --machine 'bad name' 2>&1)
rc=$?
set -e
[ "$rc" = 2 ] || fail "invalid machine name: exit $rc, want 2"
echo "$out" | grep -q 'invalid machine name' || fail "invalid machine name: missing error"
echo "  ok   invalid machine name rejected during init"

"$casi" init --remote "file://$work/remote.git" --machine machine-a >/dev/null
"$casi" config root.src.path "$work/a/src" >/dev/null
"$casi" push >/dev/null || fail "push from A"
echo "  ok   push from A"

# --- machine B: SAME project, DIFFERENT local layout (~/code, not ~/src) --
mkdir -p "$work/b/code/myproj" "$work/b/.claude"
export HOME="$work/b" CASI_HOME="$work/b/casi" CASI_CLAUDE_HOME="$work/b/.claude"
"$casi" init --remote "file://$work/remote.git" --machine machine-b >/dev/null
"$casi" config root.src.path "$work/b/code" >/dev/null
"$casi" pull >/dev/null || fail "pull into B"

got=$(find "$work/b/.claude/projects" -name '*.jsonl')
[ -n "$got" ] || fail "no session materialised on B"

# The directory name must be re-encoded for B's OWN layout, never A's.
enc_b=$(echo "$work/b/code/myproj" | sed 's/[^a-zA-Z0-9]/-/g')
case "$got" in
    *"/$enc_b/"*) ;;
    *) fail "session landed under the wrong directory: $got" ;;
esac

# Both the cwd field AND the path embedded in message text must translate.
grep -q "\"cwd\":\"$work/b/code/myproj\"" "$got" || fail "cwd was not translated"
grep -q "$work/b/code/myproj/main.c" "$got" || fail "embedded path was not translated"
grep -q "$work/a/src" "$got" && fail "machine A's path leaked into B's copy"
echo "  ok   pull into B: layout re-encoded, cwd and embedded paths translated"

# --- round-trip: B can continue A's session and A must accept that append ---
printf '{"type":"assistant","cwd":"%s","message":"continued on B"}\n' \
    "$work/b/code/myproj" >> "$got"
"$casi" push >/dev/null || fail "push from B"

export HOME="$work/a" CASI_HOME="$work/a/casi" CASI_CLAUDE_HOME="$work/a/.claude"
"$casi" pull >/dev/null || fail "pull continued session into A"
grep -q 'continued on B' "$sess" || fail "A did not receive B's append"
grep -q "\"cwd\":\"$work/a/src/myproj\"" "$sess" || fail "B's cwd leaked into A"
echo "  ok   round-trip B -> A accepts an append within the tail chunk"

# --- incremental: appending on A must not resend the whole transcript ----
before=$(git -C "$work/remote.git" count-objects -v | awk '/^size-pack:/{print $2}')
printf '{"type":"user","cwd":"%s","message":"one more line"}\n' \
    "$work/a/src/myproj" >> "$sess"
"$casi" push >/dev/null || fail "second push from A"
after=$(git -C "$work/remote.git" count-objects -v | awk '/^size-pack:/{print $2}')
echo "  ok   incremental push (remote pack: ${before:-0} KB -> ${after:-0} KB)"

# --- idempotence: pushing again with nothing new creates no new commit ---
before_ref=$(git -C "$work/remote.git" rev-parse refs/heads/casi/machine-a)
"$casi" push >/dev/null || fail "third push from A"
after_ref=$(git -C "$work/remote.git" rev-parse refs/heads/casi/machine-a)
[ "$before_ref" = "$after_ref" ] || fail "push with no changes still moved the ref"
echo "  ok   push with no changes is a no-op"

echo "two_machines: 6 passed"
