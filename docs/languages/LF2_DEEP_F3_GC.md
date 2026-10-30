# `F3` — Bottom Heap Types: The GC Subtyping Gap and Its Fix {#lf2_deep_f3}

Status: deep-study + fix note (revision 1 — 2026-10-30). It investigates `F3` of
[`LF2_INDEX.md`](LF2_INDEX.md) §5 — a **valid** WasmGC module refused by the
loader — states the rule the engine got wrong, and records the fix and its
tests. This is the one finding `LF-2` classed as a serious engine gap, and it is
treated as such: the rule is fixed in the type registry, with the minimal repro
and the real toolchain modules as regression fixtures.

**Headline.** `heapMatches` in `WasmBase/WasmTypeRegistry.cpp` did not model the
bottom heap types as subtypes of a concrete type, so any `sub` annotation whose
field narrows `(ref null $t)` to `nullref` was rejected as invalid. Dart's
module fails exactly on that shape; Kotlin/Wasm's fails with the same message
from a different field relation. One function, one rule, two toolchains blocked:
that is engine-level, class **A**, and the fix is required before any WasmGC
language can be studied at all.

Audience: whoever owns the type registry, anyone relying on a green `L4` row,
and `LF-3` when it decides what to re-run.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What the rule is, in the spec's terms |
| §3 | What the engine did, and why the modules were refused |
| §4 | The minimal reproduction and the real-world instances |
| §5 | The fix |
| §6 | The tests, including the negative controls |
| §7 | What the fix changes for the report set |
| §8 | What remains open about Kotlin's second relation |

## 2. The rule

The GC proposal's heap-type lattice has four bottom types — `none` (bottom of
the internal hierarchy), `nofunc`, `noextern`, `noexn` (and `nocont` for stack
switching). They are subtypes of **every** type in their hierarchy:

```
nullref          = (ref null none)      <: (ref null $t)     for struct/array $t
nullfuncref      = (ref null nofunc)    <: (ref null $f)     for func type $f
nullexternref    = (ref null noextern)  <: extern
nullcontref      = (ref null nocont)    <: (ref null $c)     for cont type $c
```

A declared subtype's comptype must match its supertype's **covariantly** in its
immutable fields. So this is valid:

```wat
(type $b (sub final (struct (field i32))))
(type $a (sub (struct (field (ref null $b)))))     ;; field: (ref null $b)
(type $c (sub final $a (struct (field nullref))))  ;; field: nullref <: (ref null $b)
```

and so is the same shape with `(ref $b)` narrowed to `(ref none)`, or a
`nofunc` field narrowed in a function-typed field.

## 3. What the engine did

`heapMatches` had three cases:

```cpp
if (a.kind == CanonHeapType::Kind::Abstract) {
    if (b.kind == CanonHeapType::Kind::Abstract) return abstractMatches(a.abstract, b.abstract);
    // b.kind == Global: fell through to `return false`
}
if (a.kind == CanonHeapType::Kind::Global) { /* concrete/abstract or concrete/concrete */ }
return false;
```

`abstractMatches` *did* know the bottom types (`None` matches every internal
abstract type, `NoFunc` matches `Func`, …), and `concreteMatchesAbstract` handled
the reverse direction. The missing edge was **abstract-bottom → concrete**, so
every field that used `nullref` (or `nullfuncref`) against a concrete supertype
was reported as:

```
type section: invalid `sub` annotation (comptype does not match its supertype)
```

Because the same `heapMatches` backs `TypeRegistry::matchesHeap`, the gap was not
only a load-time strictness problem: a runtime `ref.cast`/`call_indirect`
matching against such a type would have had the same blind spot.

## 4. The reproduction

### 4.1 Minimal (now a test fixture)

`Test/wasm_wat/bottom_subtype.wat`:

```wat
(module
  (type $b (sub final (struct (field i32))))
  (type $a (sub (struct (field (ref null $b)))))
  (type $c (sub final $a (struct (field nullref))))
  (func (export "answer") (result i32) i32.const 42))
```

`wasm-tools validate --features all` accepts it (`exit 0`); before the fix the
loader refused it, and after the fix it loads and `answer` returns 42.

### 4.2 The real instances

