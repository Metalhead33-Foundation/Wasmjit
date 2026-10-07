# Garbage Collection (Wasm 3.0) — Implementation Notes, Spec Conformance, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmType`, `WasmTypeRegistry`, `WasmModuleInstance`,
`LibJitTypeTranslator`, `LibjitOpcodeDispatcher`, or the test suite.

This note is deliberately split the same way as
[`TYPE_IDENTITY.md`](TYPE_IDENTITY.md):

1. what the WebAssembly 3.0 specification **demands** of a GC-capable engine;
2. what it explicitly does **not** demand (and where an engine is free);
3. what `WasmJit` **currently implements**;
4. the **gaps** between (3) and (1);
5. where we can and should play **fast and loose** on purpose, because this
   project is a spiritual successor to Quake 3's QVM rather than a
   browser-embedded engine (see [`ABI.md`](ABI.md) "Design Philosophy").

It contains **no new code**. Shipped code and this document are the two
artefacts; the plan in §7 says which gaps are worth closing and which are not.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does Wasm 3.0 require for GC? |
| §3 | What does Wasm 3.0 *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short of §2? |
| §6 | Where do we *choose* to deviate, and why? |
| §7 | Milestones to close the important gaps |
| §8 | Indicative test survey |
| §9 | References |
| §10 | Beyond 3.0 (very last priority): Stringref + JS String Builtins |

Terminology follows the spec: a **heap type** classifies a reference; a
**reference type** is `(ref null? ht)`; a **defined type** (`deftype`) is a
`sub`-annotated composite (`func` / `struct` / `array`); a **`rec` group** is a
mutually recursive block of defined types. "`match`" is the spec's subtyping
relation, written `C ⊢ t ≤ t'`.

## 2. What Wasm 3.0 demands (the GC surface we must honour to be conformant)

WebAssembly 3.0 (change history dated 2026-10-03) folds in the garbage
collection proposal together with reference types, typed function references,
tail calls, exception handling, multi-memory, memory64, extended `const`,
relaxed SIMD, annotations and profiles. GC is therefore part of the core
language, not an add-on.

### 2.1 Types

- **Three disjoint hierarchies.** Internal (`any` → `eq` → `i31` / `struct` /
  `array` / concrete aggregates), external (`extern`), and function (`func`).
  Bottom types are `none`, `noextern`, `nofunc`.
- **Abstract heap types** and **concrete heap types** (a `typeidx`), with the
  subtype lattice `i31 <: eq <: any`, `struct <: eq`, `array <: eq`,
  `none <: (every internal)`, etc.
- **Defined types** with `final?` and a **list** of declared supertypes:
  `sub final? x* ct`. Composite types are `func p* r*`, `struct ft*`, `array ft`.
- **Fields**: `mut? st`, storage type `st` is a value type or a packed `i8`/`i16`.
- **`rec` groups**: type indices inside a group may refer to the group's own
  members (including later ones); references to other groups follow normal
  scoping. A bare type is a group of one.
- **Reference abbreviations**: `anyref`, `eqref`, `i31ref`, `structref`,
  `arrayref`, `nullref`, `nullexternref`, `nullfuncref` are exactly
  `(ref null X)`.
- **Equivalence and matching** are specified as *relations* on types
  (expansion, rolling/unrolling, closed types, `expand`/`unroll`), applied in
  validation, at link time (import matching) and at execution time (casts).
  Struct width, mutable-field invariance, function parameter
  contravariance / result covariance, and array covariance all fall out of
  the relation.

### 2.2 Instructions

| Family | Instructions |
|---|---|
| struct | `struct.new`, `struct.new_default`, `struct.get`, `struct.get_s`, `struct.get_u`, `struct.set` |
| array | `array.new`, `array.new_default`, `array.new_fixed`, `array.new_data`, `array.new_elem`, `array.get`, `array.get_s`, `array.get_u`, `array.set`, `array.len`, `array.fill`, `array.copy`, `array.init_data`, `array.init_elem` |
| i31 | `ref.i31`, `i31.get_s`, `i31.get_u` |
| casts | `ref.test`, `ref.cast` (each with the non-null `(ref ht)` and nullable `(ref null ht)` operand forms), `br_on_cast`, `br_on_cast_fail` |
| equality | `ref.eq` (only on `eq`-hierarchy references) |
| null | `ref.null`, `ref.is_null`, `ref.as_non_null`, `br_on_null`, `br_on_non_null` |
| extern bridge | `any.convert_extern`, `extern.convert_any` |

`ref.func` and typed `call_ref` / `return_call_ref` belong to the typed
function-references proposal that GC builds on.

### 2.3 Normalisation / canonicalisation

The spec defines **expansion** (substitute a `typeidx` with its definition),
**rolling/unrolling** of `rec` groups, **closed types**, and the relations
above. It does **not** name an algorithm for any of it — see §3.

### 2.4 Runtime behaviour

- **Casts and null operations trap** on failure, not silently: `ref.cast`
  traps if the operand does not match; `i31.get_*` on a non-i31 traps;
  `struct.get` / `array.*` on null trap; out-of-bounds array access traps;
  and an `array.new` with an absurd (huge unsigned) length must fail rather
  than silently succeed.
- **`ref.eq`** is defined only for the `eq` hierarchy and compares the objects
  as the language abstracts them (so an unboxed `i31` compares by value).
- **`extern.convert_any` / `any.convert_extern`** are mutual inverses on the
  values on which they are defined, and let internal references leave and
  re-enter the internal hierarchy.
- **Element/data segments** feeding `array.new_data` / `array.new_elem` /
  `array.init_*` are evaluated following the segment rules; a passive segment
  is dropped explicitly, and a segment's references are **not** re-evaluated
  per use.
- **Allocation failure** is an *implementation limitation* (spec Appendix):
  the spec models it as "the implementation may fail", not as deterministic
  behaviour a script may rely on.

### 2.5 Constant expressions and typed tables

3.0 extends `const` with `ref.i31`, `struct.new`, `struct.new_default`,
`array.new`, `array.new_default`, `array.new_fixed`, `any.convert_extern` and
`extern.convert_any`, so GC aggregates can appear in global and element
initialisers. Typed tables may carry an initialiser expression, which is what
makes a non-nullable reference table expressible.

## 3. What Wasm 3.0 explicitly does *not* demand

The GC proposal is built around *representation independence* and *pay as you
go*. The following are **not** required, which is precisely the room we use in
§6.

### 3.1 No collector, no collection schedule

- **Nothing requires that unreachable objects ever be reclaimed.** A
  conforming engine may allocate forever and never collect; the only observable
  consequence is the spec's own *implementation limitation* escape hatch for
  exhausting memory. There is no observable finalization, no "collection
  happened" signal, and no bound on memory use.
