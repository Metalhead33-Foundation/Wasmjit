# Unified Type Store and Component Type Identity — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / refactor plan (revision 1 — first cut of the type-store and
type-identity plan).
Audience: anyone touching `WasmBase/WasmType.hpp`, `WasmBase/WasmCanonicalType.hpp`,
`WasmBase/WasmTypeRegistry.{hpp,cpp}`, `WasmBase/WasmComponentModel.hpp`,
`WasmBase/WasmComponentParser.inc`, `WasmBase/WasmComponentValidation.cpp`, or the
type-translation layer that consumes canonical identities.

This note follows the nine-slot split used by [`GC.md`](GC.md),
[`SIMD.md`](SIMD.md) and [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md). It is an
**internal type-representation design note**, not a WebAssembly proposal and not
a record of a shipped feature. It contains **no new code**: §4 is the current
representation, §5 is the gap between that representation and the target, and §7
is the migration plan.

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
`std::vector<FieldType>`; `ArrayType` owns a `FieldType`. `Subtype` is the type
that `Module` exposes and that the JIT type translation consumes. It is
**88 bytes** on the current x86-64/libstdc++ toolchain, plus heap allocations for
each non-empty list (`docs/TYPE_IDENTITY.md` §12.1).

`WasmBase/WasmCanonicalType.hpp` defines a second representation:
`CanonicalType { kind, isFinal, supertypes, depth, funcParams, funcResults,
structFields, arrayElement }`. Every canonical type has four vectors, regardless
of kind, and `CanonFieldType`/`CanonStorageType`/`CanonValueType` are embedded by
value. The layout-derived size on the same toolchain is about **144 bytes**,
before payload heap allocations.

`WasmBase/WasmTypeRegistry.{hpp,cpp}` (`TypeRegistry`) does the following:

- `registerModule(std::vector<Subtype>)` moves each module's parser output into
  a stable heap block and returns a `std::span<const Subtype>`.
- `internModule(...)` builds a `std::vector<CanonicalType>` for each `rec` group,
  serializes the group to a `std::string` for the intern table, hash-conses it,
  and stores the canonical copy in `canonicalTypes`.
- `TypeId` is allocated from `nextTypeId`; `RecGroupId` indexes `recGroups`.
- The registry retains both the `Subtype` block and the canonical records, plus
  the intern table and its serialized keys. `TypeRegistry::typeCount()` and
  `parsedTypeCount()` make the duplication visible.

The current flow is therefore:

```text
binary type section -> std::vector<Subtype> (module/registry block)
                    -> CanonicalType group (canonical table)
                    -> TypeId
```

### 4.2 Components: an AST plus a validator-only resolved graph

`WasmBase/WasmComponentModel.hpp` defines the component AST:

- `ComponentValueType` is a 12-byte `{kind, typeIndex, primitive}` pair.
- `DefinedValueType` stores every alternative inline: `fields`, `cases`,
  `element`, `fixedLength`, `tuple`, `labels`, `ok`, `error`, `typeIndex`,
  `streamElement`, `futureElement`, `mapKey`, `mapValue`. Its layout-derived
  size is about **216 bytes**.
- `ComponentFuncType`, `ComponentTypeDef`, `ComponentDecl`, `InstanceDecl`,
  `ImportDecl`, `ExportDecl`, `CoreTypeDef`, `Alias`, `NameAttributes` and
  `Attribute` are similarly expanded. `ComponentTypeDef` is about **344 bytes**
  because it contains all five alternatives; `ComponentDecl` is roughly **360
  bytes** and `InstanceDecl` about **256 bytes** because every declaration
  payload is inline.
- Labels, names and attribute text are `std::string`; a `std::string` is 32 bytes
  on this toolchain before its heap payload.

`WasmBase/WasmComponentValidation.cpp` then builds a second graph with
`std::shared_ptr` nodes:

- `ResolvedValueType` mirrors the full `DefinedValueType` alternative set and
  owns `std::vector<std::pair<std::string, ValuePtr>>`, `std::vector<ResolvedCase>`,
  `std::vector<std::string>` and optional `shared_ptr`s. Its layout-derived size
  is about **240 bytes**.
- `ResolvedType` adds a kind plus five `shared_ptr` members; `ResolvedDecl`
  carries a full `NameAttributes`, a `SortIdx`, a `TypePtr` and core-module
  bookkeeping.
