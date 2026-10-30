# The backend work order

> **Status: current.** Written 2026-10-29 to close `AAS9` in
> [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.3. It is the checklist a
> second code generator works from; [`LAYERING.md`](LAYERING.md) §4 states what a
> backend **may not violate**, this note states what it **must provide**.

## 1. What this document is

`WasmBase/` owns everything that is not code generation: parsing, validation,
type identity, memories, tables, globals, the GC heap, strings, exceptions,
continuations, traps, diagnostics, and the instantiation ordering. A backend
turns a validated `Module` into callable machine code and nothing else.

The interface is two objects, both in the product:

| Seam | Type | Header | Who implements |
|---|---|---|---|
| Compilation | `WASM::ModuleInstantiator` | [`WasmModuleInstance.hpp`](../WasmBase/WasmModuleInstance.hpp) | The backend |
| Session / façade | `WASM::BackendSession` | [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp) | The backend (and the harness, to drive it) |

Everything in §2–§7 is a requirement. §8 is how you know you are done.

## 2. The compilation seam

`ModuleInstantiator::instantiate()` is **concrete**: it creates the instance,
resolves imports, builds the memory/global/table index spaces, applies active
data and element segments, and runs the start function. A backend implements
four protected hooks and must not re-implement the rest:

| Hook | Called when | Must do | Must not do |
|---|---|---|---|
| `translateTypes(instance, module, internals)` | first | Translate every type in `module.typeIds`, in order, into the backend's type representation | Allocate heap objects, run Wasm |
| `declareFunctions(instance, module, internals)` | before any body | Fill `internals.internalCallables` — **one entry per defined function, in index order** — with `Callable{ fnPtr, instance.context(), typeIdx, module.typeId(LocalTypeIdx{typeIdx}) }`, then call `instance.store().registerCallableRange(...)` | Emit code, throw for a module-caused condition |
| `compileFunctions(instance, module, internals)` | after declare | Emit every body and store the entry pointer in the matching `Callable::fnPtr`; a backend with a driver/trampoline also fills `rawFnPtr` and the per-type `tailEntry` thunk (below) | Change `internalCallables`' size or order (the range is registered and must stay stable) |
| `callStartFunction(instance, funcIdx, internals)` | only if the module has a start section | Invoke the start function through the compiled ABI | Run any other function |

The four rules that are easy to get wrong:

1. **`internalCallables` is a stable range.** The core records its address and
   size; a resize after `declareFunctions` invalidates `Store::callableAt`, which
   the ref barriers and `call_ref` use to recognise an internal callable.
2. **`Callable::typeId` is the canonical id**, from `module.typeId(...)` — not the
   local index. Every cross-module type check is a `TypeRegistry::matches` on it.
3. **`fnPtr` is null until compiled.** The core treats a null `fnPtr` as "this
   backend cannot execute here" and refuses at the call boundary; it never jumps
   through null.
4. **Nothing throws on a module-caused failure.** Report through the trap slot
   (§4.4) and return; the core decides. A C++ exception below generated code
   cannot unwind through it.

## 3. The session seam

`BackendSession` is what an embedder holds to drive a backend without including a
backend header:

| Method | Obligation |
|---|---|
| `instantiator()` | The one `ModuleInstantiator` this session owns |
| `aggregateLayout(fields)` | Apply the tuple rule of [`ABI.md`](ABI.md) "The result tuple layout"; [`WasmBackendAbi.hpp`](../WasmBase/WasmBackendAbi.hpp) has the same rule as executable PODs if you would rather call it than re-derive it |
| `createLaneWiseOp(multiply)` | A scalar `i32x4` add/multiply usable by the diagnostic SIMD benchmark; it exists so the benchmark never needs your IR |
| `invoke(...)` | Call a compiled `Callable` with already-marshalled slots, owning **all** calling-convention details (including `v128`) and trap handling |

The *selector* is not part of the core: `WASMJIT_BACKEND` and the "create a
session" factory live with whoever ships the backend (`Test/TestBackend.cpp`
today, a plugin's `create` tomorrow). `WasmBase/` must not learn a backend name
([`LAYERING.md`](LAYERING.md) §2).

## 4. The ABI you must emit

| # | Obligation | Where it is specified |
|---|---|---|
| 4.1 | Every compiled function takes `VMContext*` as arg0 | [`ABI.md`](ABI.md) "Overview" |
| 4.2 | Multi-result functions take a trailing `void* results`; a single result is returned by value | [`ABI.md`](ABI.md) "Multiple Results" |
| 4.3 | Result tuple offsets follow the alignment rule (`i32`/`f32` 4, `i64`/`f64` 8, `v128` 16, reference one word) | [`ABI.md`](ABI.md) "The result tuple layout" |
| 4.4 | A trap is a **pending status**: report through `wasm_trap_slot()` (`reportTrap`/`reportTrapAt`) and return. Never `longjmp`, never `abort`, never throw | [`SAFETY.md`](SAFETY.md) §2.4–§2.6; `WasmTrap.hpp` |
| 4.5 | A `v128` parameter or result is the 16-byte aggregate of [`ABI.md`](ABI.md) "`v128` by Value" | same |
| 4.6 | Tail calls: request/return the state through `currentTailCallState()`; the direct-self-recursion fast path may use a native tail call, everything else must go through the trampoline or the semantic `call` + `return` fallback | [`ABI.md`](ABI.md) "Tail calls"; `WasmValue.hpp`; [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) TC-5 |
| 4.9 | If you implement the generalized tail-call trampoline, expose `Callable::rawFnPtr` (the entry behind your `fnPtr` driver) and a per-function-type `Callable::tailEntry` thunk `void(VMContext*, void* fn, uint64_t* args, uint64_t* results, void* resultBuffer)`; a callable with a null `tailEntry` must still be callable through `fnPtr` (the core falls back) | [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) §4; `LibJit/LibjitModuleCompiler.cpp` |
| 4.7 | Continuations: provide the `ContinuationInvokeFn` your `wasm_cont_new_ex` calls need, and call a target through its C ABI using the kind arrays | [`ABI.md`](ABI.md) "Continuations"; `WasmContinuation.hpp` |
| 4.8 | `call_ref`/`call_indirect` type checks compare canonical `TypeId`s and surface `IndirectCallTypeMismatch` | [`ABI.md`](ABI.md); `WasmTypeIdentity.hpp` |

## 5. Memory, GC and ownership

1. **Never allocate Wasm entities yourself.** Memories, tables, globals' cells,
   tags, strings, exceptions and GC objects belong to the `Store`. Ask the
   instance/store; do not keep a parallel allocator.
2. **Follow the GC layout exactly** — header words, per-type size/alignment,
   array data offset ([`WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp),
   [`proposals/GC.md`](proposals/GC.md) invariant H1). The runtime and the
   generated code must agree byte for byte; a drift is silent corruption.
3. **Use the ref barriers** (`refClone`/`refDrop` through the instance) whenever a
   reference moves into or out of a store-owned slot.
4. **Stay inside the store's byte budget.** Nothing bypasses
   `Store::reserveBytes` ([`SAFETY.md`](SAFETY.md) §2.3).
5. **Executable memory is yours**, with a W^X handoff; the core never patches
   code. If your toolkit can interpret instead, expose that as a capability and a
   configuration, not as a core fallback ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7.6).

## 6. Helpers: what you call, what you bring

**Call these** (exported for exactly this purpose; `[plugin]` asserts they
resolve in the process image):

| Family | Examples |
|---|---|
| Entry check | `wasm_stack_check(ctx)` — every compiled function calls it first |
| Traps | `wasm_trap_slot()`, `wasm_trap_report`, `wasm_trap_report_at` |
| SIMD | the 31 `wasm_v128_*` helpers (`wasm_v128_load`, `wasm_v128_and`, …) |
| Exceptions | `wasm_exn_create(ctx, …)`, `wasm_exn_pending`, `wasm_exn_set_pending` |

**Bring your own** (backend-private, deliberately hidden): string-object
construction, `i32`/`i64` bit helpers, ref clone/drop thunks, GC allocation
wrappers, and any intrinsic your IR lowers to. The LibJIT backend keeps all of
these as `static` functions in its own translation units; a plugin backend does
the same.

**Do not** add a helper to `WasmBase/` that only one backend could use: that is a
layering violation with extra steps.


## 7. Packaging and registration

1. **Register a name**: `WASMJIT_REGISTER_BACKEND("name", factory)` from your own
   translation unit, or export `wasmjit_backend_query` if you ship as a plugin.
2. **Declare capabilities** (`WASMJIT_CAP_*`): SIMD, components, continuations,
   profiles, executes. The runtime refuses a module the backend cannot handle
   *before* codegen, the same way `ProfileConfig` refuses a profile.
3. **Honour the profile gate**: read `profile()` in `instantiate()`; never
   compile an opcode the active profile excludes.
4. **Plugin form**: link the one shared `libWasmBase` (never a static copy), set
   `build_flags`, and keep `abi_version`/`core_abi_version` honest
   ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7.2/§7.7).
5. **Install into your own directory only.** A new backend writes no product code
   outside its own directory except a registration line and a `.pro`.

## 8. Definition of done

A backend is done when all of the following are true:

| Check | How |
|---|---|
| The core still builds without your directory | `Project.pro` minus your subdir |
| `[layering]` passes | no backend vocabulary in `WasmBase/` |
| The harness selects you by name | `WASMJIT_BACKEND=<name>` reaches your session |
| `[backendabi]` passes | the POD/layout rule is honoured |
| The generic fixtures instantiate and call | `loadAndInstantiateTestModule` + `TestBackend::Session::invoke` |
| Trap, GC and exception fixtures pass | a trap unwinds by returning, not by throwing |
| The installed-header consumer works with you | `Test/install-check.sh` with your backend selected |
| A plugin build loads and refuses mismatches | `[plugin]` against your `.so` |

If a check cannot pass, the honest outcome is a documented deviation in your own
note plus a `WASMJIT_CAP_*` flag that makes the runtime refuse the unsupported
shape — not a silent wrong answer.

## 9. What the core already does for you

Parsing and validation; type identity and the `match` relation; import
resolution; memory/table/global index spaces and growth; active data/element
segments; the start-section ordering; the GC heap, reclamation and safepoints;
strings; exception objects and the pending slot; the continuation providers;
the trap slot and diagnostics; the byte budget; the profile gate; and the
`Store` an instance lives in. A backend that finds itself re-implementing one of
those has likely found a gap in this document — write it down.

## 10. References

- [`LAYERING.md`](LAYERING.md) §4 — the contract rows this checklist elaborates.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.3 — `AAS8`/`AAS9`/`AAS11`,
  the gaps this note closes.
- [`ABI.md`](ABI.md) — the calling convention, the tuple rule, `v128`, traps,
  continuations.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the dynamic form, its
  descriptor and the nine hard problems.
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6.1.1 — the header tiers that
  decide whether a helper of yours belongs in `WasmBase/` (it does not).
- Source anchors: `WasmBase/WasmModuleInstance.hpp` (`ModuleInstantiator`),
  `WasmBase/WasmBackendSession.hpp` (`BackendSession`),
  `WasmBase/WasmNullInstantiator.cpp` (the smallest correct `declareFunctions`),
  `WasmBase/WasmBackendAbi.hpp` (the executable tuple rule),
  `LibJit/LibjitModuleCompiler.cpp` (a full implementation).

