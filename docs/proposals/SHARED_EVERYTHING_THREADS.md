# Shared-Everything Threads — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — **SET-1 partial,
SET-2 host-side, and SET-3 implemented**; SET-4/SET-5 absent. The engine has
`shared` table/global/segment annotations, shared `comptype`/heap-type encoding,
and the working draft's `0xFE 0x4F–0x71` global/table/struct/array atomics plus
`ref.i31_shared`. Wasm agents remain deferred (`THREADS.md` TH-5); the runtime
is made safe for host-side multithreading, not for concurrent mutators.)

Audience: anyone touching `WasmType` (`shared`/`globaltype`), `WasmStore`,
`WasmModuleInstance`, the GC heap/refcount, `WasmOpcode` (`0xFE` atomics),
`WasmTrap`, or a future multi-agent runtime.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.
The guest/host boundary contract is [`THREADSAFETY.md`](../THREADSAFETY.md).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does Shared-Everything Threads require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

Shared-Everything Threads is a broad, actively changing proposal. It extends
the baseline threads model from shared linear memory to shareable Wasm objects
and adds the runtime pieces needed by languages with threads:

- **`shared` annotations** on functions, tables, globals, memories, tags and
  GC types. A shared item may refer only to shared items; non-shared items may
  refer to shared items. Validation enforces the relation.
- **Shared GC data.** Atomic accesses to struct/array fields, globals and table
  slots, with sequential-consistency and acquire-release orderings. The working
  draft uses new `0xFE` sub-opcodes plus `ref.i31_shared`, and restricts
  reference atomics to `anyref`/`shared anyref` and equality-comparable forms.
- **Thread-local storage.** `thread_local` globals with per-thread values, so a
  module can have TLS without an instance-per-thread model.
- **Managed waitqueues.** `waitqueue`/`(shared waitqueue)` types,
  `waitqueue.new`/`notify`, and `struct/array/global/table/memory.wait`
  operations; waits return notified / not-equal / timed-out status.
- **Thread lifecycle.** Component-model builtins for spawning/joining threads
  and related lifecycle operations.

The proposal is explicitly a catch-all under active development. Its binary
form, instruction set and component builtins are not a stable target.

## 3. What the proposal does not demand

- No requirement to implement every piece; the proposal is an umbrella and
  explicitly warns that it may change.
- No final binary encoding in the fetched overview; opcode assignments and
  `shared` productions are still moving.
- No requirement for a JS host: the thread lifecycle is intended to work in
  non-JS embeddings too.
- No shared-to-unshared references by default; allowing them is an open design
  question with weak/strong/GC-cycle trade-offs.
- No requirement to change the baseline threads semantics; existing shared
  memories and seqcst atomics remain valid.
- No requirement to support waiting on a main/browser thread; those waits may
  trap even if they would not block.
- No required GC strategy for shared heaps; the spec does not mandate a
  collector or cross-heap cycle collection.

## 4. What we have implemented today

- **Baseline threads plus SET-3 atomics.** `WasmBase/WasmOpcode.hpp` has all 67
  baseline `0xFE` atomics and the 35 working-draft shared-everything opcodes
  `0x4F–0x71` (`global.atomic.*`, `table.atomic.*`, `struct.atomic.*`,
  `array.atomic.*`), plus `ref.i31_shared` (`0xFB 0x1F`). The shared forms take
  a `u8` ordering immediate (0 seqcst, 1 acqrel) and the named index/type
  immediates; they lower through the same alignment-checked `wasm_atomic_*`
  helpers as linear-memory atomics.
- **Shared annotations (SET-1 partial).** `TableType::isShared` comes from
  limits bit 1, `GlobalType::isShared` from the `mut` flag bit 1, and
  element/data segments use bit 3. `Subtype`/`CanonicalType` carry `isShared`
  from the `0x65` `sharecomptype` prefix; `ValueType::isShared` decodes the
  `0x65` shared-heap-type prefix and shared refs are compared as a distinct
  hierarchy. Shared tables/memories must declare a maximum, and global/table
  import matching compares sharedness.
