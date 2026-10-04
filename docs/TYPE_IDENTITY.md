# Type Identity, Equivalence, and Matching

Status: design note / implementation reference (revision 2).
Audience: anyone touching `WasmType`, `WasmTypeRegistry`, `ModuleInstance`,
`RegistryImportResolver`, or `LibJitTypeTranslator`.

This note separates three questions that are easy to conflate:

1. **What the WebAssembly specification requires** — the *relations*
   (`match` / subtyping, type equivalence) and where they must hold.
2. **What the specification does not require** — representation, canonical
   forms, global type tables, algorithms.
3. **What is nevertheless useful to implement here** — structural
   canonicalization/interning of `rec` groups, a global type identity, and
   cross-module matching.

The spec answers (1) declaratively; (2) follows from the spec being
representation-agnostic; (3) is our design choice.

Revision 2 turns the design (§6) into an ordered implementation plan (§8) and
test plan (§9), and records decisions that were previously open (§7).

## 1. Scope

This is about **type identity**, not value layout. Layout already lives in
`Subtype` / `FuncType` / `StructType` / `ArrayType` (`WasmBase/WasmType.hpp`)
and is consumed by the JIT (`LibJitTypeTranslator`).

Two relations matter:

- **Equivalence** — "are these two types the same type?"
- **Subtyping / matching** — "is type A usable where type B is expected?"

## 2. The spec's type model

### 2.1 Type indices are module-local

A module's type section defines a sequence of types. Every reference to a type
inside the module (in value types, supertypes, and instructions) is a
`typeidx` into *that module's* type index space. Validation builds a context
`C` from the module's own definitions; a module is *closed*, i.e. its components
can only refer to definitions that appear in the module itself.

### 2.2 Recursive types and `rec` groups

The type section is a sequence of **recursive type groups** (`rec`), each a
mutually recursive block of one or more subtypes. Scope-wise, validating a
group extends the context with the group's own members, so members may refer to
each other; references to *later* groups are not in scope. A self/mutual
reference can be read as a **recursive type index** (`rec.i`, de Bruijn-style):
the *i*-th component of the surrounding `rec`.

### 2.3 Expansion, rolling/unrolling, closed types

The spec introduces extended (non-encodable) type forms that occur only during
validation/execution:

- A **type use** may be a `deftype` directly — the result of *substituting* a
  `typeidx` with its definition.
- A type use may be a **recursive type index** — the result of *rolling up* a
  `rec` group into its own members.
- A type is **closed** when it contains no `typeidx` and no free recursive type
  index: all indices substituted, all free recursive indices unrolled.

These operations are how the spec makes two types comparable. They are
normative concepts, but the spec does **not** call them "canonicalization" and
does not prescribe an algorithm.

## 3. What the specification demands

### 3.1 `match` (subtyping)

The core spec defines a subtyping relation — **matching**, written
`C ⊢ t ≤ t'` — and states it is "applicable in validation rules, during module
instantiation when checking the types of imports, or during execution, when
performing casts." Rules exist for number/vector types (only themselves), heap
types (abstract hierarchy, reflexivity, transitivity), reference types
(nullability/covariance), value/result/instruction types, composite types
(`func` is contravariant in parameters and covariant in results; `struct`
supports width + field subtyping; `array` is covariant in the element), field
and storage types (mutable fields invariant, immutable fields covariant),
defined types, limits, global/memory/table/tag types, and external types.

### 3.2 Type equivalence

Defined-type subtyping has exactly two rules:

- **reflexivity**: a defined type matches itself;
- **supertype**: if a defined type has a supertype that matches the target, it
  matches.

The spec note is decisive:

> "there is no explicit definition of type *equivalence*, since it coincides
> with syntactic equality."

"Syntactic equality" is on **closed** types: once type indices are expanded and
recursive references are rolled up, structural identity is literal structural
identity. Recursive types are compared as closed structures.

The GC proposal's design document (`proposals/gc/MVP.md`,
"Appendix: Formal Rules for Types") makes the machinery concrete:

- **Equivalence**: `C ⊢ tie(x) == tie(x')` implies `C ⊢ x == x'`; and
  `C ⊢ rec.i == rec.i`. Composite types are equivalent iff their components
  are, field-by-field, with identical finality and supertypes.
- **Subtyping**: if `unroll(C(x)) = sub final? (x1* x'' x2*) st` and
  `C ⊢ x'' <: x'`, then `C ⊢ x <: x'` — subtyping of a defined type goes through
  an unrolling of its declared supertype relation.
- **Variance**: `func t1* t2* <: func t1'* t2'*` requires `t1'* <: t1*` and
  `t2* <: t2'*`; structs admit width subtyping; mutable fields are invariant.

The final spec distills these into the `Deftype_sub/refl` and
`Deftype_sub/super` rules plus the heap/reference/composite rules.

### 3.3 Where `match` must hold

- **Validation**: declared subtype/supertype annotations must be well-formed
  (e.g. a `struct`'s fields must structurally match its supertype's fields).
- **Instantiation / linking**: each import's declared type must `match` the type
  of the provided extern value. **This is the cross-module case.**
- **Casts**: `ref.test`, `ref.cast`, `br_on_cast(_fail)` classify heap types via
  the subtyping relation.
- **Indirect calls**: `call_indirect` checks that the callee's function type
  matches the expected type; it is a match check, not a plain equality check.

### 3.4 Consequence for cross-module values

Because equivalence is structural over closed forms, two modules that
independently define structurally identical (iso-recursive) types are talking
about the *same* type. That is what makes it legal for a reference produced by
one module to satisfy a typed reference in another.

## 4. What the specification does NOT demand

- **No "canonicalization" pass.** The core spec never uses the term for types
  (the only `canonical` is NaN canonicalization). The relation is declared
  declaratively: "the rules ... do not define an algorithm."
- **No global type table.** Types belong to the module that defines them; the
  spec never requires a process-wide registry or global type ids.
- **No interning or merging.** Nothing requires structurally equal types to
  share identity or representation; comparing them on demand is equally valid.
- **No prescribed representation of `typeidx`.** Module-local numeric indices
  are syntax; any internal encoding is allowed.
- **No behaviour specified for ill-formed modules** beyond the fact that only
  valid modules can be instantiated.

## 5. What is useful to implement anyway

Evaluating the declarative relation naively (structural comparison on every
import check, cast, and type test) is awkward and slow. A standard
implementation technique — used by production engines — is to **canonicalize
and intern `rec` groups**:

- **Global identity.** Compute a canonical, recursion-free key for each `rec`
  group (in-group references as member indices, out-of-group references as
  already-interned global ids), hash-cons it, and assign a stable global
  identity. Two structurally equal groups — from any module — then share one
  identity.
- **O(1) equivalence.** Equivalence collapses to an integer comparison.
- **Cross-module `match`.** Subtyping becomes a walk up a canonical supertype
  chain, so import checking and casts work naturally across modules.
- **Memory dedup.** Repeated function signatures (the bulk of types in real
  modules) collapse to one canonical entry.
- **Stable ids for the JIT.** `LibJitTypeTranslator` can key its caches by the
  global id instead of module-local indices, enabling cross-module reuse of
  lowered `jit_type_t`s (see §6.7).

None of this is mandated; it is an implementation strategy that *realizes* the
spec's relations efficiently and safely.

## 6. Design for this engine

### 6.1 Identifiers

| Name | Meaning |
|------|---------|
| `LocalTypeIdx` | The existing module-local `uint32_t` used by `Subtype`, `ValueType::heapType`, and `Callable::typeIndex` today. Means only "type *n* in *this* module". |
| `TypeId` | Dense `uint32_t`, assigned by `TypeRegistry`. Identifies one canonical defined type, process-wide. |
| `RecGroupId` | Identity of an interned group: a contiguous run of `TypeId`s. |

`LocalTypeIdx`, `TypeId` and `RecGroupId` are **distinct struct types, not
aliases of `uint32_t`**, so the compiler rejects accidental mixing. A module-local
index must never be used to identify a type across modules.

