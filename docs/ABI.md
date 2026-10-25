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

> **Status:** all three stages are in, but **R2 and R3 reached their goal by a
different route than the "Decision" below proposed.** The engine kept one
pointer-sized word per reference value and table slot and tells classes apart by
*provenance* (Store-registered callable, GC-heap header, i31 low bit, otherwise a
host reference) instead of by tag bits. No tag word was introduced; the `Ref` /
tag table below is the design that was considered and **not** taken, kept here
because the *goals* it served (generic reference storage, extern identity) did
have to be met.

Before R1 the engine used a flat `Value` array for globals. R1 replaced that half
with cells; R2's goal — a reference of any type surviving `table.set`/`table.get`
— was closed by treating the existing pointer word as generic storage (FR-6,
EX-6), and R3's goal — `externref` identity and a real `extern` hierarchy — by
boxing on the GC heap at `extern.convert_any` (GC-8/RT-1).

### Today

| Value / slot | Encoding | Consequence |
| --- | --- | --- |
| `funcref` | `Callable*`, recognised by `Store::callableAt()` | a callable from *another* module is still recognised (GC.md G2) |
| `externref` | opaque `void*` | identity is pointer identity; a host pointer passes through every conversion unchanged |
| `exnref` | exception-object pointer | survives a table round-trip through the same generic slot (EX-6) |
| GC ref | Store-owned object pointer, canonical type id in `GcHeader` | the type id is what `ref.test`/`ref.cast` read; a struct also carries the optional Custom Descriptors RTT reference at `GcHeader::kDescriptorOffset` |
| `i31ref` | `(value << 1)` with the low bit set | also the reason a real pointer can never look like an i31 |
| extern box | GC object with the reserved type id `GcHeader::kExternBoxTypeId` | produced by `extern.convert_any`, unwrapped by `any.convert_extern`, released by the GC walker with its inner reference |
| table slot | one pointer-sized word (`TableInstance::base` is `Callable**`) | the funcref view is just the ABI; any reference fits |
| global | a `Value*` cell per index; `VMContext::globals` is `Value**` | imported cells alias the exporter |
| exported global | the exporter's cell is published | a later `global.set` is visible to importers (and vice versa) |

Classification is therefore *by provenance*, in one place
(`ModuleInstance::refMatchesHeapType`): a registered callable is a `func`, a
GC-heap object carries its canonical type id, an i31 is an i31, and anything
left is treated as an opaque host reference, which matches only the top abstract
types. That is why `ref.test externref` is true for a pointer the engine has
never seen — which is exactly what the harness relies on for `(ref.extern N)`.

### Considered and not taken: a tagged reference word

The original decision was **one 64-bit tagged reference word** for every
reference value and table slot, plus global cells, with the tag in the low three
bits (all heap pointers are 8-byte aligned):

| Tags | Payload |
| --- | --- |
| `0` `Func` | `Callable*` |
| `1` `Extern` | `ExternBox*` (Store-owned) |
| `2` `Gc` | GC object pointer |
| `3` `Exn` | exception-object pointer |
| `4` `I31` | signed 31-bit value in bits 3…33 |
| `5`–`7` | reserved |
| all-zero word | null (the only null representation) |

It was dropped because provenance classification answers the same questions
(`ref.test`, `ref.cast`, `ref.eq`, `call_indirect`) without touching a single
slot layout, and because the tag word would have rewritten every call argument,
table slot and global for a hierarchy the engine can already name. What was kept
from the design:

- the **global cell** half (R1), including imported cells aliasing the exporter;
  all `global.get`/`global.set` lowering funnels through `globalSlotAddress`, so
  the backend loads the cell pointer and then the cell;
- the **extern box** idea (R3), reimplemented as a reserved GC object rather than
  a separate `ExternBox { host, identity }` — which also means the GC walker
  releases the inner reference for free;
- **box at the conversion, not at the boundary**: `extern.convert_any` boxes
  internal references, `any.convert_extern` unboxes, and host pointers are never
  wrapped, so their identity survives.

The harness maps `(ref.extern N)` to a stable synthetic pointer derived from `N`
(`Test/WastScript.cpp`), which is enough for `ref.eq` and `ref.test externref`;
it is an identity *mapping*, not a host object model (the engine has none, by
design — see `HostBridge` below for the embedder-supplied alternative).

