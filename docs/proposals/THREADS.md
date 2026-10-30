# Threads and Atomics — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_threads}

Status: design note / implementation reference (revision 12 — **TH-1…TH-6 are
implemented**; TH-5 (real agents) is built as an opt-in
`ConcurrencyModel::Agents` with a guest-address-keyed waiter table, concurrent
entry into the one instance, per-agent trap state and a multi-mutator-safe GC
path. `Single` stays the default and is bit-identical to the previous revision.
**At most one instance per module; multiple instances are ruled out**, and
concurrent compilation is rejected as a feature
([`../THREADSAFETY.md`](../THREADSAFETY.md) §3.1–§3.2).
The per-agent state groundwork is implemented: `Agent` owns an
`ExecutionState` (trap, exception, tail-call, defer, counters and stack limit).
The stack-switching A/B providers are continuation execution contexts, not
agents, and do not change the multi-agent model. §3 records the project's
execution model: **`Single` by default, `Agents` opt-in**; §4.3.1 records how
each former blocker was closed.)
Audience: anyone touching `WasmMemory`, `WasmStore`, `WasmOpcode`,
`WasmOpcodeDispatcher`, `LibjitOpcodeDispatcher`, or the test harness.

This note follows the same split as [`GC.md`](GC.md) and
[`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md):

1. what the threads proposal **demands**;
2. what it explicitly does **not** demand (and where an engine is free);
3. what `WasmJit` **currently implements**;
4. the **gaps** between (3) and (1);
5. where we can and should play **fast and loose** on purpose.

It contains **no new code**. The guest/host responsibility split this note
assumes is stated in [`THREADSAFETY.md`](../THREADSAFETY.md).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the threads proposal require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §4.3.1 | What are the concrete blockers to true multi-agent / multithreaded execution? |
| §5 | Where do we fall short of §2? |
| §6 | Where do we *choose* to deviate, and why? |
| §7 | Milestones to close the important gaps |
| §8 | Indicative test survey |
| §9 | References |

## 2. The proposal's model and what it demands

**Status first.** The threads proposal (shared linear memory, atomic
instructions, wait/notify, `atomic.fence`) is **not part of WebAssembly 3.0**.
The 3.0 change history lists no thread/atomic feature, and the 3.0 instruction
index contains no `atomic.*` instruction. It is a separate, widely implemented
proposal. Everything below is therefore "what a conforming *threads-enabled*
engine must do", not "what 3.0 requires".

### 2.1 Agents and agent clusters

- An **agent** is an execution context (for the web embedding, an ECMAScript
  agent) extended with a WebAssembly stack and evaluation context; informally a
  *thread*. All agents belong to an **agent cluster**.
- Agents in a cluster may run concurrently and may share memories.

### 2.2 Shared linear memory

- A memory can be marked **shared**, in which case it can be imported/exported
  and shared between all agents of a cluster.
- Import matching must agree on shared-ness: importing shared memory without
  declaring it shared (or vice versa) is a **validation error**.
- A shared memory **must declare a maximum size**.
- `memory.grow` and `memory.size` have **sequentially consistent** ordering.
  Growing a shared memory commits the new pages for every agent; it can fail
  for the usual max reason or for reservation failure.
- **Data-segment initialisation** is specified precisely for shared memory: in
  definition order, low to high, byte granularity, as non-atomic accesses, and
  the whole module's data initialisation then **synchronizes** with other
  agents (a barrier).

### 2.3 Atomic memory accesses

- New atomic **loads/stores**: `i32.atomic.load`, `i32.atomic.load8_u`,
  `i32.atomic.load16_u`, `i64.atomic.load`, `i64.atomic.load8_u`,
  `i64.atomic.load16_u`, `i64.atomic.load32_u`, and the matching
  `*.atomic.store{,8,16,32}` forms. All accesses are **sequentially
  consistent**; there is no weaker ordering.
- RMW returns the **pre-modification** value; narrow accesses zero-extend the
  loaded value to the result width, and wrap the operand before storing.
- `cmpxchg` reads, compares against `expected` (wrapped to the access width),
  conditionally stores `replacement`, and always returns the loaded value.
- **Alignment**: the memarg's alignment immediate must be exactly the natural
  alignment of the access size (validation error otherwise), and a
  **misaligned effective address traps** at runtime. Non-atomic accesses to
  shared memory are *not* required to be aligned.

### 2.4 Wait and notify

- `memory.atomic.notify` (addr, count) → number woken; sequentially consistent.
- `memory.atomic.wait32` / `wait64` (addr, expected, timeout-ns as `i64`) → `i32`:
  - `0` "ok" (woken), `1` "not-equal", `2` "timed-out".
  - Starts with an atomic load; if it differs from `expected`, returns `1`.
  - `timeout < 0` means "never expires".
  - The spec explicitly says a suspended agent is **not spuriously woken**.
  - Wait traps if the memory is **unshared**, or if the address is misaligned
    or out of bounds. Notify's alignment requirement is 32 bits.
- Trap if the waiter count would reach 2³².

### 2.5 `atomic.fence`

- A sequentially consistent fence with no operands, not tied to any memory
  (valid even in a module with no memory). The binary encoding carries a
  reserved `0x00` immediate.

### 2.6 Mutable global imports/exports

- The ability to import/export **mutable** globals was split into its own
  proposal and is now core; it is the natural way to share a flag/lock between
  agents without a linear memory.

## 3. What the proposal does not demand

- **No concurrency is required.** An engine may accept shared memories and
  atomic instructions while only ever running **one agent** at a time. In that
  execution model every atomic access is trivially sequentially consistent and
  there is no observable difference from a real implementation. Nothing in the
  proposal forces the embedder to spawn OS threads.

  **Project decision (revision 12).** This engine ships **`Single` as the
  default** and **`Agents` as an opt-in** execution model. Multi-agent execution
  — many agents on the *one* module instance (there is at most one; multiple
  instances are ruled out) — is implemented: concurrent entry, the cross-agent
  waiter table, per-agent trap state and the multi-mutator-safe GC path are
  `TH-5`. `Single` remains a conformant engine and the shipping default; the
  embedder selects `Agents` in `RuntimeConfig`. See `TD9` in §6, `TH-5` in §7,
  and
  [`../MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md).