`Module` keeps its `types` span (layout, consumed by the JIT) and gains a
parallel `typeIds` vector mapping `LocalTypeIdx -> TypeId`.

**Single boundary.** There is exactly one place where local indices become
global identities: `TypeRegistry` interning, surfaced to the rest of the engine
as `TypeId Module::typeId(LocalTypeIdx) const`. Registry, runtime, linking, GC
and JIT code reason in `TypeId`s and do not convert back and forth.

### 6.2 Canonical form

A canonical type is a recursion-free description. Its references are abstract
heap types, global ids, or (during construction only) positions within the group
being built:

    CanonHeapType = Abstract(AbstractHeapType)
                  | Global(TypeId)
                  | Rec(memberIndex)       // construction only
    CanonValueType = Num/Vec(opcode)
                   | Ref(nullable, CanonHeapType)
    CanonStorage  = Packed(i8|i16) | Value(CanonValueType)
    CanonField    = { CanonStorage, isMutable }
    CanonicalType = { kind: Func|Struct|Array, isFinal,
                      supertype: optional CanonHeapType (Global after interning),
                      depth,              // length of the supertype chain
                      ... kind-specific payload ... }

Notes:

- `Rec(memberIndex)` is the **absolute index of the member within its `rec`
  group** — exactly the spec's `rec.i`. Groups do not nest, so no de Bruijn
  depth or relative offset is needed.
- `CanonValueType` carries numeric/vector kinds and nullability. A reference type
  is a heap type plus nullability; nullability is not part of the heap type.
- **At most one supertype.** Validation allows at most one declared supertype,
  so a canonical type stores one optional `supertype` and a derived `depth`.
  The parser rejects declarations with more than one supertype.
- A canonical type's identity includes `isFinal` and its supertype.
- After interning, every `Rec(i)` is rewritten to `Global(TypeId)`, so the stored
  graph is purely id-based. Canonical types never contain a `LocalTypeIdx`.

### 6.3 Streaming canonicalization and interning

The **group** is the unit of interning, never an individual type. Process the
type section group by group, in order:

