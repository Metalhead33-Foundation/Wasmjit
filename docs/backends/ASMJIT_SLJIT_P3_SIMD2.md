# AsmJit/SLJIT v128 slice 2 {#backend_p3_simd2}

*Post-mortem: integer lanes, comparisons, `all_true`*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: the integer lane families of `v128` — `add`/`sub` for all four integer
shapes, `mul` for `i16x8`/`i32x4`/`i64x2`, the comparison families (`eq`, `ne`,
`lt_s`, `lt_u`, `gt_s`, `gt_u`, `le_s`, `le_u`, `ge_s`, `ge_u` for `i8x16`,
`i16x8` and `i32x4`; the six signed/unsigned forms the spec provides for
`i64x2`), the four `neg` forms, and the four `all_true` reductions. 55 hooks per
backend. Out: shifts, saturating and narrowing/widening families, float lanes,
`shuffle`/`swizzle`, extending/lane loads, and `v128` signatures.

## 0. Development log

### 2026-11-05 — entry 1: one table, two backends

- **The slice is generated, not hand-written.** `WasmBase/wasm_simd_ops.inc` is
  already the single list of op names, and the naming is a mechanical transform:
  `WASM_SIMD_OP_CMP(i8x16LtS, i8x16_lt_s, …)` means the hook is
  `dispatchI8x16LtS` and the C entry point is `wasm_i8x16_lt_s`. A 55-row table
  was expanded into both backends, so a typo in one hook name cannot survive in
  only one of them. That also means the next family is a table edit, not a
  review of a hundred similar functions.
- **The core needed no line.** Slice 1 established the three helper signatures
  the op list declares — `(a, b, out)`, `(a, out)` and `(a) -> i32` — and this
  slice uses exactly those, so `WasmBase/` is untouched. Two small emitters were
  added beside the existing ones: SLJIT needed `emitV128Unary` (extracted from
  `v128.not`) and `emitV128Reduce`; AsmJit needed `emitVectorReduce`. All three
  reuse the slice-1 register discipline unchanged.
- **`all_true` is a reduction, not a bitwise trick.** The core exposes it per
  shape (`wasm_i8x16_all_true`, …), returning `i32`; the backend only has to
  pass one pointer and store the result, which is why the reduce emitter is ten
  lines on both sides.

### 2026-11-05 — entry 2: the lowering was right; the expectations were wrong

Three cases failed on the first run — `i8x16.lt_s(1, 2) == 255` instead of `-1`,
`i16x8.ge_s(1, 2)` returned `0xFFFF0000`, and `i8x16.neg(1)` returned `255`.
Two probes settled it:

1. **Direct helper probe**: calling `wasm_i8x16_lt_s` and `wasm_i8x16_neg` on a
   stack array of `0x01`/`0x02` bytes produced `0xFF` in all sixteen bytes (32
   byte assertions, all green). The shared layer is correct.
2. **Stored-vector probe**: storing the module's result and reading it back with
   `i32.load` showed `0x000000FF` — one set byte per 32-bit word.

The cause was the fixture, not the compiler: `i32x4.splat(1)` writes
`01 00 00 00` in each word, so only the **low byte** of every `i8x16` lane group
is 1, and only the **low 16-bit lane** of every `i16x8` pair is 1. The
byte-wide comparisons must therefore be driven with byte-fill splats
(`i32.const 0x01010101`), and the 16-bit ones with `0x00020002`. The corrected
expectations match the spec's all-ones lane masks exactly.

Two smaller lessons from the same probe: the fixture needed `(memory 1)` before
any `v128.store` (the module failed validation with "unknown memory 0"), and a
probe test is only useful if it fails loudly — `INFO` on a passing `CHECK(true)`
prints nothing, so the probe had to compare against an impossible value.

## 1. What the shared ABI forced

Nothing new. The op list already declares `wasm_<entry>` for every family with
the three signatures above, so both backends are still transports: no lane loop,
no signedness rule and no mask constant lives in `Sljit/` or `Asmjit/`. The
interesting consequence is that a new lane family is *cheaper than a new test
fixture* — which is why the fixture, and the mental model behind it, was the
real work of this slice.

