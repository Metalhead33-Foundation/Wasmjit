# Type Identity {#doc_type_identity}

*Equivalence, canonicalisation, and matching*

Status: design note / implementation reference (revision 4; §13 study added
2026-11-12).
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
test plan (§9), and records decisions that were previously open (§7). Revision 3
adds the Store-owned/one-canonical-instance target and the JIT cutover pointer
in §6.7/§10/§12.3; the detailed TM-6…TM-11 plan lives in
[`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.10. Revision 4 adds §13,
a feasibility study of host-object type identity; it records no implementation
and changes no identity rule in §6.

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
| `TypeId` | Dense `uint32_t`, assigned by the store's `TypeRegistry`. Identifies one canonical defined type **within that store** (runtime-scoped since `ES-2` option B; two stores have independent id spaces). |
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

**Component domain.** The identifiers above describe the core domain. Component
types now have store `TypeId`s for their definitions, but they are not
structurally interned and the validator still resolves them through a local
`ResolvedType` graph. The planned shared store, component `TypeId` domain,
resource identity rules and migration stages are in
[`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md).

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
                      supertypes: [CanonHeapType],   // Global after interning
                      depth,              // 1 + max(supertype depths); 0 if none
                      ... kind-specific payload ... }

Notes:

- `Rec(memberIndex)` is the **absolute index of the member within its `rec`
  group** — exactly the spec's `rec.i`. Groups do not nest, so no de Bruijn
  depth or relative offset is needed.
- `CanonValueType` carries numeric/vector kinds and nullability. A reference type
  is a heap type plus nullability; nullability is not part of the heap type.
- **Supertypes are a list.** The abstract syntax is `sub final? x* ct`, and the
  GC proposal's formal rules apply the subtype check `(C ⊢ st <: expand(C(x)))*`
  to every declared supertype, so a canonical type stores a **list** of
  supertypes (in practice 0 or 1) plus a derived `depth` (`1 + max(supertype
  depths)`, or 0 when there are none). Each supertype must be *strictly earlier*
  than the subtype; that ordering is enforced at parse time, not by rejecting
  additional supertypes.
- A canonical type's identity includes `isFinal` and its supertypes.
- **Custom Descriptors.** A struct type's optional `descriptor`/`describes`
  clauses are part of identity, exactly like fields and supertypes. Both name a
  type in the same `rec` group; the pair must agree, must not be
  self-referential, and is serialized into the interning key. Exact heap types
  (`exact $t`) also carry an `isExact` bit distinct from ordinary `$t`, because
  `(ref exact $t)` and `(ref $t)` are not the same type. The exact form is a
  subtype of its base form, but an exact target accepts only the same exact
  source; it is not a supertype of the base type's declared subtypes. Exact
  function imports (`0x20 typeidx`) record `ImportFunction::isExact`; exports do
  not repeat the flag.
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
   each member's `depth` from its supertypes (each always an earlier member or an
   earlier group), and store the canonical group.
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
      else:                                walk actual's supertype closure for expected

    matches(actual: TypeId, expected: TypeId)   // concrete-only wrapper
      == matchesHeap(Global(actual), Global(expected))

Because a supertype is always a strictly earlier definition, the supertype graph
is acyclic; the walk needs only a visited set (or a lazily built ancestor
bitset) and is bounded by `depth`. If profiling ever shows it matters, a per-type
set/array makes it O(1).

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

Two rules in that comparator are worth stating explicitly, because both were
wrong in the first implementation and a real toolchain module hit each
([`languages/LF2_DEEP_F3_GC.md`](languages/LF2_DEEP_F3_GC.md)):

- **Bottom heap types are subtypes of concrete types.** `none` is the bottom of
  the internal hierarchy, so `(ref null none)` (`nullref`) is a subtype of
  `(ref null $struct)` and of `(ref null $array)`; `nofunc` is a subtype of any
  concrete function type, `nocont` of any concrete continuation type. Those are
  the only legal abstract → concrete edges; a *non-bottom* abstract type (`any`,
  `eq`, `struct`, `array`, `func`, …) is a supertype of concrete types and must
  not match one. `noextern`/`noexn` have no concrete supertype.
