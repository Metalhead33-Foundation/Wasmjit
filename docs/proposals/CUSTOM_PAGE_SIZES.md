# Custom Page Sizes — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_custom_page_sizes}

Status: design note / implementation reference (revision 2 — CPS-1…CPS-4 are
implemented: both page sizes decode, allocation/growth/matching use the per
memory size, and the byte-exact fixtures pass. Only the two page sizes the
proposal defines are accepted.)

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
  `MemoryType { indexType, limits, pageSize }`. `MemoryType::decode` reads the
  limits, selects the index type from `flags & 0x04`, and — when flag bit 3 is
  set — consumes the page-size exponent `p`. Only `p = 0` (1 byte) and
  `p = 16` (64 KiB) are accepted; any other exponent is a named load error.
- `WasmModule::processMemorySection` stays in sync with the stream because the
  exponent is consumed by the type decoder, not left for the next memory.
- `LinearMemory` carries `pageSize`, and `StoreOwnedMemory`,
  `PrivateLinearMemory`, `SharedLinearMemory`, `WasmStore::createLinearMemory`
  and `Store::growMemory` do all byte math with it. `memory.size` loads
  `offsetof(LinearMemory, pageSize)` in LibJIT, and
  `ModuleInstance::growMemory` divides by it.
- `WasmRegistryImportResolver` requires `memory.pageSize == type.pageSize`
  exactly (no page-size subtyping) and computes current/max pages with that
  size.
- `WasmValidator::checkLimits` derives the page cap from the byte budget
  (2^32 bytes for memory32, 2^64 for memory64), rejects unknown limits flag
  bits, and `ModuleInstance::initializeMemories` applies the same cap.
- `Test/main.cpp` has three fixtures: byte-exact size/grow/bounds on a
  one-byte-page memory, an import page-size mismatch (rejected) next to a
  matching one (linked), and an unsupported-exponent rejection. The `memory`
  (65/0), `memory64` (55/0) and `memory-multi` (6/0) scripts are unchanged.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~CPS1~~ | ~~**The page-size flags do not decode.**~~ **Done (CPS-1):** `MemoryType::decode` consumes `p` for flags `0x08`–`0x0F` and stores `pageSize`; exponents other than 0/16 are rejected. | `WasmType.hpp` (`MemoryType`) | Closed |
| ~~CPS2~~ | ~~**`MemoryType`/`LinearMemory` have no page size.**~~ **Done (CPS-1/CPS-2):** both carry the field; `LinearMemory::pageSize` is appended after `is64` so existing offsets are unchanged. | `WasmType.hpp`; `WasmMemory.hpp` | Closed |
| ~~CPS3~~ | ~~**Allocation and growth hard-code 64 KiB.**~~ **Done (CPS-2):** all byte math multiplies by `pageSize`. | `WasmMemory.cpp`; `WasmStore.cpp`; `WasmModuleInstance.cpp` | Closed |
| ~~CPS4~~ | ~~**Bounds, size and page caps assume 64 KiB.**~~ **Done (CPS-2/CPS-3):** `memory.size`, grow, the 32-bit cap and the byte bounds use the configured size. | `WasmValidator.cpp`; `WasmModuleInstance.cpp`; `LibJit/LibjitOpcodeDispatcher.cpp` | Closed |
| ~~CPS5~~ | ~~**Import matching ignores page size.**~~ **Done (CPS-3):** page sizes compare exactly. | `WasmImport.hpp`; `WasmRegistryImportResolver.cpp` | Closed |
| ~~CPS6~~ | ~~**No focused coverage.**~~ **Done (CPS-4):** three hand-built fixtures cover size/grow/bounds, import mismatch and the unsupported exponent. | `Test/main.cpp` | Closed |


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
| **CPS-1 — Type and parser** | ✅ `pageSize` on `MemoryType`; decode `0x08`–`0x0F` and consume `p`; accept only 0/16. | CPS1, CPS2 | Done: the one-byte-page module parses and `wasm-tools print` agrees on `(pagesize 0x1)`. |
| **CPS-2 — Memory/store math** | ✅ `LinearMemory::pageSize` threaded through allocation, growth, shared reservation, size and grow. | CPS3 | Done: three one-byte pages are exactly three bytes and growing by two adds two bytes. |
| **CPS-3 — Matching and validation** | ✅ Exact page-size matching, byte-budget page caps, unknown-flag rejection, and `memory.size` reads the field. | CPS4, CPS5 | Done: a 64 KiB memory does not satisfy a 1-byte-page import; the matching one links. |
| **CPS-4 — Tests** | ✅ Three hand-built fixtures plus the unchanged `memory`/`memory64`/`memory-multi` scripts. | CPS6 | Done: the bounds fixture traps at byte 3 of a 3-byte memory while the 64 KiB paths stay green. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-24):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `memory` | 65 | 0 | 25 | 64 KiB model unchanged; strict run 87/0 |
| `memory64` | 55 | 0 | 14 | 64-bit index type unchanged; strict run 69/0 |
| `memory-multi` | 6 | 0 | 0 | multiple memories unchanged |
| `custom page sizes: one-byte pages size and grow are byte-exact` | 27 assertions | 0 | 0 | 3 pages = 3 bytes; grow adds exactly the requested bytes; byte 3 traps |
| `custom page sizes: a page-size mismatch does not satisfy a memory import` | 9 assertions | 0 | 0 | 64 KiB is rejected for a 1-byte import; the matching memory links |
| `custom page sizes: an unsupported page-size exponent is rejected at load` | 1 assertion | 0 | 0 | `p = 8` is a named load error |

Reading: the memory substrate now distinguishes page counts from bytes. The
bounds fixture traps at exactly the byte bound rather than at a rounded host
page, and no existing memory script changed its result.

## 9. References

- Custom Page Sizes:
  https://github.com/WebAssembly/custom-page-sizes/blob/main/proposals/custom-page-sizes/Overview.md
- In-repo: [`MEMORY64.md`](MEMORY64.md), [`MULTI_MEMORY.md`](MULTI_MEMORY.md),
  [`THREADS.md`](THREADS.md), [`MEMORY_CONTROL.md`](MEMORY_CONTROL.md),
  [`../SAFETY.md`](../SAFETY.md), [`../ABI.md`](../ABI.md); sources
  `WasmBase/WasmType.hpp` (`Limits`, `MemoryType`),
  `WasmBase/WasmModule.cpp` (`processMemorySection`),
  `WasmBase/WasmMemory.hpp`/`.cpp` (`LinearMemory`, `StoreOwnedMemory`),
  `WasmBase/WasmStore.cpp`, `WasmBase/WasmModuleInstance.cpp`,
  `WasmBase/WasmRegistryImportResolver.cpp`, `WasmBase/WasmValidator.cpp`,
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchMemorySize`).

