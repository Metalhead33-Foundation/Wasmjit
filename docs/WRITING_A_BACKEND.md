# Writing a backend {#doc_writing_a_backend}

*The checklist and the walkthrough*

Status: how-to note, revision 4 (2026-11-05). It is both the checklist of what a
code generator must provide and the walkthrough of how to provide it; §14 is the
production plan for the AsmJit and SLJIT backends and the post-mortem they must
produce. It contains no engine behaviour of its own.

This note is the **checklist** (what a code generator must provide) *and* the
**walkthrough** (which class to subclass, which method to implement, in what
order). [`LAYERING.md`](LAYERING.md) §4 states what a backend **may not violate**;
this note states what it **must provide** and how to provide it.

**If you only read one section**, read §3 (the four hooks), §6 (the ABI you emit —
including reference ownership) and §11 (what the SLJIT backend does and
deliberately does not do). A backend author continuing one of the two partial
implementations should also read §14 before planning a slice.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What is a backend, and what is not mine to write? |
| §3 | Which abstract class must be subclassed, and which four methods? |
| §4 | How does the core drive those four hooks? |
| §5 | How do I decode a function body without writing a decoder? |
| §6 | What ABI must the generated code follow, and who owns a reference? |
| §7 | Which interface must the backend *expose* (`BackendSession`)? |
| §8 | How is the backend packaged, registered, given capabilities and selected? |
| §9 | What may I allocate, and what belongs to the `Store`? |
| §10 | Which helpers do I call, and which must I bring? |
| §11 | What does the SLJIT backend contain, and what does it not do yet? |
| §12 | How do I test it, and what counts as done? |
| §13 | References |
| §14 | What is the production plan for AsmJit and SLJIT, and what must their post-mortem contain? |

## 2. What is a backend, and what is not mine to write?

A backend turns a **validated** `WASM::Module` into callable machine code. That
is the whole job. The core already does the following, and a backend that starts
re-implementing one of them has found a gap in the note rather than a task:

- parsing and validation (including the profile gate);
- type identity, interning and the `match` relation;
- import resolution; memory, table and global index spaces; their growth;
- active data/element segments and the start-section ordering;
- the GC heap, reclamation, safepoints, strings, exceptions, continuations;
- the trap slot, diagnostics and the allocation budget.

Two seams connect a backend to the core:

| Seam | Type | Header | Who implements it |
|---|---|---|---|
| Compilation | `WASM::ModuleInstantiator` | [`WasmModuleInstance.hpp`](../WasmBase/WasmModuleInstance.hpp) | **you** |
| Session / façade | `WASM::BackendSession` | [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp) | **you** (plus the harness, to drive it) |

Nothing under `WasmBase/` may learn your backend's name
([`LAYERING.md`](LAYERING.md) §2). If a change seems to require it, the change
belongs in your directory.

## 3. The compilation seam: `ModuleInstantiator`'s four hooks

`ModuleInstantiator::instantiate()` is **concrete** and is not yours to
re-implement. It creates the instance, resolves imports, builds the index
spaces, applies the active segments, and runs the start function. You subclass
`WASM::ModuleInstantiator` and implement exactly four protected virtuals:

| Hook | Called | You must | You must not |
|---|---|---|---|
| `translateTypes(instance, module, internals)` | first | lower every `module.typeIds` entry into whatever type representation your emitter needs | allocate heap objects, run Wasm |
| `declareFunctions(instance, module, internals)` | before any body | fill `internals.internalCallables` with **one `Callable` per defined function, in index order**, then `instance.store().registerCallableRange(...)` | emit code; throw for a module-caused condition |
| `compileFunctions(instance, module, internals)` | after declare | emit every body and store its entry pointer in the matching `Callable::fnPtr` | resize or reorder `internalCallables` |
| `callStartFunction(instance, funcIdx, internals)` | only with a start section | invoke the start function through your ABI | run any other function |

The smallest correct `declareFunctions` is the one in
[`WasmNullInstantiator.cpp`](../WasmBase/WasmNullInstantiator.cpp); the smallest
*executing* one is in [`Sljit/SljitModuleCompiler.cpp`](../Sljit/SljitModuleCompiler.cpp).

Four rules that are easy to get wrong:

1. **`internalCallables` is a stable range.** The core records its address and
   size; a resize after `declareFunctions` invalidates `Store::callableAt`,
   which the reference barriers and `call_ref` use to recognise an internal
   callable.
2. **`Callable::typeId` is the canonical id** from `module.typeId(LocalTypeIdx{idx})`,
   never the local index. Every cross-module type check is a
   `TypeRegistry::matches` on it.
3. **`fnPtr` stays null until a body is compiled.** The core treats null as
   "this backend cannot execute here" and refuses at the call boundary; it never
   jumps through null. This is the honest way to be partial (§11).
4. **Nothing throws on a module-caused failure.** Report through the trap slot
   and return; a C++ exception below generated code cannot unwind through it
   ([`SAFETY.md`](SAFETY.md) §2.4–§2.6).
