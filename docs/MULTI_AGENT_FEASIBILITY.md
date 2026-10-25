# Multi-Agent Execution — Feasibility of Demoting the Class-E Blockers

Status: feasibility study (revision 1 — 2026-10-25; measurements on
`LibjitDispatcher` at `ea4fb30`). **No code, no decision to build.** The current
milestone ([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md)) stays single-agent;
this note asks what it would cost to change that later.
Audience: anyone who wants to revisit `proposals/THREADS.md` `TD9`/`TH-5`,
anyone bringing up Asmjit or SLJIT, and anyone touching `Agent`,
`ExecutionState`, `ModuleInstance`, `Store`, `GcHeap`, the continuation
providers, or the embedder surface.

`API_ABI_STABILITY.md` §4 classifies every open gap. Multi-agent execution is the
one family it marks **class E** — "the only known solution restructures the
backend set or the `WasmBase` ↔ backend contract" — and it pins the current
milestone to the single-agent model precisely so that this classification does
not silently poison the milestone (§5.3 there). This note takes the next step:
**can the E be split into cheaper pieces?** It answers three questions:

1. Can multi-agent execution be made **backend-agnostic**?
2. What does **multithreading** cost on each of the three intended backends
   (LibJIT today, AsmJit and SLJIT when they exist)?
3. Can multi-agent execution be **opt-in for the embedder**, the way the
   JS-integration hooks are?

Short answer: **yes / very little / yes** — because the agent's state already
lives outside the backends, and generated code reaches it through helpers rather
than embedded addresses. The expensive part is *not* the code generators; it is
the GC safepoint, the per-instance bookkeeping and the scheduler, all of which
are core-side and can be staged.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What would "demoting" a class-E blocker mean, and what counts as success? |
| §3 | What is per-agent today, and what is still shared? |
| §4 | Study 1: is the multi-agent seam already backend-agnostic? |
| §5 | Study 2: what does multithreading cost on LibJIT, SLJIT and AsmJit? |
| §6 | Study 3: can multi-agent execution be opt-in for an embedder? |
| §7 | The staging plan, with the class of every stage |
| §8 | Risks and open questions |
| §9 | What this does *not* change (the milestone, `TD9`, the code) |
| §10 | Test survey: what exists to build on |
| §11 | References |

## 2. What "demoting" means

### 2.1 The vocabulary, borrowed not redefined

The classes are defined in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4:

| Class | Meaning (abridged) |
|---|---|
| **A** | Additive / backend-neutral: a field, a rule, a helper; a backend that does nothing new keeps working. |
| **B** | Per-backend, bounded by an existing contract row of [`LAYERING.md`](LAYERING.md) §4. |
| **C** | Blocked upstream: no stable encoding to implement. |
| **D** | Deferred by a recorded decision. |
| **E** | The only known solution restructures the backends or the core ↔ backend contract. |

"Multi-agent execution" is one *feature*, not one gap. Its honest solution is a
set of changes, and the milestone clause cares about the class of **each change**.
Demotion therefore means:

1. **Split** the family along the real boundaries (backend / core-subsystem /
   embedder surface).
2. **Name the gateway** for each piece: the smallest change that makes the next
   piece additive.
3. **Show that the backend share is B or less.** If any backend has to be
   re-architected, the family stays E and the answer to question 1 is "no".

### 2.2 The bar

The study succeeds only if all three of these hold:

| Bar | Test |
|---|---|
| The seam stays | A backend that satisfies today's [`LAYERING.md`](LAYERING.md) §4 rows needs **no new obligation** to run under several agents. |
| The stages are independent | Each stage compiles, tests green and ships alone; no stage requires the next one to be honest. |
| The default stays free | An embedder that does not ask for agents pays no lock, allocation or branch it does not already pay (the `HostBridge` / `ProfileConfig` precedent). |

## 3. Today's model: one agent per host thread, state already outside the instance

### 3.1 What is already per-agent (`WasmAgent.hpp`)

`ExecutionState` is the part a continuation also owns, and it holds **everything
that used to be process-global**:

| State | Field | Why it must be per-agent |
|---|---|---|
| Pending trap (`TrapSlot`: flag, kind, message) | `trapSlot` | A trap in one agent must not be visible to another. |
| Pending Wasm exception | `pendingException` | Same. |
| Tail-call trampoline target/args | `tailCallState` (`Callable*` + 16 slots) | A trampoline loop is per execution. |
| Deferred-cleanup stack | `deferRecords` / `deferStack` | Trap-time host cleanup is per execution. |
| Host↔Wasm and native-import depth | `wasmCallDepth`, `nativeImportDepth` | Decides where `WASM::Trap` may be thrown. |
| Stack limit | `stackLimit` | Installing one per stack (continuation stacks do) is already the rule. |
| Diagnostics ring/sink | `diagnostics` | Two agents on one host thread must stay independent. |
| Owning agent | `owner` | Null for a bare execution state. |

`Agent` owns exactly one `ExecutionState` plus an id; `AgentScope` installs it;
`currentExecutionState()` resolves the calling host thread's state lazily. The
`TRAP-6`/`SS-6` migration is what made this true (`SAFETY.md` §2.7 states the same
rule as policy), and it is the single most important reason this study can be
optimistic.

Generated code never embeds an agent address: it calls `wasm_trap_slot()`,
`wasm_exn_slot()`, `currentTailCallState()` and `wasm_stack_check(ctx)`.

### 3.2 What is still shared (and therefore where a second agent would race)

| Shared thing | Where | Mutable at run time? |
|---|---|---|
| GC reclamation queue | `ModuleInstanceInternals::gcPending` (`std::vector<PendingGcObject>`) | **Yes** — `reclaimGcObject()` pushes from generated code's `refDrop` path. |
| Cycle-collector candidates | `gcCycleRoots` | **Yes** — `possibleCycleRoot()` on a surviving release. |
| Reclamation counters | `gcAllocatedObjects`, `gcReclaimedObjects` | Yes (diagnostics only). |
| Segment drop flags | `dataSegmentDropped`, `elementSegmentDropped` | **Yes** — `data.drop` / `elem.drop` / `memory.init` / `table.init`. |
| Guest-visible globals | `globalsStorage` / `globalRefs` | **Yes** — by design; the guest must synchronize (atomics). |
| Memories / tables | Store-owned, reached through `VMContext` | Grow via `Store`, which already orders shared growth (`TH-3`). |
| Compiled handles, translated types, string literals, trap-message pool | `internals` | No — frozen after compilation. |
| Store containers | `Store::storeMutex_` | Host-side only (create/lookup). |
| GC region allocator | `GcHeap::mutex_` | Host-side only. |
| Continuation stacks | provider A/B | Per execution context, serial hand-off today. |

Two observations matter more than the raw list:

- **The refcount is already atomic.** `refDrop` uses
  `std::atomic_ref<uint32_t>` with a relaxed CAS loop; a count reaching zero
  stamps the header and queues the object. So the *count* is agent-safe today.
