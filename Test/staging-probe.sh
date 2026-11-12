#!/bin/sh
# Call-shape probe matrix for the frame-form staging area.
#
# Two cells per backend:
#   * `staging: call arities*` — the 255/256/257-parameter sweep (2,040/2,048/
#     2,056 bytes), direct and indirect, plus a v128 result, four v128
#     arguments and four i64 arguments. The modules are identical across
#     backends, so a divergent result is visible as a differing cell.
#   * `spec: call` — the suite's 100-parameter fixture, the shape that set the
#     old 16-slot limit in the first place.
#
# Usage:
#   Test/staging-probe.sh [path/to/WasmJit]
#   WASMJIT=... Test/staging-probe.sh
#
# Exit status is 0 only when every cell passed.
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-${WASMJIT:-$ROOT/build/Desktop-Debug/Test/WasmJit}}

if [ ! -x "$BIN" ]; then
	echo "probe: no executable at $BIN" >&2
	exit 2
fi
cd "$ROOT" || exit 2

failures=0

run_cell()
{
	backend=$1
	filter=$2
	output=$(WASMJIT_BACKEND="$backend" "$BIN" "$filter" --rng-seed 12345 2>&1)
	status=$?
	summary=$(printf '%s\n' "$output" | \
		grep -E 'All tests passed|test cases:' | tail -1)
	if [ $status -ne 0 ]; then
		failures=$((failures + 1))
	fi
	printf '  %-7s %-22s exit=%d  %s\n' "$backend" "$filter" "$status" \
		"${summary:-no summary}"
}

printf 'staging call-shape matrix: %s\n' "$BIN"
for backend in libjit sljit asmjit; do
	run_cell "$backend" 'staging: call arities*'
	run_cell "$backend" 'spec: call'
done

if [ $failures -ne 0 ]; then
	printf 'staging call-shape matrix: FAILED (%d cell(s))\n' "$failures"
	exit 1
fi
printf 'staging call-shape matrix: passed (6 cells)\n'
