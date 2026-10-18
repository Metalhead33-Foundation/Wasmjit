# Unified Type Store and Component Type Identity — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 6 — core modules are
Store-owned: `Module` keeps only `typeIds`, groups and a reverse index map, and
the JIT/runtime/validator consume `CoreTypeView` or Store-derived signature
views. Closed component records whose nested value types are already
primitive/interned share TypeIds across components, and validation publishes a
component-local type index → `TypeId` map for defined value/function/resource
types; component/instance declarations now have nominal Store records and
TypeIds. The remaining structural component/instance interning, `CoreRecType`
ownership, type schemes/resources and Canonical ABI metadata remain; §7.11
records their exact blockers, including the cross-proposal ones).
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
Component `CoreRecType` still owns parser `Subtype` vectors; that is the
remaining TM-6 sub-item and is coupled to the component local-index resolver in
§7.11.2.

Revision 6 adds the first half of that resolver: `ResolvedType` carries the
parser-assigned `TypeId` for defined value/function/resource definitions, and
`ComponentValidator::run()` publishes `Component::localTypeIds` over the whole
component type index space (including aliases/imports). Component/instance
declarations are copied into compact `ComponentDeclRecord` spans and receive
nominal Store TypeIds. They are not structurally interned yet, because the
declaration-set equality rule still has to be pinned against `wasm-tools`.

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
  bytes in `payloadBytes()` make the reuse measurable. Records containing a
  component-local `TypeIndex`, resources, `own`/`borrow`, and component/instance
  declarations are deliberately **not** interned yet (see §7.11.2).

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
  Resolution is by component-local index. `ComponentValidator::run()` publishes
  `Component::localTypeIds` over that space (aliases/imports included).
  Component/instance declarations get nominal Store records and ids; schemes are
  still unrepresented, so an id can remain `kNone` only for an unvalidated
  component or a future scheme. Store value payloads still use local `TypeIndex`
  references, so the map is a bridge rather than the final canonical form.

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
| Component value type | Component-local `typeIndex` in the value reference; the definition has a store `TypeId`, published as `Component::typeId(localIndex)` after validation | Partial: closed records with only primitive/interned children; local-index records are not interned yet | `TypeStore` + `ComponentTypeDef` handle + `ResolvedType::typeId`/`Component::localTypeIds` + `ResolvedValueType` graph |
| Component resource | Store `TypeId` for the definition; validator-local `resourceId` for the resolved resource | No | `TypeStore` + validator graph |
| Component instance/component type | Component-local index; declarations are copied into compact Store records and get a **nominal** `TypeId` | Nominal only; structural dedup pending declaration equality | `TypeStore` declaration pools + `ComponentDeclRecord`; AST remains the source view |
| Component type scheme | Not represented yet | No | — (CM-1d blocker, §7.11.3) |

The core-module identity machinery is now Store-owned: a loaded `Module` retains
no `Subtype`, and the validator, JIT and runtime GC paths use Store views. The
component layer has a parser and a structural validator; revision 4 added
Store-level identity for closed, local-index-free records, and revision 5 moved
the core modules to Store-owned views. The owner column is
the current state, not the target: `Component` still retains component
declarations and `CoreRecType` still retains core-type parser vectors, while the
target is Store-only type definitions with component references. §7.10 defines
the completion plan and §7.11 records what gates each remaining row.

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
  declarations and `CoreRecType` parser vectors, and the component validator
  keeps the resolved graph.
- The intern table stores a serialized `std::string` per group in addition to
  the canonical group itself.

