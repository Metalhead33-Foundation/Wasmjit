# Memory64 — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — M64-1…M64-3
implemented: 64-bit `size`/`grow` plumbing, the unsigned address widening, and a
manual 64-bit test).
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
- The JIT computes addresses in **64-bit** and interprets the operand as the
  memory's **unsigned** index: an i32 address is widened through `uint32_t` to
  `uint64_t` (zero-extend), an i64 address is used as-is. The bound check is
  overflow-safe — it requires `byteOff >= index` and `endOff >= byteOff`
  (neither addition wrapped) *and* `endOff <= memorySize`. Sign-extending the
  i32 straight to 64 bits instead let `address + offset + width` wrap and slip
  past the check (`address.wast`'s `8u_good3(i32.const -1)`), and the same
  wrap affected `memory_trap64`.
- **`memory.size` / `memory.grow` and `table.size` / `table.grow` are 64-bit.**
  The grow helpers take an unsigned 64-bit delta and return an all-ones
  sentinel on failure (readable as `-1` in either width); the result is pushed
  as an i64 for a 64-bit memory/table and an i32 otherwise
  (`memoryIndexTypeForIndex`, `tableIs64Bit`). `ModuleInstance::growMemory` /
  `growTable` and the `Store` helpers are `uint64_t` throughout (M64-1, M4).
- **Index-space caps are enforced**: a 32-bit memory is clamped to 65536 pages
  (4 GiB) and a 32-bit table to 2³²−1 entries at instantiation; everything is
  additionally charged against the Store's allocation budget
  (`docs/TESTING.md` "Allocation guardrails").
- `memory.size`/`memory.grow`/`memory.copy`/`memory.fill`/`memory.init` and the
  `table*` equivalents carry their 64-bit operands through the same helpers.
- The harness's `spectest` host also exports `table64` / `memory64`, so
  `table64.wast`'s `(import "spectest" "table64" ...)` resolves.
- **Evidence:** the 64-bit scripts are in `Test/wast_supported.txt` and green
  (`address64` 242/0, `align64` 74/0, `binary_leb128_64` 1/0, `bulk64` 70/0,
  `call_indirect64` 2/0, `float_memory64` 90/0, `memory64` 54/0,
  `memory64-imports` 48/0, `memory_grow64` 49/0, `memory_redundancy64` 8/0,
  `memory_trap64` 172/0, `table64` 11/0), plus the manual test
  `memory64: 64-bit memory and table size/grow are 64-bit`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~M1~~ | ~~Size/grow plumbing is 32-bit.~~ **Done (M64-1):** `memory.size`/`grow`, `table.size`/`grow`, `ModuleInstance::growMemory`/`growTable` and `wasm_{memory,table}_grow_impl` are all 64-bit now; a 64-bit memory/table pushes an i64. | `LibjitOpcodeDispatcher.cpp`, `WasmModuleInstance.cpp` | Closed |
| ~~M4~~ | ~~`table64` width is suspicious.~~ **Done (M64-1):** `tableIs64Bit` (limits flag bit 2) selects an i64 result/delta; `table.size`/`grow` on a 64-bit table are exercised by `table64.wast` and the manual test. | `dispatchTableSize`, `dispatchTableGrow` | Closed |
| M2 | **No index-type validation.** Nothing rejects an operand whose type disagrees with the memory's/table's declared index type at *validation* time; the address is now widened according to the *memory's* declared type, so the instruction behaves as its memory dictates rather than being rejected. This is the usual no-validating-front-end stance. | no validating front-end | Low (deliberate) |
| M3 | **Unrepresentable maxima become "unbounded".** A `memory64` declaration whose max does not fit a byte count is treated as `UINT64_MAX`; growth then only fails at host allocation — which is now the Store's allocation budget, so it fails as `-1` instead of trying. | `StoreOwnedMemory` constructor, `WasmStore` | Closed in effect (Low) |
| ~~M5~~ | ~~No manual 64-bit test.~~ **Done (M64-3):** `Test/wasm_wat/memory64_index.wat` + `memory64: 64-bit memory and table size/grow are 64-bit`, which also asserts that a delta beyond 2³² pages is rejected as `-1` rather than truncated. | `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| M64D1 | **Keep all internal bookkeeping in `uint64_t`** (`LinearMemory::memorySize`, `Limits::initial/maximum`) and compute addresses in `jit_type_ulong`. | §3: the representation is free; this avoids wraparound in effective addresses. | Correct 64-bit addressing without special cases. |
| M64D2 | **Cap a memory/table at the host allocation budget and let `grow` fail beyond it.** | §3: addressing 2⁶⁴ bytes is explicitly not required; the spec's implementation-limitation rules cover it. | A single, portable failure mode that also protects the host (`docs/TESTING.md` "Allocation guardrails"). |
| M64D3 | **Treat an unrepresentable maximum as "unbounded" and let the budget fail the grow.** | §3; the budget is the concrete host limit. | No oversized integer math at declaration time, no giant allocation attempt. |
| M64D4 | **Keep the 32-bit result plumbing for now, documented.** The truncation is unobservable for any memory a host can actually back. | §3; only reachable with a >2³²-page memory. | Avoids a JIT ABI change until needed. |
| M64D5 | **Do not implement the BigInt JS API.** | §3: web-embedding only. | Scope control. |

Guardrail: effective-address computation must stay in 64-bit even for a 32-bit
memory (zero-extend the `i32`), so that `memoryBase + offset` cannot wrap
32-bit arithmetic.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **M64-1 — Widen size/grow** | ✅ Memory/table `size`/`grow` helpers are 64-bit and push `i64` for 64-bit memories/tables; index-space caps are enforced at instantiation; every byte is charged against the Store budget. | M1, M4 | Done: a 64-bit memory/table reports and accepts 64-bit values, and an over-2³²-page delta is rejected as `-1` instead of truncated. |
| **M64-2 — Index-type check** | ◑ Not implemented as validation; instead the address is *widened according to the memory's declared index type* (`memoryIndexTypeForIndex`), and a mismatch is left to a validating front-end that does not exist. | M2 | Partially: the behaviour is well-defined; only the rejection is missing. |
| **M64-3 — Manual test** | ✅ `Test/wasm_wat/memory64_index.wat` + `memory64: 64-bit memory and table size/grow are 64-bit`. | M5 | Done: store/load through an i64 address, `memory.size`/`grow`, `table.size`/`grow`, and the huge-delta rejection are all asserted. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-13; `assert_trap` now executed). The
following 64-bit scripts are all in `Test/wast_supported.txt` and part of the
default green run:

| Script | passed / failed / skipped | Reading |
|---|---:|---|
| `address64` | 242 / 0 / 0 | 64-bit address arithmetic, including the unsigned-widening cases |
| `align64` | 74 / 0 / 83 | alignment |
| `binary_leb128_64` | 1 / 0 / 1 | 64-bit LEB encoding |
| `bulk64` | 70 / 0 / 0 | bulk ops on 64-bit memories/tables |
| `call_indirect64` | 2 / 0 / 0 | 64-bit table index |
| `float_memory64` | 90 / 0 / 0 | f32/f64 access through i64 addresses |
| `memory64` | 54 / 0 / 15 | core 64-bit memory semantics |
| `memory64-imports` | 48 / 0 / 30 | importing a 64-bit memory |
| `memory_grow64` | 49 / 0 / 0 | `memory.size`/`grow` return and accept i64 |
| `memory_redundancy64` | 8 / 0 / 0 | redundant loads/stores |
| `memory_trap64` | 172 / 0 / 0 | out-of-bounds i64 accesses **do** trap (M64-1/M1) |
| `table64` | 11 / 0 / 3 | 64-bit table size/grow, including `spectest.table64` |

`memory_grow64` and `memory_trap64` only became meaningful once `assert_trap`
ran: before that, every trap case was skipped. See `docs/TESTING.md`.

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
  (shared-memory cap), [`docs/ABI.md`](../ABI.md);
  sources `WasmBase/WasmType.hpp` (`Limits`, `MemoryType`),
  `WasmBase/WasmMemory.{hpp,cpp}`, `WasmBase/WasmModuleInstance.cpp`,
  `LibJit/LibjitOpcodeDispatcher.cpp` (`effectiveMemoryAddress`,
  `dispatchMemorySize`, `dispatchMemoryGrow`, `dispatchTableSize`),
  `Test/wast_supported.txt`.
