# Type Imports and Exports — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_type_imports}

Status: design note / implementation reference (revision 4 — TI-0…TI-4 are
done: type imports decode `sub <abstract heaptype>`, enter the local type index
space before definitions, receive nominal Store ids, type exports are
registered by name, and link-time matching enforces the declared bound. The
proposal-side implementation is complete; private/opaque types remain post-MVP).
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

- `ExternalKind` has `Function` 0, `Table` 1, `Memory` 2, `Global` 3, `Tag` 4
  and `Type` 5.
- `Module::processImportSection` decodes a type import's `boundkind` and
  abstract `heaptype`, rejects a post-type-section type import, and interns the
  nominal `(module, field, bound)` identity through
  `TypeStore::internCoreImport`. Type imports occupy the low indices of the
  local type index space; `processTypeSection` passes their ids as the interning
  prefix.
- `processExportSection` accepts `ExternalKind::Type`; `WasmValidator` bounds-
  checks the index; `ModuleInstance::registerExports` publishes
  `module->typeId(index)` through `ImportRegistrar::registerType`.
- `RegistryImportResolver` has a separate type namespace and
  `resolveType(module, field, bound)` checks the provided type with
  `WasmTypeRegistry::matchesHeap` before returning its canonical `TypeId`.
- `CoreTypeView` exposes `CanonTypeKind::Import` and `importBound()`; matching,
  JIT translation and validation treat an imported type as the abstract type its
  bound denotes.
- `ModuleInstance::typeImports()` records the resolved ids in import order.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~TI1~~ | ~~Type external kind is decoded but not linked.~~ **Done (TI-1/TI-2).** | `WasmType.hpp`, `WasmModule.cpp`, `WasmTypeStore.cpp` | Closed |
| ~~TI2~~ | ~~No `typetype` / bound decoding.~~ **Done (TI-1).** | `WasmModule.cpp` | Closed |
| ~~TI3~~ | ~~Type exports are ignored.~~ **Done (TI-2).** | `WasmModule.cpp`, `WasmValidator.cpp`, `WasmModuleInstance.cpp` | Closed |
| ~~TI4~~ | ~~No type namespace in the resolver.~~ **Done (TI-2/TI-3).** | `WasmRegistryImportResolver.*` | Closed |
| ~~TI5~~ | ~~No validation/linking rule.~~ **Done (TI-3):** the provided type must be a subtype of the declared abstract bound; a mismatch fails instantiation. | `WasmValidator.cpp`, `WasmRegistryImportResolver.cpp` | Closed |
| ~~TI6~~ | ~~No test.~~ **Done (TI-4):** hand-authored exporter/importer binaries cover a positive `sub any` link and a negative `sub func` link. | `Test/main.cpp` | Closed |

Remaining post-MVP: `private.new`/`private.get`, opaque exports and casts to an
imported type. The proposal leaves those out of the MVP; the importing module
sees the imported type as abstract, so no dependent local type is rewritten to
the provided definition.

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TID1 | **Represent an imported type as a nominal Store record with an abstract representation bound.** | §3: type imports have no runtime semantics; `SAFETY.md`: an unsupported descriptor must not be guessed at. | No dependent local type is rewritten; a cast to an imported type is the proposal's own post-MVP item. |
| TID2 | **Represent an imported type as a canonical `TypeId`** and reuse `WasmTypeRegistry` matching. | §3: no new class of type definitions; representation is known at compile time. | One identity mechanism for imports, exports and `(ref $t)`. |
| TID3 | **Allow the pre-type import section only for type imports**, as the proposal says. | §2/§3: this is the one sanctioned ordering relaxation. | The MVP avoids mutual section dependencies. |
| TID4 | **Defer `private.new`/`private.get` and opaque exports.** | §3: they are explicitly post-MVP. | A smaller, sound MVP. |

Guardrail: a type import must never be resolved by structural guessing. The
provided definition must be matched against the declared bound through the
canonical registry, and an unmatched import must fail instantiation.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TI-0 — Honest rejection** | ✅ Consume/report the descriptor; later folded into TI-1. | TI1 | A type-import module never desynchronises the import stream. |
| **TI-1 — Decode `typetype`** | ✅ Parse `boundkind`, the abstract `heaptype`, the pre-type import ordering and the local-index prefix. | TI1, TI2 | A hand-built module decodes its type imports. |
| **TI-2 — Type exports** | ✅ Decode export kind `0x05`, validate the index, register the type under its name and publish its canonical id. | TI3, TI4 | A second module can import the exported type. |
| **TI-3 — Match at link time** | ✅ Check the provided type against the bound with registry subtyping; fail instantiation on a mismatch. | TI5 | A positive and a negative link test both behave as specified. |
| **TI-4 — Tests** | ✅ Hand-authored exporter/importer binaries cover `sub any` and a `sub func` mismatch (private types excluded). | TI6 | The tests run in the default suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-20): no upstream type-import script
exists in the testsuite snapshot. The feature is graded by the hand-authored
`core type imports link by their abstract bound` test: a struct type exported as
`m.t` satisfies a `(type (sub any))` import, the importer records the exporter's
canonical `TypeId`, and a `(sub func)` bound rejects the same export. The test
runs in the default suite.

Reading: TI-0…TI-4 are closed; only the proposal's post-MVP private/opaque type
surface remains out of scope.

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