The footprint therefore scales with `N` types in *both* representations and with
the number of nested lists, not just with the number of distinct types.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ◑ TM1 | **Component structural interning is partial.** Closed records with primitive children now share a `TypeId` across components (record/variant/flags/enum/tuple/list/fixed-list/option/result/map/stream/future/func), and `Component::typeId(localIndex)` publishes ids for defined value/function/resource types. Component/instance declarations have **nominal** Store records, not structural interning yet. A record containing a component-local `TypeIndex`, a resource, `own`/`borrow` or a scheme is still non-interned. | `TypeStore::addComponent*`, `componentIntern*`, `Component::localTypeIds`; §7.11.2. | High |
| ◑ TM2 | **Core modules no longer retain a parser copy.** `processTypeSection` discards the temporary `Subtype` buffer after interning. Component core types still keep parser `Subtype` vectors in `CoreRecType`; `Subtype` remains the component parser scratch type. | `WasmModule::processTypeSection`; `WasmComponentModel.hpp` `CoreRecType`; §4.1. | Medium |
| TM3 | **The component parser no longer keeps a parallel type tree**, but the validator still expands resolved types into `ResolvedValueType`/`ResolvedType`. | `WasmComponentValidation.cpp`; §4.2. | Medium |
| ◑ TM4 | **Core `Subtype` still pays for every composite alternative** in the component parser's `CoreRecType`; core modules use the compact Store records. | `WasmType.hpp` `CompositeDefinition`; `WasmComponentModel.hpp`. | Medium |
| ◑ TM5 | **Core parser lists still allocate per node** while a component core type is parsed; core modules discard them after interning. Component type lists are pooled and component labels/names are interned. | `FuncType`/`StructType`; `WasmTypeStore.cpp`. | Medium |
| TM6 | **Scope-local component indices have no explicit global mapping.** A component type index is not a `TypeId`, and no table records the local-to-global relationship after validation. | `TypeScope::types`; `resolveTypeIndex`. | High |
| TM7 | **Open component types are not modelled.** A `sub resource` import is a type parameter, but there is no scheme/substitution representation that distinguishes it from a concrete resource identity. This is CM-1d work. | `ResolvedResourceType::abstract`; no type-parameter type. | High |
| ~~TM8~~ | ~~**No footprint/identity instrumentation.**~~ **Done (revision 2):** `TypeStore` exposes record/payload/allocation counters, and the test suite asserts the compact record sizes and the shared-store contents. | `WasmBase/WasmTypeStore.hpp`, `Test/main.cpp`. | Closed |
| TM9 | **Component type scope ownership is explicit for the parser**, while the validator's resolved graph remains a temporary scaffold. | `TypeStore` + `ComponentTypeDef`; resolved graph in `WasmComponentValidation.cpp`. | Medium |
| ◑ TM10 | **The canonical store is not the only type owner yet.** Core modules are Store-owned: `Module` keeps no `Subtype` block, and `TypeRegistry` keeps no parser blocks. Component core types still keep `Subtype` vectors inside `CoreRecType`, and component declarations remain in the AST. | `CoreRecType::subtypes`; `ComponentTypeDef`; §7.11.2. | High |
| ✅ TM11 | **The core JIT/validator cutover is complete.** `LibJitTypeTranslator` has a canonical `translateTypes(typeIds, store)` overload; `LibjitOpcodeDispatcher`/`ModuleCompiler` use `typeView`/`funcTypeAt`; `ModuleInstance` GC walking and constant expressions use `CoreTypeView`; `WasmValidator` reads canonical views and `typeCount()`. The old translator `span<Subtype>` overload is test-only; `FuncType` signature materialisation remains a Store-derived compile view, not a retained module block. | `LibJit/LibJitTypeTranslation.*`, `WasmBase/WasmValidator.cpp`, `WasmBase/WasmModuleInstance.cpp`. | Closed (core) |
| ◑ TM12 | **Component type resolution is still validator-only.** Closed local-index-free definitions intern through `TypeStore`; validation publishes `Component::localTypeIds` and gives component/instance types nominal Store records. Records with local refs, structural declaration interning, `eq`/`sub resource` mapping and canonical `TypeId` references in value payloads remain. | `TypeScope::types`; `ResolvedType::typeId`; `Component::localTypeIds`; `ComponentDeclRecord`; §7.11.2. | High |
| TM13 | **Type schemes and resource substitution are not modelled.** There is no scheme/template, type-parameter identity, substitution map, or distinction between an open component type and a closed instantiated type. | `ResolvedResourceType::abstract`; no `TypeSchemeId`/`TypeParamId`. | High |
| TM14 | **Canonical ABI metadata is not represented.** There is no flatten/join/layout table keyed by resolved component type, no `canonopt`-qualified ABI signature, and no canonical-ABI runtime type path for resources/handles. | `WasmComponentValidation.cpp` validates `canon` indices only; no ABI lowering. | High |
| ◑ TM15 | **The ownership/dedup/ABI gates are partially test-enforced.** New tests pin the core-module no-`types` gate, repeated-load identity, closed-component cross-component interning, label-order sensitivity, resource freshness, the component `localTypeIds` map and nominal declaration records. Still absent: the component `CoreRecType` ownership gate, structural declaration interning and ABI cache identity. | `Test/main.cpp`; §7.11.5. | Medium |

