# SIMD (Fixed-width and Relaxed) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmType`, `WasmValue`, `WasmOpcode`,
`WasmOpcodeDispatcher`, `LibJitTypeTranslation`, `LibjitOpcodeDispatcher`, or
the test harness.

This note follows the same split as [`GC.md`](GC.md) and
[`THREADS.md`](THREADS.md):

1. what the SIMD proposals **demand**;
2. what they explicitly do **not** demand (and where an engine is free);
3. what `WasmJit` **currently implements**;
4. the **gaps** between (3) and (1);
5. where we can and should play **fast and loose** on purpose.

It contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the SIMD proposals require? |
| §3 | What do they *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short of §2? |
| §6 | Where do we *choose* to deviate, and why? |
| §7 | Milestones to close the important gaps |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the spec demands

**Status first.** These are two different documents with different spec status:

- **Fixed-width SIMD** (128-bit packed SIMD, `v128`) is **Release 2.0** of the
  core specification.
- **Relaxed SIMD** is **Release 3.0** of the core specification (the 3.0 change
  history's "Relaxed Vector Instructions", cross-referenced to the
  `relaxed-simd` proposal).

Both are therefore part of what a conforming 3.0 engine must support.

### 2.1 The `v128` value type

- `v128` is the **only** new value type. It is a concrete 128-bit value with
  bits numbered 0–127; bit 0 is the LSB of the first of the 16 bytes
  (little-endian).
- The bits carry no intrinsic interpretation; each instruction says how to read
  them (`i8x16`, `i16x8`, `i32x4`, `i64x2`, `f32x4`, `f64x2`, or as a bag of
  bits).
- `v128.const` is a constant instruction, so a `v128` global can be
  initialised.

### 2.2 Immediates and validation

- `ImmByte` (0–255), `ImmLaneIdx2` (0–1), `ImmLaneIdx4` (0–3),
  `ImmLaneIdx8` (0–7), `ImmLaneIdx16` (0–15), `ImmLaneIdx32` (0–31).
  An out-of-range lane immediate is a **validation error**.
- `i8x16.shuffle` takes **sixteen** `ImmLaneIdx32` bytes; indices 0–15 select
  from the first operand and 16–31 from the second.
- Memory immediates use the normal `MemArg`; for a `v128` access the alignment
  must not exceed the natural alignment (16 for `v128.load`/`store`). An
  alignment *smaller* than natural is allowed and means an unaligned access.

### 2.3 Instruction families (fixed-width)

- memory: `v128.load`, `v128.load8x8_s/u`, `load16x4_s/u`, `load32x2_s/u`,
  `load8/16/32/64_splat`, `load32/64_zero`, `v128.store`,
  `v128.load{8,16,32,64}_lane`, `v128.store{8,16,32,64}_lane`;
- construction/access: `v128.const`, `i8x16.shuffle`, `i8x16.swizzle`,
  `*.splat`, `*.extract_lane[_s|_u]`, `*.replace_lane`;
- integer: add/sub/mul, neg/abs, `min/max_{s,u}`, `avgr_u`, `q15mulr_sat_s`,
  `extmul_{low,high}_{i8x16,i16x8,i32x4}_{s,u}`,
  `extadd_pairwise_*`, shifts (`shl`, `shr_s`, `shr_u`), bitwise
  and/or/xor/not/andnot, `bitselect`, all/any true, `bitmask`, `dot_i16x8_s`,
  the saturating `add_sat_/sub_sat_` forms;
- comparison: `eq/ne/lt/gt/le/ge` for the integer and float shapes;
- float: add/sub/mul/div, `min/max`, `pmin/pmax`, `sqrt`, `abs`, `neg`, `ceil`,
  `floor`, `trunc`, `nearest`;
- conversions: `i32x4.trunc_sat_f32x4_{s,u}`, `f32x4.convert_i32x4_{s,u}`,
  `i32x4.trunc_sat_f64x2_{s,u}_zero`, `f64x2.convert_low_i32x4_{s,u}`,
  `f32x4.demote_f64x2_zero`, `f64x2.promote_low_f32x4`,
  `i8x16/i16x8/i32x4/i64x2` narrowing/widening pairs.

Float semantics mirror the scalar rules: `roundTiesToEven`, wasm NaN
canonicalisation, and no trapping.

