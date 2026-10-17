# Garbage Collection (Wasm 3.0) — Implementation Notes, Spec Conformance, and Deliberate Deviations

Status: design note / implementation reference (revision 11 — GC's own
conformance surface is **complete**: the last runtime gap, `br_on_cast` /
`br_on_cast_fail` (`G7`/`GC-4`), was a *decoder* bug — a typed-reference block
type (`0x64 ht`) left its heap-type immediate in the byte stream, so the rest of
the function was decoded from the wrong offset — plus missing label-value
modelling, now fixed for all four `br_on_*` instructions; table imports are
type-checked (`R1`). What remains is deferred by design (`G3`/`R4` need the
embedder's host-reference model, `G9`/`R8` need a validating front end).
Revision 10's collector is
**complete**: reference counting (`RC-1`/`RC-1b`), trap-safe refcount ops with an
iterative safepoint walk (`RC-3`) and Bacon–Rajan cycle collection (`RC-2`) all
landed at the `gcCollect()` safepoint. The typed-`assert_trap` check then turned up
three conformance bugs, now fixed: `array.new_data`/`array.new_elem` validate the
source range *before* allocating (they used to report "out of memory" where the
spec wants an out-of-bounds trap), `array.init_data`'s source offset counts bytes
rather than elements, and `i31.get_*` null-checks its reference.
Revision 8 turned on operand-stack
ownership: `kStackOwnsRefs` makes the operand stack own a count for every
pushed reference, so references held only transiently are reclaimed too. It was
blocked by a module that pushed a null `jit_value_t`; such a module is now rejected
at load ([`NULL_JIT_VALUE_DIAGNOSIS.md`](../NULL_JIT_VALUE_DIAGNOSIS.md)).
Revision 7 made objects *unowned* at birth, reference-typed slots (heap slots,
locals, globals, segment copies) retain, and `gcCollect()` recycles queued
zero-count objects at a safepoint. Revision 6 made arrays inline; revision 5 moved
the GC heap into a Store-owned mmap arena; revisions 3–4 chose reference counting
and added the refcount header and write barriers.)
Audience: anyone touching `WasmType`, `WasmTypeRegistry`, `WasmModuleInstance`,
`LibJitTypeTranslator`, `LibjitOpcodeDispatcher`, or the test suite.

This note is deliberately split the same way as
[`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md):

1. what the WebAssembly 3.0 specification **demands** of a GC-capable engine;
2. what it explicitly does **not** demand (and where an engine is free);
3. what `WasmJit` **currently implements**;
4. the **gaps** between (3) and (1);
5. where we can and should play **fast and loose** on purpose, because this
   project is a spiritual successor to Quake 3's QVM rather than a
   browser-embedded engine (see [`ABI.md`](../ABI.md) "Design Philosophy") —
   including the collector-strategy decision (§6.1) and the reference-counting
   challenge/mitigation study (§6.4).

It contains **no new code**. Shipped code and this document are the two
artefacts; the plan in §7 says which gaps are worth closing and which are not.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does Wasm 3.0 require for GC? |
| §3 | What does Wasm 3.0 *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short of §2? |
| §6 | Where do we *choose* to deviate, and why? (the reference-counting collector, §6.1/§6.4) |
| §7 | Milestones to close the important gaps |
| §8 | Indicative test survey |
| §9 | References |
| §10 | Beyond 3.0 (very last priority): Stringref + JS String Builtins |

Terminology follows the spec: a **heap type** classifies a reference; a
**reference type** is `(ref null? ht)`; a **defined type** (`deftype`) is a
`sub`-annotated composite (`func` / `struct` / `array`); a **`rec` group** is a
mutually recursive block of defined types. "`match`" is the spec's subtyping
relation, written `C ⊢ t ≤ t'`.

One distinction matters throughout this note and is easy to get wrong: a
**composite type** (`struct` / `array`) describes a *heap object* and is **not**
a value type. Only a **reference** to such an object (`(ref null? $t)`) is a
value. See §2.6.

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

### 2.6 References, structs and arrays: aggregates are heap objects, not values

This is the part of the GC extension most often misunderstood, so it is worth
stating plainly: **a `struct` or `array` type is not a value type.** It
describes the layout of a heap object. The only thing a Wasm program can put on
the operand stack, pass as a parameter, return, or keep in a local/global is a
**reference** to such an object. There is no by-value C-style struct anywhere
in Wasm 3.0 — not as a parameter, not as a result, not as a local.

**References are opaque handles.**

- A reference type is `(ref null? ht)`: a nullable or non-nullable handle to a
  value classified by the heap type `ht`. Reference types **are** value types,
  so they live on the stack just like an `i32`.
- A reference has **no arithmetic, no address-of, no dereference, and no
  pointer comparison**. The complete operation set is `ref.null`,
  `ref.is_null`, `ref.as_non_null`, `br_on_null` / `br_on_non_null`,
  `ref.func`, `ref.eq` (only on the `eq` hierarchy), `ref.test` / `ref.cast` /
  `br_on_cast*`, the extern bridge (`any.convert_extern` /
  `extern.convert_any`), `call_ref`, and the aggregate accessors below.
- A reference is the **identity** of the object: two references that compare
  `ref.eq`-equal are the same object, and a mutation through one is visible
  through the other. Aliasing is the norm, not an exception.
- The spec says nothing about what a reference *is* — a raw pointer, an index
  into a handle table, or a tagged word are all conformant (§3.2). That
  freedom is exactly what this engine uses.

**Structs.**

- `struct.new $t` / `struct.new_default $t` **allocate a fresh object** on the
  heap with the shape of `$t`, and push a non-null `(ref $t)`. `struct.new`
  consumes the field values (in declaration order); `new_default` zero
  initialises.
- Fields are reachable **only through the object**: `struct.get[_s|_u]` takes
  the `(ref null $t)` operand and returns the field; `struct.set` takes the
  reference and the new value. A **null** reference traps.
- `$t` itself (the `struct ft*` definition, with a `sub`/`final` annotation) is
  never a value; it is the object's layout and the thing an instruction's
  `typeidx` is validated against. Mutability (`mut? ft`) is a per-field
  property, not a property of the object.

**Arrays.**

- `array.new $t`, `array.new_default $t`, `array.new_fixed $t n`,
  `array.new_data $t d` and `array.new_elem $t e` **allocate a fresh array**
  whose **length is fixed at creation**, and push a non-null `(ref $t)`.
- The element type comes from the array's definition (`array ft`) and may be a
  packed storage type (`i8`/`i16`).
- All access is through the reference: `array.len`, `array.get[_s|_u]`,
  `array.set`, `array.fill`, `array.copy`, `array.init_data`,
  `array.init_elem`. An out-of-bounds index traps, as does a **null**
  reference. There is no `array.grow` and no addressable storage — the elements
  are not part of linear memory and cannot be reached with `i32.load`.

**`i31` is the one unboxed internal value.** `i31ref` is an unboxed scalar (no
heap object required), which is why `ref.eq` compares it by value. Everything
else in the internal hierarchy is a reference to a heap object.

**Why this matters for an implementation.** The aggregate definition is a
*type-system and layout* artefact; the runtime value is a reference. Lowering
a `struct` to a by-value aggregate on the operand stack (or passing one as a C
struct) conflates the two, and then aliasing, object identity, and
`struct.set` visibility all break. §4.4 shows how this engine keeps them
separate.

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
- **No requirement that cycles be reclaimed.** An engine that reference-counts
  and leaks cycles, or that never collects at all, is conformant; only the
  implementation-limitation escape hatch applies. That is what makes a
  cycle-leaking RC design *spec-legal*. It is not automatically *acceptable* for
  a long-running host, which is why §6.4 treats cycles as a day-one problem
  rather than an edge case.
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
  [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md).)

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
`WasmBase/WasmTypeRegistry.{hpp,cpp}` — see [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)
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

- GC objects live in the **Store-owned `GcHeap`** (`WasmBase/WasmGcHeap.{hpp,cpp}`):
  mmap-backed regions segregated by 16-byte size class, each with a free list;
  an instance's regions are released in bulk by `~ModuleInstance`. Because the
  backing is `mmap` and not the libc allocator, **no `malloc`/`calloc`/`free`
  happens while generated code runs** (§6.1/GC-2, `docs/SAFETY.md` §2.3).
- `allocateStructObject(size, localTypeIdx)`: allocates from the heap, writes the
  canonical `TypeId` and a reference count of 1 into the 8-byte header
  (`[u32 TypeId][u32 refcount]`, `WasmGcHeader.hpp`).
- `allocateArrayObject(dataOffset, elementSize, length, localTypeIdx)`: **one**
  heap block — `[u32 TypeId][u32 refcount][u32 length]` followed by the inline
  elements at `dataOffset` (§4.8, D13/GC-9).
- **Type tags come from the header, not a side map.** `GcHeap::owns(ref)` decides
  "is this a GC pointer" from the region ranges, and `tryGetGcTypeId` then reads
  the `TypeId` at the header — the old per-object `gcObjectTypes` map is gone
  (D2/GC-1), so matching works across instances and allocates nothing per object.
- **Reference counting & ownership (RC-5/RC-1b).** `ModuleInstance::refClone/`
  `refDrop`/`refStore` maintain the header count through `std::atomic_ref`
  (saturating clone, trapping underflow). A fresh object is **unowned** (count
  0); the first *owning slot* retains it — heap slots (`struct.set`/`struct.new`,
  `array.set`/`fill`/`new*`, `global.set`, `table.set`), reference-typed
  **locals** (`local.set`/`tee`, via `wasm_ref_clone`/`drop`), global
  initialisers, and the segment/bulk-copy paths (`bufferInitFromElems`,
  `wasm_buffer_copy_impl`). Parameters and control-flow slots are **borrows**
  (uncounted). Reclamation is deferred to an explicit **safepoint**:
  `reclaimGcObject` queues a zero-count object and `gcCollect()` recycles queued
  objects still at zero — safe because no Wasm frame (and so no uncounted
  stack/local reference) is live then, and the count is re-checked so a
  resurrected object is not freed.
- `refMatchesHeapType(ref, ht, nullable)` implements a *runtime* `match`:
  - `null` matches only nullable targets;
  - tagged `i31` immediates (low bit set) match `any`/`eq`/`i31`;
  - function references are recognised by pointer-scanning
    `importStorage` / `internalCallables` and matched via `Callable::typeId`;
  - GC objects are matched via `GcHeap::owns` + the header `TypeId` +
    `TypeRegistry::matchesHeap`;
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

There are **two distinct lowerings** here, matching the two-level model of
§2.6: the *layout of the heap object*, and the *reference to it*.

**Aggregate layout (the pointee).**

- `struct` lowers to `[u32 TypeId header][field...]` using LibJIT's struct
  layout; packed `i8`/`i16` fields lower to `jit_type_sbyte` / `jit_type_short`.
- `array` lowers to `[u32 TypeId header][u32 length][element* data]`.
- `translateStruct` / `translateArray` return this **bare struct type**. It is
  a *layout descriptor*, never a runtime value: it is consumed by
  `jit_type_get_size(...)` (the allocation size handed to
  `allocateStructObject` / `array.new*`) and by `jit_type_get_offset(...)` in
  `structFieldOffset` / `arrayLengthOffset` / `arrayDataOffset`.
- `translateTypes` PASS 1 caches that bare struct in `canonicalCache` (keyed by
  `TypeId`), and `jitTypeForTypeIdx(idx)` hands out the bare struct.

**References (the value).**

- Abstract reference types lower to `void*`, except `i31`, which uses a
  *tagged* LibJIT type (`jit_type_create_tagged(void_ptr, TYPE_TAG_WASM, "wasm.i31")`).
- A reference with a **concrete heap type** lowers to a **pointer to** that
  aggregate's layout: `translateType()` wraps the cached bare struct with
  `jit_type_create_pointer(..., 1)`, regardless of nullability.
  `jitRefTypeForHeapType` / `structRefType` / `arrayRefType` all route through
  that same path, so this is the single place where aggregate → reference
  happens.
- Consequently the operand stack holds **pointers**, not aggregates:
  `struct.new` pushes the `void*` returned by `wasm_struct_new_default_impl`,
  and `struct.get` / `struct.set` do `refAsVoidPtr(ref)` followed by
  `jit_insn_load_relative(..., fieldOffset, ...)`.

**Do not double-wrap.** `translateStruct` must keep returning the bare struct.
Returning `jit_type_create_pointer(struct, 1)` from it would make `translateType`
produce `pointer(pointer(struct))`, and would make `jit_type_get_size` report
the size of a pointer and `jit_type_get_offset` meaningless — the classic
symptom of confusing an aggregate type with a reference to it.

Two implementation footnotes:

- The third argument of `jit_type_create_struct` is LibJIT's **`incref`**, not
  `is_union` (`/usr/local/include/jit/jit-type.h`), so `... , 1)` is fine.
- The `std::span<const WASM::StorageType>` overload of `translateStruct` is
  currently **uncalled** (dead code). If it is ever wired up, it follows the
  same rule: return the bare struct, and let the reference site wrap it.


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

A block's type may itself be a typed reference (`(block (result (ref $t)))`,
encoded `0x64 ht` / `0x63 ht`). `BlockType` carries that heap-type immediate and
`storageTypesForBlockType` passes it to the JIT type translation; without it the
immediate stayed in the byte stream and the rest of the function decoded from the
wrong offset (the root cause of the `br_on_cast` abort, §5.1 `G7`).

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

