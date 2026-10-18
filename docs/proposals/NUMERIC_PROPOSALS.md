# Wide Arithmetic, Half Precision, and Rounding-Mode Control — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — three numeric
extension proposals are surveyed together. None is implemented: there is no
wide-arithmetic opcode, no `f16` lane type, and no rounding-variant opcode.)

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
  an unmodelled opcode at load, which is the current behaviour.

## 4. What we have implemented today

- `WasmBase/WasmOpcode.hpp` defines `MiscOpcode` (`0xFC`) sub-opcodes `0`–`17`:
  the `trunc_sat` family, bulk memory and table operations. Sub-opcodes `19`–`22`
  (Wide Arithmetic) and `0x80`–`0xBB` (rounding variants) are absent; the misc
  dispatcher's default rejects them.
- There is no `ValueTypeCode::F16` and no `f16` lane shape. `ValueTypeCode`
  covers `i32`, `i64`, `f32`, `f64`, `v128` and the reference types; the SIMD
  type translation handles `v128` as a 16-byte aggregate.
- SIMD lowering and dispatch cover the existing fixed-width `v128` surface,
  including `f32x4`/`f64x2` operations, but not the `0xFD 0x120`–`0x14F` FP16
  range.
- Multiple results already work through the caller-provided result buffer
  (`MULTI_VALUE.md` MV4), so the `i64.*128` result shape is compatible with the
  existing ABI once the opcodes are decoded.
- No test fixture exercises Wide Arithmetic, FP16, or the rounding variants.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| NUM1 | **Wide Arithmetic does not decode.** `0xFC 19`–`22` are outside the `MiscOpcode` enum and reach the default rejection. | `WasmOpcode.hpp` (`MiscOpcode` ends at 17); `WasmOpcodeDispatcher::dispatchPrefixMisc` default. | Medium |
| NUM2 | **No Wide Arithmetic lowering.** There is no carry-propagating add/sub or widening multiply helper; a decoded instruction would have no execution path. | `LibjitOpcodeDispatcher.cpp`; no `add128`/`mul_wide` symbol. | Medium |
| NUM3 | **No `f16` type or lane shape.** `ValueTypeCode` has no FP16 code, and `translateValueType` has no `f16` case. | `WasmBase/WasmType.hpp`; `LibJit/LibJitTypeTranslation.cpp`. | Medium |
| NUM4 | **No FP16 memory or SIMD opcodes.** `0xFC 0x30/0x31` and `0xFD 0x120`–`0x14F` are unassigned. | `WasmOpcode.hpp` (`MiscOpcode`, `SimdOpcode`); SIMD dispatcher. | Medium |
| NUM5 | **No rounding-variant opcodes.** `0xFC 0x80`–`0xBB` are unassigned; only nearest-even forms exist. | `WasmOpcode.hpp` (`MiscOpcode` 0–17). | Medium |
| NUM6 | **No rounding helpers or reference results.** Lowering a `_ceil`/`_floor`/`_trunc` opcode needs the proposal's exact zero-sign, NaN and overflow rules, not the host's ambient rounding mode. | `LibjitOpcodeDispatcher.cpp` numeric helpers; `SIMD_LIBJIT.md` helper policy. | Medium |
| NUM7 | **No focused coverage.** The suite has no wide-arithmetic, FP16 or rounding-variant scripts; existing `i32`/`i64`/`simd_*` scripts do not exercise the new encodings. | `Test/wast_supported.txt`; `Test/wasm_wat/` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| NUMD1 | **Defer all three until a toolchain needs them.** Reject the opcodes at load meanwhile. | §3: none is a 3.0 requirement; an unmodelled opcode may be rejected. | No speculative type or ABI churn. |
| NUMD2 | **Lower Wide Arithmetic with helper calls first**, then add native carry/`mul_wide` fast paths behind the same semantics. | §3: lowering and performance are free; the two-result values are exact. | A small first implementation that works on every backend. |
| NUMD3 | **Model `f16x8` as a `v128` lane shape with helper-backed conversions and arithmetic.** | §3: no first-class `f16` value or hardware path is required. | Reuses the existing SIMD frame and `v128` representation. |
| NUMD4 | **Implement rounding as explicit opcode variants, never by changing the host FP environment.** | §3: the proposal defines explicit instructions and no global mode. | Deterministic results and no save/restore state across calls. |
| NUMD5 | **Keep nearest-even forms untouched.** | §2.3: the proposal only adds variants; existing instructions keep their meaning. | No risk to the already-green numeric surface. |

Guardrail: the FP16 and rounding variants must use Wasm's NaN, zero-sign and
trap rules, not the host compiler's default. A helper that silently uses the
host's current rounding mode is a bug even if it looks correct on x86.


## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **NUM-1 — Wide Arithmetic** | Add `0xFC 19`–`22`, their two-result typing, and a correct carry/widening helper path. | NUM1, NUM2 | A hand-built module computes `i64.add128`, `sub128`, `mul_wide_s/u` edge cases and compares both result lanes. |
| **NUM-2 — Rounding variants** | Add the `0xFC 0x80`–`0xBB` decode table and the `_ceil`/`_floor`/`_trunc` helpers/reference results. | NUM5, NUM6 | Every variant returns the exact rounded value and the specified zero sign/NaN for a fixture covering each family. |
| **NUM-3 — FP16** | Add the `f16` lane shape, `f32.load_f16`/`store_f16`, and the `0xFD` FP16 dispatch table. | NUM3, NUM4 | FP16 memory round-trips and a representative `f16x8` arithmetic/comparison/conversion fixture passes. |
| **NUM-4 — Tests** | Curate proposal scripts or add focused `Test/wasm_wat` fixtures for all three. | NUM7 | The tests fail if an opcode is decoded with the wrong width or result order. |

These milestones are independent; none is required for Wasm 3.0. `NUM-1` is the
smallest and can land without touching `ValueTypeCode`; `NUM-3` is the largest
because it adds a new lane shape to type translation, `Value`, and SIMD lowering.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| wide-arithmetic | — | — | — | no script in the snapshot and no implementation |
| FP16 / half-precision | — | — | — | no script and no `f16` type |
| rounding-mode-control | — | — | — | no script; `simd_f32x4_rounding` covers existing nearest-even SIMD, not the new variants |
| `i32` / `i64` / `simd_*` | existing | existing | existing | nearest-even integer/FP behaviour only; not evidence for these proposals |

Reading: there is nothing to grade for the new encodings. The existing numeric
suite tells us the helpers to reuse, not that the proposals work.

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
  `LibJit/LibJitTypeTranslation.cpp`, `LibJit/LibjitOpcodeDispatcher.cpp`.

