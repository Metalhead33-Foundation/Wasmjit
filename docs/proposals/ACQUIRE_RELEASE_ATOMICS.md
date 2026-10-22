# Acquire-Release Atomics — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — the proposal is
**not implemented**. All existing atomic accesses and the fence are
sequentially consistent; there is no ordering immediate and no `pause`.)

Audience: anyone touching `WasmType` (`MemArg`), `WasmOpcode` (`AtomicOpcode`),
`WasmOpcodeDispatcher`, `wasm_atomic.*`, `LibjitOpcodeDispatcher`, or the
threads/atomics test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.

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

## 2. What the proposal demands

Acquire-Release Atomics extends the existing threads atomics with a weaker
ordering and a spin-loop hint:

- **`acqrel` ordering.** An acquire read prevents later accesses from moving
  before it; a release write prevents earlier accesses from moving after it.
  RMWs are an acquire read followed by a release write.
- **Ordering immediate.** For memory atomics, bit 4 (`0x10`) of the `memarg`
  flags says a `u8` ordering follows the flags (and any `memidx`/`typeidx`)
  before `offset`:
  ```
  memarg ::= flags:u32 [memidx|typeidx:u32] [ordering:u8] offset:u32
  ```
  `0` is `seqcst` (the default when absent); `1` is `acqrel`. For RMWs the low
  nibble is the read ordering and the high nibble the write ordering; the
  current proposal requires both to match (`0x00` or `0x11`).
- **Fence.** `atomic.fence`'s reserved byte becomes the ordering immediate:
  `0x00` = seqcst, `0x01` = acqrel.
- **`pause`.** A new `0xFE 0x04` instruction is a semantic no-op and a hint to
  lower to the architecture's spin-loop relaxation (`PAUSE`/`YIELD`).

The text format accepts an optional `seqcst`/`acqrel` ordering keyword before
offset/alignment. Non-atomic instructions must reject a set bit 4; that is a
decode error.

## 3. What the proposal does not demand

- No new value types, memories or tables.
- No separate `acquire`/`release` orderings in the current version; only
  `seqcst` and `acqrel`.
- No required pause implementation: `pause` may lower to a no-op if the target
  has no suitable hint.
- No change to the sequential-consistency default: an omitted ordering is still
  `seqcst`, so existing modules keep their meaning.
- No new waiter semantics; it only changes ordering on existing atomics.
- No requirement for multiple agents in this engine. The encoding and lowering
  can be implemented in a single-agent build; the ordering is observable only
  once a multi-agent execution model exists.

## 4. What we have implemented today

- `WasmBase/WasmOpcode.hpp` defines the complete baseline `AtomicOpcode`
  (`0xFE` prefix): 67 entries from `memory.atomic.notify` through the last
  `cmpxchg` variant.
- `WasmOpcodeDispatcher::dispatchPrefixAtomic` parses a `MemArg` for every
  atomic except `atomic.fence`, whose reserved byte is checked to be zero.
  `MemArg` itself only recognizes bit 6 for a memory index; there is no
  ordering flag.
- All 67 handlers execute; `WasmBase/wasm_atomic.*` performs the access with
  `__atomic_*` at `__ATOMIC_SEQ_CST`, checks natural alignment, and traps on a
  misaligned effective address (`THREADS.md` TH-1).
