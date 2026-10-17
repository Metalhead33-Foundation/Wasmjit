# Memory Control — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — the proposal is an
early umbrella of sub-proposals; none of them is implemented).
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
- `MiscOpcode` (`WasmOpcode.hpp`) defines `0xFC` sub-opcodes `0`–`17`
  (trunc-sat, bulk memory, table ops). Sub-opcode `0x12` (18, the prototype
  `memory.discard`) is **unassigned**; `dispatchPrefixMisc`'s `default` throws
  `"Invalid or unsupported 0xFC misc sub-opcode"`.
- `MemoryType` carries only `indexType` and `limits`. There is no protection,
  mappable or virtual mode; unknown limit flags are preserved but not acted on.
- There is no `memory.commit` and no page-protection API.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| MC1 | **`memory.discard` does not decode.** `0xFC 0x12` is unassigned and reaches the misc dispatcher's default. | `WasmOpcode.hpp` (`MiscOpcode`), `WasmOpcodeDispatcher.cpp` | High |
| MC2 | **No zero-fill helper.** There is no bounds-checked page-zeroing primitive to lower `memory.discard` onto. | `WasmMemory.cpp`, `wasm_simd_scalar`/bulk helpers | Medium |
| MC3 | **No decommit hint.** The engine never releases physical pages to the host. | `WasmMemory.cpp` | Low |
| MC4 | **No static protection / `mappable` / `virtual` mode.** `MemoryType` has only index type and limits. | `WasmType.hpp` (`MemoryType`) | Low |
| MC5 | **No BYOB / host memory window API.** A non-web embedder has no way to hand a writable window to a producer. | `WasmMemory.hpp` | Low |
| MC6 | **No test.** There is no `memory.discard` script in the snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MCD1 | **Implement `memory.discard` as a bounds-checked zero-fill.** | §2/§3: zero-fill is the specified semantics and the OS hint is optional. | Exact behaviour on every host, with an optional fast path. |
| MCD2 | **Drop the decommit hint** on hosts without a usable virtual-memory API. | §3: no particular OS mechanism is required. | Portability. |
| MCD3 | **Keep the `memidx` immediate** and reuse the existing bounds check. | §2's signature carries `$memidx`; §3 leaves the addressing free and multi-memory is already in the tree. | No new address path. |
| MCD4 | **Do not implement protection/mappable/virtual from the umbrella.** | §3: sub-proposals are independent and may disappear. | Scope stays on the one concrete instruction. |
| MCD5 | **Treat an unknown memory flag as data, not behavior.** | §3: no behaviour is attached until the proposal defines it. | Forward compatibility with future flags. |

Guardrail: `memory.discard` must never trap on a valid in-bounds range, and an
out-of-bounds range must trap (or be rejected) before any page is touched.
Zeroing must also be visible to a shared memory's other agents.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MC-0 — Honest rejection** | Keep `0xFC 0x12` rejected until the helper exists (no accidental interpretation). | MC1 | A `memory.discard` module fails with a named error. |
| **MC-1 — Decode** | Add the misc sub-opcode and the `memidx` immediate. | MC1 | The instruction decodes. |
| **MC-2 — Zero-fill** | Lower to a bounds-checked page-zeroing helper. | MC2 | A discard makes the range read as zero and preserves the rest. |
| **MC-3 — Hint** | Optionally `madvise`/`VirtualFree`/`mmap` the pages when the host supports it. | MC3 | The fast path is observable only in RSS, not results. |
| **MC-4 — Tests** | Add a manual `memory.discard` test (no script in the snapshot). | MC6 | The test asserts zeroing, bounds and cross-memory behaviour. |
| **MC-5 — Later sub-proposals** | Revisit protection/mappable/virtual when they reach a stable design. | MC4, MC5 | A separate note/milestones exist. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory` | 65 | 0 | 25 | the existing size/grow surface is green |
| `memory-multi` | 6 | 0 | 0 | multiple memories are the base for `memidx` |
| `memory64` | 55 | 0 | 14 | 64-bit addresses are already handled |
| `memory.discard` | — | — | — | no script in the snapshot |

Reading: the memory substrate is solid; there is simply no discard script to
grade. MC-4's manual test is the first real coverage.

## 9. References

- Proposal overview (`WebAssembly/memory-control`):
  https://github.com/WebAssembly/memory-control/blob/main/proposals/memory-control/Overview.md
- `memory.discard` sub-proposal:
  https://github.com/WebAssembly/memory-control/blob/main/proposals/memory-control/discard.md
- In-repo: [`MULTI_MEMORY.md`](MULTI_MEMORY.md), [`MEMORY64.md`](MEMORY64.md),
  [`BULK_MEMORY.md`](BULK_MEMORY.md), [`THREADS.md`](THREADS.md),
  [`../SAFETY.md`](../SAFETY.md), [`../ABI.md`](../ABI.md); sources
  `WasmBase/WasmMemory.hpp`/`.cpp`, `WasmBase/WasmType.hpp` (`MemoryType`),
  `WasmBase/WasmOpcode.hpp` (`MiscOpcode`),
  `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixMisc`).
