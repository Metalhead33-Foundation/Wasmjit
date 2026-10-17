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
| Module | type indices name function types; function/code count consistency; data-count consistency; start function `[] -> []`; duplicate export names and export index bounds; memory/table limits (min ≤ max, memory32 ≤ 65536 pages, shared memories declare a max, table32 fits in 32 bits); tag types have no results; global initialisers (const/null/ref.func/imported immutable `global.get`, extended-const arithmetic); active element/data targets; element function indices |
| Control | `block`/`loop`/`if`/`else`/`end` with typed params/results and exact stack height; `br`/`br_if`/`br_table`/`return`; `call`/`call_indirect`/`call_ref`; tail calls including result-type matching; `try_table` catch labels (payload, `exnref`, arity); `br_on_null`/`br_on_non_null`/`br_on_cast`/`br_on_cast_fail` |
| Numeric | i32/i64/f32/f64 unary, binary, comparison, conversion, sign-extension and saturating forms, with exact operand/result types |
| Memory | declared index type (`i32`/`i64`), alignment ≤ natural, 32-bit offset range, loads/stores, `memory.size/grow/init/copy/fill` |
| Table | table64 index type, `get/set/grow/size/fill/copy/init` |
| SIMD | 194 arithmetic/comparison/conversion shapes taken from `wasm_simd_ops.inc`, plus const/bitwise/splat/bitmask, loads/stores, lanes/shuffle/swizzle |
| Atomics | loads/stores/RMW/cmpxchg/wait/notify with access-size alignment and operand types |
| References | `ref.null`/`ref.func`/`ref.is_null`/`ref.eq`/`ref.as_non_null`; `call_ref $t` requires `(ref null $t)`; full subtyping (nullability, func/extern/exn/any hierarchies, concrete-type subtyping) |

## Conservative policy

An opcode without a rule (`ref.test`/`ref.cast`, the relaxed ternary SIMD
forms) sets `typeTainted`: the rest of the body is still scanned for immediate
checks, but the operand stack is no longer trusted and no type error is
reported there. A `try_table` **with catches** is tainted after its catch labels
are checked, because a catch injects its payload into an outer label, which the
linear stack does not model. A validator must never reject a module it could
not check — the cost is coverage, measured below, not correctness.

## Measured coverage

`WASM_SPEC_VALIDATE=1` on the curated set (2026-10-17):

| Kind | Binary cases | Rejected | Accepted |
| --- | ---: | ---: | ---: |
| `assert_invalid` | 1193 | **1193** | 0 |
| `assert_malformed` | 17 | 8 | 9 |
| `assert_unlinkable` | 32 | **32** | 0 |
| `assert_uninstantiable` | 0 | — | — |
| **total** | **1242** | **1233** | **9** |

The default suite is unchanged: **7879 assertions / 194 test cases, all
green**; only 4 of 194 scripts fail in the strict run, and all four are
decoder-level `assert_malformed` cases (`binary0`, `binary_leb128_64`, `custom`,
`global`) — no curated `assert_invalid` module is accepted.

The `atomic.wait`-on-unshared check is deliberately **not** enforced: the
curated `atomic.wast` treats it as valid (`atomic.notify` likewise), so `T5`
stays a documented single-agent deviation rather than a validator rule.

## Finding

`Module` did not initialise `dataSegmentCount`/`hasDataCount`; the data-count
rule read garbage and rejected valid modules until the constructor was fixed.
The bug was latent because nothing read those fields before.

## Next

- Type `ref.test`/`ref.cast` (still conservative) and the relaxed ternary SIMD
  forms.
- The “declared function reference” rule (`ref.func x` requires `x` to appear in
  an element segment) is not checked.
- The nine remaining strict failures are decoder-level `assert_malformed` cases.
- Add curated `assert_uninstantiable` coverage (the handler is already wired).
