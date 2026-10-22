# Custom Page Sizes — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — the proposal is
**not implemented**. The engine hard-codes a 64 KiB page and does not decode the
page-size flag or the `mempagesize` field.)

Audience: anyone touching `WasmType` (`MemoryType`/`Limits`), `WasmMemory`,
`WasmStore`, `WasmModule` (memory section), `WasmModuleInstance` memory
addressing, or the memory test surface.

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

A memory type gains a page size:

```
memtype ::= idxtype limits share mempagesize
mempagesize ::= u32        ; page size in bytes, default 65536
```

The binary `limits` production adds new flag forms `0x08`–`0x0F`; the low three
bits still select min/max, shared and 32/64-bit, and bit 3 says a page-size
exponent `p` follows:

```
0x08 n       p  => i32, unshared, 2**p
0x09 n m     p  => i32, unshared, 2**p
0x0a n       p  => i32, shared,   2**p
0x0b n m     p  => i32, shared,   2**p
0x0c n       p  => i64, unshared, 2**p
0x0d n m     p  => i64, unshared, 2**p
0x0e n       p  => i64, shared,   2**p
0x0f n m     p  => i64, shared,   2**p
```

The current proposal permits exactly two page sizes: **1 byte** (`p = 0`) and
**64 KiB** (`p = 16`). The limits remain page counts; a memory's byte size is
`pages * pageSize`. Memory type matching requires the exact same page size; there
is no page-size subtyping. `memory.size`/`memory.grow` still return/take page
counts, and the memory's index type remains `i32` or `i64` independently.

The text format spells it as `(pagesize N)` in the memory type or memory
abbreviation. No new instructions are required.

## 3. What the proposal does not demand

- No new instruction or opcode: the page size is static type information.
- No page-size subtyping or coercion; matching is exact.
- No dynamic page-size lookup (`memory.page_size` was rejected in favour of
  static knowledge).
- No page sizes other than 1 and 65536 in the current proposal, although the
  encoding leaves room for powers of two up to 65536.
- No change to the memory index type or to 64-bit memory semantics.
- No requirement to use small host pages internally; an engine may allocate
  larger backing storage as long as the observable byte size/bounds match.
- No JS API requirement; `WebAssembly.Memory` exposure is host-side.

## 4. What we have implemented today

- `WasmBase/WasmType.hpp` defines `Limits { flags, initial, maximum }` and
  `MemoryType { indexType, limits }`. `MemoryType::decode` reads the limits and
  only inspects `flags & 0x04` for the index type; it does not consume a
  page-size exponent and has no `pageSize` field.
- `WasmModule::processMemorySection` decodes `numMemories` followed by
  `MemoryType`s. A new `0x08`–`0x0F` flag would leave the page-size byte in the
  stream; the next memory (or section) would be decoded from the wrong offset.
- `WasmBase/WasmMemory.cpp` hard-codes `WASM_PAGE_SIZE = 0x10000` and uses it
  for initial bytes, maximum bytes and growth. `WasmStore.cpp` and
  `WasmModuleInstance.cpp` use the same constant for memory creation, size and
  grow.
- `WasmRegistryImportResolver.cpp` computes page counts and max pages with
  `0x10000`, so import matching cannot distinguish page sizes.
- `WasmValidator` uses the hard-coded 32-bit page cap (`65536` pages) and the
  memory address type; there is no page-size field in its memory context.
- There is no page-size test. `memory` and `memory64` scripts exercise the
  fixed 64 KiB model only.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| CPS1 | **The page-size flags do not decode.** `MemoryType::decode` ignores bit 3 and does not read the exponent `p`. | `WasmType.hpp` (`Limits`, `MemoryType`); `WasmModule.cpp` (`processMemorySection`). | High |
