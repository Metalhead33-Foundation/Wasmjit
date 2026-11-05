# `Asmjit/`

*Experimental AsmJit backend*

This directory contains an experimental, deliberately limited WebAssembly
backend. It implements the four `ModuleInstantiator` hooks and executes a
scalar `i32`/`i64`/`f32`/`f64` subset. It is not feature-complete and must not be treated as a
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
- The body compiler currently supports i32/i64/f32/f64 parameters, results, and
  locals; i32/i64 constants and local access; i32.wrap_i64, i64.extend_i32_s/u and i64.extend8/16/32_s; i64 arithmetic (`add/sub/mul/div/rem`),
  bitwise operations, shifts, rotations, comparisons, and `eqz`; i32 arithmetic,
  bit counts, sign extension, comparisons, bitwise operations, shifts, and rotations;
  scalar `f32`/`f64` constants, arithmetic, `min`/`max`, `abs`/`neg`/`sqrt`,
  rounding (`ceil`/`floor`/`trunc`/`nearest`), `copysign`, all comparisons, the
  checked and saturating integer conversions, `demote`/`promote`, and
  reinterpretation. A float travels through the value stack as its IEEE bit
  pattern in a Gp and materialises in an Xmm only at an arithmetic/compare/ABI
  boundary, so `select`, block transfer and locals are the same code as for
  integers;
  `drop`, untyped and typed scalar `select`; i32/i64/f32/f64 globals; void and single-scalar-result `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`,
  branches to the implicit function label, and explicit/implicit returns; direct calls
  (native imports, forward calls and mutual recursion), typed `call_indirect`,
  with scalar signatures including native and cross-instance Wasm imports;
  i32-only direct/indirect tail-call opcodes (currently lowered to call+return;
  true tail-call stack semantics and machine-level elimination remain outstanding);
  and memory32/memory64 i32/i64/f32/f64 loads/stores, including signed/unsigned narrow extensions,
  `memory.size`/`memory.grow` at both index widths, checked memory32 `memory.copy` (with overlap-safe `memmove` semantics), `memory.fill`, `memory.init`, `data.drop`, `table.init`, `elem.drop`, `table.size`, and checked overlap-safe `table.copy`.
  Memory accesses use a checked helper that observes the shared-memory size
  accessor and reports out-of-bounds traps through the pending-trap protocol.
  Function nodes are declared in one CodeHolder before any bodies are emitted,
  so direct-call labels resolve across the entire module. Result-bearing blocks
  currently support only one scalar result and no block parameters; `br_table`
  requires identical target stack heights/signatures. The `unreachable`
  instruction reports a trap and supplies a limited single-result placeholder,
  but general unreachable-stack polymorphism remains unsupported. Cross-instance scalar Wasm imports are covered by tests. Scalar globals are lowered; the first four `v128` slices (const/load/store, locals, `i32x4` lane access, the bitwise family, `any_true`, integer lane arithmetic, the comparison families, `all_true`, the shifts, saturating arithmetic, `min`/`max`, `avgr_u`, `abs`/`popcnt`, widening, `extmul`, `extadd_pairwise`, `dot`, both narrowing families, the `f32x4`/`f64x2` arithmetic/comparison/unary/conversion families, float lane access and the transport families (extending/splat/zero loads, lane loads and stores, `i8x16.swizzle`, `i8x16.shuffle`), through the shared helper ABI, and `ref.null`/`ref.is_null`/`ref.eq`/`ref.as_non_null`/`ref.func`/`br_on_null`) are lowered, while `v128` signatures (deferred by decision — see `docs/backends/V128_SIGNATURE_DECISION.md`), `ref.func`, `f16`, bulk memory on a memory64 and
  most remaining instructions are still unsupported and fail explicitly.
- `Test/TestBackend.cpp` exposes an `AsmJitSession` through
  `WASMJIT_BACKEND=asmjit`, including i32 invocation marshalling, aggregate
  layout calculation, and the existing lane-wise SIMD diagnostic interface.