- **No particular algorithm.** Mark/sweep, copying, reference counting,
  region/arena, or garbage-never are all conformant. The spec never mentions
  stack maps, write barriers, generations, or compaction.
- **No GC timing guarantees**, no latency bound, no incremental/parallel
  requirement, and no interaction rule with threads for the MVP.

### 3.2 No mandated representation

- **Heap object layout is free.** Fields, headers, vtables, maps, and tags are
  an implementation detail.
- **`i31` need not be a heap object.** Unboxing it (as a tagged machine word
  or a small immediate) is explicitly the point: "i31 is the type of unboxed
  scalars".
- **Runtime type information is optional/pay-as-you-go.** Only code that
  performs casts needs runtime type identity; aggregates never used in a cast
  need not carry any tag at all. (The Overview's explicit-RTT design was
  superseded by the MVP's implicit `ref.test`/`ref.cast`; either way the
  representation stays unconstrained.)
- **Reference identity is free.** Apart from the `ref.eq` rule on the `eq`
  hierarchy, nothing constrains how references are represented or compared.
  There is no requirement that `extern.convert_any` results be pool-allocated,
  interned, or stable across calls.

### 3.3 No mandated canonicalisation

- The spec defines type **equivalence** and **matching** as relations; it does
  **not** require a global type table, structural hashing, `rec`-group
  interning, or any specific `TypeId` scheme. A module could keep types
  module-local forever and compare structurally at every use. (Our interning
  and process-wide `TypeId`s are a design choice, discussed in
  [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md).)

### 3.4 Explicitly *out* of the 3.0 GC MVP

These appear in Post-MVP documents, not in the standard:

- finalizers, weak references, ephemerons;
- **type parameters**/parametric polymorphism, union types, "defined type"
  extensions to imported types;
- `i31` as a definable/importable type;
- the `stringref` / `stringview_*` family and the JS String Builtins
  (separate proposals, *not* part of 3.0 — see §10; the enum entries in
  `WasmType.hpp` are pre-emptive placeholders);
- non-`eq` immutable aggregates;
- thread-shared managed heap / atomics on aggregates.

### 3.5 Not demanded at the embedding boundary

- Traps are part of the language, but **how an embedder surfaces them**
  (exception, longjmp, process abort, host error code) is an embedding
  decision, not a spec constraint. The spec also does not require that
  allocation failure be catchable.
- Host/embedder representations for `extern` values are free; `externref` is
  an opaque, engine-chosen value.

## 4. What we have implemented today

### 4.1 Syntax / parsing (`WasmBase/WasmType.hpp`, `WasmModule.cpp`)

- `AbstractHeapType` (`func`, `extern`, `any`, `eq`, `i31`, `struct`, `array`,
  `noextern`, `nofunc`, `none`), `HeapType` (abstract **or** `typeidx`),
  `ValueTypeCode` including the shorthand reference codes and `Ref`/`RefNull`.
- `ValueType` (`opcode` + `heapType`), `StorageType` (packed `i8`/`i16` or a
  value type), `FieldType` (`isMutable`).
- `FuncType` / `StructType` / `ArrayType` behind
  `CompositeDefinition = std::variant<...>`, wrapped in `Subtype`
  (`isFinal`, `supertypeIndices`, `composite`).
- `rec` groups are parsed as `Module::typeGroups` (`TypeGroup{first,count}`),
  including forward references inside a group (see `forward_type_ref`,
  `rec_groups`, `rec_singleton` tests).
- `Module::typeId(LocalTypeIdx)` is the single place a module-local index
  becomes a process-wide `TypeId`.

### 4.2 Type identity, canonicalisation, matching

(`WasmBase/WasmCanonicalType.hpp`, `WasmBase/WasmTypeIdentity.hpp`,
`WasmBase/WasmTypeRegistry.{hpp,cpp}` — see [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md)
for the full design.)

- `TypeRegistry::registerModule` takes ownership of a module's `Subtype` block
  and hands back a stable `span`.
- `TypeRegistry::internModule` canonicalises each `rec` group into
  `CanonicalType` records, serialises the group to a key, interns it, and
  returns a `LocalTypeIdx -> TypeId` map. `CanonRecGroup` records the
  `[first, first+count)` run so a group's members stay contiguous.
- Declared-subtype validation runs before interning: a supertype must exist,
  must not be `final`, and the comptype must match structurally
  (`subtypes.wat`, `bad_subtype_*` tests).
- `matches` / `matchesHeap` implement the `match` relation on the canonical
  table: abstract lattice, nullability, struct width, mutable-field
  invariance, array covariance, function parameter contravariance / result
  covariance.
- `LibJitTypeTranslator` keeps a `canonicalCache` keyed by `TypeId`, so two
  modules that canonicalise to the same type share one lowered
  `jit_type_t` (`LibJit/LibJitTypeTranslation.cpp`).

### 4.3 Heap objects and runtime type tags (`WasmBase/WasmModuleInstance.{hpp,cpp}`)

- `ModuleInstanceInternals::gcObjectTypes` —
  `std::unordered_map<const void*, TypeId>` — is the only runtime type store.
- `allocateStructObject(size, localTypeIdx)`:
  `calloc`, writes the canonical `TypeId` into the 4-byte header
  (`offsetof 0`), and registers the pointer in `gcObjectTypes`.
- `allocateArrayObject(headerSize, elementSize, length, localTypeIdx)`:
  a `calloc`'d header `[u32 TypeId][u32 length][void* data]` plus a separate
  `calloc`'d element buffer.
- `tryGetGcTypeId(ref)` consults the map; `refMatchesHeapType(ref, ht, nullable)`
  implements a *runtime* `match`:
  - `null` matches only nullable targets;
  - tagged `i31` immediates (low bit set) match `any`/`eq`/`i31`;
  - function references are recognised by pointer-scanning
    `importStorage` / `internalCallables` and matched via `Callable::typeId`;
  - GC objects are matched via `gcObjectTypes` + `TypeRegistry::matchesHeap`;
  - unknown references match only `any`/`extern`/`nofunc`.
- Runtime layout helpers (`storageBytes`, `storageAlign`, `structFieldOffsets`,
  `structSize`, `arraySize`, `storeValue`) mirror the JIT lowering and are
  explicitly marked "keep in sync with translateStruct / translateArray".
- `evalConstantExpr` is a small stack machine covering `i32/i64/f32/f64.const`,
  `global.get`, `ref.null`, `ref.func` (sentinel form), and the GC `const`
  extensions: `ref.i31`, `struct.new`, `struct.new_default`, `array.new`,
  `array.new_default`, `array.new_fixed`, `any.convert_extern`,
  `extern.convert_any`.