Revision 4 closed the local-index-free half of TM1/TM12. Revision 5 closes the
core-module half of TM6/TM10 and all of TM11: a loaded `Module` retains no type
payload and the JIT/validator/runtime read Store views. The remaining TM2/TM4/
TM5/TM10 residue is the component parser's `CoreRecType`; TM6 (component
local-index mapping), TM9, TM13 and TM14 remain open. §7.11 states which are
internal work and which depend on another proposal.


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
  signature views. `Subtype` is now parser/component scratch; the remaining
  `CoreRecType` conversion is the first item of the component closure in
  §7.11.2. §7.10.3 is the contract for the cutover, including the cross-module GC
  `subtypeForTypeId` change.

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
  `Module::types`; the parser uses a temporary `std::vector<Subtype>` and keeps
  only `typeIds`/`typeGroups`/a reverse index map. `CoreRecType::subtypes` and
  the component declaration vectors remain transitional and are tracked in
  §7.11.2.

The core state now matches the target; the component `CoreRecType` is the only
remaining retained type syntax. “Same type” is “same `TypeId`” for both domains,
but component local indices still need their `TypeId` map.

Acceptance metrics for TM-6:

- **Done (core):** `TypeRegistry::blocks`/`TypeBlock` and `Module::types` are
  gone; `Module::typeCount()` is `typeIds.size()`, not syntax storage.
- `parsedTypeCount()` remains a diagnostic count of definitions seen, not
  retained storage.
- **Done (core):** a repeated module load leaves the canonical store unchanged;
  two modules with the same signature share a `TypeId`.
- **Done (core):** the only `Subtype` during a core module parse is the temporary
  parser buffer; its lifetime ends at interning.
- **Remaining (component):** a component's `CoreRecType` still stores
  `std::vector<Subtype>`; it should reference Store `TypeId`/`CoreTypeView`.
