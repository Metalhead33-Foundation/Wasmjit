# `Sljit/`

*The SLJIT backend*

Status: **Snapshot (2026-11-09):** **production-ready within the declared
envelope** — `WASMJIT_BACKEND=sljit ./Test/WasmJit --rng-seed 12345` reports
**22,451 assertions in 821 cases, green**, the same case set and assertion count
as the reference backend. The envelope is x86-64 Linux / SysV AMD64, and the
documented refusals are listed in "Known limitations" below. See
[`docs/BACKEND_PRODUCTION_READINESS.md`](../docs/BACKEND_PRODUCTION_READINESS.md)
for the measured evidence and
[`docs/backends/PRODUCTION_GAPS.md`](../docs/backends/PRODUCTION_GAPS.md) §30 for
the defect ledger that closed the last failures.

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
  comparison families, the four `all_true` reductions, the twelve shifts, the
  saturating `add`/`sub` and `q15mulr`, `min`/`max`, `avgr_u`, `abs`/`popcnt`,
  the widening/`extmul`/`extadd_pairwise`/`dot` families, the two narrowing
  families, the `f32x4`/`f64x2` arithmetic, comparison, unary and conversion
  families, float lane access (`f32x4`/`f64x2`
  `splat`/`extract_lane`/`replace_lane`), and the transport families (extending,
  splatting and zeroing loads, lane loads and lane stores, `i8x16.swizzle` and
  `i8x16.shuffle`), all through the core's shared helper ABI;
- `ref.null`, `ref.is_null`, `ref.eq`, `ref.as_non_null` with its null trap,
  `ref.func` (resolved at run time, internal and imported) and `br_on_null`; `ref.eq` is lowered but
  unfixtured until GC values exist, and `br_on_non_null` waits on AsmJit block
  parameters.

A unit test exercises the same `asmjit_i32_basic`/`asmjit_i64_basic` fixtures
that the AsmJit suite uses (`[sljit][module][parity]`), so the shared slice is
checked against one fixture set rather than two drifting ones.

## Conformance

The generic backend-selected run is the gate, and it is green: 22,451 assertions
in 821 cases. The last failures were four cases with two root causes:

* the Custom Descriptors family (`struct.new_desc`, `struct.new_default_desc`,
  `ref.get_desc`, `ref.cast_desc_eq(_null)`, `br_on_cast_desc_eq(_fail)`) had no
  lowering here — it was LibJIT-only;
* a native import with four parameters could not be called, because SLJIT’s
  C-call form accepts four arguments and `VMContext*` is the first. AssemblyScript
  `abort(msg, file, line, col)` needs five.

The second is fixed by staging the arguments and marshalling them through a
per-signature libffi closure (`stagedImportEntry` in `SljitOpcodeDispatcher.cpp`),
which also makes a native import inside a frame-form module correct: the call site
branches on `Callable::context`, so a null context routes to the adapter and a
non-null one to the ordinary `(ctx, frame)` call.

A refused function leaves that function’s `Callable::fnPtr` **null**; the core
reports "no compiled body" at the call boundary, and the module compiler refuses
the whole module with the function index and the reason
(`Sljit::ModuleCompiler::unsupportedFunctions()` still lists per-function reasons).

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

* one executable mapping per function, because `SLJIT_WX_EXECUTABLE_ALLOCATOR=1`
  is defined so code never lives in a read-write-execute page;
* a native import with more than three Wasm parameters, or any native import in a
  frame-form module, is called through the staged adapter, i.e. one libffi call
  per invocation. That is the price of SLJIT’s four-argument C-call ceiling;
* an **indirect** call whose signature carries a `v128` *and* more than three
  parameters is refused in a module that also imports a native function: the
  adapter has no `ffi_type` for a vector. `return_call_ref` keeps the
  register-form ceiling for the same reason;
* the operand stack’s reference ownership is exact for the paths the suites
  exercise (arguments, locals, created objects, returned references, the
  descriptor family), but a reference read out of a struct field or array
  element is still a *borrow*, and `drop` releases only continuations and
  strings. That is a leak, never a wrong result
  ([`docs/backends/PRODUCTION_GAPS.md`](../docs/backends/PRODUCTION_GAPS.md) §30);
* the helper-call lowering of division, memory access and calls is deliberately
  correctness-first; a later slice can inline the checked paths where the
  post-mortem measures a benefit.

Generated code **is** released on `Runtime::unload` and runtime teardown through
`ModuleCompiler::releaseInstance`. The staged-import closures are process-lifetime
and shared, like the rest of the process-wide helper caches.

## Next steps

1. `ref.func` (which also unlocks a `ref.eq` fixture), then the remaining
   reference/exception/continuation work. `v128` in signatures is **deferred by
   decision** — see [`docs/backends/V128_SIGNATURE_DECISION.md`](../docs/backends/V128_SIGNATURE_DECISION.md);
   it must be revisited before the saga closes. What landed is recorded in
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
