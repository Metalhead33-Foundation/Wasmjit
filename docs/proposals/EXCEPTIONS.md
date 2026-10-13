# Exception Handling (with `exnref`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmModule`, `WasmType`, `WasmStore`,
`WasmVMContext`, `LibjitOpcodeDispatcher`, or the test harness.

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

"Exception Handling with `exnref`" (`exceptionsFinal`) is a **Phase 5**
proposal, integrated into WebAssembly **3.0**. It is the *final* shape of the
proposal: the legacy `try`/`catch`/`catch_all`/`rethrow`/`delegate`
instructions were **removed**.

- **Tags.** A tag section (and tag imports/exports) declares tags, each an
  attribute byte (`0x00`) plus a **function type**. A tag identifies an
  exception kind; its type is the payload.
- **Throwing**: `throw tagidx` (using the tag's payload operands) and
  `throw_ref` (re-throw an existing `exnref`).
- **Catching**: `try_table blocktype catch*`. Each catch clause is one of
  - `catch tagidx labelidx` — on a matching tag, branch to the label with the
    payload;
  - `catch_ref tagidx labelidx` — same, plus the `exnref`;
  - `catch_all labelidx` — any tag, no payload;
  - `catch_all_ref labelidx` — any tag, plus the `exnref`.
- **`exnref` value type.** Heap type `exn`, a subtype of `extern`.
  `(ref null exn)` is a first-class reference: it can be stored in tables and
  globals, returned, and compared; `throw_ref` re-throws it. On the web it is
  `WebAssembly.Exception`.
- **Tag identity matters**: a catch matches only the exact tag that was thrown.

## 3. What the proposal does not demand

- **No unwinding mechanism.** Table-driven unwinding, `setjmp`/`longjmp`,
  native exceptions, or CPS are all acceptable.
- **No performance contract.**
- **No representation requirement** for a tag or an `exnref`.
- **No requirement to interoperate with the host** for a non-web embedder;
  `WebAssembly.Tag`/`Exception` are JS-API surface.
- **No change to trap semantics**: a trap is not an exception.
- **No requirement that exceptions be catchable across Wasm/host boundaries**
  unless the embedder chooses to.

## 4. What we have implemented today

- **Parsing**: the tag section (`processTagSection`, `Tag { attribute;
  typeIdx }`), tag imports (`ImportTag`), and the `ExternalKind::Tag` export
  kind are all decoded.
- **Instructions**: `Opcode::Throw` (`0x08`), `Opcode::ThrowRef` (`0x0A`) and
  `Opcode::TryTable` (`0x1F`) are decoded; `CatchClause` supports
  `Catch`/`CatchRef`/`CatchAll`/`CatchAllRef` with a `tagIdx` and `labelIdx`,
  and `try_table` reads them.
- **But the runtime is a stub**:
  - `dispatchTryTable` simply calls `dispatchBlock` and **throws the catch
    clauses away**;
  - `dispatchThrow` and `dispatchThrowRef` emit an **unconditional trap**
    (`emitTrapUnreachable` + `markUnreachable`), so a `throw` never reaches a
    handler;
  - there is **no `exnref` value type** (`ValueTypeCode` has no `exn`), no tag
    runtime instance, and no tag storage (`Store::StoreOwnedTag` is still a
    forward declaration marked TODO);
  - `resolveTag` exists on the resolver but is **never called** by the
    instantiator (the same gap as imported globals, [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md)).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| EX1 | **`throw` and `throw_ref` are unconditional traps.** `throw.wast` → signal 6; `throw_ref.wast` → signal 11. No exception ever propagates. | `dispatchThrow`, `dispatchThrowRef` | High |
| EX2 | **`try_table` ignores all catch clauses.** It behaves as a plain block, so nothing catches; `try_table.wast` → signal 6. | `dispatchTryTable` | High |
| EX3 | **`exnref` does not exist as a value type.** `CatchRef`/`CatchAllRef` cannot push the exception reference; `throw_ref`/`ref.null exn` have no representation. | `WasmType.hpp` (`ValueTypeCode`, `AbstractHeapType`) | High |
| EX4 | **No tag instances or identity.** Tags are parsed but never instantiated; `resolveTag` is never called; matching by tag identity is impossible. | `Store::StoreOwnedTag`, `resolveTag` | High |
| EX5 | **Exnrefs cannot be stored.** Tables hold `Callable*`; there is no place for an `exnref` in a table or (aliased) global. | `WasmTable.hpp`, `WasmModuleInstance.cpp` | Medium |
| EX6 | **The harness skips `assert_exception`.** No exception behaviour is verified. | `docs/TESTING.md` | High (verification) |
| EX7 | **No manual test.** | `Test/main.cpp` | Medium |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| EXD1 | **Implement unwinding with `setjmp`/`longjmp`** to a per-invocation handler chain, rather than a table-driven personality routine. | §3: the mechanism is free. This is the QVM/`Sys_Error` pattern. | Minimal machinery; a `throw` is a jump to the nearest `try_table` frame. |
| EXD2 | **Keep a single `VMContext`-reachable handler stack** pushed/popped by `try_table` entry/exit. | §3; the handler set is naturally stack-shaped. | O(1) throw, no metadata tables. |
| EXD3 | **Represent an `exnref` as a hunk pointer** to `{ tagId, payload }` in the GC region ([`GC.md`](GC.md) §6.1). | §3: representation free; `exnref` is opaque. | Reuses the arena and GC object model; `throw_ref` is a re-jump. |
| EXD4 | **Intern tags to a process-wide `TagId`**, exactly like `TypeId` ([`TYPE_IDENTITY.md`](TYPE_IDENTITY.md)), and match by id. | §2 requires exact tag identity; a canonical id is one implementation. | Cheap, cross-module-correct matching. |
| EXD5 | **Interpret the `catchop` flags directly** (`catch_ref`/`catch_all_ref`) so the label arity is known at compile time. | §2. | No runtime decision about whether to push the `exnref`. |
| EXD6 | **Do not implement `WebAssembly.Tag`/`Exception`.** | §3: web-only. | Scope control. |

**Guardrails.**

- A trap must remain a trap; do not let `longjmp` turn a genuine trap into a
  catchable exception (or vice versa).
- `longjmp` must not skip live C++ objects with destructors. The JIT-emitted
  frames and the native helpers used here are POD-only; keep it that way, and
  document any helper that allocates.
- `catch_all_ref` must push the *same* `exnref` that `throw_ref` would re-throw.
- `throw` with no enclosing `try_table` must trap (uncaught exception).

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **EX-1 — `exnref` type** | Add `exn`/`exnref` to the type system and the `0x69` shorthand, plus `ref.null exn`. | EX3, EX5 (partly) | A module declaring an `exnref` global/param loads. |
| **EX-2 — Tags** | Instantiate tags (a process-wide `TagId`), wire `resolveTag`, and expose tag identity to the JIT. | EX4 | Two modules importing the same tag compare equal; different tags do not. |
| **EX-3 — Throw/unwind** | Implement `throw`/`throw_ref` with the handler stack (`setjmp`/`longjmp`). | EX1 | A caught `throw` reaches the right `catch` clause with the right payload. |
| **EX-4 — `try_table`** | Emit handler entry/exit for `try_table` and route each catch clause, including `*_ref`. | EX2 | `try_table.wast` runs; `catch_ref` pushes the `exnref`. |
| **EX-5 — Harness/tests** | Support `assert_exception`; add a manual throw/catch test. | EX6, EX7 | Exception results are verified. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | Result | Reading |
|---|---|---|
| `tag` | 6 passed / 0 failed / 4 skipped | tag parsing/validation works |
| `throw` | signal 6, 0 passing commands | EX1 |
| `throw_ref` | signal 11, 0 passing commands | EX1 |
| `try_table` | signal 6, 0 passing commands | EX2 |

`tag` is in `Test/wast_supported.txt`; the other three are not.

## 9. References

- Proposal overview (`WebAssembly/exception-handling`, final / `exnref`):
  https://github.com/WebAssembly/exception-handling/blob/main/proposals/exception-handling/Exceptions.md
- Spec — Change History (Release 3.0 "Exception Handling"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Instructions (`try_table`, tags) and Types (`exnref`):
  https://webassembly.github.io/spec/core/syntax/instructions.html
  https://webassembly.github.io/spec/core/syntax/types.html
- In-repo: [`docs/GC.md`](GC.md) (hunks, `TypeId`), [`docs/MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md)
  (the `resolveTag`/`resolveGlobal` pattern), [`docs/FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md)
  (reference types), [`docs/ABI.md`](ABI.md);
  sources `WasmBase/WasmType.hpp` (`Tag`, `ImportTag`, `CatchClause`),
  `WasmBase/WasmModule.cpp` (`processTagSection`),
  `WasmBase/WasmOpcode.hpp` (`Throw`, `ThrowRef`, `TryTable`),
  `WasmBase/WasmImport.hpp` (`resolveTag`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchThrow`, `dispatchThrowRef`,
  `dispatchTryTable`), `Test/wast_supported.txt`.