- `TypeScope` holds `std::vector<TypePtr> types` and a parallel core-type list.
  Resolution is by component-local index. There is no component `TypeId` and no
  interning in `TypeRegistry`.

The current flow is therefore:

```text
binary component -> ComponentTypeDef / DefinedValueType AST
                 -> ResolvedType / ResolvedValueType shared_ptr graph
                 -> component-local type index
```

The second graph exists only for the validator's lifetime, but it expands every
nested type and every string a second time. The AST remains allocated alongside
it.


### 4.3 Identity and ownership today

| Type | How it is identified today | Interned? | Owner |
|---|---|---|---|
| Core defined type | `LocalTypeIdx` in a module; `TypeId` after `internModule` | Yes, by `rec` group | `Module` span + `TypeRegistry` block/table |
| Core canonical type | `TypeId` | Yes | `TypeRegistry` |
| Component value type | Component-local `typeIndex` | No | `ComponentTypeDef` AST + `ResolvedValueType` graph |
| Component resource | Validator-local `resourceId` / `ResolvedResourceType` | No | Validator graph |
| Component instance/component type | Component-local index | No | AST + validator graph |
| Component type scheme | Not represented yet | No | — |

The core identity machinery is real and tested; the component layer has a parser
and a structural validator but no store-level identity. This is the central
asymmetry the refactor addresses.

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
- The registry keeps the parser block and the canonical table; the component
  validator keeps the AST and the resolved graph.
- The intern table stores a serialized `std::string` per group in addition to
  the canonical group itself.

The footprint therefore scales with `N` types in *both* representations and with
the number of nested lists, not just with the number of distinct types.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TM1 | **Component types have no process-wide identity.** They are addressed by component-local indices and have no `TypeId`, no interning and no cross-component equality path. | `TypeRegistry` exposes only core `TypeId`s; `ResolvedType` is validator-local. | High |
| TM2 | **Core types have two long-lived representations.** `Subtype` and `CanonicalType` both expand every function/struct/array, and the registry retains both. | `WasmTypeRegistry::registerModule` + `internModule`; §4.1. | High |
| TM3 | **Component types are expanded twice.** The parser's `DefinedValueType`/`ComponentTypeDef` and the validator's `ResolvedValueType`/`ResolvedType` both retain the full structure. | `WasmComponentValidation.cpp` §46–128; §4.2. | High |
| TM4 | **Every type pays for every alternative.** `Subtype` embeds `FuncType`'s vectors in structs/arrays; `DefinedValueType` and `ComponentTypeDef` embed every payload. | `WasmType.hpp` `CompositeDefinition`; `WasmComponentModel.hpp`. | High |
| TM5 | **Per-node vectors and strings dominate the heap.** Each populated list and every label/name is a separate owning allocation; the registry also keeps a serialized intern key per group. | `FuncType`, `StructType`, `DefinedValueType`, `ResolvedValueType`, `std::unordered_map<std::string, RecGroupId>`. | High |
| TM6 | **Scope-local component indices have no explicit global mapping.** A component type index is not a `TypeId`, and no table records the local-to-global relationship after validation. | `TypeScope::types`; `resolveTypeIndex`. | High |
| TM7 | **Open component types are not modelled.** A `sub resource` import is a type parameter, but there is no scheme/substitution representation that distinguishes it from a concrete resource identity. This blocks a sound shared identity for CM-1d instantiation. | `ResolvedResourceType::abstract`; no type-parameter type. | High |
| TM8 | **No footprint/identity instrumentation.** There are no `sizeof` assertions for the component records and no store-level byte/allocation counters, so the claimed footprint reduction cannot be tested. | Current tests cover parser/validator behaviour, not storage. | Medium |
| TM9 | **No explicit ownership boundary for component type storage.** The parser AST and validator graph live in different places; nothing states which object owns the long-lived type structure. | Parser AST on `Component`; resolved graph local to `validateComponent`. | Medium |


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
| API churn | `TypeRegistry`/`Subtype` consumers break during the transition. | Keep core APIs and `Subtype` parser input; add aliases/views; remove only after all consumers are migrated. |

## 8. Indicative test survey

