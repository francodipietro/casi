#!/bin/sh
# spec002:R6 — keep historical integration fixtures focused on their subject
# while the production CLI accepts only `casi init <remote-url>`.
set -eu
real=${CASI_REAL:?CASI_REAL is required}
if [ "${1-}" != init ]; then exec "$real" "$@"; fi
shift
url=
machine=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --remote) url=$2; shift 2 ;;
        --machine) machine=$2; shift 2 ;;
        --keyfile) shift 2 ;;
        --encrypt) shift ;;
        *) url=$1; shift ;;
    esac
done
[ -n "$url" ] || exec "$real" init
printf 'spec002 integration passphrase\nspec002 integration passphrase\n' | "$real" init "$url"
if [ -n "$machine" ]; then "$real" config core.machine "$machine"; fi
