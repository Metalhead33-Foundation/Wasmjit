# Memory64 — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmType` (`Limits`, `MemoryType`, `TableType`),
`WasmMemory`, `WasmModuleInstance`, `LibjitOpcodeDispatcher`, or the test
harness.

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

Memory64 is a **Phase 5** proposal, integrated into WebAssembly **3.0**.

- **64-bit memory index type.** A memory (or table) declaration can choose an
  `i64` index type; in the limits flags, bit 2 (`0x04`) selects it. The
  declaration may mix memories of either index type in one module.
- **Address operands become `i64`** for a 64-bit memory: loads/stores use an
  `i64` effective-address operand, `memory.size` returns `i64`, `memory.grow`
  takes an `i64` delta and returns the previous `i64` size.
- **Bulk operations** (`memory.copy`/`fill`/`init`) use `i64` offsets and
  lengths for a 64-bit memory. `table64` likewise makes `table.*`,
  `call_indirect` and element segments use `i64` indices.
- **Limits scale:** a 64-bit memory's minimum/maximum are page counts that can
  exceed 2³², with a byte size up to the host address space.
- **JS API** exposes `i64` values as `BigInt` for `WebAssembly.Memory.grow`.

## 3. What the proposal does not demand

- **No requirement to actually address 2⁶⁴ bytes.** The engine may cap a memory
  at the host address space and fail `grow` beyond it; the spec's
  implementation-limitation rules apply.
- **No representation requirement.** 64-bit bookkeeping fields and 64-bit
  pointer arithmetic are the natural choice but not mandated.
- **No requirement to keep `i64` addresses distinct from `i32` ones at
  runtime**; the type only has to be honoured where it is observable.
- **No change to page size** (still 64 KiB) or alignment rules.
- **No trapping requirement for out-of-range `grow`** beyond the normal "return
  −1 / fail" rule.

## 4. What we have implemented today

- `MemoryIndexType { I32, I64 }` is parsed from `Limits::flags` bit 2
  (`MemoryType::decode`), and `Limits` stores `uint64_t initial` / `optional
  uint64_t maximum`.
- `LinearMemory` uses `uint64_t memorySize` / `memoryMax`, so the runtime
  never truncates to 32 bits.
- `StoreOwnedMemory`/`SharedLinearMemory` do their arithmetic in `uint64_t`
  and guard against overflow; an unrepresentable maximum is treated as
  unbounded, and a shared memory is capped at 4 GiB.
- The JIT computes addresses in **64-bit**: the memory paths convert the
  effective address to `jit_type_ulong` and compare against the 64-bit
  `memorySize` before the access, so a 32-bit memory zero-extends and a 64-bit
  memory is used as-is.
- `memory.size`/`memory.grow`/`memory.copy`/`memory.fill`/`memory.init` and the
  `table*` equivalents carry their 64-bit operands through the same helpers.
- **Evidence:** a large part of the 64-bit suite is in
  `Test/wast_supported.txt` and green — `address64`, `align64`,
  `binary_leb128_64`, `bulk64`, `call_indirect64`, `float_memory64`,
  `memory64`, `memory64-imports`, `memory_grow64`, `memory_redundancy64`,
  `memory_trap64`, `table64`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| M1 | **Size/grow plumbing is 32-bit.** `dispatchMemorySize` converts the page count to `jit_type_int`; `wasm_memory_grow_impl` and the table helpers (`wasm_table_grow_impl`, `wasm_table_fill_impl`, `wasm_table_copy_impl`) take/return `int32_t`. For a 64-bit memory/table these truncate beyond 2³² pages (or an `i64` delta beyond 2³¹). The curated 64-bit scripts pass only because they stay small. | `LibjitOpcodeDispatcher.cpp` memory/table paths | Medium (latent) |
