# SIMD (Fixed-width and Relaxed) {#prop_simd}

*Implementation Notes, Spec Status, and Deliberate Deviations*

Status: design note / implementation reference (revision 5 — fixed-width and
relaxed SIMD are implemented and validated; S6's relaxed ternary rules are
typed, and the only remaining milestones are the optional SIMD-7 performance
measurement and the optional SIMD-8 native path).
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
  (`v128_definition`, created with `jit_type_create_struct(..., 4, 0)`) and then
  forces **16-byte size and 16-byte alignment** with
  `jit_type_set_size_and_alignment` (SIMD-L1).
- In the GC struct layout (`WasmModuleInstance.cpp`) a `v128` field contributes
  `storageBytes = 16` and `storageAlign = 16` through `gcStorageAlign`
  (`WasmGcHeader.hpp`), so the runtime and the backend derive the same layout from
  one place (invariant H1).

### 4.2 Opcodes

- `WasmOpcode.hpp` enumerates **236** fixed-width `SIMDOpcode` entries covering
  the entire fixed-width set (loads/stores incl. lane/splat/zero, `v128.const`,
  shuffle, swizzle, splat/extract/replace, integer/float arithmetic and
  comparison, shifts, bitwise, conversions, saturating ops) **plus the 20 relaxed
  sub-opcodes** (`0x100`–`0x113`, values 256–275).
- `WasmOpcodeDispatcher::dispatchPrefixSIMD` decodes every immediate form:
  `MemArg`, a raw lane-index byte, the 16 bytes of `v128.const`, and the 16
  shuffle lanes. An unknown sub-opcode throws
  `std::runtime_error("Invalid or unsupported 0xFD SIMD sub-opcode")`.
- Every one of those handlers executes: the `0xFD` block in the backend has no
  `notImplemented` left. Each one lowers to a call into the shared helper layer
  (`WasmBase/wasm_simd_vector.*`, SD5); that layer's twin scalar implementation
  (`WasmBase/wasm_simd_scalar.*`) is the differential oracle in the tests, and a
  backend with its own vector registers may lower the same op natively instead
  (§4.6 below).
- The non-JIT `Stub::OpcodeDispatcher` implements the same handlers as textual
  pretty-printers (inspection only).

### 4.3 Constant expressions and globals

- `evalConstantExpr` decodes the `0xFD`/`0x0C` arm into the `Value`'s 16 bytes, so
  a `v128.const` initialiser works.
- `dispatchGlobalGet`/`Set` treat a `v128` global as what it is — 16 bytes at
  offset 0 of its cell — and reuse the `v128.load`/`v128.store` helpers rather
  than inventing a struct load.

### 4.4 Tests

- **All 66 `simd_*` / relaxed scripts are curated and pass with `failed=0`**
  (`Test/wast_supported.txt`); the harness decodes `v128` arguments lane by lane
  and compares results with the `nan:canonical` / `nan:arithmetic` policies
  applied per lane (`Test/WastScript.cpp`).
- Four layers carry the detail §4.9 below: the
  differential oracle (T1), the `Test/wasm_wat/simd_*.wat` fixtures (T2), the
  spec scripts (T3) and the tagged benchmark (T4). See
  [`../TESTING.md`](../TESTING.md) Part 3.

### 4.5 The helper ABI (the portable baseline)

All SIMD helpers are `extern "C"`, take **pointers instead of values**, and never
trap (memory helpers use the existing bounds check). Four shapes:

| Shape | Signature | Examples |
|---|---|---|
| Unary | `void h(const void* a, void* out)` | `v128.not`, `i32x4.abs`, `f32x4.sqrt` |
| Binary | `void h(const void* a, const void* b, void* out)` | `i32x4.add`, `v128.and`, `i8x16.swizzle` |
| Ternary | `void h(const void* a, const void* b, const void* c, void* out)` | `v128.bitselect`, `*.relaxed_laneselect` |
| Scalar result | `int32_t h(const void* a[, …])` | `*.extract_lane_s/u`, `i8x16.bitmask`, `v128.all_true` |