- Element segments feed GC expressions through `bufferInitFromElems` and
  `applyActiveSegments`.

### 4.4 JIT lowering (`LibJit/LibJitTypeTranslation.{hpp,cpp}`)

- `struct` lowers to `[u32 header][field...]` using LibJIT's struct layout;
  packed `i8`/`i16` fields lower to `jit_type_sbyte` / `jit_type_short`.
- `array` lowers to `[u32 header][u32 length][element* data]`.
- Reference types lower to `void*`, except `i31`, which uses a *tagged*
  LibJIT type (`jit_type_create_tagged(void_ptr, TYPE_TAG_WASM, "wasm.i31")`).
- A reference with a **concrete heap type** lowers to a pointer to that
  aggregate's lowered type (`jit_type_create_pointer(...)`), regardless of
  nullability.

### 4.5 Opcode emission (`LibJit/LibjitOpcodeDispatcher.{hpp,cpp}`)

The decode layer is `WASM::OpcodeDispatcher` (`WasmBase/WasmOpcodeDispatcher.cpp`),
which reads the immediates and calls pure-virtual `dispatch*` methods.
`LibJIT::OpcodeDispatcher` and `Stub::OpcodeDispatcher` implement them.

Implemented GC dispatch: `struct.new`, `struct.new_default`, `struct.get`,
`struct.get_s`, `struct.get_u`, `struct.set`; `array.new`,
`array.new_default`, `array.new_fixed`, `array.new_data`, `array.new_elem`,
`array.get`, `array.get_s`, `array.get_u`, `array.set`, `array.len`,
`array.fill`, `array.copy`, `array.init_data`, `array.init_elem`;
`ref.test`/`ref.cast` (± `null`), `br_on_cast`, `br_on_cast_fail`,
`ref.i31`, `i31.get_s`, `i31.get_u`, `ref.eq`, `ref.as_non_null`,
`br_on_null`, `br_on_non_null`, `any.convert_extern`, `extern.convert_any`.

Notable emission details:

- `emitRefTypeTest` lowers every `ref.test`/`ref.cast`/`br_on_cast` type check
  to a call to the native helper `wasm_ref_matches_heap_type_impl`, i.e. the
  test always goes through `refMatchesHeapType`.
- `ref.cast` traps by branching to `emitAbort` (`std::abort()`), and
  `struct.get` / `array.*` use `jit_insn_check_null`, which faults on null.
- `i31` helpers (`wasm_i31_new_impl`, `wasm_i31_get_s/u_impl`) implement the
  tagged-word representation; `ref.i31` masks to 31 bits by construction.
- `any.convert_extern` / `extern.convert_any` are representation-preserving
  no-ops (the value is pushed straight back).
- `dispatchBrOnCast` / `dispatchBrOnCastFail` read the `castop` flags byte but
  ignore it, and test only against `ht2`.
- `dispatchReturnCallRef` is `dispatchCallRef` + `dispatchReturn` (correct
  results, no tail-call optimisation).

### 4.6 Test coverage

- Manual tests for type identity / subtyping live in `Test/main.cpp`
  (same-module vs cross-module `TypeId`s, forward references, `bad_subtype_*`).
- Spec scripts are wired via `Test/SpecSuite.cpp` + `Test/WastScript.cpp`.
  The curated (`wast_supported.txt`) GC-adjacent set today is `array_new_data`,
  `ref`, `ref_eq`, `ref_null`, `type-canon`. `type-subtyping.wast` is excluded
  because `wasm-tools` cannot parse it (`Test/Test.pro`).
- The harness only marshals **scalar** arguments and results
  (`isScalarValue` in `Test/WastScript.cpp`); reference/`v128` values are
  **skipped**. This matters when reading §8: many GC assertions are "passed"
  only because they were skipped, and some "failures" are caused by a skipped
  reference argument rather than a runtime bug.

### 4.7 Reference types (the reference-types proposal)

Reference types is folded into 2.0/3.0 core and is the substrate GC builds on;
this subsection records the parts of it that are *not* already covered above.

- **Value types.** `funcref` / `externref` (`ValueTypeCode::FuncRef` /
  `ExternRef`), the bottom shorthands `nullfuncref` / `nullexternref` /
  `nullref`, and the generic `(ref null? ht)` form. The abstract forms lower
  to `void*`; a `(ref null? ht)` with a concrete heap type lowers to a pointer
  to the aggregate (§4.4).
- **Reference instructions.** `ref.null`, `ref.is_null`, `ref.func`,
  `ref.eq`, `ref.as_non_null`, `br_on_null`, `br_on_non_null`
  (`LibjitOpcodeDispatcher.cpp`), plus the typed-function-reference
  `call_ref` / `return_call_ref` discussed in §4.5.
- **Tables.** Store-owned (`StoreOwnedTable` / `TableInstance`,
  `WasmTable.hpp`), with a `Callable**` slot array and `TableInstance::base`
  as field 0 for JIT addressing. Multiple tables, table imports/exports and
  table sharing between instances are implemented (`docs/ABI.md`).
- **Table instructions.** `table.get`, `table.set`, `table.size`,
  `table.grow`, `table.fill`, `table.copy`, `table.init`, `elem.drop`, and
  `call_indirect` with an explicit table index. Indices are bounds-checked
  (`checkedTableIndex`) and null slots trap.
- **Element segments.** Active / passive / declarative modes, in both the
  MVP index form (`initIndices`) and the expression form (`initExprs`, with
  `ref.func` / `ref.null` / GC constructors). Active segments are applied at
  instantiation (`applyActiveSegments`); passive ones feed `table.init`,
  `array.new_elem` and `array.init_elem`.
- **`call_indirect` / `call_ref` type checking.** `emitCallableTypeCheck`
  compares `Callable::typeId` with the canonical `TypeId` of the
  instruction's type index, with a registry-subtyping slow path
  (`wasm_callable_type_matches`), so cross-module and subtype-compatible
  callees work (`docs/TYPE_IDENTITY.md`).
- **`externref`** is an opaque `void*` supplied by the embedder; there is no
  host object model behind `Value::ref`.

Not present: a host/embedder bridge for `externref` (no JS-side object or
table model), reference-typed **global imports** (imported globals are not
resolved at all — G6), and validation of `ref.eq` operand types or of table
element types on import.

## 5. Gaps between the implementation and §2

Severity is about *observable correctness for a conforming module*, not about
effort.

### 5.1 Observable wrong results (conformance gaps)

