# WasmJit ABI: VMContext Passing Convention

## Overview

Every JIT-compiled Wasm function takes a `VMContext*` as its first argument
(arg0).  Which `VMContext*` gets passed depends on the call kind and on the
value of `Callable::context`.

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
    //   vm->memoryBase  — linear memory
    //   vm->memorySize  — current memory size
    //   vm->globals     — global variables
    //   vm->table       — function table
    //   vm->hostData    — embedder state (set after instantiation)
}
```

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

| Field                  | Type            | Purpose                                                |
|------------------------|-----------------|--------------------------------------------------------|
| `memoryBase`           | `uint8_t*`      | Linear memory base (offset 0 for cheap addressing)     |
| `memorySize`           | `uint64_t`      | Current memory size in bytes                           |
| `memoryMax`            | `uint64_t`      | Max memory size in bytes                               |
| `module`               | `const Module*` | Type graph metadata for GC/reference checks            |
| `globals`              | `Value*`        | Flat array of global values                            |
| `table`                | `Callable**`    | Function table for `call_indirect`                     |
| `tableSize`            | `uint64_t`      | Current table size                                     |
| `tableMax`             | `uint64_t`      | Max table size                                         |
| `importedFunctions`    | `Callable*`     | Imported function handles (import section order)       |
| `importedFunctionCount`| `uint32_t`      | Number of imported functions                           |
| `hostData`             | `void*`         | Opaque pointer for embedder state                      |

### `Callable` (`WasmBase/WasmValue.hpp`)

Universal function reference stored in tables and import storage.

| Field      | Type          | Purpose                                                   |
|------------|---------------|-----------------------------------------------------------|
| `fnPtr`    | `void*`       | Function pointer (signature: `ret fn(VMContext*, ...)`)   |
| `context`  | `VMContext*`  | `nullptr` → JIT passes caller's ctx; non-null → baked constant |
| `typeIndex`| `uint32_t`   | For `call_indirect` runtime type checking                 |

## Relevant Source Files

| File | What |
|------|------|
| `WasmBase/WasmVMContext.hpp` | VMContext struct |
| `WasmBase/WasmValue.hpp` | Callable struct and `callCallable` helper |
| `LibJit/LibjitOpcodeDispatcher.cpp` | `dispatchCall` (line ~1027), `dispatchCallIndirect` (~1061), `dispatchCallThroughCallable` (~612) |
| `LibJit/LibjitModuleCompiler.cpp` | `declareFunctions` sets internal `Callable::context` |
| `WasmBase/WasmModuleInstance.cpp` | `resolveImports` stores resolved `Callable`s |
| `Test/main.cpp` | `native_debug` test demonstrates native import pattern |