- **Host-side hardening (SET-2 partial).** `Store` containers, type interning,
  the GC heap and the string store are mutex-guarded; trap, exception and
  tail-call state belong to the per-agent `ExecutionState` (TRAP-6 is
  implemented, `WasmBase/WasmAgent.hpp`). A single `ModuleInstance` is still not
  concurrently callable — the real multi-agent scheduler and shared GC heap are
  deferred with SET-4/SET-5 and `THREADS.md` TH-5.
- **No waitqueue/TLS/lifecycle.** `waitqueue` (`0x68`) collides with the
  stack-switching continuation heap type in this engine's current encoding
  space, so it is not decoded; `thread_local` globals and component thread
  spawn/join are absent.
- **Tests.** Raw-binary fixtures cover a shared global with
  `global.atomic.rmw.add`/`global.atomic.get`, a shared table with
  `table.atomic.set`/`get`, `sharecomptype`, and `(ref null (shared any))`.
  The 35 opcodes decode and execute in the single-agent model.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| SET1 | **Shared annotations are not fully validated.** Table/global/segment/shared-comptype/shared-ref flags decode, but shared-to-shared restrictions on every referenced item (and shared tags) are not enforced. | `WasmValidator.cpp`; `WasmType.hpp`. | Medium |
| SET2 | **No shared GC heap / multi-mutator collection.** Atomically accessed numeric fields are tear-free; reference-slot exchanges are pointer-atomic but do not yet run the GC retain/release write barrier, so reference-typed atomic fields are not advertised as leak-safe. The heap remains single-mutator. | `WasmGcHeap.*`; `LibjitOpcodeDispatcher.cpp` (`*Atomic*`); `THREADS.md` TH-5. | High (deferred) |
| SET3 | **No managed waitqueue.** `0x68` is currently `AbstractHeapType::Cont`; the working draft's `waitqueue` type needs a collision-free assignment. | `WasmType.hpp`; `THREADS.md`. | High |
| SET4 | **No thread-local globals.** `GlobalType` has `isShared` but no `thread_local` storage. | `WasmType.hpp`; `WasmModuleInstance` globals. | High |
| SET5 | **No thread lifecycle/component builtins.** No spawn/join or component integration. | `WasmComponentModel.hpp`; `COMPONENT_MODEL.md`. | High |
| SET6 | **Runtime is not multi-mutator.** Host-thread-safe, not agent-safe: one instance cannot be entered concurrently and the GC heap has one mutator. Trap/exception/tail-call state is per agent (`Agent`/`ExecutionState`, TRAP-6 done); what remains is the multi-mutator execution model, deferred with TH-5. | `WasmStore.hpp`; `WasmGcHeap.*`; `WasmAgent.hpp`. | High (deferred with TH-5) |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SETD1 | **Implement only the stable-enough wire subset; reject unknown reserved encodings clearly.** The current `0x4F–0x71` atomics, annotation bits and `0x65` prefixes are decoded; `waitqueue`/TLS remain rejected or absent. | §3: it is an active umbrella; an engine may reject unsupported features. | Useful ABI surface without guessing the moving parts. |
| SETD2 | **Keep the single-agent execution model as a stop-gap.** | §3: baseline threads and shared memory already work; agents are deferred by `THREADS.md` TD9. | No premature scheduler/GC redesign. |
| SETD3 | **Do not allow shared-to-unshared references.** | §3: that is the conservative reading and the proposal's default validation rule. | No weak-reference or cross-heap-cycle semantics to guess. |
| SETD4 | **Do not promise cross-heap cycle collection.** | §3: the spec does not mandate a collector or cross-heap cycle collection. | The existing single-heap RC collector remains honest. |
| SETD5 | **Reject waits on an unsupported/main thread rather than block it.** | §2/§3: the proposal permits such waits to trap. | No host-thread deadlock. |

Guardrail: a `shared` annotation is a static safety claim, not a runtime flag.
This revision accepts the annotations and executes the corresponding atomics in
the documented single-agent model; it does **not** claim cross-agent safety for
the GC heap, waitqueues or a migrating instance. Until `THREADS.md` TH-5 lands,
the safe boundary is host-side serialization of the module's **single instance**
(and Store operations), not per-thread instances.