| # | Gap | Evidence | Severity |
|---|---|---|---|
| G1 | **Per-instance type tags.** `gcObjectTypes` lives on the allocating `ModuleInstance`; a cast/test executed by another instance cannot see it. The `TypeId` *is* written into the object header but never read back for matching. | `refMatchesHeapType` → `tryGetGcTypeId`; cross-module `ref.test`/`ref.cast` on aggregates. | High |
| G2 | **Cross-module function references.** Function refs are matched by scanning the *current* instance's `importStorage`/`internalCallables`. A `funcref` produced by module B is not found, and the "unknown reference" fallback matches only `any`/`extern`/`nofunc` — **not** `func`. `ref.test funcref` on it is wrong. | `refMatchesHeapType`; `call_ref`/`ref_func` spec scripts. | High |
| G3 | **`any.convert_extern` / `extern.convert_any` are identity.** The two hierarchies are merged: an `externref` that is really an `i31`/struct survives `any.convert_extern`, and an internal reference survives `extern.convert_any` and then still passes `ref.test i31`/`struct`/`array`. | `dispatchAnyConvertExtern` / `dispatchExternConvertAny`; `ref_test` extern cases. | High |
| G4 | **Element-segment initialisers are re-evaluated per use.** The spec evaluates a segment once; `array.new_elem` must copy the *same* stored references, so two arrays built from one segment are `ref.eq`. We rebuild fresh objects each time. | `bufferInitFromElems` calls `evalConstantExpr` per entry; `array_new_elem.wast:121` expects `1`, gets `0`. | High |
| G5 | **Table initialiser expressions are dropped.** `TableType` (`WasmType.hpp`) stores only `elementType` + `limits`; `initializeTable` zero-fills. `(table $t 3 3 (ref i31) (ref.i31 (global.get $g)))` silently becomes three `null`s. Also blocks non-nullable reference tables. | `i31.wast:136-138` fail. | Medium |
| G6 | **Imported globals are never resolved.** `ImportResolver::resolveGlobal` is declared but not called anywhere; `initializeGlobals` leaves import slots zero. Surfaces in GC `const` expressions that read an imported global. | `grep resolveGlobal` finds no caller; `i31.wast:148` fails. | High (pre-existing, not GC-specific) |
| G7 | **`br_on_cast` flags ignored + control-flow modelling.** `castop` (source/target nullability) is discarded, so `br_on_cast` with nullable variants is wrong; the branch-target slot model then asserts. | `dispatchBrOnCast` `(void)castop;`; `br_on_cast.wast` hits `!slots.empty()`, `br_on_cast_fail.wast` hits `!valueStack.empty()`. | High |
| G8 | **`ref.func` as a constant expression is a sentinel** (`Value.ref = funcIdx`), only resolved when a segment is applied. A `ref.func` global initialiser stays a bogus pointer. | `evalConstantExpr` case `0xD2`; `initializeGlobals`. | Medium |
| G9 | **No validation pass.** `assert_invalid` / `assert_malformed` are skipped by the harness; GC type rules (cast target vs source, defaultability of `struct.new_default`, nullability constraints, `ref.eq` operand types) are unchecked and may crash rather than reject. | `docs/TESTING.md` execution table. | Medium |

### 5.2 Robustness / crash gaps

| # | Gap | Evidence |
|---|---|---|
| C1 | `array.init_data` and `array.init_elem` abort (signal 6). | `spec: array_init_data`, `spec: array_init_elem` |
| C2 | `ref_cast.wast` and `ref_func.wast` "produced no report (exit code 1)" — an uncaught C++ exception during load/instantiate/run. | SpecSuite child report |
| C3 | `call_ref.wast` segfaults. | `spec: call_ref` SIGSEGV |
| C4 | `return_call_ref.wast` trips a `std::span` bounds assertion. | trace in child |
| C5 | `type-equivalence.wast` aborts (signal 6). | `spec: type-equivalence` |
| C6 | **Traps are process-fatal.** `ref.cast` failure → `emitAbort` (`std::abort`); null deref → `jit_insn_check_null` (SIGSEGV); allocation failure → `std::abort`. The harness skips `assert_trap`, so the whole trap family is untested and unusable in-process. | `emitAbort`, `jit_insn_check_null`, `allocateStructObject`/`allocateArrayObject` |
| C7 | `array.new` with a negative `i32` length becomes a huge `uint32_t`; `calloc` failure aborts rather than trapping. | `dispatchArrayNew` / `allocateArrayObject` |

### 5.3 Hygiene / latent risks (not yet wrong, easy to get wrong)

