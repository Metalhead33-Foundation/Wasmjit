# Function References (Typed Function References) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
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
- `ref.test`/`ref.cast` classify function references via the `Callable`'s
  `typeId` when the callable belongs to the current instance.
- Evidence: `call_indirect` 113/0, `func_ptrs` 23/0.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| FR1 | **`call_ref` segfaults** during compilation/instantiation (0 passing commands). | `spec: call_ref` → signal 11 | High |
| FR2 | **`ref_func.wast` produces no report** (uncaught exception on load/instantiate). | `spec: ref_func` | High |
| FR3 | **Cross-module function references are mis-classified.** `ref.test`/`ref.cast` scan only the current instance's `importStorage`/`internalCallables`, and the "unknown reference" fallback does not include `func` — so a `funcref` from another instance fails `ref.test funcref`. | `refMatchesHeapType`; [`GC.md`](GC.md) G2 | High |
| FR4 | **`ref.func` outside a segment is an unresolved sentinel** in `const` expressions (globals), so a `ref.func` global is a bogus pointer. | [`GC.md`](GC.md) G8 / [`EXTENDED_CONST.md`](EXTENDED_CONST.md) | Medium |
| FR5 | **`return_call_ref` aborts** (tail-call codegen). | [`TAILCALLS.md`](TAILCALLS.md) TC2 | High |
| FR6 | **Reference values cannot be stored generically.** Tables hold `Callable*` and globals hold `Value`; typed `(ref $t)` values live only on the operand stack. | `WasmTable.hpp`, `WasmModuleInstance.cpp` | Medium |
| FR7 | **No validation that `ref.func`'s function is declared** in an element segment, nor that `call_ref`'s operand type matches at validation time. | no validating front-end | Low |
| FR8 | **No manual test** for `call_ref`/typed references. | `Test/main.cpp` | Medium |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| FRD1 | **Keep `Callable` as the universal funcref** (code pointer + context + canonical type id). | §3: representation free. | One type for tables, `ref.func`, `call_ref`, `call_indirect`, imports. |
| FRD2 | **Type-check with canonical-id equality first, registry subtyping second** (already done). | §3: mechanism free; §2 only requires `match`. | Constant-time fast path, correct variance on the slow path. |
| FRD3 | **Represent nullability as the null pointer**; skip a separate tagged form. | §3; observable behaviour is all that matters. | `ref.is_null` is a pointer compare. |
| FRD4 | **Bake the callee `VMContext*` into direct calls**, load it for indirect/ref calls (see [`ABI.md`](ABI.md)). | §3; `Callable` carries the context. | Fast cross-module calls with no trampoline. |
| FRD5 | **Fix classification by giving every callable a discoverable `typeId`** (a header on the arena object, or a Store-global registry) instead of per-instance scans. | §3; closes FR3 and GC G2 together. | Correct cross-module `ref.test funcref`, O(1). |
| FRD6 | **Do not implement closures/environments.** | §2: a function reference is a typed function, no capture. | Scope control. |

Guardrails: `call_ref` must trap on null; the `typeId` used for matching must
be the canonical, process-wide identity (not the module-local `typeidx`);
`return_call_ref` inherits the [`TAILCALLS.md`](TAILCALLS.md) space guarantee.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **FR-1 — Fix `call_ref`** | Reproduce the SIGSEGV and correct the callable call path. | FR1 | `call_ref.wast` runs and shallow calls return correct values. |
| **FR-2 — Fix `ref_func`** | Reproduce the no-report failure. | FR2 | `ref_func.wast` runs. |
| **FR-3 — Discoverable callable type** | Give every `Callable` a header/registry entry so `refMatchesHeapType` needs no per-instance scan. | FR3 | A `funcref` produced by module B passes `ref.test funcref` in module A. |
| **FR-4 — `ref.func` in `const`** | Resolve the `ref.func` sentinel for global initialisers. | FR4 | A `(global funcref (ref.func $f))` holds a real reference. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `call_indirect` | 113 | 0 | 59 | indirect calls work |
| `func_ptrs` | 23 | 0 | 13 | `ref.func` in segments works |
| `call_ref` | — | SIGSEGV | — | FR1 |
| `ref_func` | — | no report | — | FR2 |
| `return_call_ref` | — | signal 6 | — | FR5 |

## 9. References

- Proposal overview (`WebAssembly/function-references`):
  https://github.com/WebAssembly/function-references/blob/master/proposals/function-references/Overview.md
- Spec — Change History (Release 3.0 "Function References"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Types (`(ref null? $t)`) and Instructions (`call_ref`):
  https://webassembly.github.io/spec/core/syntax/types.html
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/GC.md`](GC.md) §4.5/§4.7, G2/G8; [`docs/TAILCALLS.md`](TAILCALLS.md);
  [`docs/TYPE_IDENTITY.md`](TYPE_IDENTITY.md) (canonical `TypeId` and `match`);
  [`docs/ABI.md`](ABI.md) (call/context rules); sources
  `WasmBase/WasmValue.hpp` (`Callable`), `WasmBase/WasmModuleInstance.{hpp,cpp}`
  (`internalCallables`, `importStorage`, `refMatchesHeapType`),
  `WasmBase/WasmTypeRegistry.{hpp,cpp}` (`matchesHeap`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`emitCallableTypeCheck`,
  `dispatchCallThroughCallable`, `dispatchCallRef`), `Test/wast_supported.txt`.