- **No scheduling model.** No thread pool, fairness, work stealing, priority,
  or number of parallel agents is specified.
- **`notify` may wake fewer waiters than asked** (including zero); it returns
  how many it actually woke. On an unshared memory it can only return non-zero
  if the *host* created a waiter, which is a web-embedding concern.
- **A finite `wait` is only obliged to return `2` when its timeout expires**;
  the spec rules out spurious wakeups, but it does not mandate a sleep
  granularity, a timer resolution, or that a suspended agent consume no CPU
  while suspended is implementable everywhere.
- **No memory model beyond atomics.** Programs that contain data races on
  non-atomic accesses are not given any guaranteed behaviour; the proposal
  pins only the atomic operations and the `grow`/data-init rules.
- **Atomics are per-access-size.** Implementations may use lock prefixes,
  `__atomic_*` builtins, a striped lock, or a single global lock over the
  memory; only the observable atomicity/ordering matters.
- **No requirement that non-atomic accesses to shared memory be atomic**; only
  naturally-aligned scalar accesses are guaranteed to be tear-free in
  practice, and the spec does not require even that for non-atomics.
- **No thread-local storage / agent-local state** in the proposal.
- **Wait/notify return `1` vs `2`**: an implementation that cannot suspend an
  agent is still conformant for finite timeouts as long as it eventually
  returns `2` and never returns `0` without a notify.

## 4. What we have implemented today

### 4.1 Memory classes (`WasmBase/WasmMemory.{hpp,cpp}`)

- `LinearMemory` is the JIT-visible POD view: `memoryBase` (field 0),
  `memorySize`, `memoryMax`, `bool isShared`, `hostData`.
- `StoreOwnedMemory` is the owning base; `Store::createLinearMemory(initial,
  max, isShared)` picks one of:
  - **`PrivateLinearMemory`** — a `std::vector<uint8_t>`; `grow` resizes
    (base can move; nobody holds it concurrently).
  - **`SharedLinearMemory`** — reserves the *whole* maximum address range up
    front (`mmap(PROT_NONE)` / `VirtualAlloc(MEM_RESERVE)`, capped at 4 GiB),
    commits the initial pages, and grows by committing more
    (`mprotect` / `VirtualAlloc(MEM_COMMIT)`). The base pointer **never moves**,
    which is exactly what a shared memory needs. `grow` takes a `std::mutex`;
    a zero-page grow always succeeds.
- `MemoryType` parses the limits flags: bit 0 = max present, **bit 1 = shared**,
  bit 2 = 64-bit index (`MemoryIndexType::I64`). `ModuleInstance` propagates
  bit 1 into `Store::createLinearMemory`.
- The `Store` owns memories, so two instances can import the same
  `(module, field)` and observe one memory; memory imports/exports and
  multi-memory are wired (`docs/ABI.md`).

### 4.2 Opcodes

- `WasmOpcode.hpp` enumerates the complete `0xFE` **`AtomicOpcode`** set: 67
  entries covering `memory.atomic.notify`, `memory.atomic.wait32/64`,
  `atomic.fence`, atomic loads/stores, all RMW add/sub/and/or/xor/xchg
  variants, and all `cmpxchg` variants.