- **The queue is not.** `gcPending` is a plain `std::vector` push, and
  `gcCollect()` is an **embedder-driven safepoint** ([`ABI.md`](ABI.md) "Other
  store-owned entities"). In a single-agent engine that is a documented contract;
  under two agents it is the first real race.

### 3.3 The blocker list we already have

`proposals/THREADS.md` §4.3.1 lists nine blockers (`MA-1…MA-9`). This study does
not re-derive them; §4.4 re-classifies each one.

## 4. Study 1 — is the multi-agent seam already backend-agnostic?

### 4.1 What a backend actually owns

A backend is a **code generator**, not a runtime. Its obligations are the eight
rows of [`LAYERING.md`](LAYERING.md) §4 and the four `ModuleInstantiator` hooks.
Concretely, `LibJIT::ModuleCompiler` produces:

| It produces | Where it goes | Agent-relevant? |
|---|---|---|
| A lowered type per `TypeId` | `internals.translatedTypes` (frozen after compile) | No |
| A function handle per defined function | `internals.compiledFunctions` (frozen after compile) | No |
| Native code and a `Callable::fnPtr` | `internals.internalCallables` | No — the code is re-entrant by construction (see 4.2) |
| A start-function call | `callStartFunction()` | No — it runs under whoever instantiated, inside an `AgentScope` |
| A continuation invoker thunk | `ContinuationInvokeFn` (per backend) | No — the core calls it; the thunk calls a `Callable` |

Nothing in that list is per-agent state. The backend **never decides who runs**
a compiled function; the frontend picks the `VMContext*` argument (arg0) and the
agent current on that host thread. This is the structural reason the family can be
demoted.

### 4.2 The five ABI facts that make it independent

Mirroring the style of `proposals/STACK_SWITCHING.md` §10.5, multi-agent is
backend-neutral *provided* these hold — and each already holds today:

1. **Agent state is reached by helper, never by address.** Generated code calls
   `wasm_trap_slot()`, `wasm_exn_slot()`, `currentTailCallState()`. A backend that
   emitted a `TLS` address instead would break continuations already
   (`STACK_SWITCHING.md` `SS7`), so no backend does.
2. **The trap is a pending status that unwinds by returning.** Nothing in the
   frame model needs a per-thread unwind table, so "which agent traps" is decided
   by the slot the helper returned, not by a stack unwinder.
3. **The stack limit is a per-state integer**, installed when a stack becomes
   current. A second agent gets its own value without touching codegen.
4. **Compiled functions are plain native functions** with `VMContext*` as arg0.
   They hold no ambient state; re-entering the same code from two agents is legal
   if the *data* it touches is legal.
5. **Compilation is separable from execution.** A module is compiled once; the
   resulting handles are immutable. Agents only read them.

### 4.3 What a backend would have to add: membership, not structure

| Item | Cost | Why it is not a new obligation |
|---|---|---|
| Re-entrancy of the *invoker* (`ContinuationInvokeFn`, the harness `invoke`) | Per-call scratch, not per-session | The signature is already call-scoped; a backend that keeps a mutable "current call" buffer would already break continuation switches. |
| A per-entry stack check (`wasm_stack_check`) | Already emitted for tail calls and continuations | The check reads `ctx->stackLimit` / the current state's limit. |
| Nothing else | — | No thread, lock, scheduler or TLS in any backend today. |

So the answer to question 1 is **yes**: the seam is agent-neutral. The class-E
label came from the *core*, not from the code generators — which §4.4 makes
explicit.

### 4.4 Re-classifying `MA-1…MA-9`

Effort scale (used in `proposals/STACK_SWITCHING.md` §10.4): **S** — one file
plus tests; **M** — one subsystem plus tests; **L** — a design spanning the core
and a provider; **XL** — a new engine or execution model.

| Blocker (`THREADS.md` §4.3.1) | Class today | Gateway change | Class after | Effort |
|---|---|---|---|---|
| `MA-1` no scheduler / lifecycle | E (calls it an execution-model change) | Give the *embedder* the scheduler through a hook contract (§6); the core owns only "one agent enters at a time" plus a park/wake callback | A (config + hooks) + D (the scheduler itself is embedder code) | M |
| `MA-2` one mutable `ModuleInstance` | E | Partition the mutable internals: GC safepoint state per agent or guarded, drop flags atomic, guest globals/tables left to the guest's own atomics (`SET-3` already provides `global.atomic.*`) | B (bounded by the instance-state contract) | M–L |
| `MA-3` no multi-mutator GC | E | Serialize reclamation at a per-instance safepoint lock and make the *queue* agent-safe (`gcPending` per agent, merged at collect); keep the region allocator's existing mutex | B | M |
| `MA-4` no waiter table | E | A guest-address-keyed waiter table behind `wasm_atomic_wait*/notify`; the single-agent return values stay legal | B | M |
| `MA-5` Store locks are host-side | D | Keep them; document that the Store is a *management* lock, not a guest-data lock | D | S |
| `MA-6` continuations are execution contexts | D | Keep; an agent may *use* a continuation, it does not become one | D | S |
| `MA-7` provider / cross-provider gaps | D | Keep; switch is same-provider, serial migration is documented | D | S |
| `MA-8` JS/JSPI is adapter-only | D | Keep; the scheduler hook is the adapter seam | D | S |
| `MA-9` no multi-agent test / memory model | E | Two-agent fixtures + a documented guest memory model; the memory model is a *doc* deliverable | A | M |

Reading: after the gateways, **no blocker is E**. Two are B (GC queue, waiter
table), two are A (scheduler hooks, tests/model), one is B/A (instance
partitioning), four stay D — and the four D rows are exactly the decisions the
project already made.

### 4.5 Verdict for question 1

| Question | Answer |
|---|---|
| Does any backend need a new contract row? | **No.** The four `ModuleInstantiator` hooks and the eight §4 rows are unchanged. |
| Does the seam need to know what an agent is? | **No.** It receives a `VMContext*` and a `Callable`; the agent is the frontend's business. |
| Where is the work? | Core: instance partitioning, GC safepoint, waiter table. Embedder: the scheduler. |
| Class demotion | **E → B + A** (with the four D rows unchanged). |

## 5. Study 2 — multithreading on the three intended backends

### 5.0 Two different questions people call "multithreading"

| Question | What varies | Backend cost |
|---|---|---|
| **T-a: many host threads, each running one agent on its own instance** | Instances, `VMContext`s, memories | None beyond "compile in separate contexts" |
| **T-b: several agents on the *same* instance** | One instance, several `ExecutionState`s, shared globals/tables/memory/GC heap | Also none — the backend was never in this picture |

Both need the same backend properties, so the study asks one set of questions per
engine:

| # | Property | Why it matters |
|---|---|---|
| Q1 | Can two threads **compile** concurrently? | Module loading at runtime (a game streaming content) is the realistic case. |
| Q2 | Can separately compiled code **execute** concurrently? | T-a and T-b both need it. |
| Q3 | Does the engine hold **mutable global state** a second thread would race on? | The only class-B risk a backend can introduce. |
| Q4 | Is the **invoker** re-entrant (per-call scratch)? | Needed for concurrent `callCallable` / continuation resume. |

### 5.1 GNU LibJIT (the only executable backend)

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Yes, with one context per compilation.** Compilation is bracketed by `jit_context_build_start()` / `jit_context_build_end()`, which the code documents as *a lock on that context*; a wedged context blocks every later build. Two threads must therefore use two `LibJIT::Context` objects — which is what the harness already does per `TestBackend::Session`. | `LibJit/LibjitModuleCompiler.cpp:67–82`; `Test/TestBackend.cpp` (`LibJitSession` owns one `LibJIT::Context`) |
| Q2 | **Yes.** Generated functions are ordinary native code taking `VMContext*`; they embed no per-compilation mutable state. The only per-call machinery is the host-side invoker. | `ABI.md` "Overview"; `WasmValue.hpp` |
| Q3 | **The type translator is per-context** (`LibJIT::Context` owns it and its `TypeId`-keyed cache), so there is no shared translator. LibJIT's built-in types are immutable. | `LibJitContext.hpp`; `LibJitTypeTranslation.cpp` |
| Q4 | **Yes for the harness invoker**: its scratch is a local `std::vector` per call, and the `v128` workaround builds arguments on the stack. `jit_apply` is called with a per-call argument array. Verify with a two-thread invoke test at `MF-1`. | `Test/TestBackend.cpp` `LibJitSession::invoke` |

Residual LibJIT-specific tension: the installed build has quirks already
documented (`jit_insn_store_relative` on aggregates; the `jit_apply` path for a
third stack-passed aggregate, which is why the harness has a `v128` path). Those
are *correctness* quirks, not concurrency ones, but a second agent makes any
"per-session hidden state" more visible, so `MF-1` should include a two-thread
compile+invoke test rather than assume.

### 5.2 SLJIT

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Yes.** Upstream states "The compiler is thread-safe", and the model is one `sljit_compiler` per compilation unit; SLJIT can even be compiled with multithreading disabled for single-threaded applications. | upstream `sljit_src/sljitLir.h` (feature list), read 2026-10-25 |
| Q2 | **Yes.** SLJIT emits into a caller-owned buffer; the generated code has no runtime dependency on the compiler object once emitted. The backend owns the W^X handoff (allocate → emit → `mprotect` RX). | same header; `SIMD_BACKENDS.md` §3 |
| Q3 | **None documented.** Registers and stack are handled manually per `sljit_emit_enter`; the local area is fixed at entry, so per-call scratch must be a *frame* slot, not a compiler field. | `STACK_SWITCHING.md` §10.3 |
| Q4 | **Our obligation**, not SLJIT's: the invoker must not cache "the current call" in the session. | design rule |

### 5.3 AsmJit

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Yes, with per-thread objects.** `CodeHolder` + a `BaseEmitter` are per-compilation objects; `Function`/`FuncFrame`/PEI are per-emit. | upstream `asmjit` core docs (read 2026-10-25); `STACK_SWITCHING.md` §10.3 |
| Q2 | **Yes.** Emitted code is plain native code in executable memory; AsmJit keeps no thread-local execution state. | same |
| Q3 | **One open item: executable-memory ownership.** `JitRuntime`/`JitAllocator` own the code pages; whether `add()` may be called concurrently is *not* stated in the API docs we have read. Treat it as **one allocator per session, or serialized**, and verify at bring-up. | to verify (`MF-1` checklist) |
| Q4 | Same design rule as SLJIT. | design rule |

### 5.4 The matrix

`◑` = feasible with the stated precondition (here: the backend exists).

| Capability | LibJIT | SLJIT | AsmJit | Caveat |
|---|---|---|---|---|
| Concurrent **compilation** (separate contexts/compilers) | **✅ S** | **◑ S** | **◑ S** | LibJIT: one context per thread. AsmJit: allocator ownership. |
| Concurrent **execution** of compiled code | **✅ S** | **◑ S** | **◑ S** | Needs the core-side partitioning (§4.4 `MA-2`/`MA-3`), not backend work. |
| **Re-entrant invoker** | **✅ S** (verify) | **◑ S** | **◑ S** | Keep all per-call state on the stack. |
| Shared mutable **global state** in the engine | **none found** | **none documented** | **none documented** | The one AsmJit question is the code allocator. |
| **Helper layer** (SIMD, atomics) used concurrently | **✅ already** | **✅** | **✅** | Pure `extern "C"` functions over caller buffers. |
| **Continuation provider** used from several threads | **✅ core-owned** | **✅** | **✅** | Each suspended stack is an execution context; the backend only supplies the invoker. |

### 5.5 What is not backend work at all

These are the parts that look like "JIT threading" but live in the core, and
they are identical for all three engines:

- **SIMD helpers** — `wasm_v128_*` operate on caller-provided 16-byte buffers;
  no globals.
- **Atomics helpers** — `wasm_atomic_*` are seq-cst `__atomic_*` operations plus
  alignment traps; already multi-thread correct.
- **Memory and tables** — Store-owned; shared growth is ordered (`TH-3`).
- **Trap slot, exception slot, tail-call state** — per `ExecutionState`.
- **Diagnostics** — per `ExecutionState`, with a bounded ring and an optional sink.
- **Continuations** — provider A (`ucontext`, serial hand-off, serial migration)
  and B (host thread per suspended continuation) are core code.

### 5.6 Verdict for question 2

| Question | Answer |
|---|---|
| Does any engine prohibit multithreaded *use* of compiled code? | **No.** All three emit native code with no execution-time engine state. |
| Does any engine prohibit concurrent *compilation*? | **No.** LibJIT needs one context per thread; SLJIT is documented thread-safe per compiler; AsmJit needs one emitter/code-holder per compilation and an allocator-ownership decision. |
| Is the per-backend work "re-architect" or "verify"? | **Verify.** Three one-line rules, one test per backend. |
| Does it stay class B? | **Yes** — and only because the obligations already exist (`LAYERING.md` §4 rows 1, 4, 5). |

## 6. Study 3 — can multi-agent execution be opt-in?

### 6.1 The precedent is already in the tree, five times over

| Feature | How it is opt-in | Default when absent |
|---|---|---|
| Host references / lifetime | `HostBridge`: a plain aggregate of optional function pointers, installed before instantiation | Opaque `void*`, no callback |
| JSPI-style suspension | `HostContinuation` / `HostContinuationScheduler`: hooks supplied by the embedder | No scheduler; continuations still run, serially |
| Determinism / SIMD-free execution | `ProfileConfig`: exclusions re-validated at the boundary | Full profile |
| Instruction tracing | `WasmTracing.hpp`: a compile-time callback | The instruction lowers to nothing |
| Diagnostics | `DiagnosticSink`: a function pointer plus bounded ring | Zero-cost configuration |

Every one of these is *embedder-owned*, *function-pointer-shaped*, *default-off*
and *zero-cost when off*. Multi-agent can follow exactly that pattern, which is
why the JS-integration comparison the question invites is the right one.

### 6.2 The shape

| Piece | Contents | Class |
|---|---|---|
| **A concurrency mode** in the runtime/instantiation config | `enum class ConcurrencyModel { Single, SerialAgents, ConcurrentAgents }`, default `Single`, validated at `instantiate()` the way `ProfileConfig` is | A |
| **A capability query** | `supportsConcurrency(model)`; a backend or the runtime may refuse a model at the boundary, loudly | A |
| **A host scheduler hook** | `HostAgentScheduler`: an aggregate of function pointers (`spawn`, `park`, `wake`, `join`, `step`) plus `userData`, borrowed and never owned | A |
| **An agent handle** | `Agent&` / `AgentId` exposed to the embedder; one per OS thread by default | A |
| **Per-instance entry discipline** | The core guarantees "one agent inside a call at a time" for `Single`/`SerialAgents`, and only `ConcurrentAgents` relaxes it (with the §4.4 `MA-2`/`MA-3` work) | B |

The core still owns **no scheduler**. The embedder supplies it, exactly as it
supplies the JS event loop in `proposals/JS_INTEGRATION.md` — the engine gives
the seam, the game gives the policy.

### 6.3 What "opt-in" must guarantee

1. **`Single` is bit-identical to today.** Same code paths, no extra lock, no
   extra allocation, no behaviour change. The default must not read the config
   beyond a null check.
2. **Unsupported models fail at the boundary**, not mid-call. A module loaded
   with `ConcurrentAgents` on an engine that cannot honour it is rejected at
   `instantiate()` (the `ProfileConfig` precedent), before any code is generated.
3. **The engine never invents a thread.** Spawning a host thread, growing a
   thread pool or installing a signal handler is the embedder's act.
4. **Guest-visible ordering stays the guest's problem.** Shared globals/tables
   are shared state; the guest uses `global.atomic.*` / atomics, as
   `proposals/THREADS.md` §3 permits an engine to require.
5. **The safepoint stays explicit.** `gcCollect()` remains an embedder-driven
   safepoint; with agents the runtime must document *when* it is safe (no agent
   inside a call, or per-agent collection under a lock).

### 6.4 Failure modes the design must name

| Failure | Why it is new | Mitigation |
|---|---|---|
| Blocking `wait` with no other runnable agent | Today `wait` returns "timeout" without sleeping — legal but degenerate | The waiter table (`MA-4`) plus the scheduler hook; keep the degenerate result legal for `Single` |
| A trapped agent | Trap state is already per agent | Nothing to do — this is the `TRAP-6` payoff |
| Cancelling/joining an agent mid-call | No current concept | Scheduler-level; the core exposes "is this agent in a call" and a `park` point at the boundary |
| Deterministic replay | Single-agent determinism is a `PROFILES.md` claim | `ConcurrentAgents` is *outside* the deterministic profile; document it |
| Unloading a module whose code an agent is inside | The `ABI.md` lifetime rule ("instances never move; callers own lifetimes") | Same rule, now per agent: join before teardown |

### 6.5 Verdict for question 3

**Yes.** The opt-in shape is already the house pattern, and the pieces are
additive: a mode enum, a capability query, a function-pointer scheduler hook, and
explicit per-instance entry discipline. The scheduler itself is deliberately
**not** engine code — that keeps the class at A and keeps `WasmBase` free of a
thread pool it would have to defend forever.

## 7. The staging plan

Not a commitment — a **possible order** in which each step is independently
shippable, with the class it would carry. A stage may be dropped without
invalidating the ones before it.

| Milestone | Work | Class | Closes | Done when |
|---|---|---|---|---|
| **MF-0 — This study** ✅ | The demotion analysis, the per-backend matrix, the opt-in shape. No code. | — | — | This note is linked from `API_ABI_STABILITY.md` and `README.md`. |
| **MF-1 — Verify, do not assume** | A two-thread compile + two-thread invoke test per backend; write "one context/emitter/allocator per session" down as a backend rule; record the AsmJit allocator answer. | B | Question 2 | LibJIT's test runs green in the default suite; SLJIT/AsmJit inherit the test at bring-up. |
| **MF-2 — Make concurrent entry *fail* instead of corrupt** | An explicit `AgentScope` at every host boundary; a per-instance "in call" guard that refuses or traps a second concurrent entry under `Single`; `callCallable` always acts on the current agent. | A | `MA-1` (first half), `MA-9` (model) | Two host threads calling one instance under `Single` produce a deterministic error, not a data race. |
| **MF-3 — Partition the instance's mutable GC bookkeeping** | `gcPending` / `gcCycleRoots` / counters per agent (or guarded by a per-instance safepoint lock); keep the frozen fields frozen. | B | `MA-3` (half) | A stress test with two agents allocating and releasing shows no lost queue entry and no double recycle. |
| **MF-4 — Safepoint protocol for collection** | Define and document when `gcCollect()` is legal with agents (quiescence or per-agent collect); enforce it. | B | `MA-3` (rest) | A documented rule an embedder can follow, and a test that violates it and gets a clean failure. |
| **MF-5 — Real `wait`/`notify`** | A guest-address-keyed waiter table behind the existing `wasm_atomic_wait*`/`notify`; keep the single-agent return values legal. | B | `MA-4` | A two-agent message-passing fixture (`ARA5`) passes without relying on the degenerate path. |
| **MF-6 — The opt-in surface** | `ConcurrencyModel`, `supportsConcurrency()`, `HostAgentScheduler` hooks, rejection at `instantiate()`. | A | `MA-1` (rest) | `Single` is bit-identical (same suite, same numbers); an unsupported model is refused before codegen. |
| **MF-7 — Two-agent conformance + memory model** | Trap isolation, join/cancel, shared memory/globals, and a written guest memory model. | A | `MA-9` | The two-agent suite is curated; the memory model is a note an embedder can implement against. |
| **MF-8 — Re-open the milestone question** | Re-run `API_ABI_STABILITY.md` §7: did any §2.2 surface change? If yes, this is a **new milestone**, not a gap closure. | — | — | The answer is recorded, whichever it is. |

The important ordering fact: **`MF-2` is worth doing even if agents are never
shipped.** Today a second host thread entering one instance is *undefined*; a
guard turns an undefined behaviour into a documented refusal, which is a
production-readiness improvement on its own.

## 8. Risks and open questions

| # | Risk / question | Why it matters | Where it is handled |
|---|---|---|---|
| 1 | **Reference-slot atomic stores do not run the GC retain/release barrier.** `SET2` states reference-slot exchanges are pointer-atomic but not leak-safe; a shared reference field can therefore drop a count. | This is the one *correctness* prerequisite for shared GC between agents, not a performance detail. | `MF-3` scope; currently a documented `SET2` gap. |
| 2 | **`ucontext` + threads.** Provider A migrates a suspended stack between host threads *serially*; concurrent resume of the same continuation is undefined. | A careless scheduler would break it. | Keep the serial rule; the scheduler documents it. |
| 3 | **ASan/TSan fiber hooks.** Provider A already calls the sanitizer fiber hooks; a second agent multiplies fake-stack bookkeeping. | Sanitizer builds are how races are found. | `MF-1`/`MF-7` add a TSan configuration. |
| 4 | **`currentExecutionState()` is thread-local.** Two agents on one host thread need explicit `AgentScope` nesting; a library that forgets gets the wrong trap slot. | Silently wrong agent. | `MF-2` (boundary scoping) + a debug assertion. |
| 5 | **Determinism.** `PROFILES.md` claims a deterministic profile; concurrency breaks it. | A false claim is worse than a missing one (PRD2). | `ConcurrentAgents` is outside the deterministic profile. |
| 6 | **Waiter-table encoding.** `waitqueue` (`0x68`) collides with `cont`. | Guest-visible type, upstream draft. | Class C until upstream assigns one; `MF-5` can ship the runtime table without the type. |
| 7 | **Cost of `refMatchesHeapType` (O(#functions))** and the ad-hoc `Value::kind` tagging (`GC H3`/`H7`) | Perf and latent correctness under load. | Existing risks; unchanged by agents, more visible with them. |
| 8 | **Unload/teardown with an agent in flight.** | Dangling `Callable`/`VMContext` — the existing `ABI.md` lifetime rule. | `MF-6` refuses teardown while an agent is inside a call. |
| 9 | **Does an agent need its own evaluation stack?** | `THREADSAFETY.md` §10 says each agent has its own; today the operand stack is the C stack. | Nothing to build for native-stack codegen; a documented consequence, not a subsystem. |

## 9. What this does not change

- **The current milestone stays single-agent.** `API_ABI_STABILITY.md` §5.3 pins
  it; nothing here reopens it. G1's QVM-successor framing explicitly wants one
  agent at a time.
- **`TD9`/`TH-5` stay the shipping decision.** This note makes the decision
  *reversible at a known price*, which is what a feasibility study is for.
- **No code, no header, no build change.** The only artefact is this document.
- **No new backend obligation.** Every stage is core-side or embedder-side.

## 10. Test survey

What exists today to build on (measured 2026-10-25 at `ea4fb30` unless a note
says otherwise):

| Evidence | Result | Reading |
|---|---|---|
| Default suite | 11,011 assertions / 442 cases, green | The single-agent baseline every stage must preserve. |
| `spec: atomic` (curated) | 249 passed / 0 failed / 48 skipped (`proposals/THREADS.md` T10) | All 67 atomic handlers execute with real seq-cst semantics. |
| Acquire-release fixture | Single-agent ordering + `pause` | The decode/lowering half is done; the *observation* half waits for a second agent (`ARA5`). |
| Host-thread test | Two independent instances on two threads | Proves setup isolation, **not** the target concurrency model (`THREADSAFETY.md` §8 says so explicitly). |
| Continuation tests | One-shot, trap/exception transfer, budget, stack exhaustion, serial migration | The execution-context machinery a scheduler would drive already exists. |
| Two-agent test | **Absent** | `MA-9`; the first thing `MF-7` adds. |
| Race detector / TSan run | **Absent from the docs** | `MF-1`: a concurrency claim without a detector is a guess. |

## 11. References

- [`proposals/THREADS.md`](proposals/THREADS.md) §2.1, §3, §4.3.1, `TD9`/`TH-5` —
  the agent model, the freedoms, and the nine blockers re-classified here.
- [`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md)
  `SET2`/`SET4`/`SET6` — the multi-mutator GC and shared-reference gaps.
- [`THREADSAFETY.md`](THREADSAFETY.md) §8–§10 — the supported model, the minimum
  embedder protocol and the non-normative future contract.
- [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) §10 — the
  mechanism × backend matrix this note's effort scale and shape follow.
- [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §12 — the `TRAP-6` per-agent state that
  makes trap isolation free.
- [`SAFETY.md`](SAFETY.md) §3.1 (`S1`, `S8`) — no step budget, serial
  continuation use.
- [`ABI.md`](ABI.md) "Other store-owned entities" — `gcCollect()` as an
  embedder-driven safepoint.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4/§5.3 — the class vocabulary
  and the single-agent boundary this note respects.
- [`LAYERING.md`](LAYERING.md) §4 — the eight contract rows a backend keeps.
- Source anchors: `WasmBase/WasmAgent.hpp`, `WasmBase/WasmModuleInstance.hpp`
  (`ModuleInstanceInternals`), `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmGcHeap.hpp`, `LibJit/LibjitModuleCompiler.cpp` (the build
  bracket), `Test/TestBackend.cpp` (the per-session `LibJIT::Context`).