- **Immediates are scalars.** A lane index is `uint32_t`; a 16-byte immediate
  (`i8x16.shuffle`, `v128.const`) is two `uint64_t`.
- **Memory helpers take the context, resolved address, then `out`**:
  `void h(VMContext*, uint32_t memIdx, uint32_t addr, void* out)`.
- **`v128` is 16 bytes, 16-aligned, little-endian** in storage
  (`WasmGcHeader.hpp`); a helper result is written to a fresh 16-byte slot.
- **Four fusion entry points** (`wasm_v128_load_unary`, `_load_binary`,
  `_load_binary_rhs`, `_load2_binary`) copy an already-bounds-checked source into
  a local 16-byte value and call the consumer. They remove a call boundary, not
  semantics.
- **The scalar layer** (`WasmBase/wasm_simd_scalar.*`) is the oracle: every op is
  implemented there without a SIMD library and always compiled. The vector layer
  (`WasmBase/wasm_simd_vector.*`) is the only place a SIMD library is visible.

### 4.6 The LibJIT lowering: D3 + D5

| # | Design | Verdict |
|---|---|---|
| **D1** | Inline scalar IR per lane | Rejected as general: cross-lane ops have no per-lane decomposition. Kept only for `v128.const`, written inline through `jit_insn_address_of` |
| **D2** | Helper with `v128` by value | Rejected as primary: depends on LibJIT’s aggregate ABI matching the host. Benchmark challenger |
| **D3** | Helper taking pointer arguments | **Chosen**: no aggregate-ABI risk; the signature is only `void*`/`uint32_t` |
| **D4** | Batched/fused run helper | **Deferred, unbuilt**: only pays if boundary cost dominates; the measurement says it does not |
| **D5** | D3 + producer/consumer fusion | **Chosen and shipped**: `v128.const` inline, `load` fused into the following unary/binary helper |

Storage is LibJIT’s own frame slots (`jit_value_create(fn, v128Type)`), one per
static definition site, not a dynamic `alloca` bank: a bank would grow per loop
iteration and needs a stack-height pre-pass the compiler does not compute.
`v128.load`/`store`, splat and lane access stay helper calls because a 16-byte
aggregate `jit_insn_load_relative` is not supported by this build (tested:
SIGSEGV).

### 4.7 The SIMD library and the core files

**Highway 1.4.0** is the vector implementation: Apache-2.0 (the project licence),
runtime dispatch (`HWY_EXPORT`/`HWY_DYNAMIC_DISPATCH`), a portable
`EMU128`/`SCALAR` fallback, `CappedTag<T, N>` to keep vectors exactly 128 bits,
and coverage of the hard families (shuffle/swizzle, dot, extmul, q15mulr,
saturating arithmetic, narrow/widen). **SIMDe** is the documented escape hatch
(identical x86 code, header-only) but brings no dispatch. Raw intrinsics plus
`__builtin_cpu_supports` is the reference implementation the oracle represents.

| Path | Role |
|---|---|
| `WasmBase/wasm_simd_scalar.{hpp,cpp}` | scalar oracle and the `extern "C"` contract |
| `WasmBase/wasm_simd_vector.{hpp,cpp}` | vectorised bodies, entry points and dispatch |
| `WasmBase/wasm_simd_ops.inc` | one line per op; expanded once per layer |
| `LibJit/LibjitOpcodeDispatcher.*` | helper-call emission for LibJIT |
| `Sljit/`, `Asmjit/` | the same helper calls from their dispatchers |

### 4.8 Relaxed SIMD and the laxness budget

Only **L0** (fixed-width) is strict. `L1` (relaxed) is loose by construction, and
`L2` (harness) / `L3` (launcher) are free. A scalar result is conformant, and the
deterministic profile can be applied later.

Correctness traps that are **not** loose:

1. `f32x4.min`/`max` and `pmin`/`pmax` have different `±0` and NaN rules; Intel
   and ARM differ, and wasm does not allow the hardware result unexamined.
2. `i8x16.shuffle` indices 0–15 read the first operand, 16–31 the second.
3. `swizzle` zeroes out-of-range lanes; `shuffle` cannot have them; relaxed
   swizzle *is* loose.
