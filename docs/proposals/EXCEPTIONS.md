# Exception Handling (with `exnref`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — the runtime is
**complete**: EX-2 tags (process-wide `TagId`, `resolveTag`, payload typing),
EX-3 `throw`/`throw_ref` (Store-owned exception objects, pending-exception
protocol, static cross-frame dispatch), EX-4 `try_table` catch routing
(including `*_ref`) and EX-5 verification (`assert_exception`, a manual
test, three curated scripts). `throw` **10/0**, `throw_ref` **13/0**,
`try_table` **56/0**. What remains is `exnref` storage in tables (shared with
`FUNCTION_REFERENCES.md` FR-6) and `try_table` catch-label typing in the
validator; the rest of the instruction set is validated.
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
- **`exnref` value type.** Heap type `exn`, a first-class reference that is
  *its own top-level heap type* — it is **not** a subtype of `any` or `extern`
  (verified against the spec with `wasm-tools validate`: `exnref → externref`
  and `exnref → anyref` are both invalid, while `nullexnref → exnref` is
  valid). `(ref null exn)` can be stored in tables and globals, returned, and
  compared; `throw_ref` re-throws it. On the web it is
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
- **`exnref` is modelled (EX-1).** `ValueTypeCode::ExnRef` (0x69) and
  `NullExnRef` (0x74), `AbstractHeapType::Exn` (0x69) and `NoExn` (0x74); any
  function type/global/local/`ref.null`/block type that names them lowers to a
  `void*`, exactly like the other references. The bottom abstract heap types
  (`none`/`nofunc`/`noextern`/`noexn`) were re-numbered to the final 3.0
  encodings (0x71/0x73/0x72/0x74) — the old GC-draft values collided with
  `exn` at 0x69. `ref_null.wast` (34 assertions across `exnref`,
  `nullexnref` and every other null form) runs green and is curated; the
  harness verifies `(ref.null …)` *results* against a null pointer.
- **Tags have a process-wide identity (EX-2).** `Store::createTag(TypeId)`
  assigns a `TagId`; `ModuleInstanceInternals::tagIds` is the module's tag index
  space (imports first, then its own tags), filled before compilation.
  `resolveTag` is now called by the instantiator, aliases the exporting tag's
  `TagId`, and rejects a payload type that does not match the declaration;
  `registerExports` publishes the `TagId`, not the module-local index.
- **`throw`/`throw_ref` execute (EX-3).** An `exnref` is a Store-owned
  exception object: a 16-byte header `{TagId, payloadBytes}` followed by the
  payload, laid out with `translateStruct(tag params)` (the same offsets the
  multi-value ABI uses). `throw tagidx` finds its handler **statically** in the
  enclosing `try_table` frames (the tag is a compile-time index) and branches to
  the catch label with the payload (plus the `exnref` for `*_ref`);
  `throw_ref` emits a comparison chain over the same frames because its tag id
  is dynamic. With no handler the exception is left in a Store-shared *pending*
  slot and the function returns zeroes; every call site checks the slot inline
  and either dispatches to a local catch or re-propagates. A caught exception
  clears the slot. Traps remain traps (they longjmp to `TrapScope` and are never
  caught).
- **`try_table` routes its catches (EX-4).** The instruction pushes a
  `ControlBlock::TryTable` frame carrying the decoded clauses; the frame behaves
  like a block at its `end`. Catch labels are **parent-relative** (encoded label
  0 is the block enclosing the `try_table`, verified in the binary), so a catch
  target is `controlStack[frameIndex - 1 - clause.labelIdx]`.
- **Verification (EX-5).** The harness executes `assert_exception` (the
  invocation returns normally with a pending exception), rejects a trap where an
  exception is expected and vice versa, and clears stale state before each
  invocation. `Test/wasm_wat/exceptions.wat` covers a caught payload and the
  uncaught (embedder-visible) case. `throw`, `throw_ref` and `try_table` are
  curated.
- **Still absent:** `exnref` cannot be stored in a **table** (a slot is a
  `Callable*`; the generic-slot gap, `EX5`/FR-6). Payload references are retained
  forever because exception objects live until Store teardown — a deliberate,
  bounded lifetime choice.