- `WasmOpcodeDispatcher::dispatchPrefixAtomic` decodes the immediates
  correctly: `atomic.fence` consumes its `u8` ordering immediate (0 or 1) and
  `pause` (`0xFE 0x04`) has none; every other baseline atomic reads an
  `AtomicMemArg` (ordering-capable). Ordinary `MemArg` rejects bit 4.
- **All 67 handlers execute (TH-1).** Each one reuses `effectiveMemoryAddress`,
  so the memory index, unsigned address widening, wrap-safe bounds check and
  allocation budget are identical to a scalar load/store, then calls a helper
  in `WasmBase/wasm_atomic.*` that performs the access with `__atomic_*` at
  seq_cst and traps on a misaligned effective address
  (`TrapKind::UnalignedAtomicAccess` → "unaligned atomic"). The 67 entry points
  are generated from four macro lists in one place, so they cannot drift from
  the virtual declarations. `i64.atomic.rmw8/16/32.*` pass their width and get
  the zero-extended old value back, exactly like the instruction.
- `atomic.fence` lowers to `wasm_atomic_fence(ordering)` =
  `__atomic_thread_fence(__ATOMIC_SEQ_CST|__ATOMIC_ACQ_REL)`; `wait`/`notify`
  use the cross-agent waiter table (TH-2/TH-5, §4.3).
- The non-JIT `Stub::OpcodeDispatcher` implements the same handlers as textual
  pretty-printers (useful for inspection, no execution).

### 4.3 The agent execution model (TH-5, revision 12)

- **Agents are opt-in.** `RuntimeConfig::concurrency =
  ConcurrencyModel::Agents` admits several host threads into the one
  `ModuleInstance` at once; `Single` (the default) keeps the previous
  one-inside-a-call contract and refuses a concurrent entrant with a named
  error. The embedder owns the scheduler and the OS threads: it creates an
  `Agent` per thread (`Runtime::createAgent`), installs it with `AgentScope`,
  and calls exports through `Runtime::callExport`. The core spawns no thread —
  `HostAgentScheduler` is declared, embedder-owned and never called by the
  engine ([`../MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md)
  §6.2).
- **A real cross-agent waiter table** (`wasm_atomic.cpp`) keys waiters by the
  effective address generated code already computed. `wait` re-reads the cell
  under the table mutex, parks on a condition variable with a real timeout and
  returns 0 (woken), 1 (not-equal) or 2 (timed out); `notify` wakes up to
  `count` waiters and returns the number woken. A notify that arrives before
  the park returns 0, so a retry loop is safe.
- **Multi-mutator-safe instance state.** `activeCalls` is an atomic counter
  (not a flag), the GC reclaim queue and cycle candidates stay under the
  per-instance `gcMutex`, the GC allocation and reference counters are atomic,
  the data/element segment drop flags are atomic, and reference-typed atomic
  slot updates run a retain/release barrier
  (`wasm_atomic_ref_store/xchg/cmpxchg`,
  [`SHARED_EVERYTHING_THREADS.md`](SHARED_EVERYTHING_THREADS.md) SET2).
- **The store containers, type interning, GC heap and string store are
  mutex-guarded.** Concurrent *compilation* remains rejected (the backend
  build region is a lock); concurrent *execution* of already-compiled code is
  the supported path.
- **At most one `ModuleInstance` per `Module`** (the cardinality rule). A
  second agent is a second execution context on the same instance, not a
  second instance. The component runtime is the one sanctioned exception,
  because one `core instance` per declaration is the component model's own
  semantics (see [`../MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md)
  §2.3.6).
- **Guest-visible ordering is the guest's responsibility.** Shared globals,
  tables and memory are shared state; the guest uses `global.atomic.*`,
  `table.atomic.*`, `struct.atomic.*`, `array.atomic.*`,
  `i32/i64.atomic.*` and `memory.atomic.wait/notify`. The engine provides the
  operations, not a guest mutex or a language runtime's lock.

### 4.3.1 Former blockers, and how each was closed

