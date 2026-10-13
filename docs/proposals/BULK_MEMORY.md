# Bulk Memory Operations — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmModule`, `WasmModuleInstance`,
`WasmOpcodeDispatcher`, `LibjitOpcodeDispatcher`, or the test harness.

This note follows the same split as [`GC.md`](GC.md),
[`SIMD.md`](SIMD.md) and [`THREADS.md`](THREADS.md). It contains **no new
code**.

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

Bulk Memory Operations is a **Phase 5** proposal, integrated into WebAssembly
**2.0**. It generalises segment handling:

- **Segment modes.** Data and element segments are no longer only active. A
  segment may be
  - *active* (mode 0/2 for data, 0/4 for elem) — copied at instantiation,
  - *passive* (mode 1/5) — used only by `*.init`,
  - *declarative* (mode 3, elements only) — declares functions for `ref.func`,
    never placed in a table.
  Active data segments may target a non-zero memory (multi-memory) and active
  element segments a non-zero table.
- **New instructions**
  - `memory.init dataidx memidx` — copy a range of a passive data segment into
    linear memory;
  - `data.drop dataidx` — mark a passive data segment empty;
  - `memory.copy dstmem srcmem` — overlapping copy, like `memmove`;
  - `memory.fill memidx` — byte-fill, like `memset`;
  - `table.init elemidx tableidx`, `elem.drop elemidx`,
    `table.copy dsttable srctable`, `table.fill tableidx`.
- **Dropping.** After `data.drop`/`elem.drop` a segment behaves as if empty:
  a later `*.init` with a non-zero length traps out of bounds; a zero-length
  `*.init`, or `array.new_data`/`array.new_elem`, is fine.
- **Traps.** Every `*.init`/`copy`/`fill` is bounds-checked against both the
  source segment and the destination memory/table, and traps on overflow.
  `memory.init` without a `data count` section is invalid.
- **Data count section.** An optional section that declares the number of data
  segments so `memory.init`/`data.drop` can be validated in a single pass.

## 3. What the proposal does not demand

- **No implementation strategy.** `memcpy`/`memmove`/`memset`, hand loops, or
  a JIT-emitted sequence are all fine.
- **No alignment requirement.** Byte-granular, arbitrary alignment.
- **Segment storage is free.** Active segments may be eagerly copied at
  instantiation; passive segments may be kept, copied lazily, or represented
  as a pointer into the module's bytes.
- **`data.drop` has no observable side effect** other than making later
  `memory.init`/`array.new_data` behave as empty; it need not free anything.
- **No requirement to validate eagerly**; a missing `data count` section may
  be rejected at validation time.
- **No ordering or atomicity requirement** for non-shared memories.

## 4. What we have implemented today

- **Decode** for the whole family: `MiscOpcode::{MemoryInit, DataDrop,
  MemoryCopy, MemoryFill, TableInit, ElemDrop, TableCopy, TableFill}`
  (`WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp`), with the multi-memory
  `memidx` immediates.
- **Runtime**:
  - active data segments are copied by `applyActiveSegments` (bounds-checked
    against `memorySize`);
  - active element segments are written through `applyActiveSegments` /
    `bufferInitFromElems`;
  - passive segments are kept in `Module::dataSegments` /
    `Module::elementSegments` and read by `memoryInit`,
    `bufferInitFromData`, `bufferInitFromElems`, `tableInit`;
  - dropping is a `std::vector<bool>` flag per segment
    (`dataSegmentDropped`, `elementSegmentDropped`);
  - `memory.copy` and `memory.fill`, `table.copy` and `table.fill` are JIT
    calls into native helpers, and are multi-memory / multi-table aware.
- **Test evidence**: `memory_copy` 4368/0, `memory_fill` 30/0,
  `table_copy` 522/0 (see §8).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| BM1 | **`memory.init` aborts** before any command passes. `memory_init.wast` → signal 6. Most likely the dropped-segment or bounds path in `ModuleInstance::memoryInit` (`std::abort()`), which is fatal even for a legitimate trap. | `spec: memory_init` | High |
