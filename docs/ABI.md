# WasmJit ABI: VMContext Passing Convention

## Overview

Every JIT-compiled Wasm function takes a `VMContext*` as its first argument
(arg0).  Which `VMContext*` gets passed depends on the call kind and on the
value of `Callable::context`.

> **Runtime thread-safety target:** one `ModuleInstance` per `Module`, with one
> `VMContext` per instance. This ABI description covers the calling and context
> shapes, not a licence to use per-thread instances as a concurrency model.
> The guest/host responsibility split is in [`THREADSAFETY.md`](THREADSAFETY.md).

## Design Philosophy

WasmJit is designed as a spiritual successor to Quake 3's QVM — not as a
browser-embedded WASM engine.  Some level of flexibility and deviation from
the official WASM spec is accepted when it simplifies the implementation.

In particular, **addresses of other module instances' VMContext structs are
burned into the JIT-compiled machine code at compile time**.  This is brittle
by normal standards, but it is safe as long as those instances are held in
`std::unique_ptr<ModuleInstance>` and never move.  If an instance is destroyed
before its callers, callers will hold dangling pointers — the caller must
ensure correct lifetimes.

**No trampolines are generated for cross-module direct calls.**  The callee's
`VMContext*` is simply baked in as an immediate constant.

## VMContext Selection Rules

| Call instruction     | How arg0 is determined                                       |
|----------------------|--------------------------------------------------------------|
| `call` (internal)    | Caller's `VMContext*` (same instance)                        |
| `call` (imported, `context == nullptr`)  | Caller's `VMContext*` — native host import sees caller's memory |
| `call` (imported, `context != nullptr`)  | `Callable::context` burned as constant — cross-module Wasm import |
| `call_indirect`       | `Callable::context` loaded at runtime — callee's context     |
| `call_ref`            | `Callable::context` loaded at runtime — callee's context     |

## Multiple Results

A function type with more than one result does **not** return an aggregate by
value. The backend lowers it to

```
void f(VMContext*, <params...>, void* results)
```

where the last argument points at a result buffer owned by the caller. The
callee writes the results through it, in declaration order, each field at its
natural alignment; the caller passes the address of a buffer it allocated and
reads the fields back after the call. Single-result and void signatures are
unchanged, so the common `ret f(VMContext*, ...)` shape above still holds.

The spec harness marshals them for `assert_return` (comparing each field) and
passes a caller-owned buffer on the result-discarding paths (bare `action`,
`assert_trap`, `assert_exception`, `assert_exhaustion`) — see
`docs/proposals/MULTI_VALUE.md` MV1. An embedder calls a multi-result export
with

```cpp
alignas(16) unsigned char results[16] = {};   // translateStruct's size
WASM::callCallableWithResults(callable, results, arg0, arg1);
```

— the helper appends the buffer to the native call. `callCallable<Ret>` remains
the API for void/single-result signatures. The buffer must be at least as large
as the lowered result tuple (`jit_type_get_size(translateStruct(results))`);
fields sit at the offsets `translateStruct` assigns, in declaration order.
Within a module, and across modules, direct/indirect calls and the tail-call
driver all use the buffer convention.

## Reference and Global Representation

> **Status:** **R1 (globals as cells) is implemented.** R2 (tagged table slots)
> and R3 (host extern boxes) remain staged; nothing in their descriptions has
> changed yet.

Before R1 the engine used one untyped `void*` for every reference, `Callable*`
table slots, and a flat `Value` array for globals. R1 has replaced the global
half with cells; the reference/table half is still as described. That
representation is enough for the core types in the common case, but it is the
shared blocker behind four proposals:
generic/typed tables (`FUNCTION_REFERENCES.md` FR6, `EXCEPTIONS.md` EX5), host
externrefs and `any`/`extern` identity (`GC.md` R4/G3/D5), mutable imported
globals (`MUTABLE_GLOBALS.md` MG2/MG3/MG4), and component-level reference
bookkeeping (`COMPONENT_MODEL.md` CM8).

### Today

| Value / slot | Encoding | Consequence |
| --- | --- | --- |
| `funcref` | `Callable*` | fine |
| `externref` | host `void*` | no box and no identity; only static typing separates it from a funcref |
| `exnref` | exception-object pointer | fine on the stack or in a global, not storable in a table |
| GC ref | Store-owned object pointer | fine |
| `i31ref` | `(value << 1) \| 1` | fine |
| table slot | `Callable*` (`TableInstance::base`) | only *nominally* a function table; externref/GC/exnref values are cast through it |
| global | a `Value*` cell per index (R1); `VMContext::globals` is `Value**` | imported cells alias the exporter |
| exported global | the exporter's cell is published (R1) | a later `global.set` is visible to importers (and vice versa) |

