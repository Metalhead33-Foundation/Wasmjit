# `Asmjit/` — provisional AsmJit integration

This directory now contains an initial, deliberately limited WebAssembly backend.
It implements the four `ModuleInstantiator` hooks and can compile a small
straight-line i32 subset. It is not feature-complete and must not be treated as
a drop-in replacement for the existing backend.

## What exists

- `AsmjitContext` owns an `asmjit::JitRuntime` and the executable allocations
  created through it.
- `compileReturn42()` emits a tiny x86/x64 function, installs it into executable
  memory, and returns its entry point.
- `compileLaneWiseI32x4()` uses AsmJit's function compiler and register
  allocator to emit a host-ABI function that adds or multiplies four 32-bit
  lanes. The inputs and output are ordinary pointers; the implementation does
  not assume a particular host argument-register convention.
- `TypeTranslator` maps Wasm integer, floating-point, reference, and V128
  values to AsmJit `TypeId`s and builds C-declaration function signatures. It
  prepends `VMContext*`, maps V128 to AsmJit's 128-bit vector type, and uses a
  trailing caller-owned result buffer for multi-value results.
- `ModuleCompiler` implements `translateTypes`, `declareFunctions`,
  `compileFunctions`, and `callStartFunction`. It declares all internal
  function records before compiling bodies, preserving the two-pass
  instantiation contract. Each generated function emits the required
  `wasm_stack_check(VMContext*)` prologue call.
- The first body compiler supports i32 parameters/results/locals, `i32.const`,
  `local.get`, `local.set`, `local.tee`, `drop`, `i32.add`, `i32.sub`, `i32.mul`,
  explicit `return`, and implicit function return. Unsupported instructions
  fail explicitly through the base dispatcher; they are never silently
  ignored. Structured control flow, calls, memory, traps beyond the stack
  guard, and non-i32 function values are not implemented yet.
- `Test/TestBackend.cpp` exposes an `AsmJitSession` through
  `WASMJIT_BACKEND=asmjit`, including i32 invocation marshalling, aggregate
  layout calculation, and the existing lane-wise SIMD diagnostic interface.
- `Test/AsmjitModuleCompilerTests.cpp` instantiates a WAT fixture and executes
  exported add/sub/mul/local functions, all expected to return 42.
- Packed `i8`/`i16` storage retains its narrow 8-/16-bit representation.
  Arithmetic lowering must explicitly extend those values to `i32` before
  applying Wasm integer operations; the storage translator does not silently
  widen their in-memory layout. `compilePackedI8Extend()` and
  `compilePackedI16Extend()` emit signed and unsigned extension probes using
  `movsx`/`movzx`, with tests for high-bit-set inputs.
- `compileV128I32x4Add()` emits a real XMM-backed packed addition using
  `movdqu` and `paddd`, with a test covering lane wraparound.
- `compileV128Bitwise()` emits XMM-backed V128 AND, OR, and XOR operations
  using `pand`, `por`, and `pxor`. Tests compare all 16 output bytes against
  the corresponding scalar bitwise operation.
- `Test/AsmjitTypeTranslatorTests.cpp` covers scalar signatures, opaque
  references, V128-by-value signatures, packed storage widths, multi-value
  result-buffer placement, and rejection of packed function signatures (which
  are invalid Wasm).
- The context releases all generated entry points at destruction. If tracking
  an allocation fails after executable memory is allocated, it releases that
  allocation before propagating the host-side exception.
- `Test/AsmjitBackendTests.cpp` checks that emitted machine code executes,
  returns the expected scalar value, and performs lane-wise, vector, and
  packed-storage operations.
- The dependency is confined to this subproject. `WasmBase/` remains
  backend-agnostic and does not include or link AsmJit.

The module compiler is an initial execution slice, not full Wasm ABI or semantic
coverage. In particular, the SIMD and packed-storage probes are still native
code-generation diagnostics; the module compiler does not yet lower Wasm SIMD
instructions or packed GC field operations.

## Build dependency

The qmake target consumes the `asmjit` pkg-config package:

```bash
pkg-config --modversion asmjit
```

Install a current AsmJit development package or build/install AsmJit from its
upstream repository so that `asmjit.pc` is visible through `PKG_CONFIG_PATH`.

## Next implementation stages

1. Add scalar comparisons, bitwise operations, shifts, conversions, and numeric edge cases.
2. Add structured control flow and direct/indirect calls while preserving two-pass declarations.
3. Add linear-memory access, globals, imports, and complete trap propagation.
4. Add reference/GC operations, exceptions, continuations, and Wasm SIMD lowering.
5. Meet every applicable item in
   [`docs/BACKEND_WORK_ORDER.md`](../docs/BACKEND_WORK_ORDER.md) before calling
   the backend complete.

The existing GNU LibJIT backend remains the only executing WebAssembly backend.
`Sljit/` is unchanged.
