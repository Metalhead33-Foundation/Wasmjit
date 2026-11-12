#!/bin/sh
# SIMD-8 verification — the V-0 probe matrix.
#
# Every cell runs the whole SIMD surface (the hand-written vector cases plus the
# curated `simd_*` spec scripts) and reports its assertion/case count. The
# `native` column is the shipped path; the `helper` column is
# `WASMJIT_SIMD_NATIVE=0`, which must agree on SLJIT and AsmJIT because the
# helper layer is the reference. LibJIT has no native SIMD path, so its two
# columns are the same run and are kept for the symmetry of the matrix.
#
# Usage:
#   Test/simd-native-probe.sh [path/to/WasmJit]
#   WASMJIT=... SIMD_FILTER='*simd*' Test/simd-native-probe.sh
#
# Exit status is 0 only when every cell passed.
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-${WASMJIT:-$ROOT/build/Desktop-Debug/Test/WasmJit}}
FILTER=${SIMD_FILTER:-'*simd*'}

if [ ! -x "$BIN" ]; then
	echo "probe: no executable at $BIN" >&2
	exit 2
fi
cd "$ROOT" || exit 2

failures=0

run_cell()
{
	backend=$1
	mode=$2
	if [ "$mode" = helper ]; then
		output=$(WASMJIT_SIMD_NATIVE=0 WASMJIT_BACKEND="$backend" \
			"$BIN" "$FILTER" --rng-seed 12345 2>&1)
	else
		output=$(WASMJIT_BACKEND="$backend" \
			"$BIN" "$FILTER" --rng-seed 12345 2>&1)
	fi
	status=$?
	summary=$(printf '%s\n' "$output" | \
		grep -E 'All tests passed|test cases:' | tail -1)
	if [ $status -ne 0 ]; then
		failures=$((failures + 1))
	fi
	printf '  %-7s %-7s exit=%d  %s\n' "$backend" "$mode" "$status" \
		"${summary:-no summary}"
}

# Structural inventory: how many dispatcher entries take the native path. These
# are the two numbers SIMD-8 records, and a change that adds an op without a
# dispatch line shows up here as a mismatch against the note.
asmjit_binary=$(grep -c 'override { emitVectorBinaryNative(' \
	Asmjit/AsmjitModuleCompiler.cpp)
asmjit_shift=$(grep -c 'override { emitVectorShiftNative(' \
	Asmjit/AsmjitModuleCompiler.cpp)
asmjit_native=$((asmjit_binary + asmjit_shift))
sljit_native=$(grep -cE \
	'if \(emitV128(Bitwise|Splat|Extract|Replace)Native' \
	Sljit/SljitOpcodeDispatcher.cpp)
printf 'SIMD-8 V-0 probe matrix: %s\n' "$BIN"
printf '  native-dispatch inventory: AsmJIT=%s (%s binary + %s shift)  SLJIT=%s\n' \
	"$asmjit_native" "$asmjit_binary" "$asmjit_shift" "$sljit_native"
printf '  filter: %s\n' "$FILTER"

for backend in libjit sljit asmjit; do
	run_cell "$backend" native
	run_cell "$backend" helper
done

if [ $failures -ne 0 ]; then
	printf 'V-0 probe matrix: FAILED (%d cell(s))\n' "$failures"
	exit 1
fi
printf 'V-0 probe matrix: passed (6 cells)\n'
