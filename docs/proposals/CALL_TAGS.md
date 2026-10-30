# Call Tags — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_call_tags}

Status: design note / implementation reference (revision 2 — CTA-2 canonical
tag identity is implemented in the Store; the proposal has no binary encoding
yet, so opcodes, tag references, `call_tag.*` section entries and
`func_switch` remain blocked).
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
- `TagId` is an identity for an exception payload type, not a call tag.
  **Call tags now have their own Store-owned canonical identity**
  (`Store::CallTagId` / `StoreOwnedCallTag`): `canonicalCallTag(TypeId)` is
  idempotent per concrete signature, and two modules with the same function-type
  `TypeId` receive the same tag. It is deliberately separate from `TagId`.
- There is still no tag reference type: `AbstractHeapType` has the GC and
  exception hierarchies (`exn` included), but no `tag` heap type, and tags are
  not values on the operand stack.
- The upstream overview assigns **no binary encoding** for
  `call_with_tag`/`call_tag.canon`/`call_tag.new`/`func_switch`, so no opcode
  or tag-section arm was guessed. An unknown primary opcode still reaches the
  dispatcher's `default` and throws.
- Added the hand-built/API test
  `call-tag canonical identity is shared per concrete signature`, which checks
  canonical equality across modules and distinctness for a different signature.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| CTA1 | **No call-tag opcodes and no assigned encoding.** The upstream overview only names `call_with_tag`, `call_indirect_with_tag`, `call_return_with_tag`, `call_tag.canon` and `call_tag.new`; it assigns no byte values or section discriminants. | `WasmOpcode.hpp`, upstream `Overview.md` | **Blocker** |
| CTA2 | **No tag heap type or tag values.** Tags cannot be referenced, passed or stored; `AbstractHeapType` has no `tag`. | `WasmType.hpp` | High |
| ~~CTA3~~ | ~~**No canonical tag identity.**~~ **CTA-2 groundwork done:** `Store::canonicalCallTag(TypeId)` returns a stable canonical tag and records the signature; `Store::callTag(id)` exposes it. This is independent of the missing wire encoding. | `WasmStore.hpp` / `.cpp` | Closed (API) |
| CTA4 | **No `func_switch` or per-function tag list.** The code and function declarations cannot express multiple handled signatures or a fallback; there is no encoding for either. | `WasmModule.cpp` (`processCodeSection`), tag section parser | High |
| CTA5 | **No fallback-handler path.** An unhandled canonical tag cannot trap through a call-tag table because the table does not exist. | — | Medium |
| ~~CTA6~~ | ~~**No tests.**~~ **CTA-2 API test added:** canonical tags are equal across modules for one signature and distinct for another. No upstream call-tags script exists. | `Test/main.cpp` | Closed (CTA-2 only) |

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
| **CTA-0 — Honest rejection** | Keep the unencoded call-tag opcodes out of the decoder; an unknown primary opcode still throws. | CTA1, CTA2 | A call-tag module fails at the boundary. |
| **CTA-1 — Tag heap type** | Blocked on a wire encoding for the tag reference type; add the `tag` heap type, tag references and tag values (distinct from `exn`). | CTA2 | A tag can be a parameter/result/global and import/export by identity. |
| **CTA-2 — Canonical tags** | ✅ Store-owned `CallTagId`/`StoreOwnedCallTag` and idempotent `canonicalCallTag(TypeId)`, plus the cross-module API test. The `call_tag.new` section entry is still blocked on the encoding. | CTA3, CTA6 | Two modules derive equal canonical tags for the same signature. |
| **CTA-3 — Calling** | Blocked on the opcode and tag-value encodings; implement `call_with_tag`, `call_indirect_with_tag` and `call_return_with_tag`, including fallback/trap rules. | CTA1, CTA5 | A `funcref` with multiple handled signatures dispatches correctly. |
| **CTA-4 — `func_switch`** | Blocked on the code-section encoding; decode the construct and per-function `(call_tag ...)`. | CTA4 | A `func_switch` selects the implementation by tag. |
| **CTA-5 — Tests** | The canonical-tag API test is in the default suite; the proposal's scripts do not exist yet. | CTA6 | The call-tag tests are curated. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `tag` | 6 | 0 | 4 | **exception tags only**; unrelated to call tags |
| `call-tag canonical identity...` | 10 assertions | 0 | 0 | CTA-2 API: equal canonical tags across modules, distinct for a different signature |
| (none) | — | — | — | no call-tags script in the snapshot |

Reading: canonical tag identity is graded; the wire-level call-tag operations
remain blocked on the proposal's missing encoding.

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
