# AsmJit/SLJIT v128 slice 4 {#backend_p3_simd4}

*Post-mortem: widening, `extmul`, `extadd_pairwise`, `dot`, `narrow`*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: the last integer SIMD families — the twelve widening ops
(`extend_low`/`extend_high` for `i8x16→i16x8`, `i16x8→i32x4`, `i32x4→i64x2`,
signed and unsigned), the twelve `extmul` ops, the four `extadd_pairwise` ops,
`i32x4.dot_i16x8_s`, and `i8x16.narrow_i16x8_s/u`. **29 of the 33 candidate
hooks landed; the two `i16x8.narrow_i32x4_*` opcodes are quarantined** because
the AsmJit lowering disagrees with the reference (see §2.2). With that
quarantine, every other integer SIMD family the core exposes is lowered in both
backends.

## 0. Development log

### 2026-11-05 — entry 1: the table edit, and a name-generation bug worth recording

- **33 rows, four macro shapes.** The widening (`WIDEN1`/`WIDEN1H`),
  `extmul` (`WIDEN2`/`WIDEN2H`), `extadd_pairwise` (`PAIRWISE`) and `narrow`
  (`NARROW`) families all reduce to the two emitters the previous slices
  established: one vector in for widening/pairwise, two vectors in for
  `extmul`/`narrow`/`dot`. No new emitter, no `WasmBase/` change.
- **Bug (generation): I derived the C entry name from the Camel name.** Dropping
  the `i` prefix of `I16x8ExtendLowI8x16S` produced
  `wasm_16x8_extend_low_8x16_s`, and the compiler rejected 28 of the lines in
  one build. The fix was to stop doing string surgery: the table must carry the
  `entry` column exactly as `wasm_simd_ops.inc` spells it. The 28 bad lines were
  removed by matching `wasm_(16x8|32x4|64x2)_(extend|extadd|extmul)` and
  regenerated from the entry column — a mechanical repair, but one that only
  worked because every generated line is a single line.

### 2026-11-05 — entry 2: the third round of lane-packing expectation errors

Six assertions failed on the first run; all six were my expectations, not the
lowering:

| Symptom | Cause |
|---|---|
| `i16x8_extend_high_i8x16_s` → 0 | the values were in the **low** 8 bytes; `_high` reads bytes 8–15 |
| `i32x4_extend_high_i16x8_u` → 0 | same, for the high four i16 lanes |
| `i16x8_extmul_high_i8x16_s` → 0 | same, for the high eight bytes |
| `i8x16_narrow_i16x8_s` → `0x303` | `0x00020002` is **two** i16 lanes `[2,2]`, so the third i32 lane is `0x00000303`, not a byte fill |
| `i8x16_narrow_i16x8_u` → 0 | the op saturates a **signed** i16 input to u8, so `-1` clamps to `0` |
| `i64x2_extend_low_i32x4_u` (passed) | — |

This is the third slice in a row where the fixture's lane arithmetic, not the
compiler, was the defect. The rule that would have prevented all six: write the
**lane map** down next to the export (`i32x4` word *w* holds i16 lanes `2w`,
`2w+1`; `_high` reads the upper half of the *vector*, i.e. bytes 8–15) and only
then pick constants.

### 2026-11-05 — entry 3: a real AsmJit defect, found by the A/B case and quarantined

`i16x8.narrow_i32x4_s` disagreed between the backends, reproducibly:

| Probe | Result |
|---|---|
| `wasm_i16x8_narrow_i32x4_s` called directly on `alignas(16)` arrays | `0200 0200 0000 0000 0300 0300 0000 0000` — **correct** |
| SLJIT backend, `i32x4.extract_lane 0` / `2` | `0x00020002` / `0x00030003` — **correct** |
| AsmJit backend, same extracts | `2` / `3` — i.e. lanes `[2,0,0,0, 3,0,0,0]` |

Ruled out: the shared helper (direct probe), the constant materialisation (the
same constants are correct in `i8x16.narrow_i16x8_s`), the shared
`emitVectorHelper2` (identical call shape to `extmul`, which is correct), and
buffer alignment (`new_stack(32, 32)` changed nothing). The failing result is
exactly the narrow of inputs whose upper lanes are zero, so the leading suspect
is the *argument materialisation for this particular helper* — not the
signature, the emitter or the helper.

Because the two backends must not diverge, **both** `i16x8.narrow_i32x4_s` and
`i16x8.narrow_i32x4_u` are quarantined: the dispatchers were removed from both,
the fixture exports and assertions were removed, and the pair is listed in the
residual table as an **open defect**, not as an unimplemented family. The
quarantine is better than shipping a lowering that is wrong on one backend: a
refused opcode is a documented limitation, a wrong one is a silent
miscompilation.