| # | Risk |
|---|---|
| H1 | **Dual layout logic.** `structFieldOffsets`/`structSize`/`arraySize` in `WasmModuleInstance.cpp` must stay byte-for-byte in sync with `translateStruct`/`translateArray` in `LibJitTypeTranslation.cpp`. Packed `i8`/`i16` fields and `v128` (lowered as four `i32` with 4-byte alignment) are the risky spots. Nothing enforces the invariant. |
| H2 | **`gcObjectTypes` is never pruned**, so it grows without bound alongside the leak. |
| H3 | **`refMatchesHeapType` is O(#functions)** for function references (two linear scans) and is called for every cast/test. |
| H4 | **`i31` uses a tagged LibJIT type** while `i31.get_s/u` take `void_ptr`; correctness depends on `castRefValue`/`jit_insn_convert` being a bit-preserving reinterpretation. Worth a focused test. |
| H5 | **`stringref`/`stringview_*` enum + `void*` lowering exist but are not in 3.0** and have no parser/dispatch; the reserved encodings are stale (see §10.3). Keep explicitly unsupported. |
| H6 | **`ref.eq` is raw pointer equality with no type check**; unboxed `i31` compares by value only because the representation is canonical. `ref.eq` on non-`eq` operands is not rejected. |
| H7 | **`Value::kind` is set ad hoc** when a GC constant is produced (`I31Ref`, `StructRef`, `ArrayRef`), but the JIT ignores it; two paths must agree that the union payload is represented the same way. |

### 5.4 Harness limitations that can look like implementation gaps

- Reference **arguments** and **results** are skipped (`isScalarValue`), so
  `assert_return` on `externref`/`anyref` is not actually checked, and a
  skipped `(invoke "init" (ref.extern 0))` leaves the `ref_test` tables empty —
  which is why a large part of `ref_test.wast` "fails" even though the
  relevant opcodes work.
- `compareResult`'s failure message prints only the *actual type*, never the
  actual value (`"... but got " << typeName(resultCode)`), so a real
  wrong-value failure reads like `expected i32 42 but got i32`.
- `assert_trap`, `assert_invalid`, `assert_malformed`, `assert_unlinkable`,
  `module_definition`/`module_instance` and text-format modules are all
  skipped, which removes exactly the cases that would exercise GC traps and
  GC validation.

### 5.5 Reference-types-specific gaps

Some of these are distinct from the GC gaps above; others are the same root
cause viewed through the reference-types feature.

| # | Gap | Evidence |
|---|---|---|
| R1 | **Table imports are not type-checked.** `RegistryImportResolver::ModuleRegistry::resolveTable` discards the `TableType` (`(void)type;`), so element type and limits are never matched. Memory imports *are* checked (`memorySatisfiesImport`). Reference types requires the match. | `WasmRegistryImportResolver.cpp:226` |
| R2 | **`elem.wast` / `table_init.wast` fail outright.** `table_init` aborts (signal 6) before the first command passes; `elem` exits with no report. Both scripts lean on passive `funcref` expression-form segments, so the segment evaluator is the prime suspect. | `spec: elem`, `spec: table_init` |
| R3 | **Dropped-segment semantics.** `bufferInitFromElems` / `tableInit` abort whenever the segment was dropped, even for a zero-length access; the spec treats a dropped segment as empty (only a non-zero length traps). | `bufferInitFromElems`, `tableInit`, `elemDrop` |
| R4 | **`externref` has no host model.** An `externref` is just a `void*`; there is no way to create or inspect a host extern object, and `ref.eq` on externrefs is unvalidated pointer equality. | `Value::ref`, `refMatchesHeapType` |
| R5 | **Reference arguments/results are never marshalled**, so most table/reference tests execute with skipped actions (§5.4, §8.1) — a "failure" there frequently is not a runtime bug. | `isScalarValue` in `Test/WastScript.cpp` |
| R6 | **`ref.func` outside an element segment is an unresolved sentinel** (the reference-types face of G8), so a `ref.func` global stays a bogus pointer. | `evalConstantExpr` case `0xD2` |
| R7 | **Table-op traps abort** (`std::abort()` in `tableInit`/`tableCopy`/`tableFill` and the bounds checks), so `assert_trap` cannot run and in-process traps are fatal (same root as C6). | `dispatchTable*` |
| R8 | **Table initialiser expressions are unsupported** (shared with G5), and an element segment's declared value type is never validated against the table's element type. | `TableType`, `applyActiveSegments` |

## 6. Where we can and should play fast and loose

The project's stated philosophy ([`ABI.md`](ABI.md)) is a QVM successor, not a
browser engine: **flexibility and deliberate deviation are accepted when they
simplify the implementation**. GC is the area where the spec (§3) grants the
most freedom, so it is where we should spend that freedom.

### 6.1 The organising idea: hunks, not a collector

QVM never free-ran a collector; it reserved a **hunk** (`Hunk_Alloc`) and threw
it away when its owner (usually a map/module) went away. Wasm allows exactly
this: nothing in §3 requires reclamation.

**Recommendation:** replace `calloc`-per-object with a per-`ModuleInstance`
**bump/region allocator**, and free whole regions on instance destruction.
- No write barriers, no stack maps, no root scanning, no finalizers.
- O(1) allocation (pointer bump), which is what the Overview's "reliably
  cheap, ideally constant time" asks for.
- The existing `Store` (which outlives instances) already frames the lifetime
  rule; a region slots next to `StoreOwnedMemory`/`StoreOwnedTable`.

Guardrail: this requires the same discipline `ABI.md` already demands — a
reference must not outlive the instance that allocated it. That is an embedder
contract, documented, not enforced by the engine.

### 6.2 Deviations worth making (and why they are safe)

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| D1 | **Region allocation, no collector** (§6.1). | §3.1: collection is never required. | O(1) alloc, no barriers, no root set, huge simplification. |
| D2 | **Type identity only in a 4-byte header; delete `gcObjectTypes`.** Read `*(uint32_t*)ref`; compare via the canonical table. | The `TypeId` is already stamped at offset 0; §3.2 lets us choose the representation. | Fixes cross-module casts (G1), removes a hash-map lookup per cast, removes a permanently-growing map (H2). |
| D3 | **Cache subtype `depth` in the tag / canonical table** so `ref.test`/`ref.cast` against an abstract type becomes an integer range/bitset check, not a `TypeRegistry` call. | `CanonicalType` already stores `depth`; cast semantics are ours to implement (§3.2). | Inline-able casts on the hot path. |
| D4 | **Keep `i31` unboxed**, low-bit tagged; require heap allocations to be ≥8-byte aligned so the tag is unambiguous. | §3.2 explicitly allows unboxed scalars; already implemented. | No allocation for i31, trivial `ref.eq`. |
| D5 | **`any.convert_extern` / `extern.convert_any` as identity**, documented as "no host/reference mixing". | §3.2/§3.5: extern representations are free; the inverse property is trivially satisfied by identity. | Removes a wrapper from the hot path (already the behaviour — make it intentional). |
| D6 | **No validating front-end.** Trust the producer (AssemblyScript / `wasm-tools`), as QVM trusted its compiler. | §3.3: validation is required for *conforming modules*; we can choose to require valid input. | No validation engine to build, and no GC validation rules to encode. |
| D7 | **Single `trap()` sink** (abort today, exceptions/longjmp later) instead of per-instruction trap tables. | §3.5: trap surfacing is an embedding choice. | One place to change; keeps JIT clean. |
| D8 | **Materialise element/data segments once at instantiation** into the region. | §2.4 requires exactly "evaluated once"; this *closes* G4 while staying simple. | Correct `array.new_elem` identity, cheaper repeated use. |
| D9 | **Evaluate table initialisers once at instantiation** into the table's slot array. | §2.5; *closes* G5 and is needed for typed tables. | Non-nullable reference tables start working. |
| D10 | **Do not implement** finalizers, weak refs, type parameters, unions, strings, thread-shared heaps. | §3.4: all Post-MVP / out of 3.0. | Scope control. |
| D11 | **Elide null checks** only when the static type is non-nullable *and* we trust validation. | Sound if the module is valid; unsafe with D6. | Fewer branches on `struct.get`/`array.get`. |
| D12 | **Keep type interning**, but stop paying for it where it does not matter (e.g. function types never used in casts/imports) once the dedup ratio is measured. | §3.3: canonicalisation is optional. | Less work at link time without losing cross-module `match`. |

### 6.3 What we must *not* play loose with (observable semantics)

| Invariant | Why |
|---|---|
| `ref.eq` on the `eq` hierarchy (including i31 by value) | Directly observable; relies on canonical tagged representation. |
| Null handling and nullability matching | Cheap and observable; `ref.is_null`, `br_on_null`, `ref.as_non_null`. |
| `struct.get_s` / `get_u` sign/zero extension of packed fields | Observable values; must match the JIT load type. |
| Array `len` and element stride consistency with the JIT lowering | Memory-safety invariant; a mismatch is silent corruption (H1). |
| Process-wide `TypeId`s for cross-module `match` | Otherwise linking/casts across modules regress (`TYPE_IDENTITY.md`). |
| Deterministic trap behaviour | Abort is acceptable, silent garbage is not. |
| `extern`/`any` inverse property *if* a host passes externrefs | D5 is only safe under a no-mixing contract; it must be explicit, or box in a debug build. |

## 7. Plan

Ordered so that the *conformance* gaps close before the *optimisation* work.
`GC-n` is independent of `TYPE_IDENTITY.md`'s `M-n`.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **GC-1 — Header-based tags** | `refMatchesHeapType` reads the `u32` at `ref[0]` for aggregates instead of `gcObjectTypes`; give function references a discoverable canonical `typeId` that needs no per-instance scan; remove the map. | G1, G2, H2, H3 | Cross-module `ref.test`/`ref.cast` on structs/arrays and `ref.test funcref` on an imported funcref are correct; `ref_test`/`ref_cast` stop failing for the right reasons. |
| **GC-2 — Region allocator** | Per-instance bump allocator; free regions on instance destruction; keep a debug "poison on free" mode. | D1, C7 (partly) | No per-object `malloc`; allocation is a pointer bump; instance teardown reclaims everything. |
| **GC-3 — Instantiation-time materialisation** | Evaluate element/data segments once (D8); store and evaluate table init exprs (D9); resolve imported globals (G6). | G4, G5, G6 | `array_new_elem` identity holds; typed/non-nullable tables initialise; GC `const` exprs reading imported globals work. |
| **GC-4 — `br_on_cast` correctness** | Honour the `castop` nullability flags; fix the branch-target slot model so `br_on_cast(_fail)` never trips `!slots.empty()` / `!valueStack.empty()`. | G7 | Both scripts run to completion; nullable variants yield spec results. |
| **GC-5 — Crash triage** | `array.init_data`/`init_elem`, `ref_cast`, `ref_func`, `call_ref`, `return_call_ref`, `type-equivalence`. | C1–C5 | No child aborts/segfaults; each script passes or fails with a real mismatch message. |
| **GC-6 — Trap sink** | Route every trap through one `trap()` primitive; make it a longjmp/exception hook so `assert_trap` can eventually run. | C6, D7 | A failed cast / null deref is recoverable in-process; no `std::abort` on the normal path. |
| **GC-7 — Fast casts (optional)** | Depth/range-tag encoding (D3), inline caches for repeated `ref.test`/`ref.cast` at a site, measured against the registry-call baseline. | H3 | Casts on a hot loop are a few instructions; benchmark recorded. |
| **GC-8 — Extern interop (optional)** | Only if an embedder needs host externrefs: box `extern.convert_any` values, keep identity under the no-mixing contract (D5). | G3 | `ref_test` extern cases pass; the contract is documented in `ABI.md`. |

Guardrails for every milestone:

- Do not regress the curated `wast_supported.txt` set; run
  `./WasmJit '~[spec]'` and `./WasmJit` after each change.
- Keep the layout invariant (H1): if you touch one of
  `structFieldOffsets`/`structSize`/`arraySize` **or** `translateStruct`/
  `translateArray`, change the other in the same commit and add a test that
  compares offsets.
- Keep `ref.eq`/null/packed-extension semantics covered by at least one test
  each (§6.3).

## 8. Indicative test survey

Survey taken with the existing `build/Desktop-Debug/Test/WasmJit` binary
(2026-10-04; the working tree may be a few minutes newer than that binary).
`WASM_SPEC_TIMEOUT=10`, one script at a time. **Read together with §5.4**:
"skipped" is not "checked", and reference-argument cases are skipped, not
executed.

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `struct` | 12 | 0 | 18 | happy path works; most ref-valued assertions skipped |
| `array` | 23 | 0 | 31 | ditto |
| `array_copy` | 24 | 0 | 11 | works |
| `array_fill` | 23 | 0 | 7 | works |
| `array_new_data` | 9 | 0 | 19 | works |
| `array_new_elem` | 5 | 1 | 18 | G4 (segment re-evaluation) |
| `array_init_data` | — | crash | — | C1 (signal 6) |
| `array_init_elem` | — | crash | — | C1 (signal 6) |
| `i31` | 62 | 4 | 7 | G5 (table init), G6 (imported global) |
| `ref_eq` | 83 | 0 | 6 | works |
| `ref_is_null` | 14 | 2 | 6 | table/segment init (G5/G4) |
| `ref_null` | 2 | 0 | 32 | mostly skipped |
| `ref_as_non_null` | 4 | 0 | 3 | works |
| `ref_test` | 38 | 32 | 1 | mostly §5.4 (skipped `init` leaves tables null), plus G1/G2/G3 |
| `ref_cast` | — | no report | — | C2 |
| `ref_func` | — | no report | — | C2 |
| `call_ref` | — | SIGSEGV | — | C3 |
| `return_call_ref` | — | abort | — | C4 |
| `br_on_cast` | — | abort | — | G7 |
| `br_on_cast_fail` | — | abort | — | G7 |
| `extern` | 1 | 0 | 17 | mostly skipped |
| `type-canon` | 2 | 0 | 0 | type identity works |
| `type-equivalence` | — | abort | — | C5 |

Reproduce a single row with, from `build/Desktop-Debug/Test`:

```bash
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: <name>' -r compact
```

### 8.1 Reference-types / table survey

Same binary and caveats as §8. The `table_*` scripts are the reference-types
coverage that is not GC-specific.

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `ref` | 1 | 0 | 12 | mostly skipped |
| `ref_eq` | 83 | 0 | 6 | works |
| `ref_null` | 2 | 0 | 32 | mostly skipped |
| `ref_is_null` | 14 | 2 | 6 | table/segment init |
| `ref_as_non_null` | 4 | 0 | 3 | works |
| `ref_func` | — | no report | — | C2 |
| `extern` | 1 | 0 | 17 | mostly skipped |
| `elem` | — | no report | — | R2 |
| `table` | 18 | 0 | 28 | works |
| `table_get` | 2 | 1 | 13 | §8.1 note (harness) |
| `table_set` | 3 | 0 | 23 | works |
| `table_size` | 37 | 0 | 2 | works |
| `table_grow` | 29 | 2 | 27 | §8.1 note (harness) |
| `table_fill` | 1 | 0 | 44 | mostly skipped |
| `table_copy` | 522 | 0 | 1206 | works |
| `table_init` | — | abort | — | R2 |
| `table-sub` | 1 | 0 | 2 | works |
| `func_ptrs` | 23 | 0 | 13 | works |
| `call_indirect` | 113 | 0 | 59 | works |

**Harness note for `table_get` / `table_grow`.** Their two/one failures come
from actions the harness silently skips: `table_get.wast:23` is
`(invoke "init" (ref.extern 1))` and `table_grow.wast:21,29` pass
`(ref.extern …)`, i.e. reference *arguments*, which `isScalarValue` rejects
(`onAction` → `skip`). The setup therefore never runs and a later
`assert_return` observes the untouched table. These are §5.4 artifacts, not
runtime bugs.

## 9. References

Specification (WebAssembly 3.0, living draft):

- Appendix — Change History (Release 3.0 feature list):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Types:
  https://webassembly.github.io/spec/core/syntax/types.html
- Instructions (reference, aggregate, `*_cast`):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- Validation — Matching / Conventions / Modules:
  https://webassembly.github.io/spec/core/valid/matching.html
  https://webassembly.github.io/spec/core/valid/conventions.html
  https://webassembly.github.io/spec/core/valid/modules.html
- Appendices — Implementation Limitations / Type Soundness:
  https://webassembly.github.io/spec/core/appendix/index.html

GC proposal design documents:

- `proposals/gc/Overview.md` — requirements, representation independence,
  pay-as-you-go, efficiency considerations:
  https://github.com/WebAssembly/gc/blob/main/proposals/gc/Overview.md
- `proposals/gc/MVP.md` — the design 3.0 actually standardised (heap-type
  hierarchies, instructions, formal rules for type equivalence/subtyping):
  https://github.com/WebAssembly/gc/blob/main/proposals/gc/MVP.md
- `proposals/gc/Post-MVP.md` — finalizers, weak refs, type parameters, unions,
  strings (all deliberately out of scope here).

Related proposals covered in §10 (neither is part of 3.0):

- Stringref (reference-typed strings):
  https://github.com/WebAssembly/stringref/blob/main/proposals/stringref/Overview.md
- JS String Builtins (JS-API `wasm:` builtin namespace):
  https://github.com/WebAssembly/js-string-builtins/blob/main/proposals/js-string-builtins/Overview.md

In-repo:

- [`docs/TYPE_IDENTITY.md`](TYPE_IDENTITY.md) — type identity, interning,
  `match`, and the `M1`–`M8` plan.
- [`docs/ABI.md`](ABI.md) — VMContext passing, Store ownership, and the
  "spiritual successor to QVM" design philosophy.
- [`docs/TESTING.md`](TESTING.md) — running/filtering the two test families.
- [`docs/THREADS.md`](THREADS.md) — companion note on the threads/atomics
  proposal (shared memory, atomics, wait/notify).
- [`docs/SIMD.md`](SIMD.md) — companion note on fixed-width SIMD (2.0) and
  Relaxed SIMD (3.0).
- Phase 5 companion notes: [`BULK_MEMORY.md`](BULK_MEMORY.md),
  [`MULTI_VALUE.md`](MULTI_VALUE.md), [`EXTENDED_CONST.md`](EXTENDED_CONST.md),
  [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md),
  [`MULTI_MEMORY.md`](MULTI_MEMORY.md), [`MEMORY64.md`](MEMORY64.md),
  [`TAILCALLS.md`](TAILCALLS.md), [`EXCEPTIONS.md`](EXCEPTIONS.md),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md),
  [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md).
