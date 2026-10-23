# Bulk Memory Operations — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — BM-1…BM-6
implemented: recoverable traps, dropped/empty semantics, the `data`/`elem`
fixes, and the `data count` requirement).
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
- **Traps, not aborts (BM-2).** Every bounds failure in `memoryInit`,
  `tableInit`, `bufferInitFromData`, `bufferInitFromElems` and the native
  helpers raises a real Wasm trap via `WASM::trap` — a pending status that
  unwinds by returning and becomes `WASM::Trap` at the host boundary
  (`WasmBase/WasmTrap.hpp`, docs/TRAP_CLEANUP.md). The JIT's own bounds checks
  (`effectiveMemoryAddress`, `checkedTableIndex`), the memory/table grow paths
  and an uninitialised table slot (`call_indirect`) go through the same sink.
  The harness consequently *executes* `assert_trap` instead of skipping it.
- **Dropped/empty semantics (BM-1/BM-3).** A dropped — including implicitly
  dropped — segment behaves as *empty*: a zero-length
  `memory.init`/`table.init`/`bufferInitFrom*` on it is a no-op, anything longer
  traps. Active and declarative segments are marked dropped after
  instantiation; a dropped segment is never re-evaluated.
- **Element-segment references are evaluated once (BM-5, GC.md G4).** An
  expression-form segment is evaluated at instantiation into
  `ModuleInstanceInternals::elementSegmentRefs`; `table.init` and
  `array.new_elem` copy *those* references. Re-evaluating the expression per
  use broke `ref.eq` between two `table.init`s of the same segment
  (`table_init.wast`).
- **Extended-constant segment offsets.** `evalConstantExpr` implements the six
  Wasm 3.0 integer add/sub/mul forms, so `(data (i32.add …))` and
  `(elem (i32.mul …))` offsets evaluate (`data.wast`).
- **Imported globals are resolved** at instantiation, so a segment offset that
  reads `spectest.global_i32` (or an imported `funcref` global) is correct.
- `memory.copy` and `memory.fill`, `table.copy` and `table.fill` are JIT
  calls into native helpers, and are multi-memory / multi-table aware; each
  helper re-checks its `memidx`/`tableidx` before indexing.
- **Test evidence**: `memory_copy` 4386/0, `memory_fill` 36/0,
  `table_copy` 1728/0, `memory_init` 183/0, `data` 34/0, `elem` 108/0,
  `table_init` 725/0, `bulk64` 70/0 (see §8).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~BM1~~ | ~~`memory.init` aborts before any command passes.~~ **Done (BM-2/BM-3):** the abort was the dropped-segment branch plus `std::abort()` on out-of-range; dropped segments are now empty, and real failures trap. `spec: memory_init` → **183 passed / 0 failed**. | `spec: memory_init` | Closed |
