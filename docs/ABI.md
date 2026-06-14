# WasmJit ABI: VMContext Passing Convention

## Overview

Every JIT-compiled Wasm function takes a `VMContext*` as its first argument (arg0). Which `VMContext*` gets passed depends on the call kind, and the choice has significant implications for native/host imports.

## The Decision: Option C

After analyzing several approaches, we chose **Option C**: the JIT always passes the **caller's** `VMContext*` for direct `call` instructions (both internal and imported), and uses `Callable::context` only for indirect calls (`call_indirect` and `call_ref`).

### Why?

Native/host functions (trampolines) imported into a Wasm module need access to the calling module's resources — linear memory (`memoryBase`), globals (`globals`), the function table (`table`), and host-specific state (`hostData`). If an embedder registers a native function like WASI's `fd_write`, that function must read from the calling module's memory. Passing the caller's `VMContext*` gives it all of that in one pointer.

### VMContext passed per call type

| Call instruction     | arg0 = ?                               | Rationale                                               |
|----------------------|----------------------------------------|---------------------------------------------------------|
| `call` (internal)    | Caller's `VMContext*`                  | Internal functions already share the same instance      |
| `call` (imported)    | **Caller's** `VMContext*` (Option C)   | Native imports see the caller's memory/globals/table    |
| `call_indirect`       | `Callable::context` (callee's context) | Cross-module — callee needs its own memory/globals      |
| `call_ref`            | `Callable::context` (callee's context) | Same as call_indirect — callee owns the referenced func |

### What this means for embedders

**Registering a native host function:**

```cpp
// Callable::context can be nullptr at registration time — the JIT ignores it
// for direct import calls. The native function receives the instance's
// VMContext* at call time.
resolver.registerFunction("env", "my_host_func", WASM::Callable{
    .fnPtr     = reinterpret_cast<void*>(my_host_func),
    .context   = nullptr,   // ignored for direct calls — JIT passes caller's ctx
    .typeIndex = typeIdx
});

// After instantiation, attach host state to the instance:
auto instance = compiler.instantiate(module, resolver);
instance->context()->hostData = &myHostState;
```

**Native function signature:**

```cpp
void my_host_func(WASM::VMContext* vm, int32_t arg1, double arg2) {
    // vm->memoryBase  — access Wasm linear memory
    // vm->globals     — access Wasm globals
    // vm->table       — access Wasm function table
    // vm->hostData    — access embedder-specific state
    uint8_t* heap = vm->memoryBase;
    // ...
}
```

### Future: Cross-module Wasm→Wasm calls

When module A imports a function from module B via `call`, the imported Callable's `context` field should point to module B's `VMContext`, but the JIT currently passes module A's context. For cross-module **direct** `call` imports, a trampoline is needed:

```
Module A: call $imported_from_B
         → JIT passes A's VMContext* as arg0
         → trampoline: receives A's ctx, then invokes B's function with B's ctx
```

This trampoline can be generated at instantiation time using the `Callable::context` field. This case is not yet implemented because the current codebase only supports native imports and single-module usage. The `Callable::context` field remains available for this purpose.

## Data Structure Details

### `VMContext` (`WasmBase/WasmVMContext.hpp`)

A plain-old-data struct passed as arg0 to every JIT-compiled function. Fields:

| Field              | Type          | Purpose                                                   |
|--------------------|---------------|-----------------------------------------------------------|
| `memoryBase`       | `uint8_t*`    | Linear memory base (offset 0 for cheap addressing)        |
| `memorySize`       | `uint64_t`    | Current memory size in bytes                              |
| `memoryMax`        | `uint64_t`    | Max memory size in bytes                                  |
| `module`           | `const Module*` | Type graph metadata for GC/reference checks             |
| `globals`          | `Value*`      | Flat array of global values, indexed by global index      |
| `table`            | `Callable**`  | Function table for `call_indirect`                        |
| `tableSize`        | `uint64_t`    | Current table size                                        |
| `tableMax`         | `uint64_t`    | Max table size                                            |
| `importedFunctions` | `Callable*`  | Imported function handles in import section order         |
| `importedFunctionCount` | `uint32_t` | Number of imported functions                            |
| `hostData`         | `void*`       | Opaque pointer for embedder state (WASI, callbacks, etc.) |

### `Callable` (`WasmBase/WasmValue.hpp`)

The universal function reference stored in tables and import storage:

| Field      | Type          | Purpose                                                   |
|------------|---------------|-----------------------------------------------------------|
| `fnPtr`    | `void*`       | Raw function pointer (signature: `ret fn(VMContext*, params...)`) |
| `context`  | `VMContext*`  | Used by `call_indirect`/`call_ref` to pass the callee's context; **not** used by direct `call` (see Option C above) |
| `typeIndex` | `uint32_t`   | For `call_indirect` runtime type checking                  |

## Relevant Source Files

- `WasmBase/WasmVMContext.hpp` — VMContext struct definition
- `WasmBase/WasmValue.hpp` — Callable struct (lines 32-36) and `callCallable` helper
- `LibJit/LibjitOpcodeDispatcher.cpp` — `dispatchCall` (line ~1014), `dispatchCallIndirect` (line ~1055), `dispatchCallThroughCallable` (line ~612)
- `WasmBase/WasmModuleInstance.cpp` — `resolveImports` stores `Callable`s into `importStorage`
- `Test/main.cpp` — `native_debug` test demonstrates the pattern
- `Test/helper.hpp` — `registerHostFunction` template