- Source anchors: `WasmBase/WasmType.hpp`, `WasmBase/WasmTypeRegistry.{hpp,cpp}`,
  `WasmBase/WasmCanonicalType.hpp`, `WasmBase/WasmModuleInstance.{hpp,cpp}`,
  `WasmBase/WasmOpcode.hpp`, `WasmBase/WasmOpcodeDispatcher.cpp`,
  `LibJit/LibJitTypeTranslation.{hpp,cpp}`, `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}`,
  `WasmStub/StubOpcodeDispatcher.cpp`, `Test/WastScript.cpp`,
  `Test/wast_supported.txt`.

## 10. Beyond 3.0 (very last priority): Stringref and JS String Builtins

Neither of these is part of WebAssembly 3.0, and neither is on the critical
path for this project. They are recorded here so that the reserved names in
`WasmType.hpp` are not mistaken for implemented features, and so that a future
decision has the facts in one place. **Treat this section as the last item on
any roadmap.**

### 10.1 Status

- **Stringref** ("Reference-Typed Strings", `WebAssembly/stringref`) is a
  *separate core proposal* layered on reference types / GC. It adds
  reference-typed strings, string views, and a string table section.
- **JS String Builtins** (`WebAssembly/js-string-builtins`) is a **JS-API**
  proposal, not a core-language change: it defines importable Wasm builtin
  functions bound by a JS host under the reserved `wasm:` import namespace,
  enabled behind a compile-time flag.