- `ComponentTypeDef` for component/instance types references store-owned
  declaration records (`TypeSpan`), not `std::vector<ComponentDecl>` /
  `std::vector<InstanceDecl>` owned by the component AST.

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
| **TM-6 — Store-owned canonical partition** | ◑ Core modules done: remove per-module `Subtype` blocks; parse a temporary group, intern it to `TypeId`, retain only local maps/views. Remaining: `CoreRecType::subtypes` and component declaration vectors (coupled to §7.11.2). | TM10 (core) | Core modules retain no type syntax; `TypeRegistry` stores one record per canonical type. Component `CoreRecType` remains. |
| **TM-7 — JIT view cutover** | ✅ Core modules done: `LibJitTypeTranslator`, `LibjitModuleCompiler`, `LibjitOpcodeDispatcher`, `ModuleInstance`, `WasmValidator` use `CoreTypeView`/`TypeId`; `funcTypeAt` is a Store-derived compile/test signature view. | TM11 | No JIT/validator path reads `Subtype` from a module; cross-module GC walking uses the store. |
| **TM-8 — Component closed-type interning** | ◑ First slices: `TypeStore` interns closed records whose nested `ComponentValueType`s are primitive; validation publishes `Component::localTypeIds`; component/instance declarations have nominal Store records. Remaining: records with local refs, canonical `TypeId` references in payloads, structural declaration interning, `CoreRecType`. | TM12 (partial) | Done for local-index-free records, the local map and nominal declaration records; the remaining work is tracked in §7.11.2. |
| **TM-9 — Schemes and resources** | Add `TypeSchemeId`/`TypeParamId`, resource IDs, substitution maps and instantiate-to-closed-type. | TM13 | `eq` aliases share, `sub resource` gives a fresh parameter, and two substitutions produce distinct closed types when resources differ. |
| **TM-10 — Canonical ABI metadata** | Add `CanonicalAbiSignature` derivation keyed by `(TypeId, CanonOptsKey)`; expose flat/layout data without changing type identity. | TM14 | A closed component function has a stable ABI signature; flat scalar/string/resource fixtures resolve it; no ABI cache aliases different options. |
| **TM-11 — Ownership/dedup/ABI gates** | Assert one record per canonical type, no retained `Subtype` in Module/Component, store-owned views, and ABI cache identity. | TM15 | Tests fail if any module/component owns a type payload or if identical closed types get distinct ids. |

The order is deliberate: TM-6 removes the duplicate raw owner; TM-7 proves the
existing consumers can work from store views; TM-8–TM-10 add the component
identity/ABI layers on top of a single owner. TM-11 is the regression gate that
prevents future code from reintroducing per-module type storage.


### 7.11 Blockers and out-of-proposal dependencies

The completion plan splits into internal gating work and decisions another
proposal owns. This section is the authoritative status list; the §5 rows keep
their IDs.

#### 7.11.1 Core consumer cutover (TM-6/TM-7): core modules closed, `CoreRecType` remains

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

Remaining: component `CoreRecType` still owns `std::vector<Subtype>`, and
`ComponentValidator::validateCoreTypeDef` still builds `std::vector<const
Subtype*>` across component core-type scopes. Converting it needs the same
component local-index → `TypeId` map as §7.11.2, so it is tracked there rather
than duplicated here. The `FuncType` compatibility view is Store-derived and
does not reintroduce a module-owned type block.

#### 7.11.2 Internal gating: component local indices and declarations (TM-8/TM-12)

The parser assigns `TypeId`s to defined value/function/resource definitions, but
a nested `ComponentValueType::Kind::TypeIndex` is still a component-local index.
Interning such a record would make two unrelated components share an identity, so
the store deliberately skips it: `TypeStore::addComponent*` returns a fresh
record when a nested value is `TypeIndex`, and the `$listRecord` test keeps that
distinction. This resolver also owns the remaining TM-6 sub-item: component
`CoreRecType` still stores parser `Subtype`s and `validateCoreTypeDef` still
walks `const Subtype*` slots. Closing this needs:

0. replace `CoreRecType::subtypes` with `TypeId`s/`CoreTypeView` and convert
   `validateCoreTypeDef` to the same local-index map;
1. **Partially done:** `ComponentValidator::run()` publishes
   `Component::localTypeIds` over the component type index space for defined
   value/function/resource types (aliases/imports included); component/instance
   types and schemes still map to `kNone`;
2. a canonical `ComponentValueType` that can carry a `TypeId`, not only a local
   index, plus a rewrite of the store payloads from syntax to canonical form;
3. **Partially done:** component/instance declarations are copied into
   Store-owned `ComponentDeclRecord` spans; structural interning of the
   declaration set still needs the `wasm-tools` equality rule;
4. **Partially done:** component/instance types receive nominal TypeIds;
   nested references still use local indices until payload canonicalisation.