4. `trunc_sat_*` saturates; `trunc_f*` must trap.
5. Float rounding is `roundTiesToEven`; NaN canonicalisation applies to vector
   float ops exactly as to scalar.
6. Lane immediates are validated at decode; it is the cheapest memory-safety
   guard in the feature.

Other measured hazards: `q15mulr_sat_s`’s `-32768 × -32768` corner (SSE2 and
SSSE3 paths disagree), packing lane order (`narrow`/`widen`/`extmul`), 64-bit
lane shifts/comparisons on baseline SSE2, and `bitmask`/`all_true` per-width
reductions.

### 4.9 Measured cost and the batched path

- The D5 peepholes removed half the calls in the measured workload:
  **4.10 → 3.47 ns/op**. The whole helper crossing is ~12 cycles and nearly all
  of it is the call (two nested dispatches), not the arithmetic; the slot
  round-trip is minor.
- **D4 was left unbuilt** because removing a call wins more than making the
  callee cheaper, and no measured case justified a batched ABI.
- **A release build found a real relaxed-op defect:** `relaxed_madd`/`nmadd`
  compiled to an FMA under GCC while the pinned reference stayed unfused, so debug
  and release disagreed. The four ops now forward to the reference, making the
  permitted choice structural.
- The test layers are T1 differential oracle, T2 `Test/wasm_wat/simd_*.wat`, T3
  spec scripts (all curated scripts pass with `failed=0`) and T4 the diagnostic
  benchmark; see [`../TESTING.md`](../TESTING.md) Part 3.

### 4.10 The engine survey, kept as history

