# `Asmjit/` — provisional AsmJit integration

This directory now contains the first executable-code-generation foundation for
an eventual AsmJit backend. It is deliberately **not yet a WebAssembly backend**:
it does not implement `ModuleInstantiator`, does not register a backend name,
and must not be selected to execute Wasm modules.

## What exists

- `AsmjitContext` owns an `asmjit::JitRuntime` and the executable allocations
  created through it.
- `compileReturn42()` emits a tiny x86/x64 function, installs it into executable
  memory, and returns its entry point. The context releases all such entry points
  at destruction.
- `Test/AsmjitBackendTests.cpp` checks that emitted machine code executes and
  returns the expected value.
- The dependency is confined to this subproject. `WasmBase/` remains
  backend-agnostic and does not include or link AsmJit.

This is an integration probe: it verifies that the project can compile against
AsmJit, emit host machine code, allocate executable memory, call it, and release
it. It is not evidence of Wasm instruction coverage, ABI compatibility, trap
handling, GC integration, or SIMD lowering.

## Build dependency

The qmake target consumes the `asmjit` pkg-config package:

```bash
pkg-config --modversion asmjit
```

Install a current AsmJit development package or build/install AsmJit from its
upstream repository so that `asmjit.pc` is visible through `PKG_CONFIG_PATH`.
The upstream project recommends consuming its current source or package-manager
build rather than relying on old distribution snapshots.

## Next implementation stages

1. Add an AsmJit-owned module compiler and translate the core type/aggregate ABI.
2. Implement function declaration and a minimal scalar instruction subset.
3. Add a session façade, invocation marshalling, trap propagation, and backend
   selection in the test harness.
4. Expand instruction coverage incrementally, with tests for control flow,
   calls, memory, references, exceptions, continuations, and SIMD.
5. Meet every applicable item in
   [`docs/BACKEND_WORK_ORDER.md`](../docs/BACKEND_WORK_ORDER.md) before calling
   the backend complete.

The existing GNU LibJIT backend remains the only executing WebAssembly backend.
`Sljit/` is unchanged.
