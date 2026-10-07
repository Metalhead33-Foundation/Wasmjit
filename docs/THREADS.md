# Threads and Atomics — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmMemory`, `WasmStore`, `WasmOpcode`,
`WasmOpcodeDispatcher`, `LibjitOpcodeDispatcher`, or the test harness.

This note follows the same split as [`GC.md`](GC.md) and
[`TYPE_IDENTITY.md`](TYPE_IDENTITY.md):

1. what the threads proposal **demands**;
2. what it explicitly does **not** demand (and where an engine is free);
3. what `WasmJit` **currently implements**;
4. the **gaps** between (3) and (1);
5. where we can and should play **fast and loose** on purpose.

It contains **no new code**.

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
  correctly: a reserved `0x00` byte for `atomic.fence` (checked, throws if not
  zero) and a `MemArg` for everything else, then dispatches to virtuals.
- **All 67 `LibJIT::OpcodeDispatcher` handlers are stubs** that call
  `notImplemented(__func__)`, which prints
  `LibJIT: unimplemented opcode handler <name>` to `stderr` and calls
  `std::abort()`. A module containing any atomic instruction therefore aborts
  during compilation/instantiation.
- The non-JIT `Stub::OpcodeDispatcher` implements the same handlers as textual
  pretty-printers (useful for inspection, no execution).

### 4.3 What is absent

- **No execution of any atomic instruction**, hence no load/store/RMW/cmpxchg,
  no wait/notify, no fence lowering.
- **No agent/thread model**: one `ModuleInstance` executes on the caller's
  thread, the `Store` is a single process-global, and there is no API to create
  an agent or run instances concurrently.
- The shared-memory **allocation/growth** machinery exists, but nothing
  consumes it atomically.

### 4.4 Tests

- Manual tests `imported memory is shared between two module instances` and
  `imported table is shared between two module instances` (`Test/main.cpp`,
  `Test/wasm_wat/shared_memory_{a,b}.wat`, `shared_table_{a,b}.wat`) pass.
  Despite the names, these test **cross-module import sharing** (the Store
  ownership model), **not** threads shared memory.
- **`extern/WasmTestsuite` contains no atomic/thread/shared script** (0 of the
  257 `*.wast` files match `atomic|thread|shared|wait|notify`), and none are in
  `Test/wast_supported.txt`. There is currently **no spec coverage** for this
  feature area, so there is also no regression signal to protect.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| T1 | **Every atomic instruction aborts.** 67 dispatch stubs call `notImplemented` → `std::abort`. | `LibjitOpcodeDispatcher.cpp` 0xFE block; any `atomic.*` module crashes at instantiation. | High |
| T2 | **`atomic.fence` is not lowered.** No compiler or hardware fence is emitted, so even a future single-threaded atomic implementation would need this before cross-agent use. | `dispatchAtomicFence` | Medium |
| T3 | **Wait/notify missing**, so no blocking primitive: `memory.atomic.wait*` and `notify` abort. | `dispatchMemoryAtomicWait32/64`, `dispatchMemoryAtomicNotify` | High |
| T4 | **No runtime alignment check.** The proposal requires misaligned atomic accesses to trap and the memarg alignment to be exactly natural; nothing enforces either. | no validation pass; `dispatchPrefixAtomic` only decodes `MemArg` | Medium |
| T5 | **No shared-ness validation.** The `shared` flag drives allocation, but nothing rejects `wait` on an unshared memory, or an import whose shared-ness disagrees with the declaration. | `resolveImports` checks only memories' existence; `dispatchMemoryAtomicWait*` unimplemented | Medium |
| T6 | **No maximum requirement.** The proposal makes a maximum mandatory for shared memory; the code accepts a shared memory without one (it caps at 4 GiB internally instead). | `initializeMemories`, `SharedLinearMemory` | Low |
| T7 | **`memory.size`/`grow` ordering is not published.** Shared `grow` takes a mutex, but `memory.size` is a plain load and no `seq_cst` fence is emitted around either, so the sequentially-consistent guarantee is not established for a future multi-agent build. | `dispatchMemorySize`, `SharedLinearMemory::growMemory` | Low (invisible while single-agent) |
| T8 | **No data-segment init barrier semantics.** `applyActiveSegments` copies segment bytes directly; there is no fence/barrier before other agents are allowed to observe the memory. | `applyActiveSegments` | Low (single-agent) |
| T9 | **No agent API / thread pool.** There is no way to run two instances concurrently, which is also what makes the current single-threaded stance safe. | no such API in `ModuleInstance`/`Store` | Informational |
| T10 | **Zero test coverage.** No spec scripts exist in the submodule, and no manual threads test exercises atomics. | §4.4 | High (for future work) |

## 6. Where we can and should play fast and loose

