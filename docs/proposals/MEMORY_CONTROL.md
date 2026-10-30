# Memory Control — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — the umbrella's one
concrete instruction, `memory.discard`, decodes, validates, zero-fills and uses
a POSIX shared-memory decommit hint when available; the remaining sub-proposals
(protection/mappable/virtual) are still design space).
Audience: anyone touching `WasmMemory`, `WasmType` (`MemoryType`), the memory
instructions, `WasmOpcode` (`MiscOpcode`), or the host memory API.

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

Memory Control is an early-stage **umbrella** proposal: each sub-proposal is a
separate document, and they may merge or disappear before Phase 2.

- **BYOB for `WebAssembly.Memory`** (copies): let web APIs write into Wasm
  memory without an intermediate copy.
- **`memory.discard`** (footprint): `memory.discard $memidx : [addr:idx
  size:idx] -> []` zero-fills a byte range, rounding the start down and the end
  up to the page size. On a host with virtual memory the pages may be
  decommitted or remapped; the zero-fill is the specified fallback. SpiderMonkey
  prototypes it as `0xFC 0x12`.
- **Static memory protection** (protection): optional no-access and read-only
  regions at the start of a memory (null pointers, read-only data).
- **`mappable`** (copies/protection): a mappable region at the start of the
  address space for mapped files and protection.
- **`virtual` mode** (copies/footprint/protection): per-page map/unmap.
- **Lazy commit** (TODO): reserve without committing, optionally with a
  `memory.commit` complement to `memory.discard`.

The only instruction with a concrete semantics today is `memory.discard`; the
others are design space.

## 3. What the proposal does not demand

- No final binary encoding: even `memory.discard`'s `0xFC 0x12` is a prototype,
  not a standardized assignment.
- No particular OS mechanism; dropping the decommit hint and doing a plain
  zero-fill is always allowed for `memory.discard`.
- No memory-ordering model yet (the overview lists it as a TODO).
- No JS API for a non-web embedder.
- No requirement to implement every sub-proposal; they are independent.

## 4. What we have implemented today

- The linear-memory model is Store-owned (`LinearMemory`), with `memory.size`
  / `memory.grow`, `memory.fill` / `memory.copy` / `memory.init`, multiple
  memories, memory64 and shared/atomic memories.
- Every access is bounds-checked (`effectiveMemoryAddress`, the bulk-memory
  helpers, the atomic helpers); memory64 uses the overflow-safe unsigned check.
- `MiscOpcode` (`WasmOpcode.hpp`) defines `0xFC` sub-opcodes `0`–`18`. Sub-opcode
  `0x12` (18) is `MemoryDiscard`; `dispatchPrefixMisc` reads the `memidx`
  immediate and calls `dispatchMemoryDiscard`.
- The validator types it as `[addr:idx size:idx] -> []`, with both operands
  using `memoryAddressType(memidx)` (so memory64 takes `i64`).
