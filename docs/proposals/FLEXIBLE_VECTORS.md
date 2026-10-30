# Flexible Vectors — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — the upstream
proposal now has a binary draft, but its lane prefixes are already core
opcodes in WebAssembly 2.0/3.0 and its value-type bytes collide with GC packed
types; implementation is therefore blocked before FV-2).
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
- The current binary draft defines six value types (`vec.i8`, `vec.i16`,
  `vec.i32`, `vec.i64`, `vec.f32`, `vec.f64`) and six lane-type instruction
  prefixes (`0x7A`–`0x75`), followed by a LEB128 `vecop`.
- It remains explicitly early-stage: "it is expected to evolve".

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
- The upstream binary draft was reviewed. Its **instruction prefixes are not
  free**:
  - `0x7A` is `i64.ctz`,
  - `0x79` is `i64.clz`,
  - `0x78` is `i32.rotr`,
  - `0x77` is `i32.rotl`,
  - `0x76` is `i32.shr_u`,
  - `0x75` is `i32.shr_s`.

  Treating those bytes as a new instruction prefix would reinterpret existing
  valid modules. The draft's value types collide as well: `0x78`/`0x77` are the
  engine's GC packed `i8`/`i16` storage codes, and `0x75`/`0x76` are the
  engine's provisional Constant-Time markers (which could be moved, but the
  core-opcode collision remains).
- Because there is no safe byte string that means "flexible vector length"
  rather than an existing instruction, no decoder arm or JIT lowering was
  added. A test would have to assert one interpretation or the other, which is
  exactly the ambiguity `SAFETY.md` forbids.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| FV1 | **No safe encoding to implement.** The draft's lane prefixes `0x7A`–`0x75` are existing core opcodes (`i64.ctz`…`i32.shr_s`), and its value types collide with GC packed storage (`0x78`/`0x77`); decoding them would silently change existing modules. | upstream `BinaryFlexibleVectors.md`; `WasmOpcode.hpp` (`Opcode`); `WasmType.hpp` (`I8`/`I16`) | **Blocker** |
| FV2 | **No runtime vlen concept.** `v128` is fixed at 128 bits everywhere. | `WasmType.hpp`, `WasmOpcode.hpp` (`SIMDOpcode`) | Low |
| FV3 | **No mask/tail model.** The extensions that would make flexible vectors usable are explicitly open. | proposal Overview | Low |
| FV4 | **No test or script.** The snapshot has no flexible-vector script. | `extern/WasmTestsuite` | Low |

The blocker is a **multi-proposal encoding collision**, not a missing
mechanism: resolving it requires either the proposal to move its prefixes/types
or a project-wide extension encoding that cannot be mistaken for core opcodes
and GC packed types. This engine deliberately does not invent that encoding.

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| FVD1 | **Treat the current engine as a `vlen = 128` implementation.** | §2 fixes the length at runtime; §3 leaves the lowering free, so once encodings exist 128 is a legal instance. | No change to the shipped SIMD surface. |
| FVD2 | **Do not emulate wider vectors by chaining `v128` ops.** | §3/§2: the observable vlen and masking semantics would be wrong. | No false conformance claims. |
| FVD3 | **Keep the width decision in the backend, not in `WasmBase/`.** | §3: the lowering is free; `docs/LAYERING.md` keeps the width decision in the backend. | AsmJit/SlJIT can pick a different width later. |
| FVD4 | **Reject the draft's opcodes until the encoding is collision-free.** | `SAFETY.md`: unknown bytes must not be guessed at, and the draft reuses existing core opcodes. | Forward compatibility and no silent reinterpretation of old modules. |

Guardrail: a 128-bit engine may claim to be *an instance* of a length-agnostic
semantics only after the proposal defines a collision-free encoding. It must not
claim to implement the proposal today, and it must not steal an existing core
opcode prefix.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **FV-1 — Resolve the encoding collision** | The proposal or the project must move the vector prefixes/value types off `0x7A`–`0x75` (core opcodes and GC packed storage). | FV1 | A byte string unambiguously means a flexible-vector instruction. |
| **FV-2 — Decode the ops** | Add the length-agnostic opcodes and a vlen field to the module/runtime configuration. | FV2 | A module decodes and reports its configured vlen. |
| **FV-3 — Lower to native width** | Emit the widest native vector the backend supports, with a scalar fallback. | FV2, FV3 | Ops produce correct results at 128 and 256 bits. |
| **FV-4 — Tests** | Add width-parametrized tests once the proposal has a suite. | FV4 | The same module passes at two vlen settings. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `simd_*` family (66 scripts) | all green | 0 | per-script | the fixed 128-bit substrate is complete |
| flexible-vector script | — | — | — | none in the snapshot; blocked by FV1 |

Reading: the fixed-width SIMD surface is the foundation, not evidence for this
proposal. There is nothing to grade until FV1 resolves the encoding collision.

## 9. References

- Proposal overview (`WebAssembly/flexible-vectors`):
  https://github.com/WebAssembly/flexible-vectors/blob/main/proposals/flexible-vectors/Overview.md
- Binary draft (the collision source):
  https://github.com/WebAssembly/flexible-vectors/blob/main/proposals/flexible-vectors/BinaryFlexibleVectors.md
- Spec — Vector instructions:
  https://webassembly.github.io/spec/core/exec/instructions.html
- In-repo: [`SIMD.md`](SIMD.md), [`../SIMD_LIBJIT.md`](../SIMD_LIBJIT.md),
  [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md), [`../LAYERING.md`](../LAYERING.md);
  sources `WasmBase/WasmType.hpp` (`v128`), `WasmBase/WasmOpcode.hpp`
  (`SIMDOpcode`), `LibJit/LibjitOpcodeDispatcher.cpp`.
