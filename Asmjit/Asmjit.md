# AsmJit backend {#backend_asmjit}

*The third code generator: AsmJit registration, emitted surface and refusals*

This directory contains a native WebAssembly backend built on AsmJit. It
implements the four `ModuleInstantiator` hooks and executes the shared lowering
slice with AsmJit’s register allocator.

**Status (2026-11-10):** **production-ready within the declared envelope** —
`WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345` reports **22,533
assertions in 824 cases, green**, the same case set and assertion count as the
reference backend. The envelope is x86-64 Linux / SysV AMD64 with the external
AsmJit library available at build time, and the documented refusals are the same
three as SLJIT’s (see `Sljit/Sljit.md` "Known limitations"). See
[`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md) §13 for the verdict
and §8 for the open register and the defect ledger. It is still not a
*code-quality* drop-in replacement for GNU LibJIT.

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
- The body compiler covers the same lowering slice as LibJIT, including the
  Custom Descriptors family (`struct.new_desc`, `struct.new_default_desc`,
  `ref.get_desc`, `ref.cast_desc_eq(_null)`, `br_on_cast_desc_eq(_fail)`), whose
  descriptor operand is retained into the new object's header slot and released
  from the operand stack, and trap-site attribution: `unreachable` reports at
  its own instruction and a helper-raised trap is annotated with the calling
  function's index, offset and `$name` before the frame returns.
- The scalar core supports i32/i64/f32/f64 parameters, results, and
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
  `drop`, untyped and typed scalar `select`; i32/i64/f32/f64 globals; void and scalar/`v128`-result `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`,
  branches to the implicit function label, and explicit/implicit returns; direct calls
  (native imports, forward calls and mutual recursion), typed `call_indirect`,
  with scalar signatures including native and cross-instance Wasm imports;
  direct/indirect/reference tail-call opcodes with constant-stack coverage
  (`[asmjit]` and the generic suite);
  and memory32/memory64 i32/i64/f32/f64 loads/stores, including signed/unsigned narrow extensions,
  `memory.size`/`memory.grow` at both index widths, checked memory32 `memory.copy` (with overlap-safe `memmove` semantics), `memory.fill`, `memory.init`, `data.drop`, `table.init`, `elem.drop`, `table.size`, and checked overlap-safe `table.copy`.
  Memory accesses use a checked helper that observes the shared-memory size
  accessor and reports out-of-bounds traps through the pending-trap protocol.
  Function nodes are declared in one CodeHolder before any bodies are emitted,
  so direct-call labels resolve across the entire module. Result-bearing blocks
  support scalar and `v128` results and block parameters; `br_table` still
  requires identical target stack heights/signatures. The `unreachable`
  instruction reports a trap and supplies a limited single-result placeholder,
  but general unreachable-stack polymorphism remains unsupported. Cross-instance
  Wasm imports are covered by tests. Globals are lowered. The complete shared
  SIMD surface, the reference family, GC construction and access, the Custom
  Descriptors family, exceptions and continuations all execute; `v128`
  signatures are implemented (two `uint64_t` words, per
  [`docs/ABI.md`](../docs/ABI.md)),
  and the opt-in frame ABI is available on the same terms as the other two
  backends. What remains are the named refusals, not a missing instruction
  family; `f16` has no stable encoding.
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

The module compiler covers the same lowering slice as the other two backends and
runs the generic suite to completion. The remaining precision limits are the
ones recorded in [`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md)
§30 — notably reference ownership through a struct field or array element is a
borrow, not an owned count — and they are leaks, never wrong results.

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

## Known limitations and next steps

1. **Reference ownership is exact and measured** since 2026-11-11: a reference
   read out of a struct field or array element takes its own count, `select`
   releases the arm it did not pick, `drop` releases the stack’s count, and a
   native import’s reference result is cloned. `gc_ownership.wat` (73 assertions)
   and `native_ref_result_probe.wat` (13) assert the counts here, so `GAP-4` and
   `GAP-5` are closed.
2. **A frame-form tail trampoline.** `return_call_ref` above the register-form
   ceiling is a semantic call + return here
   ([`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md) §8, `GAP-2`).
3. **Portability.** Only x86-64 Linux / SysV AMD64 is exercised; the target
   matrix in [`docs/ABI.md`](../docs/ABI.md) keeps the
   other rows *provisional*.
4. **A wide `v128` indirect call in a module with a native import** is *accepted*
   since 2026-11-11 (`GAP-11`, `wide_v128_indirect_probe.wat`). `ref_test`’s
   `ref_test_extern` case answered `0` where the spec says `2`; that was the
   extern-box conversion replacing its operand without taking the stack’s count,
   and it is **fixed** here and in SLJIT (`GAP-15`, 2026-11-11), with
   `gc_extern_box.wat` as the fixture. What remains open is the full-suite
   `SIGSEGV` (`GAP-16`): a case that moves with the run, passes alone, and
   disappears when `WasmBase` is built `-O2` — so it is a build-configuration
   dependency on uninitialised state, not a refusal.
5. Meet every applicable item in
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) before calling the
   backend complete.

GNU LibJIT, SLJIT and AsmJIT execute the same generic suite and are
production-ready within the same envelope. LibJIT remains the most mature in
*code quality*, not the only full-featured backend.
