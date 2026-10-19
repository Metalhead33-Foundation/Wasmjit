# Unified Type Store and Component Type Identity — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 11 — core modules are
Store-owned: `Module` keeps only `typeIds`, groups and a reverse index map, and
the JIT/runtime/validator consume `CoreTypeView` or Store-derived signature
views. Closed component records and type-bearing component/instance declaration
sets intern through canonical `TypeId` payloads, and validation publishes a
canonical component-local type index → `TypeId` map; component `CoreRecType`
groups intern to Store TypeIds and drop their parser scratch. Resource
parameters substitute and regenerate per instantiation, open types intern as
alpha-equivalent `SchemeId`s, and `TypeStore::canonicalAbi` provides
option-keyed flat/layout ABI metadata. Parser syntax records remain as the
component AST; external visibility now has a resource/structural slice, while
annotated-name rules and the ABI runtime/layout remain, and §7.11 records their
exact cross-proposal blockers).
Audience: anyone touching `WasmBase/WasmType.hpp`, `WasmBase/WasmCanonicalType.hpp`,
`WasmBase/WasmTypeRegistry.{hpp,cpp}`, `WasmBase/WasmComponentModel.hpp`,
`WasmBase/WasmComponentParser.inc`, `WasmBase/WasmComponentValidation.cpp`, or the
type-translation layer that consumes canonical identities.

This note follows the nine-slot split used by [`GC.md`](GC.md),
[`SIMD.md`](SIMD.md) and [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md). It is an
**internal type-representation design note**, not a WebAssembly proposal. It
describes the implementation rather than adding a WebAssembly surface: §4 is the
current representation, §5 is the gap between that representation and the target,
and §7 is the migration plan. §7.11 is the authoritative blocker/status list for
the remaining completion milestones.

Revision 2 records the first implementation. `WasmTypeStore` owns compact core
records and the shared component payload pools; `TypeRegistry` exposes core
records through `CoreTypeView`; the component parser writes defined-value,
function and resource types into the same store, and `ComponentTypeDef` is a
small handle plus a declaration payload. `Subtype` remains the parser/JIT
compatibility layer, and `ComponentValidator` still builds its `ResolvedType`
graph.

Revision 4 added component structural interning: `TypeStore` interns closed
component records that contain no unresolved local type index. The component
intern table, its hit/miss counters and its footprint accounting are part of the
store; the tests assert cross-component record/list/func sharing, label-order
sensitivity, resource freshness and the deliberate non-interning of a list whose
element is a local index.

Revision 5 closes the **core-module** half of TM-6/TM-7. `TypeRegistry` no longer
retains parser blocks and `Module` has no `types` member: `processTypeSection`
interns a temporary vector and keeps only `typeIds`, `typeGroups` and a
TypeId→local-index map. `Module::typeView` is the consumer API; `funcTypeAt` is
the remaining Store-derived `FuncType` compatibility view for compile/test
paths. The validator's module-level well-formedness walk, JIT type translation,
JIT struct/array access, the runtime GC walker and constant-expression GC
construction use canonical views/storage. The old `module.types` reads are gone.
Component `CoreRecType` now interns its groups into Store records and clears the
parser `Subtype` scratch at validation; a validated component retains only
`typeIds`. The remaining component work is canonical value-payload references,
structural declaration interning and schemes/ABI (§7.11.2).

Revision 6 adds the first half of that resolver: `ResolvedType` carries the
parser-assigned `TypeId` for defined value/function/resource definitions, and
`ComponentValidator::run()` publishes `Component::localTypeIds` over the whole
component type index space (including aliases/imports). Component/instance
declarations are copied into compact `ComponentDeclRecord` spans and receive
nominal Store TypeIds. They are not structurally interned yet, because the
declaration-set equality rule still has to be pinned against `wasm-tools`.

Revision 7 closes the `CoreRecType` ownership residue. `TypeRegistry::internGroup`
interns one `rec` group against a component-local TypeId map; `validateCoreTypeDef`
uses it for top-level core types and module-type declarations, stores the result
in `CoreRecType::typeIds`, and clears `CoreRecType::subtypes`. A validated
component therefore retains only canonical TypeIds for its core types.

Revision 8 closes the internal TM-8/TM-12 residue. `ComponentValueType` gains
`Kind::TypeId`, and after validation `ComponentValidator` rewrites the resolved
value/func/instance/component graph into Store records whose nested references
are `TypeId`s, not local indices. `Component::localTypeIds` therefore publishes
canonical ids, component/instance declaration sets with type-bearing records
intern structurally, and `own`/`borrow` reference a nominal resource `TypeId`
through `TypeStore::addComponentTypeRefCanonical`. Parser syntax records remain
in the store as the component AST and deliberately keep `Kind::TypeIndex`; the
canonical record is the identity, and `ComponentTypeDef::id` may differ from
`Component::typeId(localIndex)` for a type with local references. Declaration
sets containing memory/global/table entries remain nominal because
`ComponentDeclRecord` has no core type payload for them. Component
instantiation also substitutes abstract `sub resource` imports in the resulting
instance exports and records the canonical result in `ComponentInstance::typeId`.
Arbitrary open type schemes and Canonical ABI runtime metadata remained.

Revision 9 adds the CM-1d resource-generativity slice and the first Canonical ABI
metadata slice. Resources defined by an instantiated component get fresh
nominal ids/`TypeId`s per instantiation, while provided resources stay shared.
An open component/value/func type containing an abstract resource is recognized
and left uninterned (`kNone`) instead of being assigned a closed id.
`TypeStore::canonicalAbi` caches flat params/results keyed by
`(TypeId, CanonicalAbiOptions)` for primitives, strings, lists, fixed lists,
records, tuples and resource handles, and `canonicalAbiLayout` derives size and
alignment for those shapes plus enums/flags; variant join/layout and lift/lower
remain gated by `COMPONENT_MODEL.md`. Core-module component imports also compare the
declared module type structurally (canonical local type ids plus table/memory/
global shapes) instead of accepting any module with the right sort.

Revision 10 closes arbitrary open-scheme identity. `SchemeId` is a separate
process-wide identity; `TypeStore::internScheme` interns an alpha-normalized
structural key in which abstract resource parameters are placeholders assigned
by first occurrence. `Component::localSchemeIds`/`schemeId(localIndex)` publish
the scheme identity for open component/value/func types, while
`typeId(localIndex)` stays `kNone` so a scheme never masquerades as a closed
type. `ComponentInstance` carries the same split (`typeId`/`schemeId`), and
`TypeScheme::parameterCount` records the parameter arity.

Revision 11 adds the first external-visibility slice and annotated-name rules.
Imports/exports of type sort introduce externally-visible names; resource,
record, variant, enum and flags types transitively used by a function
import/export must already have such a name. Imports may depend only on
imported (not exported) names. Type exports/aliases are cloned at the
visibility boundary so a local definition does not become visible
retroactively. Extern-name syntax, `implements` interface names,
`versionsuffix` semver consistency and `[constructor]`/`[method]`/`[static]`
resource binding are checked; getter/setter semantics remain.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What must a unified type store guarantee? |
| §3 | What is the store free to choose? |
| §4 | What is implemented today? |
| §5 | Where does the current representation fall short? |
| §6 | Where can and should the refactor play fast and loose? |
| §7 | How does the migration proceed? |
| §8 | What tests and measurements prove it? |
| §9 | References |

## 2. What a unified type store must guarantee

The store is an internal contract, not a wire-format feature. Its guarantees are
the relations the rest of the engine already depends on, plus the ownership rules
that keep a module from growing an unbounded graph of duplicate type objects.

### 2.1 Core type contract

- **Keep the existing core identity model.** `LocalTypeIdx` is still a
  module-local index; `TypeId` is still the process-wide identity of a canonical
  defined type; `RecGroupId` still identifies a contiguous interned `rec` group
  (`docs/TYPE_IDENTITY.md` §6.1).
- **Preserve `rec`-group canonicalization.** A group is the unit of interning;
  references inside the group are resolved by full structural equality, not by a
  hash match alone. A bare type and a one-member `rec` group retain the same
  identity (`docs/TYPE_IDENTITY.md` §6.3).
- **Preserve the core `match` relation.** Abstract heap-type classification,
  reference nullability, function variance, struct width and field rules, array
  covariance, and mutable-position equality stay exactly as specified. A compact
  record must be a view over the same canonical relation, not a second matcher.
- **Keep the JIT-facing core layout available.** A backend consumes a core
  function/struct/array shape and a core value type. The store may change where
  those lists live, but it may not change their meaning, ordering or packing.

### 2.2 Component type contract

- **Structural identity for defined component types.** Two structurally equal
  component value/function/instance/component types must be the same type, even
  when they came from different components. This is the analogue of core
  canonicalization, and is the intended reuse of `TypeRegistry`
  (`COMPONENT_MODEL.md` CMD9).
- **Nominal identity for resources.** A resource is never structurally interned.
  Each resource definition, import and instantiation supplies a fresh resource
  identity; an `eq` alias shares the existing identity; `sub resource` introduces
  a new abstract resource identity. Two resources with the same representation
  and destructor are still different resources.
- **No local index escapes validation.** A component type index is meaningful
  only in its component's index space. After validation, the store receives a
  process-wide identity, and no stored type record contains a component-local
  index as an identity.
- **One graph, not two.** The parser's AST and the validator's resolved graph
  must not both retain an expanded copy of every value type, case, field and
  declaration. The validator may keep diagnostics/location data, but the type
  structure itself is stored once.
- **Open types are explicit.** A component type that contains an unsatisfied
  type import is a type *scheme*, not a closed type. Substitution through type
  imports (CM-1d) must be able to instantiate a scheme without confusing a type
  parameter with a concrete `TypeId`.
- **Rejection is the failure policy.** An out-of-range index, a forward
  reference, an unknown sort or a malformed declaration rejects the component;
  there is no tolerant canonicalization mode.