| Module | Shape | Relation to `F3` |
|---|---|---|
| `t20_dart.wasm` (Dart 3.13.7) | type 41: `(sub final 11 (struct … (field nullref) …))` over a supertype field `(ref null 1)` | **exactly** the missing rule |
| `t53_kotlin_wasi.wasm` (Kotlin/Wasm 2.4.21) | `$kotlin.String (sub final $kotlin.Any (… (field $vtable (ref "#type15 <classVTable>")) …))` over `$kotlin.Any`'s `(field $vtable (ref $<classVTable>))` | a *different* relation — concrete-subtype covariance — which the standalone check already accepts; its failure needs its own explanation (§8) |

Both modules are valid per `wasm-tools`, which is why the study reports an
engine defect rather than a toolchain quirk.

## 5. The fix

One function, one added edge: `heapMatches` now answers the
abstract-bottom → concrete question instead of falling through to `false`.

```cpp
if (a.kind == CanonHeapType::Kind::Abstract) {
    if (b.kind == CanonHeapType::Kind::Abstract)
        return abstractMatches(a.abstract, b.abstract);
    if (b.kind != CanonHeapType::Kind::Global)
        return false;
    // A bottom heap type is a subtype of every compatible concrete type. This is
    // the only legal abstract→concrete edge in the GC lattice.
    const CoreTypeView expected = v.type(b.id);
    if (expected.kind() == CanonTypeKind::Import)
        return abstractMatches(a.abstract, expected.importBound().abstract);
    switch (a.abstract) {
    case AbstractHeapType::None:
        return expected.kind() == CanonTypeKind::Struct
            || expected.kind() == CanonTypeKind::Array;
    case AbstractHeapType::NoFunc:   return expected.kind() == CanonTypeKind::Func;
    case AbstractHeapType::NoCont:   return expected.kind() == CanonTypeKind::Cont;
    default:                         return false;
    }
}
```

Deliberate omissions, so the fix is not a blanket "accept anything":

- `NoExtern` and `NoExn` stay `false`: `extern`/`exn` have no concrete subtypes,
  so a concrete type index can never be their supertype.
- A **non-bottom** abstract type (`any`, `eq`, `struct`, `array`, `func`, …)
  against a concrete type still returns `false`: `any` is a *supertype* of every
  internal type, never a subtype of one.
- The nullable/`shared` bits are already handled by `valueMatches` before
  `heapMatches` is called, so the fix does not touch them.

Because `TypeRegistry::matchesHeap` delegates to the same function, the fix
applies to the runtime matcher as well as to the load-time `sub` check — one
rule, one implementation.

## 6. The tests

| Test | Asserts |
|---|---|
| `Test/wasm_wat/bottom_subtype.wat` + a positive case in the subtype test | the minimal module loads, and its type group interns |
| negative controls in the same test | a **non-bottom** abstract narrowing (`any`/`struct` against a concrete type) is still rejected; `nofunc` against a struct type is rejected |
| `matchesHeap` unit assertions | `matchesHeap(None, Global(struct))`, `matchesHeap(NoFunc, Global(func))` are true; `matchesHeap(Abstract(Any), Global(struct))` is false |
| `t53_kotlin_wasi.wasm` re-run | the real Kotlin module either loads (and then the `L4` verdict changes) or fails with a *different*, documented reason |
| `t20_dart.wasm` re-run | the Dart module gets past the type section (and then hits its `dart:` import surface, as `S4` predicted) |

The existing negative tests (`bad_subtype_field`, `bad_subtype_kind`,
`bad_subtype_final`) stay: the fix must not turn the validator into a permissive
one.

## 7. What the fix found on the way: `F3a`–`F3d`

Fixing the rule exposed three more places where the same "bottom type / dormant
payload" assumption lived, and one that is still open. They are numbered as
sub-findings so a reader can tell which are closed.

| ID | Where | What was wrong | Status |
|---|---|---|---|
| **F3a** | `WasmTypeRegistry.cpp`, `heapMatches` | the abstract-bottom → concrete edge was missing (§5) | **fixed** |
| **F3b** | `WasmCanonicalType.hpp`, `CanonHeapType::operator==` | compared `memberIndex` even when `kind == Global`, so two identical global references compared unequal and a **mutable** struct field failed its invariance check (the Kotlin module's `(mut (ref null …))` field: ids equal, `memberIndex` 0 vs 2) | **fixed**; `heap-type equality ignores the dormant payload` |
| **F3c** | `WasmValidator.cpp`, `dispatchStructNew`/`…NewDesc` | a packed `i8`/`i16` field's *operand* was typed as the packed code instead of `i32`, so `struct.new` with an `i8` field rejected a legal `i32` operand (Kotlin's `SharedVariableBoxBoolean`) | **fixed** (a `fieldOperandType()` helper; `struct.new_default` uses it too) |
| **F3d** | runtime `ref.cast` on a Kotlin GC object | after F3a–F3c the module loads and validates, then start-up failed with `cast failure in function 305`; the object's header had been turned into `kNone` by `reclaimGcObject`, i.e. it was **freed while a live reference existed** | **fixed** (§7.1) |