### 4.8 Array layout: inline (implemented, D13/GC-9)

Wasm mandates **nothing** about how an array is laid out in memory (§3.2:
layout is free). The engine used to lay an array out as a "C struct containing a
pointer" — a header plus a separate element buffer — which cost two allocations
and a double indirection per element access. It is now **inline**: header and
elements in one block (docs/proposals/GC.md D13/GC-9).

**Before (until GC-9).** `translateArray` lowered an array to a four-field
struct `[u32 TypeId][u32 refcount][u32 length][element* data]`, and
`ModuleInstance::allocateArrayObject` performed **two** allocations:

```
      array object (calloc #1)               element buffer (calloc #2, len*stride)
    +---------------------------+          +-----------------------------+
ref | +0 : u32 TypeId           |          | elem[0] | elem[1] | ...    |
 -> | +4 : u32 refcount         | data --> |                             |
    | +8 : u32 length           |          +-----------------------------+
    | +16: element* data        |
    +---------------------------+
```

Every element access was a **double indirection**: `array.get $t` did
`load_relative(ref, +16, element*)` and then `load_elem(dataPtr, index)`
(`dispatchArrayGet`/`GetS`/`GetU`/`Set`/`Fill`/`Copy`/`Init*` all repeated this),
and `array.new*` loaded the same field before its fill loop.

**Now: inline.** One allocation holds the header and the elements:

```
   single allocation (dataOffset + len*stride)
  +-------------------------------------------------------------+
  | +0 : u32 TypeId | +4 : u32 refcount | +8 : u32 length | pad |
  +-------------------------------------------------------------+
  | elem[0] | elem[1] | ...                                      |
  +-------------------------------------------------------------+
                                          ^ element base = ref + dataOffset
```

Element access is a **single** indirection:
`load_elem(ref + dataOffset, index)` (or `load_elem_address` for the packed
`get_u`/`get_s` path). `dataOffset` is a per-array-type constant,
`align_up(12, alignof(element))`, computed by `WASM::gcArrayDataOffset` and
shared by the JIT and the runtime (H1) — not a stored field.

| Dimension | Out-of-line (before) | Inline (now) |
|---|---|---|
| Allocations per array | 2 | **1** |
| Fixed overhead per array | 8 B header + 8 B `data` field (+ padding) + two allocator headers | 12 B header (padded to `dataOffset`) + one allocator header |
| Element access | 2 dependent loads (ref → data → elem) | **1 load (ref + offset → elem)** |
| Cache lines touched by one `array.get` | usually 2 (header, buffer) | 1 (unless the array spans a line) |
| Empty array (`len = 0`) | `data == nullptr`; the field must never be dereferenced | just the header; no null case |
| Locality / streaming | buffer can be anywhere | header and elements adjacent |
| Free / arena teardown | two blocks to reclaim | **one block** |
| Moving GC later | **not planned** — reference counting is non-moving (§6.1) | (same: no forwarding pointers) |
| `array.copy` self-overlap | `memmove` on the shared buffer | `memmove` within one object |
| LibJIT representation | fixed 4-field struct | header-only 3-field struct + computed address arithmetic |

Arrays are uniquely easy to inline because **their length is fixed at
creation** — there is no `array.grow`. Structs were already inline; arrays were
the last out-of-line case, and the two-block shape also forced `RC-1b` to
recycle two blocks per array — now one.

**How it is implemented.** LibJIT has no flexible-array-member type, so the
lowered type is the *header-only* struct and the element base is address
arithmetic:

- `translateArray` returns `{u32 TypeId, u32 refcount, u32 length}` (12 bytes)
  instead of a struct with a trailing pointer.
- `WASM::gcArrayDataOffset(storageType)` gives the per-type
  `dataOffset = align_up(12, storageAlign(elem))`, shared by the runtime and the
  JIT (H1).
- `OpcodeDispatcher::arrayDataOffset(arg)` returns that constant, and
  `arrayElementBase(ref, arg)` produces `ref + dataOffset` as an `element*`;
  `array.get`/`get_s`/`get_u`/`set`/`fill`/`copy`/`init_data`/`init_elem` and the
  `array.new*` fill loops all go through it.
- `allocateArrayObject(dataOffset, elemSize, length, typeIdx)` allocates one
  block of `dataOffset + length*elemSize` bytes, writes `TypeId`@0,
  `refcount`@4 and `length`@8, and leaves the elements zero-filled (the heap is
  mmap-backed).
- `array.copy` / `init_data` / `init_elem` still call
  `wasm_buffer_copy_impl` / `wasm_buffer_init_*_impl`, but the `void*` they
  receive is now `ref + dataOffset` rather than a loaded `data` field; a
  self-copy keeps `memmove` semantics.

**Guardrails / caveats.**

- Element **alignment** is the one thing that changes shape: `dataOffset` must
  respect the element's alignment (`i64`/`f64` → 8, `v128` → 4 as lowered
  today), so it is a per-type constant, not the fixed `8`.
- The runtime and JIT offset/stride constants must not drift (H1). Add a test
  that asserts the JIT's computed offsets equal the runtime helpers'.
- `array.copy` between arrays of **different element sizes** currently emits a
  trap; that is a validation concern and should stay rejected either way.
- This is an optimisation, not a conformance fix: do it only when arrays are
  on a hot path (measure first, §6.2/D12 spirit).

### 4.9 Traps and segment materialisation (shared with `BULK_MEMORY.md`)

Two pieces of shared machinery landed with the memory/bulk work, and they are
what made most of §8 move:

- **A trap sink.** `WasmBase/WasmTrap.hpp` implements `WASM::trap` as a
  `longjmp` to an embedder-armed `TrapScope`. The JIT's `emitAbort` (used by
  `ref.cast`, `unreachable`, bounds checks) and every `jit_insn_check_null`
  (now `emitTrapIfNull`) route through it, as do the native helpers. The harness
  executes `assert_trap`; a *missing* trap is now a real failure
  (`docs/TESTING.md`).
- **Segment materialisation.** Expression-form element segments are evaluated
  once at instantiation into `ModuleInstanceInternals::elementSegmentRefs`, and
  `table.init` / `array.new_elem` copy *those* references (G4). Imported globals
  are resolved before any local initialiser runs (G6; MUTABLE_GLOBALS.md MG-1).

## 5. Gaps between the implementation and §2

Severity is about *observable correctness for a conforming module*, not about
effort.

### 5.1 Observable wrong results (conformance gaps)

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~G1~~ | ~~Per-instance type tags.~~ **Done (GC-2a):** `GcHeap::owns(ref)` identifies a GC pointer from its region (a process-wide structure) and `tryGetGcTypeId` reads the `TypeId` back from the object header, so a cast/test from any instance sees it. Reference *arguments* are marshalled now, so cross-module verification runs (`ref_test` 69/2). | `GcHeap::owns`, `tryGetGcTypeId`; `ref_test` 69/2 | Closed |
| ~~G2~~ | ~~**Cross-module function references.**~~ **Done (`GC-1`):** every `Callable` carries its canonical `TypeId`, and `Store::callableAt` identifies it by a range test over the Store's registered callable ranges (one per instance) — no per-instance `importStorage`/`internalCallables` scan, and the fallback no longer decides the answer. | `refMatchesHeapType`, `Store::callableAt`; `ref_test` 69/2 | Closed |
| G3 | **`any.convert_extern` / `extern.convert_any` are identity.** The two hierarchies are merged: an `externref` that is really an `i31`/struct survives `any.convert_extern`, and an internal reference survives `extern.convert_any` and then still passes `ref.test i31`/`struct`/`array`. | `dispatchAnyConvertExtern` / `dispatchExternConvertAny`; `ref_test` extern cases. | High |
| ~~G4~~ | ~~Element-segment initialisers are re-evaluated per use.~~ **Done (GC-3):** expression-form segments are evaluated once at instantiation into `elementSegmentRefs`; `table.init` and `array.new_elem` copy those references, so two arrays built from one segment are `ref.eq`. | `array_new_elem` 14/0, `table_init` 725/0 | Closed |
| ~~G5~~ | ~~Table initialiser expressions are dropped.~~ **Done:** `TableType` carries the initialiser (`initExpr`), the decoder reads the spec's `0x40 0x00` form — note the marker byte, read as a *signed* LEB, is `-0x40` (`void`), which is exactly how it is detected — `initializeTable()` fills every slot with the value (each slot owning a reference, RC-5), and `resolveTableRefFuncs()` fills a `(ref.func …)` default *after* compilation, the same deferral `resolveGlobalRefFuncs()` uses because the callables do not exist during construction. Non-nullable reference tables now initialise. | `i31` `68/0` (was `63/5`); `table` `18/0` (it previously aborted on an unsized callable vector) | Closed |
| ~~G6~~ | ~~Imported globals are never resolved.~~ **Done (GC-3 / MUTABLE_GLOBALS MG-1):** `resolveImports` resolves every imported global before any local initialiser runs. | `global` 70/0; `i31` 65/3 (the remaining failures are G5) | Closed |
| ~~G7~~ | ~~**`br_on_cast` flags ignored + control-flow modelling.**~~ **Done (GC-4):** `castop` is honoured, and the abort was *not* in the opcode at all: `BlockType` could not represent a typed reference, so `(block (result (ref i31)))` — encoded `02 64 6C` — consumed `0x64` and left the heap-type byte `0x6C` (`i32.mul`) to be decoded as the next instruction. A function body with such a block was therefore decoded from the wrong offset, which is what surfaced as `popValue()` underflow in an opcode the source does not contain. `BlockType` now carries the heap-type immediate and `storageTypesForBlockType` passes it to the JIT. The operand-stack half is fixed too: all four `br_on_*` instructions now model their label's value list (`t*`) and stage the branch's reference over it (`storeStackTopToSlots`) instead of overwriting `slots.back()`, and a branch to the function's implicit label is emitted as a conditional return (like `dispatchBrIf`). `br_on_non_null`'s fall-through no longer leaves the consumed reference on the stack. | `BlockType` (`WasmType.hpp`), `dispatchBrOnCast`/`Fail`/`Null`/`NonNull`; `br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0, `br_on_non_null` 11/0 | Closed |
| ~~G8~~ | ~~**`ref.func` as a constant expression is a sentinel** (`Value.ref = funcIdx`), only resolved when a segment is applied.~~ **Done:** the sentinel is resolved after compilation — `resolveGlobalRefFuncs()` rewrites globals, `callableFromSegmentRef()` resolves segment copies, and `resolveTableRefFuncs()` (G5) fills a table default. | `table.wast` exercises an exported `(ref $f)` global across modules (`table` 18/0) | Closed |
| G9 | **No validation pass.** `assert_invalid` / `assert_malformed` are skipped by the harness; GC type rules (cast target vs source, defaultability of `struct.new_default`, nullability constraints, `ref.eq` operand types) are unchecked and may crash rather than reject. | `docs/TESTING.md` execution table. | Medium |

### 5.2 Robustness / crash gaps

| # | Gap | Evidence |
|---|---|---|
| ~~C1~~ | **Done:** `array_init_elem` → **33/0/3** and `array_init_data` → **44/0/2** — the dropped-segment/bounds aborts now trap, and the data-offset bug behind the last 3 mismatches is fixed (`src` is a *byte* offset, not an element index). | `spec: array_init_data`, `spec: array_init_elem` |
| ~~C2~~ | **Done:** `ref_cast` **33/11/1**, `ref_func` **14/0/3** — the fatal path was an invalid reference reaching `jit_insn_check_null`, which is now the trap sink. | SpecSuite child report |
| ~~C3~~ | **Done:** `call_ref` **31/0/4** (was SIGSEGV) — the null-callable check now traps instead of dereferencing. | `spec: call_ref` |
| ~~C4~~ | **Done:** `return_call_ref` **40/0/13** (tail-call trampoline + trap sink). | `spec: return_call_ref` |
| ~~C5~~ | **Done:** `type-equivalence` **30/1/1** (was an abort); one real mismatch remains. | `spec: type-equivalence` |
| ~~C6~~ | **Done:** `emitTrap`/`emitTrapIfNull` and every native helper go through `WASM::trap` (longjmp to the embedder's `TrapScope`) carrying a typed reason, the harness executes `assert_trap` *and* checks the kind, and a budget refusal is a typed `OutOfMemory` trap rather than an abort. What still aborts, deliberately: `WASM::trap` with no armed scope, and a host failure the budget could not pre-empt (`docs/SAFETY.md` §2.4). | `WasmTrap.hpp`, `emitTrap`, `allocateStructObject` |
| C7 | `array.new` with a negative `i32` length becomes a huge `uint32_t`; the GC heap refuses a region past the Store budget and the caller traps (previously a `calloc` failure aborted). | `dispatchArrayNew` / `allocateArrayObject` / `GcHeap::allocate` |

### 5.3 Hygiene / latent risks (not yet wrong, easy to get wrong)

| # | Risk |
|---|---|
| H1 | **Dual layout logic.** `structFieldOffsets`/`structSize`/`arraySize` in `WasmModuleInstance.cpp` must stay byte-for-byte in sync with `translateStruct`/`translateArray` in `LibJitTypeTranslation.cpp`. Packed `i8`/`i16` fields and `v128` (lowered as four `i32` with 4-byte alignment) are the risky spots. Nothing enforces the invariant. |
| ~~H2~~ | ~~`gcObjectTypes` is never pruned.~~ **Done (GC-2a):** the map is gone; the type tag is read from the object header and `GcHeap::owns` uses region ranges. |
| H3 | **`refMatchesHeapType` is O(#functions)** for function references (two linear scans) and is called for every cast/test. |
| H4 | **`i31` uses a tagged LibJIT type** while `i31.get_s/u` take `void_ptr`; correctness depends on `castRefValue`/`jit_insn_convert` being a bit-preserving reinterpretation. Worth a focused test. |
| H5 | **`stringref`/`stringview_*` enum + `void*` lowering exist but are not in 3.0** and have no parser/dispatch; the reserved encodings are stale (see §10.3). Keep explicitly unsupported. |
| H6 | **`ref.eq` is raw pointer equality with no type check**; unboxed `i31` compares by value only because the representation is canonical. `ref.eq` on non-`eq` operands is not rejected. |
| H7 | **`Value::kind` is set ad hoc** when a GC constant is produced (`I31Ref`, `StructRef`, `ArrayRef`), but the JIT ignores it; two paths must agree that the union payload is represented the same way. |
| ~~H8~~ | ~~Arrays cost two allocations and two indirections.~~ **Done (GC-9 / D13):** arrays are inline — one allocation, one indirection per access. |

### 5.4 Harness limitations that can look like implementation gaps

- Reference **arguments and results** are marshalled now. Arguments use `referenceArgument`; results are compared as null (exact), as a concrete identity through the same mapping (`(ref.extern N)`, `(ref.i31 K)`), or as a bare placeholder (`(ref.func)` must be a registered `Callable*`; `(ref.array)` and the other bare forms must be non-null). `either` result lists (relaxed SIMD) are verified against every alternative. `ref_test` therefore reads **69 passed / 2 failed / 0 skipped**, and both failures are `ref_test_extern` — the `any`/`extern` identity deviation `G3`/`D5`, not `G2`. `R5` is closed: `table_fill` went 23→**36/0/9**, `array_new_data` 24→**28/0/0**.
- `compareResult` reports the actual *value* as well as the type, so a
  wrong-value failure reads `expected i32 42 but got i32 0`.
- `assert_trap` **is** executed, in-process, and its expected text is compared
  against the trap's *kind* (`WASM::trapKindName`): trapping for the wrong
  reason is a failure (`docs/SAFETY.md` §2.4). `assert_invalid`,
  `assert_malformed`, `assert_unlinkable`, `module_definition`/
  `module_instance` and text-format modules are still skipped, which removes
  exactly the cases that would exercise GC validation (G9).

### 5.5 Reference-types-specific gaps

Some of these are distinct from the GC gaps above; others are the same root
cause viewed through the reference-types feature.

| # | Gap | Evidence |
|---|---|---|
| ~~R1~~ | ~~**Table imports are not type-checked.**~~ **Done:** the exporter publishes its `TableType` (and the canonical `TypeId` of a concrete element type) in `ImportedTable`, and `RegistryImportResolver::resolveTable` matches it against the importing module's declared type — element type by subtyping (`TypeRegistry::matchesHeap`, with shorthands normalised and nullability checked) and limits/index-type/shared-ness by the spec's limit rule. A table registered directly by an embedder carries no type and stays unchecked, so native imports are unaffected. | `WasmImport.hpp` (`ImportedTable`), `WasmRegistryImportResolver.cpp` (`tableSatisfiesImport`), `WasmModuleInstance.cpp` (`registerExports`, `resolveImports`) | Closed |
| ~~R2~~ | **Fixed.** `elem` → **108/0/46**, `table_init` → **725/0/67** (both previously aborted). The prime suspect was right: the segment evaluator, plus a null `call_indirect` slot left by an unresolved imported global. | `spec: elem`, `spec: table_init` |
| ~~R3~~ | **Fixed.** A dropped segment is treated as *empty*: `bufferInitFromData`/`bufferInitFromElems` return for a zero-length access and trap only for a non-zero one, and the `array.new_data`/`array.new_elem` range check reads the same dropped flag. | `bufferInitFromElems`, `checkDataSegmentRange`/`checkElementSegmentRange` |
| R4 | **`externref` has no host model.** An `externref` is just a `void*`; there is no way to create or inspect a host extern object, and `ref.eq` on externrefs is unvalidated pointer equality. | `Value::ref`, `refMatchesHeapType` |
| ~~R5~~ | ~~**Reference arguments/results are never marshalled**~~ **Done (harness):** arguments use `referenceArgument`; results are compared as null, as a concrete identity through the same mapping, or as a bare placeholder (`(ref.func)` → registered callable, `(ref.array)`/… → non-null); `either` lists are checked alternative by alternative. `table_fill` 36/0/9, `array_new_data` 28/0/0, `relaxed_min_max` 25/0/0. | `Test/WastScript.cpp` | Closed |
| ~~R6~~ | **Fixed (the same defect as G8).** `ref.func` is now resolved after compilation for globals, segment copies and table defaults alike. | `resolveGlobalRefFuncs`, `callableFromSegmentRef`, `resolveTableRefFuncs` |
| ~~R7~~ | **Fixed.** Table-op bounds checks and the null-slot check route through `WASM::trap`, so `assert_trap` runs and in-process traps are recoverable. | `dispatchTable*`, `emitTrapIfNull` |
| R8 | **An element segment's declared value type is never validated against the table's element type.** The *table-initialiser* half of this row is **fixed** (G5): `TableType::initExpr` is decoded from the spec's `0x40 0x00` form and applied to every slot. | `TableType`, `initializeTable`, `resolveTableRefFuncs` |

### 5.6 Collector / lifetime gaps (closed by RC-1…RC-3)

§3.1 lets an engine never reclaim, so an absent collector is a *choice*, not a
defect. Revision 3 nevertheless committed to reclaiming by reference counting, so
these three rows are what had to close for "objects are never freed" to stop
being a defect — and they have. What remains on the lifetime side is the
borrow-side leak documented with `RC-1b` in §7 (parameters and control-flow slots
hold *uncounted* borrows, so a reference only ever held there leaks until
teardown).

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~L1~~ | ~~**Nothing is recycled yet.** Objects are allocated from the GC heap, but `reclaimGcObject` only records the 0 transition; the region holds them until instance teardown.~~ **Done:** `reclaimGcObject` queues a zero-count object and `gcCollect()` recycles it at a safepoint, releasing the object's own references iteratively (RC-1b/RC-3); a graph kept alive only by its own cycles is reclaimed by trial deletion (RC-2). Asserted by the seven `GC reclamation:` tests against `GcHeap::liveObjectCount()`. | `WasmModuleInstance.cpp` (`gcCollect`, `collectCycles`), `WasmGcHeap.cpp` | Closed |
| ~~L2~~ | ~~**No ownership or refcount exists.** A `void*` reference carries no count and no lifetime; there is nowhere to hook reclamation.~~ **Done (RC-1):** every GC object carries `[u32 TypeId][u32 refcount]` (`WasmGcHeader.hpp`, D14) and is *unowned at birth*; `refClone`/`refDrop`/`refStore` move the count. Non-objects (`i31`, `externref`, `funcref`) are deliberately uncounted (RC12). | `WasmGcHeader.hpp`, `ModuleInstance::ref*` | Closed |
| ~~L3~~ | ~~**Reference slots are written as raw pointers.** `struct.set` / `array.set` / `table.set` / globals store `void*` with no retain/release barrier, and the JIT emits the store directly.~~ **Done (RC-1 / D15):** every reference-typed store goes through `wasm_ref_store` (retain the new value, release the overwritten one) — `struct.set`, `array.set`/`fill`/`copy`, `local.set`/`tee`, `global.set`, `table.set` and the segment copies. §7's guardrail forbids a bare store into a reference slot. | `emitRefStore`, `dispatch*Set` | Closed |

## 6. Where we can and should play fast and loose

The project's stated philosophy ([`ABI.md`](../ABI.md)) is a QVM successor, not a
browser engine: **flexibility and deliberate deviation are accepted when they
simplify the implementation**. GC is the area where the spec (§3) grants the
most freedom, so it is where we should spend that freedom.

### 6.1 The organising idea: reference counting, not hunks and not mark-sweep

QVM never free-ran a collector; it reserved a **hunk** (`Hunk_Alloc`) and threw
it away when its owner (usually a map/module) went away. Wasm allows almost the
same freedom: §3.1 does not require reclamation, and it does not require cycles
to be collected either.

**Decision (revision 3): the engine reclaims with reference counting.** Hunks
and arena allocation are *not* discarded — they remain the **backing allocator**
(a per-`ModuleInstance` bump region) — but they stop being the *reclamation
policy*. A stop-the-world, trace-based mark-sweep collector is **unlikely to be
implemented** here, for one decisive reason:

- **We have no stack maps and no cheap way to build them.** A tracing collector
  must find every root, and our JIT keeps Wasm values in *machine* registers,
  LibJIT temporaries and the C stack, with no liveness metadata (`ABI.md`: a
  `VMContext*` is arg 0, the operand stack is the machine stack). RC needs no
  root set: a reference *is* a root, and ownership is explicit at every store.
- RC is **incremental and pause-free**, which suits an engine embedded in a game
  loop; a stop-the-world collector introduces an unbounded pause at an arbitrary
  instruction.
- RC is **deterministic**: an object frees exactly when its last reference drops,
  which makes leaks and use-after-free reproducible under the test harness.
- RC composes with the Store/instance lifetime rule already in `ABI.md`: a
  reference must not outlive the instance that allocated it, and the region
  teardown stays as a coarse backstop.

**What it costs, and the one non-negotiable:** a naive RC **leaks cycles** — and
our aggregates (`struct.new`, `array.new*`, and `array.set` to a same-typed
element) make cycles trivial to build. A cycle leaking per call in a long-lived
host is a bug, not an implementation-limitation footnote. Therefore:

- **cycle handling is a day-one milestone, not a later optimisation.** `RC-2`
  (a root-free Bacon–Rajan synchronous cycle collector, §6.4/RC1) lands *with*
  `RC-1`; the collector is not "shipped" until cycles are reclaimed.
- every reference mutation goes through **one helper** (`wasm_ref_store`, §6.2/
  D15), so the release/retain barrier — and the cycle-collector hook — has a
  single place to live.
- `RC-3` makes the refcount operations **trap-safe** (no wrap, no double free,
  no unbounded recursion), because a counting bug must surface as a Wasm trap or
  a clean leak, never as silent corruption.

§6.4 is the full catalogue of RC traps and their mitigations.

### 6.2 Deviations worth making (and why they are safe)

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| D1 | **Reference counting with a region-backed allocator** (§6.1): a refcount in the object header, mmap-backed per-instance regions segregated by size class with per-region free lists, region teardown as the leak backstop. ◑ The backing allocator landed (`GC-2a`); recycle/RC-1b is next. | §3.1: any reclamation algorithm (and none) is conformant; §3.1 also does not require cycles to be collected. | Deterministic, pause-free reclamation with **no stack maps and no root scanning** — the two things this JIT cannot cheaply provide — and no libc allocator call in generated code. |
| D2 | ✅ **Type identity only in the header; `gcObjectTypes` deleted.** The tag is read from `ref[0]`, and `GcHeap::owns(ref)` (region ranges) is the GC-pointer test. | The `TypeId` is stamped at offset 0; §3.2 lets us choose the representation. | Done (GC-2a): fixes cross-instance casts (G1), removes a hash-map lookup per cast, removes a permanently-growing map (H2) and a per-object libc allocation. |
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
| D13 | ✅ **Arrays are inline** (header + elements in one allocation), replacing the `[header][length][data*]` form (done, GC-9). | §3.2: heap-object layout is ours to choose, and Wasm arrays never grow, so the size is known at creation. | Half the allocations (now one), one indirection per element access, better locality; see §4.8. |
| D14 | **Refcount in the object header**, `[u32 TypeId][i32 refcount]` at offsets 0/4; `i31` stays unboxed and uncounted; `externref` / `funcref` are not ours to count. | §3.2: heap-object layout is free, and unboxed scalars are allowed. | The count lives with the object: no side table, no per-object metadata lookup. |
| D15 | **Every reference mutation goes through one native helper** — `wasm_ref_store(void** slot, void* newRef)` (release old, retain new), `wasm_ref_clone(void* ref)`, `wasm_ref_drop(void* ref)` — alongside the existing `wasm_ref_matches_heap_type_impl` and `wasm_buffer_copy_impl` (which must *also* retain when it copies ref-typed elements). The JIT never emits a raw `void*` store into a ref slot. | §3.2: representation is free; §3.5: failures may be surfaced however the embedder likes. | One barrier point, one place to hook the cycle collector; a missed barrier becomes a visible omission instead of a hidden leak. |
| D16 | **Atomic refcount in the header** (`std::atomic<int32_t>`), even before threads exist; non-atomic operations only in a single-threaded build. | §3.4 keeps thread-shared managed heaps out of 3.0, but §3.1/§3.2 leave the representation to us; paying now avoids a second reference ABI later (`THREADS.md`). | Threads-ready counts for one header word and one atomic RMW on the slow path, with no ABI break when `THREADS.md` lands. |
| D17 | **Deferred reference counting (optional, measure first):** do not count references that never escape a frame (operand-stack temporaries, frame-local Wasm locals); recover them with a conservative scan or a shadow root stack at safepoints. | §3.1: the algorithm is free. Deferring is only sound if *something* can still find those references — a stack scan — so it is an optimisation, never a licence to skip barriers on escaping stores. | Removes most clone/drop traffic from scalar-heavy loops; see §6.4/RC10. |
| D18 | **Root-free cycle collection** (Bacon–Rajan synchronous trial deletion) rather than a tracing pass. | §3.1: cycles need not be collected, so any method is extra credit; trial deletion needs no root set, unlike a full trace. | Reclaims the one class of garbage RC misses, **without stack maps**. |
| D19 | **Trap-safe refcount arithmetic**: saturating `clone` on overflow (leak, never wrap), trap on `drop` below zero, poison-on-free in debug, and a bounded/iterative drop worklist. | §3.5: failure surfacing is an embedding choice. | A refcount bug becomes a recoverable trap or a deterministic leak — never silent corruption or a C-stack overflow. |

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
| **Aggregates are references, not by-value values** | A `struct`/`array` type is a heap-object *layout*; only `(ref ...)` is a value. Treating the aggregate type as a stack value (or passing it as a C struct) breaks object identity, aliasing and `struct.set` visibility (§2.6, §4.4). |
| **Every ref store is a `wasm_ref_store`** | A raw `void*` store skips releasing the old value and retaining the new: a leak or a use-after-free, with no other barrier to catch it (§6.4/RC4). |
| **Refcount header matches the JIT** | `[u32 TypeId][i32 rc]` offsets are read by both the runtime helpers and generated code; drift is silent corruption (extends H1). |
| **`clone` / `drop` balance survives traps** | The trap sink (`longjmp`) must not skip a release; helper and emitted code must not hold a borrowed ref across a trap point (§6.4/RC9). |
| **Cycles are reclaimed, not merely documented** | A cycle that leaks per call is a bug in a long-lived host; `RC-2` must actually collect it (D18). |

### 6.4 Reference counting: the traps and the mitigations

Reference counting trades a tracing collector's root-set problem for a set of
*ownership disciplines*: the collector no longer finds garbage for you, so every
place a reference is created, overwritten or destroyed is a place to get it
wrong. This is the catalogue — each classic RC failure mode, why it is
specifically dangerous in *this* engine, the mitigation, and the milestone that
owns it. (`RC1`…`RC13` here are study rows, not §5 gaps.)

| # | Challenge | Why it bites *here* | Mitigation | Milestone |
|---|---|---|---|---|
| RC1 | **Cycles leak.** A struct/array referring to itself, directly or through a chain, never reaches count 0. | Trivial to build from Wasm: `array.set` a field to an array that (transitively) points back. A per-call cycle is an unbounded leak in a long-running host. | **Root-free cycle collection from day one**: Bacon–Rajan synchronous trial deletion over the region's objects (D18); no stack maps needed. | `RC-2` |
| RC2 | **Refcount races.** Concurrent clone/drop on one object corrupts the count. | Even before `THREADS.md`, the harness can host nested calls; once shared memory and agents land, races are real. | Atomic count in the header with acquire/release ordering (D16), plus a race stress test. | `RC-4` |
| RC3 | **References not held by a slot are unowned** (operand-stack temporaries, registers, Wasm locals). | Values live on the machine stack with no metadata; a tracing collector would need stack maps, and RC must still decide who owns a transient. | Default: every pushed ref is *owned* and balanced by an explicit drop. Optional **deferred RC** skips frame-local refs and recovers them with a conservative/shadow-stack scan (D17). | `RC-1`, `RC-5` |
| RC4 | **Missing / duplicated write barrier.** A store that overwrites a ref slot without releasing the old value, or retaining the new one, leaks or frees live memory. | The JIT currently emits raw `void*` stores for `struct.set` / `array.set` / `table.set` / globals — there is no barrier to forget *yet* because there is none at all (L3). | One choke point: **every** ref store is `wasm_ref_store` (D15); no raw store in generated code. | `RC-1` |
| RC5 | **Ownership across the ABI.** A `void*` returned from a call, pushed to a host, or stored in a `Callable` — who retains it? | `ABI.md`'s value model is by-value `void*`; a caller and callee that disagree on ownership leak or double-free. | Document the borrow/own contract at every boundary; clone on hand-off if the callee keeps it; `wasm_ref_clone` / `wasm_ref_drop` at the embedder edge. | `RC-1` (+ `ABI.md`) |
| RC6 | **Long-lived slots** (globals, table slots, materialised element/data segments) and **teardown ordering**. | These outlive a single call; dropping them after their region is gone is a use-after-free, and forgetting them is a leak. | Slots are roots with a +1 count; segments retain on materialisation and release on overwrite/teardown; region teardown is the last-ditch release. | `RC-1`, `GC-3` |
| RC7 | **Count overflow / underflow / double drop.** | A 32-bit count can wrap under pathological sharing; a stray extra drop frees a live object. | Saturating `clone` (leak, never wrap), trap on `drop` below zero, poison-on-free in debug (D19). | `RC-3` |
| RC8 | **Recursive drop of a deep graph** frees depth-first and can overflow the C stack. | Generated frames already sit on the C stack; a long array→array chain recurses once per element. | Iterative drop worklist (a deferred-free list drained in bounded batches) (D19). | `RC-3` |
| RC9 | **Traps mid-mutation.** A `longjmp` trap can skip a release that a C++ destructor would have run. | `WASM::trap` is `longjmp` (GC-6); helper and emitted code must not hold a borrowed ref across a trap point, or the release is lost. | Keep helper bodies RAII-free/POD and drop before any trapping operation; assert in debug that no borrowed ref is live across a trap. | `RC-1`, `RC-3` |
| RC10 | **RC traffic on the hot path** (an atomic RMW per reference op). | Every `struct.set` / `array.get` of a reference becomes counted, unlike today's free raw load/store. | Non-atomic ops in a single-threaded build; **deferred RC** removes frame-local traffic; batch clone/drop pairs; measure before optimising (D12 spirit). | `RC-5` |
| RC11 | **Header layout / false sharing.** The count next to the `TypeId` is written on every clone/drop and shares the line with the `TypeId` read on every cast. | Layout is fixed by D14; under threads this is a contention point. | Accept it first; if measured, separate the hot fields or pad — the same "measure first" rule. | `RC-4` |
| RC12 | **Foreign references.** `externref` is host-owned; `funcref` is a `Callable*` owned by the Store; `i31` is an immediate. | Counting them would free memory we do not own, or count a non-object. | Never count `i31`, `externref` or `funcref`; their lifetime stays the embedder's / the Store's (D14). | `RC-1` |
| RC13 | **Cycles through the host.** A loop that closes through embedder state is invisible to the object graph. | RC (and trial deletion) only sees Wasm-heap edges; a host that retains a ref back into a cycle it also holds keeps it alive. | Document the embedder contract: the host owns that edge and must break it; the engine cannot collect what it cannot see. | `RC-1` (+ `ABI.md`) |

**Status of these rows.** They are owned by milestones, so their status follows:
`RC1`, `RC4`, `RC6`, `RC7`, `RC8`, `RC9`, `RC12`, `RC13` are satisfied
(`RC-1`/`RC-1b`/`RC-2`/`RC-3` landed, with `ABI.md` recording the embedder
borrow/own contract); `RC2` and `RC11` wait on `RC-4` (atomic counts), and
`RC10` waits on `RC-5` (deferred RC, optional and "measure first"). `RC3`'s
default — every pushed reference is *owned* and balanced by an explicit drop —
is implemented (`kStackOwnsRefs`), so only its optional variant is outstanding.
See §7.1 for the numeric-order roll-up.

**On a "backup tracing pass".** A full tracing pass needs a root set, which is
exactly what we cannot cheaply build (§6.1). Two trace-shaped fallbacks stay
honest: (a) at **region teardown** the whole region is dead by definition, so a
walk there reclaims anything the counts missed — but only when the instance
dies; (b) if `RC-2`'s trial deletion proves insufficient, a **conservative**
stack scan (Boehm-style) could feed a tracing pass, at the cost of false
retention and the usual conservatism hazards. Neither replaces `RC-2`; they are
belt-and-braces (`RC-6`).

## 7. Plan

Ordered so that the *conformance* gaps close before the *optimisation* work.
`GC-n` is independent of `TYPE_IDENTITY.md`'s `M-n`. `RC-n` is the
reference-counting programme of §6.1/§6.4 that turns the region allocator into a
real collector. **`RC-2` (cycles) lands together with `RC-1`, not after it** —
the collector is not considered shipped until cycles are reclaimed. The
**Closes** column may name a §5 gap (`L#`) or a §6.4 study row (`RC#`).

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **GC-1 — Header-based tags** | ✅ Done (`GC-2a` + `GC-1`): aggregates read the `u32` at `ref[0]` and `gcObjectTypes` is deleted (`GcHeap::owns` is the GC-pointer test); function references are identified through the Store's callable-range registry and carry their canonical `TypeId`, so no per-instance pointer scan remains (`H3`). | G1, G2, H2, H3 | Done: cross-instance `ref.test`/`ref.cast` on structs/arrays read the header, and a cross-module `ref.test funcref` needs no scan (`ref_test` 69/2 — the two failures are `G3`). |
| **GC-2 — Region allocator** | ◑ Landed (`GC-2a`): the Store-owned `GcHeap` — mmap-backed regions segregated by size class, per-region free lists, budget-charged, released by `~ModuleInstance`; `allocateStructObject`/`allocateArrayObject` no longer call `calloc` and type tags come from the header (`GcHeap::owns`). **Open:** none of the original two — `reclaimGcObject` → `GcHeap::recycle` is wired (`RC-1b`) and the debug poison-on-free mode landed (`GcHeap::setPoisonOnFree`, `RC-3`). `allocate()` now re-zeroes a block taken off a free list: mmap zero-fills only the *first* use, and `struct.new_default` writes no fields at all, so a recycled block must be cleared before it is handed out. | D1, D2, L1, C7 (partly) | Done for backing: no per-object libc allocation; allocation is a free-list pop or a bump; instance teardown reclaims everything even if counting misses. |
| **GC-3 — Instantiation-time materialisation** | ✅ Element/data segments are evaluated once (D8), imported globals are resolved (G6), and table initialiser expressions are decoded and applied (D9/G5): `TableType::initExpr` → `initializeTable()` for every default that is final at construction, `resolveTableRefFuncs()` for a `(ref.func …)` one after compilation. | G4, G5, G6 | Done: `array_new_elem` identity holds; GC `const` exprs reading imported globals work; typed/non-nullable tables initialise (`i31` `68/0`, `table` `18/0`). |
| **GC-4 — `br_on_cast` correctness** | ✅ Done: `castop` is honoured; the crash was a **decoder** gap (`BlockType` dropped the heap-type immediate of a `0x64 ht` block type, desynchronising the function body) plus the label-value model; all four `br_on_*` instructions now model `t*`, stage the branch's reference with `storeStackTopToSlots`, and treat a branch to the function label as a conditional return. | G7 | Done: `br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0, `br_on_non_null` 11/0. |
| **GC-5 — Crash triage** | ✅ `ref_cast`, `ref_func`, `call_ref`, `return_call_ref` and `type-equivalence` all run (the trap sink) and `array.init_elem` is clean. The ordering/offset bugs that the typed-`assert_trap` check then exposed are fixed too: the source range of `array.new_data`/`array.new_elem` is validated **before** the allocation (so an absurd length reports the out-of-bounds trap, not "out of memory"), and `array.init_data`'s source offset is a *byte* offset (scaling it by the element size broke every element wider than a byte). | C1–C5 | Done: no child aborts/segfaults; each script passes or fails with a real mismatch message. `array` is back to `40/0` and `array_init_data` to `44/0`. |
| **GC-6 — Trap sink** | ✅ `WASM::trap` (longjmp to a `TrapScope`, `WasmBase/WasmTrap.hpp`) is the single sink; `emitAbort` and every `jit_insn_check_null` use it, and the harness executes `assert_trap`. Allocation failure and `notImplemented` still abort. | C6, D7 | Done: a failed cast / null deref is recoverable in-process; no `std::abort` on the memory/table/GC-reference path. |
| **GC-7 — Fast casts (optional)** | Depth/range-tag encoding (D3), inline caches for repeated `ref.test`/`ref.cast` at a site, measured against the registry-call baseline. | H3 | Casts on a hot loop are a few instructions; benchmark recorded. |
| **GC-8 — Extern interop (optional)** | Only if an embedder needs host externrefs: box `extern.convert_any` values, keep identity under the no-mixing contract (D5). | G3 | `ref_test` extern cases pass; the contract is documented in `ABI.md`. |
| **GC-9 — Inline arrays** | ✅ Done: header-only JIT type, per-type `dataOffset` (`gcArrayDataOffset`), one allocation, and `arrayElementBase` for every element access. | D13, H8 | Done: `array.new_default` is one heap block (the `GC reference counting` test asserts `totalAllocations() + 1`), and all array scripts are unchanged from the §8 baseline. |
| **RC-1 — Reference-counted heap objects (core)** | ✅ Store barriers + slot ownership: `[u32 TypeId][u32 refcount]` header, **count 0 at birth**, `wasm_ref_store`/`_clone`/`_drop` (`WasmGcHeader.hpp`, `ModuleInstance::ref*`), and every ref-typed slot store retains — `struct.set`/`struct.new`, `array.set`/`fill`/`new*`/`copy`, `global.set`, `table.set`, `local.set`/`tee`, global initialisers, segment copies. | L2, L3, RC4, RC12, D14, D15 | Done: a created object is unowned until a slot retains it, and the JIT `struct.set`/`array.set`/`local` paths move the count. |
| **RC-1b — Reclaim at a safepoint** | ✅ `reclaimGcObject` queues a zero-count object; `gcCollect()` recycles queued objects that are still at zero (resurrection-checked). Call it where no Wasm frame is live and the embedder holds no un-counted references. | RC4, RC9, L1, D19 | Done: the `GC reference counting` test drops an object to zero, collects, and reuses the recycled block. **Leak-side, not UAF:** parameters and control-flow slots are borrows, so a reference that is only ever held in one of those is never released and leaks until `GC-2` teardown. The operand stack **does** own its references (`kStackOwnsRefs`: push clones, pop/drop releases) and ref locals **are** released at frame exit, so those paths are exact. (Both used to be blocked by a module that pushed a null `jit_value_t`; such a module is now rejected at load — see [`NULL_JIT_VALUE_DIAGNOSIS.md`](../NULL_JIT_VALUE_DIAGNOSIS.md).) The safepoint also releases the dead object's **own** references (`releaseOutgoingRefs`, RC-3), so an object that dies together with its parent is reclaimed rather than leaked; the remaining leak is exactly the borrow case above. A resurrection (count 0 → 1 before the safepoint) now also restores the type tag the queue stamped over, which it previously left clobbered. |
| **RC-2 — Cycle collection (day one, with RC-1)** | ✅ Bacon–Rajan synchronous trial deletion. The candidate buffer is fed by a release that leaves an object alive (`possibleCycleRoot`, bounded at `kMaxCycleCandidates`); the colours live in a map local to one trial, so no header word is needed and the JIT-mirrored layout is untouched (H1); the three phases are iterative DFSs over the same `childSlotsOf` walk the reclaimer uses. `gcCollect()` runs it at the same safepoint as the reclaimer — **including when nothing was queued**, which is the normal case for a cycle, since a cycle's last external release leaves every count above zero. No stack maps, no root set (D18). | RC1, D18 | ✅ A two-object cycle and a self-`struct` cycle are both reclaimed once their external references drop (the test asserts the live-object count returns to its pre-graph value), while a cycle still referenced from outside is kept **and its counts restored exactly** — asserted, because forgetting to add back the internal references would silently corrupt the count. **Trigger:** the trial is driven by releases, so a cycle is examined once something pointing at it was dropped, which is how it becomes unreachable in the first place. |
| **RC-3 — Trap-safe refcount ops** | ✅ Saturating `clone` (`refClone`), trapping `drop` below zero (`refDrop` → `TrapKind::Internal`, never wrap), poison-on-free, the leak counters (`GcHeap::liveObjectCount()`, `totalRecycles()`), and the **iterative drop worklist**: `gcCollect()` walks a dying object's reference-typed fields/elements via `subtypeForTypeId` + `releaseOutgoingRefs` and appends children that reached zero to the same worklist, so one safepoint frees the whole graph without recursion. | RC7, RC8, RC9, D19 | Done: overflow/underflow/double-drop trap or leak deterministically, and a deep chain is released iteratively — the test uses 10⁵ links (the doc's 10⁶ is the same shape, shortened to keep the suite quick), which no recursive walk survives. Five tests cover it: baseline-return of the live count, poison-on-free plus reuse zeroing, clone saturation, a shared child surviving one of two dying parents, and the deep chain. **Still open elsewhere:** frames' uncounted borrows (parameters, control-flow slots) are not covered by the walk, and a type outside this instance's module is left to leak rather than corrupted. |
| **RC-4 — Atomic refcounts / threads-ready** | Make the header count `std::atomic<int32_t>` with acquire/release ordering; stress a clone/drop race. | RC2, RC11, D16 | The count is atomic in every multi-thread-capable build and a two-thread clone/drop stress test is clean; unblocks `THREADS.md`. |
| **RC-5 — Deferred RC (optional, measure first)** | Stop counting frame-local, non-escaping references; recover them with a conservative or shadow-stack root scan at safepoints. | RC3, RC10, D17 | Only if refcount traffic measurably dominates a local-heavy loop; a before/after benchmark is recorded. |
| **RC-6 — Backup tracing pass (optional, last resort)** | A region-teardown trace (all objects dead by definition) and, if ever needed, a conservative-scan-fed trace. | RC1 (belt-and-braces), D18 | Region teardown reclaims everything the counts missed; a full trace stays documented as unnecessary while `RC-2` holds. |

