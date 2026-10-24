# Stack Switching (Continuations) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 10 — SS-0/SS-1/SS-2,
the per-execution-context Agent/ExecutionState refactor (SS-6), the A
native-stack provider (default), the B host-thread fallback, the
host-facing `HostContinuation` handle, multiple/loop handler dispatch,
`cont.bind`, `resume_throw`/`resume_throw_ref` (without handler clauses so far),
A cross-thread serial migration, and ASAN fiber annotations are implemented.
The still-narrow executable slice keeps i32/void payloads and does not yet
implement `switch`, multi-value/reference payloads, or load a root list for a
tracing collector. §10.15 records the runtime decision: native stacks (A) are
primary, host threads (B) are the opt-in fallback, and a bespoke interpreter
fallback (C) is permanently rejected. True multi-agent execution remains
deferred.)
Audience: anyone touching `WasmModule` (types), `WasmType` (heap types),
`WasmOpcode`, `WasmStore`, `WasmModuleInstance`, the JIT compiler, a backend's
continuation lowering, or the exception-tag machinery.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Revision 1 contained no code; revisions 2–4
implement the continuation type, heap and tag model, revision 6 adds the
per-execution-context state and the runtime decision, revision 7 propagates
the no-bespoke-interpreter policy to the project-level documents, revision 8
adds the B-provider runtime and its instruction lowering, and revision 9 adds
the A provider (default), the host-facing continuation handle, multiple/loop
handlers, `cont.bind`, injected exceptions, cross-thread A migration and ASAN
fiber annotations.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the proposal require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |
| §10 | Which execution mechanisms are viable on each of the three backends, what each costs, and what the core must own before one can start? |

## 2. What the proposal demands

Stack Switching adds typed continuations: a snapshot of execution on a stack that
can be suspended and resumed, enabling coroutines, generators, async/await and
lightweight tasks.

- A new composite type `cont $ft` (opcode `-0x23`, byte `0x5D`), where `$ft` is
  the function type of the continuation.
- Two new heap types: `cont` (`-0x18`) and `nocont` (`-0x0b`).
- Reuse of **tags** from exception handling, but with a continuation signature:
  the tag describes the values passed into and out of a suspension.
- Seven instructions (`0xE0`–`0xE6`):

| Opcode | Instruction | Role |
|---|---|---|
| `0xE0` | `cont.new $ct` | capture a `(ref $ft)` as a new continuation |
| `0xE1` | `cont.bind $ct $ct'` | partially apply a continuation |
| `0xE2` | `suspend $t` | suspend the current continuation on a tag |
| `0xE3` | `resume $ct hdl*` | resume a continuation under handlers |
| `0xE4` | `resume_throw $ct $e hdl*` | resume by throwing into a continuation |
| `0xE5` | `resume_throw_ref $ct hdl*` | the `exnref` form of the same |
| `0xE6` | `switch $ct $t` | symmetric switch to a peer continuation |

- Casting to a continuation type is invalid: `ref.test`, `ref.cast`,
  `br_on_cast` and `br_on_cast_fail` reject a target in the `cont` hierarchy
  (`rt castable` iff `rt` is not `<: (ref null cont)`).
- Handler clauses are `(on $tag $label)` or `(on $tag switch)`; a resumed
  continuation is spliced onto the current one (asymmetric) or swapped with a
  peer (symmetric).
- Continuations are **one-shot** in the MVP: resuming a consumed continuation
  traps.

## 3. What the proposal does not demand

- No stack representation: copied stacks, segmented stacks or native stacks are
  all allowed.
- No scheduling policy and no thread semantics; switching is explicit.
- No multi-shot continuations in the MVP.
- No particular handler compilation, no unwind-table requirement.
- No change to the meaning of ordinary calls or exceptions; a continuation is
  not an exception object.

**Project policy, not a proposal constraint:** WasmJit will not add a bespoke
interpreter fallback. If interpretation is ever required it must be the
backend's own compile/interpret configuration (GNU LibJIT already provides an
interpreter), not a new engine; `WasmStub/` is a pretty-printer, not a runtime.
The decision applies engine-wide and is recorded with its reasoning in §10.15
(D-3).

## 4. What we have implemented today

- **SS-0 / SS-3: the A and B providers execute the instruction set below.**
  `cont.new`, `cont.bind`, `suspend`, `resume`, `resume_throw` and
  `resume_throw_ref` are decoded, validated and lowered by LibJIT. `switch`
  (`0xE6`) remains a named opcode and fails with a named
  `stack-switching instruction is not implemented` error. **A (native stacks)
  is the default; B (host threads) is the explicit fallback.**
- **SS-1's type model is implemented.** `0x5D` parses as `ContType { funcType }`;
  `CanonTypeKind::Cont`, a canonical `contFunc` reference, `CoreCont` payloads
  and `AbstractHeapType::Cont`/`NoCont` (`0x68`/`0x75`) are represented. A
  `(ref null cont)` parameter, result or block type decodes and canonicalises
  correctly. The proposal's shorthand `0x68` for `(ref null cont)` is modelled
  as `ValueTypeCode::ContRef` and canonicalises to the same `(ref null cont)`
  as the explicit form.
- **SS-2 is implemented.** Exception tags exist (`TagId`, tag section,
  imports/exports, `throw` / `try_table`); a tag type is a function type whose
  result signature may be non-empty (the control-tag form), `attribute` must be
  0, and a result-bearing tag exports/imports by the same process-wide
  `TagId`. The upstream rule that one control tag may be used by `throw`,
  `suspend` and `switch` holds: `throw` and a `try_table` `catch`/`catch_ref`
  consume the tag's parameters and ignore its result signature, so a
  result-bearing tag is also a valid exception tag.
- **SS-6 is implemented.** `Agent` owns an `ExecutionState` (trap slot, pending
  exception, tail-call state, deferred cleanup, depth counters and stack
  limit); a host thread holds only a pointer to the current state. A
  continuation worker installs its **own** `ExecutionState`, so a trap,
  exception, tail-call target or defer record never leaks into the resumer.
- **The providers live in
  [`WasmContinuation.hpp`](../../WasmBase/WasmContinuation.hpp).** The contract
  is backend-neutral; `LibJit/` only emits the calls and branches on the
  returned status.
  - **A — native stacks (default):** an mmap-backed stack with a `PROT_NONE`
    guard page, a `ucontext` switch, and a per-stack
    `ExecutionState`/`stackLimit`. A can be resumed/dropped on a different host
    thread when the calls are serial (the engine TLS and LibJIT state travel
    with the stack); concurrent use is not allowed. ASAN fiber annotations are
    called on every switch when the sanitizer is linked.
  - **B — host threads (explicit fallback):** one host thread per suspended
    continuation, a pthread stack charged to the budget, and a condition
    variable hand-off.
  - Both enforce one-shot semantics with a per-worker generation plus a
    consumed flag; transfer a trap (kind + copied message) or an uncaught
    exception to the resumer's `ExecutionState`; bound live continuations and
    charge stack memory against the Store budget (`OutOfMemory` on refusal);
    and unwind a dropped suspended continuation (A) or cancel and join its
    worker (B).
- **Host-facing handle.** `HostContinuation` wraps a `(ref cont)` lowered to
  `void*` (for example, an exported function result), owns one reference, and
  offers `resume` plus `suspendCurrent` for a native import. `HostContinuationScheduler`
  adds the opt-in, engine-agnostic JSPI/Lua/Python hook contract: `onSuspended`
  receives the replacement handle and payload, and `onCompleted` receives the
  final status. This is the JSPI/embedder seam and the groundwork for a future
  scheduler; it is **not** an agent API and does not add concurrency.
