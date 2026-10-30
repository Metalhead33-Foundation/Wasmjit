# `Asmjit/` — provisional AsmJit integration

This directory contains an experimental, deliberately limited WebAssembly
backend. It implements the four `ModuleInstantiator` hooks and executes an
initial i32 subset. It is not feature-complete and must not be treated as a
drop-in replacement for GNU LibJIT.

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
  `compileFunctions`, and `callStartFunction`. It declares all internal function nodes before compiling bodies, preserving
the two-pass instantiation contract and enabling forward and mutually recursive
direct calls. Each generated function emits the required
  `wasm_stack_check(VMContext*)` prologue call.
- The body compiler currently supports i32 parameters/results/locals; constants,
  local access, `drop`; i32 globals; arithmetic (`add/sub/mul/div/rem`),
  `clz`/`ctz`/`popcnt`, sign extension; signed/unsigned comparisons; bitwise
  operations, shifts and rotations; void `block`/`loop`/`if`/`else`, `br`/`br_if`;
  explicit/implicit returns; direct calls (native imports, forward calls and
  mutual recursion) and typed `call_indirect`;
  and memory32 i32 loads/stores, including signed/unsigned 8/16-bit extensions,
  plus `memory.size`/`memory.grow`.
  Memory accesses use a checked helper that observes the shared-memory size
  accessor and reports out-of-bounds traps through the pending-trap protocol.
  Function nodes are declared in one CodeHolder before any bodies are emitted,
  so direct-call labels resolve across the entire module. Block result types, unreachable-stack polymorphism, tail-call integration,
  memory64, other value types,
  and most remaining instructions are still unsupported and fail explicitly.
- `Test/TestBackend.cpp` exposes an `AsmJitSession` through
  `WASMJIT_BACKEND=asmjit`, including i32 invocation marshalling, aggregate
  layout calculation, and the existing lane-wise SIMD diagnostic interface.
- `Test/AsmjitModuleCompilerTests.cpp` instantiates WAT fixtures and executes arithmetic, comparison, bitwise,
  control-flow, direct-call (including native imports and mutual recursion), and memory32
  cases, including signed/unsigned narrow loads and out-of-bounds traps.
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
  allocation before propagating the host-side exception. Module compilation
  emits a module into one CodeHolder and installs it only after every function
  body succeeds, so a failed module leaves no partially installed code.
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

1. Extend structured control flow to result-bearing blocks, full branch-stack semantics, and unreachable-stack polymorphism.
2. Add tail-call integration, cross-instance Wasm-import tests, and broader call ABI support.
3. Expand linear memory to the remaining load/store widths, memory64, and bulk-memory operations.
4. Add i64 arithmetic, conversions, globals, references/GC, exceptions, continuations, and Wasm SIMD lowering.
5. Meet every applicable item in
   [`docs/BACKEND_WORK_ORDER.md`](../docs/BACKEND_WORK_ORDER.md) before calling
   the backend complete.

GNU LibJIT remains the only full-featured WebAssembly backend; AsmJit is an experimental partial execution backend.
`Sljit/` is unchanged.