- The two are designed to be used together (the JS String Builtins document
  explicitly hopes to type its parameters with the core `stringref` type
  eventually), but either can be used alone.

### 10.2 What the Stringref proposal defines

New reference types, defined as **opaque** values (like `externref` /
`funcref`), plus a `string` heap type:

| Type | Encoding (current proposal) | Meaning |
|---|---|---|
| `stringref` | `0x64` (`-0x1c`) | nullable reference to a string |
| `stringview_wtf8` | `0x63` (`-0x1d`) | WTF-8 view (byte position) |
| `stringview_wtf16` | `0x62` (`-0x1e`) | WTF-16 view (code-unit position) |
| `stringview_iter` | `0x61` (`-0x1f`) | codepoint iterator |

A string is a sequence of **unicode scalar values and isolated surrogates**
(i.e. the WTF-16 codepoint domain). The proposal does **not** mandate an
internal encoding: WTF-8 and WTF-16 are both allowed, and a view only has to
be *able* to expose the other encoding (eager copy or breadcrumbs are both
acceptable).

Instruction families (all `0xFB`-prefixed, separate from the GC sub-opcodes):

- **creating** `string.new_utf8`, `string.new_lossy_utf8`, `string.new_wtf8`,
  `string.new_wtf16`, `string.const` (constant, usable in global
  initialisers; literals live in a new string table section immediately
  before the globals section);
- **measuring** `string.measure_utf8`, `string.measure_wtf8`,
  `string.measure_wtf16`, `string.is_usv_sequence`;
- **encoding to linear memory** `string.encode_utf8`,
  `string.encode_lossy_utf8`, `string.encode_wtf8`, `string.encode_wtf16`;
- **general** `string.concat`, `string.eq`;
- **views** `string.as_wtf8` / `string.as_wtf16` / `string.as_iter`,
  `stringview_wtf8.advance` / `.slice` / `.encode_*`,
  `stringview_wtf16.length` / `.get_codeunit` / `.slice` / `.encode`,
  `stringview_iter.next` / `.advance` / `.rewind` / `.slice`;
