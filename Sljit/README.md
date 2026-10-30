# `Sljit/` — the provisional second backend

This directory holds a **working but deliberately provisional** SLJIT-backed
code generator. It is the second implementation of the compilation seam and the
worked example for [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md);
what it is *not* is a replacement for `LibJit/`, or a claim of feature parity.

## What executes today

`Sljit.pro` compiles the vendored `extern/sljit` sources into `libSljit.so`
(no external `-lsljit`), links the one shared `libWasmBase`, and registers the
backend as `sljit`:

```cpp
WASM::Runtime::create("sljit");          // product path
WASMJIT_BACKEND=sljit ./Test/WasmJit '[sljit]'   // harness path
```

The emitter lowers a straight-line integer subset:

- up to **three** Wasm parameters, each `i32` or `i64`, plus `VMContext*`
  (SLJIT's `sljit_emit_enter` accepts at most four arguments);
- zero or one result, `i32` or `i64`;
- `local.get`, `local.set`, `local.tee`, `i32.const`, `i64.const`,
  `i32`/`i64` `add`/`sub`/`mul`/`and`/`or`/`xor`/`shl`/`shr_s`/`shr_u`,
  `drop`, `nop`, `return`, and the implicit `end`;
- the guest stack guard (`wasm_stack_check`) and a pending-trap epilogue that
  annotates the trap site and returns a benign value.

`BackendSession` adds the tuple layout (`computeAggregateLayout`), the
diagnostic lane-wise `i32x4` operation (real SLJIT code, freed on destruction)
and `invoke` (a `libffi` call over the marshalled slots).

## What it refuses

Every other opcode — control flow, calls, memory, globals, floating point,
references, SIMD, exceptions — leaves that function's `Callable::fnPtr` **null**.
The core then refuses the call at the boundary ("no compiled body"). Nothing is
miscompiled, nothing is interpreted, and no partially emitted code is installed.
The per-function reasons are available from
`Sljit::ModuleCompiler::unsupportedFunctions()`.

## Known limitations

- generated wasm code is never freed: `ModuleInstantiator` has no
  instance-teardown hook, so there is nowhere safe to call `sljit_free_code`;
- one executable mapping per function, because `SLJIT_WX_EXECUTABLE_ALLOCATOR=1`
  is defined so code never lives in a read-write-execute page;
- no `rawFnPtr` driver and no `tailEntry` thunk: the callable's entry is the raw
  generated code and the core falls back to `call` + `return` for tail calls.

## Files

| File | Role |
|---|---|
| `SljitBackend.{hpp,cpp}` | `createModuleCompiler()`, `createSession()`, `WASMJIT_REGISTER_BACKEND("sljit", …)` |
| `SljitModuleCompiler.{hpp,cpp}` | the four `ModuleInstantiator` hooks and the start-function call |
| `SljitOpcodeDispatcher.{hpp,cpp}` | the body emitter (frame slots, operand stack, prologue/epilogue) |
| `SljitBackendSession.{hpp,cpp}` | the `WASM::BackendSession` façade |

Nothing in `WasmBase/` had to change to add it. That is the point of the seam.

## Next steps

Direct `call`/`call_indirect` (needs the call ABI and a type check), then
`block`/`loop`/`if`/`br` (labels and a merge model), then memory access. Each
step extends `SljitOpcodeDispatcher` and its `[sljit]` coverage; the checklist
and the definition of done stay in
[`docs/BACKEND_WORK_ORDER.md`](../docs/BACKEND_WORK_ORDER.md) §8.