### 2.3 Operational contract

- **One owner.** The store, owned by the process-lifetime `Store`, owns records,
  payload arrays, declaration arrays and interned strings. Modules and components
  hold IDs/spans/views, not owning copies.
- **One canonical instance.** A canonical type has exactly one record in the
  store. There must never be two `Subtype`, `TypeRecord`, `DefinedValueType` or
  `ResolvedValueType` instances representing the same canonical type. If two
  definitions have the same canonical key, they **must** resolve to the same
  `TypeId`; object address is never an identity. A parser may build a temporary
  syntax object for one `rec` group, but that object is discarded at interning
  and is never retained by a `Module`, `Component`, `TypeStore` or instance.
- **Module/component state is references only.** After parsing, a `Module` or
  `Component` may own a local-index → `TypeId` map, a `TypeSpan`/`RecGroupId` or
  other handles. It may not own a `Subtype`, a canonical payload, a component type
  payload or a declaration payload. `ModuleInstance`/`ComponentInstance` may own
  runtime substitutions and handle tables; those are not type definitions.
- **Stable identity and stable views.** A `TypeId` is never reused. Records are
  addressed by index, so growth of a backing array does not invalidate a view.
- **Bounded allocation.** Every input-derived count is checked against the
  remaining input before a reserve/allocation. The type store obeys the
  allocation-budget policy in [`../SAFETY.md`](../SAFETY.md); a failed allocation
  is reported, not an `abort()`.
- **Backend-agnostic core.** No type-store name, layout or method in `WasmBase/`
  may name or depend on a specific code generator; see
  [`../LAYERING.md`](../LAYERING.md).
- **No per-type owning vectors in the long-lived form.** A type may own a span
  into a shared pool; it may not need a separate heap allocation for each
  parameter, result, field, case, label or declaration list.
- **Consistent counters.** The store reports records, payload bytes, string
  bytes, intern-table bytes and dedup ratios so the footprint claims below can be
  tested rather than asserted.

## 3. What the type store is free to choose

The WebAssembly specification fixes the type *relations*, not the in-memory
representation (`docs/TYPE_IDENTITY.md` §4). In particular, this refactor is free
to choose:

- One dense `TypeId` space for both domains, or separate domain-specific ID
  wrappers sharing a store.
- A structure-of-arrays layout, a tagged record plus payload pools, or another
  index-oriented encoding, provided the relations in §2 are preserved.
- Whether the parser builds a compact record directly or builds a temporary AST
  and collapses it before validation. The requirement is that the *long-lived*
  representation is single and compact, not that the parser has no temporary
  representation.
- Whether `TypeRegistry` keeps its name and core API while gaining a component
  domain, or is renamed to `TypeStore`. The former is the lower-risk migration.
- How intern keys are serialized/hashed, as long as a hit is confirmed by full
  structural equality and resource identities are not structural.
- Whether component `instance`/`component` declarations are normalized by name
  in the intern key. The choice is allowed only after confirming the spec's
  equality relation and testing it against `wasm-tools`; source order must remain
  available for diagnostics.
- Whether `Subtype` remains as a parser/compatibility view or is eventually
  removed. It does not need to disappear in the first migration step.
- How much diagnostic source information survives after validation. Location
  tables may be discarded independently of type identity.

The one thing that is **not** free is conflating semantic domains: a core value
type and a component value type are different languages even when both are
eventually backed by the same store.

## 4. What is implemented today

There are two independent representations, and neither is a unified type store.

### 4.1 Core: `Subtype` plus a canonical copy

`WasmBase/WasmType.hpp` defines the parser/runtime layout:

```text
Subtype
  bool isFinal
  std::vector<uint32_t> supertypeIndices
  CompositeDefinition composite
    = std::variant<FuncType, StructType, ArrayType>
```

`FuncType` owns two `std::vector<StorageType>` members; `StructType` owns a
`std::vector<FieldType>`; `ArrayType` owns a `FieldType`. `Subtype` is now a
**parser/component scratch type**: `Module::processTypeSection` builds it only
while parsing and discards it after interning. It is **88 bytes** on the current
x86-64/libstdc++ toolchain, plus heap allocations for each non-empty list
(`docs/TYPE_IDENTITY.md` §12.1); no core module retains one.

`WasmBase/WasmCanonicalType.hpp` still defines the temporary
`CanonicalType { kind, isFinal, supertypes, depth, funcParams, funcResults,
structFields, arrayElement }` used while a `rec` group is being built. It is no
longer the long-lived canonical table: `WasmBase/WasmTypeStore.{hpp,cpp}` owns a
16-byte `TypeRecord` plus shared `CanonStorageType`/`CanonFieldType`/
`CanonHeapType` pools. `CoreTypeView` exposes the same shape from the store (or
from the temporary group while interning).

`WasmBase/WasmTypeRegistry.{hpp,cpp}` (`TypeRegistry`) does the following:

- `internModule(std::span<const Subtype>, groups)` consumes a **temporary**
  parser buffer, builds each `CanonicalType` group, validates and interns it,
  appends the resolved records to `TypeStore::appendCore`, and returns the
  local-index → `TypeId` map. It retains no `Subtype`.
- `TypeId` indexes the shared store; `RecGroupId` indexes `recGroups`.
- `canonicalType(TypeId)` returns a `CoreTypeView`, not a copied
  `CanonicalType`. `Module::typeView`/`typeId` are the consumer API; `Module`
  also keeps a TypeId→local-index map so `funcTypeAt` can materialise a
  Store-derived `FuncType` for the remaining compile/test compatibility paths.
- The JIT type translator has a canonical `translateTypes(typeIds, store)`
  overload; the old parser-view overload exists only for its cache regression
  test.

The current core flow is therefore:

```text
binary type section -> std::vector<Subtype> (temporary parser buffer)
                    -> CanonicalType group (temporary interning form)
                    -> TypeStore::appendCore (compact records/pools)
                    -> TypeId map in Module -> CoreTypeView
```

### 4.2 Components: an AST plus a validator-only resolved graph

`WasmBase/WasmComponentModel.hpp` defines the component AST:

- `ComponentValueType` is the compact 8-byte `{kind, primitive, typeIndex}`
  reference defined in `WasmTypeStore.hpp`.
- Defined-value and function payloads no longer live in the AST:
  `ComponentTypeDef` is a small `{kind, TypeId, declaration payload}` handle.
  `ComponentRecord`/`Variant`/`List`/`Tuple`/`Flags`/`Enum`/`Option`/`Result`/
  `Map`/`Stream`/`Future`/`Own`/`Borrow` records and their shared arrays are in
  `TypeStore`.
- `ComponentDecl` and `InstanceDecl` are a tag plus a `std::variant` of owning
  declaration payloads, so the old inline union is gone. The declaration stubs
  themselves still allocate because `ImportDecl`/`ExportDecl`/`Alias` are not
  pooled yet.
- Labels, field names and function parameter names are interned `StringId`s in
  the store's `StringTable`; `NameAttributes` text remains a `std::string` for
  the declaration layer.
- `TypeStore::addComponent*` structurally interns **closed** component records
  (primitive, record, variant, flags, enum, tuple, list, fixed-list, option,
  result, map, stream, future and func) whose nested `ComponentValueType`s are
  primitive. `componentIntern_`, `componentInternHits/Misses` and the intern-key
  bytes in `payloadBytes()` make the reuse measurable. Parser syntax records
  keep a component-local `TypeIndex` and are deliberately not interned; the
  validator's canonical rewrite uses `Kind::TypeId`, so local references,
  resources and `own`/`borrow` intern too. Type-bearing component/instance
  declaration sets intern; a set containing memory/global/table records stays
  nominal because those records have no core type payload (see §7.11.2).

`WasmBase/WasmComponentValidation.cpp` then builds a second graph with
`std::shared_ptr` nodes:

- `ResolvedValueType` mirrors the old `DefinedValueType` alternative set and
  owns `std::vector<std::pair<std::string, ValuePtr>>`, `std::vector<ResolvedCase>`,
  `std::vector<std::string>` and optional `shared_ptr`s. Its layout-derived size
  is about **240 bytes**.
- `ResolvedType` adds a kind, a `TypeId` for defined value/function/resource
  types, and five `shared_ptr` members; `ResolvedDecl` carries a full
  `NameAttributes`, a `SortIdx`, a `TypePtr` and core-module bookkeeping.
- `TypeScope` holds `std::vector<TypePtr> types` and a parallel core-type list.
  Resolution is by component-local index. `ComponentValidator::run()` first
  canonicalises the resolved graph into Store records whose nested
  `ComponentValueType`s are `Kind::TypeId`, then publishes
  `Component::localTypeIds` over that space (aliases/imports included).
  Component/instance declarations get canonical Store records and ids; schemes
  are still unrepresented, so an id can remain `kNone` only for an unvalidated
  component or a future scheme. The parser's `ComponentTypeDef::id` remains the
  syntax record's id and may differ from the canonical `Component::typeId` for a
  type with local references.

The current flow is therefore:

```text
binary component -> ComponentTypeDef handle + TypeStore component records
                 -> ResolvedType / ResolvedValueType shared_ptr graph
                 -> component-local type index
```

The second graph still exists for the validator's lifetime and expands every
nested type again, but the parser no longer keeps a parallel `DefinedValueType`
or `ComponentFuncType` tree. Component type interning and the removal of the
resolved graph remain TM-3/TM-4.


### 4.3 Identity and ownership today