- **A resolved heap type compares only its active payload.** `CanonHeapType`
  carries `abstract`, `id` and `memberIndex`, and exactly one applies for a given
  `kind`. Equality must therefore compare the active member (plus `isExact`) and
  ignore the dormant ones: a stale `memberIndex` on a `Global` reference is not a
  different type, and treating it as one makes a *mutable* field fail its
  invariance check even when both sides name the same `TypeId`.

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

The remaining cutover is documented in
[`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.10.3 (TM-7): the JIT
takes a local-index → `TypeId` map plus `CoreTypeView`, not a
`span<const Subtype>`; `ModuleInstance::subtypeForTypeId` becomes a store lookup
rather than a scan of the current module. That also fixes cross-module GC walking
for a type the walking module did not itself declare.

### 6.8 Ill-formed input policy

Canonicalization assumes well-formed input, and the policy for anything else is
**reject**. A forward reference to a later group, an out-of-range index, a
supertype that is not strictly earlier than the subtype, or an invalid `sub`
annotation causes the module to fail registration, and it is never instantiated.
No tolerance mode is planned.

## 7. Decisions record

| Question | Decision |
|----------|----------|
| Recursive reference encoding | Absolute member index (`rec.i`), not relative offset or depth. |
| Number of supertypes | A list (the spec allows `x*`); each strictly earlier than the subtype; `depth` derived. |
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

**Status: M0–M8 shipped.** What each one left behind is recorded in §10 rather
than in this list, which stays as written so the order and the "done when"
clauses remain readable. M8 measured the dedup ratio and registry footprint
(§12) and decided against the `Subtype` shrink: `Subtype` is parser scratch, not
the long-lived store record.

**M0 — Invariants and strong types.**
- Introduce `LocalTypeIdx`, `TypeId`, `RecGroupId` as distinct types and
  `Module::typeId(LocalTypeIdx)`.
- Add counters for types parsed versus types interned (used in M8).
- *Done when:* the engine builds with the strong types in place and no behaviour
  change.

**M1 — Preserve `rec` groups.**
- In `Module::processSubtypes`, record `groups: vector<{first, count}>` alongside
  the flat `types` span. A bare type is a group of one.
- Reject forward references to later groups and supertypes that are not strictly
  earlier than the subtype. Do **not** reject multiple supertypes: the spec
  allows a list.
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
- Decide whether the `Subtype` shrink in §12 is worth doing.
- Update §7 of this note's predecessor ("Current state and gaps", now §10) to
  match the new reality.
- *Done:* `TYPE_IDENTITY M8` reports 250:1 on repeated signatures and 3.9:1 on a
  growing-arity stress workload, with 16-byte store records; the parser-side
  `Subtype` shrink is not scheduled because it is transient (§12).

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
| Multiple declared supertypes | Accepted; each must be strictly earlier. |
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

- `TypeRegistry` (`WasmBase/WasmTypeRegistry.hpp`) owns a compact canonical
  `TypeStore`. `internModule` consumes a temporary parser vector and retains no
  per-module `Subtype` block; `Module` keeps `typeIds`, `typeGroups` and a
  TypeId→local-index map only.
- `Module::typeView`/`typeId` are the consumer API; type indices remain
  module-local. This closes the core-module half of the `TM-6`/`TM-10` ownership
  gap. Component `CoreRecType` still stores parser `Subtype`s and is tracked in
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.11.2. A validated
  `CoreRecType` stores Store `TypeId`s and clears its parser `Subtype` scratch.
- `Module::isSubtype` resolves local indices to canonical `TypeId`s and delegates
  to `TypeRegistry::matches`, so structural composite subtyping and cross-module
  matching use the same relation as the rest of the store.
- The JIT/validator migration from `Subtype` to `CoreTypeView` is complete for
  core modules; `funcTypeAt` is a Store-derived `FuncType` signature view for
  compile/test paths. See [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md)
  §7.10.3/§7.11.1.
- ~~`ModuleInstance::refMatchesHeapType` and GC object headers use module-local
  indices.~~ **Done (M6):** GC object headers store a canonical `TypeId`
  (`GcHeap::owns` locates the object by region, then the header is read);
  `refMatchesHeapType` resolves the expected heap type
  against its module and matches via `TypeRegistry::matchesHeap` (concrete and
  abstract), so casts work across modules.
- ~~`Callable::typeIndex` is a module-local index.~~ **Done (M6):** `Callable`
  now carries a module-local `localTypeIdx` *and* a canonical `typeId`
  (`kNone` for native imports); `call_indirect`/`call_ref`/function imports
  match on `typeId`.
- ~~The parser **flattens `rec` groups** into one `Subtype` vector
  (`Module::processSubtypes`), so group boundaries are currently lost — the
  information canonicalization needs first.~~ **Done (M1–M4):** the parser
  preserves `rec` groups in `TypeGroup`s, `TypeRegistry::internModule`
  canonicalizes each group to a closed form and hash-conses it to a stable
  `TypeId`, and `TypeRegistry::matches` / `matchesHeap` do cross-module matching.
- ~~`LibJitTypeTranslator`'s cache is module-local; a per-module `reset()` is
  required for correctness. There is no stable type identity to cache
  against today.~~ **Done (M7):** the translator holds a persistent
  `unordered_map<TypeId, jit_type_t>` keyed on the canonical `TypeId`, so
  translated types are shared across modules and translation order no longer
  matters.
- **Component types now intern through the shared store.** Validation rewrites
  the local `ResolvedType` graph into compact `TypeStore` records with canonical
  `TypeId` children, so closed records intern across components; parser syntax
  records remain an AST. Resource parameters substitute/regenerate during
  instantiation, open types intern as alpha-equivalent `SchemeId`s (a separate
  identity domain from `TypeId`), and ABI flat/layout metadata is keyed by
  `(TypeId, options)`; the ABI runtime/remaining compound layouts remain in
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.11.3–§7.11.4.

## 11. Non-goals / deferred

- The GC **heap** itself (allocation, collection) — a separate concern.
- Host/extern type identity (the JS API side) — not modelled here.
- Type **reflection / introspection** APIs.
- `VMContext` no longer needs layout work for the features shipped here:
  multi-memory and multi-table are wired through the `memories`/`tables`
  index-space arrays (`VMContext`). GC constant expressions are evaluated by a
  small stack machine in `ModuleInstance::evalConstantExpr`.
- Replacing the `Subtype` layout here. The canonical form is a second,
  index-oriented representation in this note; the compact-store refactor is in
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md). Revision 5 of that note
  removes the per-module `Subtype` block: a loaded core `Module` retains only
  `typeIds`/`typeGroups`, and the JIT/validator/runtime use `CoreTypeView`.
  `Subtype` remains the component parser's `CoreRecType` scratch type.

## 12. Type storage footprint (`Subtype` and the compact store)

The registry does not store a module's types as a `std::vector<Subtype>` block:
`internModule` consumes a temporary parser vector and keeps compact Store
records. The parser/component `Subtype` cost below therefore applies to the
temporary type section and to component `CoreRecType`, not to the long-lived
core `Module`.

### 12.1 Measured size

Measured on the current toolchain (x86-64, libstdc++, 2026-11-03,
`4d52031`) with `sizeof`:

| type | bytes |
|------|------:|
| `TypeRecord` (long-lived store record) | **16** |
| `TypeId` | **4** |
| `Subtype` (parser scratch) | **104** |
| `std::vector<uint32_t> supertypeIndices` | 24 |
| `CompositeDefinition` (`std::variant`) | 56 |
| `FuncType` | 48 (two `std::vector`s) |
| `StructType` | 24 |
| `ArrayType` | 16 |
| `FieldType` | 16 |
| `StorageType` | 12 |
| `ValueType` | 8 |

The 104 bytes of the parser-side `Subtype` break down as:

    bool isFinal                             1
    bool isShared                            1
    padding                                  6
    std::vector<uint32_t> supertypeIndices  24
    std::optional<uint32_t> descriptorIndex  8
    std::optional<uint32_t> describesIndex   8
    CompositeDefinition (variant)           56
                                           ---
                                           104

The variant is `max(FuncType 48, StructType 24, ArrayType 16)` plus a 1-byte
discriminant padded to 8, i.e. 56. Consequently **every** parser `Subtype` —
including a struct or an array — reserves the 48-byte `FuncType` payload (two
`std::vector`s), and every type pays 24 bytes for `supertypeIndices` even when it
has none. That cost is transient: the parser vector is discarded after
interning, and a loaded `Module` retains only `typeIds`/`typeGroups`.

### 12.2 Measured registry footprint

The long-lived cost is the Store's compact records plus their shared payload
pools. The M8 measurement (`TYPE_IDENTITY M8: dedup ratio and registry
footprint`, `Test/main.cpp`) uses two synthetic workloads shaped like real
repeated signatures:

| Workload | parsed | interned | groups | `payloadBytes()` | allocations | dedup |
|---|---:|---:|---:|---:|---:|---:|
| A: 250 modules × the same 4 signatures | 1,000 | 4 | 1 | 400 B | 4 | **250:1** |
| B: 100 modules × (6 common + 2 growing-arity unique) | 800 | 206 | 101 | 735,680 B | 4 | **≈3.9:1** |

Workload A is the representative case: real modules repeat function
signatures, and interning collapses them to one record each. Workload B is a
stress probe: its unique signatures grow from 1 to 200 parameters, so the
payload pools — not the 16-byte records — dominate (about 20,000 storage entries
in 735 KB). The measurement therefore pins two facts:

- The fixed term is tiny: 16 bytes per interned record plus 4 bytes per
  `TypeId`, not the parser `Subtype`'s 104 bytes.
- Pool size follows the **width** of the signatures that survive interning, not
  the number of modules that repeated them.

Because the registry never unregisters (§6.6), the records are cumulative over
the process lifetime, but the parser `Subtype` blocks are not: they are freed at
the end of each module's type section.

### 12.3 Relationship to the rest of this document

- **Related (to interning).** The canonicalization in §6 changes how many types
  are stored: structurally identical `rec` groups are stored once, so repeated
  function signatures — the bulk of types in real modules — stop multiplying the
  store population. `TypeId` is the only canonical identity, there is exactly
  one `TypeRecord` per canonical type, and per-module `Subtype` blocks do not
  exist (TM-6). A module keeps only its `LocalTypeIdx -> TypeId` map and views
  into the Store; see
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.10.1–§7.10.2.
- **Mostly separate (layout vs. identity).** The 104-byte parser figure is a
  property of the scratch layout (`std::variant` + owning `std::vector`s), not
  of type identity or `match`. Shrinking it would optimize a temporary rather
  than the registry; the measured long-lived record is already 16 bytes.


### 12.4 Possible future optimizations (planned separately)

The layout work below is specified and sequenced in
[`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md); this section remains the
footprint rationale. **M8 decision:** none of it is scheduled. The long-lived
record is already 16 bytes, and the 104-byte `Subtype` is parser scratch that is
freed after the type section, so shrinking it would not reduce registry memory
or steady-state module cost.