| # | Former blocker | How it was closed (2026-10-29) | Where it lives now |
|---|---|---|---|
| MA-1 | No scheduler or thread lifecycle. | **Closed by scope, deliberately:** the core owns no scheduler. `Runtime::createAgent` + `AgentScope` give one execution context per host thread; the embedder spawns/joins the threads, exactly as `HostAgentScheduler` describes. `wait`/`notify` now wake real waiters. | `WasmAgent.hpp`; `WasmRuntime.hpp`; `WasmConcurrency.hpp` |
| MA-2 | One mutable `ModuleInstance` per module, unsynchronized entry. | **Closed:** `activeCalls` is an atomic counter; `Single` keeps the one-entrant refusal, `Agents` admits every entrant. Segment drop flags are atomic; all other mutable instance state is either guest-synchronized (globals/tables/memory) or lock-guarded (GC). | `WasmModuleInstance.hpp` (`enterCall`/`leaveCall`), `WasmModuleInstance.cpp` (`*.store/load`) |
| MA-3 | No multi-mutator GC. | **Closed:** the reclaim queue, cycle candidates and counters are under `gcMutex`; the allocation counters and segment flags are atomic; `GcHeap` is mutex-guarded; the refcount is `atomic_ref`. Reference-typed atomic slots run the retain/release barrier. `gcCollect` remains a quiescent safepoint and refuses an in-call instance. | `WasmGcHeap.*`; `WasmModuleInstance.{hpp,cpp}`; `wasm_atomic.cpp` |
| MA-4 | No cross-agent wait/notify table. | **Closed:** an address-keyed waiter table with condition variables, real timeouts, no lost wake-up and a no-spurious-wake predicate. | `wasm_atomic.cpp` (`WaiterTable`) |
| MA-5 | Locks are host-side, not a concurrency model. | **Accepted as the model:** the Store/type/string/GC locks protect bookkeeping; guest-visible ordering is the guest's job through the atomic instructions. This is the §3 freedom an engine may take. | `THREADSAFETY.md` §2–§3 |
| MA-6 | Continuations are execution contexts, not agents. | **Out of scope by design:** a continuation runs serially and is not a second mutator; `Agents` is the concurrency model. | `STACK_SWITCHING.md` D-5 |
| MA-7 | Provider / cross-provider gaps. | **Out of scope:** both providers stay serial; the agent model uses host threads, not continuation stacks. | `WasmContinuation.cpp` |
| MA-8 | JS/JSPI is adapter-only. | **Out of scope:** an embedder-supplied event loop remains the JS adapter's concern, not the core's. | `JS_INTEGRATION.md` |
| MA-9 | No multi-agent test or memory model. | **Closed:** `[multiagent]` covers concurrent entry, acquire-release message passing, wait/notify, shared-global RMW, trap isolation, concurrent GC allocation/reclamation and the reference-slot barrier; the guest memory model is the atomic subset documented here. | `Test/ConcurrencyTests.cpp`, `Test/AgentFixture.hpp` |

**Non-blockers that were already done.** Per-agent trap, exception, tail-call,
defer, depth and stack-limit state (`TRAP-6`/SS-6), real seq-cst `__atomic_*`
operations and the per-agent trampoline state. The multi-agent work above built
on them; none needed a representation change.

**Design direction (revision 12).**

- **At most one instance per module.** Multiple instances are ruled out by
  design, so "make a second instance" is never the answer to a concurrency
  problem ([`../THREADSAFETY.md`](../THREADSAFETY.md) §3.1).
- **Many agents on that one instance is the implemented multithreading model**:
  GPU-style worker launches, thread-per-export, async/await via continuations,
  and intra-Wasm threading built on agents the embedder spawns. Concurrent
  *execution* of compiled code and a *re-entrant invoker* are supported;
  concurrent *compilation* is not.
- **Concurrent compilation is rejected as a feature**: a backend's build region
  is a lock, and the only consumer would be the instance multiplicity the first
  rule forbids.
- **`Single` stays the default.** `TD9` remains the shipping contract for an
  embedder that does not opt in; `Agents` is the opt-in model.

### 4.4 Tests

- Manual tests `imported memory is shared between two module instances` and
  `imported table is shared between two module instances` (`Test/main.cpp`,
  `Test/wasm_wat/shared_memory_{a,b}.wat`, `shared_table_{a,b}.wat`) pass.
  Despite the names, these test **cross-module import sharing** (the Store
  ownership model), **not** threads shared memory.
- **The submodule's single-agent atomic script is wired in and curated
  (TH-4).** `extern/WasmTestsuite/proposals/threads/atomic.wast` (142
  `assert_return`s, 28 misaligned `assert_trap`s, one shared memory, no second
  agent) is listed explicitly in `Test/Test.pro`, converts at build time, and
  runs as `spec: atomic` — **249 passed / 0 failed / 48 skipped**; the 48 skips
  are the negative-module families, which are executed only under
  `WASM_SPEC_VALIDATE=1` ([`../VALIDATION.md`](../VALIDATION.md)). It is in
  `Test/wast_supported.txt`, so it runs in the default suite.
- **Multi-agent coverage (TH-5).** `Test/ConcurrencyTests.cpp` (`[multiagent]`,
  6 cases / 51 assertions) runs two host threads with their own `Agent` on one
  instance: acquire-release message passing, a real wait/notify round-trip,
  shared-global atomics, per-agent trap isolation, GC allocation/reclamation
  stress and the reference-slot barrier. `Test/AgentFixture.hpp` is the
  hand-encoded module (acquire-release ordering and `pause` are not yet
  printable by `wasm-tools`).

