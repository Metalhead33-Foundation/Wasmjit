# Type Imports and Exports — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking proposal (revision 1 — nothing is
implemented; the import decoder does not even reject the new external kind
cleanly).
Audience: anyone touching `WasmModule` (type/import/export sections),
`WasmTypeRegistry`, `WasmImport`, `WasmRegistryImportResolver`, or the
validator.

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

Type Imports and Exports lets a module import type definitions rather than
only functions, tables, memories, globals and tags.

- A new external kind **`0x05` = Type** in the import and export sections.
- An import descriptor of kind Type carries a `typetype`:
  `boundkind:u8` (MVP: `0` = `sub`) followed by a `heaptype` bound.
- An additional import section may appear **before** the type section, and it
  may contain only type imports. The import section after the type section may
  not contain them.
- A type export carries an `s33` type index (a heap type) in its `exportdesc`.
- Imported types are **abstract** to the importing module but have a known
  representation (the bound, an abstract heap type in the MVP), so
  `(ref $t)` can be compiled without knowing the definition.
- At link time the provided type must satisfy the declared bound
  (subtyping); type exports are transparent by default. Private/opaque type
  definitions (`private.new` / `private.get`) are **post-MVP**.

## 3. What the proposal does not demand

- No runtime semantics: type imports are a validation/linking feature.
- No new class of type definitions; type indices keep denoting heap types.
- No relaxation of section order beyond the pre-type import section.
- No bounds other than `sub` in the MVP, and no bounds over concrete (non-
  abstract) heap types.
- No `WebAssembly.Type` JS API for a non-web embedder; private types are
  post-MVP.
- No change to how a representation is chosen at compile time.

## 4. What we have implemented today

- `ExternalKind` (`WasmBase/WasmType.hpp`) has `Function` 0, `Table` 1,
  `Memory` 2, `Global` 3 and `Tag` 4; **there is no Type kind**.
- `Module::processImportSection` (`WasmBase/WasmModule.cpp`) is an if-chain
  over those five kinds with **no `else`**. An unknown kind is not consumed and
  not rejected: the `typetype` payload stays in the stream, so a single type
  import is silently dropped and a following import is decoded from the wrong
  offset.
- `processExportSection` casts any kind byte to `ExternalKind`. The validator's
  export switch and `ModuleInstance::registerExports` have no Type arm, so a
  type export is silently accepted and never registered.
- `WasmTypeRegistry` already gives every type a canonical `TypeId` and supports
  structural matching; `Module::typeIds` maps local indices to it. This is the
  substrate a type-import implementation would use.
- The registry (`WasmRegistryImportResolver`) keys functions, globals, memories,
  tables and tags by name. It has **no type namespace**.
- `Module::processSecetions` walks sections in file order, so an import section
  before the type section would run before any type is registered.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TI1 | **No Type external kind.** Kind `0x05` has no enum value, no parser arm and no payload consumption. | `WasmType.hpp` (`ExternalKind`), `WasmModule.cpp` (`processImportSection`) | High |
| TI2 | **No `typetype` / bound decoding.** `boundkind` and `heaptype` are never read, and a pre-type import section is not a supported ordering. | `WasmModule.cpp` | High |
| TI3 | **Type exports are ignored.** Kind `0x05` in the export section is stored as an out-of-range `ExternalKind`, then skipped by both the validator and `registerExports`. | `WasmModule.cpp`, `WasmValidator.cpp`, `WasmModuleInstance.cpp` | High |
| TI4 | **No type namespace in the resolver.** Even a decoded type import could not be resolved or matched against an exported type. | `WasmRegistryImportResolver.hpp` | High |
| TI5 | **No validation/linking rule.** The bound/subtyping/transparency rules of §2 are not modelled. | `WasmValidator.cpp` | Medium |
| TI6 | **No test.** No type-import script exists in the snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TID1 | **Reject kind `0x05` with a named error before full support.** | §3: type imports have no runtime semantics; `SAFETY.md`: an unsupported descriptor must not be guessed at; silent desync is worse than a clear rejection. | A type-import module fails at the boundary, not mid-section. |
| TID2 | **Represent an imported type as a canonical `TypeId`** and reuse `WasmTypeRegistry` matching. | §3: no new class of type definitions; representation is known at compile time. | One identity mechanism for imports, exports and `(ref $t)`. |
| TID3 | **Allow the pre-type import section only for type imports**, as the proposal says. | §2/§3: this is the one sanctioned ordering relaxation. | The MVP avoids mutual section dependencies. |
| TID4 | **Defer `private.new`/`private.get` and opaque exports.** | §3: they are explicitly post-MVP. | A smaller, sound MVP. |

Guardrail: a type import must never be resolved by structural guessing. The
provided definition must be matched against the declared bound through the
canonical registry, and an unmatched import must fail instantiation.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TI-0 — Honest rejection** | Give `ExternalKind` a Type value or an explicit rejection and consume/report the descriptor. | TI1 | A type-import module fails with a named error instead of desynchronising. |
| **TI-1 — Decode `typetype`** | Parse the pre-type import section, `boundkind` and `heaptype`; keep the declared bound. | TI1, TI2 | A hand-built module decodes its type imports. |
| **TI-2 — Type exports** | Decode export kind `0x05`, register the type under its name and publish its canonical id. | TI3, TI4 | A second module can import the exported type. |
| **TI-3 — Match at link time** | Check the provided type against the bound with registry subtyping; fail instantiation on a mismatch. | TI5 | A positive and a negative link test both behave as specified. |
| **TI-4 — Tests** | Add MVP import/export and subtyping cases (private types excluded). | TI6 | The tests run in the default suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no type-import/export script in the testsuite snapshot |

Reading: nothing to grade. TI-0 is the first milestone because a type import
currently does not fail safely.

## 9. References

- Proposal overview (`WebAssembly/proposal-type-imports`):
  https://github.com/WebAssembly/proposal-type-imports/blob/master/proposals/type-imports/Overview.md
- Spec — Modules (imports/exports and matching):
  https://webassembly.github.io/spec/core/valid/modules.html
- In-repo: [`GC.md`](GC.md) (canonical `TypeId`/registry), [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) CM6,
  [`../SAFETY.md`](../SAFETY.md); sources `WasmBase/WasmType.hpp`
  (`ExternalKind`), `WasmBase/WasmModule.hpp`/`.cpp` (`processImportSection`,
  `processExportSection`), `WasmBase/WasmTypeRegistry.hpp`,
  `WasmBase/WasmRegistryImportResolver.hpp`.
