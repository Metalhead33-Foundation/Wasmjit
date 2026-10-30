# `Asmjit/` — provisional AsmJit integration

This directory now contains the first executable-code-generation foundation for
an eventual AsmJit backend. It is deliberately **not yet a WebAssembly backend**:
it does not implement `ModuleInstantiator`, does not register a backend name,
and must not be selected to execute Wasm modules.

## What exists

- `AsmjitContext` owns an `asmjit::JitRuntime` and the executable allocations
  created through it.
- `compileReturn42()` emits a tiny x86/x64 function, installs it into executable
  memory, and returns its entry point.
- `compileLaneWiseI32x4()` uses AsmJit's function compiler and register
  allocator to emit a host-ABI function that adds or multiplies four 32-bit
  lanes. The inputs and output are ordinary pointers; the implementation does
  not assume a particular host argument-register convention.
- `TypeTranslator` maps Wasm integer, floating-point, and reference values to
  AsmJit `TypeId`s and builds C-declaration function signatures. It prepends
  `VMContext*`, uses indirect pointers for V128 parameters, and uses a trailing
  caller-owned result buffer for V128 results (AsmJit's documented
  `FuncSignature::TypeId` has no 128-bit vector entry). Multi-value results
  use the project's trailing result-buffer pointer ABI.
- Packed `i8`/`i16` storage retains its narrow 8-/16-bit representation.
  Arithmetic lowering must explicitly extend those values to `i32` before
  applying Wasm integer operations; the storage translator does not silently
  widen their in-memory layout.
- `compileV128I32x4Add()` emits a real XMM-backed packed addition using
  `movdqu` and `paddd`, with a test covering lane wraparound.
- `Test/AsmjitTypeTranslatorTests.cpp` covers scalar signatures, opaque
  references, V128 arguments/results, packed storage widths, multi-value
  result-buffer placement, and rejection of packed function signatures (which
  are invalid Wasm).
- The context releases all generated entry points at destruction. If tracking
  an allocation fails after executable memory is allocated, it releases that
  allocation before propagating the host-side exception.
- `Test/AsmjitBackendTests.cpp` checks that emitted machine code executes,
  returns the expected scalar value, and performs both lane-wise operations.
- The dependency is confined to this subproject. `WasmBase/` remains
  backend-agnostic and does not include or link AsmJit.

These are native code-generation probes and an initial ABI type mapping, not
evidence of Wasm instruction lowering, full Wasm ABI compatibility, trap
handling, GC integration, or Wasm SIMD instruction lowering. In particular,
the lane-wise function is a diagnostic primitive; it does not yet implement
`WASM::BackendLaneWiseOp`.

## Build dependency

The qmake target consumes the `asmjit` pkg-config package:

```bash
pkg-config --modversion asmjit
```

Install a current AsmJit development package or build/install AsmJit from its
upstream repository so that `asmjit.pc` is visible through `PKG_CONFIG_PATH`.

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
