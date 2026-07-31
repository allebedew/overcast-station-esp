#!/usr/bin/env bash
# ESP-IDF wrapper: sources the IDF environment, then runs idf.py from the
# project root.
#   ./build.sh                  -> idf.py build
#   ./build.sh flash monitor
#   ./build.sh size-components
# Point IDF_PATH somewhere else to use a different IDF checkout.
#
# Having a single entry point also means one permission rule covers every
# build, instead of one rule per `bash -c '. export.sh && idf.py ...'` variant.
set -e
set -o pipefail
cd "$(dirname "$0")"

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH — set IDF_PATH to your checkout" >&2
    exit 1
fi

# export.sh is noisy and not -u clean, so silence it and keep -u off.
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1

if [ "$#" -eq 0 ]; then
    exec idf.py build
fi

exec idf.py "$@"
