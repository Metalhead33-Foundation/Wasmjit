# Wide Arithmetic, Half Precision, and Rounding-Mode Control — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_numeric_proposals}

Status: design note / implementation reference (revision 4 — Wide Arithmetic,
Rounding-Mode Control and Half Precision are implemented. `f16x8` is a `v128`
lane shape; there is still no first-class `f16` value type by design.)

Audience: anyone touching `WasmType`, `WasmOpcode`, `WasmOpcodeDispatcher`,
`LibjitOpcodeDispatcher`, SIMD lowering, or the numeric test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (three related proposals in one
note). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do Wide Arithmetic, Half Precision and Rounding-Mode Control demand? |
| §3 | What do they *not* demand? |
| §4 | What numeric surface is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

### 2.1 Wide Arithmetic

Four instructions provide 128-bit operations assembled from 64-bit values.
There is no new value type; the two-result forms use the existing multiple-result
ABI.

| Instruction | Stack type | Encoding |
|---|---|---|
| `i64.add128` | `[i64 i64 i64 i64] -> [i64 i64]` | `0xFC 19` |
| `i64.sub128` | `[i64 i64 i64 i64] -> [i64 i64]` | `0xFC 20` |
| `i64.mul_wide_s` | `[i64 i64] -> [i64 i64]` | `0xFC 21` |
| `i64.mul_wide_u` | `[i64 i64] -> [i64 i64]` | `0xFC 22` |

The first two take the low/high halves of two 128-bit operands and return the
low/high halves of the result. The multiplication forms return the full
128-bit product of two signed or unsigned 64-bit operands. The proposal
deliberately does not add 128-bit division, comparison, shift or rotate
instructions.

### 2.2 Half Precision (FP16)

The proposal adds an IEEE binary16 **lane type**, `f16`, and an `f16x8` SIMD
shape. It is not a first-class Wasm value type: memory and scalar operations
convert through `f32`, and the JS API does not expose `f16` values.

- Memory: `f32.load_f16` (`0xFC 0x30`) and `f32.store_f16` (`0xFC 0x31`).
- Lane: `f16x8.splat`, `extract_lane`, `replace_lane` (`0xFD 0x120`–`0x122`).
- Arithmetic/comparison: `f16x8` `abs`, `neg`, `sqrt`, `ceil`, `floor`,
  `trunc`, `nearest`, `eq`, `ne`, `lt`, `gt`, `le`, `ge`, `add`, `sub`, `mul`,
  `div`, `min`, `max`, `pmin`, `pmax` (`0xFD 0x130`–`0x144`).
- Conversions: `i16x8.trunc_sat_f16x8_s/u`, `f16x8.convert_i16x8_s/u`,
  `f16x8.demote_f32x4_zero`, `f16x8.demote_f64x2_zero`,
  `f32x4.promote_low_f16x8`, `i16x8.trunc_f16x8_s/u`
  (`0xFD 0x145`–`0x14D`).
- FMA: `f16x8.madd`, `f16x8.nmadd` (`0xFD 0x14E`–`0x14F`).

The proposal uses the same NaN, default-NaN and rounding rules as the existing
scalar floating-point operations; it does not introduce JavaScript-visible
half-precision values.

### 2.3 Rounding-Mode Control

The proposal adds explicit `_ceil`, `_floor` and `_trunc` variants of the
existing floating-point operations and conversions, rather than a dynamic
global rounding mode. The nearest-even forms remain unchanged, and the proposal
does **not** add rounding variants of `i32/i64.trunc_f*` (integer rounding) in
its current form.

The new instructions occupy the `0xFC 0x80`–`0xBB` range and cover:

- `f32/f64` `sqrt`, `add`, `sub`, `mul`, `div`;
- conversions among `f32`, `f64`, `i32`, `i64`, including `demote`/`promote`;
- all three variants, with `ceil` at `0x80`–`0x93`, `floor` at `0x94`–`0xA7`
  and `trunc` at `0xA8`–`0xBB`.

Every result is a pure function of the operands and the instruction variant;
there is no process-global rounding state to save or restore.

## 3. What the proposal does not demand

- **No new value type for Wide Arithmetic.** The four instructions operate on
  existing `i64` values and return two `i64`s; a backend may lower them with
  ordinary 64-bit arithmetic, carries and a widening multiply.
- **No particular lowering or performance.** A correct implementation may use
  helper calls, a native `adc`/`sbb` sequence, `mul_wide`, or a
  compiler-recognised pattern; the proposal's performance motivation is not a
  conformance requirement.
