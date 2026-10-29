# Multi-Agent Execution — Feasibility of Demoting the Class-E Blockers

Status: feasibility study and implementation record (revision 2 — 2026-10-29;
the study is revision 1's measurements at `ea4fb30`, and **MF-1…MF-8 are now
implemented**). The opt-in model is built: `ConcurrencyModel::Agents`,
`Runtime::createAgent`, the entry discipline, the waiter table, concurrent GC
and the multi-agent tests. `Single` remains the milestone default, so the
current [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) gates are unchanged.
Audience: anyone revisiting `proposals/THREADS.md` `TD9`/`TH-5`, anyone bringing
up Asmjit or SLJIT, and anyone touching `Agent`, `ExecutionState`,
`ModuleInstance`, `Store`, `GcHeap`, the continuation providers, or the embedder
surface.

`API_ABI_STABILITY.md` §4 classified every open gap. Multi-agent execution was
the one family it marked **class E** — "the only known solution restructures the
backend set or the `WasmBase` ↔ backend contract" — and it pinned the current
milestone to the single-agent model precisely so that this classification did
not silently poison the milestone (§5.3 there). This note took the next step:
**can the E be split into cheaper pieces?** It answers three questions:

1. Can multi-agent execution be made **backend-agnostic**?
2. What does **multithreading** cost on each of the three intended backends
   (LibJIT today, AsmJit and SLJIT when they exist)?
3. Can multi-agent execution be **opt-in for the embedder**, the way the
   JS-integration hooks are?

Short answer: **yes / very little / yes** — because the agent's state already
lives outside the backends, and generated code reaches it through helpers rather
than embedded addresses. The expensive part was *not* the code generators; it was
the GC safepoint, the per-instance bookkeeping and the waiter table, all of which
are core-side and were staged as `MF-1…MF-7`.

The direction is recorded in §2.3: **at most one instance per module (multiple
instances are ruled out), and many agents on that one instance is the
implemented multithreading model.** Concurrent *execution* of compiled code and a
re-entrant invoker are supported; concurrent *compilation* is rejected as a
feature, because it is unsafe and its only consumer would be the instance
multiplicity the rule forbids.

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

### 2.3 The design direction (recorded 2026-10-25)

The owner's direction, stated here so it stops being folklore. It is **one rule
about cardinality** plus **one decision about concurrency**:

> **One instance per module — at most.** Multiple instances of the same `Module`
> are **ruled out by design**, not merely unsupported. **Many agents may execute
> that one instance's compiled code.** That is the accepted multithreading model.

This is deliberately not "WasmJit is single-threaded". It is: *the instance is
the unit of state, the agent is the unit of execution, and the population of
instances per module is exactly zero or one.*

### 2.3.1 What the cardinality rule buys

| Consequence | Why it matters |
|---|---|
| **`ModuleInstance` becomes a per-`Module` singleton** | `Runtime::instantiate(module)` can be idempotent: it returns the existing instance or creates the only one. There is no "which instance?" question in the API, in the type registry, or in a `Callable`. |
| **A baked `VMContext*` stays unambiguous** | `ABI.md` burns `Callable::context` into generated code; with one instance per module there is exactly one context to burn. |
| **"Make another instance" stops being a workaround** | [`THREADSAFETY.md`](THREADSAFETY.md) §9 already refuses it; the rule makes it a design fact instead of advice. |
| **It removes the consumer of concurrent compilation** | §2.3.3: the only reason to compile one module twice in parallel is to build two instances of it. |
| **Isolation is achieved by loading a module twice, not instantiating twice** | Two games, two tenants, two variants: two `Module` loads (two decoded modules, two type blocks in the `Store`) — which is what a runtime boundary already gives you. It is an **escape hatch, not the intended usage** (see below). |

**Isolation as a whole is discouraged.** The row above is a statement about what
the design makes *possible*, not a recommendation. The intended behaviour is to
treat a **long-lived module as if it were a dynamically linked library** — you
typically load it once, and it persists for the entire duration of the
application — and a **short-lived module as a *script***, e.g. a level script
from a late-1990s or early-2000s first-person shooter: loaded, run, and
discarded when the level ends. Ideally an embedder should **not require the
isolation at all**. That is again the "spiritual successor of the QVM" theme of
this library ([`ABI.md`](ABI.md) "Design Philosophy"): one resident module set
with a single instance each, not a per-tenant fleet of VMs.