The validator's `TypeScope`/`ResolvedType` graph is the only resolver today; the
parser cannot do this alone because imports/aliases/outer scopes are resolved
there. This is internal work, but it touches
`WasmComponentParser.inc`, `WasmComponentModel.hpp`,
`WasmComponentValidation.cpp` and `WasmTypeStore.*`.

#### 7.11.3 Out-of-proposal: schemes and type imports (TM-9/TM-13)

`sub resource` imports and open component types are Component Model **CM-1d**
work ([`COMPONENT_MODEL.md`](COMPONENT_MODEL.md)). This note has no
`TypeSchemeId`/`TypeParamId`, no substitution map, and no distinction between a
scheme and a closed type; adding one before CM-1d would invent semantics the
component model owns. What exists now is nominal: a resource definition gets a
fresh `TypeId`, an `eq` alias shares it, and `own`/`borrow` are not structurally
interned. Blocker: `COMPONENT_MODEL.md` CM-1d (type imports/exports and
instantiation).

#### 7.11.4 Out-of-proposal: Canonical ABI metadata (TM-10/TM-14)

A `(TypeId, CanonOptsKey)` flatten/join/layout signature requires component
instantiation, `canon lift`/`lower`, resource handle tables and a thunk /
`VMContext` path. Those are `COMPONENT_MODEL.md` CM-4…CM-7; this note
deliberately stops at the metadata contract. Adding an ABI cache now would
either duplicate CM-4's layout rules or freeze an unimplemented calling
convention. Related intersection: [`ABI.md`](../ABI.md)'s host-reference model
(R2/R3/R4) and GC `G3` own the `externref` representation the resource/handle
path will consume. Blocker: CM-4…CM-7 plus `ABI.md` R4.

#### 7.11.5 Internal: gates (TM-11/TM-15)

Implemented in revision 4/5:

- `WASM::Module` has no `types` member (compile-time gate), and a repeated core
  module load yields the same `TypeId`s without growing the canonical store;
- `Module::typeView`/`funcTypeAt` are the only type accessors; the JIT, runtime
  GC walker, constant-expression GC construction and validator use Store views;
- closed component records share across components; label order is significant;
- resources and `own`/`borrow` stay fresh; a record with a local index is not
  interned;
- `Component::localTypeIds` publishes defined value/function/resource identities;
  component/instance types get nominal Store records with compact
  `ComponentDeclRecord` spans (structural interning still pending).

Still gated:

- “no `Subtype`/declaration payload in `Component`” — `CoreRecType` and the
  component declaration variants remain (§7.11.2);
- “one canonical record per closed component type” — needs §7.11.2;
- ABI cache identity — needs CM-4…CM-7 (§7.11.4);
- the ASAN/allocation-budget footprint sweep over the full suite — the counters
  and the new identity paths are tested, but the full sweep remains a CI gate.


## 8. Indicative test survey

The refactor is gated by the existing behaviour suite first and by new identity
and footprint tests second. The counts below are the baseline at the time of
writing; re-measure rather than trusting them after the first stage.