- **Drop the `std::variant`.** Replace `CompositeDefinition` with a manually
  tagged union, or a structure-of-arrays registry (parallel `kind` / `isFinal` /
  payload-index arrays), so struct and array entries do not reserve `FuncType`'s
  48 bytes.
- **Share component storage.** Have `FuncType` / `StructType` reference slices of
  a registry-owned arena instead of owning `std::vector`s. Canonicalization
  already centralizes types globally, making shared, immutable parameter/result
  arrays natural and eliminating per-type allocations.
- **Shrink `supertypeIndices`.** In practice a type has 0 or 1 supertypes, so a
  small inline form (one `TypeId` plus a "none" marker, spilling to a vector only
  when needed) replaces the 24-byte vector.
- **Inline `isFinal`** into the kind tag, removing the padding.
- **One canonical record, no retained syntax.** `TypeId` is the only canonical
  identity; the store keeps exactly one `TypeRecord` per canonical type, and
  `Module`/`Component` keep only local-index maps and views. `Subtype` becomes a
  parser temporary instead of a per-module stored block. This is the TM-6
  ownership/dedup target in
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §7.10.1–§7.10.2.
- **The dedup ratio is measured.** M8 found 250:1 on repeated signatures and
  3.9:1 on a growing-arity stress workload; interning already removes most of
  the pressure that motivated the layout work. The remaining cost is the width
  of the signatures that survive, which the shrink below does not address.