| BM2 | **Out-of-bounds traps are process-fatal.** `memoryInit`, `bufferInitFromData`, `tableInit`, `bufferInitFromElems` call `std::abort()` instead of a recoverable trap, so the `assert_trap` half of every bulk script is untestable and an embedder cannot recover. | `WasmModuleInstance.cpp` | High |
| BM3 | **Dropped-segment zero-length access aborts.** The spec makes a dropped segment behave as empty; a zero-length `memory.init`/`table.init` (or `array.new_*`) must be a no-op, but the code aborts unconditionally when the flag is set. | `memoryInit`, `tableInit`, `bufferInitFromElems` | Medium |
| BM4 | **`data.wast`: 30 passed / 4 failed.** Some data-segment edge case is wrong (offsets, drop, or the data-count path). | `spec: data` | Medium |
| BM5 | **`elem.wast` produces no report** (uncaught exception on load/instantiate). | `spec: elem` | High |
| BM6 | **No `data count` section handling.** `hasDataCount` is parsed but nothing validates it; `memory.init` is accepted without it. | `WasmModule` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| BMD1 | **`memmove`/`memset`/`memcpy` for copy/fill/init.** | §3: strategy is free; `memmove` already gives overlap semantics. | Correct and fast; no JIT loops. |
| BMD2 | **Drop = a single `bool`; never free anything.** | §3: dropping has no observable side effect beyond emptiness. | Trivial drop, no ownership churn. |
| BMD3 | **Keep passive segments pointing into the module's bytes; never copy them at instantiation.** | §3: storage is free; the `Module` outlives its instances. | Zero-copy, less start-up work. |
| BMD4 | **Copy active segments eagerly at instantiation (already done).** | §2/§3: active segments must be in place before the start function runs. | Simple, and matches the data-init rules. |
| BMD5 | **Do not validate `data count`; require valid input.** | Consistent with the "no validating front-end" stance (`docs/GC.md` §6, D6). | No extra pass. |
| BMD6 | **Route every bounds failure through the single `trap()` sink** (once it exists), rather than `abort` per call site. | §2 requires a trap; §3 leaves the mechanism to the embedder. | Recoverable traps, testable `assert_trap`, closes BM2. |

Guardrails: `memory.copy` must remain `memmove`-correct for overlapping ranges;
`table.copy` must handle overlapping table ranges and shared tables from the
Store; `memory.init` must respect the multi-memory `memidx`.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **BM-1 — Dropped/empty semantics** | Make zero-length `*.init` on a dropped/empty segment a no-op; only a non-zero length traps. | BM3 | `array_new_elem` and a hand-written drop-then-zero-init module pass. |
| **BM-2 — Trap sink** | Replace the `std::abort()` bounds checks in the bulk helpers with the shared `trap()` primitive. | BM2 | `assert_trap` cases can run; no `abort` on the normal path. |
| **BM-3 — Triage `memory_init` / `data` / `elem`** | Reproduce the `memory_init` abort, the 4 `data` mismatches, and the `elem` no-report. | BM1, BM4, BM5 | All three scripts pass their non-trap assertions. |
| **BM-4 — `data count`** | Validate `memory.init`/`data.drop` against `hasDataCount`. | BM6 | A module using `memory.init` without a data-count section is rejected. |

Guardrail: run `./WasmJit '~[spec]'` (the curated set contains `bulk64`).

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory_copy` | 4368 | 0 | 82 | works |
| `memory_fill` | 30 | 0 | 70 | works |
| `table_copy` | 522 | 0 | 1206 | works (`table_copy` is curated) |
| `memory_init` | — | abort | — | BM1 (signal 6) |
| `data` | 30 | 4 | 34 | BM4 |
| `elem` | — | no report | — | BM5 |

`bulk64` is in `Test/wast_supported.txt` and is part of the default run.

## 9. References

- Proposal overview (`WebAssembly/bulk-memory-operations`):
  https://github.com/WebAssembly/bulk-memory-operations/blob/master/proposals/bulk-memory-operations/Overview.md
- Spec — Change History (Release 2.0 "Bulk Memory Operations"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Instructions (memory/table instructions):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/GC.md`](GC.md) §5.5 (R3), [`docs/MULTI_MEMORY.md`](MULTI_MEMORY.md),
  [`docs/MEMORY64.md`](MEMORY64.md); sources
  `WasmBase/WasmModule.{hpp,cpp}` (`ElementSegment`, `DataSegment`,
  `hasDataCount`), `WasmBase/WasmModuleInstance.cpp` (`applyActiveSegments`,
  `memoryInit`, `dataDrop`, `tableInit`, `elemDrop`, `bufferInitFrom*`),
  `WasmBase/WasmOpcode.hpp` (`MiscOpcode`), `LibJit/LibjitOpcodeDispatcher.cpp`.