### 4.5 Cross-cutting: per-agent state and the tail-call trampoline

Two pieces of per-thread state interact with a future multi-agent build; both now
belong to the execution context:

- **Per-agent trap/exception/tail-call state is implemented (TRAP-6).** The
  pending trap slot (`wasm_trap_slot()`), the pending Wasm exception, the
  deferred-cleanup stack, the call/import depth counters and the stack limit live
  in `ExecutionState`; `Agent` owns one, a host thread has a default agent, and
  `AgentScope` installs another agent for a call. The generated-code helpers
  resolve the current state, so generated code did not change —
  [`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §12.1.
- **The trampoline state is per agent.** `ExecutionState::tailCallState` is
  reached through `currentTailCallState()`; generated code does not embed the
  compiling thread's TLS address. Two host threads — and two agents on one host
  thread — therefore do not race. If a future agent migrates between OS threads,
  the state moves with its agent rather than with the thread.
- **A continuation is not an agent.** The stack-switching A provider switches
  native stacks; its continuation is serial and may migrate between host threads,
  but never runs concurrently with its resumer. The B fallback uses a host thread
  per suspended continuation, but `resume` blocks and only one execution context
  mutates the instance at a time. Neither has a scheduler, waiter table or
  multi-mutator GC. TH-5 remains the gated feature for true multi-agent
  execution.

Two further properties of the tail-call lowering:

- **It handles differing signatures.** A `return_call` whose callee's parameter
  list differs from the caller's is dispatched through the callee *type's*
  thunk (`Callable::tailEntry`), so it keeps the space guarantee (TAILCALLS.md
  TC-5). Only a `v128` link (no upstream tail-call script has one) and a
  thunk-less embedder callable fall back to `call` + `return`.
- **Every internal wasm→wasm call now goes through a per-function driver.** Each
  non-tail call costs an indirect call plus one extra native frame, so deeply
  *non-tail*-recursive code reaches stack limits sooner than it did before. Not
  threads-specific, but it changes the stack budget a multi-agent scheduler would
  have to account for.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~T1~~ | ~~**Every atomic instruction aborts.**~~ **Done (TH-1):** all 67 handlers lower to the shared helpers (`WasmBase/wasm_atomic.*`), which perform the access with `__atomic_*` at seq_cst; `spec: atomic` is **249/0/48** and curated. | `LibjitOpcodeDispatcher.cpp` 0xFE block; `wasm_atomic.cpp` | Closed |
| ~~T2~~ | ~~**`atomic.fence` is not lowered.**~~ **Done (TH-1):** `wasm_atomic_fence()` is `__atomic_thread_fence(__ATOMIC_SEQ_CST)`; every atomic access already uses seq_cst, so the fence and the operations share one ordering. | `dispatchAtomicFence`, `wasm_atomic_fence` | Closed |
| ~~T3~~ | ~~**Wait/notify missing.**~~ **Closed (TH-2 → TH-5):** the cross-agent waiter table in `wasm_atomic.cpp` implements the full return-value contract: 0 (woken), 1 (not-equal), 2 (timed out), with a real timeout and no spurious wake. `notify` wakes up to `count` waiters and returns how many it woke. | `dispatchMemoryAtomicNotify/Wait32/Wait64`, `wasm_atomic_wait`, `[multiagent]` | Closed |
| ~~T4~~ | ~~**No runtime alignment check.**~~ **Done (TH-1):** every helper traps with `TrapKind::UnalignedAtomicAccess` ("unaligned atomic") when the *effective* address is not naturally aligned — before a typed pointer is formed. `atomic.wast`'s 28 `assert_trap … "unaligned atomic"` cases pass. The *immediate* (`align == natural`) remains a validation concern (no validator; `TD6`). | `wasm_atomic.cpp` (`requireAlignment`) | Closed (runtime half) |
| T5 | **Wait/notify on unshared memory is accepted.** This is a deliberate single-agent deviation (`TD6`): the curated `atomic.wast` treats both forms on an unshared memory as valid, so the validator does not reject them. | `WasmValidator.cpp` (deliberately no rule); `TD6` | Deliberate (documented) |
| ~~T6~~ | ~~**No maximum requirement.**~~ **Done:** the validator's `checkLimits` rejects a shared memory without a maximum. | `WasmValidator.cpp` (`checkLimits`) | Closed |
| ~~T7~~ | ~~**`memory.size`/`grow` ordering is not published.**~~ **Done (TH-3):** `Store::growMemory` issues a seq-cst release fence after committing pages, and generated `memory.size` calls `WASM::wasm_memory_size`, which performs an acquire load for shared memories (and a plain load for private ones). | `WasmMemory.cpp`, `WasmStore.cpp`, `LibjitOpcodeDispatcher.cpp` | Closed |
| ~~T8~~ | ~~**No data-segment init barrier semantics.**~~ **Done (TH-3):** `applyActiveSegments` issues a seq-cst fence after the active-data loop, before the instance is published or its start function runs. | `applyActiveSegments` | Closed |
| ~~T9~~ | ~~**No agent API / thread pool.**~~ **Closed (TH-5):** `ConcurrencyModel::Agents` + `Runtime::createAgent` + `AgentScope` let an embedder run several host threads on the one instance; the core still owns no thread pool or scheduler (by design). `Single` remains the default and refuses a concurrent entrant. | `WasmConcurrency.hpp`, `WasmRuntime.hpp`, `WasmModuleInstance::enterCall`; §4.3 | Closed |
| ~~T10~~ | ~~**No test coverage is wired in.**~~ **Done (TH-1/TH-4):** `proposals/threads/atomic.wast` is listed explicitly in `Test/Test.pro`, converts at build time, runs as `spec: atomic` (**249 passed / 0 failed / 48 skipped** — the skips are the negative-module families, which run only under `WASM_SPEC_VALIDATE=1`) and is curated in `Test/wast_supported.txt`. | `Test/Test.pro`, `Test/wast_supported.txt` | Closed |
| ~~T11~~ | ~~**The tail-call trampoline's pending state is process-global.**~~ **Done (TRAP-6 / SS-6):** the state is `ExecutionState::tailCallState`, resolved through `currentTailCallState()` for the current agent (a host thread has a default agent). | `WasmBase/WasmAgent.hpp`; §4.5 | Closed: two host threads and two agents on one thread do not race |
| ~~T12~~ | ~~**Trap/exception state is per host thread, not per agent.**~~ **Done (TRAP-6 / SS-6):** `Agent` owns an `ExecutionState` (trap slot, pending exception, deferred cleanup, depth counters, tail-call state and stack limit); a host thread has a default agent and `AgentScope` installs another. | `WasmBase/WasmAgent.hpp`; [`../TRAP_CLEANUP.md`](../TRAP_CLEANUP.md) §12.1 | Closed (state); true multi-agent execution remains TH-5 |

## 5.1 Component-model async, streams, futures and tasks — single-agent backend

The Component Model's asynchronous execution model is folded into this note
rather than duplicated in `COMPONENT_MODEL.md`:

- The **interfaces** are implemented in `WasmBase/WasmComponentAsync.hpp`:
  task/context/backpressure state, waitables and waitable sets, stream/future
  endpoints over a shared element queue, copy results/event codes, and a
  `ComponentExecutor` seam with a `SingleAgentExecutor` implementation
  (`thread.index` 0, `available-parallelism` 1, no OS thread).
- The **single-agent semantics execute** in `WASM::ComponentRuntime`:
  `stream.new`/`future.new`, read/write (including a blocked async read completed
  by a later write), cancel, drop and forward; `task.return`/`task.cancel`;
  `context.get`/`set`; `backpressure.inc`/`dec`; `waitable-set.new/wait/poll/
  drop`, `waitable.join`; `subtask.cancel`/`drop`; `error-context.new/
  debug-message/drop`; and `thread.*` single-agent answers. A synchronous copy
  that would block traps with a named single-agent error rather than hanging.
- **Deferred**: true concurrent copies, a second agent, real wakeups, an
  out-of-line event loop, and the async lift/lower calling convention. These are
  the same staging decision as §3/`TD9`: one agent, synchronous calls, no
  scheduler. The interface is deliberately the seam a future TH-5 backend fills
  in, so no caller changes when that happens.

## 6. Where we can and should play fast and loose

The proposal's freedoms still matter: concurrency is **not required**, so
`Single` remains a conformant engine and the default. `Agents` is the opt-in
addition, and it changed the *execution model* (an entry counter, a waiter table
and a multi-mutator-safe GC path), not the representation of shared memory or
the atomic helpers.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TD1 | **Offer `Single` as the default and `Agents` as an opt-in model.** The embedder selects; the engine validates at `instantiate()`. | §3: no concurrency is mandated; `Single` keeps the trivial seq-cst semantics. | The old contract for embedders that do not opt in, and a supported concurrent model for those that do. |
| TD2 | **Lower atomics to real `__atomic_*` operations at seq_cst** (`wasm_atomic.*`), rather than plain accesses plus fences. | §2.5/§3: only observability matters; seq_cst is the ordering the spec exposes, and it is free on x86. | Correct under one agent *and* ready for a second agent without changing the helper layer. |
| TD3 | **`memory.atomic.notify` returns how many waiters it woke.** | §3: notify may wake fewer than requested; the waiter table counts exactly what it claimed. | A real wake-up instead of a stub. |
| TD4 | **`memory.atomic.wait*`** atomic-loads, returns `1` on a mismatch, otherwise parks on a condition variable for the requested timeout and returns `0` (woken) or `2` (timed out). A negative timeout waits indefinitely. | §3: an implementation that cannot suspend an agent could return `2`, but with the waiter table there is no reason to. | The full observable contract, `[multiagent]` covers the wake-up. |
| TD5 | **`atomic.fence` lowers to `__atomic_thread_fence(seq_cst)`** (or a compiler barrier). | §2.5; a no-op would be wrong the moment two agents exist, and a seq_cst fence is nearly free on x86. | Correct ordering with one instruction. |
| TD6 | **Enforce natural alignment for atomics at run time** (a cheap `addr % size == 0` check → `trap()`); the static memarg rule is not a runtime concern. | §2.3 requires the misaligned *effective address* to trap for atomics; `alignment ≤ natural` is a static property and `Module::validate()` enforces it ([`../VALIDATION.md`](../VALIDATION.md)), so the backend only needs the dynamic half. | Safe on non-x86; one compare on the atomic path only. |
| TD7 | **Do not implement a thread pool or `Atomics.wait` host interop.** The embedder supplies threads; the core supplies agents and the waiter table. Thread-local storage belongs to the shared-everything proposal, which has no stable binary encoding yet. | §3: none of those is required. | The core stays free of a thread pool it would have to defend forever. |
| TD8 | **Keep `SharedLinearMemory`'s reserve/commit base-stability** — it is what makes multi-agent a change of *scheduler* rather than a change of *representation*. | It is implemented and costs nothing when only one agent maps it. | The agent build needed no memory-representation change. |
| TD9 | **`Single` remains the shipping default** — one `ModuleInstance` runs one call at a time; atomics are real `__atomic_*` operations and the waiter table is inert when no second agent exists. `Agents` is the opt-in model added by TH-5 ([`../MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) §6.2). | §3 first bullet: one agent is conformant; every atomic is trivially sequentially consistent. | An embedder that never opts in sees the old behaviour; one that does gets real concurrency. |