5. **A compile failure is a diagnostic, never an `abort`.** Lowering and code
   generation run *before* any Wasm frame exists, so they may throw — but the
   module is untrusted input, and `std::abort()`, a bare `assert`, or a silent
   null entry point lets it take the host down. Emit
   `WASM.COMPILE.FUNCTION_FAILED` with the module-local function index, then
   throw; if your library cannot be torn down after the failure, leak it rather
   than crash ([`languages/LF2_DEEP_F1_BACKEND.md`](languages/LF2_DEEP_F1_BACKEND.md) §5).

A `Callable` also carries the optional tail-call fields (`rawFnPtr`,
`tailEntry`). Leaving them null is legal: the core falls back to
`call` + `return` ([`ABI.md`](ABI.md) "Tail calls", `TC-5`).

## 4. The order the core calls them in

`instantiate()` does, in order:

```text
profile gate (re-validate when not the full profile)
ModuleInstance constructor      ← imports, memories, globals, tables
translateTypes()
declareFunctions()              ← callables + registerCallableRange
compileFunctions()              ← fnPtr back-filled
resolveGlobalRefFuncs() / resolveTableRefFuncs()
applyActiveSegments()           ← data/element segments
callStartFunction()             ← only when the module has a start section
```

So a body may rely on every callable *existing* while it is compiled, but it may
assume nothing about segments — they are applied afterwards.

## 5. How to decode a body without writing a decoder

`WASM::OpcodeDispatcher` ([`WasmOpcodeDispatcher.hpp`](../WasmBase/WasmOpcodeDispatcher.hpp))
is the core's decoder **and** a visitor: one `dispatch…()` method per opcode. A
backend subclasses it and overrides only the opcodes it lowers:

```cpp
class MyDispatcher : public WASM::OpcodeDispatcher {
protected:
    void dispatchI32Add() override { /* emit my IR */ }
    void onUnhandledOpcode(const char* name) override { /* refuse this function */ }
};

MyDispatcher dispatcher(/* your state */);
Euph::Io::ConstBufferDevice device(codeSpan);
WASM::FastMemReader stream(device);
dispatcher.readCode(stream);
```

- The default `onUnhandledOpcode()` throws `std::logic_error`. Overriding it is
  how a backend turns "opcode I do not lower" into a per-function refusal
  instead of a hard failure.
- `instructionOffset` is set before every dispatch and is the coordinate the
  branch hints and the trap-site annotations use.
- **Branch hints are available without re-parsing the custom section**
  ([`proposals/SMALL_PROPOSALS.md`](proposals/SMALL_PROPOSALS.md) §4). Call
  `setBranchHints(body.branchHints.data(), body.branchHints.size())` once, before
  `readCode`, and ask `branchHintAt(instructionOffset)` where your code generator
  can use an answer — a likely/unlikely `if` or `br_if`, block layout, branch
  polarity. The lookup is a binary search and happens only when you ask, so a
  backend that ignores hints (or a hint-less module) pays nothing. A hint is
  advisory: acting on it must never change a result, a trap or a validation
  outcome, and `BranchHintKind::None` means "no opinion".
- Bodies are already validated, so you may trust the operand types the
  validator proved. You may **not** trust indices without a bounds check if
  your emitter would write out of bounds on a wrong one; a checked `.at()` or an
  explicit test is cheap at compile time.
- `PreparedFunctionStack` ([`WasmType.hpp`](../WasmBase/WasmType.hpp)) flattens
  a signature + body into `allLocals` / `allReturns`
  (`parameterCount` + `localCount`), which is what a frame layout needs.

There is no requirement to use this class. It is simply the same decode path the
validator and the LibJIT backend use, so a body your backend accepts is a body
the core already agreed on.

## 6. The ABI the generated code must follow

| # | Obligation |
|---|---|
| 6.1 | Every compiled function takes `WASM::VMContext*` as arg 0, then the Wasm parameters |
| 6.2 | A single result is returned by value; a multi-result signature takes a trailing `void* results` |
| 6.3 | Result tuple offsets follow [`ABI.md`](ABI.md) "The result tuple layout" (`i32`/`f32` 4, `i64`/`f64` 8, `v128` 16, a reference one word) |
| 6.4 | A trap is a **pending status**: report through `wasm_trap_report`/`reportTrap` and return. Never `longjmp`, never `abort`, never throw |
| 6.5 | The prologue calls `wasm_stack_check(VMContext*)` before the body runs, and checks the pending slot afterwards |
| 6.6 | A `v128` parameter or result is the 16-byte aggregate of [`ABI.md`](ABI.md) "`v128` by Value" |
| 6.7 | References are one pointer-sized word and move through `refClone`/`refDrop` when they enter or leave a store-owned slot — and the ownership model below is part of the ABI, not an implementation detail |
| 6.8 | Executable memory is yours, with a W^X handoff. The core never patches code |
| 6.9 | Tail calls: request and return the state through `currentTailCallState()`; the direct-self-recursion fast path may use a native tail call, everything else must go through the trampoline or the semantic `call` + `return` fallback ([`ABI.md`](ABI.md) "Tail calls", [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) `TC-5`) |
| 6.10 | If you implement the generalized trampoline, expose `Callable::rawFnPtr` (the entry behind your `fnPtr` driver) and a per-function-type `Callable::tailEntry` thunk `void(VMContext*, void* fn, uint64_t* args, uint64_t* results, void* resultBuffer)`; a callable with a null `tailEntry` must still be callable through `fnPtr`, because the core falls back |
| 6.11 | Continuations: provide the `ContinuationInvokeFn` your `wasm_cont_new_ex` calls need, and call a target through its C ABI using the kind arrays ([`ABI.md`](ABI.md) "Continuations", `WasmContinuation.hpp`) |
| 6.12 | `call_ref` / `call_indirect` type checks compare canonical `TypeId`s and surface `IndirectCallTypeMismatch` — never a silent call through a mismatched signature |

