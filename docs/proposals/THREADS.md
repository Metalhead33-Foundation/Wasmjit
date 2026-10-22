# Threads and Atomics — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 6 — TH-1, TH-2, TH-3,
TH-4 and TH-6 are implemented; TH-5 (real agents) is deferred indefinitely but
not ruled out. TH-3 adds size/grow and data-init fences, and makes the tail-call
state host-thread-local. §3 records the project's explicit execution model:
**single-agent only, for the foreseeable future**; the single-agent build is a
long-term stop-gap, not an oversight.)
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

  **Project decision (revision 3).** This engine deliberately targets
  **single-agent execution only**. True multithreading — agents, OS threads, a
  scheduler, TLS, cross-agent `wait`/`notify` — is **deferred indefinitely, but
  not ruled out**: every piece of the representation (stable shared-memory
  bases, `__atomic_*` accesses, per-thread trap state) is kept in a shape that a
  later multi-agent build can use. The single-agent model is a **long-term
  stop-gap**, chosen because it keeps the runtime simple and the spec suite
  meaningful; it is not a claim that the engine must never be concurrent. See
  `TD9` in §6 and the deferred `TH-5` in §7.
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
  are the single-agent forms (TH-2, `TD9`).
- The non-JIT `Stub::OpcodeDispatcher` implements the same handlers as textual
  pretty-printers (useful for inspection, no execution).

### 4.3 What is absent

- **No agent/thread model — by decision, not by omission.** One
  `ModuleInstance` executes on the caller's thread, the `Store` is a single
  process-global, and there is no API to create an agent or run instances
  concurrently. True multithreading is **deferred indefinitely but not ruled
  out** (§3, `TD9`, TH-5): the stable shared-memory base, the real `__atomic_*`
  helpers, the host-thread-local trap state and now the host-thread-local
  tail-call state are all shaped so that a future agent build changes the
  *scheduler*, not the representation.
- Host-side concurrency hardening (SET-2, done without wasm agents): the Store's
  containers are mutex-guarded, type interning is serialized, and the GC heap and
  string store take recursive locks. A single instance is still not concurrently
  callable — that is the deferred agent work. The target runtime is one
  `ModuleInstance` per `Module` (one `VMContext`), so future concurrency must make
  that single instance safe rather than multiplying instances.
- There is no cross-agent waiter table, so `wait` cannot be woken (`notify`
  returns 0) and the timeout is not slept (`TD4`).

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
  are `assert_invalid` module-validation cases (no validating front end). It is
  in `Test/wast_supported.txt`, so it runs in the default suite.

### 4.5 Cross-cutting: the tail-call trampoline's known limits

The tail-call lowering (`docs/TAILCALLS.md`, TC-2) added host-thread-local state and
a per-function call wrapper. Both interact with a future multi-agent build, so
they are recorded here:

- **The trampoline state is host-thread-local.** `WASM::g_tailCallState` is
  `thread_local`; generated code calls `WASM::currentTailCallState()` to obtain
  the executing thread's address rather than embedding the compiling thread's
  TLS address. Two host threads entering the JIT therefore do not race. A
  future multi-agent build must still decide whether one agent may migrate
  between OS threads; if so it needs an explicit per-agent state object.