### 2.4 Relaxed SIMD

Relaxed SIMD adds instructions whose **result may depend on the
implementation** (hardware support, precision, ordering). The spec defines,
for each relaxed instruction, a **set** of allowed results; returning any
member is conformant. The set is parameterised by implementation parameters,
and a separate **deterministic profile** prescribes every parameter to be `0`,
pinning one specific result per instruction.

The instruction families are:

- `i8x16.relaxed_swizzle` (out-of-range lane behaviour is implementation-defined);
- `i32x4.relaxed_trunc_f32x4_{s,u}` and
  `i32x4.relaxed_trunc_f64x2_{s,u}_zero` (saturating vs hardware-defined);
- `f32x4`/`f64x2.relaxed_madd` and `.relaxed_nmadd` (single rounding with FMA,
  or double rounding without);
- `i8x16/i16x8/i32x4/i64x2.relaxed_laneselect`;
- `f32x4`/`f64x2.relaxed_min` and `.relaxed_max` (NaN / signed-zero choice);
- `i16x8.relaxed_q15mulr_s` (optional saturation);
- `i16x8.relaxed_dot_i8x16_i7x16_s` and
  `i32x4.relaxed_dot_i8x16_i7x16_add_s` (intermediate precision / order).

None of them traps or has side effects; only the produced bits vary.

## 3. What the spec does not demand

- **No hardware SIMD is required.** A conformant engine may lower every
  instruction to per-lane scalar code (or even a helper-function call). The
  spec fixes results, never machine instructions.
- **`v128` need not be a vector register.** It is a value with a defined
  bit layout; a 16-byte struct in memory is a valid representation, as is a
  pair of 64-bit integers.
- **No register allocation, vectorisation, or performance contract.** Nothing
  requires fusing adjacent `v128` operations or keeping values in registers.
- **Unaligned memory access is legal** whenever the memarg alignment is smaller
  than natural; only the memarg-vs-natural *validation* restriction is fixed.
- **No NaN-payload preservation.** Like scalar wasm, arithmetic NaN payloads
  are not reproducible; only the general NaN/rounding rules matter.
- **Relaxed SIMD is loose by construction.** Any result from the allowed set is
  conformant; there is no requirement to use an FMA, to pick a particular
  rounding, or to *detect* CPU features. Adopting the deterministic profile is
  optional; ignoring it is allowed.
- **No auto-vectorisation** of scalar loops is required, and no particular
  lowering of `i8x16.shuffle` (jump table, byte moves, or `pshufb` are all
  fine).
- **No JS API for a non-web embedder.** In the web embedding, `v128` values are
  not directly JS-marshalable; the core spec says nothing about host handling.
- **No requirement that `v128.const` be materialised at compile time** or as
  data; any correct lowering is fine.

## 4. What we have implemented today

### 4.1 Type and value representation

- `ValueTypeCode::V128` is parsed from the type section and accepted anywhere a
  value type is (function signatures, locals, globals, block types).
- `WASM::Value` (`WasmValue.hpp`) is `alignas(16)` and has a
  `uint8_t v128[16]` union member, so a `v128` fits its 16-byte payload.
- `LibJitTypeTranslator` lowers `v128` to a **struct of four `jit_type_int`**
  (`v128_definition`, created with `jit_type_create_struct(..., 4, 0)`) — a
  16-byte, 4-byte-aligned aggregate.
- In the GC struct layout (`WasmModuleInstance.cpp`) a `v128` field contributes
  `storageBytes = 16` and `storageAlign = 4`, with the comment "v128 lowers to a
  4-aligned struct of ints" (kept in sync with the JIT, §5/S4).

### 4.2 Opcodes

- `WasmOpcode.hpp` enumerates **236** `SIMDOpcode` entries covering the entire
  fixed-width set (loads/stores incl. lane/splat/zero, `v128.const`, shuffle,
  swizzle, splat/extract/replace, integer/float arithmetic and comparison, shifts,
  bitwise, conversions, saturating ops). **Relaxed SIMD is not enumerated at
  all.**
