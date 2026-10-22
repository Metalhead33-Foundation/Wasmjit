# Function References (Typed Function References) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 5 — FR-1…FR-8 are
closed: typed references are stored in typed tables and globals, `call_ref` /
`return_call_ref` consume them, and the manual fixture exercises both the
table and global paths).
Audience: anyone touching `WasmValue` (`Callable`), `WasmTypeSystem`,
`WasmModuleInstance`, `LibjitOpcodeDispatcher`, or the test harness.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the proposal require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

Typed Function References is a **Phase 5** proposal, integrated into
WebAssembly **3.0** (it is the prerequisite the GC proposal builds on).

- **Typed function references.** A reference type may name a concrete
  function type: `(ref null? $t)` where `$t` is a `typeidx` of a `func` type.
  `funcref` is `(ref null func)`.
- **`ref.func funcidx`** yields a typed function reference; a function may
  only be referenced if it is declared in an element segment (active,
  passive, or declarative).
- **`call_ref typeidx`** calls a typed function reference, requiring it to be
  non-null and to have a type that **matches** the instruction's type.
- **`return_call_ref typeidx`** is the tail-call form.
- **Subtyping** applies to function types: parameter contravariance, result
  covariance, so a function of a subtype may be used where a supertype is
  expected. `ref.test`/`ref.cast`/`br_on_cast` therefore work on function
  references too.
- The typed-function-references proposal introduced the basic reference
  instructions that the reference-types proposal shares: `ref.as_non_null`,
  `br_on_null`, `br_on_non_null` (plus `ref.null`/`ref.is_null`/`ref.eq`).

## 3. What the proposal does not demand

- **No representation requirement.** A function reference may be a code
  pointer, a table index, a closurestruct, or a tagged pair (code + context).
- **No specific type-check mechanism** for `call_ref`; a canonical type id, a
  vtable, or a runtime type descriptor are all acceptable.
- **No requirement that `funcref` be a raw code pointer.**
- **No JS API requirement** for a non-web embedder.
- **No requirement on how nullability is represented** as long as
  `ref.is_null`/`ref.as_non_null`/... are observable.
- **No closure/environment semantics**; a function reference is just a typed
  function, not a closure over Wasm state.

## 4. What we have implemented today