- **GC-integrated** (only when both proposals are present)
  `string.new_*_array`, `string.encode_*_array`.

Semantics worth noting:

- a **null `stringref` traps on every instruction except `string.eq`**;
- `string.new_*` and `string.const` may fail under memory pressure
  (implementation limitation); oversize inputs must trap;
- `stringview_wtf8` / `_wtf16` positions are byte / code-unit offsets advanced
  explicitly; the views exist precisely because the engine's internal encoding
  may differ from what the guest wants, and the conversion strategy is left to
  the implementation.

### 10.3 What is (and is not) in the tree

Only **placeholders**:

- `ValueTypeCode::StringRef` / `StringViewWtf8` / `StringViewWtf16` /
  `StringViewIter` exist (`WasmType.hpp`), are named in the stub
  pretty-printer, and are lowered to `void*` by `LibJitTypeTranslation.cpp`.
- Nothing else: no parser support for the string table section, no
  `string.const` in `evalConstantExpr`, no `0xFB` sub-opcode handling for any
  `string.*` / `stringview_*` instruction, no runtime string objects, and no
  `wasm:js-string` import resolution.

**The reserved encodings are stale and must be fixed before any real use.**
`WasmType.hpp` uses `StringRef = -0x19 (0x67)`,
`StringViewWtf8 = -0x1A`, `StringViewWtf16 = -0x1B`,
`StringViewIter = -0x1E`, while the current proposal encodes `stringref` as
`0x64` (`-0x1c`) and the views as `0x63` / `0x62` / `0x61`. Worse,
`-0x19 (0x67)` is `AbstractHeapType::None` in this codebase, so a `0x67`
byte read in a heap-type position means `none` — a `stringref` spelled that
way would be silently decoded as the bottom type. Fix the constants (and add
a `string` arm to `AbstractHeapType`) before wiring anything up.

### 10.4 What the JS String Builtins proposal defines

A JS host exposes builtin functions under the reserved `wasm:` namespace,
enabled by a compile-time flag in the JS-API. The MVP set:

`wasm:js-string` — strings are `externref`; `(ref extern)` is non-null:

| Builtin | Signature |
|---|---|
| `cast` | `(externref) -> (ref extern)` |
| `test` | `(externref) -> i32` |
| `fromCharCodeArray` | `((ref null (array (mut i16))), i32, i32) -> (ref extern)` |
| `intoCharCodeArray` | `(externref, (ref null (array (mut i16))), i32) -> i32` |
| `fromCharCode` | `(i32) -> (ref extern)` |
| `fromCodePoint` | `(i32) -> (ref extern)` |
| `charCodeAt` | `(externref, i32) -> i32` |
| `codePointAt` | `(externref, i32) -> i32` |
| `length` | `(externref) -> i32` |
| `concat` | `(externref, externref) -> (ref extern)` |
| `substring` | `(externref, i32, i32) -> (ref extern)` |
| `equals` | `(externref, externref) -> i32` |
| `compare` | `(externref, externref) -> i32` |

`wasm:text-encoder` / `wasm:text-decoder` (available when the host implements
the Encoding API) add UTF-8 conversions:

| Builtin | Signature |
|---|---|
| `wasm:text-encoder` `measureStringAsUTF8` | `(externref) -> i32` |
| `wasm:text-encoder` `encodeStringIntoUTF8Array` | `(externref, (ref null (array (mut i8))), i32) -> i32` |
| `wasm:text-encoder` `encodeStringToUTF8Array` | `(externref) -> (ref (array (mut i8)))` |
| `wasm:text-decoder` `decodeStringFromUTF8Array` | `((ref null (array (mut i8))), i32, i32) -> (ref extern)` |

Notes:

- the array-based builtins pin the array's element type (mutable `i16` /
  `i8`), so they only work once GC arrays exist (they do, §4);
- `cast` / `test` are the "is this `externref` a JS string" pair;
- `equals` deliberately accepts `null` on either side; every other builtin
  traps on `null` or a non-string;
- the proposal's stated motivation is that an imported glue function's
  overhead is "prohibitive" for primitives like `String`, so a runtime is
  expected to **recognise and inline** the builtin call sites — the opposite
  of a generic import.

### 10.5 How it would map onto this engine (QVM spirit)

- **No GC dependency is required.** Both proposals treat strings and views as
  opaque reference values. Here that is exactly the existing `void*`
  representation, so a string can be a **hunk**: one allocation with a small
  header (encoding tag + length + optional breadcrumbs) in the same region
  allocator proposed in §6.1. Strings then live and die with their instance,
  like every other GC object in this engine.
- **`externref` strings belong to the host**, not us. In a QVM-style embedder
  those are pointers into embedder-owned state (`VMContext::hostData`), which
  is already the `externref` contract (§4.7). A non-JS host simply resolves
  `wasm:js-string` imports to its own implementations, or rejects them.
- **`wasm:js-string` is a resolver problem, not a codegen problem** — unless
  we want the inlining the proposal is designed around. A first cut registers
  the builtins through `ImportResolver::resolveFunction` with
  `Callable::typeId = TypeId::kNone` and `context = nullptr`; the JIT already
  handles native imports (`docs/ABI.md`). Fast paths can come later.
- **`string.const`** needs the new string table section parsed plus one more
  arm in `evalConstantExpr`. Views need explicit `advance` state, i.e. a small
  mutable object — again a region/hunk allocation.

### 10.6 If we ever pick this up (ordered, optional)

| Milestone | Work | Done when |
|---|---|---|
| **SR-1** | Fix the reserved `ValueTypeCode` constants to the current encodings and add a `string` heap type; add a decode test. | A hand-built module with `stringref` decodes and round-trips its type section. |
| **SR-2** | Parse the string table section and `string.const`; add `string.const` / `string.new_*` to `evalConstantExpr`. | A `(global stringref (string.const 0))` works. |
| **SR-3** | Minimal runtime: a WTF-8 hunk string object, `string.new_wtf8` / `string.new_wtf16`, `string.measure_*`, `string.eq`, `string.concat`. | String-focused scripts (if/when they exist) pass for the WTF paths. |
| **SR-4** | Views: `as_wtf8` / `as_wtf16` / `as_iter`, `advance`, `slice`, `encode`. | View round-trips match the proposal's worked examples. |
| **SR-5** | `wasm:js-string` (and optionally `text-encoder` / `text-decoder`) via the import resolver. | A module importing `wasm:js-string.length` runs against a host-provided builtin. |

Until then, keep the enum entries documented as **placeholders only**, and do
not add a `string` arm to `AbstractHeapType` without fixing the `-0x19`
(`none`) collision first.
