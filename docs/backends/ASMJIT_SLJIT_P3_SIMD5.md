# AsmJit/SLJIT v128 slice 5 {#backend_p3_simd5}

*Post-mortem: fixing the quarantined AsmJit `narrow` defect*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened as the fix step
that slice 4 handed over. Both backends are green and the quarantine is lifted.**

Scope: find and fix the defect that forced slice 4 to quarantine
`i16x8.narrow_i32x4_s/u`, re-enable the two opcodes in both backends, and add
the regression that would have caught the root cause four slices earlier.

## 0. Development log

### 2026-11-05 — entry 1: the isolation path

Slice 4 had already ruled out the helper, the constants' *payload*, the shared
`emitVectorHelper2` and the buffer alignment. This step narrowed it in four
probes, each cheaper than the last guess:

1. **Copy the two inputs into fresh buffers before the call.** No change — so
   input aliasing / register-allocator coalescing was not the cause.
2. **Instrument the helper call from inside AsmJit** (a temporary `extern "C"`
   wrapper that dumped its arguments and its output, then called the real
   helper). The helper received `a = 0200 0000 0000 0000`, i.e. the *constant*
   was already wrong at the call site, before any narrow semantics.
3. **Instrument `dispatchV128Const`.** The decoded payload and the 64-bit word
   were correct (`lo = 0x0000000200000002`, bytes `02 00 00 00 02 00 00 00`),
   so the defect was in the **store**, not in the decode.
4. **Materialise the word into a `Gp64` and store the register** instead of
   storing the immediate into memory. Everything passes, including both
   narrows.

Root cause: **x86 has no `mov r/m64, imm64`.** A 64-bit immediate can only be
loaded into a register, so storing a `uint64_t` straight into a qword memory
operand is not encodable; AsmJit's convenience overload accepted it and emitted
a 32-bit store. The low word of a `v128.const` therefore lost **bytes 4–7**.

Why four slices stayed green: every earlier fixture read only bytes that survive
the truncation.

| Fixture | Bytes actually checked | Result with the defect |
|---|---|---|
| `simd_basic` `const_extract` (`1 2 3 4`, lane 2) | bytes 8–11 → from `hi` | **passed** |
| `simd_basic` `bitselect_value` | bytes 0–3 → low half of `lo` | passed |
| `simd_lanes` / `simd_int2` | splat-built vectors, not constants | passed |
| `simd_widen` `i16x8_extend_high_*` | bytes 8–15 → from `hi` | passed |
| `simd_widen` `i16x8_narrow_i32x4_s` | all 16 bytes → bytes 4–7 included | **failed** |

So the defect was latent for every constant with a non-zero second lane of the
low word, and `narrow_i32x4` was simply the first lowered op that *reads* those
bytes. The quarantine was correct, and the opcode was not the problem.

## 1. What the shared ABI forced

Nothing — the fix is inside one backend and touches no contract. It is worth
recording that the shared helper ABI made the defect *findable*: because both
backends call the same entry point, a direct-helper probe could declare the
semantics correct in one run and confine the search to the lowering.

## 2. What broke, with the failing fixture

| Symptom | Cause | Fixture export |
|---|---|---|
| AsmJit `narrow_i32x4_s` lanes `[2,0,0,0,3,0,0,0]` | `v128.const` stored only the low 32 bits of its low word | `i16x8_narrow_i32x4_s(_lane0)` |
| AsmJit constants with a non-zero second lane silently zeroed | `mov(qword_ptr, uint64_t)` is not encodable; the overload truncated | `const_lane1` / `const_lane3` (new regression) |

## 3. What the toolkit could not express

- **A memory-destination 64-bit immediate.** The instruction does not exist; a
  register is mandatory. The node-based emitter's convenience overload hides
  that, which is exactly how it became a silent miscompile.

## 4. What the tests did not catch

- **Four slices without checking every word of a constant.** `const_extract`
  looked convincing (a non-zero lane) but read the high immediate; nothing read
  bytes 4–7. The new `const_lane1`/`const_lane3` exports pin those bytes in the
  slice-1 fixture and in both backends.
- **The failure only appeared through the A/B case.** A single-backend test
  would have reported "wrong value" with no indication that the other backend
  was right.
- **No direct test compares a constant's 16 stored bytes** with a reference
  array; the lane exports cover the four `i32` lanes, which is sufficient for
  this defect but not for byte-level constants.

## 5. What was slower, and by how much

**Not measured, and probably measurable:** the fix adds one register move per
constant word (two moves per `v128.const`). That is the correct trade: the
alternative is relying on an encoding that does not exist.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the isolation order: direct-helper probe → argument dump → dispatcher dump →
  minimal fix;
- the rule that **a 64-bit immediate is loaded into a register, never stored
  straight into memory**;
- regression fixtures that read **every** word of a constant, not one
  convenient lane;
- quarantine-on-disagreement, and the follow-up fix that lifts it in the same
  series rather than leaving it as a footnote.

Do not copy:

- trusting an emitter overload to encode something the ISA cannot express;
- a fixture whose "constant" coverage reads only the bytes that happen to be
  correct;
- diagnosing a lane-level failure from lane-level symptoms — the last place to
  look here turned out to be the first: how the operand was built.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `i16x8.narrow_i32x4_s/u` | **none** — enabled in both backends | fixed here; `simd_widen.wat` and the A/B case cover it |
| `f32x4`/`f64x2` arithmetic, comparisons, unary ops, conversions | **landed in slice 6** ([`ASMJIT_SLJIT_P3_SIMD6.md`](ASMJIT_SLJIT_P3_SIMD6.md)) | `simd_float.wat` |
| `f32x4`/`f64x2` lane access, `f16` | `onUnhandledOpcode` | slice 7 (float-bearing signatures) |
| `i8x16.shuffle`/`swizzle`, relaxed SIMD | `onUnhandledOpcode` | transport-only helpers exist |
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers exist |
| `v128` parameters/results, block types, `select_t` | refused by name | unchanged from slice 1 |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

With this fix the **whole integer `v128` surface** the core exposes is lowered in
both backends and covered by paired tests.

## 8. Sign-off

- Fix: AsmJit `v128.const` now materialises each 64-bit word through a register;
  `i16x8.narrow_i32x4_s/u` re-enabled in both backends; regression exports
  `const_lane1`/`const_lane3` added to the slice-1 fixture.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 534 assertions in 11 cases;
  `[sljit]` 1658 in 26, `[asmjit]` 1128 in 29; the full suite — 20543
  assertions in 685 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- **No open defects** carried forward from the integer SIMD work.