**Guardrails.**

- Never let two agents share a `PrivateLinearMemory` (its base can move on
  `grow`); shared memory must use `SharedLinearMemory`.
- `seq_cst` fences around atomics are a hard rule once more than one agent can
  be scheduled — do not optimise them away "because it is single-threaded"
  without also gating on the single-agent precondition.
- Alignment traps and the `wait`-on-unshared trap are observable; implement
  them in both models.
- `notify` must return the number of waiters it actually claimed, and a waiter
  must never return `0` without a notify. The table mutex is the lost-wake-up
  barrier; do not "optimise" the re-read out of `wasm_atomic_wait`.

## 7. Plan

Ordered by value; `TH-n` is independent of the other documents' milestones.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TH-1 — Single-agent atomics** | ✅ Done: the 67 handlers call `wasm_atomic_*` helpers (`WasmBase/wasm_atomic.*`) that perform the access with `__atomic_*` at seq_cst, after the shared `effectiveMemoryAddress` bounds check; natural-alignment traps (`UnalignedAtomicAccess`); `atomic.fence` is `__atomic_thread_fence(seq_cst)`. | T1, T2, T4 | Done: `spec: atomic` **249/0/48**, curated. |
| **TH-2 — Wait/notify** | ✅ Done, including the cross-agent table: `notify` wakes up to `count` waiters and returns the number woken; `wait32/64` returns 0 (woken), 1 (not-equal) or 2 (timed out), with a real (possibly infinite) timeout and no spurious wake. | T3, T4 | Done: `atomic.wast` passes, and `[multiagent]` proves a real wake-up without a value change. |
| **TH-3 — Shared-memory hygiene** | ✅ `SharedLinearMemory` never moves its base; import matching compares shared-ness; shared memories/tables must declare a maximum; `memory.grow` publishes with a seq-cst fence; `memory.size` acquire-loads shared metadata; active data-segment initialisation ends with a seq-cst barrier; the tail-call state is per-agent. | T6, T7, T8, T11 | Done: shared memory without a max is rejected; `memory.grow`/`size` are ordered; host threads do not share tail-call state. |
| **TH-4 — Threads test coverage** | ✅ Done: `proposals/threads/atomic.wast` is wired into `Test/Test.pro` (single-agent atomic semantics, including the 28 misaligned `assert_trap`s) and curated. | T10 | Done: `spec: atomic` runs in the default suite (`Test/wast_supported.txt`). |
| **TH-5 — Real agents** | ✅ **Done 2026-10-29:** `ConcurrencyModel::Agents` (`WasmConcurrency.hpp`) + `Runtime::createAgent`/`AgentScope` + an atomic entry counter (`Single` refuses a second entrant, `Agents` admits it), a guest-address-keyed waiter table, per-agent trap/tail-call state, atomic GC counters and segment flags, a mutex-guarded reclaim queue/heap, and the reference-slot barrier. The core owns no scheduler; the embedder spawns/joins host threads. **At most one instance per module**; concurrent compilation stays rejected. | T9, T12 | Two agents produce the same results as the sequentially-consistent model, each sees only its own traps, and `[multiagent]` is green (6 cases / 51 assertions). |
| **TH-6 — Component async interface** | ✅ `WasmComponentAsync.hpp` defines tasks, waitables, stream/future endpoints and the `ComponentExecutor` seam; `SingleAgentExecutor` and `ComponentRuntime` execute the synchronous subset. | T9/T11 (interfaces), TH-5 (backend) | All single-agent async tests pass; replacing the executor is the only change a multi-agent backend needs. |