- **Known limitation (tail calls):** a `return_call` lowered to the trampoline
  replaces the frame, so the tail-calling function's `try_table`s are no longer
  active for the callee's exceptions (they propagate to the caller's caller).
  The non-trampoline fallback (`call` + `return`) keeps them active. The spec
  suite does not exercise this corner; a future tail-call-aware handler model
  would close it.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~EX1~~ | ~~**`throw` and `throw_ref` are unconditional traps.**~~ **Done (EX-3):** both execute; `throw` dispatches statically to the innermost matching catch, `throw_ref` compares tag ids at runtime, and an unhandled throw becomes a pending exception that every call site re-propagates. | `throw` 10/0, `throw_ref` 13/0 | Closed |
| ~~EX2~~ | ~~**`try_table` ignores all catch clauses.**~~ **Done (EX-4):** the frame carries its decoded clauses, and every throw site routes the payload (and the `exnref` for `*_ref`) to the catch label. | `try_table` 56/0 | Closed |
| ~~EX3~~ | ~~**`exnref` does not exist as a value type.**~~ **Done (EX-1):** `ExnRef` (0x69) and `NullExnRef` (0x74) are first-class value types at every use site (function signature, global, local, `ref.null`, block type), the `exn`/`noexn` heap types are decoded, and the JIT lowers them to `void*`. | `ref_null` 34/0/0 | Closed |
| ~~EX4~~ | ~~**No tag instances or identity.**~~ **Done (EX-2):** every tag declaration gets a Store-owned `TagId`; imports alias the exporter's id; imported payload types are checked; exports publish the id. | `tag` 6/0; the tag-identity manual test | Closed |
| EX5 | **Exnrefs cannot be stored in tables.** Globals hold a `Value` (so an `exnref` global works, EX-1), but a table slot is a `Callable*` and there is no place for a non-funcref reference — the same generic-slot gap as `FUNCTION_REFERENCES.md` FR-6. See `docs/ABI.md` §Reference and Global Representation (R2). | `WasmTable.hpp`, `WasmModuleInstance.cpp` | Medium (shared with FR-6) |
| ~~EX6~~ | ~~**The harness skips `assert_exception`.**~~ **Done (EX-5):** `assert_exception` executes; it fails when the call traps or returns without a pending exception, and `assert_return`/`assert_trap`/plain actions reject a pending exception. | `Test/WastScript.cpp` | Closed |
| ~~EX7~~ | ~~**No manual test.**~~ **Done (EX-5):** `Test/wasm_wat/exceptions.wat` + `exceptions: a caught throw carries its payload and an uncaught one stays pending`. | `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| EXD1 | **Unwind with a Store-owned *pending-exception* state plus per-call-site static dispatch**, not `setjmp`/`longjmp`. `throw`/`throw_ref` that find a handler in the current function branch directly to its catch label; otherwise they set the pending slot and return zeroes. Every call site checks the slot inline; on a pending exception it runs the same static dispatch for its own enclosing `try_table`s, or re-propagates. | §3: the mechanism is free. This is also the only workable mechanism here: `setjmp` cannot be armed in a JIT frame, and a helper's `jmp_buf` points at a dead frame, so `longjmp` cannot resume at a catch label. A Store-owned slot is shared by every module in the store. | Constant-time handler lookup for `throw` (the tag is a compile-time index), one inline load + branch per call, no unwind tables, no personality routine, no change to the value ABI. |
| EXD2 | **Resolve handlers statically where possible.** `throw tagidx` searches the enclosing `try_table` frames (innermost first) for a matching catch at compile time; `throw_ref` emits a comparison chain over the same frames because its tag id is dynamic. Traps are never catchable: they still `longjmp` to the embedder's `TrapScope`. | §3: the handler set is lexically known. The spec requires a trap *not* to be an exception (`try_table.wast` asserts this with `unreachable` and `divide by zero`). | No handler stack at run time; `catch` arity and payload offsets are constants; traps and exceptions stay disjoint. |
| EXD3 | **Represent an `exnref` as a hunk pointer** to `{ tagId, payload }` in the GC region ([`GC.md`](GC.md) §6.1). | §3: representation free; `exnref` is opaque. | Reuses the arena and GC object model; `throw_ref` is a re-jump. |
| EXD4 | **Intern tags to a process-wide `TagId`**, exactly like `TypeId` ([`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)), and match by id. | §2 requires exact tag identity; a canonical id is one implementation. | Cheap, cross-module-correct matching. |
| EXD5 | **Interpret the `catchop` flags directly** (`catch_ref`/`catch_all_ref`) so the label arity is known at compile time. | §2. | No runtime decision about whether to push the `exnref`. |
| EXD6 | **Do not implement `WebAssembly.Tag`/`Exception`.** | §3: web-only. | Scope control. |

**Guardrails.**

