# Shared-Everything Threads — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 5 — **SET-1, SET-2,
SET-3, SET-6 and SET-7 implemented**. The engine has `shared`
table/global/segment annotations, shared `comptype`/heap-type encoding, and the
working draft's `0xFE 0x4F–0x71` global/table/struct/array atomics plus
`ref.i31_shared`; those atomics now run the GC retain/release barrier for
reference slots (SET2). Agents are built (`THREADS.md` TH-5): an opt-in
`ConcurrencyModel::Agents` makes the one instance concurrently callable, with a
real waiter table and a multi-mutator-safe GC path. **SET-4 (TLS/managed
waitqueues) and SET-5 (thread lifecycle) remain unimplemented because the
proposal has no stable binary encoding for them** — `waitqueue`'s `0x68`
collides with `cont` here, `thread_local` has no assigned `globaltype`
encoding, and the component-model spawn/join builtins are still moving.)

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
- **Concurrent mutators (SET-2, revision 5).** `RuntimeConfig::concurrency =
  Agents` admits several host threads into one `ModuleInstance`; the entry
  guard is an atomic counter, the GC reclaim queue/cycle candidates are under
  the per-instance `gcMutex`, the allocation/reference counters and the
  data/element drop flags are atomic, and `GcHeap`/`Store`/string interning are
  mutex-guarded. The trap/exception/tail-call state belongs to the per-agent
  `ExecutionState` (TRAP-6).
- **Reference atomic slots run the write barrier (SET2).**
  `wasm_atomic_ref_store/xchg/cmpxchg` combine the pointer-atomic update with
  the GC retain/release pair (ownership transfers on `xchg`, retain/release the
  observed value on a failed `cmpxchg`), and the global/table/struct/array
  atomic set/xchg/cmpxchg handlers use them for reference-typed slots.
- **No managed waitqueue/TLS/lifecycle.** `waitqueue` (`0x68`) collides with the
  stack-switching continuation heap type in this engine's current encoding
  space, so it is not decoded; `thread_local` globals and component thread
  spawn/join have no stable binary form yet. **These are the only SET gaps and
  they are encoding-blocked, not implementation choices** (SET-4/SET-5).
- **Tests.** Raw-binary fixtures cover a shared global with the atomic
  family, a shared table, `sharecomptype`, `(ref null (shared any))` and the
  sharedness validation rule. `[multiagent]` adds cross-agent shared-global
  RMW, concurrent GC allocation/reclamation and a direct reference-slot
  barrier test.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~SET1~~ | ~~**Shared annotations are not fully validated.**~~ **Done (SET-1, 2026-10-29):** `Validator::validateModule` enforces "a shared item may refer only to shared items" for shared function/struct/array types, shared globals and shared tables — the reference must carry the shared prefix (a bare shorthand such as `externref`/`funcref` never does), numeric values are trivially shareable. *Shared tags* remain: the tag attribute byte has no shared flag in the encoding this engine implements, so there is nothing to validate yet (and guessing one is exactly what SETD1 refuses). | `WasmBase/WasmValidator.cpp`; `Test/main.cpp` (`shared annotations are validated against every referenced type`) | Closed (tags conditional on an encoding) |
| ~~SET2~~ | ~~**No shared GC heap / multi-mutator collection.**~~ **Closed 2026-10-29:** the heap, reclaim queue and cycle candidates are guarded, the counters/segment flags are atomic, `reference`-typed atomic set/xchg/cmpxchg run `wasm_atomic_ref_*` (retain/release barrier), and `ConcurrencyModel::Agents` makes the one instance concurrently callable. `gcCollect` remains a documented quiescent safepoint. | `WasmGcHeap.*`; `LibjitOpcodeDispatcher.cpp` (`*Atomic*`); `wasm_atomic.cpp`; `[multiagent]` | Closed |
| SET3 | **No managed waitqueue.** `0x68` is currently `AbstractHeapType::Cont`; the working draft's `waitqueue` type needs a collision-free assignment. | `WasmType.hpp`; `THREADS.md`. | **Encoding-blocked** (no stable binary form) |
| SET4 | **No thread-local globals.** `GlobalType` has `isShared` but no `thread_local` storage; the proposal has not assigned a `globaltype` encoding for it. | `WasmType.hpp`; `WasmModuleInstance` globals. | **Encoding-blocked** |
| SET5 | **No thread lifecycle/component builtins.** The component-model spawn/join builtins have no stable shape; no encoding is assigned. | `WasmComponentModel.hpp`; `COMPONENT_MODEL.md`. | **Encoding-blocked** |
| ~~SET6~~ | ~~**Runtime is not multi-mutator.**~~ **Closed 2026-10-29:** `Agents` is implemented (`THREADS.md` TH-5) and `[multiagent]` exercises concurrent entry, shared-global RMW, per-agent traps, GC allocation/reclamation and the reference-slot barrier. | `WasmConcurrency.hpp`; `WasmModuleInstance`; `Test/ConcurrencyTests.cpp`. | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SETD1 | **Implement only the stable-enough wire subset; reject unknown reserved encodings clearly.** The current `0x4F–0x71` atomics, annotation bits and `0x65` prefixes are decoded; `waitqueue`/TLS remain rejected or absent because they have no stable encoding. | §3: it is an active umbrella; an engine may reject unsupported features. | Useful ABI surface without guessing the moving parts. |
| SETD2 | **Keep `Single` as the default and make `Agents` opt-in.** | §3: concurrency is not required; the embedding chooses. | The old contract for existing embedders, real concurrent mutators for those that opt in. |
| SETD3 | **Do not allow shared-to-unshared references.** | §3: that is the conservative reading and the proposal's default validation rule. | No weak-reference or cross-heap-cycle semantics to guess. |
| SETD4 | **Do not promise cross-heap cycle collection.** | §3: the spec does not mandate a collector or cross-heap cycle collection. | The existing single-heap RC collector remains honest. |
| SETD5 | **Reject waits on an unsupported/main thread rather than block it.** | §2/§3: the proposal permits such waits to trap. | No host-thread deadlock. |