- `WasmOpcodeDispatcher::dispatchPrefixSIMD` decodes every immediate form
  correctly: `MemArg`, a raw lane-index byte, the 16 bytes of `v128.const`, and
  the 16 shuffle lanes. An unknown (or relaxed) sub-opcode throws
  `std::runtime_error("Invalid or unsupported 0xFD SIMD sub-opcode")`.
- **All 236 `LibJIT::OpcodeDispatcher` handlers are stubs** calling
  `notImplemented(__func__)` (`fprintf` + `std::abort()`). `v128.const`,
  `v128.load`/`store`, every arithmetic op, and **`global.get`/`global.set` of a
  `v128` global** all abort.
- The non-JIT `Stub::OpcodeDispatcher` implements the same handlers as textual
  pretty-printers (inspection only).

### 4.3 Constant expressions and globals

- `evalConstantExpr` handles only `i32/i64/f32/f64.const`, `global.get`,
  `ref.null`, `ref.func` and the GC constructors. **`v128.const` is not
  handled**, so a `v128` global initialiser throws during instantiation.
- `zeroConstantForType` returns a "best-effort" `nint` 0 for the `v128` struct
  type, with an explicit "expand when those opcodes are implemented" comment.

### 4.4 Tests

- `extern/WasmTestsuite` contains **59 `simd_*.wast` scripts**, but **none are in
  `Test/wast_supported.txt`**. Every SIMD script currently terminates with
  signal 6 (abort) after 0 passing commands, because instantiation compiles a
  function containing a SIMD opcode.
- The spec harness **cannot check SIMD results anyway**: `isScalarValue` skips
  `v128` arguments and results (see `docs/TESTING.md`), so a passing SIMD script
  would report mostly `skipped`, not `passed`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| S1 | **No SIMD execution at all.** All 236 fixed-width handlers abort via `notImplemented`. Any module touching `v128` dies at instantiation. | 0xFD block in `LibjitOpcodeDispatcher.cpp`; every `simd_*.wast` is signal 6. | High |
| S2 | **Relaxed SIMD is not enumerated or decoded.** `SIMDOpcode` has no relaxed entries; a relaxed sub-opcode throws `"Invalid or unsupported 0xFD SIMD sub-opcode"`. | `WasmOpcode.hpp`, `dispatchPrefixSIMD` default | High (3.0) |
| S3 | **`v128` globals and `v128.const` initialisers are unsupported.** `global.get`/`set` of a `v128` abort; `evalConstantExpr` has no `0xFD` arm. | `dispatchGlobalGet/Set`, `evalConstantExpr` | Medium |
| S4 | **Alignment disagreement.** The JIT lowers `v128` to a 4-byte-aligned `{i32,i32,i32,i32}` struct, and the GC struct layout uses `storageAlign = 4`, while `WASM::Value` is `alignas(16)`. A `v128` struct field or operand-stack value can therefore be under-aligned relative to 16. | `LibJitTypeTranslation.cpp`, `WasmModuleInstance.cpp`, `WasmValue.hpp` | Medium (latent) |
| S5 | **`zeroConstantForType` is wrong for `v128`.** It returns an integer 0 for the struct type, which cannot be a valid 16-byte zero. | `LibjitOpcodeDispatcher.cpp:467` comment | Medium |
| S6 | **No validation.** Lane indices, shuffle indices, and the "align ≤ natural" rule are never checked; an out-of-range lane immediate is used as-is. | no validating front-end (`docs/TESTING.md`) | Medium |
| S7 | **The harness cannot verify SIMD.** `isScalarValue` skips `v128` args/results, so even a working implementation would not turn the `simd_*.wast` scripts green without harness work. | `Test/WastScript.cpp`; `docs/TESTING.md` | High (for progress measurement) |
| S8 | **No test coverage in the curated set** and no manual SIMD test. | `Test/wast_supported.txt` | High (for regression safety) |
| S9 | **`v128` calling convention is unverified.** Multi-value/struct ABI for `v128` parameters and returns (and v128 in globals/tables) has never been exercised. | no test passes a `v128` | Medium |

## 6. Where we can and should play fast and loose

