# Type Identity, Equivalence, and Matching

Status: design note / implementation reference.
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
  group (in-group references as de Bruijn indices, out-of-group references as
  already-interned global ids), hash-cons it, and assign a stable global
  identity. Two structurally equal groups — from any module — then share one
  identity.
- **O(1) equivalence.** Equivalence collapses to an integer comparison.
- **Cross-module `match`.** Subtyping becomes a walk over a canonical supertype
  DAG, so import checking and casts work naturally across modules.
- **Memory dedup.** Repeated function signatures (the bulk of types in real
  modules) collapse to one canonical entry.
- **Stable ids for the JIT.** `LibJitTypeTranslator` can key its caches by the
  global id instead of module-local indices, enabling cross-module reuse of
  lowered `jit_type_t`s (see §6.5).

None of this is mandated; it is an implementation strategy that *realizes* the
spec's relations efficiently and safely.

## 6. Proposed design for this engine

### 6.1 Global identity

- `TypeId`: dense `uint32_t`, assigned by `TypeRegistry`.
- `RecGroupId`: identity of an interned group (a contiguous run of `TypeId`s).
- `LocalTypeIdx`: the existing module-local `uint32_t` used by `Subtype`,
  `ValueType::heapType`, and `Callable::typeIndex` today.

`Module` keeps its `types` span (layout, consumed by the JIT) and gains a
parallel `typeIds` vector mapping `LocalTypeIdx -> TypeId`.

### 6.2 Canonical form

A canonical type is a recursion-free description whose references are either
abstract heap types or global ids:

    CanonTypeRef  = Abstract(AbstractHeapType)
                  | Global(TypeId)
                  | Rec(depth)            // during construction only
    CanonStorage  = { isPacked, CanonTypeRef }
    CanonField    = { CanonStorage, isMutable }
    CanonicalType = { kind: Func|Struct|Array, isFinal,
                      supertypes: [CanonTypeRef],
                      ... kind-specific payload ... }

`Rec(depth)` is a de Bruijn index into the enclosing group, used while hashing a
group. After interning, `Rec` references are resolved to `Global(TypeId)`, so the
stored graph is purely id-based.

### 6.3 Streaming canonicalization

Process the type section group by group, in order:

1. Translate each member's component references: in-group index -> `Rec{member
   index}`; earlier index -> `Global{localToGlobal[index]}`; later index ->
   invalid (reject or tolerate per policy).
2. Hash the whole group (structural, de Bruijn-stable) and look it up.
3. On a hit, reuse the group's ids; on a miss, allocate fresh ids and store the
   canonical group.
4. Record `localToGlobal[groupStart + j]`.

Because earlier groups are canonicalized first, out-of-group references are
already global ids. Group identity is therefore well-founded and the whole type
section needs a single pass.

### 6.4 `match` across modules

With canonical ids:

    matches(actual, expected):
      if actual == expected:            true
      if expected is abstract:          classify actual against the hierarchy
      else:                             search actual's supertype DAG (memoized,
                                        cycle-safe) for expected

Cross-module behaviour then falls out of global ids. The structural composite
comparator (func contravariance, struct width, mutable-field invariance, array
variance) is needed mainly for *validation* and for checking declared `sub`
annotations; import and cast matching is primarily id equality plus supertype
reachability plus abstract classification.

### 6.5 JIT type caching on stable identities

`LibJitTypeTranslator` currently keys on `ValueType{opcode, heapType}` where
`heapType` is a **module-local** index, and indexes `translatedTypes` by that
local index. That is precisely why it needs a per-module `reset()` for
correctness: the keys are not globally meaningful.

With a global `TypeId`, the cache can be keyed by identity:

- concrete reference types -> `TypeId`;
- abstract heap types -> `(opcode, AbstractHeapType)`;
- `translatedTypes` -> a map `TypeId -> jit_type_t` (plus a per-module
  `LocalTypeIdx -> TypeId` lookup).

`jit_type_t` is a per-`jit_context` structural entity, so identical canonical
types may share one. The per-module reset can then be dropped, or kept only as a
defensive measure.

## 7. Current state and gaps in this codebase

- `TypeRegistry` (`WasmBase/WasmTypeRegistry.hpp`) owns type blocks but has **no
  identity or interning**: `registerModule` hands out a
  `std::span<const Subtype>` per module.
- `Module::types` is that span; all type indices remain module-local.
- `Module::isSubtype` walks declared `supertypeIndices` (module-local) and does
  not implement structural composite subtyping or cross-module matching.
- `ModuleInstance::refMatchesHeapType` and GC object headers use module-local
  indices; an object's runtime type tag is a local index, not a global id.
- The parser **flattens `rec` groups** into one `Subtype` vector
  (`Module::processSubtypes`), so group boundaries are currently lost — the
  information canonicalization needs first.
- `LibJitTypeTranslator`'s cache is module-local; a per-module `reset()` is
  required for correctness (`LibJit/LibJitTypeTranslation.cpp`). There is no
  stable type identity to cache against today.

## 8. Non-goals / deferred

- The GC **heap** itself (allocation, collection) — a separate concern.
- Host/extern type identity (the JS API side) — not modelled here.
- A full **validation** algorithm. Canonicalization assumes well-formed input;
  a policy is needed for ill-formed modules (tolerate or reject).
- Type **reflection / introspection** APIs.
- Replacing the `Subtype` layout representation. The canonical form is a second,
  index-oriented representation; dedup is expected to offset its cost.

## 9. References

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