Guardrails: run `./WasmJit '~[spec]'` after each step; keep `Single` the
default so existing behaviour is unchanged. `Agents` is additive: the entry
counter is the only per-call cost `Single` pays, and its refusal path is tested.
The target remains **one `ModuleInstance` per `Module`**, so a second agent is a
second execution context, never a second instance; the guest lock/waitqueue
standard is "use the atomic instructions", and the embedder is responsible for
joining its threads before tearing down the runtime
([`../THREADSAFETY.md`](../THREADSAFETY.md) §9).

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-29):

| Test | Result | Reading |
|---|---|---|
| `spec: atomic` | **249 passed / 0 failed / 48 skipped** | all 67 atomic handlers execute (loads, stores, RMW/xchg/cmpxchg, fence), natural-alignment traps and the rewritten wait/notify; the skips are the negative-module families, which run only under `WASM_SPEC_VALIDATE=1` ([`../VALIDATION.md`](../VALIDATION.md)). Curated |
| `[multiagent]` | **51 assertions / 6 cases** | concurrent entry, acquire-release message passing, a real wait/notify wake, shared-global RMW, per-agent trap isolation, concurrent GC allocation and the reference-slot barrier |
| `imported memory is shared between two module instances` | pass (8 assertions) | cross-module *import* sharing, not threads |
| `imported table is shared between two module instances` | pass (15 assertions) | cross-module *import* sharing, not threads |
| default suite | 11,454 assertions / 481 cases, green | no regression from the multi-agent work |