The C ABI of a compiled function is therefore
`Ret fn(VMContext* ctx, Params…)` — which is exactly the shape
`WASM::callCallable<Ret>` invokes, so an embedder's call needs no backend
knowledge.

### Reference ownership: parameters, locals and the stack

A reference is a counted pointer and *who holds a count* is part of the ABI your
generated code must honour. The model is three sentences:

- **The operand stack owns one count per reference it holds.** Pushing clones;
  every pop that discards a reference drops it.
- **Every reference local owns one count.** `local.set`/`local.tee` clone into
  the slot and drop the value it replaces; the epilogue drops every reference
  local.
- **A parameter arrives as the caller's borrow.** The caller still owns its
  argument; the callee may not drop it.

The third sentence is the one that bites, because it makes `local.set` a
special case: the *first* store into a parameter slot would be dropping a count
the callee never held, while every later store must drop the count the slot now
owns. Do **not** resolve that per instruction. Retain once, at entry: clone each
reference parameter into its own slot in the prologue, release it in the
epilogue, and then the two local rules above are static and correct for every
instruction — including an instruction that re-executes in a loop. LibJIT's
version is `emitReferenceParameterRetains()`
([`LibjitOpcodeDispatcher.cpp`](../LibJit/LibjitOpcodeDispatcher.cpp)); copy the
shape, not the code.

**The failure mode this prevents.** A function that allocates an object, stores
it into a **parameter** (the `this`-in-an-initialiser shape every OO language
emits) and returns it will free the object inside the callee if the parameter
branch skips the retain and then drops the stack's count:

```
Old value = 121            # the object's TypeId
New value = 4294967295     # TypeId::kNone, written by reclaimGcObject
```

The caller's next `ref.cast` then either traps (`cast failure in function N`) or,
before the `kNone` guard in `heapMatches`, crashed in `CoreTypeView::kind()`. The
Kotlin/Wasm `wasm-wasi` module hit this in `$kotlin.collections.ArrayList.<init>`
and it is recorded as `F3d` in
[`languages/LF2_DEEP_F3_GC.md`](languages/LF2_DEEP_F3_GC.md) §7.1.

**Do not track ownership per instruction.** A loop re-executes the same *emitted*
`local.set`, so a compile-time "the slot owns a count now" flag releases the old
value only on its first iteration. That version leaked instead of crashing: the
existing `two agents allocate and release GC objects on one instance` reclaim
count went from 40,000 to 2. Static rules plus the entry-time retain are the only
shape that survives both cases.

**How to test it.** `Test/wasm_wat/gc_refcount.wat` covers the allocate-and-release
loop; add the initialiser shape — a function that `struct.new`s inside an `if`,
stores the object into its reference parameter and returns it, and a caller that
stores and casts it — and assert the object is still alive at the cast. If you do
not lower references yet (`Asmjit`, `Sljit` today), refuse them explicitly and
keep `Callable::fnPtr` null; add this rule to your checklist *before* you do.

## 7. The session seam: `BackendSession`