Guardrails for every milestone:

- Do not regress the curated `wast_supported.txt` set; run
  `./WasmJit '~[spec]'` and `./WasmJit` after each change.
- Keep the layout invariant (H1): if you touch one of
  `structFieldOffsets`/`structSize`/`arraySize` **or** `translateStruct`/
  `translateArray`, change the other in the same commit and add a test that
  compares offsets.
- Keep `ref.eq`/null/packed-extension semantics covered by at least one test
  each (§6.3).
- **Do not land `RC-1` without `RC-2`.** A collector that cannot reclaim cycles
  is not shippable; they are one deliverable split across two rows. ✅ Satisfied:
  both landed, and `gcCollect()` now reclaims cycles at the same safepoint.
- **No raw reference stores.** Every ref-typed store in generated code goes
  through `wasm_ref_store` (D15); review for a bare `jit_insn_store` into a ref
  slot.
- Keep the refcount-header offsets (`TypeId@0`, `rc@4`) identical in the runtime
  helpers and the JIT, and add them to the H1 offset test.

### 7.1 Consolidated status — one row per number

Work proceeds **in numeric order**: a higher number is not started until *every*
aspect of the current one is closed **and** its evidence recorded here. "Aspect"
means each sub-item that number carries across §5 and §6.4 — e.g. `G8` stayed open
until `ref.func` resolved for globals, for segment copies *and* for table
defaults, not just the first one to be fixed.