| CPS2 | **`MemoryType`/`LinearMemory` have no page size.** There is nowhere to store the configured size, so allocation and matching cannot use it. | `WasmType.hpp` (`MemoryType`); `WasmMemory.hpp` (`LinearMemory`). | High |
| CPS3 | **Allocation and growth hard-code 64 KiB.** `StoreOwnedMemory`, `PrivateLinearMemory`, `SharedLinearMemory`, `WasmStore` and `WasmModuleInstance::growMemory` multiply by `0x10000`. | `WasmMemory.cpp`; `WasmStore.cpp`; `WasmModuleInstance.cpp`. | High |
| CPS4 | **Bounds, size and page caps assume 64 KiB.** Validator/memory helpers divide by `0x10000`; a one-byte page needs different page-count/byte math and a different 32-bit cap. | `WasmValidator.cpp`; `WasmModuleInstance.cpp`; `WasmRegistryImportResolver.cpp`. | High |
| CPS5 | **Import matching ignores page size.** It compares limits and `is64` but not the page size required by the proposal. | `WasmImport.hpp`; `WasmRegistryImportResolver.cpp` (`resolveMemory`). | Medium |
| CPS6 | **No focused coverage.** No custom-page-size script or manual fixture exists. | `Test/wast_supported.txt`; `Test/main.cpp`. | Medium |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| CPSD1 | **Implement only the two current page sizes (1 and 65536).** | §3: the proposal permits only those two; the encoding leaves future powers of two open. | A small, testable subset without a general power-of-two allocator. |
| CPSD2 | **Keep limits as page counts and compute bytes at the memory boundary.** | §2: limits stay in pages; only byte size/bounds change. | `memory.size`/`grow` and the existing index types remain untouched. |
| CPSD3 | **Match page sizes exactly, with no subtyping.** | §2: the proposal explicitly forbids a page-size subtype relation. | Simple, spec-aligned import matching. |
| CPSD4 | **Use a larger host allocation/backing store if needed.** | §3: only observable byte size and bounds matter. | Portability on hosts whose granularity is larger than one byte. |
| CPSD5 | **Treat the page size as static metadata, not a runtime register or instruction.** | §3: no `memory.page_size` instruction exists. | No extra addressing path or per-access branch. |

Guardrail: a one-byte page memory must still trap exactly at its byte bounds;
rounding the byte size to a host page must never make an out-of-bounds access
succeed. `memory.grow` must commit exactly the requested number of pages at the
configured page size, not 64 KiB.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CPS-1 — Type and parser** | Add `pageSize` to `MemoryType`, decode `0x08`–`0x0F` and consume `p`; validate `p ∈ {0,16}`. | CPS1, CPS2 | A hand-built memory section with both page sizes round-trips without desynchronising. |
| **CPS-2 — Memory/store math** | Thread the page size through `WASM_PAGE_SIZE` uses in allocation, growth, max/size and shared reservations. | CPS3 | A one-byte-page memory of N pages has exactly N bytes and grows by exactly N bytes. |
| **CPS-3 — Matching and validation** | Add page size to memory import matching, byte bounds and the 32-bit page cap. | CPS4, CPS5 | A mismatched page size is rejected; a matching one links; bounds and caps use the configured size. |
| **CPS-4 — Tests** | Add a manual fixture with 1-byte and 64 KiB memories, grow, bounds and import mismatch. | CPS6 | The fixture distinguishes page counts from bytes and catches a hard-coded 64 KiB path. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory` | 65 | 0 | 25 | fixed 64 KiB model green; no page-size field |
| `memory64` | 55 | 0 | 14 | 64-bit index type green; still 64 KiB pages |
| `memory-multi` | 6 | 0 | 0 | multiple memories green; no page-size coverage |
| custom-page-sizes | — | — | — | no script in the snapshot and no implementation |

Reading: the memory substrate is solid but assumes one page size. CPS-4 is the
first test that would expose a hard-coded `0x10000`.

## 9. References

- Custom Page Sizes:
  https://github.com/WebAssembly/custom-page-sizes/blob/main/proposals/custom-page-sizes/Overview.md
- In-repo: [`MEMORY64.md`](MEMORY64.md), [`MULTI_MEMORY.md`](MULTI_MEMORY.md),
  [`THREADS.md`](THREADS.md), [`MEMORY_CONTROL.md`](MEMORY_CONTROL.md),
  [`../SAFETY.md`](../SAFETY.md), [`../ABI.md`](../ABI.md); sources
  `WasmBase/WasmType.hpp` (`Limits`, `MemoryType`),
  `WasmBase/WasmModule.cpp` (`processMemorySection`),
  `WasmBase/WasmMemory.hpp`/`.cpp` (`LinearMemory`, `WASM_PAGE_SIZE`),
  `WasmBase/WasmStore.cpp`, `WasmBase/WasmModuleInstance.cpp`,
  `WasmBase/WasmRegistryImportResolver.cpp`, `WasmBase/WasmValidator.cpp`.