A compact representation on the order of 8–16 bytes per type for the common
function-signature case (plus shared component storage) looks achievable — a
roughly 5–10x cut in the fixed term. That is the target of
[`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md), not of this note.

Reproduce the figures with:

    #include "WasmType.hpp"
    printf("%zu\n", sizeof(WASM::Subtype));    // 104 on x86-64 / libstdc++
    #include "WasmTypeStore.hpp"
    printf("%zu\n", sizeof(WASM::TypeRecord)); // 16 on x86-64 / libstdc++

## 13. Host-object type identity (study)

Status: feasibility study (2026-11-12; implementation snapshot `9b2ca92`). No
implementation and no change to any identity rule in §6. Owner labels:
core/component identity is **ENG**; a host registry and its descriptors are
**EMB**; generated descriptors and bindings are **TOOL**; C’s inability to
declare a typed `(ref $T)` is **LANG**.

### 13.1 The question

[`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §17 studies a GObject-shaped host
object runtime. This section answers only the identity half: could a `GType`-like
registry give C-family guests a typed object identity, and could GC languages
share it silently?

Three things must stay separate:

1. **Representation** — the machine word (`void*`, `u64`, a resource handle).
2. **Type relation** — Wasm `match`, `ref.test`/`ref.cast`, or a host `is_a`.
3. **Object model** — creation, fields, methods, inheritance, lifetime.

A host registry can own (3), and a host `is_a`/`cast` is a function call. It
cannot inject a new relation into (2) for a C guest, and it cannot make a
nominal host type participate in `ref.cast`.

### 13.2 What `TypeId` is, and is not

- `TypeId` is a dense `uint32_t` assigned by the Store’s `TypeRegistry` (§6.1).
  It identifies one **canonical core Wasm defined type** within that Store.
- It is **structural over closed forms** (§3.2): two modules that declare
  structurally identical `rec` groups receive the same `TypeId` after interning.
- It is **Store-scoped**, not process-global; two runtimes have independent id
  spaces (§6.1, `EMBEDDING_SURFACE.md` §4.4).
- It is **not** a name, not a serialized ABI value, not an object address, and
  not stable across runs or Stores. A generated header must not hard-code one
  (`EMBEDDING_SURFACE.md` §14.4).
- A GC object header stores the `TypeId`; `ref.test`/`ref.cast` read it
  (`ABI.md` “GC Object and Heap ABI”, `GC.md` §4.3).
- The JIT caches lowered types by it (§6.7). Anything that changes its meaning
  is an engine change, not an adapter decision.

`LocalTypeIdx` remains module-local; `RecGroupId` identifies an interned group.
They are distinct struct types, not aliases (§6.1). None of them is a host type.

### 13.3 Three identity domains

| Domain | Key | Scope | Answers | Cannot answer |
|---|---|---|---|---|
| **Core Wasm type** | `TypeId` | one Store | structural equivalence and `match` of core types; `ref.test`/`ref.cast` on GC objects | a host class name, method set or C++ RTTI |
| **Component type** | Store `TypeId` for closed definitions, `SchemeId` for open types, nominal resource identity | one Store / component instance | WIT structural types, `own`/`borrow`, resource handle validity | a core `(ref $T)` field relation; host object layout |
| **Host object type** | stable name + versioned descriptor; host id | a host runtime (may span Stores) | host object kind, fields, methods, interfaces, host `is_a` | Wasm `match`/`ref.cast`; cross-Store identity |

A handle must say which domain it belongs to. Comparing a host id with a
`TypeId`, or moving a handle between Stores, is invalid even when the machine
word is the same size. This is the same rule as `EMBEDDING_SURFACE.md` §14.5:
kind, identity and ownership are not the same check.

### 13.4 Nominal GType versus structural Wasm

GType is **nominal**: a type is registered, has a name and a parent, and may
implement interfaces; two structurally identical C structs are different GTypes
unless they are registered and related. Wasm core types are **structural**:
after canonicalization, two identical `rec` groups are the same type, even from
different modules, and declared supertypes are part of the identity (§3.2–§3.4).

That difference is not an implementation detail; it decides what can be shared:

- A **Wasm GC object** can be shared by two GC modules because the *type* is
  structural and the header carries the canonical `TypeId`. Its language-level
  class identity is still each runtime’s own
  (`EMBEDDING_SURFACE.md` §14.6).
- A **host object** can be shared through a handle because both sides agree on a
  **descriptor name**, not because Wasm’s `match` understands the host type.
  The host `is_a` is a function over the host registry.
- A **C guest** cannot declare `(ref $T)` at all (`LG-1`); a host descriptor
  does not change its type system. It can hold a handle and call functions.

“Everyone understands it” is therefore only possible at the **handle plus
descriptor** level, or at the **component resource** level. It is not possible
at the Wasm type level unless every producer emits the same canonical Wasm type.

### 13.5 What a host descriptor can contain

A host runtime needs enough metadata to validate a handle and expose an API,
without leaking a layout:

| Field | Purpose | Constraint |
|---|---|---|
| stable name | cross-run/cross-Store identity | reverse-DNS or WIT-style name; not a numeric id |
| version | descriptor evolution | a field reorder or type change is a new version |
| kind | object/record/array/resource | determines which operations are legal |
| parent / interfaces | host-side hierarchy and capability query | names, not Wasm heap types; no `(ref $I)` |
| fields | name, logical type, accessor id | no raw offset or `sizeof` in the guest-visible descriptor |
| methods | name, signature id | function-table id or WIT function, not a C++ vtable pointer |
| destructor | release hook id | host-side id; a component resource uses `resource.drop` |
| thread affinity | which agent may call | host policy; not inferred from the Wasm type |
| flags | copyable/weak/finalizable/etc. | host policy; document the default |

The descriptor is **host metadata**. A guest sees functions, not the descriptor,
unless the API deliberately exposes introspection. A generated C header should
carry the stable name and typedefs, never a `TypeId`, a C++ `typeid` or a field
offset (`EMBEDDING_SURFACE.md` §14.4). The host resolves the name to its own id;
the Store resolves a Wasm GC type to its `TypeId`; the two mappings meet only
where a host object is deliberately wrapped in a Wasm GC object.

### 13.6 Interfaces and subtyping

Wasm GC has a supertype list and structural width subtyping; it has no interface
types (§3.1). A GObject-style interface is a **nominal capability set**, so it
cannot be a `(ref $I)` and cannot participate in `ref.cast`. The honest models
are:

1. **Capability query** — `query_interface(handle, name) -> handle` (or null),
   and the returned handle supports the interface’s functions. Host `is_a` is a
   function call, not a Wasm type test.
2. **Separate resource type** — a WIT resource per interface, with `own`/`borrow`
   and its own destructor policy. This fits the Component Model and keeps the
   nominal boundary explicit.
3. **Declared Wasm supertypes** — if the object is a Wasm GC struct and the
   interface can be expressed as a structural supertype, put it in the Wasm type
   at module compile time. It cannot be added later; `TypeId` identity includes
   the supertype list (§6.2).

A host runtime can offer all three, but it must not claim that a host interface
is a `ref.test` target. A generated header should expose `query_interface` and
the interface’s functions, not a cast operator that pretends to be `ref.cast`.

### 13.7 What the runtime must not claim

- **Not a global `TypeId`.** `TypeId` is Store-scoped (§6.1). A host registry
  that spans Stores needs its own domain and a mapping, not a reused id.
- **Not a replacement for `TypeRegistry`.** The registry owns core type
  identity; the host registry owns host object metadata. Sharing an interning
  table for names is possible; sharing the id space is not.
- **Not a stable ABI if it exposes C++ layout or RTTI.** `typeid`, vtable
  pointers, `sizeof(T)` and raw offsets are process-local and compiler-specific.
- **Not a way for C to declare `(ref $T)`.** That is `LANG`/`TOOL` (`LG-1`).
- **Not cross-Store object identity.** A host pointer in one Store/runtime is
  not valid in another; a resource handle is per instance. `own` moves a
  resource handle; it does not make a pointer valid in a new Store.
- **Not a finalizer or a GC.** The engine has no finalizer; a host object is
  released explicitly, from `instanceDestroying`, or by `resource.drop`.

### 13.8 Relationship to component resources

Component resources are the closest standard identity for a host object:

- a resource type is **nominal**; an `own`/`borrow` handle belongs to a
  component instance and is validated against the expected resource type;
- `resource.rep` exposes the engine-defined representation (the host can put a
  table index there), and `resource.drop` runs the declared destructor;
- the handle table is per instance and bounded (`maxLiveHandles`), and stale,
  mistyped and out-of-scope handles are rejected
  (`WasmComponentRuntime.hpp`, `COMPONENT_MODEL.md` §2.3/§4.3).

A host runtime type maps naturally to a **WIT resource type**: the WIT resource
is the guest-visible identity, and the HOR’s type descriptor is the `rep`
metadata. This keeps the identity domain explicit and avoids inventing a second
wire ABI. It also means the resource’s nominal identity, not a host name, is
what the component validator checks; two components with the same resource name
are not the same resource unless they are linked/aliased.

For a core module that is not a component, the fallback is a synthetic module
plus a host handle table; that path must document its own host type domain and
must not pretend it is a Wasm `TypeId`.

### 13.9 Verdict

| Question | Verdict |
|---|---|
| Can a GType-like registry provide a host object identity? | **Yes**, as an auxiliary host domain keyed by stable names and descriptors |
| Can it extend Wasm’s `match`/`ref.cast`? | **No**; host types are not Wasm heap types |
| Can C declare `(ref $T)` through it? | **No**; a handle is not a reference type, and the C type system does not change |
| Can two GC languages share a WIT resource? | **Yes**, at the component boundary, with `own`/`borrow` and nominal resource identity |
| Can two GC languages share a core Wasm GC struct? | **Yes**, if they emit the same canonical type in the same Store; language class semantics still differ |
| Can a host object and a Wasm GC object be the same type? | **No**; they can be bridged by a wrapper/resource, not identified |
| Does this need an engine change? | **No**; `TypeId`, `TypeRegistry` and resource handles stay as they are |

### 13.10 What would make this a tested claim

1. **Structural vs nominal.** Two modules in one Store declare structurally
   identical Wasm structs; their `TypeId`s are equal and a typed reference
   crosses. Two host types with identical fields but different stable names are
   different host types; `is_a` is false both ways. This is the key test of the
   distinction.
2. **Store boundary.** The same stable type name in two Stores maps to different
   `TypeId`s; a host handle from one Store is rejected by the other.
3. **Cast separation.** A host handle passed where a `(ref $T)` is expected is
   rejected (or treated as `externref`), not silently matched; a Wasm GC object
   and a host object with the same fields are not interchangeable.
4. **Interface query.** An object implements two interfaces; each query returns
   a handle that supports only its own functions; a method from the other
   interface is rejected.
5. **Resource parity.** A WIT resource version of the same API: `own` moves,
   `borrow` is scoped, `resource.drop` runs the destructor once, and a handle
   from another instance is rejected.
6. **Descriptor evolution.** Adding a field or renaming a type is a new version;
   an old handle does not silently resolve to the new layout.

Until these exist, the verdict is a design argument, not measured evidence.


## 14. References

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
