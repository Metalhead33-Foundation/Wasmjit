# Multiple Memories — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmType` (`MemArg`), `WasmVMContext`,
`WasmMemory`, `WasmStore`, `LibjitOpcodeDispatcher`, or the test harness.

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

Multiple Memories is a **Phase 5** proposal, integrated into WebAssembly
**3.0**.

- A module may **define or import several linear memories**; each has its own
  independent address space and its own limits.
- **`MemArg` gains an explicit memory index.** In the binary encoding, bit 6
  (`0x40`) of the alignment field signals that a `memidx` LEB follows; without
  it the access targets memory 0. So `i32.load`/`i32.store`/all memory
  instructions can address any memory.
- **`memory.size` and `memory.grow` take a `memidx`** immediate.
- **Bulk operations** (`memory.copy`, `memory.fill`, `memory.init`) take
  memory indices: `memory.copy` can copy **between two different memories**.
- **Data segments** name their target memory (`memoryIdx`), including active
  segments.
- **Imports/exports**: memories are imported/exported like before, and
  resolution must match limits (and shared-ness, see
  [`THREADS.md`](THREADS.md)).

## 3. What the proposal does not demand

- **No limit on the number of memories.**
- **No representation requirement.** An array of pointers, a linked list, or
  specialised fields for 1/2/N memories are all fine.
- **No obligation to keep the `memidx` at runtime** if the engine can fold a
  constant index into the access.
- **No requirement on address-space layout** (`memoryBase` need not be
  contiguous with another memory's).
- **No change to alignment rules**; each access still follows the normal
  alignment rules for its own memory.

## 4. What we have implemented today

- `MemArg { uint32_t memidx; uint32_t align; uint64_t offset; }` and the
  `0x40`-flag decode/encode in `WasmType.hpp`. The encode side restores the
  multi-memory form when `memidx != 0`.
- `VMContext::memories` is an array of `LinearMemory*` plus `memoryCount`,
  indexed exactly like the Wasm memory index space (imports first, then the
  module's own memories).
- The `Store` owns every memory (`createLinearMemory`, `memory(id)`), and
  `ModuleInstance::initializeMemories` appends locals after the resolved
  imports.
- Every memory instruction already carries a `memidx`: loads/stores through
  `MemArg`, `memory.size`/`grow` (LEB `MemIdx`), `memory.copy`/`fill`/`init`,
  and data segments via `DataSegment::memoryIdx`.
- Multi-memory import sharing works through the Store, like tables.
- Evidence: `spec: memory-multi` → **6 passed / 0 failed / 0 skipped**, and the
  manual test `multi-memory: two linear memories are independent` passes.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| MM1 | **No bounds check on the memory index.** A `memidx` past `memoryCount` is used to index `ctx.memories` without validation (the load sites assume the fold is valid), so a technically-invalid module can read a wild pointer rather than trap. | `LibjitOpcodeDispatcher.cpp` memory paths | Medium |
| MM2 | **`memory-multi` is not curated.** The passing script is not in `Test/wast_supported.txt`, so it only runs in an explicit sweep. | `Test/wast_supported.txt` | Low |
| MM3 | **Import matching for memories is checked, but data-segment `memoryIdx` is not.** An active segment with an out-of-range memory throws a `runtime_error` at instantiation (acceptable) rather than a Wasm link error. | `applyActiveSegments` | Low |
| MM4 | **Harness cannot marshal memories across modules.** The `spectest` host exposes one shared memory; multi-memory scripts that import/export more are only covered insofar as they use `memory` instructions. | `Test/WastScript.cpp` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MMD1 | **Fold a constant `memidx` into the load/store offset** (already done: the index is a compile-time constant, so `ctx.memories[K]` is loaded once). | §3: the index need not exist at runtime. | No per-access index work. |
| MMD2 | **Keep one flat `memories` array and a `memoryCount`.** | §3: representation free. | Minimal change from single-memory code. |
| MMD3 | **Add a single-memory fast path** (skip the array indirection when `memoryCount == 1`) if it ever shows up in a profile. | §3: representation free. | Fewer loads on the hottest path. |
| MMD4 | **Do not validate `memidx`** beyond a cheap bounds check → `trap()`. | Consistent with the no-validation stance; the check is trivial and prevents a wild read (MM1). | Safe with valid input, not silently unsafe with invalid input. |

Guardrail: `memory.copy` must handle **cross-memory** copies (source and
destination `MemIdx` differ) with correct overlap behaviour within each memory.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MM-1 — memidx bounds check** | Add a bounds check (or a compile-time assertion) for every `memidx` use. | MM1 | An out-of-range `memidx` traps instead of reading a wild pointer. |
| **MM-2 — Curate** | Add `memory-multi` to `Test/wast_supported.txt` (it is already green). | MM2 | The default run covers multi-memory. |
| **MM-3 — Manual cross-memory test** | Extend `Test/wasm_wat/multi_memory.wat` (or add one) with a `memory.copy` between two memories. | MM3 | The test asserts the copied bytes in the destination memory. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Test / script | Result | Reading |
|---|---|---|
| `spec: memory-multi` | 6 passed / 0 failed / 0 skipped | multi-memory works |
| manual `multi-memory: two linear memories are independent` | pass | independent address spaces |
| `memory` (single memory) | curated, passing | baseline |

## 9. References

- Proposal overview (`WebAssembly/multi-memory`):
  https://github.com/WebAssembly/multi-memory/blob/main/proposals/multi-memory/Overview.md
- Spec — Change History (Release 3.0 "Multiple Memories"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Instructions (`memarg` with memory index):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/MEMORY64.md`](MEMORY64.md), [`docs/BULK_MEMORY.md`](BULK_MEMORY.md),
  [`docs/THREADS.md`](THREADS.md), [`docs/ABI.md`](ABI.md);
  sources `WasmBase/WasmType.hpp` (`MemArg`, `MemoryType`),
  `WasmBase/WasmVMContext.hpp` (`memories`, `memoryCount`),
  `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmStore.{hpp,cpp}`,
  `WasmBase/WasmModuleInstance.cpp` (`initializeMemories`,
  `applyActiveSegments`), `LibJit/LibjitOpcodeDispatcher.cpp`,
  `Test/wasm_wat/multi_memory.wat`.