Static typing at each use site is why this works at all. It stops working when
the static type is only `anyref` (an `ref.test`/`ref.cast` must distinguish a
funcref from an externref from a GC object), and host interop needs identity.

### Decision

One **64-bit tagged reference word** for every reference value and table slot,
plus **global cells**. The tag is in the low three bits; all heap pointers are
8-byte aligned, so the word stays pointer-sized and every existing 8-byte slot
keeps its size.

| Tags | Payload |
| --- | --- |
| `0` `Func` | `Callable*` |
| `1` `Extern` | `ExternBox*` (Store-owned) |
| `2` `Gc` | GC object pointer |
| `3` `Exn` | exception-object pointer |
| `4` `I31` | signed 31-bit value in bits 3…33 |
| `5`–`7` | reserved |
| all-zero word | null (the only null representation) |

- `ref.is_null` is `word == 0`; `ref.eq` is null==null, then payload equality
  within one tag (i31s compare by value).
- `ref.test`/`ref.cast` read the tag for the hierarchy, then the GC object's type
  id; `any.convert_extern`/`extern.convert_any` become box/unbox, not identity.
- `call_indirect`/`call_ref` require `Func` and non-null, otherwise the usual
  trap. A non-funcref table can no longer be mistaken for a callable array.
- Table slots become `Ref` (`uint64_t`); `TableInstance::base` stays the first
  field, only its pointee type changes. The write barrier runs only for `Gc`.
- Globals become **cells**: `VMContext::globals` is a `Value**` (one pointer per
  global index) and an imported global's cell belongs to the exporter. All
  `global.get`/`global.set` lowering funnels through `globalSlotAddress`, so the
  backend change is: load the cell pointer, then load/store at the cell.
- `ExternBox { const void* host; uint64_t identity; }` is Store-owned. The spec
  harness maps `(ref.extern N)` to one box per `N` (memoized per script), which
  makes `ref.eq` on externrefs meaningful and lets `table_fill`'s
  `(ref.extern 1)` round-trip keep its identity.

### Migration