- **The validator and lowering are deliberately narrow.** At most one i32
  parameter and one i32 result (or void) for the continuation function. Multiple
  `(on $tag $label)` clauses and loop handler labels are supported; `cont.bind`
  accepts an i32 prefix and applies to new or suspended continuations;
  `resume_throw`/`resume_throw_ref` currently accept no
  handler clauses and inject the exception at the suspension point. `switch`,
  multi-value/reference payloads and the rest remain unimplemented. A
  suspended stack's references are kept alive by the engine's refcounts; a
  tracing collector would additionally need the §10.9 root list.
- **Execution remains single-agent.** Ordinary calls use the native C stack;
  A switches continuations on one host thread, and B's worker threads are
  execution contexts, **not agents**. Neither provider permits concurrent
  entry into an instance, and THREADS TH-5 remains deferred indefinitely.
- **Tests:** runtime coverage now includes both providers, the host handle,
  trap/exception transfer, one-shot/budget, A stack exhaustion and serial
  cross-thread migration; the WAT modules cover `cont.new`/`resume`, an
  argument, a handled suspension, an unhandled suspension, multiple handlers,
  a loop handler, `cont.bind`, `resume_throw` and an exported continuation.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~SS1~~ | ~~**No continuation composite type.**~~ **SS-1 done:** `0x5D` parses into `ContType`, canonicalises to `CanonTypeKind::Cont` with a `contFunc` reference, and no longer desynchronises the type section. | `WasmType.hpp` (`ContType`), `WasmModule.cpp`, `WasmTypeRegistry.cpp` | Closed (model) |
| ~~SS2~~ | ~~**No `cont`/`nocont` heap types.**~~ **SS-1 done:** `AbstractHeapType::Cont`/`NoCont` and `(ref null cont)` canonicalisation are implemented. | `WasmType.hpp` (`AbstractHeapType`), `WasmValidator.cpp` | Closed (model) |
| ~~SS3~~ | ~~**No continuation instructions.**~~ **SS-0 + stage-2 done for three:** `cont.new`, `suspend` and `resume` execute; `cont.bind`, `resume_throw*` and `switch` remain named opcodes that fail with a named error. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp`, `LibJit/LibjitOpcodeDispatcher.cpp` | Closed for three; open for the rest |
| SS4 | **The runtime is a narrow A/B implementation, not the full continuation model.** It supports i32/void payloads, multiple handlers, loop handler labels, `cont.bind` (i32 prefix, on new and suspended continuations), `resume_throw*` at a suspension point (no handler clauses yet) and serial cross-thread A migration. It still lacks `switch`, multi-value/reference payloads, full `resume_throw` handler clauses and a tracing-collector root list. | `WasmBase/WasmContinuation.hpp`/`.cpp`, `LibJit/LibjitOpcodeDispatcher.cpp` | High for full conformance; closed for the implemented A/B slice |
| ~~SS5~~ | ~~**Tags carry no continuation signature.**~~ **SS-2 done:** tag types are function types and may carry a non-empty result signature; `attribute` is checked, exception uses consume the parameter signature and ignore the results, and imports/exports match by `TagId`. | `WasmValidator.cpp` (`validateModule`); `WasmModule.cpp` (`processTagSection`); `WasmRegistryImportResolver.cpp` (`resolveTag`) | Closed |
| ~~SS6~~ | ~~**No test.**~~ **Stage-2 done:** positive `cont.new`/`resume`/`suspend` modules plus runtime tests for one-shot, trap/exception transfer and the budget; the original model/rejection tests remain. | `Test/main.cpp`, `Test/wasm_wat/cont_*.wat` | Closed (stage-2 slice) |
| ~~SS7~~ | ~~**Execution state is per host thread, not per execution context.** The trap slot, pending exception, tail-call state, defer stack, depth counters and stack limit are TLS or global, and there is no agent object to own them.~~ **SS-6 done:** `Agent` owns an `ExecutionState`; the generated-code helpers resolve the current state; two agents on one thread, per-thread defaults and the per-state stack limit are tested. | `WasmBase/WasmAgent.hpp`/`.cpp`; `WasmTrap.cpp`, `WasmExn.cpp`, `WasmStack.cpp`; `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SSD1 | **Keep `0xE1` and `0xE4`–`0xE6` rejected until the rest of the runtime exists**, while `0xE0`/`0xE2`/`0xE3` execute through the A/B providers. | §3: the stack representation and encoding are free; `SAFETY.md` forbids guessing the execution semantics, and a named failure is better than a dead stack. | The type, tag and three-instruction surfaces are usable without pretending the full instruction set works. |
| SSD2 | **Reuse the exception-tag identity; encode a control tag as an ordinary function type with results.** | §2/§3: the tag's result signature is the proposal's control-tag form, and the same tag may be thrown, suspended and switched on; `EXCEPTIONS.md` EX-2 already gives tags a process-wide `TagId`. | One tag registry for both proposals, and exception semantics stay unchanged (results ignored). |
| SSD3 | **Keep continuations one-shot.** | §3: multi-shot is explicitly out of the MVP. | No stack-copying collector. |
| SSD4 | **Do not emulate continuations with heap-allocated state machines.** | §3 leaves the mechanism free, but §2 requires suspending the actual execution state; a state-machine rewrite changes observable one-shot/splice semantics. | No false conformance. |
| SSD5 | **Let the backend choose the stack representation** (native copy or heap stack) behind a core contract. | §3: stack representation is free; `../LAYERING.md` keeps the contract in the core and the implementation in the backend. | LibJIT and future backends can differ. |