The upstream script remains the single-agent regression signal for the whole
atomic opcode surface; `[multiagent]` is the second-agent signal (`TH-5`).

## 9. References

Threads proposal:

- Overview (`WebAssembly/threads`, `proposals/threads/Overview.md`) — agents
  and clusters, shared linear memory, data-init rules, atomic accesses, wait /
  notify, fence:
  https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md
- Mutable globals (split out, now core):
  https://github.com/WebAssembly/mutable-global
- Sign-extension ops (split out, now core):
  https://github.com/WebAssembly/sign-extension-ops

WebAssembly 3.0 spec (for contrast — threads is *not* in it):

- Change History (Release 2.0 / 3.0 feature lists; no thread/atomic entry):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Instruction index (no `atomic.*` entries):
  https://webassembly.github.io/spec/core/syntax/instructions.html

In-repo:

- [`docs/GC.md`](GC.md) — the companion design note and the format this one
  follows; §6.1 "hunks" also applies to the runtime data model.
- [`ACQUIRE_RELEASE_ATOMICS.md`](ACQUIRE_RELEASE_ATOMICS.md) — the weaker
  ordering/`pause` extension to the baseline atomics.
- [`SHARED_EVERYTHING_THREADS.md`](SHARED_EVERYTHING_THREADS.md) — the shared
  GC/tables/TLS/waitqueue follow-on, and its multi-agent prerequisites.
- [`docs/ABI.md`](../ABI.md) — `VMContext`, Store ownership, and the single
  process-global `Store`.
- [`docs/THREADSAFETY.md`](../THREADSAFETY.md) — guest/host responsibility split
  for exports, imports and host threads.
- Source anchors: `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmStore.{hpp,cpp}`,
  `WasmBase/WasmType.hpp` (`MemoryType`, `Limits`),
  `WasmBase/WasmModuleInstance.cpp` (`initializeMemories`,
  `applyActiveSegments`), `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixAtomic`),
  `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}` (0xFE stub block),
  `WasmStub/StubOpcodeDispatcher.cpp` (pretty-printers),
  `Test/main.cpp` (shared-memory / shared-table tests),
  `Test/wasm_wat/shared_{memory,table}_{a,b}.wat`.