| Number | Status | Evidence (current) | Notes |
|---|---|---|---|
| `G1` | ✅ Closed | `GcHeap::owns` + header `TypeId` (`ref_func` 14/0) | — |
| **`G2`** | ✅ **Closed** | `ref_test` **38/32 → 69/2**, `ref_cast` **33/11 → 45/0** | Cross-module `funcref` identity. The Store now owns a registry of callable ranges (`registerCallableRange`/`callableAt`), so a `funcref` is identified — and its canonical `TypeId` read — from *any* module, in O(#instances) instead of the old per-instance O(#functions) scan; and the "unknown reference" fallback now lists `Func`. **The evidence only moved once `R5` was fixed**: these cases were gated behind a skipped `(invoke "init" (ref.extern 0))`. |
| `G3` | ✗ **Deferred — intersects `ABI.md`** | `ref_test` 69/2 (the 2 are `ref_test_extern`) | `any.convert_extern`/`extern.convert_any` are identity (D5). The remaining 2 failures show *why* identity is not enough once the two hierarchies are mixed: `extern.convert_any` of an i31 leaves a tagged immediate in an `externref` slot, and `ref.test (ref extern)` then correctly reports false. Distinguishing the hierarchies needs a representation decision (tag or box) that is only coherent **together with the host-object model of `externref`** — which is `ABI.md`'s domain (`R4`), not GC's. See §7.2. |
| `G4` | ✅ Closed | `array_new_elem` 14/0, `table_init` 725/0 | — |
| `G5` | ✅ Closed | `i31` 68/0, `table` 18/0 | Table initialisers: decode (`0x40 0x00`), apply every slot, defer `ref.func`. |
| `G6` | ✅ Closed | `global` 70/0 | — |
| `G7` / `GC-4` | ✅ Closed | `br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0, `br_on_non_null` 11/0 | **The decode-diff found it:** `BlockType` could not represent a typed reference, so `(block (result (ref i31)))` (`02 64 6C`) consumed `0x64` and left `0x6C` to be read as the next opcode — the stray `I32DivU`/`I32Mul` was the heap-type byte. `BlockType` now carries the heap-type immediate. The control-flow half also landed: all four `br_on_*` ops model the label's `t*` values and store the whole window with the branch's reference staged on top, and a function-label target is a conditional return (`emitFunctionReturn(false)`). |
| `G8` | ✅ Closed | `table` 18/0 (exported `(ref $f)` global across modules) | Sentinel resolved after compilation in all three places. |
| `G9` | ✗ Open | harness still skips `assert_invalid` | No validating front end beyond the unmodelled-value-type rejection (`D`). |
| `C1`–`C7` | ✅ Closed | `array_init_data` 44/0, `array` 40/0, `ref_cast`/`call_ref` running | Crash triage complete; the aborts that remain are deliberate (`docs/SAFETY.md` §2.4). |
| `H1` | ✅ Guarded | `GC layout: the JIT's struct field offsets match the shared layout helpers` | The dual layout logic is now *checked*: the test asserts the JIT's LibJIT field offsets equal `gcAlignUp`/`gcStorageAlign`/`gcStorageBytes` (the shared source of truth the runtime's `structFieldOffsets` uses) for packed i8/i16, i32, i64, f64 and v128 — and it passes, so the two implementations agree today. The reclaimer and the cycle trial additionally share one traversal (`childSlotsOf`). |
| `H2`, `H8` | ✅ Closed | — | — |
| `H3` | ✅ Closed | `Store::callableAt` | The per-cast O(#functions) scan is gone with `G2`: identification is now a range test over registered callable ranges (one per instance). |
| `H4` | ✅ Closed | `i31` 68/0 | The tagged-LibJIT-type / `void_ptr` round-trip that `i31.get_s/u` depend on is exercised by the spec script across every interesting pattern (0, ±1, `0x3fff_ffff`, `-0x4000_0000`, `0x7fff_ffff`, `0xaaaa_aaaa`, `0xcaaa_aaaa`) and all pass. |
| `H5` | ✗ Deferred | — | `stringref`/`stringview_*`: out of the 3.0 MVP, its own proposal (§7.2, §10). |
| `H6` | ✗ Deferred | — | `ref.eq` operand typing needs `G9`'s validator (§7.2). |
| `H7` | ✅ Closed (by design) | `WasmValue.hpp` | `Value::kind` is a *runtime-side* tag for constant-expression values (globals, segments, imports, and the harness's `referenceArgument`); generated code never sees a `Value` — the JIT derives its type from the instruction's field/opcode type. The two agree because every place a `Value` is produced sets `kind` and every consumer is runtime code. |
| `R1` | ✅ Closed | `table imports are matched against the exported table's type`; `table`/`table_init`/`elem`/`call_indirect` unchanged | The exporter publishes its `TableType` + canonical element `TypeId`; `resolveTable` matches element type by subtyping and limits/index-type/shared-ness by the spec's limit rule (`tableSatisfiesImport`). The manual test asserts that a `funcref` table cannot satisfy an `externref` import and that a `2..8` table cannot satisfy a `5..8` import, while the compatible `shared_table_b` import still links. Embedder-registered tables without a published type stay unchecked. |
| `R2`, `R3`, `R6`, `R7` | ✅ Closed | `elem` 108/0, `table_init` 725/0, `table` 18/0 | — |
| `R4` | ✗ Open (deferred) | — | `externref` has no host model (`ABI.md`, §7.2); it stays a `void*` and `ref.eq` on externrefs is unvalidated pointer equality. |
| `R5` | ✅ Closed | `table_fill` 36/0/9, `array_new_data` 28/0/0, `relaxed_min_max` 25/0/0 | Reference arguments *and* results are marshalled; `either` result lists are verified against each alternative. |
| `R8` | ◑ Partly | `TableType`, `applyActiveSegments` | Table-initialiser half fixed (`G5`); element-type validation half waits on `G9`. |
| `L1`, `L2`, `L3` | ✅ Closed | the seven `GC reclamation:` tests | Reclamation, refcounts and the store barrier all landed. |
| `GC-1` | ✅ Closed | `ref_test` 69/2, `ref_cast` 45/0 | Function references are identified through the Store's callable-range registry and carry their canonical `TypeId`, so a cross-module cast needs no per-instance pointer scan. |
| `GC-2` | ✅ Closed | `GC heap` test, poison-on-free | Recycler wired; debug poison on free landed. |
| `GC-3` | ✅ Closed | `i31` 68/0 | — |
| `GC-4` | ✅ Closed | `br_on_cast*`, `br_on_null`, `br_on_non_null` | Same work as `G7`; both closed by the block-type decode fix plus the label-value model. |
| `GC-5` | ✅ Closed | `array` 40/0, `array_init_data` 44/0 | — |
| `GC-6` | ✅ Closed | `WasmTrap.hpp` | — |
| `GC-7` | ✅ Substantively closed | `Store::callableAt` | The expensive part landed with `H3`: a cast/test is now O(#instances) instead of O(#functions). What remains of the row is *inline caching* at a hot call site — an optimisation that the plan explicitly says to measure first (D3/D12), with no measurement yet showing casts dominating. |
| `GC-8` | ✗ Deferred | — | Extern interop needs the host `externref` model: `ABI.md` (§7.2). |
| `GC-9` | ✅ Closed | `array` 40/0 | — |
| `RC-1`, `RC-1b` | ✅ Closed | `GC reference counting` test | — |
| `RC-2` | ✅ Closed | `GC reclamation: … cycle …` tests | Trial deletion, no root set (D18). |
| `RC-3` | ✅ Closed | deep-chain + poison tests | Iterative worklist, saturating clone, trapping drop. |
| `RC-4` | ✗ Open | — | Atomic counts; the multi-thread prerequisite of `THREADS.md` (`RC2`/`RC11`). |
| `RC-5` | ✗ Open (optional, measure first) | — | The *default* discipline is implemented (`kStackOwnsRefs`: every pushed reference is owned and balanced by an explicit drop). Deferred RC would *remove* that traffic, so it needs a benchmark showing refcount traffic dominating a local-heavy loop before it is worth the root-scan complexity (D17). |
| `RC-6` | ✅ Satisfied as specified | `~ModuleInstance` → `GcHeap::releaseSet` | Region teardown is the documented leak backstop: every region an instance used is released whole at teardown, whether or not the counts balanced. The row's other half — a conservative-scan-fed trace — is explicitly "only if `RC-2` proves insufficient", and trial deletion holds. |

### 7.2 Deferred: intersections with other proposals

The rule this section enforces: GC work stops where the *right* answer depends on
a decision another document owns. Those parts are recorded here with the reason,
rather than guessed at inside GC.

| Item | Intersects | Why GC cannot decide it | What GC can do meanwhile |
|---|---|---|---|
| `G3` (extern/any conversion fidelity), `R4` (host `externref` model), `GC-8` (extern interop) | **`ABI.md`** | Distinguishing the `extern` and `any` hierarchies needs a representation for a *host* extern object (tag or box), and that representation is the embedder's value model. `GC.md` D5 chose identity deliberately, under a "no host/reference mixing" contract that the spec suite does not honour. | Nothing further: the engine side is consistent with D5. When `ABI.md` fixes the host model, revisit D5 and delete `R4`/`G3`. |
| `RC-4` (atomic counts; `RC2`/`RC11`) | **`THREADS.md`** | The count already uses `std::atomic_ref` and is therefore *safe*, but acquire/release ordering and a two-thread stress test are only meaningful once shared memory and agents exist. | The relaxed-ordering choice is documented in `WasmGcHeader.hpp`; the upgrade is a one-line ordering change plus a stress test, for whoever owns threads. |
| `G9` (no validating front end), `R8`'s second half (element-type validation), `H6` (`ref.eq` operand types) | **cross-cutting validation** (also `TYPE_IDENTITY.md` M4, and the `D` fix's long-term answer) | A validator that checks GC type rules is only useful if it checks *all* the rules of *all* the proposals; a GC-only validator would reject modules the rest of the engine accepts. | `Module::validateValueTypes()` already rejects unmodelled value types at load; the GC rule list (cast target vs source, `struct.new_default` defaultability, nullability, `ref.eq` operands) is recorded in §5.1 `G9` for whoever writes the validator. |
| `H5`, §10 (`stringref`, `stringview_*`) | **its own proposal** (§10 in this document) | Out of the Wasm 3.0 GC MVP by §3.4; the reserved encodings are stale and the instructions have no parser or dispatch. | Keep them explicitly unsupported — the enum + `void*` lowering exist but nothing produces them. |

**Remaining in GC's own domain** (not deferred): `G7`'s stream-drift chase (the
decode-diff above — the model half, the `castop` flags and the slot-model asserts
all landed) and `R1` (needs the registrant to publish its `TableType` through
`ImportResolver::registerTable`: the registry stores only a `TableInstance*`, which
carries no element type, so this is an interface change, not a comparator). Both
are pinned to a concrete first step. Everything else in §5.1–§5.6 that GC owns is
closed or guarded.



## 8. Indicative test survey

> **Staleness note (revision 11):** this survey is the 2026-10-13 snapshot that
> *motivated* the plan, and the numbers have moved since — `i31` 63/5 → **68/0**,
> `array` 36/4 → **40/0**, `array_init_data` 41/3 → **44/0**, `array_init_elem` and
> `table` clean, `table_init`/`elem` no longer aborting, `ref_test` 38/32 →
> **69/2** (reference arguments are marshalled now; the two failures are `G3`),
> `type-equivalence` 30/1 → **31/0**, and the four `br_on_*` scripts are green
> (`br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0,
> `br_on_non_null` 11/0). **For current per-number status read §7.1**, not the
> rows below; treat this section as history.

Survey taken with `build/Desktop-Debug/Test/WasmJit` (2026-10-13; `assert_trap`
now executed, so the "skipped" column no longer hides the trap cases).
`WASM_SPEC_TIMEOUT=10`, one script at a time.

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `struct` | 14 | 0 | 16 | happy path works; most ref-valued assertions skipped |
| `array` | 40 | 0 | 14 | improved once traps are recoverable |
| `array_copy` | 31 | 0 | 4 | works |
| `array_fill` | 27 | 0 | 3 | works |
| `array_new_data` | 23 | 0 | 5 | works |
| `array_new_elem` | 14 | 0 | 10 | G4 fixed (segment materialisation) |
| `array_init_data` | 41 | 3 | 2 | C1 no longer aborts; a data-offset bug remains |
| `array_init_elem` | 33 | 0 | 3 | works |
| `i31` | 63 | 5 | 5 | G5 (table init expressions) |
| `ref_eq` | 83 | 0 | 6 | works |
| `ref_is_null` | 15 | 1 | 6 | table init (G5) |
| `ref_null` | 34 | 0 | 0 | `exnref`/`nullexnref` modelled (EXCEPTIONS.md EX-1); null results verified |
| `ref_as_non_null` | 6 | 0 | 1 | works |
| `ref_test` | 69 | 2 | 0 | `G2` closed; the two failures are `ref_test_extern` (`G3`/`D5`) |
| `ref_cast` | 33 | 11 | 1 | C2 fixed; remaining failures are G1/G2 |
| `ref_func` | 14 | 0 | 3 | C2 fixed |
| `call_ref` | 31 | 0 | 4 | C3 fixed |
| `return_call_ref` | 40 | 0 | 13 | C4 fixed |
| `br_on_cast` | 31 | 0 | 6 | `G7`/`GC-4` fixed (block-type decode + label values) |
| `br_on_cast_fail` | 31 | 0 | 6 | `G7`/`GC-4` fixed |
| `br_on_null` | 9 | 0 | 1 | branch carries `t*` only; fall-through keeps the reference |
| `br_on_non_null` | 11 | 0 | 1 | branch carries `t* (ref ht)`; fall-through consumes it |
| `extern` | 1 | 0 | 17 | mostly skipped |
| `type-canon` | 2 | 0 | 0 | type identity works |
| `type-equivalence` | 31 | 0 | 1 | C5 fixed; clean |

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
| `ref_null` | 34 | 0 | 0 | `exnref`/`nullexnref` modelled (EXCEPTIONS.md EX-1); null results verified |
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

Reference counting now has **two manual tests**. `GC reference counting: clone,
drop and store barriers` (`Test/wasm_wat/gc_refcount.wat`) covers the header
count, `refClone`/`refDrop`/`refStore`, the underflow trap, the safepoint
`gcCollect` + block reuse (RC-1b), runs the JIT `struct.set` barrier end to end,
and asserts `array.new_default` is a **single** heap block (inline arrays, GC-9).
`GC heap: region allocation, alignment and
recycling` covers 16-byte alignment, mmap zero-fill, region batching (1000
objects → at most one extra region), free-list reuse, dedicated large regions,
`owns()` classification and bulk `releaseSet`. Still absent: an end-to-end
*reclamation* check (memory returning) and a cycle test, both of which need
`RC-2`/`RC-1b`; until then a leak is visible only through process RSS.

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

Reference counting / cycle collection (background for §6.1/§6.4):

- Bacon & Rajan, *Concurrent Cycle Collection in Reference Counted Systems*
  (ECOOP 2001) — the trial-deletion algorithm behind `RC-2`; the synchronous
  single-threaded variant is what `RC-2` needs.
- Deutsch & Bobrow, *An Efficient, Incremental, Automatic Garbage Collector*
  (CACM 1976) — the original deferred reference counting; Blackburn & McKinley,
  *Ulterior Reference Counting* (OOPSLA 2003) — the modern hybrid that defers
  stack references and traces the heap, the model for `RC-5`.
- Boehm & Weiser, *Garbage Collection in an Uncooperative Environment*
  (SPE 1988) — conservative stack scanning, relevant only to `RC-6`'s
  last-resort trace.

Related proposals covered in §10 (neither is part of 3.0):

- Stringref (reference-typed strings):
  https://github.com/WebAssembly/stringref/blob/main/proposals/stringref/Overview.md
- JS String Builtins (JS-API `wasm:` builtin namespace):
  https://github.com/WebAssembly/js-string-builtins/blob/main/proposals/js-string-builtins/Overview.md

In-repo:

- [`docs/TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) — type identity, interning,
  `match`, and the `M1`–`M8` plan.
- [`docs/ABI.md`](../ABI.md) — VMContext passing, Store ownership, and the
  "spiritual successor to QVM" design philosophy.
- [`docs/TESTING.md`](../TESTING.md) — running/filtering the two test families.
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
  header (encoding tag + length + optional breadcrumbs) in the same
  reference-counted region allocator proposed in §6.1/§6.4. Strings then live
  and die with their instance,
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
