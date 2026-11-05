# `Sljit/`

*The SLJIT backend*

Status: **Snapshot (2026-11-05):** the backend executes the shared scalar
integer and floating-point slice and is at feature parity with `Asmjit/`; the
next target is the LibJIT backend's `v128`/SIMD, reference/GC and exception
slices. It is not a replacement for `LibJit/`.

## What executes today

`Sljit.pro` compiles the vendored `extern/sljit` sources into `libSljit.so`
(no external `-lsljit`), links the one shared `libWasmBase`, and registers the
backend as `sljit`:

```cpp
WASM::Runtime::create("sljit");                 // product path
WASMJIT_BACKEND=sljit ./Test/WasmJit '[sljit]'  // harness path
```

The emitter lowers:

- up to **three** Wasm parameters and zero or one result, each `i32`, `i64`,
  `f32` or `f64` (SLJIT's `sljit_emit_enter` and its C call ABI accept at most
  four arguments, and `VMContext*` is the first);
- `i32`/`i64` constants, locals, globals; `add`/`sub`/`mul`, checked
  `div`/`rem`, `and`/`or`/`xor`, `shl`/`shr_s`/`shr_u`, `rotl`/`rotr`,
  `clz`/`ctz`/`popcnt`, all comparisons, `eqz`, sign-extension and
  `i32.wrap_i64`;
- `f32`/`f64` constants, locals, globals; `add`/`sub`/`mul`/`div`,
  `abs`/`neg`/`sqrt`/`ceil`/`floor`/`trunc`/`nearest`, `min`/`max`,
  `copysign`, all comparisons, the integer/float conversions (checked and
  saturating), `demote`/`promote` and reinterpretation;
- `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`, including block
  parameters and multi-value block results;
- direct and indirect calls, forward and mutually recursive calls, native and
  cross-instance Wasm imports, and `return_call*` lowered to call + return;
- memory32 and memory64 loads and stores (all integer widths, signed/unsigned
  narrow extensions, plus `f32`/`f64`), `memory.size`/`grow` at both index
  widths, `memory.copy`/`fill`/`init` (memory32), `data.drop`,
  `table.init`/`copy`/`size` and `elem.drop`;
- `select`/`select_t`, `drop`, `nop`, `return`, `unreachable`, and the guest
  stack guard with the pending-trap epilogue;
- the first two `v128` slices: `v128.const`/`load`/`store`, `v128` locals,
  `i32x4.splat`/`extract_lane`/`replace_lane`, the bitwise family
  (`not`/`and`/`andnot`/`or`/`xor`/`bitselect`), `any_true`, the integer lane
  arithmetic (`add`/`sub` for every shape, `mul` where the spec has it), the
  comparison families and the four `all_true` reductions, all through the core's
  shared helper ABI.

A unit test exercises the same `asmjit_i32_basic`/`asmjit_i64_basic` fixtures
that the AsmJit suite uses (`[sljit][module][parity]`), so the shared slice is
checked against one fixture set rather than two drifting ones.

## What it still refuses

`v128` in signatures, block types or `select`, the remaining `v128` families
(lane arithmetic/comparisons, the other lane widths and both float shapes,
extending/lane loads, lane stores, `shuffle`/`swizzle`, relaxed SIMD),
references/GC, exceptions, continuations, atomics, `f16` load/store, bulk
memory on a memory64, functions with more than three parameters, and functions
with more than one result. Each refusal leaves that
function's
`Callable::fnPtr` **null**; the core then reports "no compiled body" at the
call boundary. Nothing is miscompiled, nothing is interpreted, and no
partially emitted code is installed. The per-function reasons are available
from `Sljit::ModuleCompiler::unsupportedFunctions()`.

## How the slice is emitted

- **Two passes.** The first measures the operand-stack high-water mark (the
  frame cannot grow after `sljit_emit_enter`); the second emits. Both share the
  same visit code, so the measured depth cannot disagree with the emitted one.
- **Frame slots, four scratch registers.** Every local and operand is one
  machine word in the frame; `R0-R3` carry every operation and helper call, so
  register pressure is constant and a call clobbers nothing that lives in a
  slot. Three reserved slots hold the saved `VMContext*`, the indirect call
  target and a scratch word across a helper call.
- **Runtime-resolved direct calls.** A function body is emitted in its own
  SLJIT compiler, so a forward or mutually recursive target does not exist yet.
  Generated code calls `ModuleCompiler::resolveCallable(vm, index)`, which
  returns the `Callable*` (or reports an internal trap), then performs the C
  ABI call through that pointer. That costs one extra indirect call per Wasm
  call; it is the price of keeping per-function code ownership and
  `Runtime::unload`.
- **Traps are statuses, not jumps.** Division, memory bounds, indirect-call
  checks, bulk memory and table operations run through C helpers that report
  into the pending slot. Generated code stores the result, probes the slot and
  branches to a shared epilogue that returns a benign value. No C++ exception
  unwinds a generated frame (docs/SAFETY.md §2.4).

## Files

| File | Role |
|---|---|
| `SljitBackend.{hpp,cpp}` | `createModuleCompiler()`, `createSession()`, `WASMJIT_REGISTER_BACKEND("sljit", …)` |
| `SljitModuleCompiler.{hpp,cpp}` | the four `ModuleInstantiator` hooks, the start-function call, `releaseInstance` and the runtime callable resolver |
| `SljitOpcodeDispatcher.{hpp,cpp}` | the body emitter (frame slots, control frames, helpers, prologue/epilogue) |
| `SljitBackendSession.{hpp,cpp}` | the `WASM::BackendSession` façade (tuple layout, lane-wise probe, `libffi` invoke) |

Nothing in `WasmBase/` had to change to add it. That is the point of the seam.

## Known limitations

- one executable mapping per function, because `SLJIT_WX_EXECUTABLE_ALLOCATOR=1`
  is defined so code never lives in a read-write-execute page;
- no `rawFnPtr` driver and no `tailEntry` thunk: the callable's entry is the raw
  generated code and the core falls back to `call` + `return` for tail calls;
- the helper-call lowering of division, memory access and calls is deliberately
  correctness-first; a later slice can inline the checked paths where the
  post-mortem measures a benefit.

Generated code **is** released on `Runtime::unload` and runtime teardown through
`ModuleCompiler::releaseInstance`.

## Next steps

1. The remaining `v128` families (shifts, saturating/narrowing/widening, float
   lanes, extending/lane loads, `shuffle`/`swizzle`), then `v128` in signatures
   and block types. What landed is recorded in
   [`docs/backends/ASMJIT_SLJIT_P3_SIMD1.md`](../docs/backends/ASMJIT_SLJIT_P3_SIMD1.md)
   and [`docs/backends/ASMJIT_SLJIT_P3_SIMD2.md`](../docs/backends/ASMJIT_SLJIT_P3_SIMD2.md).
2. References/GC, `ref.*`, GC barriers and exceptions (P4), following
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) §6 "Reference
   ownership" — the initialiser-shape test is the gate, not an optional
   refinement.
3. Continuations and `call_ref` (P5).
4. Once a slice lands, extend the shared `[sljit][module][parity]` fixtures
   before calling it done; the checklist stays in
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) §12.
