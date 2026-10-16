# Multiple Memories — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — MM-1…MM-3
implemented: `memidx` bounds checks, curation, and a cross-memory copy test).
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
- **The `memidx` is bounds-checked.** `memoryPointerForIndex` and
  `tablePointerForIndex` reject an index past the module's memory/table index
  space at *compile* time (i.e. during instantiation), and the native
  `memory.*`/`table.*` helpers re-check the index before dereferencing
  `ctx.memories`/`ctx.tables` (MM-1).
- **Curated and regression-tested.** `memory-multi` is in
  `Test/wast_supported.txt` (MM-2), and `Test/wasm_wat/multi_memory.wat` gained
  `copy0to1`/`copy1to0`, exercised by the manual test
  `multi-memory: memory.copy moves bytes between two memories` (MM-3).
- Evidence: `spec: memory-multi` → **6 passed / 0 failed / 0 skipped**; the two
  manual multi-memory tests pass.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~MM1~~ | ~~No bounds check on the memory index.~~ **Done (MM-1):** an out-of-range `memidx` is rejected at compile time by `memoryPointerForIndex` (and `tablePointerForIndex` for tables), and the `memory.*`/`table.*` helpers re-check before indexing. A malformed module can no longer read a wild pointer. | `LibjitOpcodeDispatcher.cpp` | Closed |
| ~~MM2~~ | ~~`memory-multi` is not curated.~~ **Done (MM-2):** it is in `Test/wast_supported.txt` and runs by default. | `Test/wast_supported.txt` | Closed |
| MM3 | **Import matching for memories is checked, but data-segment `memoryIdx` is not.** An active segment with an out-of-range memory throws a `runtime_error` at instantiation (a hard failure, which is the observable outcome the spec requires) rather than a Wasm *link* error. | `applyActiveSegments` | Low (deliberate: no validating front-end) |
| MM4 | **Harness cannot marshal memories across modules.** The `spectest` host exposes one shared memory (plus `memory64`); multi-memory scripts that import/export more are only covered insofar as they use `memory` instructions. | `Test/WastScript.cpp` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MMD1 | **Fold a constant `memidx` into the load/store offset** (already done: the index is a compile-time constant, so `ctx.memories[K]` is loaded once). | §3: the index need not exist at runtime. | No per-access index work. |
| MMD2 | **Keep one flat `memories` array and a `memoryCount`.** | §3: representation free. | Minimal change from single-memory code. |
| MMD3 | **Add a single-memory fast path** (skip the array indirection when `memoryCount == 1`) if it ever shows up in a profile. | §3: representation free. | Fewer loads on the hottest path. |
| MMD4 | **Do not validate `memidx` structurally beyond a cheap bounds rejection** (now done: an out-of-range constant index fails compilation, and the helpers trap). | Consistent with the no-validation stance; the check is trivial and prevents a wild read (MM1). | Safe with valid input, not silently unsafe with invalid input. |
| MMD5 | **Charge every memory against the Store's allocation budget**, so `memory.grow` on an unbounded memory with an absurd operand fails as `-1` instead of allocating ~256 TiB. | §2 only requires a trap for an out-of-bounds *access*; a failed grow is the spec's implementation-limit case. | A hostile or accidental oversized grow cannot take the host down (`docs/TESTING.md` "Allocation guardrails"). |

Guardrail: `memory.copy` must handle **cross-memory** copies (source and
destination `MemIdx` differ) with correct overlap behaviour within each memory.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MM-1 — memidx bounds check** | ✅ `memoryPointerForIndex`/`tablePointerForIndex` reject an index past the module's index space at compile time; the native `memory.*`/`table.*` helpers re-check. | MM1 | Done: an out-of-range `memidx` fails instantiation instead of reading a wild pointer. |
| **MM-2 — Curate** | ✅ Added `memory-multi` to `Test/wast_supported.txt`. | MM2 | Done: the default run covers multi-memory. |
| **MM-3 — Manual cross-memory test** | ✅ `Test/wasm_wat/multi_memory.wat` gained `copy0to1`/`copy1to0`, exercised by `multi-memory: memory.copy moves bytes between two memories`. | guardrail | Done: the test asserts the copied bytes *and* that the source memory is untouched. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-13; `assert_trap` now executed):

| Test / script | Result | Reading |
|---|---|---|
| `spec: memory-multi` | 6 passed / 0 failed / 0 skipped | multi-memory works (and is curated) |
| manual `multi-memory: two linear memories are independent` | pass | independent address spaces |
| manual `multi-memory: memory.copy moves bytes between two memories` | pass | cross-memory copy, both directions |
| `memory` (single memory) | curated, passing | baseline |
| `spec: memory_copy` / `spec: table_copy` | 4386/0/64, 1728/0/0 | bulk ops honour `memidx`; `memory.copy` may name two memories |

## 9. References

- Proposal overview (`WebAssembly/multi-memory`):
  https://github.com/WebAssembly/multi-memory/blob/main/proposals/multi-memory/Overview.md
- Spec — Change History (Release 3.0 "Multiple Memories"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Instructions (`memarg` with memory index):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/MEMORY64.md`](MEMORY64.md), [`docs/BULK_MEMORY.md`](BULK_MEMORY.md),
  [`docs/THREADS.md`](THREADS.md), [`docs/ABI.md`](../ABI.md);
  sources `WasmBase/WasmType.hpp` (`MemArg`, `MemoryType`),
  `WasmBase/WasmVMContext.hpp` (`memories`, `memoryCount`),
  `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmStore.{hpp,cpp}`,
  `WasmBase/WasmModuleInstance.cpp` (`initializeMemories`,
  `applyActiveSegments`), `LibJit/LibjitOpcodeDispatcher.cpp`,
  `Test/wasm_wat/multi_memory.wat`.