## 7. Plan

These milestones are **not scheduled**; the proposal is active development and
the runtime substrate is a major redesign. They are ordered so that each stage
has a usable stopping point.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SET-1 — Shared annotations** | ◑ Table/global/element/data flags, `sharecomptype`, shared heap refs and sharedness-aware matching/validation are in. Full shared-to-shared restriction checking and shared tags remain. | SET1 | Shared table/global/GC type decodes; shared/unshared refs do not subtype. Still open: every referenced-item restriction. |
| **SET-2 — Multi-agent substrate** | ◑ Host-side: Store/type-intern/GC/string locks and per-agent trap/exception/tail-call state (TRAP-6 done). Missing: multi-mutator GC and a public agent API. | SET6 | Host threads do not corrupt the Store; two wasm agents still wait for TH-5. |
| **SET-3 — Shared GC atomics** | ✅ 35 opcodes `0x4F–0x71` plus `ref.i31_shared`, with ordering and natural-alignment checks, all lowered through `wasm_atomic_*`. | SET2 | Done for the single-agent model: global/table/struct/array atomic get/set/RMW/cmpxchg execute. |
| **SET-4 — TLS and waitqueues** | Add `thread_local` globals and the `waitqueue`/wait/notify family (needs a collision-free `0x68` assignment). | SET3, SET4 | A thread-local counter and a managed wait/notify round-trip pass. |
| **SET-5 — Lifecycle** | Add the component-model thread spawn/join builtins once their shape is stable. | SET5 | A component can spawn and join a thread without a JS host. |
| **SET-6 — Tests** | ◑ Raw-binary tests cover shared annotations and table/global atomics; proposal scripts and multi-agent fixtures remain. | SET7 | The tests fail if shared validation, ordering or TLS is single-agent-only. |
| **SET-7 — Per-agent state** | ✅ **Done (TRAP-6 / SS-6).** `Agent` owns an `ExecutionState` (trap, exception, tail-call, defer, counters, stack limit); a host thread has a default agent and `AgentScope` installs another. The remaining multi-agent work is the execution model itself (`THREADS.md` TH-5). | SET6 (agent half) | Done: two agents on one host thread observe independent traps and tail-call state. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `atomic` | 249 | 0 | 48 | baseline single-agent atomics |
| shared global + `global.atomic.*` | 9 assertions | 0 | — | shared flag, acqrel RMW, get |
| shared table + `table.atomic.*` | 6 assertions | 0 | — | shared table limit bit, set/get |
| `struct.atomic.*` / `array.atomic.*` | 5 + 5 assertions | 0 | — | GC field/element atomic get/set |
| host threads (4×8 parses/instantiates) | 1 assertion | 0 | — | concurrent Store/type/GC use does not corrupt |
| `sharecomptype` / shared heap ref | 5 + 6 assertions | 0 | — | shared type and `(ref null (shared any))` |
| full suite, default / strict | 9265 assertions / 352 cases | 0 | — | no regression |

Reading: annotation decoding and the working-draft atomics are green in the
single-agent model. Shared GC collection, waitqueues, TLS and lifecycle remain
SET-2…SET-6.

## 9. References

- Shared-Everything Threads:
  https://github.com/WebAssembly/shared-everything-threads/blob/main/proposals/shared-everything-threads/Overview.md
- Baseline threads:
  https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md
- In-repo: [`THREADS.md`](THREADS.md), [`ACQUIRE_RELEASE_ATOMICS.md`](ACQUIRE_RELEASE_ATOMICS.md),
  [`GC.md`](GC.md), [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
  [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md), [`../ABI.md`](../ABI.md),
  [`../SAFETY.md`](../SAFETY.md), [`../THREADSAFETY.md`](../THREADSAFETY.md);
  sources
  `WasmBase/WasmType.hpp`, `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmGcHeap.*`, `WasmBase/WasmModuleInstance.*`,
  `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`), `WasmBase/WasmTrap.cpp`,
  `WasmBase/WasmAgent.hpp` / `WasmValue.hpp` (`ExecutionState::tailCallState`).