| Type | How it is identified today | Interned? | Owner |
|---|---|---|---|
| Core defined type | `LocalTypeIdx` in a module; `TypeId` after `internModule` | Yes, by `rec` group | `TypeStore` (compact records); `Module` keeps only `typeIds`, groups and a reverse index map |
| Core canonical type | `TypeId` | Yes | `TypeRegistry` |
| Component value type | Canonical `TypeId` children in validated Store records; parser syntax records keep a component-local `typeIndex`; the definition is published as `Component::typeId(localIndex)` | Yes for canonical records; parser syntax records are not interned | `TypeStore` + `ComponentTypeDef` handle + `ResolvedType::typeId`/`Component::localTypeIds` + validator graph |
| Component resource | Store `TypeId` for a definition or abstract import; validator-local `resourceId` for the resolved resource | Nominal (never structural) | `TypeStore` + validator graph |
| Component instance/component type | Canonical `TypeId` from compact Store declaration records; `ComponentInstance::typeId` records a substituted instance type | Yes for type-bearing declaration sets; a set with memory/global/table records stays nominal | `TypeStore` declaration pools + `ComponentDeclRecord`; AST remains the source view |
| Component type scheme | `SchemeId` interned from an alpha-normalized structural key; abstract resource parameters are placeholders with `TypeScheme::parameterCount` arity | Yes, by alpha-normalized structure | `TypeStore` scheme table; `Component::localSchemeIds`/`schemeId`; `ComponentInstance::schemeId` |

The core-module identity machinery is now Store-owned: a loaded `Module` retains
no `Subtype`, and the validator, JIT and runtime GC paths use Store views. The
component layer has a parser and a structural validator; revision 4 added
Store-level identity for closed, local-index-free records, revision 5 moved the
core modules to Store-owned views, and revision 8 made validated component
payloads and type-bearing declaration sets canonical. The owner column is the
current state, not the target: `Component` still retains parser declaration
ASTs, while validated `CoreRecType` groups and component/instance declaration
records are Store-owned with `TypeId`s. The target is Store-only type definitions
with component references. §7.10 defines the completion plan and §7.11 records
what gates each remaining row.

### 4.4 Why the current form is expensive

The fixed per-type costs are only part of the problem:

- `Subtype` reserves the 48-byte `FuncType` payload for every struct and array and
  the 24-byte supertype vector even when there are no supertypes.
- `CanonicalType` reserves four 24-byte vectors for every kind; only one or two
  are populated.
- `DefinedValueType` reserves every alternative's vector/optional field.
- `ComponentTypeDef` reserves every component-type alternative.
- `ComponentDecl`/`InstanceDecl` reserve every declaration alternative.
- Every non-empty list and every `std::string` is a separate allocation.
- The core module no longer keeps the parser block; the component AST keeps its
  declaration syntax, while validated `CoreRecType` groups are reduced to
  `TypeId`s and the component validator keeps the resolved graph.
- The intern table stores a serialized `std::string` per group in addition to
  the canonical group itself.

The footprint therefore scales with `N` types in *both* representations and with
the number of nested lists, not just with the number of distinct types.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ✅ TM1 | **Component structural interning is complete for closed types.** After validation every resolved child reference is a canonical `TypeId`, so records with local references, `own`/`borrow` and resource references intern through the same store keys as primitive-only records. Component/instance declaration sets with type-bearing records intern structurally; sets with memory/global/table entries stay nominal because `ComponentDeclRecord` has no core type payload for them. Open/scheme types are TM7/TM13, not a residue of this gap. Parser syntax records remain an AST and may still carry `TypeIndex`; they are not canonical identities. | `TypeStore::addComponent*`, `addComponentTypeRefCanonical`, `componentIntern*`, `Component::localTypeIds`; §7.11.2. | Closed |
| ✅ TM2 | **No long-lived parser compatibility copy.** `Module::processTypeSection` discards its temporary `Subtype` buffer, and validated `CoreRecType` groups clear their parser scratch; both retain canonical `TypeId`s. `Subtype` remains only transient parser scratch. | `WasmModule::processTypeSection`; `WasmComponentValidation.cpp`; §4.1. | Closed |
| TM3 | **The component parser no longer keeps a parallel type tree**, but the validator still expands resolved types into `ResolvedValueType`/`ResolvedType`. | `WasmComponentValidation.cpp`; §4.2. | Medium |
| ◑ TM4 | **`Subtype` still pays for every composite alternative while parsing** a component core type; validated types use compact Store records. | `WasmType.hpp` `CompositeDefinition`; `WasmComponentModel.hpp`. | Medium |
| ◑ TM5 | **Parser lists still allocate per node while parsing**; validated core/module types discard them. Component type lists are pooled and labels/names interned. | `FuncType`/`StructType`; `WasmTypeStore.cpp`. | Medium |
| ✅ TM6 | **Component local indices now have a global mapping.** `Component::localTypeIds` records the validated component/component-core type index → `TypeId` relation, including aliases/imports. | `Component::localTypeIds`; `CoreRecType::typeIds`. | Closed |
| ✅ TM7 | **Open component types have a first-class scheme identity.** Abstract resource parameters are placeholders in the scheme key; `SchemeId` is separate from closed `TypeId`s, and instantiation substitutes provided resources. | `SchemeId`; `TypeStore::internScheme`; `Component::localSchemeIds`; `ComponentInstance::{typeId,schemeId}`. | Closed |
| ~~TM8~~ | ~~**No footprint/identity instrumentation.**~~ **Done (revision 2):** `TypeStore` exposes record/payload/allocation counters, and the test suite asserts the compact record sizes and the shared-store contents. | `WasmBase/WasmTypeStore.hpp`, `Test/main.cpp`. | Closed |
| TM9 | **Component type scope ownership is explicit for the parser**, while the validator's resolved graph remains a temporary scaffold. | `TypeStore` + `ComponentTypeDef`; resolved graph in `WasmComponentValidation.cpp`. | Medium |
| ◑ TM10 | **The canonical store is not the only type owner yet.** Core modules and validated component core types are Store-owned (`Module` keeps no `Subtype`, `CoreRecType` keeps `typeIds`). Component/instance declaration ASTs and parser value-type syntax records remain in `ComponentTypeDef`/`Component.types`, though their canonical records are Store-owned and interned. | `ComponentTypeDef` declarations; `Component::localTypeIds`; §7.11.2. | High |
| ✅ TM11 | **The core JIT/validator cutover is complete.** `LibJitTypeTranslator` has a canonical `translateTypes(typeIds, store)` overload; `LibjitOpcodeDispatcher`/`ModuleCompiler` use `typeView`/`funcTypeAt`; `ModuleInstance` GC walking and constant expressions use `CoreTypeView`; `WasmValidator` reads canonical views and `typeCount()`. The old translator `span<Subtype>` overload is test-only; `FuncType` signature materialisation remains a Store-derived compile view, not a retained module block. | `LibJit/LibJitTypeTranslation.*`, `WasmBase/WasmValidator.cpp`, `WasmBase/WasmModuleInstance.cpp`. | Closed (core) |
| ◑ TM12 | **Component type resolution is still validator-only.** Validation canonicalises the resolved value/func/instance/component graph into Store records with `TypeId` children, publishes canonical `Component::localTypeIds`, and structurally interns type-bearing declaration sets. `eq` aliases share the nominal resource id; `sub resource`/open schemes and parser-AST ownership remain. | `TypeScope::types`; `ResolvedType::typeId`; `Component::localTypeIds`; `ComponentDeclRecord`; §7.11.2. | High |
| ✅ TM13 | **Type schemes and resource substitution are modelled.** `SchemeId` interns alpha-equivalent open types with parameter placeholders; abstract resources substitute through instantiation and locally defined resources regenerate per instantiation. | `SchemeId`; `TypeStore::internScheme`; `ComponentResourcePayload::abstract`; `ComponentInstance::{typeId,schemeId}`. | Closed |
| ◑ TM14 | **Canonical ABI metadata is partially represented.** `TypeStore::canonicalAbi` derives and caches flat params/results keyed by `(TypeId, CanonicalAbiOptions)`; `canonicalAbiLayout` derives size/alignment for primitives, strings, lists, fixed lists, records, tuples, enums, flags, options, results, variants, maps, streams, futures and resource handles, with a configurable pointer width. Exact core joins for incompatible variant/result payloads, per-encoding string layout and the lift/lower runtime remain. | `WasmTypeStore.*`; `WasmComponentValidation.cpp` validates `canon` indices only; no ABI lowering. | High |
| ◑ TM15 | **The ownership/dedup/ABI gates are partially test-enforced.** Tests pin the core-module no-`types` gate, repeated-load identity, closed-component cross-component interning, label-order sensitivity, resource freshness/generativity, canonical `Component::localTypeIds`, alpha-equivalent `SchemeId` interning and parameter arity, canonical `TypeId` payloads, structural declaration interning, core-module type matching, external-visibility accept/reject cases, extern-name/annotation accept/reject cases, ABI flat shapes/layout and cache identity. Still absent: parser-AST removal and ABI runtime/variant layout. | `Test/main.cpp`; §7.11.5. | Medium |

Revision 4 closed the local-index-free half of TM1/TM12. Revision 5 closed the
core-module half of TM6/TM10 and all of TM11. Revision 7 closed TM2's long-lived
parser copy and TM6's component local-index mapping, and removed the
`CoreRecType` ownership residue. Revision 8 closes TM1 and the canonical
payload/declaration half of TM12: validation rewrites resolved component
references to `TypeId` and interns type-bearing declaration sets. The remaining
TM4/TM5 cost is transient parser scratch; TM10's parser AST, TM12's scheme
residue, TM13 and TM14 remain. §7.11 states which are internal work and which
depend on another proposal.


## 6. Where we can and should play fast and loose

