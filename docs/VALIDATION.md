# Validation

`WasmBase/WasmValidator.cpp` is the validating front end. `Module::fromFile`
calls `validate()` after decoding, so a module is checked once and every
consumer (the JIT, the stub backend, an embedder) sees the same rejection.
`Module::validate()` is also public.

## Architecture

The validator is an `OpcodeDispatcher`. The existing dispatcher already reads
every instruction and immediate (including the multi-byte prefixes), so a rule
never re-parses bytes — it is just one `dispatch*` override. `readCode` drives
the function body; the validator keeps an operand type stack and a control-frame
stack, and throws `std::runtime_error` on the first failed rule.

## What is checked

| Layer | Rules |
| --- | --- |
| Module | type indices name function types; function/code count consistency; data-count consistency; start function `[] -> []`; duplicate export names and export index bounds; memory/table limits (min ≤ max, memory32 ≤ 65536 pages, shared memories declare a max, table32 fits in 32 bits); tag types have no results; global/table initialisers and active element/data offsets (typed constant expressions); element/table type agreement, element item expressions and table initialiser typing; selected functions in element segments; empty modules |
| Control | `block`/`loop`/`if`/`else`/`end` with typed params/results and exact stack height; `br`/`br_if`/`br_table` (operand values checked against every target; the unreachable stack is polymorphic)/`return`; `call`/`call_indirect`/`call_ref`; tail calls including result-type matching; `try_table` catch labels (payload, `exnref`, arity); `br_on_null`/`br_on_non_null`/`br_on_cast`/`br_on_cast_fail` |
| Numeric | i32/i64/f32/f64 unary, binary, comparison, conversion, sign-extension and saturating forms, with exact operand/result types |
| Memory | declared index type (`i32`/`i64`), alignment ≤ natural, 32-bit offset range, loads/stores, `memory.size/grow/init/copy/fill` |
| Table | table64 index type, `get/set/grow/size/fill/copy/init`; table initialiser typing; `table.init` element-segment/table type match |
| SIMD | 194 arithmetic/comparison/conversion shapes taken from `wasm_simd_ops.inc`, plus const/bitwise/splat/bitmask, loads/stores, lanes/shuffle/swizzle, and the relaxed ternary forms (madd/nmadd, laneselect, dot-add) |
| Atomics | loads/stores/RMW/cmpxchg/wait/notify with access-size alignment and operand types |
| References | `ref.null`/`ref.func`/`ref.is_null`/`ref.eq`/`ref.as_non_null`; `ref.func x` requires `x` to be declared outside function bodies/start; `call_ref $t` requires `(ref null $t)`; `ref.test`/`ref.cast` (both nullability forms) require the operand to be a reference in the target's hierarchy; full subtyping (nullability, func/extern/exn/any hierarchies, concrete-type subtyping and canonical structural type equivalence); untyped `select` is numeric/vector only; definite initialization of non-defaultable locals |

## Conservative policy

An opcode without a rule (parts of the GC aggregate/array surface) sets
`typeTainted`: the rest of the body is still scanned for immediate checks, but
the operand stack is no longer trusted and no type error is reported there. A
`try_table` **with catches** is tainted after its catch labels are checked,
because a catch injects its payload into an outer label, which the linear stack
does not model. A validator must never reject a module it could not check — the
cost is coverage, measured below, not correctness.

## Measured coverage

`WASM_SPEC_VALIDATE=1` on the curated set (2026-10-18):

| Kind | Binary cases | Handled | Accepted |
| --- | ---: | ---: | ---: |
| `assert_invalid` | 2211 | **2211** rejected | 0 |
| `assert_malformed` | 17 | **17** rejected | 0 |
| `assert_unlinkable` | 127 | **127** rejected | 0 |
| `assert_uninstantiable` | 27 | **27** (26 rejected + 1 start trap) | 0 |
| **total** | **2382** | **2382** | **0** |

The default suite is **8477 assertions / 283 test cases, all green** (61 core
scripts were re-curated after the validator landed, plus `start.wast`). The
strict run is **fully green** as well: every one of the 2,382 binary negative
commands is handled, including all 17 `assert_malformed` cases and the strict
LEB128 width/overflow cases. All 27 `assert_uninstantiable` cases pass: one
traps in a start function (`start.wast`), the other 26 fail instantiation with
a caught error (`elem`/`data` out-of-bounds segments). 946 text-format commands
remain skipped because the loader is binary-only.

The `atomic.wait`-on-unshared check is deliberately **not** enforced: the
curated `atomic.wast` treats it as valid (`atomic.notify` likewise), so `T5`
stays a documented single-agent deviation rather than a validator rule.

## Finding

`Module` did not initialise `dataSegmentCount`/`hasDataCount`; the data-count
rule read garbage and rejected valid modules until the constructor was fixed.
The bug was latent because nothing read those fields before.

The active-segment path widened a 32-bit offset by sign extension (`i32.const
-1` became 0xFFFF_FFFF_FFFF_FFFF) and added the length before the bounds check,
so an out-of-bounds `data`/`elem` segment wrapped to a small index and wrote
past the backing array. `elem.wast` exposed it as a double free at teardown.
Offsets are now zero-extended for 32-bit memories/tables (and `i64` for 64-bit
ones), and the bounds check subtracts instead of adding.

## Next

- Type the remaining GC aggregate/array forms (the remaining `typeTainted`
  paths).
- Adopt and test the relaxed dead-code algorithm described in
  [`proposals/PARSER_PROPOSALS.md`](proposals/PARSER_PROPOSALS.md); the current
  unreachable stack is close but not claimed as the proposal's algorithm.
- ~~The “declared function reference” rule~~ — done: `dispatchRefFunc` checks the
  module's `C.refs` set (exports, globals, tables and element segments).
- ~~The remaining strict failures~~ — done: strict LEB128 width/overflow
  checking in the shared decoder rejects all 17 curated malformed modules.
- ~~Add curated `assert_uninstantiable` coverage~~ — done: `start.wast` and the
  `elem`/`data` segment cases are verified under `WASM_SPEC_VALIDATE=1`.
