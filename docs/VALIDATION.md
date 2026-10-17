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
| Control | `block`/`loop`/`if`/`else`/`end` with typed params/results; `br`/`br_if`/`br_table`/`return`; `call`/`call_indirect`/`call_ref`; tail calls including result-type matching; `try_table` block shape and tag indices |
| Numeric | i32/i64/f32/f64 unary, binary, comparison, conversion, sign-extension and saturating forms, with exact operand/result types |
| Memory | declared index type (`i32`/`i64`), alignment ≤ natural, 32-bit offset range, loads/stores, `memory.size/grow/init/copy/fill` |
| Table | table64 index type, `get/set/grow/size/fill/copy/init` |
| SIMD | 194 arithmetic/comparison/conversion shapes taken from `wasm_simd_ops.inc`, plus const/bitwise/splat/bitmask, loads/stores, lane and shuffle indices |
| Atomics | loads/stores/RMW/cmpxchg/wait/notify with access-size alignment and operand types |
| References | `ref.null`/`ref.func`/`ref.is_null`/`ref.eq`/`ref.as_non_null`; `call_ref` accepts any function reference |

## Conservative policy

An opcode without a rule (GC `br_on_cast`/`ref.test`, `try_table` catch-label
injection, relaxed ternary SIMD forms) sets `typeTainted`: the rest of the body
is still scanned for immediate checks, but the operand stack is no longer
trusted and no type error is reported there. A validator must never reject a
module it could not check — the cost is coverage, measured below, not
correctness.

## Measured coverage

`WASM_SPEC_VALIDATE=1` on the curated set (2026-10-17):

| Kind | Binary cases | Rejected | Accepted |
| --- | ---: | ---: | ---: |
| `assert_invalid` | 1153 | **1086** | 67 |
| `assert_malformed` | 13 | 8 | 5 |
| `assert_unlinkable` | 32 | **32** | 0 |
| `assert_uninstantiable` | 0 | — | — |
| **total** | **1198** | **1126** | **72** |

The default suite is unchanged: **7866 assertions / 191 test cases, all
green**; only 20 of 191 scripts fail in the strict run (down from 86 when the
measurement started at 4/1153).

Remaining accepted `assert_invalid` clusters: `if` edge rules (12),
`try_table` catch-label typing (9), `return_call_ref` (8), and the reference/GC
rules `ref_eq`, `ref`, `br_on_cast`, `br_on_cast_fail`, `br_on_null`,
`br_on_non_null` (21), plus a handful of table/SIMD/alignment cases.

## Finding

`Module` did not initialise `dataSegmentCount`/`hasDataCount`; the data-count
rule read garbage and rejected valid modules until the constructor was fixed.
The bug was latent because nothing read those fields before.

## Next

- Type the reference/GC branch and cast rules (`br_on_cast`, `ref.test`/`ref.cast`,
  `br_on_null`) and `try_table` catch-label injection.
- Type the relaxed ternary SIMD forms.
- Add curated `assert_uninstantiable` coverage (the handler is already wired).
