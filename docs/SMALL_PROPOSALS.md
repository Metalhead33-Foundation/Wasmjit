# Small Phase 5 Proposals: Sign-extension, Non-trapping Float→Int, Branch Hinting

Status: design note / implementation reference (revision 2 — §2/§3/§4 status
updated after SP-1/SP-2).
Audience: anyone touching `LibjitOpcodeDispatcher`, `WasmOpcode`,
`WasmModule` (custom sections), or the test harness.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Covers three small Phase 5 proposals that do not
each warrant a full document. Contains **no new code**.

## 1. Document map

| Section | Proposal |
|---|---|
| §2 | Sign-extension Operators |
| §3 | Non-trapping Float→Int Conversions |
| §4 | Branch Hinting |
| §5 | Cross-cutting "fast and loose" |
| §6 | Plan |
| §7 | Indicative test survey |
| §8 | References |

All three are **Phase 5**; sign-extension and non-trapping conversion are in
**2.0**, branch hinting is in **3.0**. Each §2–§4 below lists demands,
non-demands, what is implemented, and the gaps.

## 2. Sign-extension Operators

**Demands.** Five instructions: `i32.extend8_s`, `i32.extend16_s`,
`i64.extend8_s`, `i64.extend16_s`, `i64.extend32_s`. Each sign-extends a
narrow value already in a wider register to the register's width.

**Does not demand.** Any particular lowering (shift pair, `movsx`, masked
arithmetic); no trap; no change to other instructions.

**Implemented.** All five handlers exist and emit real code
(`dispatchI32Extend8S`/`16S`, `dispatchI64Extend8S`/`16S`/`32S`,
`LibjitOpcodeDispatcher.cpp` ~2405–2438), e.g. `jit_insn_convert` to
`jit_type_sbyte`/`short` then back to the full width.

**Gaps.** None. There is still **no dedicated spec script** for this proposal
in the testsuite snapshot (the ops are covered indirectly by `i32`/`i64`), but a
focused manual regression test now exists: `Test/wasm_wat/sign_extension.wat`
plus the `sign-extension operators sign-extend the low bits` case in
`Test/main.cpp`, which asserts every op at its 7/8/15/16/31/32-bit boundaries.

## 3. Non-trapping Float→Int Conversions

**Demands.** Eight instructions: `i32/i64.trunc_sat_f32/f64_s/u`. They differ
from `trunc_f*` only in the failure cases:
- `NaN` → `0`;
- a value below the type's minimum → the minimum;
- a value above the maximum → the maximum.
No trap is ever taken.

**Does not demand.** Any particular lowering; no new value types; no change to
the trapping `trunc_f*` forms (which must keep trapping).

**Implemented.** All eight helper functions exist
(`wasm_i32_trunc_sat_f32_s/u`, … `wasm_i64_trunc_sat_f64_s/u`) and are wired
through `callNativeUnary` in `dispatchI32TruncSatF32S` … `dispatchI64TruncSatF64U`.
Each clamps on `NaN`/range and returns the clamped value.

**Gaps.**
- No dedicated spec script either; coverage is indirect via `conversions`
  (520 passed / 7 failed). The 7 `conversions` failures are the known
  *conversion* bugs (`i64.extend_i32_u`, `f32/f64.convert_i64_u`;
  `docs/TESTING.md`), **not** `trunc_sat`.
- ~~`dispatchI32TruncSatF32U` converts the unsigned result down to
  `jit_type_int` explicitly, while the `_s` form returns `jit_type_int`
  directly; verify the widths are consistent for the `i64` forms too.~~
  **Checked.** The `_u` handlers use `jit_type_uint`/`jit_type_ulong` for the
  native call and convert to `jit_type_int`/`jit_type_long` only to re-tag the
  same-width bit pattern; the widths are consistent. A focused manual
  regression test now exists: `Test/wasm_wat/trunc_sat.wat` plus the
  `non-trapping float-to-int conversions saturate instead of trapping` case in
  `Test/main.cpp` (NaN, ±inf, ±overflow, exact in-range).

## 4. Branch Hinting

**Demands.** A custom section (`metadata.code.branch_hint`) that maps
`(funcidx, byte-offset)` to a hint byte: `0` = "branch unlikely",
`1` = "branch likely". The text format exposes the same via
`(@metadata.code.branch_hint …)` annotations. It attaches to conditional
instructions such as `if`/`br_if`.

