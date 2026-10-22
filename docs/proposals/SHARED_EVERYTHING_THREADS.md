# Shared-Everything Threads — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — the proposal is
**not implemented**. The engine has the baseline threads/atomics surface and a
single-agent execution model, but no `shared` annotations, shared GC heap,
thread-local globals, waitqueues or thread lifecycle.)

Audience: anyone touching `WasmType` (`shared`/`globaltype`), `WasmStore`,
`WasmModuleInstance`, the GC heap/refcount, `WasmOpcode` (`0xFE` atomics),
`WasmTrap`, or a future multi-agent runtime.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.

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

- **Baseline threads only.** `WasmBase/WasmOpcode.hpp` has all 67 baseline
  `0xFE` atomic opcodes; all execute at seqcst (`THREADS.md` TH-1). Shared
  linear memory is implemented (`SharedLinearMemory`, `isShared`), but the
  execution model is one agent on the caller's thread (`THREADS.md` §3/TD9).
- **No shared annotations.** `Subtype` has no `shared` bit; `GlobalType` has no
  `thread_local`; `TableType`/`StructType`/`ArrayType` have no shared variant.
  Memory sharing is represented by the existing limits/`isShared` path, not by
  the new general `share` production.
- **No shared GC atomics.** `GcHeap` allocation/collection and each instance's
  region set assume one mutator; individual refcount slots use atomics, but
  that does not make collection concurrency-safe. The `Store` is a
  process-global, and `g_tailCallState` is a process-global single-call-chain
  trampoline (`ABI.md`/`TAILCALLS.md`). Those are prerequisites for a
  multi-agent build.
- **No waitqueue/TLS/lifecycle.** `wait`/`notify` are the single-agent baseline
  forms (`THREADS.md` TD4); there is no `waitqueue` type, `thread_local` global
  or component-level thread builtin.
- **No test coverage.** The suite has the baseline `atomic` script, not a
  shared-everything test.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| SET1 | **No `shared` annotations or shared-type validation.** Functions, tables, globals, tags and GC types cannot be marked shared, and the shared-to-nonshared rule is not checked. | `WasmType.hpp` (`Subtype`, `GlobalType`, `TableType`); `WasmValidator.cpp`; `THREADS.md`. | High |
| SET2 | **No shared GC atomics.** There are no atomic struct/array/global/table operations, no shared `anyref` restriction and no acquire-release ordering for GC fields. | `WasmOpcode.hpp` (`AtomicOpcode` baseline); `GC.md`; no `struct.atomic.*`. | High |
| SET3 | **No managed waitqueue.** There is no `waitqueue`/`shared waitqueue` type, `waitqueue.new`/`notify`, or GC-object `wait` operation. | `WasmType.hpp`; `THREADS.md` §4.3 (single-agent waiters). | High |
| SET4 | **No thread-local globals.** `GlobalType` has only value/mutability; there is no `thread_local` annotation or per-thread storage. | `WasmType.hpp` (`GlobalType`); `WasmModuleInstance` globals. | High |
| SET5 | **No thread lifecycle/component builtins.** There is no thread spawn/join or component-model integration. | `WasmComponentModel.hpp`; `COMPONENT_MODEL.md` CM10. | High |
| SET6 | **The runtime substrate is single-agent.** `Store`, GC allocation/collection, the tail-call trampoline and trap state are not a multi-mutator design. | `WasmStore.hpp`; `WasmGcHeap.*`; `WasmBase/WasmValue.hpp` (`g_tailCallState`); `WasmTrap.cpp`. | High |
| SET7 | **No tests.** No shared-everything fixture or proposal script exists. | `Test/wast_supported.txt`; `THREADS.md` §4.4. | Medium |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SETD1 | **Do not implement the proposal until it stabilizes.** Reject `shared`-annotated types/opcodes clearly rather than mis-decoding them. | §3: it is an active umbrella with no stable encoding; an engine may reject unsupported features. | No moving-target ABI in the core. |
| SETD2 | **Keep the single-agent execution model as a stop-gap.** | §3: baseline threads and shared memory already work; agents are deferred by `THREADS.md` TD9. | No premature scheduler/GC redesign. |
| SETD3 | **Do not allow shared-to-unshared references.** | §3: that is the conservative reading and the proposal's default validation rule. | No weak-reference or cross-heap-cycle semantics to guess. |
| SETD4 | **Do not promise cross-heap cycle collection.** | §3: the spec does not mandate a collector or cross-heap cycle collection. | The existing single-heap RC collector remains honest. |
| SETD5 | **Reject waits on an unsupported/main thread rather than block it.** | §2/§3: the proposal permits such waits to trap. | No host-thread deadlock. |

Guardrail: a `shared` annotation is a static safety claim, not a runtime flag.
Do not accept a shared type and then execute it on the single-agent heap without
either implementing the multi-mutator rules or rejecting the module. Shared
memory's existing `isShared` path is not a substitute for shared GC/tables.

## 7. Plan

These milestones are **not scheduled**; the proposal is active development and
the runtime substrate is a major redesign. They are ordered so that each stage
has a usable stopping point.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SET-1 — Shared annotations** | Add `shared` to the relevant type productions and the shared-to-shared validation rule. | SET1 | A shared function/table/global/GC type validates; a shared-to-unshared reference is rejected. |
| **SET-2 — Multi-agent substrate** | Make `Store`, GC allocation/collection, trap state and the tail-call trampoline per-agent or otherwise concurrency-safe. | SET6 | Two agents can run and share a memory without corrupting the Store or trap state. |
| **SET-3 — Shared GC atomics** | Add the working draft's struct/array/global/table atomics and shared-`anyref` restrictions. | SET2 | Atomic field/slot operations execute with the requested ordering and tear-free guarantees. |
| **SET-4 — TLS and waitqueues** | Add `thread_local` globals and the `waitqueue`/wait/notify family. | SET3, SET4 | A thread-local counter and a managed wait/notify round-trip pass. |
| **SET-5 — Lifecycle** | Add the component-model thread spawn/join builtins once their shape is stable. | SET5 | A component can spawn and join a thread without a JS host. |
| **SET-6 — Tests** | Curate the proposal scripts and add multi-agent fixtures. | SET7 | The tests fail if shared validation, ordering or TLS is single-agent-only. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `atomic` | 249 | 0 | 48 | baseline single-agent seqcst atomics, not shared-everything |
| shared annotations | — | — | — | no script and no syntax |
| shared GC / waitqueue / TLS | — | — | — | no script and no implementation |
| thread lifecycle | — | — | — | no script and no component builtin |

Reading: the baseline threads surface is the only green evidence. It does not
exercise shared GC, TLS, waitqueues, or multiple agents; those are SET-1…SET-6.

## 9. References

- Shared-Everything Threads:
  https://github.com/WebAssembly/shared-everything-threads/blob/main/proposals/shared-everything-threads/Overview.md
- Baseline threads:
  https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md
- In-repo: [`THREADS.md`](THREADS.md), [`ACQUIRE_RELEASE_ATOMICS.md`](ACQUIRE_RELEASE_ATOMICS.md),
  [`GC.md`](GC.md), [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
  [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md), [`../ABI.md`](../ABI.md),
  [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp`, `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmGcHeap.*`, `WasmBase/WasmModuleInstance.*`,
  `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`), `WasmBase/WasmTrap.cpp`,
  `WasmBase/WasmValue.hpp` (`g_tailCallState`).

