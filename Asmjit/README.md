# `Asmjit/` — planned backend, no code yet

This directory is **reserved** for an AsmJit-backed code generator. It is not a
mistake and it is not dead: an AsmJit backend is a planned SIMD-performance lever
(MMX/SSE…AVX-512 coverage and a vector-aware register allocator), and the
subproject stays in `Project.pro` so the seam keeps a real placeholder next to the
executing backend.

What it is **not**: a working backend. `Asmjit.pro` declares no sources, so the
build produces an empty `libAsmjit.so` that is never installed and never
selectable by name. Do not ship it, and do not treat its presence as a claim that
anything executes.

## When the work starts

1. Read [`docs/BACKEND_WORK_ORDER.md`](../docs/BACKEND_WORK_ORDER.md) — the
   checklist of what a backend must provide — and [`docs/LAYERING.md`](../docs/LAYERING.md)
   §4, the contract it must not violate.
2. Copy `LibJit/LibJit.pro` as the starting point (it links the shared core,
   records `NEEDED libWasmBase.so.1` and refuses a static configuration).
3. Implement `WASM::ModuleInstantiator`'s four hooks and a
   `WASM::BackendSession`; register a name with `WASMJIT_REGISTER_BACKEND`.
4. Add sources to this `.pro`, add the name to the registry, and follow §8 of the
   work order (the definition of done).

Nothing in `WasmBase/` has to change to make room for it. That is the point of
the seam.