1. Translate each member's component references: in-group index -> `Rec{member
   index}`; earlier-group index -> `Global{localToGlobal[index]}`; later-group
   index -> invalid (reject).
2. Hash the whole group (order-preserving over members) and look it up.
3. On a hash hit, **verify full structural equality** against the stored group
   before reusing it; a hash match alone is never identity. On a verified hit,
   reuse the group's ids.
4. On a miss, allocate fresh contiguous ids, rewrite `Rec` to `Global`, compute
   each member's `depth` from its supertype (which is always an earlier member or
   an earlier group), and store the canonical group.
5. Record `localToGlobal[groupStart + j]`.

Because earlier groups are canonicalized first, out-of-group references are
already global ids. Group identity is well-founded and the whole type section
needs a single pass.

A bare type definition is a `rec` group of size one, and must intern to the same
identity as an explicit single-member `rec`.

### 6.4 `match` across modules

Abstract heap types are not `TypeId`s, so the entry point works on heap types:

    matchesHeap(actual: CanonHeapType, expected: CanonHeapType):
      if actual == expected:               true
      if expected is Abstract:             classify actual against the hierarchy
      if actual is Abstract:               false   // abstract never matches a concrete type
                                                   // (except bottom types, handled in classify)
      else:                                walk actual's supertype chain for expected

    matches(actual: TypeId, expected: TypeId)   // concrete-only wrapper
      == matchesHeap(Global(actual), Global(expected))

Because supertype chains are acyclic by construction (a supertype is always an
earlier definition), the walk needs no memoization or cycle handling. It is
bounded by `depth`; if profiling ever shows it matters, a per-type display array
makes it O(1).

Abstract classification reuses the existing hierarchy logic in
`ModuleInstance::refMatchesHeapType`: `any`, `eq`, `i31`, `struct`, `array`,
`func`, `extern`, plus the bottom types, and the `ref` / `ref null` split.

Reference-type matching combines `matchesHeap` with nullability (a non-nullable
reference matches a nullable one with a matching heap type, not the reverse).
Mutable global types, and other invariant positions, require equality of the
canonical value type rather than a match.

### 6.5 Validation of declared subtypes (safety gate)

The structural composite comparator (func contravariance, struct width, mutable
field invariance, array covariance) is required for **validation**: checking that
each `sub` annotation is well-formed against its declared supertype.

This is a safety requirement, not an optimization. Once casts and imports rely on
id equality plus the supertype chain, an unvalidated `sub` annotation lets a
module *claim* a subtype relation it does not satisfy, which can lead to type
confusion in compiled code. **Cross-module matching and runtime cast migration
must not be enabled until declared-subtype validation is in place.**

### 6.6 Threading and lifetime

- **Single-threaded instantiation.** Instantiation and all initialization work,
  including type interning, are strictly single-threaded by design. The registry
  therefore needs no locking. Debug builds assert that interning runs on the
  designated instantiation thread, so a violation shows up in tests.
- **Stable storage.** Canonical entries live in stable storage (a deque or arena)
  so references remain valid as the registry grows, unless it is guaranteed that
  no wasm code runs during interning, in which case a `vector` is acceptable and
  should say so in a comment.
- **Process lifetime.** The registry never unregisters. `TypeId`s are never
  reused and entries are never freed, which keeps `TypeId`s stored in GC object
  headers valid forever. Memory is cumulative; see §11 for the effect of
  interning on it, and the counters in §8 (M8) for measuring it.

### 6.7 JIT type caching on stable identities

`LibJitTypeTranslator` currently keys on `ValueType{opcode, heapType}` where
`heapType` is a **module-local** index, and indexes `translatedTypes` by that
local index. That is precisely why it needs a per-module `reset()` for
correctness: the keys are not globally meaningful.

With a global `TypeId`, the cache can be keyed by identity:

- concrete reference types -> `TypeId`;
- abstract heap types -> `(opcode, AbstractHeapType)`;
- `translatedTypes` -> a map `TypeId -> jit_type_t` (plus the per-module
  `LocalTypeIdx -> TypeId` lookup from `Module::typeIds`).

`jit_type_t` is a per-`jit_context` structural entity, so identical canonical
types may share one within a context. If more than one `jit_context` can exist,
the cache must be per context. The per-module reset can then be dropped, or kept
only as a defensive measure.

### 6.8 Ill-formed input policy

Canonicalization assumes well-formed input, and the policy for anything else is
**reject**. A forward reference to a later group, an out-of-range index, a
declaration with more than one supertype, or an invalid `sub` annotation causes
the module to fail registration, and it is never instantiated. No tolerance mode
is planned.

## 7. Decisions record

| Question | Decision |
|----------|----------|
| Recursive reference encoding | Absolute member index (`rec.i`), not relative offset or depth. |
| Number of supertypes | At most one; stored as `supertype` + `depth`; extras rejected at parse. |
| Unit of interning | The `rec` group; a hash hit is confirmed by full structural equality. |
| Abstract vs. concrete in `match` | `matchesHeap` over `CanonHeapType`; `matches(TypeId, TypeId)` is a wrapper. |
| Value types in canonical form | `CanonValueType` with numeric/vector kinds and nullability. |
| Local vs. global index types | Distinct struct types, not aliases. |
| Ill-formed modules | Reject. |
| Threading | Instantiation and initialization are single-threaded; no registry locking. |
| Registry lifetime | Process lifetime; ids never reused. |
| Subtype validation | Required before cross-module matching is enabled (§6.5). |

## 8. Implementation plan

Order: M0 -> M1 -> M2 -> M3, then M4 -> M5 -> M6, with M7 in parallel after M3.
Each milestone leaves the engine working.

**M0 — Invariants and strong types.**
- Introduce `LocalTypeIdx`, `TypeId`, `RecGroupId` as distinct types and
  `Module::typeId(LocalTypeIdx)`.
- Add counters for types parsed versus types interned (used in M8).
- *Done when:* the engine builds with the strong types in place and no behaviour
  change.

**M1 — Preserve `rec` groups.**
- In `Module::processSubtypes`, record `groups: vector<{first, count}>` alongside
  the flat `types` span. A bare type is a group of one.
- Reject forward references to later groups and declarations with more than one
  supertype.
- *Done when:* group boundaries round-trip for `type-canon.wast`-style modules
  and all existing tests pass.

**M2 — Scaffold `typeIds`.**
- Populate `Module::typeIds` with unique per-module ids, with no interning yet.
- Use the compiler errors from M0's strong types to enumerate every place that
  assumes a local index is an identity: `ModuleInstance::refMatchesHeapType`, GC
  object headers, `Module::isSubtype`, `LibJitTypeTranslator`,
  `Callable::typeIndex`.
- *Done when:* behaviour is unchanged. These ids must not be used across modules
  yet; doing so would wrongly reject valid links.

**M3 — Canonical form and interning.**
- Implement §6.2 and §6.3 in `WasmTypeRegistry`: canonical structures,
  group canonicalization, hash-cons with verified equality, stable storage.
- Switch `Module::typeIds` to the interned ids.
- *Done when:* the same group in two modules gets the same ids, structurally
  different groups get different ids, and a forced-collision hasher still keeps
  distinct groups apart.

**M4 — Declared-subtype validation (gate).**
- Implement the structural comparator (§6.5) and run it on every `sub`
  annotation at parse time.
- *Done when:* well-formed `sub` declarations are accepted and invalid ones are
  rejected. **M5 and M6 depend on this.**

**M5 — Matching at link time.**
- Implement `matchesHeap` and `matches` (§6.4).
- Route `RegistryImportResolver` import checks (function, global, table, tag)
  and `call_indirect` through them, with an id-equality fast path.
- *Done when:* `linking.wast` and `imports*.wast` pass, including cross-module
  cases.

**M6 — Runtime type tags and casts.**
- Change GC object headers, `ModuleInstance::refMatchesHeapType` and the cast
  instructions (`ref.test`, `ref.cast`, `br_on_cast(_fail)`) to `TypeId` together,
  since a cast needs the header.
- Rename `Callable::typeIndex` so its meaning is explicit: `localTypeIdx` if it
  stays module-local, `typeId` if it becomes runtime identity.
- *Done when:* `ref_cast`, `ref_test` and `br_on_cast*` pass, including with
  references crossing module boundaries.

**M7 — JIT cache on `TypeId`** (parallel with M4–M6, after M3).
- Implement §6.7.
- *Done when:* two modules sharing a signature reuse one `jit_type_t`, in either
  load order, and the per-module reset is no longer needed for correctness.

**M8 — Measure, decide on layout, update docs.**
- Report the dedup ratio (types parsed versus interned) and registry memory on
  representative workloads.
- Decide whether the `Subtype` shrink in §11 is worth doing.
- Update §7 of this note's predecessor ("Current state and gaps", now §10) to
  match the new reality.

## 9. Test plan

Beyond the existing spec tests (`type-canon.wast`, `ref_cast.wast`,
`ref_test.wast`, `br_on_cast*.wast`, `linking.wast`, `imports*.wast`), add a
dedicated suite:

| Test | What it pins |
|------|--------------|
| `rec` group round-trip | Parser emits correct group boundaries. |
| Identical group in two modules | Same `TypeId`s. |
| Different signature (`(i32)->i32` vs `(i64)->i32`) | Different `TypeId`s. |
| Singleton `rec` vs plain type definition | Same identity. |
| Self-recursive type in two modules | Same `TypeId`. |
| Mutual recursion in two modules | Same canonical group. |
| `A→B, B→A` vs `A→A, B→B` | Must not collide. |
| Same shape, different finality or supertype | Different `TypeId`s. |
| Member order inside a group | Order matters. |
| Re-registering a module | Idempotent. |
| Forced hash collision | Distinct groups stay distinct. |
| Earlier-group reference | Resolves through the earlier group's `TypeId`. |
| Forward-group reference | Rejected, never resolved through an uninitialized id. |
| More than one declared supertype | Rejected at parse. |
| Invalid `sub` annotation | Rejected by validation. |
| `matches(Derived, Base)` / `matches(Base, Derived)` | True / false. |
| Cross-module `Derived` vs independently canonicalized `Base` | Matches. |
| Function variance, struct width, mutable-field invariance, array covariance | Comparator rules. |
| Abstract hierarchy | `i31 <: eq <: any`, `struct <: eq`, bottom types. |
| Nullability | Non-nullable matches nullable, not the reverse. |
| Mutable global import with different canonical type | Rejected (invariant). |
| `call_indirect` across modules | Matching callee accepted, non-matching traps. |
| Runtime tag migration | GC header carries `TypeId`; casts work cross-module. |
| JIT identity | Same `TypeId` yields the same cached `jit_type_t`, regardless of load order. |

## 10. Current state and gaps in this codebase

- `TypeRegistry` (`WasmBase/WasmTypeRegistry.hpp`) owns type blocks but has **no
  identity or interning**: `registerModule` hands out a
  `std::span<const Subtype>` per module.
- `Module::types` is that span; all type indices remain module-local.
- `Module::isSubtype` walks declared `supertypeIndices` (module-local) and does
  not implement structural composite subtyping or cross-module matching.
- `ModuleInstance::refMatchesHeapType` and GC object headers use module-local
  indices; an object's runtime type tag is a local index, not a global id.
- `Callable::typeIndex` is a module-local index, which makes its meaning
  ambiguous once global ids exist (see M6).
- The parser **flattens `rec` groups** into one `Subtype` vector
  (`Module::processSubtypes`), so group boundaries are currently lost — the
  information canonicalization needs first.
- `LibJitTypeTranslator`'s cache is module-local; a per-module `reset()` is
  required for correctness (`LibJit/LibJitTypeTranslation.cpp`). There is no
  stable type identity to cache against today.

## 11. Non-goals / deferred

- The GC **heap** itself (allocation, collection) — a separate concern.
- Host/extern type identity (the JS API side) — not modelled here.
- Type **reflection / introspection** APIs.
- `VMContext` and multi-memory layout — a separate workstream.
- Replacing the `Subtype` layout (footprint and options below). The canonical
  form is a second, index-oriented representation; dedup is expected to offset
  its cost.

## 12. Type storage footprint (`Subtype`) and future optimization

The registry stores each module's types as a contiguous `std::vector<Subtype>`
block (`TypeRegistry::registerModule`), so the per-type cost is dominated by
`sizeof(Subtype)`.

### 12.1 Measured size

Measured on the current toolchain (x86-64, libstdc++) with `sizeof`:

| type | bytes |
|------|------:|
| `Subtype` | **88** |
| `std::vector<uint32_t> supertypeIndices` | 24 |
| `CompositeDefinition` (`std::variant`) | 56 |
| `FuncType` | 48 (two `std::vector`s) |
| `StructType` | 24 |
| `ArrayType` | 16 |
| `FieldType` | 16 |
| `StorageType` | 12 |
| `ValueType` | 8 |

The 88 bytes of `Subtype` break down as:

    bool isFinal                            1  (+ 7 padding)
    std::vector<uint32_t> supertypeIndices 24
    CompositeDefinition (variant)          56
                                          ---
                                           88

The variant is `max(FuncType 48, StructType 24, ArrayType 16)` plus a 1-byte
discriminant padded to 8, i.e. 56. Consequently **every** type — including a
struct or an array — reserves the 48-byte `FuncType` payload (two
`std::vector`s), and every type pays 24 bytes for `supertypeIndices` even when it
has none.

### 12.2 Expected footprint for a module

The block contributes `88 x N` bytes for `N` types, plus heap for the nested
vectors (`params`, `results`, `fields`, `supertypeIndices`) whenever they are
non-empty. Those vectors are filled with `push_back` (capacity grows in steps)
and each non-empty allocation carries allocator overhead, so budget up to ~2x
the raw element bytes plus ~16 B per non-empty vector:

| types | registry block | heap (rough) | total |
|------:|---------------:|-------------:|------:|
| 10 | ~0.9 KB | ~1–2 KB | ~2 KB |
| 100 | ~8.8 KB | ~10–20 KB | ~20 KB |
| 1,000 | ~88 KB | ~100–200 KB | ~0.2 MB |
| 10,000 | ~0.9 MB | ~1–2 MB | ~2 MB |

Because the registry never unregisters (§6.6), this is cumulative over the
process lifetime: the total is `88 x (types across all modules ever parsed)`, not
just the live modules. A transient 1,000-type module leaves ~0.2 MB behind.

### 12.3 Relationship to the rest of this document

- **Partly related (to interning).** The canonicalization in §6 changes how many
  types are stored: structurally identical `rec` groups are stored once, so
  repeated function signatures — the bulk of types in real modules — stop
  multiplying the `88 x N` term. Interning therefore *improves* the footprint,
  and its cost must be weighed against the extra canonical representation. Once
  a global canonical table exists, per-module `Subtype` blocks become largely
  redundant: they could shrink to a `LocalTypeIdx -> TypeId` map, reading shape
  data from the shared, compact canonical table.
- **Mostly separate (layout vs. identity).** The 88-byte figure is a property of
  the current in-memory layout (`std::variant` + owning `std::vector`s), not of
  type identity or `match`. Shrinking it is an independent optimization, in the
  same category as the GC heap (§11) rather than a prerequisite for interning;
  neither requires the other.

### 12.4 Possible future optimizations (not planned here)

- **Drop the `std::variant`.** Replace `CompositeDefinition` with a manually
  tagged union, or a structure-of-arrays registry (parallel `kind` / `isFinal` /
  payload-index arrays), so struct and array entries do not reserve `FuncType`'s
  48 bytes.
- **Share component storage.** Have `FuncType` / `StructType` reference slices of
  a registry-owned arena instead of owning `std::vector`s. Canonicalization
  already centralizes types globally, making shared, immutable parameter/result
  arrays natural and eliminating per-type allocations.
- **Shrink `supertypeIndices`.** Since §6.2 fixes at most one supertype, a single
  `TypeId` plus a "none" marker replaces the 24-byte vector.
- **Inline `isFinal`** into the kind tag, removing the padding.
- **Measure the dedup ratio first.** Before investing in layout work, measure how
  many types interning collapses (M8); it may already remove most of the
  pressure.

A compact representation on the order of 8–16 bytes per type for the common
function-signature case (plus shared component storage) looks achievable — a
roughly 5–10x cut in the fixed term — but that is a separate project.

Reproduce the figures with:

    #include "WasmType.hpp"
    printf("%zu\n", sizeof(WASM::Subtype));   // 88 on x86-64 / libstdc++

## 13. References

- WebAssembly 3.0 specification (living draft):
  - Validation — Matching:
    https://webassembly.github.io/spec/core/valid/matching.html
  - Validation — Conventions (expansion, recursive type indices, closed types):
    https://webassembly.github.io/spec/core/valid/conventions.html
  - Validation — Modules (incremental type validation; closed modules):
    https://webassembly.github.io/spec/core/valid/modules.html
  - Appendices (Type Soundness → Subtyping; Matching):
    https://webassembly.github.io/spec/core/appendix/index.html
  - Change History — Release 3.0 (feature list):
    https://webassembly.github.io/spec/core/appendix/changes.html
- GC proposal design documents (formal rules for equivalence and subtyping):
  - `proposals/gc/Overview.md`
  - `proposals/gc/MVP.md` (see "Appendix: Formal Rules for Types")
- Tests carrying relevant coverage:
  - `type-canon.wast` — recursive `rec` groups (within a module).
  - `ref_cast.wast`, `ref_test.wast`, `br_on_cast*.wast` — cast/`match` behaviour.
  - `linking.wast`, `imports*.wast` — import type matching across modules.