| Script / test | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| default C++/wast suite | 8,573 assertions / 287 cases | 0 | 0 | Existing core, GC, SIMD, exceptions and component parser/validator behaviour is the refactor's regression floor, plus the revision-4/5/6 identity tests. |
| strict validation suite (`WASM_SPEC_VALIDATE=1` and the curated negative sets) | same suite green | 0 | 0 | The compact store must not weaken any validator path, positive or negative. |
| `Test/wasm_wat/component_types.wat` | pass | 0 | 0 | Every component value type, import/export and label shape survives the AST rewrite. |
| `component_coretypes.wat`, `component_coreexterns.wat`, `component_coregc.wat` | pass | 0 | 0 | Core-type and core-extern payloads still map to the same core shapes. |
| `component_{instances,decltypes,resource,async,canonopts,attributes,aliasexport,instanceinline,coreinline,outer}.wat` | pass | 0 | 0 | Component declaration fields, resources, aliases and nested/outer scopes keep their current meaning. |
| invalid component fixtures (reference/type/alias/instantiation/canon/start/label) | pass (rejected) | 0 | 0 | Local-index mapping must reject the same bad inputs before intern. |
| new `sizeof`/allocation assertions | pass | 0 | 0 | Revision 2 pins the compact record sizes and exercises the store's byte/allocation counters; the full target ceiling remains a TM-6 gate. |
| new core identity tests | pass | 0 | 0 | Same `rec` group in two modules -> same `TypeId`; distinct groups -> distinct IDs; `match` unchanged; a loaded Module has no `types` member and retains only canonical ids. |
| new component identity tests | pass (partial) | 0 | 0 | Closed local-index-free records share across components; label order matters; resources/`own`/`borrow` stay fresh; a local-index list is deliberately not interned; `Component::localTypeIds` maps every parsed type, with component/instance declarations carrying nominal Store records and `ComponentDeclRecord` spans. `eq`/`sub resource` schemes remain §7.11.3. |
| new footprint benchmark | partial | — | — | `payloadBytes()`/`allocationCount()` include the component intern keys; the full byte/RSS ceiling and no-per-type-list-allocation measurement remain a TM-6/TM-11 gate. |
| ASAN + allocation-budget run | none yet | — | — | No use-after-free from pooled views; every input-derived count is bounded. Remains a CI gate. |

### 8.1 Identity tests to add

- **Core:** **Done:** a function signature repeated in two modules has one
  `TypeId`; two different `rec` groups have different IDs; a bare type and a
  one-member `rec` share an ID; a cross-module `match` still follows the
  supertype chain; `WASM::Module` has no `types` member and a loaded module
  exposes only `typeIds`/`typeView`.
- **Component:** **Done (partial):** `record { a: u32 }` and `record { a: u32 }`
  from different components share an ID; `record { a: u32 }` and
  `record { a: u64 }` differ; label order in a record is respected;
  optional/result/stream/future payloads with primitive children share an ID;
  `Component::localTypeIds` maps every parsed type; component/instance
  declarations get nominal Store records and `ComponentDeclRecord` spans.
  **Remaining:** records with local-index children, canonical `TypeId` payload
  references and structural declaration interning; see §7.11.2.
- **Resources:** **Done for nominal freshness:** two `resource` declarations with
  the same representation and destructor have different IDs; `own`/`borrow` are
  not structurally interned. **Remaining:** `eq`/`sub resource` aliases and
  abstract parameters need CM-1d (§7.11.3).
- **Scopes:** a nested component's local type index does not resolve in the outer
  component; an outer alias resolves to the outer `TypeId`; after validation no
  stored type refers to a component-local index. **Remaining (TM-8/TM-12).**
- **Schemes:** substituting a type import in a scheme produces the expected
  closed type; substituting two different resource IDs produces two different
  closed types; the scheme itself does not masquerade as a closed `TypeId`.
  **Blocked by CM-1d (§7.11.3).**
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
  or component type payload. **Core done:** `Module` has no `types` member and
  `TypeRegistry` keeps no parser blocks. **Component remaining:** `CoreRecType`
  and structural declaration interning are §7.11.2; component/instance
  declarations already have nominal Store records.
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
  share an id; no closed record contains a local index; component/instance
  declaration order is tested against `wasm-tools`. **Partial:** local-index-free
  records, defined-type local ids and nominal component/instance declaration
  records are done; structural declaration interning, canonical payload
  references and `CoreRecType` remain in §7.11.2.
- **Schemes/resources:** `eq` aliases share, `sub resource` creates a parameter,
  and two instantiated substitutions with different resource IDs produce
  different closed types. **Gated by CM-1d (§7.11.3).**
- **ABI metadata:** the same `TypeId` under different `canonopt` sets produces
  distinct ABI signatures; the same `(TypeId, opts)` reuses one signature;
  resource-handle traps match `SAFETY.md`. **Gated by CM-4…CM-7 (§7.11.4).**


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

