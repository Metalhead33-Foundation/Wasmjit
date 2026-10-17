# Flexible Vectors — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — the proposal is
early-stage and defines no encodings; only fixed 128-bit SIMD is implemented).
Audience: anyone touching `WasmType`, `WasmOpcode` (SIMD), the SIMD helpers, or
the JIT backend selection.

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

Flexible Vectors proposes length-agnostic vector operations beyond 128 bits.

- The vector length is **not known at compile time** but is fixed for a given
  runtime instance (the base variant).
- The program uses the same operations regardless of the hardware width; an
  engine lowers them to the widest native vector it has.
- The proposal aims to cover SIMD instruction sets beyond fixed 128-bit SIMD,
  and leaves masks, runtime length adjustment and exposing the length to
  Wasm/JS as open extensions.
- It is explicitly an early-stage design: "it is expected to evolve", and no
  instruction encodings are defined yet.

## 3. What the proposal does not demand

- No fixed vector length, no fixed instruction set and no encoding yet.
- No particular lowering; scalar emulation of wide vectors is allowed.
- No requirement that 128-bit engines change their existing SIMD.
- No masks, tail handling or vlen query in the base variant.
- No conformance suite until the proposal defines one.

## 4. What we have implemented today

- Fixed-width **128-bit SIMD** is complete: all 236 handlers execute, all
  `simd_*` and relaxed scripts in the snapshot are green, and `SIMD.md` records
  the lowering (`docs/SIMD.md`, `docs/SIMD_LIBJIT.md`).
- The value type is `v128` with a fixed lane layout; there is no vector-length
  concept, no runtime vlen and no length-agnostic opcode.
- SIMD helpers are chosen at JIT time (LibJIT today; AsmJit/SlJIT are deferred
  levers), so a future width could be selected per backend without changing
  `WasmBase/`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| FV1 | **No length-agnostic type or instruction set.** The proposal has not assigned encodings, so there is nothing to decode. | `flexible-vectors` Overview | Informational |
| FV2 | **No runtime vlen concept.** `v128` is fixed at 128 bits everywhere. | `WasmType.hpp`, `WasmOpcode.hpp` (`SIMDOpcode`) | Low |
| FV3 | **No mask/tail model.** The extensions that would make flexible vectors usable are explicitly open. | proposal Overview | Low |
| FV4 | **No test or script.** The snapshot has no flexible-vector script. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| FVD1 | **Treat the current engine as a `vlen = 128` implementation.** | §2 fixes the length at runtime; §3 leaves the lowering free, so once encodings exist 128 is a legal instance. | No change to the shipped SIMD surface. |
| FVD2 | **Do not emulate wider vectors by chaining `v128` ops.** | §3/§2: the observable vlen and masking semantics would be wrong. | No false conformance claims. |
| FVD3 | **Keep the width decision in the backend, not in `WasmBase/`.** | §3: the lowering is free; `docs/LAYERING.md` keeps the width decision in the backend. | AsmJit/SlJIT can pick a different width later. |
| FVD4 | **Reject the future opcodes until encodings stabilize.** | §3: no encoding is demanded yet; `SAFETY.md`: unknown bytes must not be guessed at. | Forward compatibility. |

Guardrail: a 128-bit engine may claim to be *an instance* of a length-agnostic
semantics only after the proposal defines the operations and a vlen query. It
must not claim to implement the proposal today.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **FV-1 — Track the proposal** | Wait for a concrete instruction set and encoding; record the decision in `SIMD.md`. | FV1 | Encodings exist and are stable. |
| **FV-2 — Decode the ops** | Add the length-agnostic opcodes and a vlen field to the module/runtime configuration. | FV2 | A module decodes and reports its configured vlen. |
| **FV-3 — Lower to native width** | Emit the widest native vector the backend supports, with a scalar fallback. | FV2, FV3 | Ops produce correct results at 128 and 256 bits. |
| **FV-4 — Tests** | Add width-parametrized tests once the proposal has a suite. | FV4 | The same module passes at two vlen settings. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `simd_*` family (66 scripts) | all green | 0 | per-script | the fixed 128-bit substrate is complete |
| flexible-vector script | — | — | — | none in the snapshot |

Reading: the fixed-width SIMD surface is the foundation, not evidence for this
proposal. There is nothing to grade until FV-1 completes.

## 9. References

- Proposal overview (`WebAssembly/flexible-vectors`):
  https://github.com/WebAssembly/flexible-vectors/blob/main/proposals/flexible-vectors/Overview.md
- Spec — Vector instructions:
  https://webassembly.github.io/spec/core/exec/instructions.html
- In-repo: [`SIMD.md`](SIMD.md), [`../SIMD_LIBJIT.md`](../SIMD_LIBJIT.md),
  [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md), [`../LAYERING.md`](../LAYERING.md);
  sources `WasmBase/WasmType.hpp` (`v128`), `WasmBase/WasmOpcode.hpp`
  (`SIMDOpcode`), `LibJit/LibjitOpcodeDispatcher.cpp`.