### 2.3.2 The accepted scenarios

Everything in this table is **acceptable** design — the question is only whether
the engine supports it *today* and what an embedder must supply.

| # | Scenario | What it actually uses | Status |
|---|---|---|---|
| **C1** | Two host threads execute code from **two different instances** | Two `ModuleInstance`s, two `VMContext`s, one process `Store` | **Supported today** (host-thread test; `THREADSAFETY.md` §8) |
| **C2** | **Many agents on one instance** — two host threads both call into the same `ModuleInstance` | N `Agent`s, one instance, shared memories/tables/globals | **Accepted direction**, not supported yet (§4–§7) |
| **C2a** | *GPU-style*: launch N workers and pass a thread index | Ordinary Wasm arguments; an import that spawns/parks agents | Same as C2 — the "thread id" is just an `i32` parameter |
| **C2b** | *Classic style*: thread A runs export `f`, thread B runs export `g` | Two `Callable` calls on one instance | Same as C2 |
| **C2c** | *One compiled function called from several host threads* | `callCallable` on the same `Callable`, different arguments | **Yes, multi-agent by the engine's definition** — one agent per host thread; the minimal C2 case |
| **C3** | *Async/await*: an event loop, `await`, promise-style suspension | Continuations (stack switching) + `HostContinuationScheduler` | **Supported today**; concurrency without parallelism, and composable with C2 |
| **C4** | *Intra-Wasm threading*: `pthread`-like or WASI-threads primitives inside the guest | Agents created by an import, atomics, a waiter table, thread-local globals, per-agent stacks | **Accepted direction**; the WASI/guest ABI is not ours to invent (§2.3.4) |

The answer to the standing question — *"a Wasm function compiled to native code,
executed from multiple host threads: does that count as multi-agent /
multithreading?"* — is **yes**. `Agent` is the engine's name for an execution
context, and a host thread always carries one. Calling the same compiled function
from two threads is therefore the *same* problem as C2b, not a separate, easier
case: what makes it safe is the instance hardening in §2.3.5, not the fact that
the code path is shared (compiled code is re-entrant by construction, §4.2).

### 2.3.3 What is ruled out

| # | Ruled out | Rationale |
|---|---|---|
| **R1** | **Multiple instances of one module** (per-thread instances, "one instance per CPU", instance pools as an isolation model) | The design rule. It keeps `VMContext` identity, type identity and the `Callable` contract unambiguous, and it refuses the workaround that would otherwise hide every missing instance-level guarantee. Component semantics are the one sanctioned exception (§2.3.6). |
| **R2** | **Concurrent compilation** as a feature (two threads compiling at the same time) | Two reasons. **(a) Unsafe**: a backend's build region is a lock, not a parallel region — LibJIT's `jit_context_build_start`/`end` serialises the context, and the AsmJit allocator question is still open ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7). **(b) It has no consumer**: the only use for compiling one module twice in parallel is building two instances of it, which R1 forbids. Compilation is therefore **serialized by the frontend**: one compilation at a time, on whatever thread the embedder chose. |
| **R2′** | Concurrent *parsing/validation* as a guarantee | Not a feature either; the documented state is that it is not a formally snapshotted read path (`THREADSAFETY.md` §6). Loading happens on the loading thread. |

### 2.3.4 Accepted *capabilities* (no longer open questions)

These are not things to evaluate; they are requirements any backend and any
embedder path must satisfy:

| Capability | Status |
|---|---|
| **Concurrent execution of compiled code** | **Accepted.** Two agents may run generated code at the same time. Code is native code over `VMContext*`; nothing in the codegen layer is per-thread. |
| **Re-entrant invoker** | **Accepted and required.** `callCallable`, the harness invoker and a backend's `ContinuationInvokeFn` must keep all per-call state on the stack. A backend that caches "the current call" in its session is non-conforming (§4.3, §5). |
| **A guest-visible thread API** (`thread.spawn`, `pthread_create`, WASI threads) | **Accepted as direction, but it is an *import*.** The engine exposes agents; the embedder (or a WASI adapter) implements spawn/join by creating an agent that enters an export. `WasmBase` grows no scheduler and no guest runtime. |
| **An event loop *inside* the guest** | **Accepted.** A guest may implement its own loop; inside Wasm it is a loop, and its `await`-like points are whatever the guest builds. If it wants real suspension it uses continuations (C3), which the engine already provides. |

This is where the engine is deliberately **WASI-shaped**: primitives and
contracts, not the policy. The Component Model is what keeps the boundary honest
— a component is a typed, linkable unit, so "threads inside a component" is a
component-level question rather than a core-engine one
([`proposals/COMPONENT_MODEL.md`](proposals/COMPONENT_MODEL.md),
[`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) `JS7`).

### 2.3.5 The instance hardening this implies

C2 is acceptable **only if** the shared instance is safe. The list below is what
"hardened" means; §4.4 assigns each item a class, §7 sequences it, and
[`SAFETY.md`](SAFETY.md) §3.3 records the security half.

| Shared thing | Required before C2 is claimed |
|---|---|
| GC reclamation queue (`gcPending`, `gcCycleRoots`) | Per agent, or guarded by a per-instance safepoint lock |
| Reference count (`std::atomic_ref<uint32_t>`) | Already atomic — no change |
| Reference-slot stores | Must run the GC retain/release barrier (**done:** `wasm_atomic_ref_*`, SET2 closed) |
| Segment drop flags | Atomic |
| Safepoint (`gcCollect()`) | A documented quiescence rule for agents |
| Globals / tables / shared memory | Guest-synchronized (atomics); documented, not engine-enforced |
| `hostData` | Host-synchronized (embedder responsibility, `THREADSAFETY.md` §9) |
| Trap / exception / tail-call / defer / diagnostics | Already per agent (`TRAP-6`) |
| Invoker scratch | Per call (already) + required of every backend |
| Waiter table | Needed for C4 and for blocking `wait` under C2 |
| Compilation | Serialized (R2) |

### 2.3.6 The one sanctioned exception: the component runtime

There is exactly one place in the tree that creates more than one instance of one
`Module`, and it is not an embedder mechanism: a **component** may declare two
core instances of the same embedded core module
(`ComponentRuntime::processCoreInstance` calls `instantiator_.instantiate(...)`
once per declared `core instance`). Those are genuinely distinct instances —
separate memories, globals and tables — which is what instantiation *means* in
the component model.

So the rule is stated precisely:

> **An embedder (and a runtime) creates at most one instance per `Module`. The
> component runtime creates one instance per declared core instance, because that
> is the component model's own semantics — and it is therefore also the owner
> that must keep those instances serialized.**

This is not a loophole: it changes no embedder-facing capability, and it is
recorded here so that nobody "discovers" it later and concludes the rule is soft.

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
engine. Note that **Q1 is no longer "can it?"**: concurrent compilation is
rejected as a feature (§2.3.3 R2), so the question is only *which object must be
per-thread* and *whether the engine documents the serialization*.

| # | Property | Why it matters |
|---|---|---|
| Q1 | Which object must be **per compilation** (and is compilation serialized)? | Compilation is serialized by rule (R2); the backend must not rely on being called concurrently, and must not require a shared build object. |
| Q2 | Can separately compiled code **execute** concurrently? | C1 and C2 both need it — **accepted, required**. |
| Q3 | Does the engine hold **mutable global state** a second thread would race on? | The only class-B risk a backend can introduce. |
| Q4 | Is the **invoker** re-entrant (per-call scratch)? | **Required** (§2.3.4): several agents may call through it at once. |

### 5.1 GNU LibJIT (the only executable backend)

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Serialized, and the context is the unit.** Compilation is bracketed by `jit_context_build_start()` / `jit_context_build_end()`, which the code documents as *a lock on that context*; a wedged context blocks every later build. The frontend never compiles two modules at once (R2), and the harness already uses one `LibJIT::Context` per session — so the build lock is never contended. | `LibJit/LibjitModuleCompiler.cpp:67–82`; `Test/TestBackend.cpp` (`LibJitSession` owns one `LibJIT::Context`) |
| Q2 | **Yes.** Generated functions are ordinary native code taking `VMContext*`; they embed no per-compilation mutable state. The only per-call machinery is the host-side invoker. | `ABI.md` "Overview"; `WasmValue.hpp` |
| Q3 | **The type translator is per-context** (`LibJIT::Context` owns it and its `TypeId`-keyed cache), so there is no shared translator. LibJIT's built-in types are immutable. | `LibJitContext.hpp`; `LibJitTypeTranslation.cpp` |
| Q4 | **Yes for the harness invoker**: its scratch is a local `std::vector` per call, and the `v128` workaround builds arguments on the stack. `jit_apply` is called with a per-call argument array. `MF-1` turns this into a test (concurrent *invoke*, not concurrent compile). | `Test/TestBackend.cpp` `LibJitSession::invoke` |

Residual LibJIT-specific tension: the installed build has quirks already
documented (`jit_insn_store_relative` on aggregates; the `jit_apply` path for a
third stack-passed aggregate, which is why the harness has a `v128` path). Those
are *correctness* quirks, not concurrency ones, but a second agent makes any
"per-session hidden state" more visible, so `MF-1` should exercise concurrent
**invocation** and a deliberately interleaved two-agent call rather than assume
the invoker is re-entrant.

### 5.2 SLJIT

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Not relied upon.** Upstream states "The compiler is thread-safe", which is a bonus, not a dependency: one `sljit_compiler` per compilation and compilation is serialized (R2). | upstream `sljit_src/sljitLir.h` (feature list), read 2026-10-25 |
| Q2 | **Yes.** SLJIT emits into a caller-owned buffer; the generated code has no runtime dependency on the compiler object once emitted. The backend owns the W^X handoff (allocate → emit → `mprotect` RX). | same header; `SIMD_BACKENDS.md` §3 |
| Q3 | **None documented.** Registers and stack are handled manually per `sljit_emit_enter`; the local area is fixed at entry, so per-call scratch must be a *frame* slot, not a compiler field. | `STACK_SWITCHING.md` §10.3 |
| Q4 | **Our obligation**, not SLJIT's: the invoker must not cache "the current call" in the session. | design rule |

### 5.3 AsmJit

| Question | Finding | Evidence |
|---|---|---|
| Q1 | **Not relied upon.** `CodeHolder` + a `BaseEmitter` per compilation, `Function`/`FuncFrame`/PEI per emit, and compilation is serialized (R2). The allocator question below therefore only asks whether one allocator can serve several *sequential* compilations. | upstream `asmjit` core docs (read 2026-10-25); `STACK_SWITCHING.md` §10.3 |
| Q2 | **Yes.** Emitted code is plain native code in executable memory; AsmJit keeps no thread-local execution state. | same |
| Q3 | **One open item: executable-memory ownership.** `JitRuntime`/`JitAllocator` own the code pages; whether `add()` may be called concurrently is *not* stated in the API docs we have read. Under R2 that question is moot for compilation; it only matters if two *sessions* share one allocator, so the rule is one allocator per session, and a bring-up check. | to verify (`MF-1` checklist) |
| Q4 | Same requirement as SLJIT: the invoker keeps its per-call state on the stack. | design rule |

### 5.4 The matrix

`◑` = feasible with the stated precondition (here: the backend exists).
`R2` = rejected as a feature by §2.3.3, so it is *not* a capability to assess.

| Capability | LibJIT | SLJIT | AsmJit | Caveat |
|---|---|---|---|---|
| **Compilation is serialized** (never two at once) | **✅ by rule** | **✅ by rule** | **✅ by rule** | The frontend serializes; the backend's build lock/allocator is never contended. |
| Concurrent **compilation** | **✗ R2** | **✗ R2** | **✗ R2** | Rejected: unsafe and without a consumer (R1). |
| Concurrent **execution** of compiled code | **✅ S** | **◑ S** | **◑ S** | Required; needs the core-side partitioning (§4.4 `MA-2`/`MA-3`), not backend work. |
| **Re-entrant invoker** | **✅ S** (verify) | **◑ S** | **◑ S** | Required; keep all per-call state on the stack. |
| Shared mutable **global state** in the engine | **none found** | **none documented** | **none documented** | The one AsmJit question is the code allocator, and only across sessions. |
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
| Does any engine require concurrent compilation to be available? | **No, and none is asked for it** (R2). LibJIT's build region is a lock, SLJIT's compiler is documented thread-safe, and AsmJit needs one emitter/code-holder per compilation — all three are compatible with a serialized frontend. |
| Is the per-backend work "re-architect" or "verify"? | **Verify.** Three one-line rules (serialize compilation, re-entrant invoker, one allocator per session), one test per backend. |
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
| **A concurrency mode** in the runtime/instantiation config | `enum class ConcurrencyModel { Single, Agents }`, default `Single` (one agent inside a call at a time), validated at `instantiate()` the way `ProfileConfig` is | A |
| **A capability query** | `supportsConcurrency(model)`; a backend or the runtime may refuse a model at the boundary, loudly | A |
| **A host scheduler hook** | `HostAgentScheduler`: an aggregate of function pointers (`spawn`, `park`, `wake`, `join`, `step`) plus `userData`, borrowed and never owned | A |
| **An agent handle** | `Agent&` / `AgentId` exposed to the embedder; one per OS thread by default | A |
| **Instance cardinality** | `Runtime::instantiate` enforces R1: one instance per `Module`, returned idempotently; a second instantiation is refused | A |
| **Per-instance entry discipline** | `Single` keeps "one agent inside a call at a time" (the `MF-2` guard); `Agents` relaxes it with the §4.4 `MA-2`/`MA-3` work | B |

The core still owns **no scheduler**. The embedder supplies it, exactly as it
supplies the JS event loop in `proposals/JS_INTEGRATION.md` — the engine gives
the seam, the game gives the policy.

### 6.3 What "opt-in" must guarantee

1. **`Single` is bit-identical to today.** Same code paths, no extra lock, no
   extra allocation, no behaviour change. The default must not read the config
   beyond a null check.
2. **Unsupported models fail at the boundary**, not mid-call. A module loaded
   with `Agents` on an engine that cannot honour it is rejected at
   `instantiate()` (the `ProfileConfig` precedent), before any code is generated.
3. **The engine never invents a thread.** Spawning a host thread, growing a
   thread pool or installing a signal handler is the embedder's act.
4. **Guest-visible ordering stays the guest's problem.** Shared globals/tables
   are shared state; the guest uses `global.atomic.*` / atomics, as
   `proposals/THREADS.md` §3 permits an engine to require.
5. **The safepoint stays explicit.** `gcCollect()` remains an embedder-driven
   safepoint; with agents the runtime must document *when* it is safe (no agent
   inside a call). `Runtime::safepoint()` skips an instance that reports an
   in-flight call, but under `Agents` that check is advisory: the embedder calls
   it only when it knows no agent can enter (normally after joining its threads).

### 6.4 Failure modes the design must name

| Failure | Why it is new | Mitigation |
|---|---|---|
| Blocking `wait` with no other runnable agent | Today `wait` returns "timeout" without sleeping — legal but degenerate | The waiter table (`MA-4`) plus the scheduler hook; keep the degenerate result legal for `Single` |
| A trapped agent | Trap state is already per agent | Nothing to do — this is the `TRAP-6` payoff |
| Cancelling/joining an agent mid-call | No current concept | Scheduler-level; the core exposes "is this agent in a call" and a `park` point at the boundary |
| Deterministic replay | Single-agent determinism is a `PROFILES.md` claim | `Agents` is *outside* the deterministic profile; document it |
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
| **MF-1 — Verify, do not assume** ✅ | **Landed 2026-10-29:** `[concurrency]` calls the *same* compiled function from two host threads (2000 calls each, all correct) and, in the process, exposed a real gap: a call from a thread that did not create the instance tripped the instantiating thread's stack guard. The host boundary now installs the calling thread's limit (`ensureCurrentThreadStackLimit`, `SAFETY.md` §2.8), so cross-thread calls work. SLJIT/AsmJit inherit the test at bring-up. | B | Question 2 | LibJIT's concurrent-invoke test runs green in the default suite ✅ |
| **MF-2 — Make concurrent entry *fail* instead of corrupt** ✅ | **Landed 2026-10-29:** `ModuleInstance::tryEnterCall`/`leaveCall` plus `Runtime::callExport` refuse a second concurrent entrant under `Single`; `Runtime::instantiate` enforces R1; and the host boundary installs the *calling* thread's stack guard (`ensureCurrentThreadStackLimit`, `SAFETY.md` §2.8), which the cross-thread test exposed as missing. `[concurrency]` exercises a real second-thread refusal and a re-entrant function across threads. | A | `MA-1` (first half), `MA-9` (model), **R1 enforcement** | Two host threads on one instance under `Single` produce a deterministic error, not a data race ✅; a second instantiation is not expressible ✅ |
| **MF-3 — Partition the instance's mutable GC bookkeeping** ✅ | **Landed 2026-10-29:** the note's sanctioned alternative — a per-instance safepoint lock. `ModuleInstanceInternals::gcMutex` (recursive) guards `gcPending`, `gcCycleRoots` and the counters; `reclaimGcObject`, `possibleCycleRoot`, `collectCycles`, `gcCollect` and the diagnostic getters take it. `[concurrency]` drives two quiescent agents through `gcCollect()` concurrently. The two-agent *allocation* stress test is `MF-7`, because it needs `Agents`. | B | `MA-3` (half) | Concurrent collectors serialize instead of racing ✅; the allocation stress test is `MF-7`. |
| **MF-4 — Safepoint protocol for collection** ✅ | **Landed 2026-10-29:** the rule is *quiescence*. `ModuleInstance::gcCollect()` refuses a call that is inside the instance (`inCall()`) with a named error, and `Runtime::safepoint()` skips an instance with a call in flight. Documented in `ABI.md` "Other store-owned entities" and `SAFETY.md` §2.8; `[concurrency]` covers the refusal and the skip. | B | `MA-3` (rest) | A documented rule an embedder can follow, and a test that violates it and gets a clean failure ✅ |
| **MF-5 — Real `wait`/`notify`** ✅ | **Landed 2026-10-29:** a guest-address-keyed waiter table behind `wasm_atomic_wait*`/`notify` (condition variables, real timeouts, an infinite timeout, no lost wake-up, no spurious wake). `notify` returns the number it woke; `wait` returns 0/1/2. | B | `MA-4` | A two-agent wait/notify fixture passes; a notify without a value change wakes the waiter (`[multiagent]`). |
| **MF-6 — The opt-in surface** ✅ | **Landed 2026-10-29:** `ConcurrencyModel { Single, Agents }` and `supportsConcurrency()` (`WasmConcurrency.hpp`), `HostAgentScheduler` hooks (declared, embedder-owned, never called by the core), `RuntimeConfig::concurrency`, validation at `Runtime::instantiate` (and at `create`), and `Runtime::createAgent()`. | A | `MA-1` (rest) | `Single` is the unchanged default; `Agents` is accepted and drives the atomic entry counter ✅ |
| **MF-7 — Two-agent conformance + memory model** ✅ | **Landed 2026-10-29:** trap isolation, join/cancel as an embedder responsibility, shared memory/globals, a concurrent GC allocation/reclamation stress and a written guest memory model (use the atomic instructions). `[multiagent]` is the two-agent suite; it runs under `Agents` in the default build. | A | `MA-9` | The two-agent suite is curated and green (6 cases / 51 assertions) ✅ |
| **MF-8 — Re-open the milestone question** ✅ | **Answered 2026-10-29:** enabling `Agents` was the one decision this note reserved. It changed no §2.2 surface — `RuntimeConfig`, `ConcurrencyModel` and the extra `Callable` field are additive; the entry counter, the waiter table and the GC guards are internal. The `API_ABI_STABILITY.md` gates therefore stay met with `Single` as the default and `Agents` as an additive opt-in. | — | — | The answer is recorded ✅ |

**What landed (2026-10-29).** `MF-1`…`MF-8` are complete. `MF-6` added the
opt-in surface; `MF-2` refuses a second entrant under `Single` and enforces one
instance per module; `MF-3`/`MF-4` made the GC and safepoint rules deterministic;
`MF-5` added the real waiter table; `MF-7` added the two-agent conformance suite.
The cross-thread test also exposed a real gap in the stack guard: the limit
captured at instantiation belongs to the creating thread, so a call from any
other thread tripped it immediately. The host boundary now installs the *calling*
thread's own limit (`ensureCurrentThreadStackLimit`, `SAFETY.md` §2.8), which is
what makes the concurrent-invoke and second-thread tests pass.
`[multiagent]` covers acquire-release message passing, wait/notify, shared-global
RMW, trap isolation, GC stress and the reference-slot barrier.

The important ordering fact: **`MF-2` is worth doing even if agents are never
shipped.** Today a second host thread entering one instance is *undefined*; a
guard turns an undefined behaviour into a documented refusal, which is a
production-readiness improvement on its own.

## 8. Risks and open questions

| # | Risk / question | Why it matters | Where it is handled |
|---|---|---|---|
| 1 | ~~**Reference-slot atomic stores do not run the GC retain/release barrier.**~~ **Closed 2026-10-29:** `wasm_atomic_ref_store/xchg/cmpxchg` combine the pointer-atomic update with the retain/release pair, and the global/table/struct/array reference-typed handlers use them. | This was the one *correctness* prerequisite for shared GC between agents, not a performance detail. | Closed (`[multiagent]` reference-slot barrier case). |
| 2 | **`ucontext` + threads.** Provider A migrates a suspended stack between host threads *serially*; concurrent resume of the same continuation is undefined. | A careless scheduler would break it. | Keep the serial rule; the scheduler documents it. |
| 3 | **ASan/TSan fiber hooks.** Provider A already calls the sanitizer fiber hooks; a second agent multiplies fake-stack bookkeeping. | Sanitizer builds are how races are found. | **Closed 2026-10-29:** `TESTING.md` documents the `CONFIG+=sanitizer CONFIG+=sanitize_thread` build; `[multiagent]` and `[concurrency]` run with **no TSan report**. |
| 4 | **`currentExecutionState()` is thread-local.** Two agents on one host thread need explicit `AgentScope` nesting; a library that forgets gets the wrong trap slot. | Silently wrong agent. | `MF-2` (boundary scoping) + a debug assertion. |
| 5 | **Determinism.** `PROFILES.md` claims a deterministic profile; concurrency breaks it. | A false claim is worse than a missing one (PRD2). | `Agents` is outside the deterministic profile. |
| 6 | **Waiter-table encoding.** `waitqueue` (`0x68`) collides with `cont`. | Guest-visible type, upstream draft. | Class C until upstream assigns one; `MF-5` can ship the runtime table without the type. |
| 7 | **Cost of `refMatchesHeapType` (O(#functions))** and the ad-hoc `Value::kind` tagging (`GC H3`/`H7`) | Perf and latent correctness under load. | Existing risks; unchanged by agents, more visible with them. |
| 8 | **Unload/teardown with an agent in flight.** | Dangling `Callable`/`VMContext` — the existing `ABI.md` lifetime rule. | The embedder joins every agent before teardown (`THREADSAFETY.md` §9); the core never unloads a plugin and the runtime outlives its instances. |
| 9 | **Does an agent need its own evaluation stack?** | `THREADSAFETY.md` §10 says each agent has its own; today the operand stack is the C stack. | Nothing to build for native-stack codegen; a documented consequence, not a subsystem. |

## 9. What this does not change

- **The current milestone's gates stay as they were.** `API_ABI_STABILITY.md`
  G1–G4 describe a QVM-successor for a *single-agent* embedding; nothing here
  reopened them, and no gate waited on `MF-*`. What the implementation changes is
  the **status of the family**: multi-agent is no longer "deferred indefinitely",
  it is "accepted, opt-in and implemented".
- **`Single` remains the default.** An embedder that never sets
  `ConcurrencyModel::Agents` gets the previous one-entrant contract, and
  `THREADSAFETY.md` §9's serialization advice still applies to that model. The
  bridge is open only when the embedder selects it.
- **Additive surface only.** `RuntimeConfig::concurrency`,
  `ConcurrencyModel`/`supportsConcurrency` and `Callable::tailEntry` are
  additions; the entry counter, waiter table and GC guards are internal. The
  `Test/abi/*.exports` baseline records the additions and the additive-only
  rule stays green.
- **One new backend obligation, documented in the work order.** A code generator
  must fill `Callable::tailEntry` with a per-type tail-dispatch thunk (or leave
  it null and accept the `call` + `return` fallback). Everything else in the
  agent work is core-side or embedder-side.

## 10. Test survey

What exists today to build on (measured 2026-10-25 at `ea4fb30` unless a note
says otherwise):

| Evidence | Result | Reading |
|---|---|---|
| Default suite | **11,454 assertions / 481 cases, green** (2026-10-29) | The single-agent baseline every stage must preserve; the earlier 11,024-assertion run is a subset of it. |
| `spec: atomic` (curated) | 249 passed / 0 failed / 48 skipped (`proposals/THREADS.md` T10) | All 67 atomic handlers execute with real seq-cst semantics. |
| Acquire-release fixture | Single-agent ordering + `pause` | The decode/lowering half is done; the *observation* half waits for a second agent (`ARA5`). |
| Host-thread test | Two independent instances on two threads | Proves setup isolation, **not** the target concurrency model (`THREADSAFETY.md` §8 says so explicitly). |
| Continuation tests | One-shot, trap/exception transfer, budget, stack exhaustion, serial migration | The execution-context machinery a scheduler would drive already exists. |
| `[concurrency]` (2026-10-29) | 81 assertions / 14 cases, green | The model claim, the `Single` refusal, the in-call guard under a real second-thread call, a re-entrant compiled function across threads, and the safepoint rules. |
| Cross-thread stack guard | `[concurrency]` proves a call from a non-creating thread is guarded by that thread's own limit | `SAFETY.md` §2.8; before the fix such a call trapped immediately with `call stack exhausted`. |
| `[runtime]` (2026-10-29) | 46 assertions / 7 cases, green | R1 (`instantiate` is idempotent), the guard under a simulated second entrant, and agent hand-out. |
| `[multiagent]` (2026-10-29) | **51 assertions / 6 cases, green** | Two agents on one instance: acquire-release message passing, real wait/notify, shared-global RMW, trap isolation, GC allocation stress/reclamation and the reference-slot barrier. |
| Race detector / TSan run | **Configuration documented; `[multiagent]` is the case to run under it** | `MF-1`: a concurrency claim without a detector is a guess. `TESTING.md` records the `CONFIG+=sanitize_thread` build; the LibJIT library itself is uninstrumented. |

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
- [`SAFETY.md`](SAFETY.md) §3.1 (`S1`, `S8`) and §3.3 (the multi-agent threat-model
  rows) — no step budget, serial continuation use, and what N agents sharpen.
- [`ABI.md`](ABI.md) "Other store-owned entities" — `gcCollect()` as an
  embedder-driven safepoint.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4/§5.3 — the class vocabulary
  and the single-agent boundary this note respects.
- [`LAYERING.md`](LAYERING.md) §4 — the eight contract rows a backend keeps.
- Source anchors: `WasmBase/WasmAgent.hpp`, `WasmBase/WasmModuleInstance.hpp`
  (`ModuleInstanceInternals`), `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmGcHeap.hpp`, `LibJit/LibjitModuleCompiler.cpp` (the build
  bracket), `Test/TestBackend.cpp` (the per-session `LibJIT::Context`).