The WebAssembly specification does not prescribe a type-store layout, and the
component model's structural equality is still being wired into this engine. The
following deviations are representation choices, not semantic shortcuts.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TMD1 | **Use one dense `TypeId` space with a domain tag** instead of separate core/component ID types. | §3 allows either ID model; every record carries its domain and every subtype operation rejects a cross-domain pair. | One intern table, one arena and one identity namespace; existing core `TypeId` values remain valid. |
| TMD2 | **Keep `Subtype` as a parser/compatibility view for the first migration**, while the store owns compact records. | §3: the spec fixes relations, not representation; §2.1 requires the core layout's meaning to survive, not the struct itself. | Avoids a flag-day rewrite of the validator/JIT consumers. |
| TMD3 | **Do not merge core `ValueType` and component `ComponentValueType` into one public value type.** They share storage and identity machinery only. | §2.2 and §3: they are different languages; the component model's primitives and `own`/`borrow` are not core valtypes. | Prevents invalid cross-domain values while still sharing the store. |
| TMD4 | **Normalize component instance/component declarations by name in the intern key**, while retaining source order for diagnostics. | §3 allows representation and interning-key choices, but only if the spec's equality relation is confirmed and tested against `wasm-tools`. | Stable structural identity without depending on binary declaration order. |
| TMD5 | **Keep the `TypeRegistry` name and core API**, adding component-domain operations rather than replacing the class in one step. | §3 allows a store rename; the existing callers and tests depend on the core surface. | Incremental migration with a working engine at every step. |
| TMD6 | **Do not build any Canonical ABI or runtime lowering in this refactor.** | The CM-1d/runtime work is explicit in `COMPONENT_MODEL.md` §7; this note only changes type storage and identity. | The refactor can be validated without a component runtime. |

Guardrails:

- Never intern a resource structurally; a resource record is nominal even when its
  representation matches another resource.
- Never let a component-local index become a `TypeId` or a `TypeRef` in a stored
  record after validation.
- Never use a hash hit as identity; confirm with full structural equality.
- Never `reserve()` an input-derived count before `SAFETY.md`'s remaining-input
  check.
- Never put a backend name, type or API into the `WasmBase/` type-store contract
  (`../LAYERING.md`).


## 7. Plan

### 7.1 Target identity model

Keep `TypeId` as the dense process-wide identity and add a domain tag to the
record it names:

```text
TypeId { uint32_t value }                 // existing strong type
TypeDomain = Core | Component
TypeKind    = CoreFunc | CoreStruct | CoreArray
            | ComponentPrimitive | ComponentRecord | ComponentVariant
            | ComponentList | ComponentFixedList | ComponentTuple
            | ComponentFlags | ComponentEnum | ComponentOption
            | ComponentResult | ComponentOwn | ComponentBorrow
            | ComponentStream | ComponentFuture | ComponentMap
            | ComponentFunc | ComponentComponent | ComponentInstance
            | ComponentResource
TypeRef  { TypeDomain domain; TypeKind kind; uint32 index } // 8 bytes
TypeSpan { uint32 first; uint32 count }   // 8 bytes
TypeRecord {
  TypeKind  kind;       // 2
  uint16    flags;      // final, mutable, async, nullable, ...
  uint32    payload;    // index into a payload pool
  TypeSpan  supertypes; // core only
}                                         // 16 bytes
```

The alternative — separate `CoreTypeId` and `ComponentTypeId` wrappers sharing one
store — is compatible with the same layout. The plan prefers the single
domain-tagged ID because it gives one intern table and one identity space, while
the domain tag still makes an accidental core/component comparison a rejected
operation rather than a silent equality.

Component-local indices remain scope-local and must not be stored as `TypeId`s.
The parse/validation path carries a `ScopedTypeIdx` (or equivalent component/scope
pair) and materializes a `TypeId` only after the definition is resolved. The
validator keeps a `localTypeIds` map for diagnostics and for the transition
between the two spaces.

### 7.2 Target records and payload pools

The long-lived representation is a structure-of-arrays store:

```text
TypeStore
  records:        vector<TypeRecord>        // indexed by TypeId.value
  coreFunc:       vector<CoreFuncPayload>
  coreStruct:     vector<CoreStructPayload>
  coreArray:      vector<CoreArrayPayload>
  compFunc:       vector<ComponentFuncPayload>
  compRecord:     vector<ComponentRecordPayload>
  compVariant:    vector<ComponentVariantPayload>
  compLabels:     vector<ComponentLabelPayload>
  compComposite:  vector<ComponentCompositePayload>
  compResource:   vector<ComponentResourcePayload>
  fields:         vector<ComponentField>
  cases:          vector<ComponentCase>
  params/results: vector<LabelValueTypeRecord>
  decls:          vector<ComponentDeclRecord>
  strings:        StringTable
  intern:         unordered_map<InternKey, TypeId>
```

The exact payload set is an implementation choice; the invariants are:

- `TypeSpan` is an `(offset, count)` pair into a store-owned pool, not a pointer.
  Pool growth does not invalidate a view.
- `TypeRecord` is 16 bytes with `isFinal` in `flags`. Core `depth` is derived
  from the supertype list and lives in a side `coreDepth` array (or a packed
  field); keeping it out of the record preserves the 16-byte target without
  losing the `match` optimization.
- Payloads are per-kind. A function type does not reserve struct/array storage;
  a record type does not reserve function vectors.
- `ComponentField { StringId name; TypeRef type; }`, `ComponentCase { StringId
  label; TypeRef type; flags; }` and `ComponentDeclRecord { StringId name;
  TypeRef type; sort; flags; }` are small, shared pool entries.
- `ComponentValueType` compresses to a tag plus a primitive or `TypeId`/pool
  index; the core `ValueType` remains unchanged for the core/JIT path.
- The store owns all pools; modules/components hold IDs and spans.

A function type's fixed cost becomes one 16-byte `TypeRecord` plus one payload
entry and two `TypeSpan`s. Lists that repeat (parameters, results, fields, cases,
declarations, labels) live in shared arrays and can be interned independently.


### 7.3 Strings and names

Add a `StringTable` keyed by content, returning a 32-bit `StringId`. Use it for
field/case/flag labels, parameter labels, declaration names, import/export names
and attribute text. A `StringId` is 4 bytes; a `std::string` is 32 bytes before
its heap payload on the current toolchain. The table must still enforce
input-derived length bounds (`SAFETY.md`) and must not be used as a way to avoid
lexical validation of names.

### 7.4 Component identity, resources and open types

The store must distinguish three things that are easy to conflate:

1. **Closed component types** — fully resolved, no free type imports. These are
   interned and receive a `TypeId` with `TypeDomain::Component`.
2. **Resource types** — nominal leaves. A resource definition, import or
   instantiation allocates a fresh `TypeId` with `ComponentResource` kind; an
   `eq` alias reuses that ID; `sub resource` allocates a new abstract resource.
3. **Type schemes** — a component type with an unsatisfied type import. Store it
   as a scheme with explicit type variables/parameters, not as if it were a
   closed `TypeId`. At instantiation, substitute actual resource IDs and intern
   the resulting closed type. This is the CM-1d boundary; the type-store refactor
   must not pretend it is solved by a flat `TypeId`.

For `instance`/`component` types, intern the declaration set by name after
confirming the spec's equality rules. Records and variants preserve label/case
order; instance/component declarations may be normalized for identity but keep
source order in the diagnostic view. This is TMD4 and needs a `wasm-tools`-based
test before it is treated as settled.

### 7.5 Core compatibility views

The refactor should not force a simultaneous rewrite of the validator, the JIT
type translation or the runtime:

- Add `SubtypeView`, `FuncTypeView`, `StructTypeView`, `ArrayTypeView` and
  `CanonicalTypeView` over `TypeId` + the store. Expose `size()`, `begin()/end()`
  and `operator[]` where existing loops need them.
- Keep `Subtype` as the parser-facing structure until the consumers are migrated;
  the registry converts it into compact records at the single interning boundary.
- Keep `TypeRegistry::canonicalType(TypeId)` returning a view (or a type alias
  with the same accessors) so `match` and the JIT cache keep one identity source.
- Remove the second long-lived core copy only after the view-based consumers are
  green; until then, `CanonicalType` is a temporary migration scaffold, not the
  target.
- TM-6/TM-7 are complete for core modules: the module-owned blocks are deleted
  and the JIT/validator/runtime consumers use `CoreTypeView` or Store-derived
  signature views. `Subtype` is now transient parser scratch; `CoreRecType`
  groups intern to `TypeId`s and clear it at validation. §7.10.3 is the contract
  for the cutover, including the cross-module GC `subtypeForTypeId` change.

### 7.6 Target footprint

The following are targets, not measured claims. The first milestone pins the
baseline with `sizeof` and allocation counters; the last compares against it.

| Current object | Current fixed cost | Target |
|---|---:|---|
| `Subtype` | 88 B + per-list heap | 16-byte `TypeRecord` + pooled payload + 8-byte spans |
| `CanonicalType` | ~144 B + four vectors | The same record/view; no second payload copy |
| `DefinedValueType` | ~216 B | 8-byte value reference + small payload |
| `ComponentTypeDef` | ~344 B | 16-byte record + payload |
| `ComponentDecl` / `InstanceDecl` | ~360 B / ~256 B | 12–16-byte declaration + pooled list entry |
| `ResolvedValueType` | ~240 B + shared-pointer control block | Eliminated; `TypeId`/`TypeRef` |
| `ResolvedType` | ~88 B + shared-pointer control block | Eliminated; `TypeId` |
| `ComponentValueType` | 12 B | 8 B |
| A label/name `std::string` | 32 B + heap | 4-byte `StringId` |
| A populated type list | 24-byte vector + heap | 8-byte `TypeSpan` into a pool |

Revision 2 implements the `TypeRecord`/`CoreTypeView`, the 8-byte
`ComponentValueType`, the `ComponentTypeDef` handle and the declaration variant.
`Subtype` (parser/JIT compatibility) and the validator's `ResolvedValueType`
graph remain; their removal/replacement is TM-1/TM-4 work.

The design target is not a single percentage. It is that a type-heavy fixture no
longer pays for every alternative and no longer performs a separate allocation
for every parameter/result/field/case/label list. TM-0 records the actual baseline
and sets the acceptance threshold.


### 7.7 Staged migration