**Does not demand.** Any observable behaviour. It is a **pure optimisation
hint**: an engine may parse it, act on it, or ignore it entirely, with no
effect on results, traps, or validation. A custom section is not part of the
module's semantics.

**Implemented.** Custom sections are walked by `Module::processSecetions` and
dispatched to `processCustomSection`, which handles the `name` section and now
also `metadata.code.branch_hint`. The parser (`Module::processBranchHintSection`)
reads the `(funcidx, {offset, length, payload})` entries and
`Module::processCodeSection` rebases each offset onto `FunctionBody::code`. The
LibJIT dispatcher records the current instruction offset (`instructionOffset`,
set by `OpcodeDispatcher::readCode`) and consults the hints in `dispatchBrIf`:
a *forward* conditional whose hint says "likely true" is emitted in the
inverted form so the likely path is the fall-through. Hint-less modules compile
byte-for-byte as before.

**Gaps.** No block *reordering*: LibJIT exposes no branch-prediction or
block-layout API and the emitter is single-pass, so the then/else bodies of an
`if` cannot be swapped even when a hint asks for it. `if` hints are therefore
parsed but do not change layout, and "measurable improvement" is not asserted
in CI (see §6 SP-2).

## 5. Cross-cutting "fast and loose"

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SPD1 | **Lower sign-extension with native width conversions** (already done). | §3/§2: lowering is free. | Minimal code. |
| SPD2 | **Lower `trunc_sat` with clamp helpers** (already done). | §3: no traps required; semantics are a pure function. | Simple and correct. |
| SPD3 | **Ignore branch hints *semantically*.** | §4: hints are not semantics; acting on them or not is explicitly allowed. | Conformance without changing results. |
| SPD4 | **Use a hint only where it is a strict win** (branch polarity for a forward `br_if`; never block reordering). | §4; optional. | Bounded, semantics-preserving work. |
| SPD5 | **No validation of hint sections.** | §4: custom sections are outside core validation. | Nothing to validate. |

Guardrail: do not conflate `trunc_f*` (trapping) with `trunc_sat_f*`
(never traps); keep the `NaN → 0` and clamp rules exactly.

## 6. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SP-1 — Manual test** | ✅ `Test/wasm_wat/sign_extension.wat` + `Test/wasm_wat/trunc_sat.wat`, exercised by two cases in `Test/main.cpp`. | no focused coverage | Done: both cases assert the boundary values (NaN, ±overflow, exact). |
| **SP-2 — (optional) Branch hints** | ◑ `metadata.code.branch_hint` is parsed/rebased and consulted in `dispatchBrIf` (forward-branch polarity). No block reordering. | optional codegen | Parser covered by `branch hints are parsed and do not change results`; a *measurable* win is out of scope (see §4 gaps). |

## 7. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | Result | Reading |
|---|---|---|
| `conversions` | 520 passed / 7 failed | the 7 are the known conversion bugs, not `trunc_sat` |
| `i32` / `i64` | no report (exit 1) | scripts exercise the ops but fail to load for unrelated reasons |
| sign-extension script | none in the testsuite snapshot | no dedicated spec coverage; manual coverage added (SP-1) |
| `trunc_sat` script | none in the testsuite snapshot | no dedicated spec coverage; manual coverage added (SP-1) |
| branch-hint script | none in the testsuite snapshot | hints are untestable as semantics; the manual `branch_hint_loop` case checks parsing + non-interference (SP-2) |

## 8. References

- Sign-extension operators:
  https://github.com/WebAssembly/sign-extension-ops/blob/master/proposals/sign-extension-ops/Overview.md
- Non-trapping float-to-int conversions:
  https://github.com/WebAssembly/nontrapping-float-to-int-conversions/blob/master/proposals/nontrapping-float-to-int-conversions/Overview.md
- Branch hinting:
  https://github.com/WebAssembly/branch-hinting/blob/master/proposals/branch-hinting/Overview.md
- Spec — Change History (Release 2.0; Release 3.0 keeps hints non-semantic):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Custom sections and annotations:
  https://webassembly.github.io/spec/core/appendix/custom.html
- In-repo: [`docs/GC.md`](GC.md) / [`docs/SIMD.md`](SIMD.md) (same structure);
  sources `WasmBase/WasmOpcode.hpp` (opcode enum, `PrefixMisc`),
  `WasmBase/WasmModule.cpp` (`processCustomSection`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchI32Extend8S` …,
  `dispatchI32TruncSatF32S` …), `Test/wast_supported.txt`,
  `docs/TESTING.md`.