- **No 128-bit division, comparison, shift or rotate.** Those were considered
  and deliberately left out of Wide Arithmetic.
- **No first-class `f16` value.** Half Precision adds a lane type and
  `f32`-mediated memory/scalar operations; it does not require `f16` parameters,
  results, locals or JavaScript conversions.
- **No required hardware path for `f16`.** An engine may use conversion helpers,
  software arithmetic or a native FP16 path; only the rounded results and trap
  rules are observable.
- **No global rounding mode.** Rounding-Mode Control uses explicit opcode
  variants; it does not require changing the host's floating-point environment
  or preserving a dynamic mode across calls.
- **No new rounding for integer truncation.** The proposal leaves the
  `i32/i64.trunc_f*` rounding variants out for now.
- **No requirement to implement any of these proposals.** An engine may reject
  an unmodelled opcode at load; this tree now models all three.

## 4. What we have implemented today

- `WasmBase/WasmOpcode.hpp` defines `MiscOpcode` (`0xFC`) sub-opcodes `0`–`22`,
  `0x30`/`0x31`, plus the `0x80`–`0xBB` rounding variants; the latter are
  dispatched through the generic `dispatchRoundingVariant(uint32_t)` hook.
- **Half Precision is implemented.** `f32.load_f16`/`f32.store_f16`
  (`0xFC 0x30/31`) decode as `MiscOpcode` entries and convert through the
  core-owned scalar helpers in `WasmBase/wasm_f16.cpp`. The whole
  `0xFD 0x120`–`0x14F` range decodes through `dispatchSIMDF16x8`, is typed as
  `v128` in the validator, and lowers through one generic LibJIT hook backed by
  the same helpers. `f16x8` is a lane shape, not a `ValueTypeCode`: there is no
  `ValueTypeCode::F16`, and the memory/scalar surface is deliberately
  `f32`-mediated, matching the proposal.
- **Wide Arithmetic is implemented.** `i64.add128`/`i64.sub128` use inline
  carry/borrow (unsigned comparisons on `jit_type_ulong`); `i64.mul_wide_s/u`
  call a portable 32-bit-limb 64×64→128 helper (no `__int128`). Both results are
  pushed low then high, so the existing multi-value ABI marshals them. The
  validator types the four instructions and the manual fixture covers carry,
  borrow and signed/unsigned multiplication edges.
- **Rounding-Mode Control is implemented.** All 60 `0xFC 0x80`–`0xBB`
  variants decode, validate and lower. The lowering computes the exact
  `long double` result and then rounds it to the next/previous representable
  f32/f64 in the requested direction (`roundToFloat`/`roundToDouble`), never by
  changing the host FP environment. The explicit hook means the decoder,
  validator, JIT and stub backend all share one range check.
- `ValueTypeCode` covers `i32`, `i64`, `f32`, `f64`, `v128` and the reference
  types; the SIMD type translation handles `v128` as a 16-byte aggregate.
  Half Precision intentionally adds no `ValueTypeCode::F16`: `f16` exists only
  as a lane interpretation of `v128`.
- SIMD lowering and dispatch cover the fixed-width `v128` surface, including
  `f32x4`/`f64x2` operations, plus the `0xFD 0x120`–`0x14F` FP16 range through
  the generic `dispatchSIMDF16x8` hook.
- Multiple results already work through the caller-provided result buffer
  (`MULTI_VALUE.md` MV4), so the `i64.*128` result shape is compatible with the
  existing ABI once the opcodes are decoded.
- `Test/wasm_wat/wide_arithmetic.wat` plus
  `wide arithmetic: add128, sub128 and mul_wide match reference values` covers
  the Wide Arithmetic edges. `rounding variants: ceil/floor/trunc are selected
  by the sub-opcode` builds the binary opcodes directly (no proposal spelling
  in `wasm-tools` yet) and checks f32 arithmetic plus i32→f32 conversion. FP16
  is covered by `f16 helpers: rounding, arithmetic and lane operations`
  (conversion edges, NaN/zero-sign rules, every lane/arithmetic/conversion/FMA
  helper, both memory helpers and the trapping forms) and by two hand-built
  binary modules: one executes every `0xFD 0x120`–`0x14F` opcode through the
  decoder/validator/LibJIT, the other checks load/store/add across the JIT
  boundary.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~NUM1~~ | ~~**Wide Arithmetic does not decode.**~~ **Done (NUM-1):** `MiscOpcode::I64Add128`–`I64MulWideU` (`0xFC 19`–`22`) decode with no immediates. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | Closed |