Each stage leaves the engine working. The order is deliberately identity-first
for the component domain, because CM-1d needs a stable identity to substitute
through type imports, and representation-first for core, because the JIT already
consumes the core layout and needs a compatibility view.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TM-0 — Baseline and ownership** | Measure `sizeof`/allocation counts for `Subtype`, `CanonicalType`, `DefinedValueType`, `ComponentTypeDef`, `ComponentDecl`, `ResolvedValueType`, `ResolvedType`; state the store owner and budget. No behaviour change. | TM8, TM9 (partly) | The measurements and the owner/budget decision are recorded; the existing suite is unchanged and green. |
| **TM-1 — Core compact store** | Introduce the store skeleton, `TypeRecord`/`TypeSpan`, core payload pools and `SubtypeView`/`CanonicalTypeView`; convert `TypeRegistry` interning to store compact records; keep `Subtype` as the parser-facing input. | TM2 (core side), TM4 (core), TM5 (core), TM9 | Core parsing/interning/JIT tests stay green; one function type no longer allocates its parameter/result vectors separately; `canonicalType` is a view, not a copied payload. |
| **TM-2 — Component compact records** | Replace `DefinedValueType` and `ComponentTypeDef` alternatives with tagged records/payloads; add the `StringTable`; make the parser write compact component nodes while retaining diagnostic source data. | TM3, TM4 (component), TM5 (component), TM9 | Component fixtures and invalid cases stay green; the compact store is the source of truth; labels/names are `StringId`s. Any temporary resolved graph is a named scaffold for TM-4, not a second long-lived representation. |
| **TM-3 — Unified component identity** | Add `TypeDomain::Component`, component interning, scope-local → `TypeId` mapping, nominal resource IDs, and explicit type schemes for `sub resource`. | TM1, TM6, TM7 | Equal component types from different components share a `TypeId`; resources stay distinct; a type-import scheme can be substituted without confusing a parameter with a resource ID. |
| **TM-4 — Remove duplicate storage** | Delete the second long-lived core copy and the validator-only resolved graph; keep only diagnostics/location data and views. | TM2 (remainder), TM3 (remainder), TM5 (remainder) | The only stored type structure is the store's; footprint counters show the expected drop; all suites green. |
| **TM-5 — API and documentation cleanup** | Decide whether `Subtype` remains a view or is removed; retain `TypeRegistry` aliases where useful; update `TYPE_IDENTITY.md`, `COMPONENT_MODEL.md` and this note. | — (cleanup; no open gap) | Public consumers have one documented way to inspect a type; no compatibility shim is undocumented. |

TM-3/TM-4/TM-5 are the original broad milestones. §7.10.6 splits their remaining
work into the concrete TM-6…TM-11 sequence: TM-8/TM-9 complete the component
identity work of TM-3, TM-6/TM-7 complete the duplicate-storage removal of TM-4,
and TM-11 supplies the ownership/dedup gates that TM-5 previously left implicit.

### 7.8 Open decisions

These need a decision during TM-0/TM-1, not after the layout is committed:

1. **One ID space vs. two typed wrappers.** The plan recommends one domain-tagged
   `TypeId` space; the alternative is `CoreTypeId`/`ComponentTypeId` over one
   store.
2. **Store-owning scope.** The plan recommends a process-lifetime store owned by
   `Store`, not a `Component`-owned graph, so nested components and future
   instantiation share identities.
3. **Core compatibility duration.** The plan recommends views first; a direct
   `Subtype` replacement is a later option only if instrumentation shows it is
   worth the consumer churn.
4. **Component declaration order.** The plan recommends normalized keys for
   instance/component declarations, gated on a `wasm-tools` equality check.
5. **Scheme representation.** Type parameters must be explicit in the store; a
   flat `TypeId` with an `abstract` flag is not sufficient for substitution.
6. **ABI metadata ownership.** The plan recommends a Store-owned
   `(TypeId, CanonOptsKey) -> CanonicalAbiSignature` cache so multiple component
   instances share it, while per-instance handle tables and substitutions are
   instance state.
7. **JIT cache scope.** `jit_type_t` is per backend context, not per Store; the
   plan keeps the canonical `TypeId` mapping in `WasmBase` and the lowered cache
   in the backend context.

### 7.9 Compatibility risks

| Risk | Impact | Mitigation |
|---|---|---|
| Core/component ID confusion | A core type accidentally satisfies a component operation, or vice versa. | Domain tag on every record; reject cross-domain `match`; tests for both directions. |
| Over-interning resources | Two distinct resources collapse into one identity, breaking handle/generativity semantics. | Nominal resource records; never use the structural intern table for `ComponentResource`; explicit resource-freshness tests. |
| Scheme/closed-type confusion | A type parameter is treated as a concrete resource, so substitution produces the wrong type or misses an instantiation. | Separate scheme representation with explicit type variables; substitute before interning the closed result. |
| View lifetime | A `TypeSpan` or record view outlives or is invalidated by store growth. | Index-based records/pools; views hold a store reference and offsets, never raw pointers; ASAN test. |
| JIT consumers see changed access | Validator/JIT code expects `Subtype`/`CanonicalType` fields and allocation behaviour. | Compatibility views first; keep the existing suite green at every stage; no layout change to the core value types. |
| Declaration-order identity | Two instance/component types that should be equal intern differently, or two that differ are merged. | `wasm-tools` equality test; normalize only the declaration key, preserve source order for diagnostics. |
| Allocation-budget regression | A malformed type count causes a large reserve before validation. | Remaining-input checks before every reserve; allocation-budget tests; reject, do not abort. |
| Lost diagnostics | Pooling and `StringId` interning discard source labels/locations needed by errors. | Keep a diagnostic side table (source offsets/StringIds) until validation errors are emitted; do not make identity depend on it. |
| Two owners or duplicate records | Two modules/components keep separate copies of the same canonical type, defeating identity and footprint goals. | One-owner invariant in §2.3; TM-6/TM-11 tests assert no retained module/component type payload and one record per closed type. |
| JIT cache crosses contexts | A `jit_type_t` from one backend context is reused in another, or a module-local key aliases a different canonical type. | Key the cache by `TypeId` inside one `jit_context`; keep local-index → `TypeId` as the only module-local map. |
| ABI cache aliases options | The same `TypeId` is lowered with different `string-encoding`/`memory`/`realloc` options and returns the wrong signature. | Key ABI metadata by `(TypeId, CanonOptsKey)`; test that equal types with different options are distinct entries. |
| Scheme/closed-type aliasing | An open scheme gets a closed `TypeId`, so substitution or resource generativity is lost. | Separate scheme table/ID; only resolved closed types enter the closed intern table. |
| API churn | `TypeRegistry`/`Subtype` consumers break during the transition. | Keep core APIs and `Subtype` parser input; add aliases/views; remove only after all consumers are migrated. |

### 7.10 Completion plan — canonical ownership and remaining layers

This section is the implementation order for the remaining type-model work. It
does not add code; it fixes the contracts that the next changes must satisfy.

#### 7.10.1 The ownership invariant

The final ownership chain is:

```text
Store                         process lifetime
  └─ TypeRegistry / TypeStore sole owner of all type definitions
       ├─ TypeRecord[]        core and component
       ├─ payload pools       core params/results/fields, component fields/cases/...
       ├─ StringTable
       ├─ intern tables       closed core/component types
       └─ scheme tables       open component types (not closed TypeIds)

Module / Component             parser and local-index state only
  ├─ local index -> TypeId
  ├─ RecGroupId / TypeSpan
  └─ handles to Store records

ModuleInstance / ComponentInstance
  ├─ TypeIds and resource IDs
  ├─ substitution maps and handle tables (runtime state, not type definitions)
  └─ backend caches keyed by TypeId (per backend context)
```

The hard rule is: **after a parse completes, no `Module` or `Component` owns a
type payload.** A `Module` may own its `std::vector<TypeId>` mapping and a
`RecGroupId` list; a `Component` may own local-index handles; neither may own a
`Subtype`, a compact record, a component declaration payload or a resolved type
graph. `Store` must survive every module/component instance, so all views are
index-based and remain valid for the process lifetime.

#### 7.10.2 Canonical partition: one instance per type

`TypeId` is a canonical identity, not an allocation counter. The target pipeline
is:

```text
parser bytes
  -> temporary PendingCoreGroup / PendingComponentType
  -> structural key (+ full equality check)
  -> one TypeId and one TypeRecord
  -> discard the temporary syntax object
```

The key rules:

- **Core:** the key is the whole `rec` group, with in-group references encoded
  as member positions and out-of-group references encoded as `TypeId`s. The key
  covers finality, supertypes, function params/results, struct fields and array
  element. A bare type and a one-member `rec` are the same key.
- **Component:** the key is the *closed* component type: kind, ordered labels
  (via `StringId`), ordered fields/cases, `TypeId` references, and nominal
  resource IDs. Instance/component declarations are included only after the
  equality rule for declaration order is pinned against `wasm-tools`; source order
  remains in the diagnostic view.
- **Excluded from structural interning:** `ComponentResource` records, open
  schemes, type parameters and substitutions. They are identity-bearing, not
  structural; they use separate tables.
- **No fallback unique IDs.** Every closed definition must come from an intern
  hit or an intern miss that creates exactly one record. There is no
  “allocate a private `TypeId` for this module” path.
- **No retained syntax.** Core modules no longer have `TypeRegistry::blocks` or
  `Module::types`; validated component `CoreRecType` groups retain only
  `typeIds` and clear their parser `Subtype` scratch. Component/instance
  declaration ASTs remain, but their compact `ComponentDeclRecord` payloads are
  Store-owned.

The core and validated-component stores now match the ownership target.
“Same type” is “same `TypeId`” for both domains; `Component::localTypeIds`
provides the local-index bridge.

Acceptance metrics for TM-6:

- **Done (core):** `TypeRegistry::blocks`/`TypeBlock` and `Module::types` are
  gone; `Module::typeCount()` is `typeIds.size()`, not syntax storage.
- `parsedTypeCount()` remains a diagnostic count of definitions seen, not
  retained storage.