`ModuleInstantiator` is what the *core* drives. `BackendSession` is what an
*embedder* drives without including a backend header
([`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp)):

| Method | Obligation |
|---|---|
| `instantiator()` | the one `ModuleInstantiator` this session owns |
| `aggregateLayout(fields)` | the tuple rule of ABI.md; `WASM::computeAggregateLayout` implements it as PODs, so calling it is allowed and is what the SLJIT session does |
| `createLaneWiseOp(multiply)` | a scalar `i32x4` add/multiply for the diagnostic SIMD benchmark; it exists so the benchmark never needs your IR |
| `invoke(...)` | call a compiled `Callable` from already-marshalled slots, owning **all** calling-convention detail (including `v128`) and the trap collection |

`invoke` is the method worth thinking about early: the embedder hands you
pointers to marshalled slots, so you need a generic caller. `libffi` is already a
core dependency (the component runtime uses it), which is why the SLJIT session
builds an `ffi_cif` from the `FuncType` instead of hard-coding every signature
shape.

## 8. Packaging, registration and selection

1. **One directory per backend.** `Sljit/` writes no product code outside
   itself except its registration line and its `.pro`.
2. **Link the one shared core, never a static copy.** Copy the block from
   [`LibJit.pro`](../LibJit/LibJit.pro) or [`Sljit.pro`](../Sljit/Sljit.pro):
   `LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase`, a `PRE_TARGETDEPS` on
   `libWasmBase.so`, `-Wl,--no-undefined`, and a configure-time `error()` when
   the build is static. That is what records `NEEDED libWasmBase.so.1` in your
   image instead of embedding a second store and type registry.
3. **Register a name** from your own translation unit:

   ```cpp
   WASM::BackendInstance makeBackend() {
       auto compiler = std::make_shared<MyBackend::ModuleCompiler>();
       return WASM::BackendInstance{ std::move(compiler), nullptr };
   }
   WASMJIT_REGISTER_BACKEND("myname", makeBackend);
   ```

   `keepAlive` is opaque state the instantiator borrows and that must outlive
   it (a code-emission context, a code cache). Return `nullptr` when there is
   none. It must also be safe to retain for the **Store lifetime**: a failed
   instantiation after the segment phase is kept alive with its `keepAlive`
   until the Store is destroyed, so references it wrote into an imported table
   remain callable (`ABI.md` "Failed instantiation retention").
4. **A session factory lives with the backend**, not in the core:

   ```cpp
   namespace MyBackend { std::unique_ptr<WASM::BackendSession> createSession(); }
   ```

   The name selector (`WASMJIT_BACKEND`) belongs to whoever ships the backend —
   `Test/TestBackend.cpp` in-tree, a plugin's `create` otherwise.
5. **Declare what you can do** and refuse the rest *before* codegen. The
   capability flags (`WASMJIT_CAP_*` in `WasmBase/wasmjit_backend.h`) cover SIMD,
   components, continuations, profiles and "executes"; the runtime refuses a
   module the backend cannot handle *before* codegen, the same way
   `ProfileConfig` refuses a profile. `Runtime::create("name")` is the public
   entry point; `WASM::createBackendInstance("name")` is the registry entry point
   an embedder with no `Runtime` uses. A `ModuleInstantiator` also answers
   `compilationCapabilities()` (see `COMPILER_POLICY.md` `CP-1`), which is how an
   optimisation degree or an interpreter request is refused by name.
6. **Honour the profile gate**: read `profile()` in `instantiate()`; never
   compile an opcode the active profile excludes.
7. **Plugin form**: link the one shared `libWasmBase` (never a static copy), set
   `build_flags`, and keep `abi_version`/`core_abi_version` honest
   ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7.2/§7.7).
8. **Install into your own directory only.** A new backend writes no product code
   outside its own directory except a registration line and a `.pro`.

## 9. Memory, GC and ownership

1. **Never allocate Wasm entities yourself.** Memories, tables, globals' cells,
   tags, strings, exceptions and GC objects belong to the `Store`. Ask the
   instance/store; do not keep a parallel allocator.
2. **Follow the GC layout exactly** — header words, per-type size/alignment,
   array data offset ([`WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp),
   [`proposals/GC.md`](proposals/GC.md) invariant H1). The runtime and the
   generated code must agree byte for byte; a drift is silent corruption.
3. **Use the ref barriers** (`refClone`/`refDrop` through the instance) whenever a
   reference moves into or out of a store-owned slot, and follow §6's ownership
   model — a reference parameter is borrowed, a reference local owns a count.
4. **Stay inside the store's byte budget.** Nothing bypasses
   `Store::reserveBytes` ([`SAFETY.md`](SAFETY.md) §2.3).
5. **Executable memory is yours**, with a W^X handoff; the core never patches
   code. If your toolkit can interpret instead, expose that as a capability and a
   configuration, not as a core fallback
   ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7.6).
6. **Give the code back when the instance is unloaded.** `Runtime::unload` calls
   `ModuleInstantiator::releaseInstance` with the instance still alive; a backend
   that owns executable memory outside a per-instance compilation context must
   free it there. The default is a no-op, which is right for a backend whose
   context *is* the instance's (the runtime destroys one backend instance per
   module, so `jit_context_destroy`/`JitRuntime` teardown frees the code). The
   SLJIT backend is the worked example: it keeps no context and frees each entry
   point explicitly. `releaseInstance` must not throw, must be idempotent (clear
   what you free), and must not touch the instance's memories — the destructor
   does that afterwards.

## 10. Helpers: what you call, what you bring

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



## 11. The SLJIT backend — a worked example, and its honest limits

`Sljit/` is the second executing backend. As of **snapshot 2026-11-05** it
covers the same scalar integer and floating-point slice as `Asmjit/` — P0–P3
(scalar) in the plan's terms — so the two can act as each other's oracle; it
still claims no parity with LibJIT, whose references/GC, exceptions and
continuations are the next target.