The single most important lever is §3's first bullet: **concurrency is not
required**. A QVM-lineage engine is single-threaded by nature, and that turns
almost all of the threads proposal into trivial semantics.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TD1 | **Keep the one-agent execution model.** Accept shared memories and atomics, but never run two agents at once. | §3: no concurrency is mandated; with one agent every atomic is trivially sequentially consistent. | The whole proposal collapses to ordinary memory accesses plus a few returns. |
| TD2 | **Lower atomic load/store/RMW/cmpxchg to plain accesses**, wrapped in a `std::atomic_thread_fence(seq_cst)` before and after (or `__atomic_*` ops if multi-agent is ever enabled). | Matches observable behaviour for a single agent; fences are cheap and keep the code future-proof. | No locking, no libatomic dependency, correct single-agent semantics. |
| TD3 | **`memory.atomic.notify` returns `0`.** | §3: notify may wake fewer than requested; no waiter can exist without a second agent. | A one-line implementation instead of a waiter table. |
| TD4 | **`memory.atomic.wait*`**: atomic-load; if `!= expected` return `1`; else if `timeout < 0` block (legitimately, since a single-agent program that waits forever for a notify is deadlocked); else sleep the requested duration and return `2`. | §3: no spurious wakeups, and `2` is the correct result when the timeout expires; no other agent can notify. | Real (if boring) blocking semantics with no scheduler. |
| TD5 | **`atomic.fence` lowers to `__atomic_thread_fence(seq_cst)`** (or a compiler barrier). | §2.5; a no-op would be wrong the moment two agents exist, and a seq_cst fence is nearly free on x86. | Correct ordering with one instruction. |
| TD6 | **Enforce natural alignment only for atomics** (a cheap `addr % size == 0` check → `trap()`), and skip the memarg-alignment validation entirely. | §2.3 requires the misaligned *effective address* to trap; the memarg check is a validation concern we already skip globally (no validating front-end). | Safe on non-x86; no validation engine. |
| TD7 | **Do not implement a thread pool, agents, `Atomics.wait` host interop, or TLS.** | §3: none is required. | Scope control; the Store/instance design already models a single process-global. |
| TD8 | **Keep `SharedLinearMemory`'s reserve/commit base-stability** even in the single-agent build — it is what makes a later multi-agent step a change of *scheduler* rather than a change of *representation*. | It is already implemented and costs nothing when only one agent maps it. | A clean upgrade path. |

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
| **TH-1 — Single-agent atomics** | Implement atomic load/store/RMW/xchg/cmpxchg via native helpers (or inline `__atomic_*`), with natural-alignment traps and an `atomic.fence` that lowers to a seq_cst fence. | T1, T2, T4 | A hand-written module with `i32.atomic.rmw.add` and `atomic.fence` runs and returns correct values. |
| **TH-2 — Wait/notify** | `memory.atomic.wait32/64` (load→compare→sleep/block→`1`/`2`) and `memory.atomic.notify` (`0`), including the unshared-memory trap. | T3, T5 | A module using `wait`/`notify` terminates; `wait` returns `1` on mismatch and `2` after a finite timeout. |
| **TH-3 — Shared-memory hygiene** | Require a maximum for `shared` memories; assert shared-ness on import matching; emit a barrier after active data segments. | T6, T7, T8 | A shared memory without a max is rejected; `memory.grow`/`size` are fenced. |
| **TH-4 — Threads test coverage** | Add manual WAT cases for each atomic family and wait/notify; add upstream `atomic.wast` / `threads.wast` to the testsuite if/when the submodule provides them. | T10 | At least one test per instruction family, plus a curated-list entry. |
| **TH-5 — Optional: real agents** | Embedder API to run an instance on another OS thread, with `__atomic_*` accesses and a real waiter table. Only if an embedder needs it. | T9 | Two agents produce the same results as the sequentially-consistent model. |

Guardrails: run `./WasmJit '~[spec]'` after each step; keep the single-agent
default so existing behaviour is unchanged.

## 8. Indicative test survey

Survey taken with `build/Desktop-Debug/Test/WasmJit` (2026-10-04).

| Test | Result | Reading |
|---|---|---|
| `spec: atomic` / `spec: atomic_fence` | **no test case** | the testsuite submodule has no atomic/threads scripts |
| `imported memory is shared between two module instances` | pass (8 assertions) | cross-module *import* sharing, not threads |
| `imported table is shared between two module instances` | pass (15 assertions) | cross-module *import* sharing, not threads |
| any module containing `atomic.*` | abort | T1: `notImplemented` → `std::abort` |

Because there is no spec coverage, the practical gate for TH-1/TH-2 is new
manual WAT tests (TH-4), not the upstream suite.

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
- [`docs/ABI.md`](ABI.md) — `VMContext`, Store ownership, and the single
  process-global `Store`.
- Source anchors: `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmStore.{hpp,cpp}`,
  `WasmBase/WasmType.hpp` (`MemoryType`, `Limits`),
  `WasmBase/WasmModuleInstance.cpp` (`initializeMemories`,
  `applyActiveSegments`), `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixAtomic`),
  `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}` (0xFE stub block),
  `WasmStub/StubOpcodeDispatcher.cpp` (pretty-printers),
  `Test/main.cpp` (shared-memory / shared-table tests),
  `Test/wasm_wat/shared_{memory,table}_{a,b}.wat`.