### Migration

| Stage | Work | Unblocks | Acceptance |
| --- | --- | --- | --- |
| **R1 — Globals as cells** ✅ | Done: `VMContext::globals` is `Value**`; `Internals::globalRefs` holds one cell per index (imported cells are the exporter's, defined cells point into `globalsStorage`); `ImportRegistrar::registerGlobal` publishes a `Value*` + `GlobalType` and `resolveGlobal` returns the cell; `registerExports` publishes the exporter's cell; `globalSlotAddress` loads it; generated code dereferences it. | MG2, MG3, MG4 | Done: `global` **77/0**; `shared_global_a/b` prove writes cross both ways; `shared_global_bad` is rejected. |
| **R2 — Generic reference slots** ✅ (different route) | Done without tags: the table slot stays one pointer-sized word behind the funcref `Callable**` view, so any reference survives `table.set`/`table.get`; `wasm_ref_store` is the single write barrier; `ref.test`/`ref.cast` classify by provenance. | FR6, EX5, GC R4, CM8 | Done: `ref_test` **71/0**, `table_fill` **36/0/9**, `array_new_data` **28/0/0**; the `exnref` table round-trip is covered by `exnref_table.wat`. |
| **R3 — Extern identity** ✅ (different route) | Done: internal i31/struct/array references are boxed by `extern.convert_any` in a reserved GC object (`GcHeader::kExternBoxTypeId`); `any.convert_extern` unwraps; host pointers stay opaque. The harness derives `(ref.extern N)` identity as a stable synthetic pointer. | GC G3/D5, host interop | Done: `ref_test` **71/0** and `extern` **18/0**, including the `ref_test_extern` cases that motivated R3. |

**Rejected alternatives:** a 16-byte `{payload, tag}` reference (doubles the size
of every call argument, table slot and global), and NaN-boxing (the value stack
also carries `f64`, so payloads would collide). Neither is worth it for a
single-threaded engine whose table indirection is already the hot path — and a
low-bit tag turned out not to be needed either.

## Embedder Guide

### Native (host) function imports

Register with `context = nullptr`.  The JIT will pass the caller's `VMContext*`
at compile time, giving the native function access to the calling module's
linear memory, globals, table, and `hostData`:

```cpp
// Registration (before instantiation). `localTypeIdx` is the index of the
// import's type in the *importing* module; `typeId` may be left as kNone.
resolver.registerFunction("env", "my_func", WASM::Callable{
    .fnPtr        = reinterpret_cast<void*>(my_native_func),
    .context      = nullptr,   // signals: use caller's VMContext
    .localTypeIdx = typeIdx
});

// After instantiation, attach host state:
auto instance = compiler.instantiate(module, resolver);
instance->context()->hostData = &myAppState;
```

**Native function signature:**

```cpp
void my_native_func(WASM::VMContext* vm, int32_t arg1, double arg2) {
    // vm is the CALLER's VMContext — access everything the module sees:
    //   vm->memories    — LinearMemory* array, imports first then locals
    //   vm->memoryCount — number of entries in vm->memories
    //   vm->globals     — one Value* cell per global index
    //   vm->tables      — TableInstance** array, same index space as Wasm
    //   vm->tableCount  — number of entries in vm->tables
    //   vm->hostData    — embedder state (set after instantiation)
}
```

#### Writing an import: exceptions, traps and RAII

An import is called **directly by generated code**, with no engine frame in
between. The generated caller has no unwind table, so an exception escaping the
import would have to unwind a frame the platform unwinder cannot describe —
i.e. undefined behaviour, in practice `std::terminate`. The contract is:

| Inside the import | Allowed? | What happens |
|---|---|---|
| `std::unique_ptr`, `std::string`, `std::vector`, `std::lock_guard`, … | **yes** | the body's own frames are ordinary C++ and unwind normally, as long as the exception stops inside the import |
| `throw` / a library call that throws | **only if the import is wrapped** (below) | the wrapper catches it and reports a pending trap |
| `WASM::trap(kind, msg)` | **only if wrapped**; otherwise abort with a diagnostic | wrapped: the wrapper re-reports the trap and returns a benign value |
| `WASM::reportTrap(kind, msg)` + returning a benign value | **always** | the caller checks the pending slot and returns |
| calling back into Wasm (`WASM::callCallable`) | yes | a trap there stays *pending*: the boundary does not throw into the import's caller |
| letting an exception reach the generated frame | **never** | undefined behaviour — use `nativeImport` or `reportTrap` |

`WasmBase/WasmNativeImport.hpp` implements the wrapper as a template, so the
import keeps its exact Wasm signature and nothing is allocated:

```cpp
#include "WasmBase/WasmNativeImport.hpp"

void host_log(WASM::VMContext* vm, int32_t value) {
    auto* state = static_cast<MyState*>(vm->hostData);
    if (state == nullptr)
        WASM::trap(WASM::TrapKind::Internal, "host_log: no state");
    std::string line = "value: " + std::to_string(value);   // RAII is fine
    state->lines.push_back(std::move(line));                // may throw
}

resolver.registerFunction("env", "log", WASM::nativeImport<&host_log>());
```

The wrapper holds a `WASM::NativeImportScope` while the body runs and converts
anything that escapes it into the engine's pending trap:

| Thrown | Reported as |
|---|---|
| `WASM::Trap` (from `WASM::trap()`) | the same kind and message |
| `std::bad_alloc` | `TrapKind::OutOfMemory` |
| any other `std::exception` | `TrapKind::Internal`, message `what()` (copied into the slot) |
| anything else | `TrapKind::Internal`, "native import threw a non-standard exception" |

The wrapper then returns the zero of the result type; the generated caller sees
the pending slot before it looks at the value. `WASM::Trap`'s message is copied
into the engine's trap slot, so `what()` may point at the exception object's own
storage.

Notes and limits:

- The template argument is the function itself, so two imports with the same
  signature get distinct trampolines. A **capturing callable** cannot be a
  non-type template argument; pass per-instance state through
  `VMContext::hostData` (set after `instantiate`, or by
  `HostBridge::instanceCreated`) or use a captureless lambda.
- Imports may run **during instantiation** (start function, element/data
  initialisation), so `hostData` may still be null. Check it, as `host_log`
  does.
- A wrapped import that returns normally is byte-for-byte the raw ABI: same
  arguments, same result, no overhead beyond the `try` block (which costs
  nothing on the non-throwing path on the usual ABIs).
- Without the wrapper, a `throw` reaches the generated frame. That is the one
  remaining "never do this" in the import contract; `WASM::trap()` detects the
  situation and aborts with a diagnostic instead of letting the unwinder fail.

### External host adapter (optional `HostBridge`)

The engine has no JavaScript engine, host-language runtime, or `JSValue` type.
An embedder that wants to bridge `externref` values to JavaScript, Lua, Python or
another host runtime installs an optional `WASM::HostBridge` before
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
the current trap path, the cleanup options and the per-call arena discussion are
in [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §11.

### Cross-module Wasm function imports

Register with `context` pointing to the **callee module instance's** `VMContext`.
The JIT will burn this address into the machine code at compile time:

```cpp
// Module A imports a function defined by module B:
auto instanceB = compiler.instantiate(moduleB, importsB);
std::optional<WASM::Callable> exported = instanceB->exportedFunction("some_func");

// In module A's import resolver: register the callee's own Callable. It already
// carries the callee's `localTypeIdx` (in *its* module), its canonical `typeId`
// and, for a compiled function, the raw entry point the driver calls.
resolver.registerFunction("B", "some_func", *exported);
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
helper probes the current stack pointer and *reports*
`TrapKind::CallStackExhausted` ("call stack exhausted") when it is below the
limit; the backend then checks the pending slot and returns. That is what turns
unbounded Wasm recursion into a recoverable trap instead of a host `SIGSEGV`,
and the embedder sees a `WASM::Trap` exception (`WasmBase/WasmTrap.hpp`).

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

A GC **struct** header is now 16 bytes:
`[u32 TypeId][u32 refcount][void* descriptor]` (`GcHeader::kHeaderBytes`); the
descriptor slot is null for an ordinary struct and holds a retained RTT/descriptor
reference for `struct.new_desc*`. Arrays keep the 12-byte
`[u32 TypeId][u32 refcount][u32 length]` header. These offsets are an internal
ABI between `WasmBase` and a backend; an embedder must not hard-code the old
8-byte struct layout.

### Module metadata descriptors (`WasmBase/WasmModule.hpp`)

`Module::importDescriptors()` and `Module::exportDescriptors()` return the
read-only metadata view used by JS Types and other embedders:

```cpp
struct ExternTypeDescriptor {
    ExternalKind kind;
    TypeId typeId;          // function/tag/type where applicable
    bool isExact;           // exact function import (`0x20`)
    GlobalType globalType;  // Global
    TableType tableType;    // Table
    MemoryType memoryType;  // Memory
    uint8_t tagAttribute;   // Tag
};

struct ImportDescriptor { std::string moduleName; std::string fieldName;
                          ExternTypeDescriptor type; };
struct ExportDescriptor { std::string name; ExternTypeDescriptor type; };
```

Import descriptors follow **import-section order** (`Module::importOrder`), not
per-kind grouping. Names are copied; `typeId` is the canonical process-wide
identity and remains the authority for function/tag/type matching. Globals,
tables and memories carry their structural declaration by value. The API is a
view over the decoded module and does not expose parser scratch storage.

This is the **JS Types metadata descriptor API**, not the core Custom
Descriptors proposal (`descriptor`/`describes`, exact types and RTT), which is
still not implemented.

### Exceptions (`WasmBase/WasmExn.hpp`)

An uncaught WebAssembly exception is **not** a trap: the invocation returns
normally with the exception left in the execution state's *pending* slot (per
agent, like the trap slot). Traps take the same shape — a pending status that
unwinds by returning — but are never catchable by `try_table` and win over a
pending exception.

| Entry point | Purpose |
|---|---|
| `void** wasm_exn_slot()` | The calling thread's pending-exception slot; generated code resolves it once per function and loads through it. |
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

### Continuations (`WasmBase/WasmContinuation.hpp`)

A continuation reference is a `void*` at the ABI (the same lowering as any
reference). A host normally receives one from an exported function whose result
is `(ref cont)`, or from `wasm_cont_resume`'s suspension result. The public
`HostContinuation` handle owns one reference and exposes:

- `HostContinuation(ref, takeOwnership)` — wrap a value; `takeOwnership=false`
  retains an extra count;
- `resume(arg, &result)` — consume the handle and block until the continuation
  completes or suspends; a suspension returns the replacement continuation in
  `result.continuation` (wrap it with `takeOwnership=true`);
- `HostContinuation::suspendCurrent(tag, value, &resumed)` — the native-import
  entry point for JSPI-style yielding. It returns 1 when resumed; 0 means there
  is no current continuation or it was cancelled, with the trap slot explaining
  why.

`ContinuationResumeResult` carries `status`, `tagId`, `value` and the
replacement handle. A is the default provider (serial; may migrate between host
threads); B is the opt-in host-thread fallback. Neither is an agent API: calls
are serial and true multi-agent execution remains deferred.

This header is only the Wasm-side JSPI substrate. The JavaScript API binding,
Promise object, microtask/event loop and JS/Wasm value conversion are the
embedder's responsibility; the full contract is in
[`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) §4.2.2. In
particular, `onSuspendedEx`/`onCompletedEx`/`onRejected` callbacks are
synchronous with `step()`, borrowed buffers are valid only during the callback,
and the host must schedule the next `step()` itself.

#### Embedder scheduling contract (`HostContinuationScheduler`)

The scheduler is opt-in and engine-agnostic. It is a serial queue of resume
tasks; `step()` resumes one, `run()` drains the queue. It has a legacy scalar
task and a wide task:

```c
bool submit(void* continuation, int32_t arg, bool takeOwnership = true);
bool submitEx(void* continuation, const ContinuationValue* args,
              uint32_t argCount, const uint8_t* argKinds,
              uint32_t resultCapacity = 0, uint32_t tagCapacity = 0,
              bool takeOwnership = true);
bool submitThrow(void* continuation, void* exception,
                 bool takeOwnership = true);
```

`submitEx` carries any `ContinuationValueKind` payload (i32/i64/f32/f64/ref/
v128); the completion result capacity defaults to the target's result count and
the tag buffer defaults to `kDefaultValueCapacity` (64 slots). Reference
arguments are retained by the scheduler for the task and released after
`step`. `submitThrow` is backed by `wasm_cont_resume_throw_ex`; the exception is
borrowed until the task runs.

The hooks are:

- `onSuspended(userData, continuation, tagId, value)` and
  `onCompleted(userData, status)` — legacy scalar forms;
- `onSuspendedEx(userData, continuation, tagId, values, valueCount, kinds)` —
  wide suspension; values/kinds are borrowed for the callback and ownership of
  the replacement continuation is transferred;
- `onCompletedEx(userData, status, results, resultCount, kinds)` — wide
  completion, including reference/i64/f32/f64/v128 results;
- `onRejected(userData, status, exception)` — an uncaught exception
  (`ContinuationStatus::Exception`); the exception payload is borrowed.

If a wide hook is not installed, the scheduler falls back to the scalar hook
(or releases the replacement handle when neither exists). This is how a
JavaScript (or Lua/Python) adapter connects without linking its engine into
`WasmBase/`: the adapter supplies the hooks and its own event loop. See
[`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) §2.3.1.

#### Wide value ABI

The generated-code path passes payloads through fixed 16-byte slots
(`WASM::ContinuationValue`, `kContinuationValueStride`). `ContinuationValueKind`
describes each slot:

| Kind | Payload |
|---|---|
| `I32` | 32-bit integer in the first word |
| `I64` | 64-bit integer in the first word |
| `F32` | IEEE-754 f32 bits in the first word |
| `F64` | IEEE-754 f64 bits in the first word |
| `Ref` | `void*` reference in the first word |
| `V128` | 16 bytes across both words |

The backend supplies `ContinuationInvokeFn`, which calls the target using its
real C signature from the kind arrays (LibJIT uses `jit_apply`; another backend
can use its own thunk). The core never interprets scalar payloads.

The generated-code entry points are:

```c
void* wasm_cont_new_ex(callable, context, invoke, paramCount, resultCount,
                       paramKinds, resultKinds);
void* wasm_cont_bind_ex(continuation, values, count);
void  wasm_cont_resume_ex(continuation, args, argCount,
                          results, resultCapacity, tagValues, tagCapacity,
                          result);
void  wasm_cont_resume_throw_ex(continuation, exception,
                                results, resultCapacity,
                                tagValues, tagCapacity, result);
int   wasm_cont_suspend_ex(tagId, args, argCount, argKinds,
                           results, resultCapacity, resultCount, resultKinds);
void  wasm_cont_switch(continuation, args, argCount,
                       results, resultCapacity, result);
void  wasm_cont_add_root(ref);
void  wasm_cont_drop_values(context, kinds, count, values);
```

`ContinuationResumeResult` carries `resultCount`/`resultValues` for a normal
completion, `tagValueCount`/`tagValues`/`tagKinds` for a suspension, and the
replacement `continuation`. The caller owns the buffers; the runtime moves
reference values into them. An unhandled suspension is dropped through
`wasm_cont_drop_values`, so a reference payload is not leaked.

`wasm_cont_switch` appends the calling continuation to the target arguments and
returns the caller's continuation parameters when the peer switches back. Both
providers implement the symmetric handoff (`ucontext` handoff on NativeStacks,
condition-variable handoff on HostThreads); cross-provider switch is rejected.
A suspended stack's pointer-typed locals/operands are registered through
`wasm_cont_add_root` for a tracing collector.

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
| `WasmBase/WasmTrap.hpp` / `WasmBase/WasmTrap.cpp` | The pending-trap protocol (`reportTrap`, `wasm_trap_slot`, `WASM::Trap`) and the deferred-cleanup stack; the design and its limits are in [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) |
| `WasmBase/WasmNativeImport.hpp` | `WASM::nativeImport<&fn>()`: the typed trampoline that makes `throw`/`WASM::trap()` safe inside a host import ("Writing an import" above) |
| `WasmBase/WasmValue.hpp` | Callable struct and `callCallable` helper |
| `WasmBase/WasmExn.hpp` | Exception objects, the pending-exception slot and the tag-id accessor |
| `LibJit/LibjitOpcodeDispatcher.cpp` | `dispatchCall` (line ~1027), `dispatchCallIndirect` (~1061), `dispatchCallThroughCallable` (~612) |
| `LibJit/LibjitModuleCompiler.cpp` | `declareFunctions` sets internal `Callable::context` |
| `WasmBase/WasmModuleInstance.cpp` | `resolveImports` stores resolved `Callable`s |
| `Test/main.cpp` | `native_debug` demonstrates native import pattern; host-bridge tests use no JS engine |