- **It only engages for identical signatures.** A `return_call` whose callee's
  parameter/result types differ from the caller's falls back to `call` + `return`
  and does grow the host stack. (Direct self-recursion is lowered to a native
  `JIT_CALL_TAIL` and is unaffected.)
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
| ~~T3~~ | ~~**Wait/notify missing.**~~ **Done, single-agent form (TH-2):** `notify` returns 0 (no agent can be waiting); `wait32/64` returns 1 on a value mismatch and 2 (timeout) otherwise, with the timeout not slept — legal per §3 and documented as `TD9`. The three wait/notify assertions in `atomic.wast` pass. | `dispatchMemoryAtomicNotify/Wait32/Wait64`, `wasm_atomic_wait` | Closed (multi-agent wake-up deferred with TH-5) |
| ~~T4~~ | ~~**No runtime alignment check.**~~ **Done (TH-1):** every helper traps with `TrapKind::UnalignedAtomicAccess` ("unaligned atomic") when the *effective* address is not naturally aligned — before a typed pointer is formed. `atomic.wast`'s 28 `assert_trap … "unaligned atomic"` cases pass. The *immediate* (`align == natural`) remains a validation concern (no validator; `TD6`). | `wasm_atomic.cpp` (`requireAlignment`) | Closed (runtime half) |
| T5 | **Wait/notify on unshared memory is accepted.** This is a deliberate single-agent deviation (`TD6`): the curated `atomic.wast` treats both forms on an unshared memory as valid, so the validator does not reject them. | `WasmValidator.cpp` (deliberately no rule); `TD6` | Deliberate (documented) |
| ~~T6~~ | ~~**No maximum requirement.**~~ **Done:** the validator's `checkLimits` rejects a shared memory without a maximum. | `WasmValidator.cpp` (`checkLimits`) | Closed |
| ~~T7~~ | ~~**`memory.size`/`grow` ordering is not published.**~~ **Done (TH-3):** `Store::growMemory` issues a seq-cst release fence after committing pages, and generated `memory.size` calls `WASM::wasm_memory_size`, which performs an acquire load for shared memories (and a plain load for private ones). | `WasmMemory.cpp`, `WasmStore.cpp`, `LibjitOpcodeDispatcher.cpp` | Closed |
| ~~T8~~ | ~~**No data-segment init barrier semantics.**~~ **Done (TH-3):** `applyActiveSegments` issues a seq-cst fence after the active-data loop, before the instance is published or its start function runs. | `applyActiveSegments` | Closed |
| T9 | **No agent API / thread pool — deferred indefinitely, not ruled out (`TD9`).** There is no way to run several agents concurrently on the single module instance, which is also what makes the current single-threaded stance safe. | no such API in `ModuleInstance`/`Store`; §3 project decision | Deferred by decision |
| ~~T10~~ | ~~**No test coverage is wired in.**~~ **Done (TH-1/TH-4):** `proposals/threads/atomic.wast` is listed explicitly in `Test/Test.pro`, converts at build time, runs as `spec: atomic` (**249 passed / 0 failed / 48 skipped** — the skips are `assert_invalid` module-validation cases) and is curated in `Test/wast_supported.txt`. | `Test/Test.pro`, `Test/wast_supported.txt` | Closed |
| ~~T11~~ | ~~**The tail-call trampoline's pending state is process-global.**~~ **Done (TH-3/SET-2):** `g_tailCallState` is `thread_local`, and generated code reaches it through `currentTailCallState()` so the address is resolved on the executing host thread. | `WASM::g_tailCallState` (`WasmBase/WasmValue.hpp`); §4.5 | Closed for host threads; agent migration is still a TH-5 design question |

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

The single most important lever is §3's first bullet: **concurrency is not
required**. A QVM-lineage engine is single-threaded by nature, and that turns
almost all of the threads proposal into trivial semantics.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TD1 | **Keep the one-agent execution model.** Accept shared memories and atomics, but never run two agents at once. | §3: no concurrency is mandated; with one agent every atomic is trivially sequentially consistent. | The whole proposal collapses to ordinary memory accesses plus a few returns. |
| TD2 | **Lower atomics to real `__atomic_*` operations at seq_cst** (`wasm_atomic.*`), rather than plain accesses plus fences. | §2.5/§3: only observability matters; seq_cst is the ordering the spec exposes, and it is free on x86. | Correct under one agent *and* ready for a second agent without changing the helper layer. |
| TD3 | **`memory.atomic.notify` returns `0`.** | §3: notify may wake fewer than requested; no waiter can exist without a second agent. | A one-line implementation instead of a waiter table. |
| TD4 | **`memory.atomic.wait*`**: atomic-load; if `!= expected` return `1`; otherwise return `2` **without sleeping**. | §3: an implementation that cannot suspend an agent may return `2` for a finite timeout; with no second agent nothing can notify, so the value can only time out. The one documented cost: an *infinite* wait also returns `2` instead of blocking forever (`TD9`). | Deterministic tests, no scheduler, no sleeping daemon. |
| TD5 | **`atomic.fence` lowers to `__atomic_thread_fence(seq_cst)`** (or a compiler barrier). | §2.5; a no-op would be wrong the moment two agents exist, and a seq_cst fence is nearly free on x86. | Correct ordering with one instruction. |
| TD6 | **Enforce natural alignment only for atomics** (a cheap `addr % size == 0` check → `trap()`), and skip the memarg-alignment validation entirely. | §2.3 requires the misaligned *effective address* to trap; the memarg check is a validation concern we already skip globally (no validating front-end). | Safe on non-x86; no validation engine. |
| TD7 | **Do not implement a thread pool, agents, `Atomics.wait` host interop, or TLS.** | §3: none is required. | Scope control; the Store/instance design already models a single process-global. |
| TD8 | **Keep `SharedLinearMemory`'s reserve/commit base-stability** even in the single-agent build — it is what makes a later multi-agent step a change of *scheduler* rather than a change of *representation*. | It is already implemented and costs nothing when only one agent maps it. | A clean upgrade path. |
| TD9 | **Ship a single-agent engine only** — one `ModuleInstance` runs on the caller's thread; atomics are real `__atomic_*` operations (so they are seq-cst even here), but there is no agent API and no scheduler. True multithreading is **deferred indefinitely, not abandoned**: the memory representation, per-thread trap/tail-call state and atomic helpers are kept in a shape a future agent model can reuse. | §3 first bullet: one agent is conformant; every atomic is trivially sequentially consistent. | No thread pool, TLS, scheduling, cross-agent waiter table or data-race tooling — the runtime stays simple and the spec suite stays deterministic. The cost is documented: `wait` cannot actually be woken by a second agent. |