| ~~BM2~~ | ~~Out-of-bounds traps are process-fatal.~~ **Done (BM-2):** `memoryInit`, `bufferInitFromData`, `tableInit`, `bufferInitFromElems` and the copy/fill helpers route through `WASM::trap`, so the harness runs `assert_trap` and an embedder can recover in-process. | `WasmTrap.hpp`, `WasmModuleInstance.cpp`, `LibjitOpcodeDispatcher.cpp` | Closed |
| ~~BM3~~ | ~~Dropped-segment zero-length access aborts.~~ **Done (BM-1/BM-3):** a dropped segment is empty — zero-length is a no-op, longer traps. | `memoryInit`, `tableInit`, `bufferInitFrom*` | Closed |
| ~~BM4~~ | ~~`data.wast`: 30 passed / 4 failed.~~ **Done:** the four failures were extended-constant offsets (`i32.add/sub/mul`); `evalConstantExpr` now implements them (EXTENDED_CONST.md EC-1) and `spec: data` → **34 passed / 0 failed**. | `spec: data` | Closed |
| ~~BM5~~ | ~~`elem.wast` produces no report.~~ **Done:** it was a null `call_indirect` slot once `table.init` reached a table slot left null by an unresolved imported global, plus the per-use re-evaluation of expression-form segments. Imported globals are resolved and segment references are cached; `spec: elem` → **108 passed / 0 failed**. | `spec: elem` | Closed |
| ~~BM6~~ | ~~**No `data count` section handling.**~~ **Done:** the validator rejects `memory.init`/`data.drop` when `hasDataCount` is false, and checks the count against the data section. | `WasmValidator.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| BMD1 | **`memmove`/`memset`/`memcpy` for copy/fill/init.** | §3: strategy is free; `memmove` already gives overlap semantics. | Correct and fast; no JIT loops. |
| BMD2 | **Drop = a single `bool`; never free anything.** | §3: dropping has no observable side effect beyond emptiness. | Trivial drop, no ownership churn. |
| BMD3 | **Keep passive segments pointing into the module's bytes; never copy them at instantiation.** | §3: storage is free; the `Module` outlives its instances. | Zero-copy, less start-up work. |
| BMD4 | **Copy active segments eagerly at instantiation (already done).** | §2/§3: active segments must be in place before the start function runs. | Simple, and matches the data-init rules. |
| BMD5 | ~~Do not validate `data count`; require valid input.~~ **Superseded — `Module::validate()` enforces it (BM6).** `memory.init`/`data.drop` without a data-count section are rejected, and the count must match the data section ([`../VALIDATION.md`](../VALIDATION.md)). | The old stance was "trust the producer" (`docs/proposals/GC.md` D6); a validating front end exists now. | One consistency check, no extra pass. |
| BMD6 | **Route every bounds failure through the single `WASM::trap` sink** (implemented). | §2 requires a trap; §3 leaves the mechanism to the embedder. | Recoverable, testable traps — closes BM2. |
| BMD7 | **Charge every memory/table byte against the Store's allocation budget**, so a bulk path that grows a memory cannot balloon the host either. | §3: the host limit is the engine's to choose; a failed grow is a spec-sanctioned outcome. | One guard for every allocation (`docs/TESTING.md` "Allocation guardrails"). |

Guardrails: `memory.copy` must remain `memmove`-correct for overlapping ranges;
`table.copy` must handle overlapping table ranges and shared tables from the
Store; `memory.init` must respect the multi-memory `memidx`.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **BM-1 — Dropped/empty semantics** | ✅ Zero-length `*.init`/`bufferInitFrom*` on a dropped or empty segment is a no-op; a longer one traps. Active/declarative segments are dropped at instantiation. | BM3 | Done: `memory_init` and `table_init` pass their dropped-segment cases. |
| **BM-2 — Trap sink** | ✅ `WASM::trap` (a pending status that the boundary turns into `WASM::Trap`) replaces the per-call-site `std::abort()`s; the harness runs `assert_trap`. | BM2, BM1 | Done: no `abort` on the memory/table/bulk runtime path; `assert_trap` is executed. |
| **BM-3 — Triage `memory_init` / `data` / `elem`** | ✅ Fixed the dropped-segment abort, the four extended-const `data` offsets, and `elem`'s null slot / segment re-evaluation. | BM1, BM4, BM5 | Done: `memory_init` 183/0, `data` 34/0, `elem` 108/0. |
| **BM-4 — `data count`** | ✅ Done: `memory.init`/`data.drop` require a data-count section; the count must match the data section. | BM6 | Done. |

Guardrail: run `./WasmJit '~[spec]'` and the bulk scripts (`~[spec]` plus
`bulk64`, which is curated) after each change.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-13; `assert_trap` now executed):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory_copy` | 4386 | 0 | 64 | works |
| `memory_fill` | 36 | 0 | 64 | works |
| `table_copy` | 1728 | 0 | 0 | works (`table_copy` is curated) |
| `memory_init` | 183 | 0 | 67 | was signal 6 (BM1); dropped/empty semantics fixed |
| `table_init` | 725 | 0 | 67 | expression-form segment references are now evaluated once (GC.md G4) |
| `data` | 34 | 0 | 34 | the four failures were extended-const offsets (BM4) |
| `elem` | 108 | 0 | 46 | was "no report" (BM5) |
| `bulk64` | 70 | 0 | 0 | curated |

`bulk64` and `table_copy` are in `Test/wast_supported.txt`; `memory-multi` joined
them (see [`MULTI_MEMORY.md`](MULTI_MEMORY.md) MM-2), so `table_copy`'s
`assert_trap` cases now run rather than being skipped.

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