- `atomic.fence` lowers to `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
- There is no ordering byte in `MemArg`, no `acqrel` enum/helper, no
  `0xFE 0x04` `pause`, and no acquire/release test script. `THREADS.md` records
  the single-agent execution model (no agents/waiters), so the ordering is not
  observable end-to-end yet.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ARA1 | **No ordering immediate in `MemArg`.** The parser reads flags/offset and does not reserve bit 4 or consume a following `u8`. | `WasmBase/WasmType.hpp` (`MemArg`); `WasmOpcodeDispatcher::dispatchPrefixAtomic`. | Medium |
| ARA2 | **Fence and RMW ordering are fixed at seqcst.** `atomic.fence` requires the reserved byte to be zero; all RMW helpers use `__ATOMIC_SEQ_CST`. | `WasmOpcodeDispatcher.cpp`; `WasmBase/wasm_atomic.*`. | Medium |
| ARA3 | **`pause` is absent.** `0xFE 0x04` is not in `AtomicOpcode`; the dispatcher's default rejects it. | `WasmOpcode.hpp` (`AtomicOpcode`); `dispatchPrefixAtomic` default. | Low |
| ARA4 | **No acquire/release lowering or scope tests.** There is no `__ATOMIC_ACQ_REL` path and no way to verify the ordering distinctions. | `wasm_atomic.*`; no ordering test. | Medium |
| ARA5 | **No focused coverage.** The atomics script is single-agent and seqcst; it does not exercise ordering bytes or `pause`. | `THREADS.md` §4.4 (`atomic` 249/0/48); `Test/wast_supported.txt`. | Low |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ARAD1 | **Default an omitted ordering to `seqcst`.** | §3: backward compatibility is part of the proposal. | Existing atomics keep their exact meaning. |
| ARAD2 | **Implement `pause` as a no-op initially**, then add `PAUSE`/`YIELD` per target. | §3: it is a hint; semantics are unchanged. | No dependence on host-specific spin-loop tuning for correctness. |
| ARAD3 | **Keep the single-agent execution model for now.** | §3: ordering is unobservable without multiple agents; `THREADS.md` defers agents by decision. | Decode/lower correctness without a scheduler rewrite. |
| ARAD4 | **Do not add separate `acquire`/`release` orderings** until the proposal does. | §3: the current proposal defines only `seqcst` and `acqrel`. | No speculative encoding. |
| ARAD5 | **Reject bit 4 on non-atomic instructions.** | §2: that is a decode error in the proposal. | A malformed ordinary load cannot be mistaken for an atomic. |

Guardrail: a default/omitted ordering must remain `seqcst`, and `acqrel` must
map to `__ATOMIC_ACQ_REL` (not `__ATOMIC_SEQ_CST`, not `__ATOMIC_RELAXED`).
`pause` must never change the value or firing of a following atomic operation.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **ARA-1 — Decode** | Add the ordering bit/byte to `MemArg`, validate it, and add `0xFE 0x04` `pause`. | ARA1, ARA3 | A fixture with `seqcst`, `acqrel` and `pause` decodes; a bad ordering or a non-atomic bit-4 use is rejected. |
| **ARA-2 — Lowering** | Thread the ordering through the atomic helpers (`__ATOMIC_SEQ_CST` / `__ATOMIC_ACQ_REL`) and the fence. | ARA2, ARA4 | Fence and RMW lower to the requested ordering; a single-agent differential test sees the same values. |
| **ARA-3 — Tests** | Add ordering/pause fixtures and, when agents exist, a two-agent message-passing test. | ARA5 | The test fails if `acqrel` silently falls back to seqcst or `pause` traps. |

ARA-1/ARA-2 are independent of a future multi-agent scheduler; ARA-3's
two-agent half waits for `THREADS.md` TH-5.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `atomic` | 249 | 0 | 48 | single-agent seqcst baseline; 48 skips are validation cases |
| acquire-release ordering | — | — | — | no script and no ordering byte |
| `pause` | — | — | — | no script and no opcode |

Reading: the baseline atomic surface is green, but it proves seqcst only. The
new ordering is not exercised by the snapshot.

## 9. References

- Acquire-Release Atomics:
  https://github.com/WebAssembly/acquire-release-atomics
- Baseline threads/atomics:
  https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md
- In-repo: [`THREADS.md`](THREADS.md), [`SHARED_EVERYTHING_THREADS.md`](SHARED_EVERYTHING_THREADS.md),
  [`GC_EXTENSIONS.md`](GC_EXTENSIONS.md) (shared `memarg` flags),
  [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`MemArg`), `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixAtomic`),
  `WasmBase/wasm_atomic.*`, `LibJit/LibjitOpcodeDispatcher.cpp`,
  `Test/wast_supported.txt`.