## 1. What the shared ABI forced

Nothing new for the 29 landed hooks: the op list's `WIDEN1`, `WIDEN1H`,
`WIDEN2`, `WIDEN2H`, `PAIRWISE`, `PAIRWISE2` and `NARROW` macros all expand to
one of the two signatures the previous slices already emit against. That is the
headline of the integer work: four emitters per backend cover the whole
integer SIMD surface, because the core's op list is the single source of the
naming and the signature shapes.

The quarantine is the counter-example: the contract is satisfied for 29 of the
33 hooks, and the two that are not are refused rather than approximated.

## 2. What broke, with the failing fixture

### 2.1 Expectation errors (six)

See the table in entry 2. Every one is a lane-map mistake, none a lowering bug.
The corrected values are asserted in the SLJIT case and mirrored by the A/B
case, which compares the two backends rather than a constant.

### 2.2 The AsmJit defect (fixed in slice 5)

See entry 3 for the evidence. **Resolved:** the cause was not the narrow
lowering but the constant store — `mov(qword_ptr, uint64_t)` truncated to 32
bits, so bytes 4–7 of a `v128.const` were lost. See
[`ASMJIT_SLJIT_P3_SIMD5.md`](ASMJIT_SLJIT_P3_SIMD5.md); both opcodes are enabled
again and covered by `simd_widen.wat` plus the new `const_lane1`/`const_lane3`
regression in the slice-1 fixture.

## 3. What the toolkit could not express

- **Nothing new.** Widening and pairwise are unary, `extmul`/`narrow`/`dot` are
  binary, and all of them fit the existing emitters.
- **The quarantine is a process tool, not a toolkit limitation.** Both backends
  can express the op; one of them computes it wrongly, so neither ships it.

## 4. What the tests did not catch

- **The quarantined defect was caught, but only as a backend disagreement.** A
  single-backend test would have passed on SLJIT and failed on AsmJit with no
  hint that the other backend was right. The A/B case is what turned "a failure"
  into "a wrong answer" — the third time in this series that the paired test
  earned its cost.
- **Saturation bounds for `narrow`** are covered only by the `-1 → 0` case and a
  small in-range case; the `[-128, 127]` clamp and the `u8` upper clamp are not.
- **`extadd_pairwise`** is exercised with uniform lanes only; an odd/even
  pattern that would catch a pairwise-order swap is not tested.
- **`dot_i16x8_s`** has one input pair (`(2,3)·(4,5)`); saturation/wrapping of
  the products is untested.
- **Alignment** remains untested, as in slices 1–3.

## 5. What was slower, and by how much

**Not measured.** 29 helper calls added; no new emitter, no change to either
representation.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the entry-name column from `wasm_simd_ops.inc` verbatim; never derive an
  entry point name from the Camel name;
- the lane-map-first fixture discipline: write the map down, then choose
  constants (`i32x4` word *w* holds i16 lanes `2w`, `2w+1`; `_high` reads bytes
  8–15; `narrow_i16x8_u` clamps a signed input);
- the direct-helper probe as the referee when two backends disagree;
- **quarantine as a first-class outcome**: remove the dispatcher from *both*
  backends, remove the fixture coverage, list the opcode as an open defect.

Do not copy:

- string surgery to build symbol names;
- a same-shape-op inference ("`extmul` works, so `narrow` must");
- leaving a lowering in place because only one backend is wrong.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `i16x8.narrow_i32x4_s/u` | **none** — enabled again | fixed in slice 5: the constant store, not the narrow lowering |
| `f32x4`/`f64x2` lanes, conversions, `f16` | `onUnhandledOpcode` | float SIMD slice, not started |
| `i8x16.shuffle`/`swizzle`, relaxed SIMD | `onUnhandledOpcode` | transport-only helpers exist |
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers exist |
| `v128` parameters/results, block types, `select_t` | refused by name | unchanged from slice 1 |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 4 (widening, `extmul`, `extadd_pairwise`, `dot`,
  `i8x16.narrow_i16x8_s/u`) in both backends; 29 hooks each, 2 opcodes
  quarantined.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 508 assertions in 11 cases;
  `[sljit]` 1632 in 26, `[asmjit]` 1114 in 29; the full suite — 20517
  assertions in 685 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- **Open item resolved:** `i16x8.narrow_i32x4_s/u` on AsmJit — fixed in slice 5
  ([`ASMJIT_SLJIT_P3_SIMD5.md`](ASMJIT_SLJIT_P3_SIMD5.md)), not re-enabled by
  inspection.