The refactor is gated by the existing behaviour suite first and by new identity
and footprint tests second. The counts below are the baseline at the time of
writing; re-measure rather than trusting them after the first stage.

| Script / test | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| default C++/wast suite | 8,354 assertions / 273 cases | 0 | 0 | Existing core, GC, SIMD, exceptions and component parser/validator behaviour is the refactor's regression floor. |
| strict validation suite (`WASM_SPEC_VALIDATE=1` and the curated negative sets) | same suite green | 0 | 0 | The compact store must not weaken any validator path, positive or negative. |
| `Test/wasm_wat/component_types.wat` | pass | 0 | 0 | Every component value type, import/export and label shape survives the AST rewrite. |
| `component_coretypes.wat`, `component_coreexterns.wat`, `component_coregc.wat` | pass | 0 | 0 | Core-type and core-extern payloads still map to the same core shapes. |
| `component_{instances,decltypes,resource,async,canonopts,attributes,aliasexport,instanceinline,coreinline,outer}.wat` | pass | 0 | 0 | Component declaration fields, resources, aliases and nested/outer scopes keep their current meaning. |
| invalid component fixtures (reference/type/alias/instantiation/canon/start/label) | pass (rejected) | 0 | 0 | Local-index mapping must reject the same bad inputs before intern. |
| new `sizeof`/allocation assertions | none yet | — | — | TM-0 gate: pin the current numbers and the target ceiling. |
| new core identity tests | none yet | — | — | TM-1 gate: same `rec` group in two modules -> same `TypeId`; distinct groups -> distinct IDs; `match` unchanged. |
| new component identity tests | none yet | — | — | TM-3 gate: structural equality, cross-component interning, resource freshness, `eq` aliasing and `sub resource`. |
| new footprint benchmark | none yet | — | — | TM-0/TM-4 gate: type bytes per fixture and peak RSS; no per-type list allocations. |
| ASAN + allocation-budget run | none yet | — | — | No use-after-free from pooled views; every input-derived count is bounded. |

### 8.1 Identity tests to add

- **Core:** a function signature repeated in two modules has one `TypeId`; two
  different `rec` groups have different IDs; a bare type and a one-member `rec`
  share an ID; a cross-module `match` still follows the supertype chain.
- **Component:** `record { a: u32 }` and `record { a: u32 }` from different
  components share an ID; `record { a: u32 }` and `record { a: u64 }` differ;
  label order in a record/variant is respected; optional/result/stream/future
  payloads with the same shape share an ID.
- **Resources:** two `resource` declarations with the same representation and
  destructor have different IDs; an `eq` alias shares its target ID; a
  `sub resource` has a fresh abstract ID; a resource never equals a structurally
  identical non-resource.
- **Scopes:** a nested component's local type index does not resolve in the outer
  component; an outer alias resolves to the outer `TypeId`; after validation no
  stored type refers to a component-local index.
- **Schemes:** substituting a type import in a scheme produces the expected
  closed type; substituting two different resource IDs produces two different
  closed types; the scheme itself does not masquerade as a closed `TypeId`.
- **Domains:** a core `TypeId` used where a component type is expected is
  rejected; a component `TypeId` never satisfies a core `match`.

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
- Measure a small, medium and type-heavy fixture before/after with the existing
  tests plus a dedicated benchmark. Record both bytes and allocations; a lower
  byte count with more allocations is not automatically a win.
- Run the negative fixtures under the allocation budget; a malformed count must
  fail before a large reserve, with the same "reject" outcome as today.

### 8.3 What is deliberately not tested here

No Canonical ABI, component instantiation, handle table or WIT ingestion is
tested by this refactor. Those remain CM-4…CM-7 in
[`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §7.


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
  - `WasmBase/WasmTypeRegistry.{hpp,cpp}` — `registerModule`, `internModule`,
    `canonicalType`, intern table.
  - `WasmBase/WasmComponentModel.hpp` — `ComponentValueType`,
    `DefinedValueType`, `ComponentTypeDef`, `ComponentDecl`, `InstanceDecl`,
    `ResourceType`, `ExternType`.
  - `WasmBase/WasmComponentValidation.cpp` — `ResolvedValueType`,
    `ResolvedType`, `ResolvedDecl`, `TypeScope`.
  - `WasmBase/WasmComponentParser.inc` — component decoder and AST construction.