- A trap must remain a trap; do not let the pending-exception protocol turn a
  genuine trap into a catchable exception (or vice versa). `try_table` never
  catches the `TrapScope` path, which `try_table.wast` asserts with
  `unreachable` and `divide by zero`.
- A caught exception must **clear the pending slot** before branching, or the
  caller (and the embedder) still observes it as uncaught.
- Exception helpers and the generated catch path are POD-only; the helpers trap
  (`longjmp`) rather than throwing a C++ exception through JIT frames.
- The exception object's payload uses the multi-value tuple layout
  (`translateStruct(tag params)`); a payload reference is retained, and objects
  live until Store teardown.
- `catch_all_ref` must push the *same* `exnref` that `throw_ref` would re-throw.
- `throw` with no enclosing `try_table` must leave the exception pending (the
  embedder reports it as uncaught).

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **EX-1 — `exnref` type** | ✅ Done: `ValueTypeCode::ExnRef`/`NullExnRef` and `AbstractHeapType::Exn`/`NoExn`, the bottom-type encodings corrected to the final 3.0 values, JIT lowering to `void*`, and `(ref.null ht)` naming the hierarchy in its `Value::kind`. The harness now verifies `(ref.null …)` results. | EX3 | Done: every `ref_null.wast` module loads and the script is **34/0/0**, curated. |
| **EX-2 — Tags** | ✅ Done: `StoreOwnedTag` + `Store::createTag`, a `tagIds` index space filled before compilation (imports first), `resolveTag` wired into instantiation (aliasing the exporter's id and checking the payload type), and `registerExports` publishing the `TagId`. | EX4 | Done: two modules importing the same tag share an id, a second tag with the identical payload type does not, and a payload-type mismatch fails to link (`tag` 6/0; manual identity test). |
| **EX-3 — Throw/unwind** | ✅ Done: the pending-exception protocol (`EXD1`/`EXD2`) — Store-owned exception objects with a tuple-laid-out payload, static catch dispatch in the current function, an inline pending check at every call site, and static cross-frame propagation. | EX1 | Done: `throw` 10/0, `throw_ref` 13/0 (including a throw across a call boundary and `catch_ref`/`throw_ref`). |
| **EX-4 — `try_table`** | ✅ Done: a `ControlBlock::TryTable` frame carries the clauses; catch labels resolve to the try_table's own frame; the payload and the `exnref` for `*_ref` are stored into the target label's slots. | EX2 | Done: `try_table` **56/0**; the two `assert_trap`s (`unreachable`, divide-by-zero) still trap rather than being caught. |
| **EX-5 — Harness/tests** | ✅ Done: `assert_exception` executes (and rejects a trap or a normal return); `assert_return`/`assert_trap`/plain actions reject a pending exception; a manual WAT test covers a caught payload and the uncaught case; all three scripts are curated. | EX6, EX7 | Done: the suite verifies exception behaviour; full run **7866 assertions / 191 test cases**, green in Debug and Release. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17, after EX-5):

| Script | Result | Reading |
|---|---|---|
| `tag` | 6 / 0 / 4 | tag parsing/validation works (the 4 skips are `assert_invalid`) |
| `ref_null` | **34 / 0 / 0** | `exnref`/`nullexnref` and every other null form, verified |
| `throw` | **10 / 0 / 3** | payloads of every scalar type, cross-function throw, polymorphic dead code; the 3 skips are `assert_invalid` |
| `throw_ref` | **13 / 0 / 2** | `catch_ref` → `throw_ref` round trips; the 2 skips are `assert_invalid` |
| `try_table` | **56 / 0 / 11** | every catch form, nested/multi-catch, imported tags, traps passing through; the 11 skips are all `assert_invalid` |
| `exceptions.wat` | 7 assertions | caught payload (host call) and the uncaught, embedder-visible pending exception |

`tag`, `ref_null`, `throw`, `throw_ref` and `try_table` are all in
`Test/wast_supported.txt`. The remaining skips are validation and reference
*results* (the usual `R5`/validator items), not exception semantics.

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
  (reference types), [`docs/ABI.md`](../ABI.md);
  sources `WasmBase/WasmType.hpp` (`Tag`, `ImportTag`, `CatchClause`),
  `WasmBase/WasmModule.cpp` (`processTagSection`),
  `WasmBase/WasmOpcode.hpp` (`Throw`, `ThrowRef`, `TryTable`),
  `WasmBase/WasmImport.hpp` (`resolveTag`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchThrow`, `dispatchThrowRef`,
  `dispatchTryTable`), `Test/wast_supported.txt`.
