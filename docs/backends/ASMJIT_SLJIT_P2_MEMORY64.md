# AsmJit/SLJIT P2 memory64 slice {#backend_p2_memory64}

*Post-mortem: memory64 loads, stores and the residual refusals*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened and closed in
one pass. The log records the order the two backends were changed; the sections
below are the reusable part.**

Scope: memory64 **loads/stores, `memory.size` and `memory.grow`** in both
backends, with the address operand, the memarg offset, the grow delta and the
size result all 64-bit. Bulk memory on a memory64 (`memory.copy`/`fill`/`init`)
stays refused: see §7. `table64` and `f16` are not touched.

## 0. Development log

### 2026-11-05 — entry 1: scope and the address-helper decision

- The refusal was a filter in each backend (`requireMemoryIndex` in SLJIT,
  `validateMemoryIndex` in AsmJit), not a missing opcode: every load/store
  already funnelled through one checked-address helper. Removing the filter and
  branching on `MemoryType::indexType` is therefore the whole shape of the
  slice.
- **SLJIT cannot pass the memory64 address and offset in one word with the
  access width.** Its call ABI is four arguments including `vm`, so the
  memory64 helper packs the memory index and the access width into one word and
  takes the 64-bit address and 64-bit immediate offset as the remaining two:
  `(vm, memAndWidth, address, offset)`. The memory32 helper keeps its existing
  packing; the two coexist behind `emitCheckedMemoryAddress`.
- **AsmJit can take five arguments**, so its memory64 helper is the obvious
  `(vm, memIdx, address64, offset64, width32)`.
- Both helpers check `address > UINT64_MAX - offset` before adding, because the
  Wasm address arithmetic is modulo-free: an overflow is an out-of-bounds trap,
  not a wrap.

### 2026-11-05 — entry 2: SLJIT lands first

- `requireMemoryIndex` now returns the declared type without filtering. A new
  `emitCheckedMemoryAddress` owns the memory32/memory64 branch and the
  pending-trap probe, so the four scalar access paths (integer load, integer
  store, float load, float store) share one implementation instead of four
  near-copies.
- `memory.grow` pushes the index-type width and loads its delta with that width:
  a 33-bit delta is a failed grow, not a successful zero-page grow. `memory.size`
  returns the index-type width. Both use new `...64` helpers; the 32-bit helper
  is unchanged.
- Bulk memory on memory64 is refused at compile time with a named reason, so the
  function is left without an entry point rather than half-lowered.

### 2026-11-05 — entry 3: AsmJit follows, with the same tests

- `validateMemoryIndex` returns the type; `validateMemoryArg` only applies the
  u32 offset limit to memory32; `emitMemoryAddress` branches on the index type
  and materialises the 64-bit offset before the invoke node (the P3 ordering
  lesson, applied without rediscovering it).
- `memory.size`/`grow` choose the helper signature, the result width and the
  operand width from the index type; the bulk-memory helpers refuse memory64.
- One compile-time correction: a conditional expression cannot choose between
  two helpers with different parameter types (`is64 ? &f64 : &f32`), so the
  call sites use an `if`/`else` with the signature built first. The same shape
  appears in `memory.size`/`grow`.

## 1. What the shared ABI forced

- **The address operand's type is the memory's index type, not the value
  width.** An `i32.load` on a memory64 takes an i64 address; the result is still
  i32. Both backends had to separate the operand width (index type) from the
  access/result width.
- **`memory.grow`'s delta and result follow the index type.** The failure value
  is `-1` in that width (i32 `0xFFFFFFFF`, i64 `0xFFFF...`).
- **`memory.size` counts pages with the per-memory `pageSize`.** Both helpers
  divide by the field, not by 65536 (CUSTOM_PAGE_SIZES.md).
- **Bounds are `u64` arithmetic with an explicit overflow check** before the
  `effective > size` comparison, in both backends.

## 2. What broke, with the failing fixture

1. **The conditional helper choice did not compile** in AsmJit (two different
   function types in one `?:`); fixed with `if`/`else` at the two sites.
2. **The old refusal filters had to go** in both backends; until then the module
   instantiated but every function using a memory64 access had a null entry
   point. The fixture's `size`/`grow` functions made that visible immediately.
3. No semantic failure survived the tests: the first run of
   `Test/wasm_wat/memory64_basic.wat` passed on both backends, which is the
   point of reusing the P3 shape (helper + declared kind) rather than inventing
   a memory64-only path.

## 3. What the toolkit could not express

| Constraint | Consequence | Accepted or temporary |
|---|---|---|
| SLJIT calls carry at most four arguments | memory64 address + offset + width + `vm` do not fit; the helper packs the index and width | accepted |
| AsmJit's `InvokeNode` argument types must be fixed before the call | the signature is built from `is64`, then the helper is chosen | accepted |
| both backends' bulk helpers take 32-bit operands | bulk memory on memory64 is refused (not miscompiled) | temporary; next slice |

## 4. What the tests did not catch

The exact-value tests pin the semantics; the interesting cases are the two
**anti-truncation probes**:

- address `0x1_0000_0000` with a sentinel at address 0 must trap (a 32-bit
truncation would read the sentinel);
- grow delta `0x1_0000_0000` must fail (a 32-bit truncation would request zero
pages and succeed).

The A/B case compares the same probes across the two generators. A shared
mistake in both would still pass the A/B case; that is why the exact tests carry
the sentinel values.

## 5. What was slower, and by how much

**Not measured.** No benchmark was run. Memory64 adds no instruction to the
memory32 path; the 64-bit path uses the same one helper call plus the offset
materialisation.

## 6. What a fourth backend should copy, and what it must not

Copy:

- one checked-address helper per index type, selected in one place, with the
  pending-trap probe inside it;
- the operand-width/index-type separation in the dispatcher, and the two
  anti-truncation tests;
- the refusal for bulk memory64 until the operands are genuinely 64-bit.

Do not copy:

- four copies of the address sequence (the SLJIT slice collapsed them into one
  helper);
- a conditional expression over helper pointers of different signatures;
- a bulk-memory helper that accepts 32-bit operands for a 64-bit memory.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| bulk memory on memory64 (`memory.copy`/`fill`/`init`) | SLJIT: per-function null entry point; AsmJit: transactional module refusal | `sljit: bulk memory on a memory64 is refused per function`, `AsmJit refuses bulk memory on a memory64 transactionally` |
| `table64` | unchanged from P0–P3 | spec `[spec]` corpus |
| `v128`/SIMD, references/GC, exceptions, continuations, atomics | unchanged | §14.1 snapshot |

The two backend refusal models are deliberately not unified here. SLJIT leaves
the offending function uncallable and keeps the rest of the module usable;
AsmJit aborts the whole module transaction and returns its allocations. The
core accepts both (it refuses the boundary in the first case and propagates the
instantiation error in the second), so the difference is a property of each
backend's transaction model, not of the ABI. `Test/wasm_wat/memory64_bulk.wat`
pins it.

## 8. Sign-off

- Slice: P2 memory64 (loads/stores, `memory.size`, `memory.grow`) in both backends.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./WasmJit '[m64]'` — 140 assertions in 5 test cases
  (SLJIT exact, AsmJit exact, cross-backend, and the two refusal-tests);
  `[sljit]` 1183 assertions in 18 cases, `[asmjit]` 892 in 25; the full suite
  20068 assertions in 677 cases; Doxygen exit 0 with no warnings.