## 2. What broke, with the failing fixture

| Symptom | Cause | Fixture export |
|---|---|---|
| `255` instead of `-1` | comparison driven by `i32x4.splat(1)`, whose bytes are `01 00 00 00` | `i8x16_lt_s` |
| `0xFFFF0000` instead of `0` | same, for `i16x8` (only lane 1 of the pair was set) | `i16x8_ge_s` |
| `255` instead of `-1` | same, for the unary `neg` | `i8x16_neg` |
| "unknown memory 0" | probe used `v128.store` before the fixture declared a memory | the probe exports (removed) |

No lowering defect was found in this slice: both backends produced the same
values through the same helpers, and the direct-helper probe pinned the shared
layer independently of either of them.

## 3. What the toolkit could not express

- **Shifts are the next mixed-signature family.** `i8x16.shl` takes a scalar
  count (`(a, count, out)`), which neither `emitV128Unary` nor
  `emitV128Binary` models; they need their own emitter on both sides.
- **SLJIT still has no vector register.** Every op is a helper call with three
  live pointers; the new families fit the same register rule (R0 last), which is
  why they needed no new lowering strategy.
- **AsmJit still allocates a buffer per op.** A compare chain allocates one
  16-byte stack slot per comparison; the register allocator folds the moves but
  not the frame slots.

## 4. What the tests did not catch

- **Every comparison is exercised on half its lanes.** The fixture reaches the
  comparison families through `i32x4.splat`, so for `i8x16`/`i16x8` only the
  low lane of each group is non-zero in the operands. That is enough to check the
  mask width and sign handling, but not a lane-varying pattern; a fixture built
  from `v128.const` with distinct bytes would cover the rest.
- **`i64x2` arithmetic is only checked in the low lane** (`i32x4.extract_lane`
  cannot observe the high 64-bit lane until `i64x2.extract_lane` lands).
- **The saturating and narrowing families** share the two-pointer signature and
  would be nearly free, but they are not tested because they are not lowered.
- **Alignment** remains untested, exactly as slice 1 recorded.

## 5. What was slower, and by how much

**Not measured.** The slice adds one helper call per lane operation. A native
lowering can replace any family behind the same representation without touching
the fixture or the other backend.

## 6. What a fourth backend should copy, and what it must not

Copy:

- generation of the dispatch table from `wasm_simd_ops.inc`, so hook and helper
  names cannot drift;
- three emitters (binary, unary, reduce) as the whole lowering surface for the
  integer families;
- the direct-helper probe as the referee when a backend disagrees with a
  fixture — it decides whether the semantics or the lowering is at fault in one
  run;
- byte-fill (`0x01010101`) and lane-fill (`0x00020002`) splats when a fixture has
  to observe byte- or word-wide lanes through an `i32` result.

Do not copy:

- the assumption that a splat of `1` fills every byte lane — it fills one byte
  per word;
- a probe that reports through `INFO` on a passing assertion;
- a fixture that stores a vector without declaring a memory.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| shifts, saturating add/sub, `avgr`, `abs`, `min`/`max`, `popcnt` | **landed in slice 3** ([`ASMJIT_SLJIT_P3_SIMD3.md`](ASMJIT_SLJIT_P3_SIMD3.md)) | `simd_int2.wat` |
| narrowing, widening, pairwise, dot, `extmul`, `extadd` | `onUnhandledOpcode` | op list already declares the helpers |
| `f32x4`/`f64x2` lanes | `onUnhandledOpcode` | float SIMD slice |
| `shuffle`/`swizzle` and relaxed SIMD | `onUnhandledOpcode` | transport-only helpers already exist |
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers already exist |
| `v128` parameters/results, block types, `select_t` | refused by name | unchanged from slice 1 |
| references/GC, exceptions, continuations, atomics, `f16` | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 2 (integer lane arithmetic, comparison families, `neg`,
  `all_true`) in both backends.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 296 assertions in 7 cases
  (both slices, SLJIT execution and the A/B cases); the full suite — 20305
  assertions in 681 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- The "unsupported" fixture is still untouched: the refused families are pinned
  by name in `WRITING_A_BACKEND.md`, not by moving a test fixture.