LibJIT has no vector type or vector opcode; SLJIT and AsmJIT have vector
instructions but no Wasm-level value model. That survey is why the helper ABI is
the portable baseline and why **SIMD-8** (native lowering) is optional rather
than the baseline. The per-engine tables are in the git history of the former
`SIMD_BACKENDS.md`; the decision is §4.6 above.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ✅ S1 | **Resolved (SIMD-L4).** All 236 fixed-width handlers execute: the 0xFD block in the backend has no `notImplemented` left, so a module using any fixed-width SIMD op now compiles and runs. What still cannot be *judged* is conformance, which is S7 rather than S1. | Verified by counting the handlers, and by `simd_arith_i.wat`, `simd_arith_f.wat` and `simd_convert.wat` all running green through the JIT. | — |
| ✅ S2 | **Relaxed SIMD is enumerated, decoded, dispatched and implemented — all 20 sub-opcodes (0x100-0x113).** Nine of them are one line each in `wasm_simd_ops.inc`, because SD5 picks the strict instruction's result where the proposal permits it (the two `relaxed_trunc_f32x4`, the two `relaxed_trunc_f64x2_*_zero`, the four `relaxed_min`/`max`, `relaxed_q15mulr_s`). The other eleven need their own shape and are hand-written entry points next to `v128.bitselect`: `relaxed_swizzle` and the four `relaxed_laneselect` delegate to the strict helpers (zeroing swizzle, bitwise select — both permitted), while the four `relaxed_madd`/`nmadd` and the two dots get new helpers. `madd`/`nmadd` **forward to the reference**: the vector form contracts into an FMA at `-O2` while the reference cannot, so the two disagreed across builds; the dots take the unsigned reading of `b` and the saturating i16 pair sum on both sides — which makes the differential exact instead of "one of N permitted", and makes the vector dot literally `maddubs`. | `WasmOpcode.hpp`, `dispatchPrefixSIMD`, `wasm_simd_ops.inc`, `wasm_simd_scalar.*`, `wasm_simd_vector.*` | Medium |
| ✅ S3 | **`v128` globals and `v128.const` initialisers work.** `evalConstantExpr` decodes the `0xFD`/`0x0C` arm into the `Value`'s 16 bytes, and `dispatchGlobalGet`/`Set` treat a `v128` global as what it is — 16 bytes at offset 0 of its `Value` — so they reuse the `v128.load`/`v128.store` helpers rather than inventing a struct load. The eight spec scripts that needed it (`simd_const`, `simd_splat`, `simd_lane`, `simd_linking`, the four `simd_store*_lane`) now pass with `failed=0`. | `evalConstantExpr`, `dispatchGlobalGet/Set`, `globalSlotAddress` | Medium |
| ~~S4~~ | **Alignment disagreement.** The JIT lowers `v128` to a 4-byte-aligned `{i32,i32,i32,i32}` struct, and the GC struct layout uses `storageAlign = 4`, while `WASM::Value` is `alignas(16)`. A `v128` struct field or operand-stack value can therefore be under-aligned relative to 16. | `LibJitTypeTranslation.cpp`, `WasmModuleInstance.cpp`, `WasmValue.hpp` | **Done (SIMD-L1)** — `jit_type_set_size_and_alignment(v128, 16, 16)` plus `gcStorageAlign` → 16 (§4.5 below); the H1 layout test tracks both. |
| ~~S5~~ | **`zeroConstantForType` is wrong for `v128`.** It returns an integer 0 for the struct type, which cannot be a valid 16-byte zero. | `LibjitOpcodeDispatcher.cpp:467` comment | **Done (SIMD-L1)** — both 16-byte zero-value paths (`zeroConstantForType`, `zeroValueForType`) now zero through the aggregate's address via `wasm_v128_zero`; `local_zero_*` in `simd_abi.wat` covers it. |
| ~~S6~~ | ~~**SIMD validation.**~~ **Done:** the validator checks lane/shuffle indices, alignment, and the type rules of all fixed-width shapes plus the relaxed ternary forms (`relaxed_madd`/`nmadd`, the four `relaxed_laneselect` forms and the relaxed dot-add). Strict `simd_*` runs green (59 cases, 118 assertions) and all relaxed scripts report 0 failures. | `WasmBase/WasmValidator.cpp` | Closed |
| ✅ S7 | **The harness verifies SIMD in full: all 66 `simd_*` / relaxed scripts pass.** `WastScript.cpp` decodes `v128` arguments lane by lane and compares results lane-wise with the per-lane `nan:canonical`/`nan:arithmetic` policies; `either` result lists are checked against every allowed vector (all 32 across the six relaxed scripts). The last limit was *not* marshalling: libjit's `jit_apply` damages a third stack-passed 16-byte aggregate, so the harness now calls any signature that is one-to-four `v128` parameters (with at most one scalar or `v128` result) through the host C ABI directly — the ABI the generated code was built for (§4.5 below). The remaining skips are the module-validation gap, not SIMD. | `Test/WastScript.cpp`; `docs/TESTING.md` | Medium |
| ✅ S8 | **No test coverage in the curated set** — resolved: 35 `simd_*.wast` scripts are curated, on top of the T1/T2 layers (20 cases, ~5.9k assertions). | `Test/wast_supported.txt` | — |
| ~~S9~~ | **`v128` calling convention is unverified.** Multi-value/struct ABI for `v128` parameters and returns (and v128 in globals/tables) has never been exercised. | no test passes a `v128` | **Done (SIMD-L1)** for parameters/returns/locals — `simd_abi.wat` + the round-trip test measure it (§4.5 below). `v128` in globals/tables stays open until SIMD-4. |

## 6. Where we can and should play fast and loose

SIMD is the feature where the spec's freedom (§3) is least used today: the
engine is scalar and single-threaded, and this project is a QVM successor, not
a graphics kernel.

The lowering these deviations assume **on LibJIT** — one native helper per op,
with hardware fast paths added *inside* the helpers — is surveyed against the
three planned backends (GNU LibJIT has no SIMD surface at all; SLJIT and AsmJit
have vector instructions but no value model, and neither is wired up) in
§4.6–§4.9 below. That section reaches the same
conclusion SD1/SD2/SD7 assume and adds a per-layer "laxness budget" for how far
conformance may be relaxed.