- `Callable` (`WasmValue.hpp`) is the universal function reference:
  `fnPtr`, `context` (the callee's `VMContext*` or `nullptr` for native),
  `localTypeIdx`, and a canonical `typeId`. It is what tables store and what
  `ref.func`/`call_indirect`/`call_ref` operate on.
- `ref.func` pushes a `Callable*` (`callablePointerForFuncIndex`) typed as the
  function's reference type.
- `call_ref`/`return_call_ref` are decoded and dispatch through
  `dispatchCallThroughCallable(callablePtr, typeIdx)`.
- **Type checking** is `emitCallableTypeCheck`: compare the callable's
  `typeId` against the canonical `TypeId` of the instruction's `typeidx`
  (fast path), then a registry subtyping lookup
  (`wasm_callable_type_matches`) for the contravariant/covariant case.
- `ref.as_non_null`, `br_on_null`, `br_on_non_null` are implemented from the
  same reference-instruction family.
- `ref.test`/`ref.cast` classify a function reference through the Store's
  callable-range registry: every `Callable` carries its canonical `typeId`, and
  a reference from any instance is identified by the range test — no
  per-instance pointer scan (`GC.md` GC-1/H3).
- Evidence: `call_indirect` 113/0, `func_ptrs` 23/0.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~FR1~~ | ~~`call_ref` segfaults.~~ **Done (trap sink):** the null/invalid callable check in `dispatchCallThroughCallable` now traps (`emitTrapIfNull`) instead of dereferencing. `spec: call_ref` → **31 passed / 0 failed / 4 skipped**. | `spec: call_ref` | Closed |
| ~~FR2~~ | ~~`ref_func.wast` produces no report.~~ **Done (trap sink):** the fatal path was a reference reaching a LibJIT null check; it is now a recoverable trap. `spec: ref_func` → **14 passed / 0 failed / 3 skipped**. | `spec: ref_func` | Closed |
| ~~FR3~~ | ~~**Cross-module function references are mis-classified.**~~ **Done (`GC-1`):** every `Callable` carries its canonical `typeId`, and `Store::callableAt` identifies a reference by a range test over the Store's registered callable ranges (one per instance), so a `funcref` produced by module B is classified correctly in module A. | `call_ref` 35/0, `ref_func` 17/0, `ref_test` 71/0 | Closed |
| ~~FR4~~ | ~~**`ref.func` outside a segment is an unresolved sentinel** in `const` expressions (globals), so a `ref.func` global is a bogus pointer.~~ **Done (`GC.md` G8):** `resolveGlobalRefFuncs()` rewrites the sentinel after compilation, so a `(global funcref (ref.func $f))` holds a real reference. | `global`, `table` (exported `(ref $f)` global across modules) | Closed |
| ~~FR5~~ | ~~`return_call_ref` aborts~~ (tail-call codegen). **Done (TAILCALLS TC-1/TC-2 + trap sink):** `spec: return_call_ref` → **40 passed / 0 failed / 13 skipped**. | [`TAILCALLS.md`](TAILCALLS.md) | Closed |
| ~~FR6~~ | ~~**Reference values cannot be stored generically.**~~ **Done (FR-6):** table slots are pointer-sized reference words behind the funcref `Callable**` view, and globals hold `Value` cells; typed `(ref null $t)` values therefore survive `table.set/get` and `global.set/get`. The manual fixture calls through both a typed table and a typed global. | `WasmTable.hpp`, `WasmModuleInstance.cpp`, `Test/wasm_wat/function_references.wat` | Closed |
| ~~FR7~~ | ~~**Validation is partial.**~~ **Done (FR-7):** the validator types `call_ref`'s operand/results, `br_on_*`, and the “declared function reference” rule (`dispatchRefFunc` requires `x` in `declaredFunctions`); the strict run is green. | `WasmValidator.cpp` | Closed |
| ~~FR8~~ | ~~**No manual test** for `call_ref`/typed references.~~ **Done (FR-8):** `function_references.wat` stores a typed `ref.func` in a table and a mutable global, calls through both, and exercises `return_call_ref`. | `Test/main.cpp`, `Test/wasm_wat/function_references.wat` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| FRD1 | **Keep `Callable` as the universal funcref** (code pointer + context + canonical type id). | §3: representation free. | One type for tables, `ref.func`, `call_ref`, `call_indirect`, imports. |
| FRD2 | **Type-check with canonical-id equality first, registry subtyping second** (already done). | §3: mechanism free; §2 only requires `match`. | Constant-time fast path, correct variance on the slow path. |
| FRD3 | **Represent nullability as the null pointer**; skip a separate tagged form. | §3; observable behaviour is all that matters. | `ref.is_null` is a pointer compare. |
| FRD4 | **Bake the callee `VMContext*` into direct calls**, load it for indirect/ref calls (see [`ABI.md`](../ABI.md)). | §3; `Callable` carries the context. | Fast cross-module calls with no trampoline. |
| FRD5 | **Fix classification by giving every callable a discoverable `typeId`** (a header on the arena object, or a Store-global registry) instead of per-instance scans. | §3; closes FR3 and GC G2 together. | Correct cross-module `ref.test funcref`, O(1). |
| FRD6 | **Do not implement closures/environments.** | §2: a function reference is a typed function, no capture. | Scope control. |

Guardrails: `call_ref` must trap on null; the `typeId` used for matching must
be the canonical, process-wide identity (not the module-local `typeidx`);
`return_call_ref` inherits the [`TAILCALLS.md`](TAILCALLS.md) space guarantee.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **FR-1 — Fix `call_ref`** | ✅ The crash was a null/invalid callable pointer being dereferenced; the check now goes through `emitTrapIfNull` (the shared trap sink). | FR1 | Done: `call_ref.wast` runs (35/0/0). |
| **FR-2 — Fix `ref_func`** | ✅ Same root cause; the reference reaches the trap sink. | FR2 | Done: `ref_func.wast` runs (17/0/0). |
| **FR-3 — Discoverable callable type** | ✅ Done (`GC-1`): every `Callable` carries its canonical `TypeId`; `Store::callableAt` identifies a reference by a range test over the Store's registered callable ranges, so no per-instance scan is needed. | FR3 | Done: `ref_test` 71/0 — a `funcref` produced by module B is classified in module A. |
| **FR-4 — `ref.func` in `const`** | ✅ Done (`GC.md` G8): `resolveGlobalRefFuncs()` resolves the sentinel after compilation. | FR4 | Done: a `(global funcref (ref.func $f))` holds a real reference (`table` 18/0 exercises it across modules). |
| **FR-7 — Reference validation** | ✅ The validator types `call_ref`, `br_on_*` and the declared-function rule. | FR7 | Done: the strict run is green, including the `ref_func`/`call_ref` negatives. |
| **FR-6 — Generic typed storage** | ✅ Typed references survive `table.set/get` and `global.set/get`; `call_ref`/`return_call_ref` consume them. | FR6 | `function references: typed refs survive in tables and globals` passes (10 assertions). |
| **FR-8 — Manual test** | ✅ `Test/wasm_wat/function_references.wat` covers the typed table, typed global, `call_ref` and `return_call_ref`. | FR8 | The manual test is in the default suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `call_indirect` | 137 | 0 | 35 | indirect calls work (was 113/0/59) |
| `func_ptrs` | 29 | 0 | 7 | `ref.func` in segments works |
| `call_ref` | 35 | 0 | 0 | strict run |
| `ref_func` | 17 | 0 | 0 | strict run |
| `return_call_ref` | 53 | 0 | 0 | strict run |
| `ref_test` | 71 | 0 | 0 | FR3/GC extern boxes; strict run |
| `function_references.wat` | 10 assertions | 0 | 0 | typed table + typed global + `call_ref`/`return_call_ref` |

## 9. References

- Proposal overview (`WebAssembly/function-references`):
  https://github.com/WebAssembly/function-references/blob/master/proposals/function-references/Overview.md
- Spec — Change History (Release 3.0 "Function References"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Types (`(ref null? $t)`) and Instructions (`call_ref`):
  https://webassembly.github.io/spec/core/syntax/types.html
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/GC.md`](GC.md) §4.5/§4.7, G2/G8; [`docs/TAILCALLS.md`](TAILCALLS.md);
  [`docs/TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) (canonical `TypeId` and `match`);
  [`docs/ABI.md`](../ABI.md) (call/context rules); sources
  `WasmBase/WasmValue.hpp` (`Callable`), `WasmBase/WasmModuleInstance.{hpp,cpp}`
  (`internalCallables`, `importStorage`, `refMatchesHeapType`),
  `WasmBase/WasmTypeRegistry.{hpp,cpp}` (`matchesHeap`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`emitCallableTypeCheck`,
  `dispatchCallThroughCallable`, `dispatchCallRef`), `Test/wast_supported.txt`.