- `Test/AsmjitModuleCompilerTests.cpp` instantiates WAT fixtures and executes i32/i64 arithmetic, comparison, bitwise and memory operations,
  control-flow, direct/indirect-call (including i32/i64 native and cross-instance Wasm imports, mutual recursion, and i32 tail-call opcodes), result-bearing control flow, `br_table`, unreachable traps, i32/i64 globals, and memory32
  cases, including signed/unsigned i32/i64 narrow loads, `select`, overlap-safe bulk copies,
  fills, passive data/element segment initialization/drop semantics, table sizing/copying, zero-length boundary operations, and out-of-bounds traps.
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
- The backend registers as `asmjit` through `AsmjitBackend.cpp`
  (`AsmjitBackend.cpp`, `WASMJIT_REGISTER_BACKEND`), so `WASM::Runtime::create("asmjit")`
  reaches it the same way the LibJIT and SLJIT backends are reached, and the
  keep-alive `BackendInstance` holds the shared `Context`. `ModuleCompiler`
  overrides `releaseInstance`, which returns the instance's one installed
  allocation through `Context::release` on `Runtime::unload`; a second instance
  of the same module keeps its own allocation and stays callable.
- `Test/AsmjitBackendTests.cpp` checks that emitted machine code executes,
  returns the expected scalar value, performs lane-wise, vector, and
  packed-storage operations, and that the registry reaches the backend by name.
- The dependency is confined to this subproject. `WasmBase/` remains
  backend-agnostic and does not include or link AsmJit.

The module compiler is an initial execution slice, not full Wasm ABI or semantic
coverage. It lowers the first Wasm SIMD slice (value transport, bitwise
operations and `i32x4` lane access) by calling the core's shared `wasm_v128_*`
helper ABI; the packed-storage probes are still native code-generation
diagnostics, and packed GC field operations are not lowered.

## Build dependency

AsmJit is consumed straight from the system installation: the sources include
`<asmjit/asmjit.h>` and `Asmjit.pro` links `-lasmjit`. On Gentoo, `equery f
asmjit` lists what that install provides — the headers under
`/usr/include/asmjit`, `libasmjit.so` under `/usr/lib64`, and CMake package
files under `/usr/lib64/cmake/asmjit`. It ships **no** `asmjit.pc`, so the
subproject does not use pkg-config: a `PKGCONFIG += asmjit` line fails
configure on such a host.

The sources target the installed AsmJit 1.21 API (`asmjit::Out<>` for output
parameters such as `invoke`'s `InvokeNode*`, and `ASMJIT_ARCH_X86` for the
target-architecture check).

## Next implementation stages

1. `ref.func`, then the remaining reference/exception work. `v128` in signatures
   is deferred by decision and must be revisited before the saga closes; SLJIT
   landed the same slices, so the shared
   `[sljit][asmjit][parity][simd]` fixture grows with both. See
   [`docs/backends/ASMJIT_SLJIT_P3_SIMD1.md`](../docs/backends/ASMJIT_SLJIT_P3_SIMD1.md)
   and [`docs/backends/ASMJIT_SLJIT_P3_SIMD2.md`](../docs/backends/ASMJIT_SLJIT_P3_SIMD2.md).
2. Extend structured control flow to general unreachable-stack polymorphism
   (SLJIT already covers block parameters and multi-value block results, so the
   two backends currently differ there).
3. References/GC, exceptions, continuations and packed GC field operations
   (P4/P5).
   When references/GC arrive, implement the ownership rule in
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) §6
   “Reference ownership”: a reference parameter is the caller's borrow, a
   reference local owns one count, and retaining a parameter at entry is what
   keeps the `local.set`/epilogue rules static (a per-instruction flag does not
   survive a loop). LibJIT's `emitReferenceParameterRetains()` is the worked
   example, and `F3d` in
   [`docs/languages/LF2_DEEP_F3_GC.md`](../docs/languages/LF2_DEEP_F3_GC.md)
   §7.1 is what skipping it costs.
4. Meet every applicable item in
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) before calling
   the backend complete.

GNU LibJIT remains the only full-featured WebAssembly backend. AsmJit and SLJIT
are the two partial execution backends and, since 2026-11-05, they execute the
same P0–P2 slice.