Guardrail: a continuation must resume exactly once. If the runtime cannot
observe a consumed continuation, it must reject the feature rather than let a
second `resume` read a dead stack.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SS-0 — Honest rejection** | ✅ `0xE0`–`0xE6` are named opcodes. Stage 2 executes `0xE0`/`0xE2`/`0xE3`; the other four still fail with a named error. | SS3, SS6 | A continuation module fails cleanly, without desync; the supported three execute. |
| **SS-1 — Types** | ✅ `ContType`, `CanonTypeKind::Cont`, canonical `contFunc`, `CoreCont` payloads and `AbstractHeapType::Cont`/`NoCont`. | SS1, SS2 | The type section decodes and canonicalises a continuation type. |
| **SS-2 — Tag signatures** | ✅ Tag types may carry a non-empty result signature (the control form); `attribute` is checked, exception uses consume the parameters and ignore the results, and imports/exports match by `TagId`. | SS5 | A result-bearing tag validates, is thrown/caught (payload = parameters), and exports/imports by identity. |
| **SS-6 — Execution state** | ✅ `Agent` owns an `ExecutionState` (trap slot, pending exception, tail-call state, defer stack, depth counters, stack limit); TLS holds the current state pointer; the helpers resolve it; isolation tests cover two agents on one thread, per-thread defaults and the per-state stack limit. | SS7 | Done: `Test/main.cpp` ("agent execution state…", "each host thread…", "the stack limit…"). |
| **SS-3 — Runtime stacks** | ✅ **A and B implemented:** A is the default (mmap-backed stack, guard page, `ucontext` switch, per-stack state/limit, ASAN fiber hooks, serial cross-thread migration); B is the explicit fallback (one host thread per suspended continuation). Both enforce one-shot, transfer traps/exceptions, unwind/cancel on drop and charge the budget. A's references are kept alive by refcounts; a tracing collector would still need a root list. | SS4 | Suspend/resume round-trips run on both providers; A stack exhaustion traps; serial migration is tested. |
| **SS-4 — Instructions** | ◑ **Narrow slice:** `cont.new`, `cont.bind`, `suspend`, `resume` with multiple/loop handlers, and `resume_throw*` without handler clauses; i32/void signatures. Remaining: `switch`, `resume_throw*` handler clauses, multi-value/reference payloads, suspension propagation. | SS3 | The supported instructions execute; `switch` and the wider value model do not. |
| **SS-5 — Tests** | ✅ A/B runtime tests, the host handle, A stack guard/migration, multiple/loop handlers, `cont.bind`, `resume_throw`, WAT modules and the model/rejection surface are in the default suite. | SS6 | `10,582` assertions / `409` cases green. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-24; whole suite 10,582 assertions /
409 test cases):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `stack-switching continuation types...` | 18 assertions | 0 | 0 | SS-0/SS-1: `cont`, `(ref null cont)` and `(ref null nocont)` decode; a still-unimplemented continuation instruction fails with a named error; `0x68` is now the modelled shorthand |
| `stack-switching control tags...` | 17 assertions | 0 | 0 | SS-2: a result-bearing tag validates and `throw`/`catch` returns its parameter payload; a non-zero attribute is rejected; continuation cast targets are rejected; the tag exports/imports by `TagId` |
| `continuation runtime...` (4 cases) | 38 assertions | 0 | 0 | A default: completion, suspend/resume, trap/exception transfer, one-shot and the live budget |
| `continuation provider...` | 5 assertions | 0 | 0 | A is the default; B can be selected explicitly and still completes |
| `host-thread fallback...` | 9 assertions | 0 | 0 | B suspend/resume hand-off across host threads |
| `host continuation handle...` (2 cases) | 24 assertions | 0 | 0 | `HostContinuation` wraps a native-created or exported continuation, resumes it, and supports `suspendCurrent` in a native import |
| `host continuation scheduler...` | 15 assertions | 0 | 0 | The embedder-owned `HostContinuationScheduler` calls `onSuspended`/`onCompleted` and resumes the replacement handle |
| `native-stack continuation exhaustion...` | 7 assertions | 0 | 0 | A's guarded mmap stack reports `CallStackExhausted`, not SIGSEGV |
| `native-stack continuation can migrate...` | 7 assertions | 0 | 0 | A is resumed serially on a different host thread; the engine/backend TLS travels with the stack |
| `stack-switching dispatches multiple handlers and a loop handler` | 8 assertions | 0 | 0 | Multiple `(on $tag $label)` clauses and a loop-labelled handler receive `[payload (ref null cont)]` |
| `stack-switching binds a continuation argument prefix` | 4 assertions | 0 | 0 | `cont.bind` fixes the first of two i32 parameters; `resume` supplies the rest |
| `stack-switching binds a suspended continuation` | 4 assertions | 0 | 0 | A suspended continuation's tag result is supplied by `cont.bind` |
| `stack-switching injects an exception...` | 4 assertions | 0 | 0 | `resume_throw` raises `$abort` at the continuation's suspension point; its `try_table` catches it |
| `stack-switching module...` (4 cases) | 14 assertions | 0 | 0 | `cont.new` + `resume` completion, an i32 argument through `resume`, a handled suspension, and an unhandled suspension that drops the replacement handle and traps |
| `agent execution state isolates...` | 35 assertions | 0 | 0 | SS-6: trap, pending exception, tail-call target, depth counters and deferred cleanup are per agent |
| `each host thread gets its own default agent...` | 5 assertions | 0 | 0 | SS-6: two host threads get distinct default agents and do not share a trap |
| `the stack limit belongs to the execution state` | 3 assertions | 0 | 0 | SS-6: `wasm_stack_check` reads the current state's limit, not the shared `VMContext` field |

Reading: the type, tag, execution-state, rejection, A/B runtime and host-handle
surfaces are graded. Handler multiplicity/loop labels, `cont.bind`,
`resume_throw` (no handler clauses), A stack exhaustion and serial A migration
are verified. `switch`, multi-value/reference payloads, full `resume_throw`
handler clauses and a tracing-collector root list are not covered because they
do not exist yet. True multi-agent execution is not tested or claimed.

## 9. References

- Proposal repository and explainer (`WebAssembly/stack-switching`):
  https://github.com/WebAssembly/stack-switching
  (https://github.com/WebAssembly/stack-switching/blob/main/proposals/stack-switching/Explainer.md)
- Spec — Exception tags (the reused identity):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`EXCEPTIONS.md`](EXCEPTIONS.md) (tags, `TagId`), [`TAILCALLS.md`](TAILCALLS.md)
  (native-stack trampoline), [`THREADS.md`](THREADS.md) (single-agent model),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), [`../ABI.md`](../ABI.md),
  [`../LAYERING.md`](../LAYERING.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`AbstractHeapType`, `Subtype`),
  `WasmBase/WasmModule.cpp` (`processSubtypes`, `processTagSection`),
  `WasmBase/WasmOpcode.hpp`, `WasmBase/WasmOpcodeDispatcher.cpp`,
  `LibJit/LibjitModuleCompiler.cpp`.
- Backend feasibility sources and the mechanism study are in §10.14.


## 10. Feasibility against the three backends

> **Status of this appendix.** This is a *pre-commitment* study, written after
> SS-2 closed. It claims no execution support and adds no milestone to §7. It
> answers one question: given the observable semantics the proposal fixes, which
> execution mechanisms are viable on the three designated backends, what does
> each cost, and what must the core own before any of them can start.
>
> `LibJit/` is the only backend that executes today. `Asmjit/` and `Sljit/`
> contain no backend sources (`Asmjit/Asmjit.pro`, `Sljit/Sljit.pro`; `Sljit/`
> ships the upstream `libSljit.so*`), so their rows are **API-level**
> assessments, not measurements. §10.11 states the minimal backend slice a
> measurement needs.

### 10.1 The two boxes

The proposal fixes observable behaviour tightly and leaves the mechanism open:

- **Fixed:** one-shot continuations (a second resume traps), splicing of a
  resumed continuation onto the current one, symmetric `switch`, handler search
  by tag *and* event kind (`(on $t $l)` handles `suspend`, `(on $t switch)`
  handles `switch`), the tag parameter/result signatures, `resume_throw` /
  `resume_throw_ref`, null/consumed continuation traps, the type rules and the
  cast ban (§2). These are not implementation choices.
- **Free (§3):** copied, segmented or native stacks; the handler compilation
  strategy; whether an engine emits unwind tables; scheduling and threads; the
  stack representation behind the instruction. This is what makes the matrix in
  §10.4 meaningful.

