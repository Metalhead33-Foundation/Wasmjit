#!/bin/sh
# LE-2: regenerate the superseded-exception fixtures and record the probe matrix
# (docs/proposals/LEGACY_EXCEPTIONS.md §4.5, P1-P9).
#
#   ./Test/legacy-eh-probe.sh [build-dir]
#
# The fixtures in Test/wasm_wat/legacy/ are *committed* artifacts: `wasm_wat/*.wat`
# is compiled by `wasm-tools parse` at build time, and that tool refuses the
# superseded form, so the `.wat` there are sources for this script, not build
# inputs. Run it after editing them, and commit the `.wasm` it writes.
#
# Tools that are optional (Binaryen, wabt, clang) are reported and skipped when
# absent: the fixtures are in the tree, so the test suite never needs them.
set -eu

source_dir=$(cd "$(dirname "$0")/.." && pwd)
fixtures="$source_dir/Test/wasm_wat/legacy"
build=${1:-$source_dir/build/Desktop-Debug}
harness="$build/Test/WasmJit"

say() { printf '%s\n' "$*"; }
skip() { say "legacy-eh-probe: skipped: $*"; }

say "legacy-eh-probe: fixtures in $fixtures"

# ── P1: build the fixtures (wabt) ─────────────────────────────────────────
if command -v wat2wasm >/dev/null 2>&1; then
    for wat in try_catch rethrow shapes delegate_skip; do
        (cd "$fixtures" && wat2wasm --enable-exceptions "$wat.wat" -o "$wat.wasm")
        say "  P1  $wat.wasm            built ($(wc -c < "$fixtures/$wat.wasm") bytes)"
    done
else
    skip "wat2wasm not found; using the committed .wasm fixtures"
fi

# ── P2: what final-spec tooling says about the legacy form ────────────────
if command -v wasm-tools >/dev/null 2>&1; then
    if wasm-tools validate "$fixtures/try_catch.wasm" 2>/tmp/legacy-eh-p2.err; then
        say "  P2  wasm-tools validate   unexpectedly accepted the legacy form"
    else
        say "  P2  wasm-tools validate   refused it: $(grep -m1 -i legacy /tmp/legacy-eh-p2.err | sed 's/^ *//')"
    fi
    if wasm-tools validate --features legacy-exceptions "$fixtures/try_catch.wasm"; then
        say "  P2b --features legacy-exceptions  accepted it (tooling can inspect the form)"
    fi
fi

# ── P4: the Binaryen translation, and the twin the tests compare against ──
if command -v wasm-opt >/dev/null 2>&1; then
    (cd "$fixtures" && wasm-opt --enable-exception-handling --enable-reference-types \
        --enable-multivalue --translate-to-exnref try_catch.wasm -o try_catch.exnref.wasm)
    say "  P4  --translate-to-exnref produced try_catch.exnref.wasm ($(wc -c < "$fixtures/try_catch.exnref.wasm") bytes)"
else
    skip "wasm-opt not found; try_catch.exnref.wasm stays as committed"
fi

# ── P3/P5/P8: what this engine does with each form ────────────────────────
if [ -x "$harness" ]; then
    if "$harness" --diagnose="$fixtures/try_catch.wasm" >/tmp/legacy-eh-p3.out 2>&1; then
        say "  P3  default               unexpectedly loaded the legacy form"
    else
        say "  P3  default               refused it: $(head -1 /tmp/legacy-eh-p3.out | cut -c1-80)..."
    fi
    if WASMJIT_LEGACY_EH=1 "$harness" --diagnose="$fixtures/try_catch.wasm" --invoke=pick --args=1 >/tmp/legacy-eh-p5.out 2>&1; then
        say "  P5  WASMJIT_LEGACY_EH=1   rewrite ran it: $(tail -1 /tmp/legacy-eh-p5.out)"
    else
        say "  P5  WASMJIT_LEGACY_EH=1   FAILED to run the rewrite"
    fi
    say "  P8  twin                  $("$harness" --diagnose="$fixtures/try_catch.exnref.wasm" --invoke=pick --args=1 2>&1 | tail -1)"
else
    skip "test harness not built at $harness"
fi

# ── P6/P9: what the current clang emits ───────────────────────────────────
if command -v clang++ >/dev/null 2>&1 && command -v wasm-tools >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    cat > "$tmp/eh.cpp" <<'CPP'
extern "C" void __cxa_throw(void*, void*, void (*)());
struct E { int x; };
int f(int v) { try { if (v) __cxa_throw(nullptr, nullptr, nullptr); return 1; } catch (E&) { return 2; } }
struct G { int x; ~G(); };
extern "C" void may_throw();
void cleanup() { G g; may_throw(); }
CPP
    clang++ --target=wasm32-unknown-unknown -fwasm-exceptions -O1 -c "$tmp/eh.cpp" -o "$tmp/eh.o" 2>/dev/null
    say "  P6  clang default         $(wasm-tools print "$tmp/eh.o" | grep -c 'try ' ) legacy try blocks, $(wasm-tools print "$tmp/eh.o" | grep -c 'rethrow') rethrow(s)"
    say "  P9  clang cleanup pad     $(wasm-tools print "$tmp/eh.o" | grep -c 'catch_all') catch_all arm(s) (destructor path)"
    clang++ --target=wasm32-unknown-unknown -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -O1 -c "$tmp/eh.cpp" -o "$tmp/eh_new.o" 2>/dev/null
    say "  P7  -wasm-use-legacy-eh=0 $(wasm-tools print "$tmp/eh_new.o" | grep -c 'try_table') try_table(s)"
    rm -rf "$tmp"
else
    skip "clang++/wasm-tools not found; P6/P9 not re-measured"
fi

say "legacy-eh-probe: done"