The helper layer is the permanent **baseline and oracle**, not the only permitted
lowering. A backend that has native vector registers is expected to lower `v128`
itself — values in those registers, per-op instructions — instead of, or in
addition to, calling the helpers, and must match the same semantics: that is the
`SIMD-8` milestone, it is what [`../LAYERING.md`](../LAYERING.md) §4's SIMD
contract row already permits, and it is why the semantics layer of §6/§7 is
written once and separately from the lowering.

**The first implementation path is LibJIT-only** — AsmJit and SLJIT are
deferred, not cancelled, and are expected to be leveraged for SIMD when they land
(§4.10 below) — and the specifics are decided in
§4.5 below: `v128` stays
the `{i32×4}` struct but is forced to 16-byte alignment; a `v128` value lives in
a LibJIT frame slot; helpers take **pointers** (`const void*` in, `void*` out) so
no aggregate-ABI assumption is needed; and the helper bodies are written against
Highway 1.4.0 with a scalar reference implementation kept as the test oracle.
That note's §4–§9 hold the design comparison, the library verdicts (including
GLM/SLEEF/xsimd/psimd), the helper ABI and a library-independent list of
semantics hazards (NaN rules, `trunc_sat` corners, `swizzle` out-of-range, …)
that no vector library provides.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SD1 | **Scalar/helper lowering first.** Represent `v128` as four `u32` and implement every op as a native helper (or an inline loop over lanes), exactly like the GC native helpers; the helper ABI is pointer-in/pointer-out, so no aggregate-ABI assumption is required (§4.5 below). | §3: no hardware SIMD is required; results are what matter. | A portable, testable baseline in far less code than intrinsics. |
| SD2 | **Add hardware fast paths only behind runtime feature detection, keeping the helpers as fallback.** Start with the hottest shapes (memory, bitwise, i32x4/f32x4 arith). Realised by writing the helper bodies against Highway 1.4.0, whose dynamic dispatch *is* the runtime detection (§4.7 below). | Any correct fallback is conformant. | Performance when it matters, without a porting matrix. |
| SD3 | **`v128.load`/`store` as a 16-byte `memcpy`; ignore the memarg alignment** (only enforce it if we ever add validation). | §3: unaligned access is legal when align < natural; x86 tolerates all alignments. | One `memcpy` per access, no alignment handling. |
| SD4 | **`i8x16.shuffle` via a 16-iteration helper** (index < 16 → operand 0, else operand 1). | §2.2; any correct lowering is fine. | Correctness first; `pshufb` later. |
| SD5 | **Relaxed SIMD: pick the plain scalar result for every op** — unfused (double-rounded) `relaxed_madd`, saturating `relaxed_trunc`, zeroing `relaxed_swizzle`, bitwise `relaxed_laneselect`, scalar-wasm NaN rules for `relaxed_min/max`, saturating `relaxed_q15mulr_s`, and for the dots the unsigned reading of the second operand plus the saturating i16 pair sum. | §2.4/§3: any allowed result is conformant; the deterministic profile is optional. | No hardware dependence, reproducible results, trivial code — and, because the *reference* is written to make the same choice, an exact differential rather than a "one of N" match. |
| SD6 | **Optionally adopt the deterministic profile later** (spec prescribes every relaxed parameter to `0`) if we want byte-for-byte reproducibility across machines. | It is a refinement of SD5, never a requirement. | Cross-machine determinism for replays/networking — very much in the QVM spirit. |
| SD7 | **One alignment everywhere — and it is 16.** Keep the 4×`u32` struct but force the lowered type to 16-byte alignment (`jit_type_set_size_and_alignment`) so frame slots, `WASM::Value`'s `alignas(16)` and the GC layout agree; all memory access stays unaligned-safe. Decided in §4.5 below. | §3: no alignment guarantee is required of the representation, only of the memory-access rule. | Removes the S4 disagreement instead of papering over it. |
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
| ✅ **SIMD-0 — ABI** | Done (SIMD-L1): the decided layout is applied (§4.5 below) — the `{i32×4}` type with 16-byte alignment forced, `gcStorageAlign` → 16 — plus `simd_abi.wat` and the round-trip test. | ~~S4~~, ~~S5~~, ~~S9~~ | ✅ A function `(param v128) (result v128)` round-trips through the JIT (measured). |
| ✅ **SIMD-1 — Core scalar lowering** | ✅ **Done (SIMD-L1/L2).** `v128.const`, the 22 memory ops, all six `*.splat`, extract/replace lane, the six bitwise ops, `i8x16.shuffle` and `i8x16.swizzle` are implemented and tested, on a runtime-dispatched vector layer with the scalar reference as the oracle. | S1 (partly) | ✅ Hand-written modules compute byte shuffles, swizzles and bitselects correctly, incl. the hazard boundary vectors. |
| ✅ **SIMD-2 — Integer ops** | **Done (SIMD-L3).** All 133 ops: the 36 comparisons, `add`/`sub`/`mul`, `min`/`max` (signed and unsigned), `neg`/`abs`, the 12 shifts, saturating `add`/`sub`, `avgr_u`, `popcnt`, `q15mulr_sat_s`, the reductions `all_true`/`any_true`/`bitmask`, and the width-changing family (`extend`/`extmul` `_low`/`_high`, `extadd_pairwise`, `dot`, `narrow`). | S1 | ✅ Differentially tested against the reference by construction; the corners a differential cannot see are pinned by goldens that were cross-checked against an independent engine; `simd_arith_i.wat` is green through the JIT (§9 T2). |
| ✅ **SIMD-3 — Float ops** | ✅ **Done (SIMD-L4).** All 52 ops: comparisons (incl. the unordered `ne`), `add`/`sub`/`mul`/`div`, `abs`/`neg`/`sqrt`, `ceil`/`floor`/`trunc`/`nearest` (ties to even), `min`/`max`/`pmin`/`pmax` with wasm's NaN and `±0` rules, and the 10 conversions — `trunc_sat`, `convert`, `convert_low`, `demote`/`promote`. | S1 | ✅ `simd_arith_f.wat` and `simd_convert.wat` green (51 exports, all read off V8); T1 green for every float op. |
| ✅ **SIMD-4 — Globals and constants** | ✅ **Done.** `v128.const` in `evalConstantExpr`; `global.get`/`set` of `v128` as 16-byte loads/stores on the global's `Value`. | S3 | ✅ `simd_globals.wat` (immutable + mutable globals, read-modify-write across calls) and the eight formerly-blocked scripts pass. |
| ✅ **SIMD-5 - Relaxed SIMD** | ✅ **Done: all 20 sub-opcodes implemented.** Nine are op-list entries (strict-helper reuse per SD5); the other eleven are hand-written entry points — five of them delegating to the strict op (`relaxed_swizzle` → zeroing swizzle, four `relaxed_laneselect` → `v128.bitselect`), six with new helpers (`madd`/`nmadd` forwarding to the reference so the rounding choice is structural, the dots unsigned+saturating). Both shapes are proven by the `simd relaxed` differential and the `simd_relaxed.wat` JIT case. All 7 relaxed spec scripts are curated. | S2 | ✅ A module using any relaxed family decodes and produces an allowed result; the op-list differential plus the hand-written table cover all 20. |
| ✅ **SIMD-6 — Harness** | ✅ **Done for every shape the engine compiles.** Marshals `v128` arguments/results (lane-wise decoding and comparison, per-lane NaN policies) and, for one-to-four-`v128` signatures, bypasses libjit's `jit_apply` by calling through the host C ABI. All 66 `simd_*` / relaxed scripts are curated with `failed=0`. | S7, S8 | ✅ `./WasmJit 'spec: *'` in tolerant mode reports the full survey, and the 66 curated scripts run in the default suite. |
| ✅ **SIMD-7 — Hardware fast paths** | ✅ Measured and decided: helpers are on Highway 1.4.0 with run-time dispatch; SIMD-L8 measured the per-op helper boundary (pre-D5 4.10 ns/v128-op, 97.6 % call overhead) as the limit. The D5 `load`+op/const peepholes later reduced this to **3.47 ns** (97.2 %) without changing the helper granularity; D4 remains gated/unbuilt. | — | The benchmark and decision are recorded; a future D4/peephole change can revisit the denominator. |
| ◑ **SIMD-8 — Native lowering per backend** (started 2026-11-11: AsmJIT lowers `v128.and`/`andnot`/`or`/`xor` natively — `movdqu`/`pand`/`pandn`/`por`/`pxor` into a fresh buffer, no helper call; the SIMD surface is green on all three backends, 7,413 assertions / 114 cases. SLJIT's equivalent (`sljit_emit_simd_mov` + `sljit_emit_simd_op2`) is the next step; the benchmark still reports the helper boundary at 86.8 % of a helper call, so the headroom is real) | A backend with vector registers (SLJIT, AsmJIT) may lower `v128` in *that backend*; the helper layer remains the portable fallback and oracle. **LibJIT is excluded permanently:** its IR has no vector type, vector opcode or vector register class (29 headers, 450 `JIT_OP_*`, five type constructors, zero vector tokens; §4.6/§4.10 below). | — (optional native path) | An optional performance path, not a missing proposal surface; all three currently use the helper lowering, and LibJIT’s is final. |

Guardrails: keep the scalar helpers as the reference implementation and the
fallback; a native per-backend lowering must match their semantics rather than
re-derive them; run `./WasmJit '~[spec]'` after each step.

## 8. Indicative test survey

Survey taken with `build/Desktop-Debug/Test/WasmJit` (2026-10-17, after SIMD-6's
marshalling work).

| Script | Result | Reading |
|---|---|---|
| `simd_f64x2_pmin_pmax` | 3873 passed, 0 failed | the largest passing script |
| `simd_f32x4_cmp` | 2583 passed, 0 failed | comparisons, both signednesses' worth of NaN rules |
| `simd_i8x16_cmp` | 415 passed, 0 failed | integer comparisons |
| `simd_load` | 31 passed, 0 failed | curated |
| `simd_store8_lane` | 49 passed, 0 failed | closed by `v128` globals (SIMD-4) |
| `simd_i8x16_arith2` | all passed, 0 failed | closed by the harness's direct-call path (SIMD-L7) |

**All 66 `simd_*` / relaxed scripts pass in full and are curated**; none abort.
Reaching that took three separate causes, and the record of them is the point:
`v128` marshalling first (SIMD-L7), then `v128` globals (SIMD-4 / SIMD-L5 —
a missing `0xFD` arm in `evalConstantExpr` and the `global.get`/`set` lowering),
then libjit's `jit_apply` damaging a third stack-passed aggregate, which the
harness now sidesteps by calling one-to-four-`v128` signatures through the host C
ABI (§4.5 below). None of the three needed a change to the
generated code. Reproduce one row with:

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

- §4.5–§4.10 above — the helper ABI, the LibJIT lowering (D3+D5), the SIMD
  library and core files, the relaxed-SIMD laxness budget, the measured cost and
  the engine survey.
- [`docs/GC.md`](GC.md) — the companion design note (and §4.4/SIMD-0 interplay
  for `v128` inside aggregates).
- [`docs/THREADS.md`](THREADS.md) — the companion note for the other
  "post-2.0 / non-3.0" feature area.
- [`NUMERIC_PROPOSALS.md`](NUMERIC_PROPOSALS.md) — the adjacent FP16 lane and
  rounding-variant proposals, plus Wide Arithmetic; none is implemented.
- [`docs/TESTING.md`](../TESTING.md) — why `v128` values are currently skipped by
  the spec harness.
- Source anchors: `WasmBase/WasmType.hpp` (`ValueTypeCode::V128`),
  `WasmBase/WasmValue.hpp` (`Value::v128`), `WasmBase/WasmOpcode.hpp`
  (`SIMDOpcode`), `WasmBase/WasmOpcodeDispatcher.cpp` (`dispatchPrefixSIMD`),
  `WasmBase/WasmModuleInstance.cpp` (`evalConstantExpr`, struct layout),
  `LibJit/LibJitTypeTranslation.cpp` (`v128_definition`),
  `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}` (0xFD stub block and
  `zeroConstantForType`), `WasmStub/StubOpcodeDispatcher.cpp`
  (pretty-printers), `Test/wast_supported.txt`.