The engine adds a third, smaller box of its own: JIT frames with no unwind
tables, traps as a pending status that unwinds by returning
([`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §8.5), an RC + cycle GC with no
stack maps, a single-mutator instance model, and a stack guard expressed as one
integer in `VMContext`. Those constraints, not the proposal, eliminate half the
mechanisms below.

### 10.2 Mechanism catalogue

Six mechanism families are considered. A family can have several providers (for
example `ucontext`, Boost.Context or a per-architecture context switch for A).
They are not equally honest: D and F cannot preserve the semantics or the JIT's
frame model, and E needs compiler support the engine does not have.

| # | Mechanism | How `suspend` works | Stack representation | What it requires |
|---|---|---|---|---|
| **A** | **Separate native stacks + context switch** | A helper called from generated code saves the current context, switches to the resumer, and returns when the continuation is resumed | One native stack per continuation (mmap'd or provider-allocated), with a guard page | A core context-switch contract, per-context state, a stack allocator/budget, and a suspend-point root list |
| **B** | **One host thread per suspended continuation** | The helper blocks the continuation thread on a condition variable and wakes the resumer; a resume wakes the continuation and blocks the resumer | The OS thread's native stack | A hand-off protocol, one mutator at a time, trap/exception transfer, and a thread/stack budget; no codegen change |
| **C** | **Interpreter fallback for continuation-using modules** | The interpreter's dispatch loop returns a suspension record | Heap frames / an explicit value stack | A second execution engine and a JIT/interpreter call ABI |
| **D** | **Native stack copying** | Copies the live stack bytes out and restores them on resume | A byte buffer per continuation | Frame maps and pointer fix-ups for every live pointer — absent in all three backends |
| **E** | **Segmented stacks / stacklets** | Switches to a new stack segment at a call boundary | A chain of fixed-size segments | A stacklet-aware calling convention and a stack check at every call; only AsmJit could plausibly emit it |
| **F** | **CPS / asyncify-style transformation** | Rewrites the function into a resumable state machine | No stack capture | A module rewrite that preserves one-shot, `switch`, `resume_throw` and host-import behaviour — it does not |

A and B are the only members that keep the JIT's native frames and the current
trap model without writing a second engine. They differ in *where* the
per-execution state lives, not in the Wasm semantics: A multiplexes contexts on
one host thread; B gives each suspended continuation its own thread. C is
**rejected by project decision** (§10.15): it would be a second execution
engine, and WasmJit will not carry a bespoke interpreter fallback. D and F are
rejected on engine and semantic grounds, and E is an AsmJit-only research item,
not an MVP candidate.

### 10.3 What each engine actually offers

Evidence: the installed GNU LibJIT headers and library
(`/usr/local/include/jit`, `/usr/local/lib64/libjit.so.0.0.1`); the upstream
SLJIT public header (`sljit_src/sljitLir.h`, read for `sljit_emit_enter`,
`SLJIT_SP`, `sljit_get_local_base` and `SLJIT_CALL_RETURN`); the upstream AsmJit
`asmjit/core/func.h` (`FuncFrame`/`FuncDetail` and the prolog/epilog inserter);
and the tree's own surveys in [`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §10.1
and [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md) §3. `Asmjit/` and `Sljit/`
contain no backend sources, so only LibJIT's row is executable today.

| Capability | GNU LibJIT (installed, primary) | SLJIT (scaffold) | AsmJit (scaffold) |
|---|---|---|---|
| State in the tree | `LibJit/`, the only executing backend | `Sljit/Sljit.pro` + upstream `libSljit.so*`; no sources | `Asmjit/Asmjit.pro`; no sources |
| Value/frame model | typed `jit_value_t`, function-local slots, explicit signatures | portable register machine; `sljit_emit_enter` reserves a **fixed local area**; the caller owns the layout | assembler/IR; `Compiler` adds register allocation; `FuncFrame` describes the frame |
| Stack-pointer control | none exposed; generated functions are ordinary C-ABI functions | `SLJIT_SP` is addressable only as `SLJIT_MEM1(SLJIT_SP)`; `sljit_get_local_base` materialises the real address of a local slot | `FuncFrame` exposes call/local/final stack size, stack alignment, red-zone and spill-zone sizes, and a preserved frame pointer (`has_preserved_fp`) |
| Calls / entry | `jit_insn_call_native`, `jit_insn_call_indirect` + `jit_type_create_signature` | `sljit_emit_call` / `sljit_emit_icall`, `SLJIT_CALL` / `SLJIT_CALL_REG_ARG` | `invoke` with `FuncSignature`/`FuncDetail`; prolog/epilog via PEI |
| Root storage at a call | `jit_value_create` slots; the dispatcher already knows the live locals and operand stack, so it can spill refs into a frame array | an explicit offset into the local area; `sljit_get_local_base` gives a pointer to it | explicit frame slots; the most direct of the three |
| Stack check | the backend already calls `wasm_stack_check(ctx)` on entry | must emit the call in the prologue | must emit the call in the prologue |
| Tail calls | per-function driver/trampoline with a 16-slot `TailCallState` and a result buffer | `SLJIT_CALL_RETURN` | `jmp` or a custom driver |
| Unwind info / CFI | none (`jit-unwind.h` is LibJIT's own walk, not a C++ personality) | none | none (asmjit#313 was declined) |
| Stack maps | none | none | none |
| Stack switching / stacklets | no API | no API; the local area is fixed at `sljit_emit_enter` | no API, but a full assembler with explicit frame control |
| Platforms | GNU/Linux and the other LibJIT targets | x86, ARM, PowerPC and the other upstream targets | x86/x86-64, AArch64 and the other upstream targets |

The row that matters most is **root storage**: every backend can materialise a
small array of live reference slots at a `suspend` call. LibJIT has the
dispatcher's compile-time type stack; SLJIT has a local area whose base
`sljit_get_local_base` can pass; AsmJit has explicit frame slots. That makes the
suspend-point root list (the hard GC requirement in §10.9) a *per-backend
emission cost*, not a blocker on any of them.

### 10.4 Mechanism × backend matrix

Effort scale: **S** — one file plus tests; **M** — one subsystem plus tests;
**L** — a design spanning the core and a provider or one backend; **XL** — a new
engine or execution model. `◑` means *feasible with the stated precondition* —
for SLJIT/AsmJit the backend itself. The SLJIT and AsmJit
backends are scaffolds, so their measurements are deferred, not their verdicts.

| Mechanism | LibJIT | SLJIT | AsmJit | Blocking fact / caveat |
|---|---|---|---|---|
| **A — native stacks + context switch** | **✅ M–L** | **◑ M–L** | **◑ M–L** | No backend blocker. The switch is a helper call; each backend must emit the call, a root array and a per-entry stack check. The core work (context object, allocator, transfer) dominates. |
| **B — host thread per continuation** | **✅ M** | **◑ M** | **◑ M** | Backend-independent; B adds no codegen work, but the backend must exist to run it. The per-thread state already exists; the work is the context object, hand-off, trap/exception transfer and the one-mutator protocol. |
| **C — interpreter fallback** | **—** | **—** | **—** | **Rejected by project decision (§10.15):** no bespoke interpreter fallback anywhere. If interpretation is ever required it must be LibJIT's own compile/interpret configuration, not a WasmJit engine. |
| **D — native stack copy** | **—** | **—** | **—** | No frame maps, raw pointers into frames, host C++ frames, ASAN fake stacks; `ABI.md` burns `VMContext*` addresses into code. |
| **E — segmented stacks** | **—** | **—** | **◑ L–XL** | LibJIT has no frame control and SLJIT's local area is fixed at enter; only AsmJit's PEI could base a stacklet ABI, and every call would need a stack check. |
| **F — CPS / asyncify** | **—** | **—** | **—** | The rewrite does not preserve one-shot, `switch`, `resume_throw` or host-import behaviour (SSD4). |

The matrix says something narrower than “A is hard”: **A and B cost roughly the
same core work**, and the backend cost is small in both. The real difference is
the *provider*: B has no assembly, no stack allocator and no ASAN fiber hooks;
it pays in threads and hand-off latency. A has none of those runtime costs but
needs a context-switch provider, guarded stacks and per-stack limits. C is the
only option that buys precise frame introspection, and it buys it by adding a
second engine.

A is also the only option whose *provider* can be swapped without touching the
instruction lowering: `ucontext`, Boost.Context, a hand-written SysV
`fcontext`, and (on another platform) Windows fibers all satisfy the same
“save/restore the callee-saved context” contract. That is why the recommendation
in §10.12 puts the mechanism-neutral context first and the provider second.

### 10.5 Why the switch itself is backend-neutral

Mechanism A is often described as “JIT stack switching”, which makes it sound
backend-specific. It is not, provided five ordinary ABI facts hold:

1. **`suspend`/`resume` are normal calls** from generated code to a runtime
   helper. All three backends can emit that call.
2. **The callee restores the stack pointer.** A context switch saves the current
   SP and restores it on resume; that is indistinguishable from a long-running
   helper call. None of the three backends assumes the SP is unchanged across a
   call (they all lower ordinary C calls).
3. **Live values in caller-saved registers are spilled before a call.** This is
   standard call lowering in all three; a switch may clobber caller-saved state,
   and the backend has already spilled what it needs.
4. **Locals are addressed relative to a restored SP or a preserved frame
   pointer.** LibJIT uses its own frame slots, SLJIT addresses the fixed local
   area through `SLJIT_SP`, and AsmJit's PEI can preserve a frame pointer.
5. **No live value sits in the red zone across the call.** A function that makes
   a call cannot keep live data in the red zone; AsmJit models the red zone
   explicitly (`FuncFrame`), and the other two do not rely on it.

Under those conditions a `swapcontext`/fcontext-style switch is transparent to
the code generator. That is why the matrix's A row has **no backend blocker**;
the backend-specific work is limited to *emitting* the suspend call with its
root array, the stack check and (where it exists) the tail-call driver.

The corollary is that the hard parts are in the core, not the backends:

- per-execution-context state and transfer (§10.7–§10.8);
- the suspend-point root list and continuation disposal (§10.9);
- a per-stack limit instead of the current single `VMContext::stackLimit`
  (§10.10);
- ASAN fiber annotations on the A provider and the absence of them on B
  (§10.11).

### 10.6 What each backend must emit

The core owns the mechanism-neutral contract
`WasmContinuation.hpp` and each backend implements only the emission side. The
contract is the same for A and B; it is the provider behind it that differs.

| Contract item | LibJIT | SLJIT | AsmJit |
|---|---|---|---|
| **Entry trampoline with a runtime signature**: invoke a `Callable` with a value array and an optional result buffer | already exists as the tail-call driver (`LibjitModuleCompiler.cpp`), using 16 `TailCallState` slots and the multi-value result buffer | must be emitted (enter/return + argument marshalling) | must be emitted (PEI or hand-written prologue) |
| **`suspend` call**: `(ctx, tagId, args, results, roots, rootCount) -> status` | emit the call; the dispatcher knows the live locals and operand stack | emit the call; spill roots into the local area | emit the call; use frame slots |
| **`resume` / `switch` / `bind`**: core entry points; the backend only calls them and branches on the returned status | M | M | S–M |
| **Stack check on entry** | already emitted (`wasm_stack_check`) | emit it | emit it |
| **Per-executing-stack limit** | core/helper change; the generated call can stay `wasm_stack_check(ctx)` | same | same |
| **Tail-call driver per execution context** | exists; must save/restore or own the state per context | emit a driver (`SLJIT_CALL_RETURN` is a building block) | emit a driver |
| **Trap/exception transfer across a resume** | core helpers; no backend change | same | same |
| **Suspend-point root spill** | force live refs into `jit_value` slots and pass the array | `sljit_get_local_base` + local slots | frame slots (explicit) |
| **General stack maps** | not needed for liveness; only the suspend-point roots | same | same |

The backend cost is therefore **M** for LibJIT (mostly the root spill and the
driver state) and **S–M** for SLJIT/AsmJit *once those backends exist*. The
larger, backend-independent cost is the ExecutionContext migration in §10.7 and
the continuation/GC contract in §10.9.

### 10.7 Per-host-thread state that must become per-execution-context

The engine's per-thread state is not a speculative concern: it is the first
obstacle any continuation mechanism meets. The table records what exists today,
where it lives, and what scope a continuation runtime needs.

| State | Today | Correct scope | Why the scope changes |
|---|---|---|---|
| `TrapSlot` (pending/kind/message) | `thread_local` in `WasmTrap.cpp:45` | **per execution context** | A trap in a resumed continuation must be reported to the resumer, not to the host thread that happens to run both. `reportTrap` copies the message, so the slot can be transferred. |
| `wasmCallDepth` | `thread_local` (`WasmTrap.cpp:49`) | per context | It gates whether `checkTrap()` may throw `Wasm::Trap` at the boundary; a suspended context has its own boundary depth. |
| `nativeImportDepth` | `thread_local` (`WasmTrap.cpp:54`) | per context | The import trampoline's catch frame belongs to the context that entered the import. |
| Defer stack (records + top index) | `thread_local` (`WasmTrap.cpp:59-60`) | per context, **and stack-scoped** | Records point into frames on the suspended stack. A peer context on the same thread must not see or drain them; the records must travel with the continuation. |
| Pending exception pointer | `thread_local` (`WasmExn.cpp:29`) | per context | `resume_throw` injects an exception into another context; an uncaught exception in a resumed continuation must surface at the resume site. |
| `TailCallState` (pending/target/args) | `thread_local` (`WasmModuleInstance.cpp:18`, declared in `WasmValue.hpp:84`) | per context | The trampoline state is transient between a tail-call return and the driver's re-dispatch, but a switch must save/restore it so two contexts cannot observe each other's target. |
| `VMContext::stackLimit` | one integer per instance, captured at construction from the creating thread (`WasmModuleInstance.cpp:317`, `WasmStack.hpp`) | **per executing stack** | Not TLS, but it encodes the same one-call-chain-per-thread assumption. Two disjoint stack mappings cannot share one numeric limit (§10.10). |
| `currentThreadStackBounds` | `pthread_self()` (`WasmStack.cpp:19`) | per stack provider | A continuation stack needs its own bounds and guard page, not the host thread's. |
| `Store` (type registry, GC heap, string store) | process-wide singleton, mutex-guarded for host-side bookkeeping | **shared** (not per agent) | Agents/continuations share the store by design; the existing locks are the starting point, not a multi-mutator collector. |
| `internals.gcSet`, `gcAllocatedObjects` | per instance (`WasmModuleInstance.cpp:323`, `:1364`, `:1404`) | shared by agents of the instance | `gcAllocatedObjects` is a plain counter and `gcSet` is mutated by the active mutator. B's one-mutator hand-off makes this safe; concurrent agents need a lock or an atomic protocol. |
| Memories, tables, globals | per instance | shared | The threads proposal's shared/unshared split applies; a continuation on the same instance shares them by construction. |
| `HostBridge` and native imports | per instance, borrowed | shared | Imports may be re-entered from a continuation; the embedder contract must say so. |
| `checkTrap` / `CallDepthScope` / `NativeImportScope` | resolve the current thread at construction/destruction | resolve the current **context** | The RAII objects live on the context's own stack, so resolving "current" at destruction is correct once the TLS pointer identifies the context. No signature changes. |
| `wasm_trap_slot()` / `wasm_exn_slot()` / `currentTailCallState()` | indirection helpers over TLS | resolve the current context | This is the migration seam: generated code never embeds the TLS address, so the ABI does not change. |

**New state that does not exist today** and has no TLS precedent:

- the **handler chain** (`(on $tag $label)` / `(on $tag switch)`) installed by
  `resume`/`resume_throw` and searched by `suspend`/`switch`;
- the **one-shot flag** and the consumed/null checks;
- **bound arguments** for `cont.bind`, in the same value-array layout as the
  entry trampoline;
- the **captured-stack descriptor** (provider-specific: mmap base/size/guard, or
  thread id/handoff state);
- the **suspend-point root list** (§10.9);
- the **resume status** (normal completion, suspension, trap, injected
  exception);
- the **parent/peer link** used by splicing and `switch`.

### 10.8 A continuation is an execution context, not necessarily an agent

This distinction decides how much of TRAP-6 is forced.

- The threads proposal's **agent** is a WebAssembly execution context, informally
  a guest thread ([`THREADS.md`](THREADS.md) §2.1). The engine deliberately has
  one agent and one host thread per call chain today.
- A **continuation** is also an execution context — a second call chain with its
  own stack, handler chain and pending state — but it is not a thread and does
  not introduce guest concurrency. Only one of the two runs at a time.
- Therefore **per-host-thread state is the wrong granularity as soon as a
  continuation is suspended**, even with one agent and one host thread. A
  fiber-style provider (A) multiplexes contexts on one thread, so the TLS state
  of the suspended context would be visible to the resumer: the trap slot, the
  exception slot, the tail-call target and the defer stack are all wrong.
- **B hides this accident**: if each suspended continuation owns a host thread,
  TLS *is* per context, and the existing trap/exception/tail-call/defer code is
  correct without a migration — as long as only one thread runs Wasm at a time
  and the hand-off provides the happens-before edge.
- **True agents remain a separate problem**: two agents sharing one host thread
  still need per-agent state even under B. That is TRAP-6
  ([`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §12.1,
  [`THREADS.md`](THREADS.md) TH-5), and the continuation work generalises its
  object from “agent” to “execution context”.

The migration sketch is the same one `TRAP_CLEANUP.md` §12.1 records, with the
scope widened:

1. an `ExecutionContext` owns `TrapSlot` (slot + defer stack + the two counters),
   the pending exception, the tail-call state, the stack limit and the handler
   chain;
2. a TLS (or `VMContext`) pointer names the *current* context;
3. `wasm_trap_slot()`, `wasm_exn_slot()`, `currentTailCallState()`,
   `CallDepthScope`, `NativeImportScope` and `defer` resolve the current context
   instead of the thread;
4. the provider switches that pointer together with the stack;
5. the generated ABI and every call site stay as they are.

This refactor is **common to A and C** and **optional but still useful for B**
(it is how B can later support true agents without a second migration). It is the
recommended first step in §10.12.

### 10.9 GC, roots and continuation disposal

The RC + cycle collector is the part that most changes the runtime design, and
the good news is that it is **less** demanding than a tracing collector would be.

**Liveness needs no stack maps.** In the JIT, reference locals own a count (the
epilogue drops them) and `pushValue` clones every reference pushed onto the
operand stack. A suspended frame therefore *holds* the counts of every live
reference it contains, even though the collector cannot enumerate the frame.
Refcount reclamation cannot free an object whose count is held by the suspended
stack, and the cycle collector's trial deletion treats such a count as an
external reference, so it will not free live data either. The only consequence
is that a cycle reachable solely from a suspended stack is **retained** until
the continuation is resumed or dropped — a leak, not a use-after-free. A moving
or root-scanning collector would need full stack maps; the current collector is
neither.

**Disposal is the hard part.** Dropping a suspended continuation must release the
counts its frames hold, and the runtime cannot walk those frames. Three answers
exist:

1. **Suspend-point root list (recommended).** The backend already knows the live
   reference locals and operand slots at a `suspend` (the dispatcher's
   compile-time value stack carries the types). At the compiled `suspend` it
   spills those slots — or their addresses — into a small array passed to the
   runtime; the continuation takes ownership of those counts. On resume the
   roots are restored; on drop the continuation releases them and frees the
   stack. This is a **suspend-point stack map**, not a general one: it exists
   only at `suspend`/`switch`, which is exactly where the captured stack can be
   abandoned.
2. **Leak until teardown.** Keep the counts and free only the stack, or keep both
   until the Store dies. This is safe and cheap but unbounded in a long-running
   server; it is a documented stop-gap for a prototype, not a final design.
3. **Interpreter fallback (C) — rejected (§10.15); listed only to close the
   argument.** Its frames are heap objects and its value stack is explicit, so
   disposal would be a normal traversal. It is not an available option.

The root list is also what makes the A provider competitive with C on GC
correctness without paying for an interpreter. Its cost is per-backend emission
(§10.6), not a new collector.

**The continuation object itself** is a `cont` reference in Wasm terms, so it
must be Store-owned and refcounted like the other store entities. Its one-shot
flag is part of the object; the drop path checks whether the captured stack is
still live and performs the root release + stack free. A null continuation and a
consumed continuation trap before any stack is touched, per the proposal.

**Strings and host references** follow the same rule: string/view counts
(STRINGREF STR-8) and host-reference counts are held by the slots that contain
them, so a suspended frame keeps them alive without a scan. `resume_throw` moves
an exception object between contexts; exception objects are already Store-owned
(`wasm_exn_create`), so the move is a pointer transfer plus the per-context
pending slot.

**Instance accounting.** `gcSet` and `gcAllocatedObjects` are per instance
(§10.7). Under B, only one mutator runs at a time, so the hand-off is the
synchronization edge; if two agents ever run concurrently on one instance, the
set and the counter need a lock or an atomic protocol. That is a THREADS
G-C/G-D item, not a continuation-specific one.

**Scope note.** All of this assumes the current non-moving RC heap. A future
moving collector would invalidate the “counts keep the frame alive” argument and
require full stack maps at every safepoint; that is out of scope here and should
be stated in any GC redesign.

### 10.10 The stack guard and stack memory

The stack guard is the sharpest ABI constraint in the whole design, because it
currently looks like a per-instance field but encodes a per-host-thread fact.

- `VMContext::stackLimit` is **one integer**, installed at instance construction
  from the creating thread's bounds (`WasmModuleInstance.cpp:317`,
  `WasmStack.cpp:48`). `wasm_stack_check(ctx)` compares a probe address against
  it.
- A continuation stack is a **different mapping**. Its addresses may be above or
  below the host stack; comparing a probe on one against the limit of the other
  is meaningless. A single shared limit either never fires on one stack or fires
  spuriously on the other.
- The fix is a **per-executing-stack limit**, not a per-instance one.
  `wasm_stack_check(VMContext*)` can read `currentExecutionContext()->stackLimit`
  (or the context pointer can carry it) while keeping the generated call and the
  exported ABI unchanged. `installStackLimit` then runs **at entry to each
  stack** — at thread start under B, at stack creation/switch under A — not only
  at instance construction.
- Fiber stacks should be **mmap'd with a `PROT_NONE` guard page** and charged to
  the Store allocation budget ([`../SAFETY.md`](../SAFETY.md) §2.3/§2.8). The
  explicit check stays the trap mechanism: the kernel's `SIGSEGV` is not a Wasm
  trap and would take the process down. `kDefaultStackReserveBytes` (512 KiB) and
  `kMaxFunctionLocalBytes` already bound how large a single frame can be; a
  continuation stack must be at least the reserve plus room for helpers.
- The thread provider (B) uses `pthread_attr_setstacksize`/`guardsize` (or an
  mmap'd stack with the same guard discipline) and installs the limit in the
  thread entry. Thread creation failure and stack-budget refusal must be
  reported as the engine's `OutOfMemory` trap, not as an abort.
- **ASAN/TSAN.** Thread stacks are native and need no special handling. A fiber
  switch must call `__sanitizer_start_switch_fiber` / `finish_switch_fiber` (or
  the current sanitizer equivalent) around the switch, otherwise ASAN's fake
  stack and the real stack disagree. LibJIT's `wasm_stack_check` already has an
  ASAN-specific probe because of the fake stack; the fiber provider needs the
  matching annotation, and the study should treat a sanitizer build as an
  acceptance gate. **Implemented:** the weak hooks are called around A's
  `swapcontext`; B needs none.
- **Budget.** Suspended continuations are a memory class the engine did not have
  before: a stack per live continuation, plus the continuation object and its
  root list. The budget must either charge them explicitly or cap the number of
  suspended continuations; “one 8 MiB pthread stack per generator” is not an
  acceptable default. The thread provider makes this visible immediately, which
  is another reason to prototype B first: it forces the budget decision before
  the fiber provider hides it behind an mmap size.
- `currentThreadStackBounds` stays as the OS-thread provider for the root stack;
  a stack provider needs a second entry point that reports the bounds of a
  **given** stack (`stackBoundsOf(base, size)`), not `pthread_self()`.

The stack guard is also where A and B differ most in operational risk: A must
get the guard page, the per-stack limit and the fiber annotations right; B gets
all three from the OS but pays in thread memory and hand-off cost. Neither is
blocked by a backend.

### 10.11 Spikes and acceptance criteria

The study's verdicts are paper verdicts until a spike runs. The order below keeps
the backend-independent work first and makes the SLJIT/AsmJit rows measurable
only after their minimal backend exists.

**Spike 0 — ExecutionContext without switching (all backends, LibJIT today).**
Create two context objects on one host thread, save/restore the current-context
pointer around a call, and verify that a trap, an uncaught exception and a
tail-call target in one context are invisible to the other. This validates the
TRAP-6 migration and needs no stack switching at all. It is the right first
deliverable because it is useful even if the runtime is never finished.

**Spike 1 — A on LibJIT.** A compiled function with a live GC reference (an array
or struct), a local and a pending trap path calls a helper that performs a
`swapcontext`-style switch; the resumer resumes it. Assert that the local and the
reference survive repeated switches, the reference count did not change, a trap
in the resumed function reaches the resumer with its kind and message, a
`PROT_NONE`-guarded stack reports `CallStackExhausted` instead of a SIGSEGV, and
an ASAN build is clean. This is the smallest experiment that exercises every
backend-neutral hazard at once.

**Spike 2 — B on LibJIT.** Two threads hand off a continuation and a resumer,
with a condition variable as the synchronization edge. Assert one-shot
enforcement (a second resume traps), trap/exception transfer, no data race on
`gcSet`/`gcAllocatedObjects` under TSAN, and a bounded thread/stack budget. B
needs no stack-switching code, so it can be built before Spike 1 and gives the
semantic tests an early oracle.

**Spike 3 — SLJIT/AsmJit.** First implement the minimal backend slice: one
Wasm function with a parameter, a local, a native call and a stack-check call;
then repeat Spike 1. The slice is **L–XL** on either scaffold, which is why the
study does not pretend the SLJIT/AsmJit rows are measured. For SLJIT the slice
must use `sljit_emit_enter` with a local area, a native call and
`sljit_get_local_base` for the root array; for AsmJit, `FuncDetail`/`FuncFrame`
with a PEI prologue and explicit frame slots.

**Spike 4 — the rest of the instruction semantics.** `switch` (symmetric
hand-off), `cont.bind`, `resume_throw`/`resume_throw_ref`, multiple handlers and
multi-value payloads. These can be tested on the chosen provider only after
Spike 1 or 2; they are not prerequisites for choosing the provider.

**Acceptance criteria** for calling a provider feasible:

1. a suspended function's locals and GC reference counts survive at least 10,000
   switches, with no leak beyond the continuation's own roots;
2. a trap in a resumed continuation reaches the resumer with the right
   `TrapKind` and message; an uncaught exception does the same;
3. a second resume of a consumed continuation traps;
4. a guard-page overrun yields `CallStackExhausted`, not a host SIGSEGV;
5. an ASAN/TSAN build is clean (fiber hooks for A; thread hand-off for B);
6. the stack/continuation budget is enforced and an over-budget `cont.new`
   becomes the engine's `OutOfMemory` trap;
7. the one-shot/splice/`switch` tests from the proposal pass, not just a
   single-threaded generator.

Until the full criteria are met, `cont.bind`, `resume_throw`,
`resume_throw_ref` and `switch` stay rejected with the existing named error.
The stage-2 slice enables only `cont.new`, `suspend` and `resume`; it must not be
described as full stack-switching support.

### 10.12 Preliminary recommendation and staging

The study's recommendation was mechanism-neutral; §10.15 now records the
project's decision (A primary, B opt-in fallback, C rejected). The staging
remains:

| Stage | Work | Why this order | Effort |
|---|---|---|---|
| **1. ExecutionContext** ✅ (SS-6) | Move the trap slot, pending exception, tail-call state, defer stack, depth counters and stack limit into a context object owned by an `Agent`; TLS holds the current pointer; helpers resolve it. Add Spike 0. | Done. Backend-independent; required by A and by true agents; it is the seam for later agents under B. | Done |
| **2. B prototype on LibJIT** ✅ | Thread per suspended continuation; `cont.new`/`resume`/`suspend` with zero/one `(on $tag $label)`; one-shot; trap/exception transfer; live/stack budget. Narrowed to i32/void payloads and non-loop handler labels. | No assembly, no new backend, no fiber ASAN hooks; validates the semantics and the tests early. B is the opt-in fallback, not the default. | Done (stage-2 slice) |
| **3. A provider** ✅ (basic) | mmap'd stacks + guard, per-stack limit, `ucontext` context switch, serial cross-thread migration, ASAN fiber annotations; same core contract as stage 2. Refcounted references keep a suspended stack alive; a tracing collector would still need the root list. | The default production provider; it is swappable without touching the instruction lowering. | Done (basic) |
| **4. Full instruction set** | `switch`, multi-value/reference payloads, full `resume_throw*` handler clauses, continuation drop/root release. | Needs the remaining value ABI and the symmetric-switch prompt model | L (next) |

The implementation order (B prototype first) is **not** the product order: A is
the default provider, B is the opt-in fallback, and C is excluded (§10.15).

This ordering also answers the SLJIT/AsmJit question without blocking on it:
stages 1–2 are LibJIT-only and measurable; stage 3's provider is
backend-independent; the SLJIT/AsmJit emission work (§10.6) follows their
minimal backend implementations, not the reverse. Their rows stay `◑` until
Spike 3 runs.

Two boundary rules:

- **C is rejected permanently.** No bespoke interpreter fallback will be added
  anywhere in WasmJit; if interpretation is ever required it must be LibJIT's
  own compile/interpret configuration, not a new engine. See §10.15.
- **Do not start with E.** AsmJit's frame control makes stacklets conceivable,
  but a stacklet-aware ABI touches every call and every backend; it is a
  research item after A works, not an alternative to it.

### 10.13 Open questions

These are the decisions the study deliberately leaves open; each one changes the
effort rather than the semantics.

1. **Platforms.** Is the runtime Linux-x86-64 only (ucontext/asm provider), or
   must it build on Windows/macOS and AArch64 from day one? Boost.Context buys
   portability at a dependency cost; a hand-written provider per architecture is
   more work but has no dependency.
2. **When is B enabled?** B is the opt-in fallback (§10.15), so the question is
   which platforms/builds select it and what thread/stack budget it advertises.
   It is legitimate under the proposal (no scheduling or thread semantics are
   required), but it changes the operational profile: a thread and a stack per
   suspended continuation, a hand-off cost per switch, and a thread budget the
   embedder must understand.
3. **Sanitizer gates.** Is an ASAN/TSAN-clean build a release gate? If so, the A
   provider needs the fiber annotations and the B prototype needs the
   one-mutator protocol audited before it can be called done.
4. **Stack size and budget.** What is the default stack per continuation, and is
   it charged to the existing 2 GiB budget (`SAFETY.md` §2.3)? The answer
   determines whether “a million suspended generators” is a trap or a thread
   exhaustion.
5. **Are true agents in scope?** If yes, the ExecutionContext work is
   mandatory even under B, and the instance-level concurrency protocol
   (`THREADSAFETY.md` §10) must be designed with it. If no, B can ship with one
   context per thread; TRAP-6 state is already done and multi-agent execution
   stays deferred.
6. **LibJIT's own compile/interpret mode.** WasmJit forbids a bespoke
   interpreter fallback (§10.15). If a future requirement needs interpretation,
   is LibJIT's own configuration the only acceptable route, and does its ABI fit?
7. **First semantic milestone.** Is `cont.bind`/`resume_throw`/`switch` part of
   the first “supported” claim, or is the first claim only
   `cont.new`/`resume`/`suspend` with one handler? The note's SS-4 currently
   names the full set; the study suggests a narrower first claim.
8. **Provider selection.** `ucontext` is deprecated on some platforms;
   Boost.Context adds a dependency; a hand-written SysV switch is small but
   per-architecture. The core contract should make this a build-time choice.
9. **Host boundary.** Can a native import resume a continuation from C++
   (`callCallable` on the continuation's target), or is `resume` only a Wasm
   instruction in the first version? The former needs a context guard around
   the host call.
10. **OOM surface.** Does `cont.new` fail as `OutOfMemory` (the engine's usual
    trap) or as a refusal to create the stack? The proposal does not prescribe
    the host failure mode; the engine's `SAFETY.md` policy does.

### 10.14 Sources for this appendix

- Upstream SLJIT public header:
  https://github.com/zherczeg/sljit/blob/master/sljit_src/sljitLir.h
  (`sljit_emit_enter` and the fixed local area, `SLJIT_SP`,
  `sljit_get_local_base`, `SLJIT_CALL`/`SLJIT_CALL_REG_ARG`, `SLJIT_CALL_RETURN`).
- Upstream AsmJit function/frame API:
  https://github.com/asmjit/asmjit/blob/master/asmjit/core/func.h
  (`FuncFrame`, `FuncDetail`, stack size/alignment, red zone, dirty/preserved
  registers, `has_preserved_fp`, prolog/epilog insertion). Unwind-info request
  asmjit#313 was declined.
- GNU LibJIT headers and library on this machine:
  `/usr/local/include/jit`, `/usr/local/lib64/libjit.so.0.0.1`; the tree's
  backend in `LibJit/`.
- In-repo: [`../ABI.md`](../ABI.md) (VMContext, result buffers, tail-call
  driver), [`../LAYERING.md`](../LAYERING.md) (what the core owns vs what a
  backend emits), [`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §8.5/§10/§12.1,
  [`../THREADSAFETY.md`](../THREADSAFETY.md) §10/§10.1, [`THREADS.md`](THREADS.md)
  TH-5/T9, [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md) §3,
  [`../SAFETY.md`](../SAFETY.md) §2.3/§2.8; sources `WasmBase/WasmStack.hpp`,
  `WasmBase/WasmStack.cpp`, `WasmBase/WasmTrap.hpp`, `WasmBase/WasmTrap.cpp`,
  `WasmBase/WasmExn.cpp`, `WasmBase/WasmValue.hpp`,
  `WasmBase/WasmModuleInstance.cpp`, `WasmBase/WasmVMContext.hpp`,
  `WasmBase/WasmContinuation.hpp`, `WasmBase/WasmContinuation.cpp`,
  `LibJit/LibjitModuleCompiler.cpp`, `LibJit/LibjitOpcodeDispatcher.cpp`;
  scaffolds `Asmjit/Asmjit.pro`, `Sljit/Sljit.pro`.

### 10.15 Decision record

Recorded by the project owner. These are binding design decisions, not options
for a later study.

| # | Decision | Consequence |
|---|---|---|
| **D-1** | **A — separate native stacks + context switch — is the primary and default continuation runtime.** | A is implemented in `WasmContinuation.cpp` as an mmap-backed stack with a guard page and a `ucontext` switch. It is the default provider; calls are serial, but a suspended stack may migrate to another host thread. ASAN fiber annotations are installed. The remaining A work is a tracing-collector root list; the refcounted engine keeps live stack references alive without one. |
| **D-2** | **B — one host thread per suspended continuation — is the opt-in fallback provider.** | B is selected explicitly (`setContinuationProvider(HostThreads)`), never as a silent automatic fallback. It is implemented in `WasmBase/WasmContinuation.hpp`/`.cpp`; it is the correctness prototype and the choice where A's provider is unavailable. Its thread/stack budget is part of its contract. |
| **D-3** | **C — a bespoke interpreter fallback — is rejected permanently.** | No WasmJit interpreter will be written for continuations, or as a fallback for anything else, ever. If interpretation is ever required it must come from LibJIT's own compile/interpret configuration (LibJIT already provides both a compiler and an interpreter), not from a new WasmJit engine. The core continuation contract must not grow an interpreter path. |
| **D-4** | **Execution state belongs to the execution context and is owned by an `Agent`.** | SS-6 is implemented: `ExecutionState` (trap slot, pending exception, tail-call state, defer stack, depth counters, stack limit) lives in `WasmBase/WasmAgent.hpp`; TLS holds the current pointer; `wasm_trap_slot()`, `wasm_exn_slot()`, `currentTailCallState()` and `wasm_stack_check()` resolve it. |
| **D-5** | **Agents are present in the code; true multi-agent execution remains deferred.** | `Agent`, `AgentScope` and per-thread default agents exist and own the state. A and B continuations are execution contexts, **not** agents: neither has a shared-instance entry point, scheduler, waiter table or multi-mutator GC. The host handle is serial-only. There is no multi-agent execution model; THREADS TH-5/T9 remain deferred indefinitely. |
| **D-6** | **The provider contract is backend-neutral.** | The contract lives in `WasmBase/WasmContinuation.hpp`; A (default) and B (fallback) are implemented once there. LibJIT, SLJIT and AsmJit only emit the suspend call, the root array and the stack check. SLJIT/AsmJit remain prerequisites for their own measurements, not for the decision. |
