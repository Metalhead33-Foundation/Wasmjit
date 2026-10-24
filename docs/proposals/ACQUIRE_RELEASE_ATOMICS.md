# Acquire-Release Atomics — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — **ARA-1…ARA-3
implemented** in the single-agent build: `MemArg` bit 4 + `u8` ordering, the
`0xFE 0x04` `pause` no-op, and order-accurate lowering to
`__ATOMIC_SEQ_CST`/`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE`/`__ATOMIC_ACQ_REL`.
Two-agent observation remains deferred with `THREADS.md` TH-5.)

Audience: anyone touching `WasmType` (`MemArg`), `WasmOpcode` (`AtomicOpcode`),
`WasmOpcodeDispatcher`, `wasm_atomic.*`, `LibjitOpcodeDispatcher`, or the
threads/atomics test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.
The guest/host boundary contract is [`THREADSAFETY.md`](../THREADSAFETY.md).

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
  `cmpxchg` variant, plus `AtomicPause = 0x04`.
- `WasmBase/WasmType.hpp` has `MemArg` (with `ordering`) and `AtomicMemArg`.
  Ordinary `MemArg` decoding rejects bit 4, so a non-atomic load cannot carry
  an ordering byte. Atomic decoding accepts `0x10`, reads the following `u8`,
  and rejects values other than 0 (`seqcst`) and 1 (`acqrel`).
- `atomic.fence` consumes its ordering byte and accepts 0 or 1; its reserved
  zero-only rule is gone. `0xFE 0x04` `pause` decodes with no immediate and
  lowers to nothing (ARAD2).
- `WasmBase/wasm_atomic.*` takes the ordering on every load/store/RMW/cmpxchg:
  loads map `acqrel` to `__ATOMIC_ACQUIRE`, stores to `__ATOMIC_RELEASE`,
  RMW/cmpxchg to `__ATOMIC_ACQ_REL`, and omission stays `__ATOMIC_SEQ_CST`.
  The fence maps 1 to `__ATOMIC_ACQ_REL` and 0 to `__ATOMIC_SEQ_CST`.
- `WasmValidator::requireAtomicMemoryAccess` checks ordering, natural
  alignment and offset; `requireMemoryAccess` rejects an ordering on a normal
  access. The acquire-release ordering is exercised end-to-end by a raw-binary
  fixture (`acquire-release ordering and pause decode, validate, and execute`).
- The two-agent message-passing half is still deferred (`THREADS.md` TH-5), so
  the ordering is not *observable* between wasm threads yet; the encoding and
  lowering are complete and distinct. The trap/exception/tail-call state
  is per agent (`Agent`/`ExecutionState`, TRAP-6 done); only the two-agent
  execution model remains deferred with TH-5.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ARA5 | **No focused multi-agent test.** A single-agent fixture covers decode/lowering, but there is no second agent to observe an acquire/release edge. | `Test/main.cpp` acquire-release case; `THREADS.md` TH-5. | Low (deferred) |


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
| **ARA-1 — Decode** | ✅ `MemArg` ordering bit/byte, `AtomicMemArg`, `pause`, decode-time rejection on non-atomics. | ARA1, ARA3 | Done: ordering/pause fixture decodes; malformed flags and ordinary bit-4 uses are rejected. |
| **ARA-2 — Lowering** | ✅ Ordering threaded through every atomic helper and the fence (`__ATOMIC_SEQ_CST` / `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` / `__ATOMIC_ACQ_REL`). | ARA2, ARA4 | Done: fence and RMW use the requested ordering; the single-agent differential fixture sees the same values. |
| **ARA-3 — Tests** | ◑ Single-agent ordering/pause fixture done. The two-agent message-passing test waits for `THREADS.md` TH-5. | ARA5 | Done for decode/lowering; multi-agent observation deferred. |

ARA-1/ARA-2 are independent of a future multi-agent scheduler; ARA-3's
two-agent half waits for `THREADS.md` TH-5.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `atomic` | 249 | 0 | 48 | single-agent baseline; 48 skips are validation cases |
| acquire-release + `pause` fixture | 5 assertions | 0 | — | acqrel store/load execute; `pause` is a no-op |
| full suite, default | 9265 assertions / 352 cases | 0 | — | no regression |
| full suite, `WASM_SPEC_VALIDATE=1` | 9265 assertions / 352 cases | 0 | — | negative-module validation stays green |
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
  [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md),
  [`../THREADSAFETY.md`](../THREADSAFETY.md); sources
  `WasmBase/WasmType.hpp` (`MemArg`), `WasmBase/WasmOpcode.hpp` (`AtomicOpcode`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixAtomic`),
  `WasmBase/wasm_atomic.*`, `LibJit/LibjitOpcodeDispatcher.cpp`,
  `Test/wast_supported.txt`.

