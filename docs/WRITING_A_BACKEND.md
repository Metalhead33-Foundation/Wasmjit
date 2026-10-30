# Writing a backend, step by step {#doc_writing_a_backend}

Status: how-to note (written 2026-10-30, when `Sljit/` stopped being an empty
subproject). It is the practical companion to
[`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md): that note is the *checklist* a
code generator must satisfy, this one is the *walkthrough* — which class to
subclass, which method to implement, in what order, and what "done" looks like
for a provisional first cut. It contains no engine behaviour of its own.

**If you only read one section**, read §3 (the four hooks), §6 (the ABI you emit)
and §9 (what the SLJIT scaffold does and deliberately does not do).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What is a backend, and what is not mine to write? |
| §3 | Which abstract class must be subclassed, and which four methods? |
| §4 | How does the core drive those four hooks? |
| §5 | How do I decode a function body without writing a decoder? |
| §6 | What ABI must the generated code follow? |
| §7 | Which interface must the backend *expose* (`BackendSession`)? |
| §8 | How is the backend packaged, registered and selected? |
| §9 | What does the SLJIT scaffold contain, and what does it not do yet? |
| §10 | How do I test it, and what counts as done? |
| §11 | References |

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
   jumps through null. This is the honest way to be partial (§9).
4. **Nothing throws on a module-caused failure.** Report through the trap slot
   and return; a C++ exception below generated code cannot unwind through it
   ([`SAFETY.md`](SAFETY.md) §2.4–§2.6).

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
   none.
4. **A session factory lives with the backend**, not in the core:

   ```cpp
   namespace MyBackend { std::unique_ptr<WASM::BackendSession> createSession(); }
   ```

   The name selector (`WASMJIT_BACKEND`) belongs to whoever ships the backend —
   `Test/TestBackend.cpp` in-tree, a plugin's `create` otherwise.
5. **Declare what you can do** and refuse the rest *before* codegen: a
   capability flag or a documented limitation. `Runtime::create("name")` is the
   public entry point; `WASM::createBackendInstance("name")` is the registry
   entry point an embedder with no `Runtime` uses.



## 9. The SLJIT scaffold — a worked example, and its honest limits

`Sljit/` is the second backend, deliberately **provisional**. It exists to make
the seam concrete and to give the vector work a real starting point, not to
claim parity with LibJIT.

| File | Role |
|---|---|
| [`SljitBackend.hpp`](../Sljit/SljitBackend.hpp) / `.cpp` | `createModuleCompiler()`, `createSession()`, `WASMJIT_REGISTER_BACKEND("sljit", …)` |
| [`SljitModuleCompiler.hpp`](../Sljit/SljitModuleCompiler.hpp) / `.cpp` | the four `ModuleInstantiator` hooks, the start-function call, and the per-function compile loop |
| [`SljitOpcodeDispatcher.hpp`](../Sljit/SljitOpcodeDispatcher.hpp) / `.cpp` | the body emitter: frame slots, the operand-stack model, the prologue/trap epilogue |
| [`SljitBackendSession.hpp`](../Sljit/SljitBackendSession.hpp) / `.cpp` | `BackendSession`: `aggregateLayout`, `createLaneWiseOp`, `invoke` (libffi) |

**What it executes today** (and therefore what the tests may assert):

- up to **three** Wasm parameters, each `i32` or `i64`, plus `VMContext*`
  (SLJIT's `sljit_emit_enter` accepts at most four arguments);
- zero or one result, `i32` or `i64`;
- `local.get`, `local.set`, `local.tee`, `i32.const`, `i64.const`,
  `i32`/`i64` `add`/`sub`/`mul`/`and`/`or`/`xor`/`shl`/`shr_s`/`shr_u`,
  `drop`, `nop`, `return`, and the implicit `end`;
- the stack guard and a pending-trap epilogue, so a call reports a
  `CallStackExhausted` trap as a pending status rather than crashing.

**What it refuses, loudly and safely:** any other opcode (control flow, calls,
memory, globals, floating point, references, SIMD, exceptions) leaves that
function's `Callable::fnPtr` **null**. The core then refuses the call at the
boundary with the existing "no compiled body" error. Nothing is miscompiled,
nothing is interpreted, and no half-emitted code is ever installed.

**Known limitations, recorded rather than hidden:**

| Limitation | Why | What replaces it |
|---|---|---|
| no control flow | `block`/`loop`/`if`/`br` need labels and a merge model; a half-implemented branch is worse than none | later |
| no `call`/`call_indirect` | needs the call ABI plus type checks; direct calls are the next step | later |
| no memory/globals/SIMD/exceptions | the helper layer and the GC layout are whole projects of their own | LibJIT |
| references are refused, not half-owned | neither this scaffold nor Asmjit lowers `ref.*`/`struct.*`/`array.*`; a reference it moves without counting is worse than one it refuses. When either adds them, [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §6 “Reference ownership” is the rule to implement, not an optional refinement | LibJIT (`emitReferenceParameterRetains`) |
| generated code is never freed | `ModuleInstantiator` has no instance-teardown hook, so there is nowhere safe to `sljit_free_code` | documented leak; LibJIT keeps its context alive the same way |
| one executable mapping per function | the W^X allocator maps each block separately | acceptable for a provisional backend; the default pooled allocator trades W^X for density |

`Sljit.pro` compiles the vendored `extern/sljit` sources into `libSljit.so` — no
external `-lsljit` — and defines `SLJIT_WX_EXECUTABLE_ALLOCATOR=1` so the
generated code goes through the read-write → read-execute handoff instead of
asking the host for RWX pages.

## 10. Testing, and what counts as done

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
exactly those five steps. The broader "definition of done" — the generic
fixtures, trap/GC/exception fixtures, the installed-header consumer and the
plugin load check — is [`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md) §8, and
a provisional backend is expected to fail most of it. That is compatible with
this note only if the failure is **documented and refused**, not silently wrong.

## 11. References

- [`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md) — the requirement checklist;
  this note is its walkthrough.
- [`LAYERING.md`](LAYERING.md) — what the core may name (nothing of yours).
- [`ABI.md`](ABI.md) — the calling convention, tuples, `v128`, traps.
- [`SAFETY.md`](SAFETY.md) — budgets, bounds, and traps versus `abort`.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) — what each toolkit can do for SIMD.
- Source anchors: `WasmBase/WasmModuleInstance.hpp`,
  `WasmBase/WasmBackendSession.hpp`, `WasmBase/WasmOpcodeDispatcher.hpp`,
  `WasmBase/WasmNullInstantiator.cpp`, `LibJit/LibjitModuleCompiler.cpp`
  (complete), `Sljit/` (provisional), `Test/TestBackend.cpp` (the harness).