- `LibJit` lowers it to `wasm_memory_discard_impl`: the bounds check runs before
  any page is touched, a zero-length range is a no-op, and the start is aligned
  down / the end up to the engine's 64 KiB page size. Operands are widened to
  `uint64_t` (with the memory's index type selecting the source width), so a
  negative i32 is a huge failing address rather than a wrapped small one. The
  helper uses `memorySize`/`memoryBase`, so it works for both memory32 and
  memory64.
- The stub backend prints `MemoryDiscard` for parity with the bulk-memory ops.
- **A decommit hint is used for shared memories (MC-3).** `StoreOwnedMemory`
  exposes `discardHint(offset, bytes)`: `PrivateLinearMemory` keeps the
  portable zero-fill, while `SharedLinearMemory` uses
  `madvise(MADV_DONTNEED)` on the reserved region (POSIX) and returns true when
  the pages are already zero-filled. The helper falls back to `memset` on every
  other host. The observable contract is unchanged.
- There is deliberately **no static protection / `mappable` / `virtual` mode**
  (`MC4`/`MC5`): those sub-proposals have no stable binary design yet.
- `MemoryType` carries only `indexType` and `limits`. There is no protection,
  mappable or virtual mode; unknown limit flags are preserved but not acted on.
- There is no `memory.commit` and no page-protection API.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~MC1~~ | ~~**`memory.discard` does not decode.**~~ **Done (MC-1):** `MiscOpcode::MemoryDiscard = 18` and the `memidx` immediate are decoded. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | Closed |
| ~~MC2~~ | ~~**No zero-fill helper.**~~ **Done (MC-2):** `wasm_memory_discard_impl` is bounds-checked and page-rounded (with `memoryIndexTypeForIndex` for i32/i64 operand widening). | `LibJit/LibjitOpcodeDispatcher.cpp` | Closed |
| ~~MC3~~ | ~~**No decommit hint.**~~ **Done (MC-3):** `StoreOwnedMemory::discardHint` decommits shared-memory pages with `madvise(MADV_DONTNEED)` on POSIX; private memories and other hosts use the specified zero-fill fallback. | `WasmMemory.cpp` (`SharedLinearMemory::discardHint`), `wasm_memory_discard_impl` | Closed |
| MC4 | **No static protection / `mappable` / `virtual` mode.** `MemoryType` has only index type and limits. | `WasmType.hpp` (`MemoryType`) | Deferred (sub-proposals are design space) |
| MC5 | **No BYOB / host memory window API.** A non-web embedder has no way to hand a writable window to a producer. | `WasmMemory.hpp` | Deferred (web/API surface) |
| ~~MC6~~ | ~~**No test.**~~ **Done (MC-4):** `Test/wasm_wat/memory_discard.wat` plus the manual test cover page rounding, preservation, zero-length and out-of-bounds traps (memory32 and memory64). | `Test/main.cpp`, `Test/wasm_wat/memory_discard.wat` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MCD1 | **Implement `memory.discard` as a bounds-checked zero-fill** (implemented). | §2/§3: zero-fill is the specified semantics and the OS hint is optional. | Exact behaviour on every host, with an optional fast path. |
| MCD2 | **Drop the decommit hint** on hosts without a usable virtual-memory API. | §3: no particular OS mechanism is required. | Portability. |
| MCD3 | **Keep the `memidx` immediate** and reuse the existing bounds check (implemented). | §2's signature carries `$memidx`; §3 leaves the addressing free and multi-memory is already in the tree. | No new address path. |
| MCD4 | **Do not implement protection/mappable/virtual from the umbrella.** | §3: sub-proposals are independent and may disappear. | Scope stays on the one concrete instruction. |
| MCD5 | **Treat an unknown memory flag as data, not behavior.** | §3: no behaviour is attached until the proposal defines it. | Forward compatibility with future flags. |
| MCD6 | **Treat a zero-length `memory.discard` as a no-op**, even at a non-page-aligned address. | §3: the range is empty, and an empty range frees/zeroes no pages. | Avoids zeroing an unrelated page for an empty request. |

Guardrail: `memory.discard` must never trap on a valid in-bounds range, and an
out-of-bounds range must trap (or be rejected) before any page is touched.
Zeroing must also be visible to a shared memory's other agents.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MC-0 — Honest rejection** | ✅ Superseded: `0xFC 0x12` is now decoded, so an unknown sub-opcode still fails loudly while `memory.discard` executes. | MC1 | Done: no accidental interpretation of unassigned sub-opcodes. |
| **MC-1 — Decode** | ✅ Added `MiscOpcode::MemoryDiscard` and the `memidx` immediate. | MC1 | Done: `memory_discard.wat` loads. |
| **MC-2 — Zero-fill** | ✅ Lowered to `wasm_memory_discard_impl` with bounds checks and 64 KiB page rounding. | MC2 | Done: the manual test asserts page rounding, preservation, zero-length and traps on memory32 and memory64. |
| **MC-3 — Hint** | ✅ `StoreOwnedMemory::discardHint`; shared memories use `madvise(MADV_DONTNEED)` on POSIX, with the specified zero-fill fallback elsewhere. | MC3 | Done: shared-memory discard zeroes the page and preserves the rest; RSS behaviour is host-dependent by design. |
| **MC-4 — Tests** | ✅ `Test/wasm_wat/memory_discard.wat` plus `memory.discard zeroes whole pages, preserves the rest, and traps out of bounds`. | MC6 | Done: 19 assertions, including the memory64 and zero-length paths. |
| **MC-5 — Later sub-proposals** | ◑ Revisit protection/mappable/virtual when they reach a stable design. | MC4, MC5 | Remaining: a separate note/milestones. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory` | 65 | 0 | 25 | the existing size/grow surface is green |
| `memory-multi` | 6 | 0 | 0 | multiple memories are the base for `memidx` |
| `memory64` | 55 | 0 | 14 | 64-bit addresses are already handled |
| `memory_discard` manual test | 24 assertions | 0 | 0 | page rounding, preservation, zero-length and OOB traps on memory32, memory64 and shared memory |
| `memory.discard` script | — | — | — | no script in the snapshot |

Reading: the memory substrate is solid and the one concrete instruction in the
umbrella is now exercised; MC-4 is the coverage that used to be missing.

## 9. References

- Proposal overview (`WebAssembly/memory-control`):
  https://github.com/WebAssembly/memory-control/blob/main/proposals/memory-control/Overview.md
- `memory.discard` sub-proposal:
  https://github.com/WebAssembly/memory-control/blob/main/proposals/memory-control/discard.md
- In-repo: [`MULTI_MEMORY.md`](MULTI_MEMORY.md), [`MEMORY64.md`](MEMORY64.md),
  [`CUSTOM_PAGE_SIZES.md`](CUSTOM_PAGE_SIZES.md) (the static page-size
  dimension), [`BULK_MEMORY.md`](BULK_MEMORY.md), [`THREADS.md`](THREADS.md),
  [`../SAFETY.md`](../SAFETY.md), [`../ABI.md`](../ABI.md); sources
  `WasmBase/WasmMemory.hpp`/`.cpp`, `WasmBase/WasmType.hpp` (`MemoryType`),
  `WasmBase/WasmOpcode.hpp` (`MiscOpcode::MemoryDiscard`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixMisc`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchMemoryDiscard`,
  `wasm_memory_discard_impl`), `Test/wasm_wat/memory_discard.wat`.