- **Done (core):** a repeated module load leaves the canonical store unchanged;
  two modules with the same signature share a `TypeId`.
- **Done:** the only `Subtype` during parsing is temporary parser scratch; its
  lifetime ends at interning for modules and at validation for component core
  types.
- **Done:** a component's `CoreRecType` stores `typeIds`, not `Subtype` vectors,
  after validation.
- **Partial:** `ComponentTypeDef` for component/instance types still keeps its
  parser declaration vectors, though the compact `ComponentDeclRecord` form is
  Store-owned. Removing the AST form waits on the declaration equality rule.

#### 7.10.3 JIT-facing view cutover

The JIT must consume the store, not the compatibility AST:

- `WasmBase` exposes `CoreTypeView` (already present) with `kind`, `isFinal`,
  `supertypes`, `funcParams`, `funcResults`, `structFields`, `arrayElement` and
  `depth`. Backend code receives this view; it never names a backend type from
  `WasmBase`.
- `Module` exposes `typeId(LocalTypeIdx)` and `typeView(LocalTypeIdx)`; it no
  longer exposes a raw `Subtype` span.
- `LibJitTypeTranslator::translateTypes` changes from
  `span<const Subtype> + span<const TypeId>` to a local-index → `TypeId` view plus
  the store. Its `translatedTypes` cache becomes a per-`jit_context`
  `TypeId -> jit_type_t` map; the module-local parallel vector remains only as a
  call-site convenience, or is derived from the local mapping.
- `LibjitModuleCompiler` obtains function signatures from
  `store.coreType(typeId)`; `LibjitOpcodeDispatcher` obtains block/function types
  the same way.
- `ModuleInstance::subtypeForTypeId` becomes `store.coreType(typeId)` rather than
  a linear scan over the current module. This also fixes the cross-module GC
  case: an object allocated by one module can be walked using its canonical type
  even when the walking module did not declare that type.
- `WasmValidator` is migrated after the JIT: it can keep a temporary
  local-index → view cache while its rules change from field access on `Subtype`
  to `CoreTypeView` accessors.
- The final `Subtype` is parser-only (or is renamed to a syntax type). No JIT
  cache, module, component, instance or store record may own one.

Acceptance for this cutover: no `LibJit*` source includes a `Subtype`-typed
module layout; two modules with the same canonical function type share one
`jit_type_t` per context; cross-module GC walking uses the store view; the full
core/GC/JIT suite stays green.


#### 7.10.4 Component schemes, resources and scope mapping

The component type resolver must distinguish three identities:

1. **Closed component type** — a fully resolved component type with no free
   type parameters. It is structurally interned and has a `TypeId`.
2. **Resource identity** — nominal. A resource definition, import or
   instantiation gets a fresh `ResourceId`; `eq` aliases share it; `sub resource`
   introduces a fresh abstract resource parameter. A resource is never
   structurally interned.
3. **Type scheme** — an open component type containing one or more type
   parameters. It is stored in a separate scheme table and is not a closed
   `TypeId`. It can be alpha-equivalent to another scheme, but it cannot be used
   where a closed type is required until instantiated.

The resolver pipeline is:

```text
parsed component declarations
  -> scope-local type indices
  -> TypeScheme / open record
  -> substitution map (TypeParamId -> TypeId/ResourceId)
  -> closed TypeId (interned)
```

`TypeScope` keeps only the local-index mapping during validation; the resolved
closed type is interned in `TypeStore`. `TypeSubstitution` is runtime/instance
state (or a store-owned substitution handle), not a type payload. A component
instance's resource IDs are fresh per instantiation even when its scheme is the
same as another instance's.

The remaining scope rules:

- A `sub resource` import creates a type parameter/resource placeholder, not a
  concrete resource.
- `type eq` aliases share the target `TypeId`; `type sub resource` does not.
- Ordinary type imports/exports use structural equality on closed types.
- Resource methods, constructors and destructors are resolved as part of the
  resource identity, not as ordinary function-type equality.
- No stored closed record may contain a component-local index after resolution.

#### 7.10.5 Canonical ABI and runtime metadata

The Canonical ABI needs a derived type table, not a second type identity:

```text
TypeId + CanonOptsKey
  -> CanonicalAbiSignature
       flat params/results, maximum flat arity, memory layout,
       size/alignment, string encoding, resource-handle shape
```

`CanonOptsKey` covers the options that change lowering but not type identity:
`string-encoding`, `memory`, `realloc`, `post-return`, `async` and any future
option. The same `TypeId` may therefore have multiple ABI signatures; the cache
is keyed by both. The table is store-owned (or a store-owned derived cache) and
is built only after a component type is closed and a `canon` definition is
validated. It must not change structural identity or `matches`.

The runtime contract:

- `canon lift`/`lower` resolves its component type to a `TypeId`, then to a
  `CanonicalAbiSignature`; the lowered core signature comes from the core
  `CoreTypeView`.
- Flattening/joining use the component `TypeId` view: records, variants,
  tuples, flags, enums, options, results, lists, fixed lists, maps, strings and
  resources. `char` and string encodings follow the chosen `canonopt`.
- Resource handles use nominal `ResourceId`s and a per-instance handle table;
  `own`/`borrow` lifetimes, `resource.new/drop/rep` and destructors are runtime
  state, not structural type data.
- Memory/realloc/post-return paths are represented in the ABI signature so the
  thunk layer does not rediscover layout per call.
- A validated `canon lift`/`lower` stores a `CanonicalAbiSignatureId` (or an
  equivalent stable handle) plus the core `TypeId` and component `TypeId`; the
  signature cache is Store-owned, while the thunk/callable and its `VMContext`
  state are per instance/context.
- ABI metadata is **inert**: it does not enter the type intern key, does not
  change `matches`, and cannot make two structurally equal types distinct.
- Out-of-range, double-drop, use-after-drop and borrow-escape behavior is a trap
  or link rejection as specified in [`../SAFETY.md`](../SAFETY.md); it is never
  undefined behavior.

This plan deliberately stops before implementing codegen. The type-model
deliverable is the ABI metadata contract and its ownership/lifetime rule; the
actual thunks belong to `COMPONENT_MODEL.md` CM-4/CM-5/CM-6. The runtime order
after TM-10 is:

1. flat `lift`/`lower` for scalars, chars and handles (CMD5);
2. `memory` + `realloc` + `post-return` and the utf8 string subset;
3. resource handle tables, `own`/`borrow`, resource builtins and destructors;
4. async/stream/future/task paths only if they remain in scope; they are
   deliberately gated non-goals in `COMPONENT_MODEL.md`.


#### 7.10.6 Completion milestones and gates

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TM-6 — Store-owned canonical partition** | ◑ Core modules and validated `CoreRecType` groups are done: parse a temporary group, intern it to `TypeId`, retain only local maps/views. Remaining: component/instance parser declaration vectors (compact Store records already exist). | TM10 (core/component-core) | Core modules and component core types retain no type syntax; `TypeRegistry` stores one record per canonical type. Component/instance AST declarations remain. |
| **TM-7 — JIT view cutover** | ✅ Core modules done: `LibJitTypeTranslator`, `LibjitModuleCompiler`, `LibjitOpcodeDispatcher`, `ModuleInstance`, `WasmValidator` use `CoreTypeView`/`TypeId`; `funcTypeAt` is a Store-derived compile/test signature view. | TM11 | No JIT/validator path reads `Subtype` from a module; cross-module GC walking uses the store. |
| **TM-8 — Component closed-type interning** | ✅ `TypeStore` interns closed records; after validation every nested reference is a canonical `TypeId`, including local references and `own`/`borrow`. `Component::localTypeIds` is canonical, type-bearing component/instance declaration sets intern structurally, and component core types are Store-owned. Parser syntax records remain an AST. | TM12 (scheme residue) | Closed for closed types; schemes/resources are TM-9/TM-13. |
| **TM-9 — Schemes and resources** | ✅ `SchemeId` alpha-equivalent scheme interning, abstract-resource placeholders, substitution through instantiate and concrete-resource generation per instantiation. | TM13 | `eq` aliases share, `sub resource` gives a fresh parameter, two substitutions produce distinct closed types when resources differ, and a repeated resource interns back. |
| **TM-10 — Canonical ABI metadata** | ◑ First slice: `CanonicalAbiSignature` keyed by `(TypeId, CanonicalAbiOptions)`, flat params/results for the scalar/string/resource/record/tuple/list/fixed-list subset. Remaining: variants/results/options/maps/streams, layout, and lift/lower. | TM14 (partial) | A closed component function has a stable ABI signature; flat fixtures resolve it; no ABI cache aliases different options. |
| **TM-11 — Ownership/dedup/ABI gates** | ◑ Assert one record per canonical type, no retained `Subtype` in Module/Component, store-owned views, open-scheme non-identity and ABI cache identity. Remaining: parser AST removal and ABI runtime/layout. | TM15 (partial) | Tests fail if any module/component owns a type payload, an open scheme gets a closed id, or identical closed types get distinct ids. |

The order is deliberate: TM-6 removes the duplicate raw owner; TM-7 proves the
existing consumers can work from store views; TM-8–TM-10 add the component
identity/ABI layers on top of a single owner. TM-11 is the regression gate that
prevents future code from reintroducing per-module type storage.


### 7.11 Blockers and out-of-proposal dependencies

The completion plan splits into internal gating work and decisions another
proposal owns. This section is the authoritative status list; the §5 rows keep
their IDs.

#### 7.11.1 Core consumer cutover (TM-6/TM-7): core modules and `CoreRecType` closed

Revision 5 closes the core-module half. The solution is a Store-boundary adapter,
not a change to `ValueType`:

- `Module::processTypeSection` interns a temporary `std::vector<Subtype>` and
  discards it. `Module` retains `typeIds`, `typeGroups` and a TypeId→local-index
  map (indices only, no type payload). `Subtype` is gone from `Module`.