SIMD is the feature where the spec's freedom (§3) is least used today: the
engine is scalar and single-threaded, and this project is a QVM successor, not
a graphics kernel.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SD1 | **Scalar/helper lowering first.** Represent `v128` as four `u32` and implement every op as a native helper (or an inline loop over lanes), exactly like the GC native helpers. | §3: no hardware SIMD is required; results are what matter. | A portable, testable baseline in far less code than intrinsics. |
| SD2 | **Add hardware fast paths only behind runtime feature detection, keeping the helpers as fallback.** Start with the hottest shapes (memory, bitwise, i32x4/f32x4 arith). | Any correct fallback is conformant. | Performance when it matters, without a porting matrix. |
| SD3 | **`v128.load`/`store` as a 16-byte `memcpy`; ignore the memarg alignment** (only enforce it if we ever add validation). | §3: unaligned access is legal when align < natural; x86 tolerates all alignments. | One `memcpy` per access, no alignment handling. |
| SD4 | **`i8x16.shuffle` via a 16-iteration helper** (index < 16 → operand 0, else operand 1). | §2.2; any correct lowering is fine. | Correctness first; `pshufb` later. |
| SD5 | **Relaxed SIMD: pick the plain scalar result for every op** — unfused (double-rounded) `relaxed_madd`, saturating `relaxed_trunc`, zeroing `relaxed_swizzle`, bitwise `relaxed_laneselect`, scalar-wasm NaN rules for `relaxed_min/max`, saturating `relaxed_q15mulr_s`, and the exact `relaxed_dot`/`dot_add`. | §2.4/§3: any allowed result is conformant; the deterministic profile is optional. | No hardware dependence, reproducible results, trivial code. |
| SD6 | **Optionally adopt the deterministic profile later** (spec prescribes every relaxed parameter to `0`) if we want byte-for-byte reproducibility across machines. | It is a refinement of SD5, never a requirement. | Cross-machine determinism for replays/networking — very much in the QVM spirit. |
| SD7 | **Pick one alignment and use it everywhere.** Simplest: keep the 4×`u32` struct (4-aligned) and make all memory access unaligned-safe; do *not* claim 16-byte alignment for stack/temporary values. | §3: no alignment guarantee is required of the representation, only of the memory-access rule. | Removes an ABI trap (S4) without changing the type. |
| SD8 | **Do not implement a `v128` JS API.** | §3: web embedding only, and not required here. | Scope control. |
| SD9 | **Order the work by what producers emit** (memory, bitwise, integer arith, splat/lane, float arith, conversions, then relaxed), not by the spec's table order. | Conformance is all-or-nothing only if we claim a 3.0 profile; the subset is still usable. | Fastest path to a usable engine. |

**Guardrails.**

- Little-endian lane numbering everywhere; `v128.const` bytes go in bit order
  0–7 first.
- `i8x16.shuffle` indices 0–15/16–31, `*.swizzle` zeroes out-of-range lanes.
- `trunc_sat` saturates; the non-`sat` forms trap — do not conflate them.
- Float ops use `roundTiesToEven` and wasm NaN rules; `relaxed_min/max` must
  still choose a member of the allowed set (the scalar result is one).
- Keep `v128` ABI consistent between `LibJitTypeTranslator`, the operand
  stack, and any struct layout (S4/S9).
- Validate lane immediates once, at decode, even though validation is skipped
  elsewhere — the cost is trivial and the failure mode otherwise is silent
  out-of-range lane access.

## 7. Plan