**Guardrails.**

- Never let two agents share a `PrivateLinearMemory` (its base can move on
  `grow`); shared memory must use `SharedLinearMemory`.
- `seq_cst` fences around atomics are a hard rule once more than one agent can
  be scheduled — do not optimise them away "because it is single-threaded"
  without also gating on the single-agent precondition.
- Alignment traps and the `wait`-on-unshared trap are observable; implement
  them even in the degenerate build.
- `notify` returning `0` is only correct while no host-created waiter exists;
  that is part of the documented no-second-agent contract.

## 7. Plan

Ordered by value; `TH-n` is independent of the other documents' milestones.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TH-1 — Single-agent atomics** | ✅ Done: the 67 handlers call `wasm_atomic_*` helpers (`WasmBase/wasm_atomic.*`) that perform the access with `__atomic_*` at seq_cst, after the shared `effectiveMemoryAddress` bounds check; natural-alignment traps (`UnalignedAtomicAccess`); `atomic.fence` is `__atomic_thread_fence(seq_cst)`. | T1, T2, T4 | Done: `spec: atomic` **249/0/48**, curated. |
| **TH-2 — Wait/notify** | ✅ Done in the single-agent form: `notify` returns 0; `wait32/64` returns 1 on a value mismatch and 2 otherwise without sleeping (`TD4`). The unshared-memory trap is a validation concern (`T5`, no front end). | T3 | Done: the script's wait/notify assertions pass (mismatch → 1, timeout → 2, notify → 0). |
| **TH-3 — Shared-memory hygiene** | ✅ `SharedLinearMemory` never moves its base; import matching compares shared-ness; shared memories/tables must declare a maximum; `memory.grow` publishes with a seq-cst fence; `memory.size` acquire-loads shared metadata; active data-segment initialisation ends with a seq-cst barrier; the tail-call state is host-thread-local. | T6, T7, T8, T11 | Done: shared memory without a max is rejected; `memory.grow`/`size` are ordered; host threads do not share tail-call state. |
| **TH-4 — Threads test coverage** | ✅ Done: `proposals/threads/atomic.wast` is wired into `Test/Test.pro` (single-agent atomic semantics, including the 28 misaligned `assert_trap`s) and curated. | T10 | Done: `spec: atomic` runs in the default suite (`Test/wast_supported.txt`). |
| **TH-5 — Optional: real agents** | ✗ **Deferred indefinitely (not ruled out):** an embedder API to run agents on the one module instance across OS threads, with a real waiter table and per-agent trap/lock ownership. Only if an embedder needs it. Until then the engine is explicitly single-agent (`TD9`). | T9 | Two agents produce the same results as the sequentially-consistent model. |
| **TH-6 — Component async interface** | ✅ `WasmComponentAsync.hpp` defines tasks, waitables, stream/future endpoints and the `ComponentExecutor` seam; `SingleAgentExecutor` and `ComponentRuntime` execute the synchronous subset. | T9/T11 (interfaces), TH-5 (backend) | All single-agent async tests pass; replacing the executor is the only change a multi-agent backend needs. |

Guardrails: run `./WasmJit '~[spec]'` after each step; keep the single-agent
default so existing behaviour is unchanged. The single-agent choice is
*documented and deliberate* (`TD9`) — do not add a partial thread model without
first making the tail-call trampoline per-agent (`T11`). TH-5 is not just a
scheduler: the target is **one `ModuleInstance` per `Module`**, so it must also
settle per-agent trap/lock ownership and a guest lock/waitqueue standard before
claiming concurrent G-C/G-D safety — see
[`../THREADSAFETY.md`](../THREADSAFETY.md) §4.6, §5.6 and §10.1.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17, after TH-1/TH-2/TH-4):

| Test | Result | Reading |
|---|---|---|
| `spec: atomic` | **249 passed / 0 failed / 48 skipped** | all 67 atomic handlers execute (loads, stores, RMW/xchg/cmpxchg, fence), natural-alignment traps, and the single-agent wait/notify triple; the skips are `assert_invalid` cases (no validating front end). Curated |
| `imported memory is shared between two module instances` | pass (8 assertions) | cross-module *import* sharing, not threads |
| `imported table is shared between two module instances` | pass (15 assertions) | cross-module *import* sharing, not threads |
| any module containing `atomic.*` | executes | TH-1: the 67 `notImplemented` stubs are gone |

The upstream script is now the regression signal for the whole atomic opcode
surface; no second-agent test can exist while the engine is single-agent
(`TD9`, TH-5).

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