- `Module::typeView(idx)` returns `CoreTypeView`; `Module::funcTypeAt(idx)`
  materialises a Store-derived `FuncType` for compile/test paths that still need
  the old signature shape. `Module::isSubtype` compares canonical `TypeId`s via
  `TypeRegistry::matches`.
- `WasmValidator` checks type well-formedness from `CoreTypeView`; `refSubtype`
  still uses module-local `ValueType.heapType` on the operand stack, but resolves
  concrete identity through `Module::isSubtype`/canonical `TypeId`s. It never
  reads a module-owned `Subtype`.
- `LibJitTypeTranslator::translateTypes(typeIds, store)` lowers from
  `CoreTypeView` and retains the `TypeId`-keyed canonical cache. The old
  `span<Subtype>` overload is test-only.
- `LibjitOpcodeDispatcher` struct/array helpers return `CoreTypeView`; field
  types go through `translateCanonStorage`. `LibjitModuleCompiler` uses
  `typeView`/`funcTypeAt`; `PreparedFunctionStack` now takes `FuncType`.
- `ModuleInstance` GC walking, reclamation and constant-expression GC
  construction use `CoreTypeView`/`CanonStorageType` and `gcStorageView`.
  `WastScript` uses `funcTypeAt` for its test-side callable signature.

`CoreRecType` is now Store-owned too: `TypeRegistry::internGroup` interns a
group against the component local map, `CoreRecType::typeIds` receives the
result, and the parser `subtypes` vector is cleared. `validateCoreTypeDef` uses
`TypeId` slots (never `Subtype*`) for module-type declarations. The `FuncType`
compatibility view remains Store-derived and does not reintroduce a module-owned
type block.

#### 7.11.2 Internal gating: component local indices and declarations (TM-8/TM-12) — closed

Revision 8 closes the internal part of this gate:

0. **Done:** `CoreRecType` stores `typeIds` after `validateCoreTypeDef` interns
   the group through `TypeRegistry::internGroup` and clears its parser
   `subtypes`; module-type declarations use `TypeId` slots.
1. **Done:** `ComponentValidator::run()` publishes `Component::localTypeIds`
   over the component type index space, including defined
   value/function/resource, component and instance types; schemes remain
   unrepresented.
2. **Done:** `ComponentValueType::Kind::TypeId` carries a canonical Store id.
   After validation the resolver rewrites every value/function/instance/
   component node into Store records with `TypeId` children; primitive children
   still use `Kind::Primitive`. `TypeStore::addComponent*` therefore interns
   records with local references exactly like primitive-only records.
3. **Done for type-bearing declarations:** component/instance declaration sets
   with func/type/instance/component records are structurally interned.
   `ComponentDeclRecord` has no core payload for memory/global/table entries, so
   a set containing one stays nominal rather than conflating unrelated core
   types with the same name/sort.
4. **Done:** `own`/`borrow` use `addComponentTypeRefCanonical`, keyed by the
   nominal resource `TypeId`; `eq` aliases share that id and fresh resources do
   not. `sub resource`/open schemes remain §7.11.3.

Two representations intentionally coexist: `Component.types` is the parser AST
and keeps `Kind::TypeIndex`; the canonical Store record reached through
`Component::typeId(localIndex)` is the identity. `ComponentTypeDef::id` is the
parser id and may differ from the canonical id for a type with local references.
The `$listRecord` test pins the distinction. Removing the parser AST is TM10, not
a correctness blocker for the canonical store.

#### 7.11.3 Schemes and type imports (TM-9/TM-13) — identity closed

`sub resource` imports and open component types are Component Model **CM-1d**
work ([`COMPONENT_MODEL.md`](COMPONENT_MODEL.md)). Revisions 8–9 give an abstract
`sub resource` import a fresh nominal Store `TypeId`
(`ComponentResourcePayload::abstract`), so it appears in
`Component::localTypeIds` and `own`/`borrow` intern against it. At
`instantiate`, `ComponentValidator` matches each abstract resource import to the
provided resource and rewrites the component signature's exports through
`substituteValue`/`substituteType`; `ComponentInstance::typeId` records the
canonical, substituted instance type. Two instantiations with different
resources therefore get different type ids, while reusing the same resource
interns back to one id.

Revision 9 also makes resources defined by the instantiated component
generative: `ResolvedComponentType::generativeResources` drives a fresh nominal
`TypeId`/resource id per instantiation, so two instances of the same definition
get distinct exported resource types. A component/value/func type that contains
an abstract resource is an open scheme: `Component::typeId` returns `kNone`, and
`Component::schemeId` returns its alpha-equivalent `SchemeId` (parameter
placeholders assigned by first occurrence).

Core-module component imports now compare the declared module type
structurally: module declaration kinds/names, canonical local type `TypeId`s,
and table/memory/global shapes. An embedded module with no parsed module type
still relies on its export ascription; the comparison is exact whenever both
sides carry type metadata.

`SchemeId` is the first-class open-type identity; `TypeScheme::parameterCount`
records arity, and the intern key is alpha-normalized. The remaining CM-1d rules
(annotated-name rules and Canonical ABI signature checks) are outside this
note. External visibility for resource/record/variant/enum/flags types is
implemented at the import/export boundary. Blocker for the rest: `COMPONENT_MODEL.md` CM-1d.

#### 7.11.4 Canonical ABI metadata (TM-10/TM-14) — flat/layout slice

`TypeStore::canonicalAbi(TypeId, CanonicalAbiOptions)` now derives and caches
flat params/results for component function types keyed by both the canonical
type and the options that affect lowering (`string-encoding`, memory, realloc,
post-return, async). The first slice covers primitives, strings, lists, fixed
lists, records, tuples and `own`/`borrow`; it is inert and never enters the
type intern key. A 1024-value/depth-64 cap prevents input-derived fixed-list
lengths from driving unbounded metadata allocation.
`canonicalAbiLayout` adds size/alignment for the same value shapes plus enums,
flags, options, results, variants, maps, streams and futures; records and tuples
apply field alignment/padding, and strings/lists use the options' 32- or 64-bit
pointer width. Joins are conservative: incompatible variant/result payload
shapes are reported unsupported rather than guessed.

A flat execution slice now exists in `LibJIT::ComponentRuntime`: embedded core
instances are instantiated, `canon lift`/`lower` run through libffi, and host
component function imports can be linked. Exact canonical join promotion for
incompatible payloads, per-encoding string layout, resource handle tables and
memory-backed values remain. Those are `COMPONENT_MODEL.md` CM-4…CM-7;
[`ABI.md`](../ABI.md)'s host-reference model
(R2/R3/R4) and GC `G3` own the `externref` representation the resource/handle
path will consume. Blocker: CM-4…CM-7 plus `ABI.md` R4.

#### 7.11.5 Internal: gates (TM-11/TM-15)

Implemented in revisions 4–10:

- `WASM::Module` has no `types` member (compile-time gate), and a repeated core
  module load yields the same `TypeId`s without growing the canonical store;
- `Module::typeView`/`funcTypeAt` are the only type accessors; the JIT, runtime
  GC walker, constant-expression GC construction and validator use Store views;
- closed component records share across components; label order is significant;
- canonical payloads use `Kind::TypeId`: a local-reference list shares its id
  across loads, and primitive-only records keep the same id as their parser
  record;
- resources stay fresh; `own`/`borrow` reference the nominal resource `TypeId`,
  and parser syntax records with a local index are deliberately not interned;
- `Component::localTypeIds` publishes canonical ids; type-bearing
  component/instance declaration sets intern structurally across loads;
  abstract resource imports substitute during instantiation and
  `ComponentInstance::{typeId,schemeId}` pin the canonical result; locally
  defined resources regenerate per instantiation; open types intern as separate
  alpha-equivalent `SchemeId`s; and `CoreRecType` groups retain `TypeId`s only
  (parser `subtypes` cleared);
- `CanonicalAbiSignature` is cached by `(TypeId, CanonicalAbiOptions)`; equal
  types/options reuse the signature, different options do not alias;
- `canonicalAbiLayout` derives size/alignment for the supported value shapes,
  including record padding and 32/64-bit pointer widths;
- core-module component arguments are matched by module-type structure, not just
  `CoreSort::Module`.

Still gated:

- “no parser AST in `Component`” — `Component.types` and component/instance
  declaration vectors remain the parser AST; canonical records are separate and
  already interned (§7.11.2);
- remaining CM-1d validation rules (`[get]`/`[set]` semantics and ABI signature
  checks) — §7.11.3;
- variant/option/result/map/stream ABI join and layout, plus lift/lower runtime
  — CM-4…CM-7 (§7.11.4);
- the ASAN/allocation-budget footprint sweep over the full suite — the counters
  and the new identity paths are tested, but the full sweep remains a CI gate.


## 8. Indicative test survey

The refactor is gated by the existing behaviour suite first and by new identity
and footprint tests second. The counts below are the baseline at the time of
writing; re-measure rather than trusting them after the first stage.