| Stage | Work | Unblocks | Acceptance |
| --- | --- | --- | --- |
| **R1 — Globals as cells** ✅ | Done: `VMContext::globals` is `Value**`; `Internals::globalRefs` holds one cell per index (imported cells are the exporter's, defined cells point into `globalsStorage`); `ImportRegistrar::registerGlobal` publishes a `Value*` + `GlobalType` and `resolveGlobal` returns the cell; `registerExports` publishes the exporter's cell; `globalSlotAddress` loads it; the interpreter path dereferences it. | MG2, MG3, MG4 | Done: `global` **77/0**; `shared_global_a/b` prove writes cross both ways; `shared_global_bad` is rejected. |
| **R2 — Tagged slots** | introduce `Ref` + tag constants (`WasmValue.hpp`); table slots become `Ref`; `call_indirect`/`call_ref` check `Func`; `table.get/set`, `ref.eq`, `ref.test/cast`, `ref.is_null` use the tag; exnref/GC/externref tables. | FR6, EX5, GC R4, CM8 | `try_table` stays 56/0; `table_fill` 36/0/9 and `array_new_data` 28/0/0 stay green; an `exnref` table test passes |
| **R3 — Extern box** | Store-owned `ExternBox`; harness `(ref.extern N)` identity; host registration helper. | GC G3/D5, host interop | `ref_test` **69/2 → 71/0** (the two failures are `ref_test_extern`), and `(ref.extern 1)` result round-trips keep identity |

R2 and R3 are independent after R1, but the tag values are fixed in R2, so R3
lands second. R1 is the contained slice: one `VMContext` field type, one lowering
site, and the import/export plumbing; the `LinearMemory`/`TableInstance` field
offsets and `v128`/scalar layouts are untouched.

**Rejected alternatives:** a 16-byte `{payload, tag}` reference (doubles the size
of every call argument, table slot and global), and NaN-boxing (the value stack
also carries `f64`, so payloads would collide). Neither is worth it for a
single-threaded engine whose table indirection is already the hot path.

## Embedder Guide

### Native (host) function imports

Register with `context = nullptr`.  The JIT will pass the caller's `VMContext*`
at compile time, giving the native function access to the calling module's
linear memory, globals, table, and `hostData`:

```cpp
// Registration (before instantiation):
resolver.registerFunction("env", "my_func", WASM::Callable{
    .fnPtr     = reinterpret_cast<void*>(my_native_func),
    .context   = nullptr,   // signals: use caller's VMContext
    .typeIndex = typeIdx
});

// After instantiation, attach host state:
auto instance = compiler.instantiate(module, resolver);
instance->context()->hostData = &myAppState;
```

**Native function signature:**

```cpp
void my_native_func(WASM::VMContext* vm, int32_t arg1, double arg2) {
    // vm is the CALLER's VMContext — access everything the module sees:
    //   vm->memories    — array of LinearMemory* (imports first, then locals)
    //   vm->memoryCount — number of entries in vm->memories
    //   vm->globals     — one Value* cell per global index
    //   vm->table       — function table
    //   vm->hostData    — embedder state (set after instantiation)
}
```

### External host adapter (optional `HostBridge`)

The engine has no JavaScript engine, host-language runtime, or `JSValue` type.
An embedder that wants to bridge `externref` values to JavaScript, Lua, Python or
another interpreter installs an optional `WASM::HostBridge` before
instantiation. It is a plain aggregate of function pointers; every callback and
the `VMContext::hostBridge` pointer default to `nullptr`:

```cpp
WASM::HostBridge bridge;
bridge.userData           = &adapter;
bridge.references.retain  = adapterRetain;   // optional, paired with release
bridge.references.release = adapterRelease;
bridge.references.equals  = adapterEquals;   // optional
bridge.references.isHostReference = adapterOwns; // optional predicate
bridge.instanceCreated    = publishHostData; // optional, before start
bridge.instanceDestroying = dropArena;       // optional, before GC teardown

auto instance = compiler.instantiate(module, resolver, &bridge);
```

The bridge is borrowed and must outlive the instance. With no bridge the old
behaviour is unchanged: an `externref` is an opaque `void*` and
`refClone`/`refDrop` do not call host code. With both `retain` and `release`
installed, the engine's existing reference barriers call them for host
references; `isHostReference` controls classification, with a conservative
fallback when omitted (not an i31 immediate, GC-heap object, StringStore object
or registered `Callable`). A reference callback may run during construction
(global initialisers and segments) before `instanceCreated`, so it must use
`userData` for any state it needs that early. `instanceCreated` runs after
initialisation and before `start`, so `hostData` is available to imports called
from `start`; `instanceDestroying` runs at the beginning of `~ModuleInstance`,
before the GC region set is unmapped, and is the designated place to release
per-instance host roots. Neither lifecycle callback may throw, trap, or re-enter
Wasm.

A reference returned by `callCallable` carries the count produced by the Wasm
call; the embedder owns that count and must drop it (or use its own arena).
`ModuleInstance::hostReferenceEquals` delegates to `references.equals`; Wasm
`ref.eq` remains an `eqref` operation and is not changed by the bridge.

This is a generic host-boundary contract, not an implementation of any JS API
proposal: BigInt conversion, `wasm:js-*` builtins, JS Types wrappers and ESM
loading stay in the external adapter. See
[`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) §4.3.

Instance lifetime and call lifetime are separate. `instanceDestroying` is the
backstop for a per-instance host arena, not a per-call cleanup hook. If an
import can run host code that traps, the adapter needs a per-call handle scope;
the current trap path and the proposed Go-style cleanup stack are described in
[`TRAP_CLEANUP.md`](TRAP_CLEANUP.md).

### Cross-module Wasm function imports

Register with `context` pointing to the **callee module instance's** `VMContext`.
The JIT will burn this address into the machine code at compile time:

```cpp
// Module A imports a function defined by module B:
auto instanceB = compiler.instantiate(moduleB, importsB);
auto exportedFunc = instanceB->exportedFunction("some_func");

// In module A's import resolver:
resolver.registerFunction("B", "some_func", WASM::Callable{
    .fnPtr     = exportedFunc->fnPtr,
    .context   = instanceB->context(),  // non-null → burned into Module A's JIT code
    .typeIndex = exportedFunc->typeIndex
});
```

**Caveat:** Module B's `ModuleInstance` must outlive all callers.  The address
of `instanceB->context()` is baked into module A's compiled functions as an
immediate constant.  If instance B is destroyed first, module A will pass a
dangling pointer on its next cross-module call.

## Data Structure Reference

### `VMContext` (`WasmBase/WasmVMContext.hpp`)

Plain-old-data struct passed as arg0 to every JIT function.

| Field                  | Type                   | Purpose                                            |
|------------------------|------------------------|----------------------------------------------------|
| `memories`             | `LinearMemory* const*` | Memory index space (imports first, then locals)    |
| `memoryCount`          | `uint32_t`             | Number of entries in `memories`                    |
| `module`               | `const Module*`        | Type graph metadata for GC/reference checks        |
| `globals`              | `Value**`       | One cell per global index (imported cells alias the exporter) |
| `tables`               | `TableInstance**` | Table index space (imports first, then locals)       |
| `tableCount`           | `uint32_t`      | Number of entries in `tables`                          |
| `importedFunctions`    | `Callable*`     | Imported function handles (import section order)       |
| `importedFunctionCount`| `uint32_t`      | Number of imported functions                           |
| `hostData`             | `void*`         | Opaque pointer for embedder state                      |
| `hostBridge`           | `const HostBridge*` | Optional, borrowed host boundary; `nullptr` is the default |
| `stackLimit`           | `uintptr_t`     | Lowest address generated code may use for its frame; `0` = no limit |

### The guest stack guard (`stackLimit`)

`VMContext::stackLimit` is the lowest address generated code may let its own
frame reach on the executing host thread; `0` means no limit is installed. The
core computes it when the instance is created (`WASM::installStackLimit`,
`WasmBase/WasmStack.hpp`) from the instantiating thread's real stack bounds,
keeping `kDefaultStackReserveBytes` (512 KiB) back from the guard page. An
instance therefore executes on the thread that created it — the same
one-call-chain-per-host-thread model as the tail-call state
([`SAFETY.md`](SAFETY.md) §2.7).

Every backend has to honour one rule: **on entry to every compiled Wasm
function, before its body runs, call `wasm_stack_check(VMContext*)`.** The
helper probes the current stack pointer and raises
`TrapKind::CallStackExhausted` ("call stack exhausted") when it is below the
limit. That is what turns unbounded Wasm recursion into a recoverable trap
instead of a host `SIGSEGV`, so the embedder's `TrapScope` sees it
(`WasmBase/WasmTrap.hpp`).

The frame is allocated by the backend's prologue *before* that check can run, so
one frame must stay well below the reserve. The core enforces this independently
of the backend: `PreparedFunctionStack::prepare` rejects a function whose
parameters and declared locals would need more than `kMaxFunctionLocalBytes`
(64 KiB) of stack storage (`WasmBase/WasmSafety.hpp`). A backend whose calling
convention spills much more than the declared locals has to account for that
difference itself.

### Linear memory ownership (`Store`)

Linear memories are **not** owned by `ModuleInstance`. They are allocated by
the single global `Store` (`WASM::Store::global()`) and referenced through the
borrowed `LinearMemory*` entries in `VMContext::memories`. Because the Store
keeps every memory at a stable address, those pointers stay valid even as more
memories are created by later instantiations.

Two modules that import the same `(module, field)` memory name resolve to the
**same** `LinearMemory*`; that is what "shared memory" means here (atomics are
not involved). The import/export name mapping lives in
`RegistryImportResolver`, which stores borrowed pointers and never owns storage.
The Store owns the storage and provides `growMemory` (recovering the owning
`StoreOwnedMemory` from the back-pointer in `LinearMemory::hostData`).

Unbounded shared memories reserve address space up front (`SharedLinearMemory`);
ordinary memories use a growable buffer (`PrivateLinearMemory`). Multi-memory
is fully wired: memory instructions carry a `MemIdx`, and the JIT indexes
`VMContext::memories` at compile time.

### Other store-owned entities

The same pattern extends to every runtime entity. The Store now also owns:

- **The runtime type registry** (`TypeRegistry`, `WasmTypeRegistry.hpp`). The
  registry owns the compact canonical records in `TypeStore`; a parsed module
  keeps only its local→`TypeId` mapping (`Module::typeIds`) and exposes
  `TypeStore` views through `Module::typeView`. Type indices remain module-local.
  The parser's `Subtype` vector is temporary and is discarded after interning,
  so the Store is the sole owner of the type definitions.
- **Tables** (`StoreOwnedTable` / `TableInstance`, `WasmTable.hpp`). The store
  owns each slot array (`Callable**`); `ModuleInstance` borrows one
  `TableInstance*` per declared table into `internals.tableRefs`, which is
  exactly what `VMContext::tables` points at. The table index is a compile-time
  constant, so the JIT folds it into the load offset and reads `base`/`size`
  straight from the `TableInstance`. Growth goes through `Store::growTable`
  (recovering the owning `StoreOwnedTable` from `TableInstance::hostData`) and
  updates the view in place, so `ctx.tables` never needs re-syncing. All table
  instructions (`table.get/set/size/grow/fill/copy/init`, `call_indirect`) are
  now multi-table.

  **Table imports are supported.** `resolveImports()` resolves each
  `ImportTable` through `ImportResolver::resolveTable` and prepends the returned
  `ImportedTable` (a borrowed `TableInstance*`) to `tableRefs`, before the
  module's own tables — exactly the Wasm table index space. The resolver also
  receives the importing module's declared table type (plus the canonical
  `TypeId` of a concrete element type) and rejects a provided table whose
  element type is not a subtype or whose limits/index type/shared-ness do not
  match (`docs/proposals/GC.md` R1). Because that
  `TableInstance*` is the *same* store-owned view the exporting module holds,
  two modules importing the same `(module, field)` share one table: growth and
  writes by either are visible to the other.

The remaining entity kinds are **pre-declared** in `Store` (`StoreOwnedGlobal`,
`StoreOwnedTag`, `StoreOwnedFunction`, `StoreOwnedElementSegment`,
`StoreOwnedDataSegment`, and the `GlobalId` / `TagId` / `FunctionId` /
`SegmentId` aliases) so the ownership boundary is explicit and the migration is
mechanical once each gains runtime behaviour. The GC heap is no longer
deferred: `Store::gc()` owns a `GcHeap` (mmap-backed regions), and each instance
allocates from it through a `RegionSet` handle that teardown releases in bulk.
GC objects follow the ownership model of `proposals/GC.md` RC-1/RC-1b: a fresh
object is unowned, an owning slot retains it, and the embedder triggers
reclamation with `ModuleInstance::gcCollect()` at a safepoint. A reference handed
out to the embedder is a **borrow** — do not hold it across a `gcCollect()`
unless you have retained it.

### Exceptions (`WasmBase/WasmExn.hpp`)

An uncaught WebAssembly exception is **not** a trap: the invocation returns
normally with the exception left in a process-global *pending* slot (the engine
is single-agent, so one slot is correct). Traps keep their `TrapScope`
`longjmp` path and are never catchable.

| Entry point | Purpose |
|---|---|
| `void* wasm_exn_pending()` | The pending exception payload, or `nullptr`. |
| `void* wasm_exn_take_pending()` | The same value, clearing the slot (what the spec harness uses). |
| `void wasm_exn_clear_pending()` | Drop it without inspecting. |
| `int32_t wasm_exn_tag_id(const void* payload)` | The thrown tag's process-wide `TagId` (compare with `ModuleInstance::tagIdForIndex`). |

An `exnref` is a pointer to a Store-owned object (`{TagId, payload}`); the
object lives until `Store` teardown, so an exception stored in a global stays
valid. A caught exception is cleared by the generated catch path before it
branches, so `wasm_exn_pending()` is non-null only for an exception the embedder
must handle. `ModuleInstance::tagIds()`/`tagIdForIndex()` expose the module's
tag index space (imports first).

### `Callable` (`WasmBase/WasmValue.hpp`)

Universal function reference stored in tables and import storage.

| Field      | Type          | Purpose                                                   |
|------------|---------------|-----------------------------------------------------------|
| `fnPtr`    | `void*`       | Function pointer (signature: `ret fn(VMContext*, ...)`)   |
| `context`  | `VMContext*`  | `nullptr` → JIT passes caller's ctx; non-null → baked constant |
| `localTypeIdx` | `uint32_t` | Module-local type index in the defining module        |
| `typeId`       | `TypeId`   | Canonical process-wide type identity; `kNone` for native imports. Used for cross-module type matching (`call_indirect`, `call_ref`, function imports). |

## Relevant Source Files

| File | What |
|------|------|
| `WasmBase/WasmVMContext.hpp` | VMContext struct |
| `WasmBase/WasmHostBridge.hpp` | Optional host boundary (reference + lifecycle callbacks) |
| `WasmBase/WasmTrap.hpp` / `WasmBase/WasmTrap.cpp` | `TrapScope` and the `longjmp` trap path; the cleanup constraints and candidate Go-style stack are in [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) |
| `WasmBase/WasmValue.hpp` | Callable struct and `callCallable` helper |
| `WasmBase/WasmExn.hpp` | Exception objects, the pending-exception slot and the tag-id accessor |
| `LibJit/LibjitOpcodeDispatcher.cpp` | `dispatchCall` (line ~1027), `dispatchCallIndirect` (~1061), `dispatchCallThroughCallable` (~612) |
| `LibJit/LibjitModuleCompiler.cpp` | `declareFunctions` sets internal `Callable::context` |
| `WasmBase/WasmModuleInstance.cpp` | `resolveImports` stores resolved `Callable`s |
| `Test/main.cpp` | `native_debug` demonstrates native import pattern; host-bridge tests use no JS engine |