Guardrail: a `shared` annotation is a static safety claim, not a runtime flag.
This revision accepts the annotations, executes the corresponding atomics, and
makes the one instance concurrently callable under `Agents`. It does **not**
claim thread-local storage, a managed waitqueue type or a component thread
lifecycle: those have no stable binary encoding, so there is nothing honest to
decode yet. The safe boundary for an embedder remains: serialize compilation,
join every agent before teardown, and let the guest synchronize shared state
with the atomic instructions.

## 7. Plan

These milestones are **not scheduled**; the proposal is active development and
the runtime substrate is a major redesign. They are ordered so that each stage
has a usable stopping point.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SET-1 — Shared annotations** | ✅ **Done 2026-10-29.** Table/global/element/data flags, `sharecomptype`, shared heap refs, sharedness-aware matching, and the shared-to-shared restriction on every referenced item (type fields/params/results, globals, tables). Shared *tags* stay open only because the encoding has no shared flag to read. | SET1 | Done: shared/unshared refs do not subtype, and a shared type/global/table that names an unshared reference is rejected at the load boundary (`shared annotations are validated against every referenced type`). |
| **SET-2 — Multi-agent substrate** | ✅ **Done 2026-10-29:** `ConcurrencyModel::Agents`, atomic entry counter, waiter table, per-agent state, atomic GC counters/segment flags, guarded reclaim queue/heap and the reference-slot barrier. Multiple agents share the *one* instance (multiple instances remain ruled out; concurrent compilation is rejected). | SET6 | Two agents execute one instance concurrently without corrupting the Store, the GC heap or each other's trap state (`[multiagent]`). |
| **SET-3 — Shared GC atomics** | ✅ 35 opcodes `0x4F–0x71` plus `ref.i31_shared`, with ordering and natural-alignment checks, all lowered through `wasm_atomic_*`; reference slots go through the retain/release barrier. | SET2 | Done: global/table/struct/array atomic get/set/RMW/cmpxchg execute, including cross-agent shared-global RMW. |
| **SET-4 — TLS and waitqueues** | **Not implementable today:** needs a collision-free type assignment for `waitqueue` and a `globaltype` encoding for `thread_local`; the proposal assigns neither. Memory `wait`/`notify` is implemented. | SET3, SET4 | Revisit when the upstream draft publishes a binary encoding. |
| **SET-5 — Lifecycle** | **Not implementable today:** the component-model thread spawn/join builtins have no stable shape or encoding. | SET5 | Revisit when the upstream draft publishes one. |
| **SET-6 — Tests** | ✅ Raw-binary tests cover shared annotations, global/table/struct/array atomics, and `[multiagent]` covers cross-agent atomics, GC and traps. Proposal scripts for TLS/waitqueue cannot exist without the encoding. | SET7 | The tests fail if shared validation, ordering or multi-agent execution regresses. |
| **SET-7 — Per-agent state** | ✅ **Done (TRAP-6 / SS-6).** `Agent` owns an `ExecutionState` (trap, exception, tail-call, defer, counters, stack limit); a host thread has a default agent and `AgentScope` installs another. The remaining multi-agent work is the execution model itself (`THREADS.md` TH-5); the A/B continuations are not agents. | SET6 (agent half) | Done: two agents on one host thread observe independent traps and tail-call state. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-29):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `atomic` | 249 | 0 | 48 | baseline single-agent atomics |
| shared global + `global.atomic.*` | 9 assertions | 0 | — | shared flag, acqrel RMW, get |
| shared table + `table.atomic.*` | 6 assertions | 0 | — | shared table limit bit, set/get |
| `struct.atomic.*` / `array.atomic.*` | 5 + 5 assertions | 0 | — | GC field/element atomic get/set |
| `[multiagent]` | 51 assertions / 6 cases | 0 | — | cross-agent acquire-release, wait/notify, shared-global RMW, traps, GC stress, reference-slot barrier |
| host threads (4×8 parses/instantiates) | 1 assertion | 0 | — | concurrent Store/type/GC use does not corrupt |
| `sharecomptype` / shared heap ref | 5 + 6 assertions | 0 | — | shared type and `(ref null (shared any))` |
| sharedness validation (SET-1) | 9 assertions | 0 | — | a shared struct field, global and table must name a shared reference; a shared numeric global is accepted |
| full suite, default | 11,454 assertions / 481 cases | 0 | — | no regression |

Reading: annotation decoding, the working-draft atomics, reference barriers and
concurrent mutators are green. The only remaining SET gaps — TLS, managed
waitqueues and the component thread lifecycle — are encoding-blocked (SET-4,
SET-5), not behaviour the engine could implement today.

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

