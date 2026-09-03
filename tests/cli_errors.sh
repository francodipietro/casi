#!/bin/sh
# A command that fails must print exactly one error line. Both the command and
# main() used to print, so the user saw the real message followed by
# "error: unknown error".
set -eu
casi="$1"

check() {
    want="$1"; shift
    got=$("$casi" "$@" 2>&1 >/dev/null | grep -c '^error:' || true)
    if [ "$got" != "$want" ]; then
        echo "FAIL: 'casi $*' printed $got error lines, want $want" >&2
        "$casi" "$@" 2>&1 >/dev/null | sed 's/^/    /' >&2
        exit 1
    fi
    if "$casi" "$@" 2>&1 >/dev/null | grep -q 'unknown error'; then
        echo "FAIL: 'casi $*' leaked the placeholder 'unknown error'" >&2
        exit 1
    fi
    echo "  ok   casi $*"
}

check 1 config
check 1 help definitely-not-a-command
check 1 config --list --extra
check 1 config core.nothing
echo "cli_errors: 4 passed"