| Script / test | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| default C++/wast suite | 8,792 assertions / 302 cases | 0 | 0 | Existing core, GC, SIMD, exceptions and component parser/validator/runtime behaviour is the refactor's regression floor, plus the revision-4…11 identity, scheme, visibility, annotation, ABI-metadata and flat-runtime tests. |
| strict validation suite (`WASM_SPEC_VALIDATE=1` and the curated negative sets) | same suite green | 0 | 0 | The compact store must not weaken any validator path, positive or negative. |
| `Test/wasm_wat/component_types.wat` | pass | 0 | 0 | Every component value type, import/export and label shape survives the AST rewrite. |
| `component_coretypes.wat`, `component_coreexterns.wat`, `component_coregc.wat` | pass | 0 | 0 | Core-type and core-extern payloads still map to the same core shapes. |
| `component_{instances,decltypes,resource,async,canonopts,attributes,aliasexport,instanceinline,coreinline,outer}.wat` | pass | 0 | 0 | Component declaration fields, resources, aliases and nested/outer scopes keep their current meaning. |
| invalid component fixtures (reference/type/alias/instantiation/canon/start/label) | pass (rejected) | 0 | 0 | Local-index mapping must reject the same bad inputs before intern. |
| new `sizeof`/allocation assertions | pass | 0 | 0 | Revision 2 pins the compact record sizes and exercises the store's byte/allocation counters; the full target ceiling remains a TM-6 gate. |
| new core identity tests | pass | 0 | 0 | Same `rec` group in two modules -> same `TypeId`; distinct groups -> distinct IDs; `match` unchanged; a loaded Module has no `types` member and retains only canonical ids. |
| new component identity/ABI tests | pass | 0 | 0 | Closed records share across components; label order matters; canonical payloads use `TypeId`; resources stay fresh, `own`/`borrow` reference their nominal resource id, abstract imports substitute, locally defined resources regenerate per instantiation, and open types share alpha-equivalent `SchemeId`s with stable arity; type-bearing component/instance declaration sets intern; `Component::localTypeIds` is canonical; `CoreRecType` retains `TypeId`s; ABI flat shapes/layouts cover the supported subset and the cache is keyed by options. Parser AST ownership is TM10. |
| new footprint benchmark | partial | — | — | `payloadBytes()`/`allocationCount()` include the component intern keys; the full byte/RSS ceiling and no-per-type-list-allocation measurement remain a TM-6/TM-11 gate. |
| ASAN + allocation-budget run | none yet | — | — | No use-after-free from pooled views; every input-derived count is bounded. Remains a CI gate. |

### 8.1 Identity tests to add

- **Core:** **Done:** a function signature repeated in two modules has one
  `TypeId`; two different `rec` groups have different IDs; a bare type and a
  one-member `rec` share an ID; a cross-module `match` still follows the
  supertype chain; `WASM::Module` has no `types` member and a loaded module
  exposes only `typeIds`/`typeView`.
- **Component:** **Done for closed types:** `record { a: u32 }` and
  `record { a: u32 }` from different components share an ID; `record { a: u32 }`
  and `record { a: u64 }` differ; label order in a record is respected;
  optional/result/stream/future payloads share an ID; canonical records with
  local-reference children are rewritten to `TypeId` payloads and intern,
  while parser syntax records keep local indices; `Component::localTypeIds`
  publishes canonical ids; type-bearing component/instance declaration sets
  intern; validated component core groups retain `TypeId`s and clear parser
  scratch. **Remaining:** open/scheme types (§7.11.3) and parser-AST removal
  (TM10).
- **Resources:** **Done for nominal freshness, abstract ids and substitution:**
  two `resource` declarations with the same representation and destructor have
  different IDs; an abstract `sub resource` import gets a fresh nominal id;
  `own`/`borrow` carry the nominal resource `TypeId` and share only for the same
  resource; instantiating with two different resources produces two canonical
  instance types, and reusing one resource re-interns the same id.
- **Scopes:** a nested component's local type index does not resolve in the outer
  component; an outer alias resolves to the outer `TypeId`; canonical Store
  records contain no component-local index. Parser syntax records remain the
  AST and keep local indices (TM10).
- **Schemes:** **Done:** substituting an abstract resource import in an
  instantiated instance's exports produces the expected closed type; two
  different resources produce different `ComponentInstance` type ids; a resource
  defined by the instantiated component is regenerated per instantiation; open
  types have `typeId == kNone` and a stable alpha-equivalent `SchemeId`, with
  `parameterCount` reflecting the abstract-resource arity.
- **Domains:** a core `TypeId` used where a component type is expected is
  rejected; a component `TypeId` never satisfies a core `match`. **Remaining;
  the store has one TypeId space, so the test belongs with the consumer cutover
  (TM-7).**

### 8.2 Footprint tests to add

- `static_assert` the fixed sizes of `TypeRecord`, `TypeSpan`, `ComponentValueType`
  and the declaration records once the toolchain is pinned; do not assert a size
  that depends on libstdc++ internals without a toolchain check.
- Expose `TypeStore::recordBytes()`, `poolBytes()`, `stringBytes()`,
  `internBytes()` and `allocationCount()` (or equivalent counters) and assert:
  - a type with two parameters allocates no per-parameter vector;
  - a component record with N fields allocates one pool run, not N strings;
  - the intern table does not retain a serialized `std::string` payload for every
    group.
- **Done (partial):** `payloadBytes()` counts the component intern keys and
  `allocationCount()` counts the intern map; `componentInternedCount()`,
  `componentInternHits()` and `componentInternMisses()` expose the dedup ratio.
  A full bytes/RSS ceiling benchmark remains open.
- Measure a small, medium and type-heavy fixture before/after with the existing
  tests plus a dedicated benchmark. Record both bytes and allocations; a lower
  byte count with more allocations is not automatically a win.
- Run the negative fixtures under the allocation budget; a malformed count must
  fail before a large reserve, with the same "reject" outcome as today.

### 8.3 What is not implemented or tested yet

No Canonical ABI execution, component instantiation, handle table or WIT
ingestion is part of this type-model pass. The metadata and runtime contracts are
planned in §7.10.4/§7.10.5; the actual thunks remain CM-4…CM-7 in
[`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §7.


### 8.4 Completion gates for TM-6…TM-11

- **Ownership:** a loaded module/component retains no `Subtype`, compact record
  or canonical component type payload. **Core done:** `Module` has no `types`
  member and `TypeRegistry` keeps no parser blocks; validated `CoreRecType`
  groups retain `TypeId`s. **Component done for canonical records:** payloads
  use `TypeId` and type-bearing declaration sets intern. **Remaining:** the
  parser AST (`Component.types`, declaration vectors) is intentionally retained
  for now (TM10), not a canonical identity.
- **Canonical partition:** load the same module twice, then a second module with
  the same signatures; `TypeId`s match and `store.recordCount()` does not grow
  per reload. **Done:** repeated modules are covered by `loaded modules retain
  only canonical type identities`, and a second module with the same signature
  by `singleton rec group interns to the same identity as a plain type`.
- **JIT identity:** two modules with the same canonical function type produce one
  `jit_type_t` per context; `ModuleInstance::subtypeForTypeId` works for a type
  declared only by another module. **Core done:** `JIT type cache is keyed by
  canonical TypeId` covers the lowering cache; the GC walker resolves
  `TypeId`→`CoreTypeView` directly. A direct cross-module GC fixture remains
  desirable but is no longer blocked by module-owned storage.
- **Component closure:** equal closed component types from different components
  share an id; canonical closed records contain no local index; type-bearing
  component/instance declaration sets intern and declaration order is part of
  the key. **Done** except parser syntax records, which are AST and keep local
  indices by design (TM10).
- **Schemes/resources:** `eq` aliases share; `sub resource` creates an abstract
  nominal parameter; two instantiated substitutions with different resource IDs
  produce different closed instance types. **Done:** open types intern as
  alpha-equivalent `SchemeId`s with parameter arity, and never as closed
  `TypeId`s. See `open component types intern as alpha-equivalent schemes`.
- **ABI metadata:** the same `TypeId` under different `canonopt` sets produces
  distinct ABI signatures; the same `(TypeId, opts)` reuses one signature; flat
  scalar/string/resource/record/tuple/list/fixed-list and conservative
  enum/flags/option/result/variant/map/stream/future shapes and layouts are
  covered, including 32/64-bit pointer widths. **Done for the flat/layout
  metadata slice and flat lift/lower execution.** Exact incompatible-payload
  join promotion and resource/memory-backed runtime/traps remain gated by
  CM-4…CM-7 (§7.11.4).


## 9. References

- [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) — core type identity, canonical
  form, `rec`-group interning, matching, and the current `Subtype` footprint.
- [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) — component parser/validator state,
  CM-1d, CM-2 and the Canonical ABI boundary.
- [`../LAYERING.md`](../LAYERING.md) — the backend-agnostic rule for
  `WasmBase/`.
- [`../SAFETY.md`](../SAFETY.md) — artifact detection, input-derived allocation
  bounds, the allocation budget, traps and `std::abort()` policy.
- [`../VALIDATION.md`](../VALIDATION.md) — the validator surface that must stay
  green through the core migration.
- WebAssembly 3.0 specification:
  - Validation — Matching:
    https://webassembly.github.io/spec/core/valid/matching.html
  - Validation — Conventions (expansion, recursive type indices, closed types):
    https://webassembly.github.io/spec/core/valid/conventions.html
- Component Model specification:
  - Explainer and binary format:
    https://github.com/WebAssembly/component-model
  - Canonical ABI:
    https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md
  - `wasm-tools` (reference parser/canonicalizer):
    https://github.com/bytecodealliance/wasm-tools
- Source anchors for the current state:
  - `WasmBase/WasmType.hpp` — `Subtype`, `CompositeDefinition`, `FuncType`,
    `StructType`, `ArrayType`, `StorageType`, `FieldType`, `ValueType`.
  - `WasmBase/WasmCanonicalType.hpp` — `CanonicalType`, `CanonStorageType`,
    `CanonFieldType`.
  - `WasmBase/WasmTypeRegistry.{hpp,cpp}` — `internModule`, `canonicalType`,
    intern table.
  - `WasmBase/WasmModule.{hpp,cpp}` — `typeIds`, `typeView`, `funcTypeAt`, the
    temporary parser buffer and the Store-boundary cutover.
  - `WasmBase/WasmComponentModel.hpp` — `ComponentValueType`,
    `DefinedValueType`, `ComponentTypeDef`, `ComponentDecl`, `InstanceDecl`,
    `ResourceType`, `ExternType`.
  - `WasmBase/WasmComponentValidation.cpp` — `ResolvedValueType`,
    `ResolvedType`, `ResolvedDecl`, `TypeScope`.
  - `WasmBase/WasmComponentParser.inc` — component decoder and AST construction.