Ordered by what unblocks the most; `SIMD-n` is independent of the other
documents' milestone names.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SIMD-0 — ABI decision** | Fix the `v128` representation/alignment (pick 4- or 16-byte alignment and apply it in the JIT lowering, `Value`, and the GC layout); add a manual test that passes and returns a `v128`. | S4, S5, S9 | A function `(param v128) (result v128)` round-trips through the JIT. |
| **SIMD-1 — Core scalar lowering** | `v128.const`, `v128.load`/`store`, bitwise ops, `*.splat`, extract/replace lane, `i8x16.shuffle`/`swizzle`, via native helpers. | S1 (partly) | A hand-written module computes a byte shuffle and a bitselect correctly. |
| **SIMD-2 — Integer ops** | add/sub/mul, `min/max`, shifts, comparisons, `all_true`/`any_true`/`bitmask`, `avgr_u`, saturating add/sub, `q15mulr_sat_s`. | S1 | The `simd_i32x4_*` / `simd_i16x8_*` attribute scripts pass their i32-returning assertions. |
| **SIMD-3 — Float ops** | arithmetic, `min/max`/`pmin/pmax`, `sqrt`/`abs`/`neg`, rounding, comparisons, conversions (`trunc_sat`, `convert`, widen/narrow, promote/demote). | S1 | The `simd_f32x4_*` / `simd_f64x2_*` scripts pass. |
| **SIMD-4 — Globals and constants** | `v128.const` in `evalConstantExpr`; `global.get`/`set` of `v128`. | S3 | A module with a `v128` global initialised by `v128.const` runs. |
| **SIMD-5 — Relaxed SIMD** | Enumerate the relaxed sub-opcodes; decode them; implement the SD5 scalar results. | S2 | A module using each relaxed family produces a member of the allowed set. |
| **SIMD-6 — Harness** | Marshal `v128` arguments/results (16-byte values, lane formatting) so `simd_*.wast` can be graded; add passing scripts to `wast_supported.txt`. | S7, S8 | `./WasmJit 'spec: simd_*'` reports real pass/fail counts. |
| **SIMD-7 — Optional: intrinsics** | SSE/AVX2 fast paths behind `__builtin_cpu_supports`, helpers as fallback. | — | A microbenchmark shows the expected speedup. |

Guardrails: keep the scalar helpers as the reference implementation and the
fallback; run `./WasmJit '~[spec]'` after each step.

## 8. Indicative test survey

Survey taken with `build/Desktop-Debug/Test/WasmJit` (2026-10-04).

| Script | Result | Reading |
|---|---|---|
| `simd_load` | signal 6, 0 passing commands | S1 |
| `simd_const` | signal 6, 0 passing commands | S1, S3 |
| `simd_bitwise` | signal 6, 0 passing commands | S1 |
| `simd_lane` | signal 6, 0 passing commands | S1 |
| `simd_splat` | signal 6, 0 passing commands | S1 |

All 59 `simd_*.wast` scripts behave the same way: they abort while the first
module is being compiled. None are in `Test/wast_supported.txt`, and none can
be graded until SIMD-6 because the harness skips `v128` values. Reproduce one
row with:

```bash
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: simd_load' -r compact
```

## 9. References

Specification (WebAssembly 3.0, living draft):

- Appendix — Change History: "Vector Instructions" is a Release 2.0 feature;
  "Relaxed Vector Instructions" is a Release 3.0 feature:
  https://webassembly.github.io/spec/core/appendix/changes.html
- Syntax — Vector instructions (fixed-width and relaxed):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- Execution — Numerics, incl. the "Relaxed Operations" section and the
  parameterisation used by the deterministic profile:
  https://webassembly.github.io/spec/core/exec/numerics.html
- Appendix — Profiles (the deterministic profile pins every relaxed parameter
  to `0`):
  https://webassembly.github.io/spec/core/appendix/profiles.html

Proposal documents:

- Fixed-width SIMD (the design document behind the 2.0 feature, incl. lane
  interpretations and immediates):
  https://github.com/WebAssembly/simd/blob/main/proposals/simd/SIMD.md
- Relaxed SIMD:
  https://github.com/WebAssembly/relaxed-simd/blob/main/proposals/relaxed-simd/Overview.md

In-repo:

- [`docs/GC.md`](GC.md) — the companion design note (and §4.4/SIMD-0 interplay
  for `v128` inside aggregates).
- [`docs/THREADS.md`](THREADS.md) — the companion note for the other
  "post-2.0 / non-3.0" feature area.
- [`docs/TESTING.md`](TESTING.md) — why `v128` values are currently skipped by
  the spec harness.
- Source anchors: `WasmBase/WasmType.hpp` (`ValueTypeCode::V128`),
  `WasmBase/WasmValue.hpp` (`Value::v128`), `WasmBase/WasmOpcode.hpp`
  (`SIMDOpcode`), `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixSIMD`),
  `WasmBase/WasmModuleInstance.cpp` (`evalConstantExpr`, struct layout),
  `LibJit/LibJitTypeTranslation.cpp` (`v128_definition`),
  `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}` (0xFD stub block and
  `zeroConstantForType`), `WasmStub/StubOpcodeDispatcher.cpp`
  (pretty-printers), `Test/wast_supported.txt`.
