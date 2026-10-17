# Call Tags — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — exception tags
exist, but **call tags do not**; the two are different proposals that share a
word).
Audience: anyone touching `WasmModule` (tag section), `WasmType`, `WasmStore`,
`WasmRegistryImportResolver`, `WasmOpcode`, or the dispatcher.

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

Call Tags generalize `call_indirect` into a dynamically typed call through a
`funcref`, with the callee (not the caller) deciding which signatures it
handles.

- **Call tags.** `call_tag.canon $functype` is the canonical tag for a function
  type; `call_tag.new $functype $func?` creates a fresh tag, optionally with a
  fallback handler. Tags live in the Tags section and can be imported/exported.
- **Calling.** `call_with_tag $tag : [ti* funcref] -> [to*]`,
  `call_indirect_with_tag $table $tag : [ti* i32] -> [to*]`, and the tail form
  `call_return_with_tag $tag`.
- **Equivalence.** `call_indirect $table $functype` is the special case
  `call_with_tag (call_tag.canon $functype) (table.get $table)`.
- **Multiple signatures.** A function definition may declare
  `(call_tag $tag*)`; the `funcref` returned by `ref.func` then handles exactly
  those tags, whose types must be supertypes of the function's type. A function
  with no declaration handles only its canonical tag.
- **Dispatch.** `func_switch ($tag $func)* $fallback?` is a new code-section
  construct that builds a `funcref` selecting the implementation by tag; an
  unmatched tag goes to the switch's fallback or to the tag's own fallback, and
  an unhandled canonical tag traps.
- **Abstraction.** `call_tag.canon` is restricted to concrete types;
  `call_tag.new` is not. Type abstraction and subtyping must be respected, and
  the canonical tag for a signature is the same in every module.

## 3. What the proposal does not demand

- No change to the `funcref` representation; a call tag is metadata attached to
  call sites and function definitions.
- No replacement of direct calls or of `call_indirect`; the latter remains a
  shorthand.
- No custom JS API; a host function may still be coerced to one signature.
- No requirement that a tag be exported; private tags are the optimization
  case.
- No funcref casts: call tags are explicitly proposed as the alternative to
  casting infrastructure.

## 4. What we have implemented today

- **Exception tags** are implemented and are *not* call tags: `Tag` carries an
  `attribute` and a payload type index, `TagId` is a process-wide identity,
  `importTags`/`tags` are parsed, `resolveTag`/`registerTag` handle imports and
  exports, and `throw`/`try_table` consume them (`EXCEPTIONS.md` EX-2).
- There is no `call_with_tag`, `call_tag.*`, `call_indirect_with_tag`,
  `call_return_with_tag` or `func_switch`. None of these names or opcodes
  exists in `WasmOpcode.hpp`; an unknown primary opcode reaches the dispatcher's
  `default` and throws.
- There is no tag reference type: `AbstractHeapType` has the GC and exception
  hierarchies (`exn` included), but no `tag` heap type, and tags are not values
  on the operand stack.
- `TagId` is an identity for an exception payload type, not a canonical
  signature→tag mapping; there is no `call_tag.canon` table and no per-function
  tag list.
- No test: `tag.wast` exercises exception-tag import/export and link-time
  typing, not call tags.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| CTA1 | **No call-tag opcodes.** `call_with_tag`, `call_indirect_with_tag`, `call_return_with_tag`, `call_tag.canon` and `call_tag.new` do not exist. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | High |
| CTA2 | **No tag heap type or tag values.** Tags cannot be referenced, passed or stored; `AbstractHeapType` has no `tag`. | `WasmType.hpp` | High |
| CTA3 | **No canonical tag identity.** `TagId` identifies an exception payload type, not a signature→tag map, and there is no cross-module canonicalization. | `WasmModuleInstance.cpp`, `WasmStore.hpp` | High |
| CTA4 | **No `func_switch` or per-function tag list.** The code and function declarations cannot express multiple handled signatures or a fallback. | `WasmModule.cpp` (`processCodeSection`), tag section parser | High |
| CTA5 | **No fallback-handler path.** An unhandled canonical tag cannot trap through a call-tag table because the table does not exist. | — | Medium |
| CTA6 | **No tests.** There is no call-tags script in the snapshot; `tag.wast` covers exception tags only. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| CTAD1 | **Do not reuse an exception `TagId` as a call tag.** | §3: call tags are a distinct construct; an exception tag identifies a payload, a call tag identifies a signature. | No semantic conflation. |
| CTAD2 | **Make a call tag a Store-owned object keyed by the canonical `TypeId`.** | §3: representation is free; §2 requires canonical equality. | One canonical tag per signature, comparable across modules. |
| CTAD3 | **Keep `call_indirect` on its current path** and add `call_with_tag` beside it. | §2 requires the equivalence; §3 does not require replacing `call_indirect`. | No regression in the existing indirect-call tests. |
| CTAD4 | **Reject the new opcodes until the table and heap type exist.** | §3: no runtime representation is demanded; `SAFETY.md` forbids guessing at unknown opcodes. | A call-tag module fails cleanly. |
| CTAD5 | **Do not change the `funcref` representation.** | §3: the proposal explicitly avoids cast infrastructure on `funcref`. | Existing `ref.func`/`call_ref` stay untouched. |

Guardrail: canonical tags must be equal across modules for the same concrete
signature, and a private tag must not leak through an exported `funcref`.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CTA-0 — Honest rejection** | Reject the call-tag opcodes and tag-reference types with a named error. | CTA1, CTA2 | A call-tag module fails at the boundary. |
| **CTA-1 — Tag heap type** | Add the `tag` heap type, tag references and tag values (distinct from `exn`). | CTA2 | A tag can be a parameter/result/global and import/export by identity. |
| **CTA-2 — Canonical tags** | Add a Store-owned canonical map from a concrete signature `TypeId` to a call tag, and `call_tag.new` entries to the tag section. | CTA3 | Two modules derive equal canonical tags for the same signature. |
| **CTA-3 — Calling** | Implement `call_with_tag`, `call_indirect_with_tag` and `call_return_with_tag`, including the fallback/trap rules. | CTA1, CTA5 | A `funcref` with multiple handled signatures dispatches correctly. |
| **CTA-4 — `func_switch`** | Decode the construct and the per-function `(call_tag ...)` declaration. | CTA4 | A `func_switch` selects the implementation by tag. |
| **CTA-5 — Tests** | Add the proposal's scripts (none in the snapshot). | CTA6 | The call-tag tests are curated. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `tag` | 6 | 0 | 4 | **exception tags only**; unrelated to call tags |
| (none) | — | — | — | no call-tags script in the snapshot |

Reading: the green `tag` result must not be read as call-tag support. The
skips are `assert_invalid` / `assert_unlinkable` cases for exception tags.

## 9. References

- Proposal overview (`WebAssembly/call-tags`):
  https://github.com/WebAssembly/call-tags/blob/main/proposals/call-tags/Overview.md
- In-repo: [`EXCEPTIONS.md`](EXCEPTIONS.md) (exception tags, `TagId`),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md) (`call_ref`, typed refs),
  [`TYPE_IMPORTS.md`](TYPE_IMPORTS.md) (concrete/abstract types and
  canonicalization), [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md);
  sources `WasmBase/WasmType.hpp` (`Tag`, `AbstractHeapType`),
  `WasmBase/WasmModule.cpp` (`processTagSection`), `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmRegistryImportResolver.cpp`, `WasmBase/WasmOpcode.hpp`.