| ~~NUM2~~ | ~~**No Wide Arithmetic lowering.**~~ **Done (NUM-1):** inline carry/borrow for add/sub and a portable 64×64→128 multiply helper, with both results pushed low then high. | `LibJit/LibjitOpcodeDispatcher.cpp` | Closed |
| ~~NUM3~~ | ~~**No `f16` type or lane shape.**~~ **Done (NUM-3):** the `f16x8` lane shape is modelled as eight binary16 lanes of `v128`; `ValueTypeCode` deliberately still has no `F16`, because the proposal's `f16` is not a first-class value. | `WasmBase/wasm_f16.{hpp,cpp}`; `WasmOpcode.hpp` (`SIMDOpcode`); SIMD dispatcher | Closed |
| ~~NUM4~~ | ~~**No FP16 memory or SIMD opcodes.**~~ **Done (NUM-3):** `0xFC 0x30/0x31` and `0xFD 0x120`–`0x14F` are assigned, decoded, validated and lowered. | `WasmOpcode.hpp` (`MiscOpcode`, `SIMDOpcode`); SIMD dispatcher; `LibJit/LibjitOpcodeDispatcher.cpp` | Closed |
| ~~NUM5~~ | ~~**No rounding-variant opcodes.**~~ **Done (NUM-2):** `0xFC 0x80`–`0xBB` decode through `dispatchRoundingVariant`, with the proposal's ceil/floor/trunc group layout. | `WasmOpcode.hpp` (`MiscOpcode` range); `WasmOpcodeDispatcher.cpp` | Closed |
| ~~NUM6~~ | ~~**No rounding helpers or reference results.**~~ **Done (NUM-2):** `roundToFloat`/`roundToDouble` choose the adjacent representable f32/f64 in the requested direction from the exact `long double` result; no host FP-mode change. | `LibjitOpcodeDispatcher.cpp`; the hand-built rounding test | Closed |
| ~~NUM7~~ | ~~**Coverage is partial.**~~ **Done (NUM-4):** Wide Arithmetic has `wide_arithmetic.wat`; Rounding-Mode Control has the hand-built direction test; FP16 has the helper suite plus two hand-built binary modules covering every opcode and the JIT load/store path. | `Test/main.cpp`; `Test/wasm_wat/wide_arithmetic.wat` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ~~NUMD1~~ | ~~**Implement Wide Arithmetic and the explicit rounding variants now; defer FP16 until its lane/result surface is needed.**~~ **Superseded (NUM-3):** FP16 is implemented through a `v128` lane shape and a helper layer, without a `ValueTypeCode` addition. | §3: no first-class `f16` value or hardware path is required. | No ABI churn, and the same helper contract is available to every backend. |
| NUMD2 | **Lower Wide Arithmetic with helper calls first**, then add native carry/`mul_wide` fast paths behind the same semantics. | §3: lowering and performance are free; the two-result values are exact. | A small first implementation that works on every backend. |
| NUMD3 | **Model `f16x8` as a `v128` lane shape with helper-backed conversions and arithmetic.** | §3: no first-class `f16` value or hardware path is required. | Reuses the existing SIMD frame and `v128` representation. |
| NUMD4 | **Implement rounding as explicit opcode variants, never by changing the host FP environment.** | §3: the proposal defines explicit instructions and no global mode. | Deterministic results and no save/restore state across calls. |
| NUMD5 | **Keep nearest-even forms untouched.** | §2.3: the proposal only adds variants; existing instructions keep their meaning. | No risk to the already-green numeric surface. |
| NUMD6 | **Use round-to-nearest-even for `f32`/`f64`→`f16` conversions and for f16 arithmetic.** The proposal Overview says "roundTiesToZero", but its own lowering table converts with `vcvtps2ph ..., 0` (nearest-even) and Arm `Fcvt` under the default FPCR, which is also the IEEE-754 default. | The observable contract is the lowering table; nearest-even is what the listed hardware instructions produce. The helper is software and neither sets nor restores the host FP mode; like the fixed-width SIMD helpers it assumes the default nearest-even mode. | One deterministic result on every target, matching the proposal's own lowering. |

Guardrail: the FP16 and rounding variants must use Wasm's NaN, zero-sign and
trap rules, not the host compiler's default. A helper that silently uses the
host's current rounding mode is a bug even if it looks correct on x86.


## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **NUM-1 — Wide Arithmetic** | ✅ Added `0xFC 19`–`22`, two-result typing, inline carry/borrow and portable `mul_wide_s/u` helpers. | NUM1, NUM2 | Done: `wide_arithmetic.wat` + 27 assertions covering carry, borrow and signed/unsigned edges. |
| **NUM-2 — Rounding variants** | ✅ Added the `0xFC 0x80`–`0xBB` generic decode/validate/dispatch hook and direction-correct `roundToFloat`/`roundToDouble` helpers. | NUM5, NUM6 | Done: the hand-built test checks f32 ceil/floor/trunc of an unrepresentable sum and an i32→f32 conversion; nearest-even forms are untouched. |
| **NUM-3 — FP16** | ✅ Added the `f16x8` lane shape, `f32.load_f16`/`store_f16`, the `0xFD 0x120`–`0x14F` decode/dispatch table and a scalar core-owned helper ABI. | NUM3, NUM4 | Done: every opcode decodes, validates and lowers; the helper suite and the two binary modules exercise conversion edges, NaN/zero-sign rules, arithmetic, comparisons, conversions, FMA, memory and the trapping forms. |
| **NUM-4 — Tests** | ✅ Wide Arithmetic, rounding variants and FP16 all have executable coverage. | NUM7 | Done: `wide_arithmetic.wat`, the rounding direction test, the f16 helper suite and the two f16 binary modules fail if result order, width, direction, rounding or trap behaviour changes. |

These milestones are independent; none is required for Wasm 3.0. `NUM-1` is the
smallest and can land without touching `ValueTypeCode`; `NUM-3` reuses the
existing `v128` frame and adds a core-owned helper ABI rather than a new value
type, `Value` shape or type-translation case.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-11-03, `4d52031`):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `wide_arithmetic` manual test | 27 assertions | 0 | 0 | `add128`/`sub128` carry/borrow and `mul_wide_s/u` signed/unsigned edges |
| `rounding variants` manual test | 13 assertions | 0 | 0 | f32 add ceil/floor/trunc on an unrepresentable sum and i32→f32 conversion direction |
| `f16 helpers` + two binary modules | 71 assertions | 0 | 0 | conversion edges, NaN/zero-sign rules, all lane/arithmetic/conversion/FMA helpers, memory helpers, trapping forms, and every `0xFD 0x120`–`0x14F` opcode through decode/validate/LibJIT |
| wide-arithmetic script | — | — | — | no script in the snapshot |
| FP16 / half-precision script | — | — | — | no upstream script, and the pinned `wasm-tools`/`wat2wasm` have no f16 text spelling; the hand-built binary modules are the evidence |
| rounding-mode-control script | — | — | — | no upstream script in the snapshot; the manual binary test is the evidence |
| `i32` / `i64` / `simd_*` | existing | existing | existing | nearest-even integer/FP behaviour only; not evidence for FP16 |

Reading: all three proposals are graded by manual fixtures because the snapshot
has no upstream scripts for them. FP16's fixtures are built as binaries, not
WAT, for the same reason.

## 9. References

- Wide Arithmetic:
  https://github.com/WebAssembly/wide-arithmetic/blob/main/proposals/wide-arithmetic/Overview.md
- Half Precision:
  https://github.com/WebAssembly/half-precision/blob/main/proposals/half-precision/Overview.md
- Rounding-Mode Control:
  https://github.com/WebAssembly/rounding-mode-control/blob/main/proposals/rounding-mode-control/Overview.md
- In-repo: [`SIMD.md`](SIMD.md), [`SIMD_LIBJIT.md`](../SIMD_LIBJIT.md),
  [`MULTI_VALUE.md`](MULTI_VALUE.md), [`GC.md`](GC.md),
  [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`ValueTypeCode`), `WasmBase/WasmOpcode.hpp`
  (`MiscOpcode`, `SimdOpcode`), `WasmBase/WasmOpcodeDispatcher.cpp`,
  `WasmBase/WasmValidator.cpp` (wide-arithmetic and f16 typing),
  `WasmBase/wasm_f16.hpp`/`wasm_f16.cpp` (the f16 scalar helper ABI),
  `LibJit/LibJitTypeTranslation.cpp`, `LibJit/LibjitOpcodeDispatcher.cpp`
  (`dispatchI64Add128`/`Sub128`/`MulWideS`/`MulWideU`, `mulWideU64`/`mulWideS64`,
  `dispatchRoundingVariant`, `roundToFloat`/`roundToDouble`,
  `dispatchF32LoadF16`/`StoreF16`, `dispatchSIMDF16x8`),
  `Test/wasm_wat/wide_arithmetic.wat`, and the hand-built rounding and f16
  tests in `Test/main.cpp`.