| M2 | **No index-type validation.** Nothing checks that a load/store address operand matches the memory's declared `MemoryIndexType`, nor that `table.*` operands match a 64-bit table's type. | no validating front-end | Medium |
| M3 | **Unrepresentable maxima become "unbounded".** A `memory64` declaration whose max does not fit a byte count is treated as `UINT64_MAX`; growth then only fails at host allocation. | `StoreOwnedMemory` constructor | Low |
| M4 | **`table64` width is suspicious.** `dispatchTableSize`/`dispatchTableGrow` push `jit_type_int` results, yet a 64-bit table's `table.size`/`grow` must be `i64`. `table64.wast` is curated and green, so the tests likely stay in range — worth a targeted `>2³²` check. | `dispatchTableSize`, `dispatchTableGrow` | Medium (latent) |
| M5 | **No manual 64-bit test.** Nothing in `Test/wasm_wat` declares a 64-bit memory or table. | `Test/main.cpp` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| M64D1 | **Keep all internal bookkeeping in `uint64_t`** (`LinearMemory::memorySize`, `Limits::initial/maximum`) and compute addresses in `jit_type_ulong`. | §3: the representation is free; this avoids wraparound in effective addresses. | Correct 64-bit addressing without special cases. |
| M64D2 | **Cap a memory at the host address space and let `grow` fail beyond it.** | §3: addressing 2⁶⁴ bytes is explicitly not required; the spec's implementation-limitation rules cover it. | A single, portable failure mode. |
| M64D3 | **Treat an unrepresentable maximum as "unbounded" and let allocation fail.** | §3; already the behaviour. | No oversized integer math at declaration time. |
| M64D4 | **Keep the 32-bit result plumbing for now, documented.** The truncation is unobservable for any memory a host can actually back. | §3; only reachable with a >2³²-page memory. | Avoids a JIT ABI change until needed. |
| M64D5 | **Do not implement the BigInt JS API.** | §3: web-embedding only. | Scope control. |

Guardrail: effective-address computation must stay in 64-bit even for a 32-bit
memory (zero-extend the `i32`), so that `memoryBase + offset` cannot wrap
32-bit arithmetic.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **M64-1 — Widen size/grow** | Make the memory/table `size`/`grow` helpers 64-bit and push `i64` for 64-bit memories/tables. | M1, M4 | A hand-written 64-bit memory reports a page count > 2³² if the host allows it. |
| **M64-2 — Index-type check** | Reject an address operand whose type disagrees with the memory/table index type (cheap, decodes once). | M2 | An `i64` address into an `i32` memory is rejected. |
| **M64-3 — Manual test** | Add a `memory64`/`table64` module to `Test/wasm_wat` + a manual test. | M5 | The test exercises a 64-bit memory and table end to end. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04). The following 64-bit scripts
are all in `Test/wast_supported.txt` and part of the default green run:

`address64`, `align64`, `binary_leb128_64`, `bulk64`, `call_indirect64`,
`float_memory64`, `memory64`, `memory64-imports`, `memory_grow64`,
`memory_redundancy64`, `memory_trap64`, `table64`.

Their passing is limited by the 32-bit plumbing in M1/M4: none of them can
allocate or index more than a 32-bit-sized object, so the truncation never
becomes observable. A `>2³²`-page test (M64-1) is the missing evidence.

## 9. References

- Proposal overview (`WebAssembly/memory64`):
  https://github.com/WebAssembly/memory64/blob/main/proposals/memory64/Overview.md
- Spec — Change History (Release 3.0 "Memory64"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Types (`limits`, 64-bit index flag) and Instructions:
  https://webassembly.github.io/spec/core/syntax/types.html
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/MULTI_MEMORY.md`](MULTI_MEMORY.md),
  [`docs/BULK_MEMORY.md`](BULK_MEMORY.md), [`docs/THREADS.md`](THREADS.md)
  (shared-memory cap), [`docs/ABI.md`](ABI.md);
  sources `WasmBase/WasmType.hpp` (`Limits`, `MemoryType`),
  `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmModuleInstance.cpp`,
  `LibJit/LibjitOpcodeDispatcher.cpp` (`effectiveMemoryAddress`,
  `dispatchMemorySize`, `dispatchMemoryGrow`, `dispatchTableSize`),
  `Test/wast_supported.txt`.
