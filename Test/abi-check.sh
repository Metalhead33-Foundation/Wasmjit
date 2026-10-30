#!/bin/sh
# ABI check: the exported symbol set may grow, never shrink.
#
#   ./Test/abi-check.sh <build-dir>          # compare against Test/abi/*.exports
#   ./Test/abi-check.sh --update <build-dir> # rewrite the baselines
#
# docs/API_ABI_STABILITY.md G2 ("additive-only evolution") says a change may add
# to the surface and may not remove from it. This is the mechanical form of that
# sentence, and it is the ES-7 follow-up to AAS1. It compares what the dynamic
# linker sees (`nm -D --defined-only`, strong symbols only: weak template
# instantiations are not an ABI promise), so "a symbol a released embedder links
# against has disappeared" fails the check.
#
# If libabigail is installed, `ABI_REFERENCE=<previous libWasmBase.so>` adds a
# full structural diff (`abidiff`) on top of the symbol list. It is optional: the
# symbol baseline works everywhere and needs no extra dependency.
set -eu

usage() {
    echo "usage: $0 [--update] <build-dir>" >&2
    exit 2
}

update=0
if [ "${1:-}" = "--update" ]; then
    update=1
    shift
fi
[ $# -eq 1 ] || usage
build=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
baseline_dir="$here/abi"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# One library: `exports <lib> <baseline>`.
exports() {
    nm -D --defined-only --format=posix "$1" \
        | awk '$2 ~ /^[TDBR]$/ { print $1 " " $2 }' \
        | LC_ALL=C sort > "$2"
}

status=0
check() {
    lib=$1
    baseline=$2
    name=$(basename "$baseline" .exports)
    if [ ! -f "$lib" ]; then
        echo "abi-check: missing library $lib" >&2
        status=1
        return
    fi
    exports "$lib" "$work/$name.current"
    if [ "$update" -eq 1 ]; then
        mkdir -p "$baseline_dir"
        cp "$work/$name.current" "$baseline"
        echo "abi-check: updated $name ($(wc -l < "$baseline") symbols)"
        return
    fi
    if [ ! -f "$baseline" ]; then
        echo "abi-check: no baseline for $name; run with --update" >&2
        status=1
        return
    fi
    LC_ALL=C sort "$baseline" > "$work/$name.baseline"
    LC_ALL=C comm -23 "$work/$name.baseline" "$work/$name.current" > "$work/$name.removed"
    LC_ALL=C comm -13 "$work/$name.baseline" "$work/$name.current" > "$work/$name.added"
    removed=$(wc -l < "$work/$name.removed")
    added=$(wc -l < "$work/$name.added")
    echo "abi-check: $name: $removed removed, $added added"
    if [ "$removed" -ne 0 ]; then
        echo "abi-check: REMOVED symbols are an ABI break (docs/API_ABI_STABILITY.md G2):" >&2
        sed 's/^/  - /' "$work/$name.removed" | head -40 >&2
        status=1
    fi
}

check "$build/WasmBase/libWasmBase.so" "$baseline_dir/libWasmBase.exports"
check "$build/LibJit/libLibJit.so" "$baseline_dir/libLibJit.exports"

# Optional structural diff (libabigail). `ABI_REFERENCE` names the previous
# release's libWasmBase.so; without it this stage is skipped, not failed.
if [ "$update" -eq 0 ] && [ -n "${ABI_REFERENCE:-}" ]; then
    if command -v abidiff >/dev/null 2>&1; then
        echo "abi-check: abidiff $ABI_REFERENCE $build/WasmBase/libWasmBase.so"
        abidiff "$ABI_REFERENCE" "$build/WasmBase/libWasmBase.so" || status=1
    else
        echo "abi-check: ABI_REFERENCE set but abidiff is not installed; skipped" >&2
    fi
fi

if [ "$update" -eq 0 ]; then
    if [ "$status" -eq 0 ]; then
        echo "abi-check: OK (no exported symbol was removed)"
    else
        echo "abi-check: FAILED" >&2
    fi
fi
exit $status