### 7.1 `F3d` — the reference-ownership bug

The crash barrier described in the earlier revision (a `kNone` guard in
`heapMatches`) made the failure observable instead of fatal; it did not explain
it. The explanation came from a hardware watchpoint on the object's header:

```
Hardware watchpoint 3: *(unsigned int*)$obj
Old value = 121
New value = 4294967295          # TypeId::kNone
0x… in WASM::ModuleInstance::reclaimGcObject(void*)
```

So the object was **reclaimed while the Wasm code still held it**. The chain:
`struct.new` → 1 stack count → `local.set` on a **parameter** → the parameter
branch of `local.set` assumed "a parameter is the caller's borrow, so it owns
nothing" and therefore skipped the retain, then dropped the stack's count → the
fresh object's refcount hit zero in the callee, and the caller's later
`ref.cast` saw a dead object.

Why a naive flag fix was wrong: ownership was tracked per *instruction*, so a
`local.set` inside a loop emitted its old-value release only for the first
iteration — enough to leak the loop's objects and fail the existing
`two agents allocate and release GC objects` test (40,000 reclaims became 2).
The model has to be **static**.

**The fix** (`LibJit/LibjitOpcodeDispatcher.{hpp,cpp}`,
`LibjitModuleCompiler.cpp`):

- `emitReferenceParameterRetains()` runs in the prologue and gives every
  reference-typed parameter its own count in its local slot, releasing it at the
  epilogue. Net zero for a parameter that is only read; correct for one that is
  overwritten.
- With that, every reference local owns exactly one count, so
  `local.set`/`local.tee` always clone into the slot and release the old value,
  and `emitDropRefLocals()` drops every reference local — static rules, valid on
  every loop iteration.
- The `kNone` guard stays as a barrier: a type id that was never stamped is a
  failed cast, not a dereference of an out-of-range record.

Kotlin/Wasm after the fix:

```
call add/i32(19,23) -> i32=42
call answer/i32() -> i32=42
call throwCatch/i32(21) -> i32=42
call throwCatch/i32(-1) -> i32=-1
call _start/void()hello from Kotlin/Wasm on WASI: add(19, 23) = 42
```

and the GC regression suite is green again (the concurrency reclaim test, the
refcount fixtures and `[legacy-eh]` all pass).

The rule is now part of the backend instructions, so the next code generator
does not rediscover it: [`WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) §6
"Reference ownership", the corresponding row in
[`BACKEND_WORK_ORDER.md`](../BACKEND_WORK_ORDER.md) §5/§8, the contract row in
[`LAYERING.md`](../LAYERING.md) §4, and the RC3 note in
[`proposals/GC.md`](../proposals/GC.md) §7.

### 7.2 What changed for the report set

- **`T20` (Dart)** gets past the type section and now fails on its **legacy
  EH** encoding (`F2`), exactly as `S4` predicted, and with
  `WASMJIT_LEGACY_EH=1` it hits
  `canonical type reference is not declared by this module` in the rewrite — a
  third, separate issue.
- **`T53` (Kotlin/Wasm `wasmWasi`)** **runs**: exports, a guest `try`/`catch`,
  and its own `_start` all behave (§7.1). Verdict: green at this engine commit.
- The minimal repro and the new unit tests are green, and the pre-existing
  negative tests stayed red-as-expected.

## 8. What remains open

| Item | Why it matters | Next step |
|---|---|---|
| The `wasm-opt`-less Dart path | the rewrite rejects Dart's legacy EH with "canonical type reference is not declared" | reproduce on a reduced module; likely a stale index in a copied type reference |
| A conformance sweep of the bottom-type rule | the spec suite already exercises `ref.null none` casts; a targeted set of `sub`-with-bottom-field modules would pin the rule | add them to `Test/wasm_wat/` |
| A GC ownership review of the other backends | `F3d` was a LibJIT lowering bug; Sljit/Asmjit do not execute references yet, but the same "who owns a parameter slot" question will arrive with them | fold into [`BACKEND_WORK_ORDER.md`](../BACKEND_WORK_ORDER.md) when they gain references |