| File | Role |
|---|---|
| [`SljitBackend.hpp`](../Sljit/SljitBackend.hpp) / `.cpp` | `createModuleCompiler()`, `createSession()`, `WASMJIT_REGISTER_BACKEND("sljit", …)` |
| [`SljitModuleCompiler.hpp`](../Sljit/SljitModuleCompiler.hpp) / `.cpp` | the four `ModuleInstantiator` hooks, the start-function call, and the per-function compile loop |
| [`SljitOpcodeDispatcher.hpp`](../Sljit/SljitOpcodeDispatcher.hpp) / `.cpp` | the body emitter: frame slots, the operand-stack model, the prologue/trap epilogue |
| [`SljitBackendSession.hpp`](../Sljit/SljitBackendSession.hpp) / `.cpp` | `BackendSession`: `aggregateLayout`, `createLaneWiseOp`, `invoke` (libffi) |

**What it executes today** (and therefore what the tests may assert):

- up to **three** Wasm parameters, each `i32` or `i64`, plus `VMContext*`
  (SLJIT's `sljit_emit_enter` and its call ABI accept at most four arguments);
- zero or one result, `i32` or `i64`;
- `local.get`, `local.set`, `local.tee`, `i32.const`, `i64.const`;
- `i32`/`i64` `add`/`sub`/`mul`, checked `div`/`rem`, `and`/`or`/`xor`,
  `shl`/`shr_s`/`shr_u`, `rotl`/`rotr`, `i32` `clz`/`ctz`/`popcnt`, every
  comparison and `eqz`, the sign-extensions and `i32.wrap_i64`;
- `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`, including block
  parameters and multi-value block results, plus `select`/`select_t`,
  `unreachable`, `drop`, `nop`, `return` and the implicit `end`;
- direct and indirect calls, forward/mutually recursive targets, native and
  cross-instance imports, and `return_call*` lowered to call + return;
- `f32`/`f64` constants, locals, globals, arithmetic, `min`/`max`,
  `abs`/`neg`/`sqrt`, rounding, `copysign`, comparisons, checked and saturating
  integer conversions, `demote`/`promote` and reinterpretation;
- `i32`/`i64` globals, memory32 and memory64 loads/stores at every integer and
  float width, `memory.size`/`grow` at both index widths, `memory.copy`/`fill`/`init`
  (memory32), `data.drop`, `table.init`/`copy`/`size` and `elem.drop`;
- the stack guard and a pending-trap epilogue, so a call reports a
  `CallStackExhausted` trap as a pending status rather than crashing.

**What it refuses, loudly and safely:** `v128`/SIMD, the `f16` proposal,
references/GC, exceptions, continuations, atomics, bulk memory on a memory64,
a call with more than three parameters and a function with more than one result.
Each leaves
that function's `Callable::fnPtr` **null**. The core then refuses the call at
the boundary with the existing "no compiled body" error. Nothing is
miscompiled, nothing is interpreted, and no half-emitted code is ever installed.

**Known limitations, recorded rather than hidden:**

| Limitation | Why | What replaces it |
|---|---|---|
| three parameters, one result | SLJIT's `sljit_emit_enter`/`sljit_emit_icall` take at most four integer arguments, and `VMContext*` is one of them | LibJIT for wider signatures |
| no `v128`/SIMD | the vector helper layer is the next slice; a half-lowered vector is worse than a refused one | LibJIT |
| no references/GC/exceptions | the GC layout and ownership model are whole projects of their own | LibJIT (`emitReferenceParameterRetains`) |
| references are refused, not half-owned | neither this backend nor Asmjit lowers `ref.*`/`struct.*`/`array.*`; a reference it moves without counting is worse than one it refuses. When either adds them, §6 “Reference ownership” is the rule to implement, not an optional refinement | LibJIT |
| one executable mapping per function | the W^X allocator maps each block separately | acceptable for a second backend; the default pooled allocator trades W^X for density |
| one indirect call per direct call | a body is emitted in its own SLJIT compiler, so `ModuleCompiler::resolveCallable` resolves the target at run time | an inlining slice once the post-mortem measures the cost |

The list no longer contains "generated code is never freed": since the unloader
shipped (2026-11-01), `Runtime::unload` — and runtime teardown — reaches
`releaseInstance`, which is where this backend calls `sljit_free_code` for every
entry point it handed out. AsmJit implements the same hook by releasing the
module's one installed allocation. That is now a *requirement* a backend
satisfies, not a limitation it documents (§9 item 6).

`Sljit.pro` compiles the vendored `extern/sljit` sources into `libSljit.so` — no
external `-lsljit` — and defines `SLJIT_WX_EXECUTABLE_ALLOCATOR=1` so the
generated code goes through the read-write → read-execute handoff instead of
asking the host for RWX pages.

## 12. Testing, and what counts as done

Start with the smallest possible module and grow:

1. **Registration.** `WASM::backendNames()` contains your name;
   `WASM::createBackendInstance("name")` returns a non-null instantiator.
2. **One constant.** The `answer` fixture
   ([`TestFixtures.hpp`](../Test/TestFixtures.hpp), `i32.const 42`) instantiates
   and `Runtime::callExport<std::int32_t>` returns 42.
3. **One parameter.** A two-parameter `i32.add` module round-trips through
   `callExport<int32_t>(instance, "add", 20, 22)`.
4. **One refusal.** A module using an opcode you do not lower must instantiate
   *and* throw "no compiled body" at the call boundary — never execute a
   partially emitted function.
5. **The session.** `aggregateLayout` matches the tuple rule;
   `createLaneWiseOp(true/false)` computes the expected four lanes.
6. **Reference ownership.** Once you lower references, add the initialiser shape
   of [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §6 “Reference ownership”: a
   function that allocates inside an `if`, stores the object into its reference
   **parameter**, returns it, and a caller that stores and casts it — plus the
   `[gc]` reclaim test. The object must still be alive at the cast.

`[sljit]` in [`Test/SljitBackendTests.cpp`](../Test/SljitBackendTests.cpp) is
exactly those five steps. The broader definition of done is the table below, and
a provisional backend is expected to fail most of it. That is compatible with
this note only if the failure is **documented and refused**, not silently wrong.

| The full definition of done | How |
|---|---|
| The core still builds without your directory | `Project.pro` minus your subdir |
| `[layering]` passes | no backend vocabulary in `WasmBase/` |
| The harness selects you by name | `WASMJIT_BACKEND=<name>` reaches your session |
| `[backendabi]` passes | the POD/layout rule is honoured |
| The generic fixtures instantiate and call | `loadAndInstantiateTestModule` + `TestBackend::Session::invoke` |
| Trap, GC and exception fixtures pass | a trap unwinds by returning, not by throwing |
| Reference ownership matches the model | the `[gc]` reclaim test, plus a function that returns a fresh object stored in a reference **parameter** (§6) |
| The installed-header consumer works with you | `Test/install-check.sh` with your backend selected |
| A plugin build loads and refuses mismatches | `[plugin]` against your `.so` |
| Unloading gives the code back | `Runtime::unload` returns true, `releaseInstance` runs, and the module re-instantiates and runs again (`[runtime]` has both cases) |

If a check cannot pass, the honest outcome is a documented deviation in your own
note plus a `WASMJIT_CAP_*` flag that makes the runtime refuse the unsupported
shape — not a silent wrong answer (§11's limitations table is the worked example).

## 13. References

- [`LAYERING.md`](LAYERING.md) — what the core may name (nothing of yours), and
  §4's contract rows this note elaborates.
- [`ABI.md`](ABI.md) — the calling convention, tuples, `v128`, traps, tail calls,
  continuations.
- [`SAFETY.md`](SAFETY.md) — budgets, bounds, and traps versus `abort`.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) — what each toolkit can do for SIMD.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the dynamic form, its
  descriptor and the nine hard problems.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.3 — `AAS8`/`AAS9`/`AAS11`,
  the gaps this checklist closed.
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6.1.1 — the header tiers that
  decide whether a helper of yours belongs in `WasmBase/` (it does not).
- Source anchors: `WasmBase/WasmModuleInstance.hpp` (`ModuleInstantiator`),
  `WasmBase/WasmBackendSession.hpp` (`BackendSession`),
  `WasmBase/WasmOpcodeDispatcher.hpp`,
  `WasmBase/WasmNullInstantiator.cpp` (the smallest correct
  `declareFunctions`), `WasmBase/WasmBackendAbi.hpp` (the executable tuple
  rule), `LibJit/LibjitModuleCompiler.cpp` (complete), `Sljit/` (provisional),
  `Test/TestBackend.cpp` (the harness).
## 14. Production plan: AsmJit and SLJIT, implemented together

This section is a **plan**, not a claim about the tree. It extends the checklist
above with the order in which the two existing partial backends become
production backends, and with the post-mortem they must leave behind for the
next author. LibJIT remains the reference implementation and the oracle for
every semantic question unless a row below explicitly says otherwise.

### 14.1 Dated starting point

**Snapshot (2026-11-05):**

- `LibJit/` is the complete backend: all lowered opcodes, references/GC,
  exceptions, continuations, SIMD helpers and the full test corpus.
- `Asmjit/` and `Sljit/` execute the same **P0–P3 scalar slice**: i32/i64 and
  f32/f64 arithmetic, conversions, globals, structured control flow (block
  parameters and multi-value block results on SLJIT), direct/indirect/tail
  calls, memory32 and memory64 loads/stores at every scalar width,
  `memory.size`/`grow` at both index widths, bulk memory32 and the table
  operations. Both register under their own name, both implement
  `releaseInstance`, and both pass `[backendabi]`, `[asmjit]`/`[sljit]` and the
  shared `[sljit][module][parity]` fixtures over the same `.wat` modules.
- Neither lowers `v128`/SIMD, references/GC, exceptions, continuations, atomics
  or `f16`, and bulk memory on a memory64 is refused; those stay refused at the
  call boundary. SLJIT additionally caps generated functions at three
  parameters and one result (SLJIT's four-argument call ABI).
- Neither is a replacement for LibJIT.

### 14.2 Why they ship together

Two code generators that share one ABI must be exercised by one conformance
matrix. Shipping a feature in one and leaving the other to "catch up later" is
how backend-specific dialects appear: a trap path that throws here, a reference
that is borrowed there, a tuple offset that disagrees by four bytes. Therefore:

1. Every slice below has the **same entry and exit criteria** for both
   backends, unless a row names a deliberate, documented refusal.
2. A feature is called complete only when it passes the **same fixtures** on
   both backends and on LibJIT where LibJIT defines the semantics.
3. The two implementations are reviewed together; a change to one that needs a
   shared-core change reopens the other's review, not just its build.
4. If one backend cannot support a feature (an ISA/register allocator limit),
   it must expose a `WASMJIT_CAP_*` refusal and a named test — never silently
   lower it differently.

### 14.3 The slices, in implementation order

| Slice | Deliverable | Exit criterion | Deliberate refusal if any |
|---|---|---|---|
| **P0 — baseline** | Both backends build, register and instantiate a module; direct calls, returns, trap slot and stack guard work. | `[backendabi]`, `[asmjit]`, `[sljit]`, install check and plugin metadata pass. | none |
| **P1 — control and stack** | `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`, `select`, multi-value results, block parameters, unreachable polymorphism. | The scalar control fixtures and the matching spec scripts pass; branch/slot ownership has no ASan leak. | none |
| **P2 — memory, tables, globals** | memory32 then memory64, all integer loads/stores, `memory.size`/`grow`, bulk memory/table, globals, table growth and bounds traps. | `[memory]`, `[bulk]`, `[table]` and the selected spec scripts pass; out-of-bounds is the spec trap kind. | atomics may remain refused until P6 |
| **P3 — floating point and conversions** | f32/f64 arithmetic, comparisons, conversions, canonical NaN policy, `v128` load/store and native SIMD where the toolkit exposes vector registers. | float/SIMD fixtures and `[profile-nan]` pass; scalar helper fallback is allowed only where documented. | relaxed SIMD may stay refused |
| **P4 — references, GC and exceptions** | The §6 reference-ownership model, `refClone`/`refDrop`/`refStore`, GC barriers, `ref.test`/`ref.cast`, exception tags and `try_table`. | The initialiser-shape test from §6, `[gc]` reclaim, `[exceptions]` and the ASan leak gate pass. | continuations remain P5 |
| **P5 — calls, tail calls, continuations** | `call_ref`, cross-instance imports, `return_call*` where applicable, the continuation invoker and the stack-switching fixtures. | `[continuation]`, tail-call fixtures, TSan narrowed runs and the unload/re-instantiate test pass. | tail-call elimination may be call+return if documented |
| **P6 — hardening and performance** | ASan/TSan, unload/release, plugin ABI, W^X, benchmark parity, refusal inventory, documentation. | The full §12 definition-of-done table is green for both; every remaining refusal is named in the backend README and a test. | none |

Each slice is a **review gate**, not a calendar promise. P0–P3 (scalar) are
green for both backends against the shared fixtures (2026-11-05 snapshot), with
one documented P2 residual: bulk memory on a memory64 is refused. `v128`/SIMD is
the remaining P3 item, and P4/P5 stay LibJIT-only until it lands. Extend the
`[sljit][module][parity]` fixture set before moving a row to green, and do not
merge the "complete" label for either until its row is green on both.

### 14.4 Tests that are part of every slice

- `[backendabi]` — tuple/layout and calling-convention conformance.
- `[parity]` — one module instantiated with both generators, outcomes compared
  (`... integer module` over `asmjit_i32_basic`, `... floating-point module`
  over `float_basic`, `... memory64 module` over `memory64_basic`).
  Duplicated expectation tables are not a substitute.
- `Test/install-check.sh` — installed headers plus the selected backend.
- `[runtime]` unload/re-instantiate — executable memory is returned.
- `[plugin]` — the backend behind the C descriptor, if the plugin build is
  enabled.
- ASan `detect_leaks=1` with `Test/asan.supp`; TSan `[multiagent]` and
  `[concurrency]` for anything touching agents, GC or continuations.
- A **backend-specific refusal test** for every row above that is not green.
  A refusal is a testable feature: "no compiled body" must be the observed
  result, not a crash, a silent zero or a host `abort()`.

### 14.5 The mandatory post-mortem

At the end of each slice, and again when the backend is declared complete, the
author writes a post-mortem **for both backends, in one document**, under
`docs/backends/` (or as a section of the backend README until that directory is
created). The post-mortem is part of the definition of done; a backend without
it is still provisional. It must contain:

1. **What the shared ABI forced.** Which rules from §6 and [`ABI.md`](ABI.md)
   were ambiguous, and how the implementation resolved them. A rule that had to
   be interpreted twice is a documentation bug.
2. **What broke, with the failing fixture.** Every miscompile, wrong trap kind,
   reference under/over-count, tuple offset, stack-switch or unload bug. Link
   the test that pins it; do not summarize it away.
3. **What the toolkit could not express.** Register-pressure limits, missing
   branch/call primitives, relocation or W^X constraints, SIMD gaps. State the
   workaround and whether it is an accepted limitation or a temporary refusal.
4. **What the tests did not catch.** Name the gap and the new test it produced.
   This is the most valuable section for the fourth backend.
5. **What was slower, and by how much.** A measured comparison on the SIMD
   benchmark or a scalar loop; no unqualified performance claim.
6. **What a fourth backend should copy, and what it must not.** Turn the
   experience into additions or corrections to §3–§12 of this note.
7. **Residual refusals.** A table mapping each unsupported opcode family to the
   capability flag, the test and the user-visible error.
8. **Sign-off.** The slice, the commit, the commands run and their counts.

The post-mortem is written by the implementer, reviewed by the backend
reviewer, and kept current: when a later slice invalidates a paragraph, update
the paragraph rather than appending a correction footnote. The nine so far are
[`docs/backends/ASMJIT_SLJIT_P0_P2.md`](backends/ASMJIT_SLJIT_P0_P2.md),
[`docs/backends/ASMJIT_SLJIT_P3.md`](backends/ASMJIT_SLJIT_P3.md),
[`docs/backends/ASMJIT_SLJIT_P2_MEMORY64.md`](backends/ASMJIT_SLJIT_P2_MEMORY64.md),
[`docs/backends/ASMJIT_SLJIT_P3_SIMD1.md`](backends/ASMJIT_SLJIT_P3_SIMD1.md),
[`docs/backends/ASMJIT_SLJIT_P3_SIMD2.md`](backends/ASMJIT_SLJIT_P3_SIMD2.md),
[`docs/backends/ASMJIT_SLJIT_P3_SIMD3.md`](backends/ASMJIT_SLJIT_P3_SIMD3.md),
[`docs/backends/ASMJIT_SLJIT_P3_SIMD4.md`](backends/ASMJIT_SLJIT_P3_SIMD4.md),
[`docs/backends/ASMJIT_SLJIT_P3_SIMD5.md`](backends/ASMJIT_SLJIT_P3_SIMD5.md) and
[`docs/backends/ASMJIT_SLJIT_P3_SIMD6.md`](backends/ASMJIT_SLJIT_P3_SIMD6.md).

### 14.6 Candidate backends for a fourth implementation

These are candidates for **after** AsmJit and SLJIT reach the same P6 gate. The
ordering is a recommendation, not a commitment; each candidate must first pass
the §2 layering test and the §12 baseline.

| Candidate | Why it fits | Main risk | Suggested role |
|---|---|---|---|
| **LLVM** (MCJIT/ORC) | Mature optimizer, native floating point/SIMD, broad target support; the closest path to competitive code. | Large dependency, long compile latency, ORC lifetime/threading, W^X and target initialization. | Production-quality second compiler, not a CI default. |
| **libgccjit** | GCC's optimizer behind a small C API; simpler embedding than LLVM; good scalar/float code. | Less mature JIT ABI, GCC-version coupling, limited control over codegen and latency. | A lighter optimized backend for embedders already using GCC. |
| **GNU Lightning** | Tiny, portable, fast to emit; a natural next step after SLJIT's RISC-like model. | Small opcode set, weaker optimization, limited SIMD/reference support. | Fast-start embeddable backend with a documented feature ceiling. |
| **DynASM** | Very low overhead, LuaJIT-proven, excellent for trace/loop-shaped code. | Hand-written assembler per target, no register allocator or optimizer, high maintenance. | A specialized hot-loop backend, not a general module compiler. |
| **Cranelift** | Rust, SSA-based, designed for Wasm; good optimization/latency balance. | Rust/C++ ABI and build integration, additional toolchain, less familiar debugging. | A third production candidate if a Rust component is acceptable. |
| **MIR** | Lightweight C, SSA, simple embedding, good for a compact backend. | Smaller ecosystem and target coverage; fewer Wasm-specific precedents. | A compact alternative to libgccjit. |

The first three are the realistic near-term list. LLVM and libgccjit are
"optimizing" backends; Lightning is "small and portable"; DynASM is "hot-loop
specialist". None may be added to `WasmBase/`: the core continues to name only
`ModuleInstantiator` and `BackendSession`.

### 14.7 Entry checklist for the fourth backend author

Before writing a line of code, read §3, §6 and §12; then produce:

- a backend directory whose `.pro` links the one shared `libWasmBase.so`;
- a `ModuleInstantiator` with the four hooks and no core changes;
- a `BackendSession` with `invoke`, `aggregateLayout` and `createLaneWiseOp`;
- the refusal inventory (which opcode families are not lowered yet);
- the P0 baseline tests and a first `[<name>]` test case;
- the post-mortem template above, opened on day one rather than at the end.

The measure of success is not "it runs the benchmark". It is that another
author can read this note, the new backend's README and its post-mortem, and
write the next backend without rediscovering the same ABI or ownership failure.
