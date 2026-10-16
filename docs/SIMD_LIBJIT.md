# LibJIT-Only SIMD Lowering — Design Decision: Frame-Slot `v128`, Pointer-Arg Helpers, Highway

Status: design note / decision record (revision 1 — records the LibJIT-only
direction, the `v128` layout, the helper ABI, the helper library, the wasm
semantics layer and the test plan; **defers all code**).
Audience: anyone implementing [`proposals/SIMD.md`](proposals/SIMD.md) §7
(SIMD-0…SIMD-7), touching `LibJit/LibJitTypeTranslation.*`,
`LibJit/LibjitOpcodeDispatcher.*`, `LibJit/LibJit.pro`, or the test harness.

Companion notes: [`proposals/SIMD.md`](proposals/SIMD.md) says *what the spec
demands and where we may deviate*; [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md)
surveys *what the engines can do*. This note turns those two into **one decision
and one normative layout**. It contains **no new code**.

Scope: **the current implementation path is GNU LibJIT only.** AsmJit and SLJIT
are **deferred, not cancelled**: both stay planned, and both are expected to be
leveraged for SIMD once implemented (§3).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does LibJIT force on us? |
| §3 | Scope: why the first SIMD path runs on LibJIT alone (and what the other backends will add later) |
| §4 | The lowering designs considered, and the chosen one |
| §5 | The helper library decision (installed stock, verdicts, Highway) |
| §6 | Normative: the `v128` layout, storage, helper ABI, dispatcher side |
| §7 | The wasm semantics layer the library does *not* give us |
| §8 | Performance model and the batched-path threshold |
| §9 | The test plan, documented as tests |
| §10 | Milestones |
| §11 | Risks and open questions |
| §12 | References |

## 2. What LibJIT forces on us

**LibJIT has no vector type, no vector opcode, and nothing to attach one to.**
The installed headers (`/usr/local/include/jit/`, 28 files) define 438
`JIT_OP_*` opcodes and exactly five type constructors
(`jit_type_create_struct`, `_union`, `_signature`, `_pointer`, `_tagged`); a
word-boundary search for `simd|vector|packed|xmm|ymm|zmm|mmx|sse|avx|neon`
across *all* of them returns nothing. This is unchanged from
[`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §3.1.

Three consequences shape everything below:

1. **Every SIMD library is invisible to LibJIT.** Highway, SIMDe, xsimd and the
   rest can only ever appear *inside our own host-compiled helper functions*,
   reached through `jit_insn_call_native` — the mechanism this backend already
   uses for GC, bulk memory and float `copysign` (20+ call sites in
   `LibjitOpcodeDispatcher.cpp`). No library makes LibJIT emit a vector
   instruction, so "which library" is *not* the performance question.
2. **The performance question is the boundary, not the op.** For one 128-bit
   operation the inner work is 1–4 instructions; getting the operand across the
   helper boundary (call + address/load + store) costs an order of magnitude
   more. The design that matters is therefore the **storage and the calling
   convention** (§6), not the ISA selection.
3. **The hard part is library-independent.** wasm's exact rules for
   `f32x4.min/max` NaN and ±0 handling, `trunc_sat` corners, `swizzle`
   out-of-range, `q15mulr_sat_s` saturation and lane ordering for
   narrow/widen/dot are **not** provided by any vector library — they are a
   semantics layer we write on top (§7), whatever the library.

## 3. Scope: why the first SIMD path runs on LibJIT alone

AsmJit and SLJIT are **deferred, not cancelled**. Both remain planned backends,
and both are expected to be *leveraged* for SIMD when their turn comes — that is
a second, later performance lever, not an abandoned idea. Neither is wired up
today (both are empty scaffolds), so the first SIMD path is LibJIT-only. What
that defers, and why it is worth waiting for:

| Deferred | Why it is not first | What it will add for SIMD |
|---|---|---|
| **AsmJit** | Not implemented (an empty scaffold: no headers, no `-lasmjit`). It is an assembler/IR allocator with no value model, so it would still need the `v128` operand model, ABI and semantics layer built from scratch — the work §4 D1–D5 and §7 describe is backend-independent. | Complete x86 instruction coverage (MMX/SSE…SSE4/AVX/AVX-512) **plus a vector-aware register allocator** (`asmjit/x86/x86_ra_pass.cpp`): `v128` values living in `xmm/ymm/zmm` instead of memory, and the option of processing several `v128` values per instruction. |
| **SLJIT** | Same: an empty scaffold, not installed, and a register-level API with no value model. | First-class SIMD registers — `SLJIT_SIMD_REG_128/256/512`, `SLJIT_VR0…`, `sljit_emit_simd_*` — so the same helper semantics could be lowered inline instead of called. |

What deferring them does **not** remove: the semantics layer (§7), the `v128`
ABI/alignment question (S4/S9) and the harness gaps (S7/S8). Those are
WasmJit-layer work under every backend, which is why doing them once, on LibJIT,
is the right order — and why the helper layer lives in the backend-agnostic core
([`LAYERING.md`](LAYERING.md)) so the later backends can reuse it unchanged.

## 4. The lowering designs considered

One `v128` operation has to be lowered somehow. Five shapes were feasible:

| # | Design | How `v128` crosses the boundary | Verdict |
|---|---|---|---|
| **D1** | **Inline-rolling**: emit scalar IR per lane (`jit_insn_load_relative` + scalar ops + `store_relative`), no helper call | — (stays in LibJIT IR) | **Reject as the general mechanism.** Cross-lane ops (`shuffle`, `swizzle`, `bitmask`, `any_true`, `pmin/pmax`, `dot`, widening/narrowing) have no per-lane decomposition, and IR size/compile time scale with the lane count. **Keep** for the small set that is genuinely single-lane and cheap: `v128.const`, `v128.load`/`store`, `*.splat`, `*.extract_lane`, `*.replace_lane`. |
| **D2** | **Per-op native helper, `v128` by value** (a 16-byte struct argument and return) | `jit_insn_call_native` with the struct type in the signature | **Reject as primary.** Depends on LibJIT's 16-byte aggregate ABI matching the host C ABI, which is unverified (gap S9) and would have to be proven separately. Kept as the **challenger** in the §8 benchmark. |
| **D3** | **Per-op native helper, pointer arguments** | pass the *address* of a 16-byte frame slot; the helper reads/writes memory | **Chosen.** No aggregate-ABI risk at all: the helper signature is only `void*`/`uint32_t`, i.e. LibJIT's best-tested primitive path. |
| **D4** | **Batched/fused run helper**: one call performs several `v128` ops from/to a contiguous bank | pointers into a bank, plus a small op script or a fixed fusion pattern | **Deferred and gated.** It is the only design that benefits from wider-than-128-bit vectors and runtime dispatch, and it only pays if the benchmark (§8) shows per-op boundary cost dominating. |
| **D5** | **Hybrid: D3 + peepholes** | as D3, plus fusing producer/consumer pairs so the intermediate never leaves the helper | **Chosen shape.** `v128.load` + arithmetic → one helper that loads and computes; `v128.const` + op → one helper with an immediate operand. Removes whole slot round-trips for the memory-heavy loops real producers emit, without any new ABI. |

Reading the table: **D5 = D3 + the useful subset of D1 + fusions**. That is the
direction this note records. Everything below specifies it.

## 5. The helper library decision

### 5.1 What is actually installed

| Library | Version | Form | Licence | Relevant to us? |
|---|---|---|---|---|
| **Highway (`libhwy`)** | **1.4.0** | headers + `/usr/lib64/libhwy.so.1.4.0` (`libhwy.pc`) | **Apache-2.0** | **Chosen** |
| **SIMDe** | 0.8.2 | header-only (`/usr/include/simde`) | MIT | Viable alternative |
| **xsimd** | 14.3.0 | header-only (`/usr/include/xsimd`) | BSD-3-Clause | Incomplete for wasm |
| **psimd** | 2020-05-17 | single header (`/usr/include/psimd.h`) | MIT | Too thin |
| **GLM** | 1.0.3 | header-only (`/usr/include/glm`, `glm.pc`) | MIT | Unsuitable |
| **SLEEF** | present | `sleef.h`, `sleef.pc` | BSL-1.0 | Irrelevant |
| **Eigen** | 3.x | headers, `eigen3.pc` | MPL-2.0 | Irrelevant |
| **Vc** | present | `/usr/include/Vc` | BSD-3-Clause | No advantage |

The project licence is **Apache-2.0** (`LICENSE`), so all of the above are
compatible; Highway is the only one that *matches it exactly*.

### 5.2 The dimensions that decide it

| Dimension | Highway | SIMDe | xsimd | psimd | GLM |
|---|---|---|---|---|---|
| Exactly-128-bit vectors (no accidental widening) | `CappedTag<T, N>` — a lane-count cap | intrinsic-level, inherently 128-bit | `batch<T, N>` fixed N | fixed 128-bit types | `vec4` (float only) |
| Covers `shuffle`/`swizzle` | `TableLookupLanes`, `BitShuffle` | `_mm_shuffle_epi8` (+ emulation) | partial | no | no |
| Covers `dot`/`extmul`/`q15mulr` | `ReorderWidenMulAccumulate`, `MulEven/Odd`, `MulFixedPoint15` | corresponding SSE2/SSSE3 intrinsics | partial | no | no |
| Saturating add/sub | `SaturatedAdd`/`SaturatedSub` | `_mm_adds_epi8` etc. | **not in the public API checked** | partial | no |
| Narrow/widen | `ReorderDemote2To`, `Promote{Lower,Upper}To` | `_mm_packs*`, `_mm_cvtepi8_epi16` | partial | no | no |
| **Runtime CPU dispatch** | **built in** (`HWY_EXPORT`/`HWY_DYNAMIC_DISPATCH`) | none (compile-time ISA) | `xsimd::dispatch` | none | none |
| Portable fallback on a bare host | **`EMU128` + `SCALAR` targets** in-tree | emulation on non-x86 | emulated ISA | scalar fallback | scalar |
| Integration cost in qmake | one multi-target TU + `-lhwy` | one `#if`d TU per ISA + handwritten dispatch | header-only | header-only | header-only |

### 5.3 Verdicts

- **Chosen — Highway 1.4.0.** Same licence as the project; the only candidate
  that provides **runtime dispatch** and a **portable fallback target** without
  us writing multiversioned translation units and `__builtin_cpu_supports`
  tables by hand; covers every hard wasm family in the checked headers; and its
  `CappedTag` keeps vectors at exactly 16 bytes so no op can silently widen.
- **Documented alternative — SIMDe.** Identical generated code on x86 (it
  forwards to the real intrinsic: `SIMDE_X86_SSE2_NATIVE`), header-only, no
  second shared library. Rejected *only* because it gives no runtime dispatch,
  so we would own the ISA-dispatch machinery permanently. This is the escape
  hatch if a second `.so` dependency ever becomes unacceptable.
- **Fallback of last resort — raw intrinsics + `__builtin_cpu_supports`.**
  Maximum control, maximum boilerplate and permanent portability debt; becomes
  the **reference implementation** the oracle in §9 is written against anyway.
- **Rejected — GLM.** Its `glm/simd/` layer is float graphics mathematics; the
  entire integer surface is `glm_i128_interleave` and `glm_i128_interleave2`
  (two functions). It cannot express wasm lane semantics and has no dispatch.
- **Rejected — SLEEF.** Vectorised transcendentals. wasm SIMD exposes no
  transcendental, and `f32x4.sqrt` must be the correctly-rounded hardware
  square root, not a polynomial approximation.
- **Rejected — Eigen.** Linear algebra; a matrix abstraction is the wrong shape
  for lane-exact integer SIMD and drags in a large header-only dependency.
- **Rejected — Vc.** A capable C++ SIMD library, but its `simdize` abstraction
  adds no capability we need over Highway while being less transparent about
  lane semantics.
- **Rejected — psimd.** A small 2020 surface (seven vector types, `…x2`
  variants) without the shuffle/dot/mul-fixed-point families wasm needs.
- **Rejected — xsimd.** Architecturally fine and widely used, but the public
  API I checked exposes no saturating add/sub, which wasm needs for
  `add_sat_*`/`sub_sat_*`/`q15mulr_sat_s`/narrowing; and its C++ `batch`
  indirection is more machinery than a 1–4-instruction helper body warrants.

### 5.4 Why Highway's dispatch model fits this backend specifically

- `HWY_DYNAMIC_POINTER(X)` / `HWY_EXPORT(X)` give a **plain function pointer**,
  which is exactly what `jit_insn_call_native` consumes — so a Highway helper is
  an ordinary C-ABI function from LibJIT's point of view, indistinguishable from
  `wasm_table_fill_impl`.
- Our headers set the per-target ISA via `#pragma GCC push_options` /
  `#pragma GCC target` (`HWY_PUSH_ATTRIBUTES` in `hwy/base.h`, applied around the
  target namespace by `HWY_BEFORE_NAMESPACE()`). **No global `-mavx2` is needed
  and the rest of the engine stays baseline.**
- The dispatch bookkeeping lives in the installed shared library, which exports
  `hwy::SupportedTargets()`, `hwy::DispatchedTarget()`,
  `hwy::VectorBytes()`, `hwy::GetChosenTarget()`. We only supply the per-target
  bodies; we do **not** build Highway itself.

### 5.5 Build integration (the recipe, not a sketch)

Highway's canonical skeleton — the pattern its own `highway.h` documents at the
`HWY_EXPORT` definition, and the one the installed headers use:

```text
WasmBase/wasm_simd_hwy.hpp   # per-target bodies; toggled guard (see below)
WasmBase/wasm_simd_hwy.cpp   # the only file that knows about Highway's dispatch
```

The per-target header must be **re-includable**, so it follows Highway's own
`ops/*-inl.h` guard convention — a guard that flips with `HWY_TARGET_TOGGLE`
(`#if defined(OUR_TOGGLE) == defined(HWY_TARGET_TOGGLE)` … `#define OUR_TOGGLE`),
which `hwy/foreach_target.h` flips after each re-inclusion. Inside it the bodies
are wrapped in `HWY_BEFORE_NAMESPACE()` / `HWY_AFTER_NAMESPACE()` around
`namespace HWY_NAMESPACE`, exactly as `hwy/base.h`'s `HWY_PUSH_ATTRIBUTES`
requires. The `.cpp` defines `HWY_TARGET_INCLUDE` before including
`<hwy/foreach_target.h>` and `<hwy/highway.h>`, then — under `#if HWY_ONCE` —
does `HWY_EXPORT(...)` for each helper and defines the `extern "C"` wrapper that
calls `HWY_DYNAMIC_DISPATCH(...)`.

Build lines (qmake), to be added with the helper sources in SIMD-L2:

| Item | Value | Source of truth |
|---|---|---|
| Cflags | `-DHWY_SHARED_DEFINE` (already in `libhwy.pc`) | `/usr/share/pkgconfig/libhwy.pc` |
| Link | `-lhwy` | same |
| Global ISA flags | **none** — per-target flags come from the pragmas Highway pushes per namespace | `hwy/base.h` `HWY_PUSH_ATTRIBUTES` |

Note: because multiple x86 targets are attainable on this machine, the header is
re-included once per enabled target (SSE2…AVX3, EMU128, SCALAR). If helper
compile time becomes annoying, Highway's `HWY_COMPILE_ONLY_*` knobs restrict the
set — a build-level tuning decision, not a design one.

## 6. Normative layout

This section is the part a future implementer must follow; it supersedes the
"explicit `alloca` bank" sketch in [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §5/W2,
and refines [`proposals/SIMD.md`](proposals/SIMD.md) SD7.

### 6.1 The `v128` representation

| Item | Decision |
|---|---|
| Lowered type | **Keep** `v128_definition = jit_type_create_struct({i32,i32,i32,i32}, 4, 0)`. The type layer, locals, globals and signatures continue to work unchanged. |
| Alignment | **Add** `jit_type_set_size_and_alignment(v128, 16, 16)` at construction. This is the whole JIT-side fix for S4; the GC struct layout (`storageAlign` in `WasmModuleInstance.cpp`) and `WASM::Value`'s `alignas(16)` must be brought into line in the same change. |
| Why this is enough | A struct-typed `jit_value` from `jit_value_create` is a real frame slot, and `jit_insn_address_of` yields its address — verified against this LibJIT build (§6.6). Note the *other* recipe the backend already used, `jit_insn_store_relative` on such a value, does **not** write it; that path was corrected in SIMD-L1. |

### 6.2 Storage: LibJIT's own frame slots are the `v128` bank

- A `v128` on the operand stack is a struct-typed `jit_value_t` backed by a
  LibJIT frame slot (`jit_value_create(fn, v128Type)`), exactly as today. One
  slot per *static* definition site; the frame is fixed for the duration of a
  call, so a value produced inside a wasm loop reuses its slot.
- A helper result is written into a fresh `jit_value_create(fn, v128Type)`
  temporary whose address is passed as the `out` argument; that temporary is
  what gets pushed on the operand stack.
- **Why not an explicit `jit_insn_alloca` bank** (the earlier sketch):
  `jit_insn_alloca` is a *dynamic* stack allocation — called inside a loop it
  grows the frame per iteration — and sizing **one** bank at function entry
  needs the maximum operand-stack height, which the compiler does not compute
  today (`PreparedFunctionStack::prepare` only flattens locals:
  `WasmBase/WasmType.cpp:4`). Frame slots need neither a height pre-pass nor a
  dynamic alloca.
- **SAFETY**: frame growth is 16 bytes per static SIMD value, bounded by the
  function's own instruction count (itself capped by `WasmSafety.hpp`) and never
  by input data. See [`SAFETY.md`](SAFETY.md).
- **Deferred optimisation** (only if the §8 benchmark says so): a shared bank
  keyed by operand-stack height, which needs the height pre-pass, or
  live-range reuse of slots. Both are internal to this section — the helper ABI
  does not change.

### 6.3 The helper ABI

All helpers are `extern "C"`, take pointers instead of values, and never trap
(the memory helpers go through the existing bounds check and raise the existing
trap, exactly like `wasm_memory_copy_impl`). The four shapes:

| Shape | Signature | Examples |
|---|---|---|
| Unary | `void h(const void* a, void* out)` | `v128.not`, `i32x4.abs`, `f32x4.sqrt`, `i8x16.splat`-family results |
| Binary | `void h(const void* a, const void* b, void* out)` | `i32x4.add`, `f32x4.min`, `v128.and`, `i8x16.swizzle` |
| Ternary | `void h(const void* a, const void* b, const void* c, void* out)` | `v128.bitselect`, `*.relaxed_laneselect` |
| Scalar-result | `int32_t h(const void* a[, …])` | `*.extract_lane_s/u`, `i8x16.bitmask`, `v128.all_true`/`any_true` |

Two conventions worth fixing now, because they decide how many helpers exist:

- **Immediates travel as scalars, never as memory.** A lane index is a
  `uint32_t` argument. A 16-byte immediate (`i8x16.shuffle`'s 16 selector bytes,
  `v128.const`) is passed as **two `uint64_t` scalars** — that keeps the whole
  signature in LibJIT's best-tested primitive path and avoids materialising a
  read-only constant in memory just to take its address.
- **Memory helpers take the VM context first**, then the resolved address, then
  `out`: `void h(WASM::VMContext* vm, uint32_t memIdx, uint32_t addr, void* out)`
  for `v128.load`, and with `const void* val` instead of `out` for
  `v128.store`. This mirrors `wasm_memory_copy_impl`/`wasm_memory_fill_impl` and
  reuses the same bounds logic rather than re-deriving it.

### 6.4 The dispatcher side

- Each `dispatch*` handler follows the existing helper pattern
  (`jit_type_create_signature(jit_abi_cdecl, …)` + `jit_insn_call_native`,
  as at `LibjitOpcodeDispatcher.cpp:489–532`): pop the operand values, pass
  `jit_insn_address_of(value)` for each `const void*`, allocate the result slot,
  pass its address as `void*`, push the slot.
- **Generalise the existing `wasm.unary` / `wasm.binary` / `wasm.ternary`
  wrappers** rather than writing ~256 bespoke call sequences. Those three
  already exist for scalar helpers and are the exact shape a `callSimdUnary/Binary/Ternary`
  needs.
- **Cache the signatures.** The current code creates a fresh
  `jit_type_create_signature` per call site; with ~256 SIMD opcodes that churn
  is avoidable by caching the handful of distinct arities (2/3/4 params, plus
  the memory variants).
- **`v128.const` is a helper, not inline stores.** This note originally sketched
  it as "four `jit_insn_store_relative`s into a slot"; that recipe does **not**
  write a `jit_value_create`d aggregate (§6.6), so it goes through
  `wasm_v128_const(uint64_t lo, uint64_t hi, void* out)` like every other op.
  `v128.load`/`store`, `splat`, `extract_lane` and `replace_lane` remain
  candidates for inline lowering — and only if the benchmark (§8) shows the call
  is measurable for them.

### 6.5 File layout

| Path | Role | Milestone |
|---|---|---|
| `WasmBase/wasm_simd_hwy.cpp` / `.hpp` | The target-dependent helper bodies + the `extern "C"` wrappers (Highway's multi-target TU). One place where the library is visible. | SIMD-L2 |
| `WasmBase/wasm_simd_scalar.cpp` / `.hpp` | The **scalar reference** implementation of *every* helper — no library, always compiled. The oracle of §9. | SIMD-L1 |
| `LibJit/LibjitOpcodeDispatcher.cpp` | Turn the 236 `notImplemented(__func__)` stubs into helper calls; add `callSimd*`; the D1 inline cases | SIMD-L2…L4 |
| `LibJit/LibJitTypeTranslation.cpp` | The 16-byte alignment line (§6.1) | SIMD-L1 |
| `WasmBase/WasmModuleInstance.cpp` | `evalConstantExpr` `v128.const` arm; `storageAlign` alignment | SIMD-L5 |
| `Test/wasm_wat/simd_*.wat` | Hand-written lane-semantics modules | SIMD-L1+ |

### 6.6 Verified mechanics (measured, not assumed)

Every mechanic this section relies on was probed against the installed LibJIT
*before* it was written into the backend — the S9 gap was **unverified**, not
known-bad, so the answer had to be measured. The probes live in the build tree
and the results are baked into the code comments:

| Mechanic | Result |
|---|---|
| `jit_type_return_via_pointer` for a 16-byte struct | `0` — returned in registers, not via a hidden pointer |
| `(param v128) (result v128)` returning its own parameter | round-trips exactly, at both 4-byte and 16-byte alignment |
| Forcing 16-byte alignment | does not change the register classification; a containing struct then places the field on a 16-byte boundary (`{i8, i32, v128, i64}` → offsets 8, 12, 16, 32) |
| `jit_insn_address_of` on a struct **parameter** | works — a helper reads the 16 bytes correctly |
| `jit_insn_address_of` on a struct **local** | works — a helper's writes are visible to a later read and to `jit_insn_return` |
| `jit_insn_load` of a struct local/param, then `address_of` | works (i.e. `local.get` of a `v128`) |
| `jit_insn_store(local, param)` | works (i.e. `local.set` of a `v128`) |
| **`jit_insn_store_relative` into a `jit_value_create`d aggregate** | **does not work** — the stores do not land. `v128.const` and the two v128 zero-value paths used it and were corrected in SIMD-L1. |

The C++ mirror of the ABI is `struct { uint32_t a, b, c, d; }`: a four-`i32`
aggregate is two INTEGER eightbytes on x86-64 System V, which is what LibJIT's
own `return_via_pointer == 0` implies and what the T1/T2 tests exercise.

## 7. The wasm semantics layer (what no library gives us)

This is where the correctness risk lives, and it is the same work under any
library choice. Each row is something a plausible implementation gets wrong
*silently* — the tests of §9 exist to catch exactly these.

| # | Hazard | Why the hardware/library default is wrong | Rule to implement |
|---|---|---|---|
| **Y1** | `f32x4`/`f64x2` `min`/`max` (and `pmin`/`pmax`) | `minps`/`Min` returns the **second** operand when either is NaN, and for `±0` returns the second operand. wasm requires a **canonical NaN** when either input is NaN, and *min* must pick `-0` / *max* must pick `+0`. | Compare in two steps: an unordered mask → canonical NaN; otherwise an ordered compare with an explicit `±0` tie-break. |
| **Y2** | `i32x4.trunc_sat_f32x4_{s,u}`, `_f64x2_{s,u}_zero` | `cvttps2dq` yields `0x80000000` for NaN and for out-of-range — indistinguishable from a legitimate `-2^31`. | NaN → `0`; `≥ 2^31` → `INT32_MAX`; `≤ -2^31 - 1` → `INT32_MIN`; unsigned forms clamp to `[0, UINT32_MAX]`. Fix up the raw conversion. |
| **Y3** | `i8x16.swizzle` | `pshufb`/`BitShuffle` zeroes a lane only when the index's **bit 7** is set; wasm zeroes every index `≥ 16`, so indices 16…127 would silently alias lanes 0…15. | Saturate the index (`min(idx, 15)` unsigned) *or* compare-and-blend before the shuffle. **The single most likely silent bug in the whole feature.** |
| **Y4** | `i8x16.shuffle` | Indices 0–15 select operand 0, 16–31 operand 1; a shuffle primitive that takes "one selector vector" invites an off-by-16 or a reversed concatenation. | Build the two-operand concatenation explicitly; verify against the oracle. |
| **Y5** | `q15mulr_sat_s` | `pmulhrsw`/`MulFixedPoint15` rounds `(a*b + 0x4000) >> 15` but yields `0x8000` for the `-32768 × -32768` corner, where wasm requires saturation to `0x7fff`. | Fix up the one saturated lane (compare the inputs against the negative-only corner). |
| **Y6** | `narrow` / `widen` / `extmul` / `pmin`/`pmax` lane order | Packing instructions interleave two inputs (`packs` produces low half from the first operand, high half from the second); getting this backwards permutes lanes while still looking "reasonable". | Assert lane placement in T1 per op, not just the numeric result. |
| **Y7** | `round` / `nearest` | Nearest must be `roundTiesToEven`; `_MM_FROUND_TO_NEAREST_INT` is available only from SSE4.1 and a naive `(int)(x + 0.5)` is wrong for ties and negatives. | Use a proper ties-to-even round; check the `0.5`, `1.5`, `2.5`, negative-tie cases. |
| **Y8** | `i64x2` shifts and comparisons | 64-bit lane shifts/comparisons are not in baseline SSE2 (comparisons need SSE4.2, shifts need per-lane emulation). | Verify the target provides real 64-bit ops or emulate; check the shift-count `≥ 64` case. |
| **Y9** | `bitmask` / `all_true` / `any_true` | The i8x16 form is sign-bit based (matches `movemask`), but the i16x8/i32x4/i64x2 forms select bit *n* of each lane — not the same reduction. | Derive each from its own sign-lane extraction; do not generalise from `movemask`. |
| **Y10** | NaN propagation for plain arithmetic | Vector ADD/MUL/SUB/DIV must follow wasm's NaN rules (canonical result unless the input is already a NaN payload case); hardware does this for the arithmetic ops, but *not* for the min/max family (Y1) or for conversions. | Confirm per op; the oracle covers the arithmetic family too. |
| **Y11** | Little-endian lane numbering | `v128.const` byte *i* is bit `8i`; a "native word order" implementation is wrong and passes on the author's machine only. | Byte-address every `v128.const`, `load`/`store` and `load?x?_s/u` case explicitly. |

Everything else in the fixed-width set is a mechanical map onto the helper
table; the relaxed set (20 ops) is deliberately the *easiest* part, because
[`proposals/SIMD.md`](proposals/SIMD.md) SD5 lets us return the plain scalar
result and stay conformant.

## 8. Performance model and the batched-path threshold

### 8.1 Where the cycles go for one `v128` op (design D5)

| Component | Rough cost | Notes |
|---|---|---|
| Helper call | 2–5 cycles + predictor pressure | Direct `jit_insn_call_native` target; via `HWY_DYNAMIC_DISPATCH` it is one extra indirect hop inside the wrapper |
| Argument setup | ~2 instructions | The slot addresses are frame offsets, so they are `lea`/immediate, not loads |
| The actual SIMD op | 1–4 cycles | `paddd`, `pminub`, `pshufb`, … |
| Slot round-trip | ~4–10 cycles | One 16-byte store into the `out` slot; the consumer reloads it |

So the boundary (call + slot traffic) is **roughly 5–10× the inner op**. That is
why §6 fixes the boundary and §5 refuses to optimise the op, and why the only
realistic step change is not "a better library" but **D4 (removing calls
entirely for runs of ops)** or the **D5 peepholes** (removing the intermediate
slot for producer/consumer pairs).

### 8.2 What the benchmark (T4) must measure

| # | Variant | What it establishes |
|---|---|---|
| M1 | D5 pointer-arg helpers | The baseline we ship |
| M2 | D2 by-value 16-byte struct helpers | Whether LibJIT's aggregate ABI (S9) is both correct *and* faster; if yes, §6.3's signatures change, nothing else |
| M3 | D1 fully-inline scalar IR for a per-lane op | The cost of the no-call route, and whether the D1 candidate set should grow |
| M4 | A native C reference loop | The upper bound no JIT-side design can beat |
| M5 | M1 with the D5 peepholes applied | The value of fusing `load`+op / `const`+op |

Protocol: a WAT-authored module with one fixed workload (e.g. a `v128` dot
product or byte shuffle over a fixed N), run from the Catch2 harness with the
same input for every variant, best-of-N, reported as **ns per `v128` op**, with
the module and the N recorded in the test. This is a diagnostic, not a CI gate.

### 8.3 The gate for D4

Implement the batched/fused-run helper (D4) **only if** the benchmark shows the
call + slot traffic exceeding half of the total loop time after the D5 peepholes
are in place. Until then D4 stays documented and unbuilt — and if it is ever
built, *that* is when wider-than-128-bit vectors and Highway's runtime dispatch
start paying for themselves, because a batched helper can process several
`v128` values per instruction.

## 9. The test plan, documented as tests

Four layers, each with a different job. Every layer is a documented artefact —
a Catch2 case, a `.wat` file, a `wast_supported.txt` entry or a bench module —
so the tests *are* the documentation of the behaviour.

### T1 — Differential oracle (the primary correctness instrument)

Every helper is tested against an independent scalar implementation over the
same inputs, in `Test/main.cpp` as Catch2 cases, one per op family
(`simd helpers: lane ops`, `simd helpers: saturating arithmetic`, …).

| Property | Detail |
|---|---|
| Oracle | `WasmBase/wasm_simd_scalar.*` — a second, deliberately naive implementation of each helper (per-lane loops, `std::` maths). Never shares code with the SIMD path. |
| Input classes | randomized (seeded, reproducible); `0x00`/`0xFF`/alternating patterns; lane-wise `0`/`1`/`INT_MIN`/`INT_MAX`; float `±0.0`, `±inf`, `qNaN`, `sNaN`, denormals, `2^31` and `2^31 ± ulp` |
| Comparison | **bit-exact 16-byte equality** plus, for the float families, the wasm NaN rule (any NaN in → canonical NaN out); never `==` on floats |
| Lane-position checks | the result is compared lane by lane with an explicit lane index, so a permuted `narrow`/`widen`/`extmul` result fails (hazard Y6) |
| Coverage gate | every `SIMDOpcode` handler must have at least one T1 case, or an explicit, listed exemption (the "no silent gap" rule) |

This layer is what catches Y1–Y11, and it is the only layer that isolates a
*helper* bug from a JIT bug.

### T2 — End-to-end lane semantics (`Test/wasm_wat/simd_*.wat`)

Hand-written modules, parsed by `wasm-tools parse` (already a build
prerequisite, see `Test/Test.pro`), each exercising one thing through the full
JIT path with exact expected vectors:

| Module | Covers |
|---|---|
| `simd_load_store.wat` | `v128.load`/`store` incl. unaligned offsets, `load*_splat`, `load32/64_zero`, lane load/store |
| `simd_const_lane.wat` | `v128.const` byte order (Y11), splat, extract/replace lane (signed and unsigned) |
| `simd_arith_i.wat` | integer add/sub/mul, shifts (incl. shift count `≥` lane width), `min/max`, `avgr_u`, saturating forms, `bitmask`, `all_true`/`any_true` (Y9) |
| `simd_arith_f.wat` | float arithmetic, `min`/`max`/`pmin`/`pmax` (Y1), `sqrt`, `abs`/`neg`, `ceil`/`floor`/`trunc`/`nearest` (Y7) |
| `simd_shuffle.wat` | `i8x16.shuffle` two-operand mapping (Y4), `i8x16.swizzle` out-of-range (Y3) |
| `simd_convert.wat` | `trunc_sat` corners (Y2), `convert`, `promote`/`demote`, widen/narrow (Y6), `extmul`, `extadd_pairwise`, `dot`, `q15mulr_sat_s` (Y5) |
| `simd_abi.wat` | `(param v128) (result v128)` round-trip; `v128` locals; `v128` global get/set |

These are the modules the milestone "done when" clauses refer to.

### T3 — Spec conformance (`simd_*.wast`)

The 59 upstream SIMD scripts only become gradeable after the harness marshals
`v128` arguments and results (`WastScript.cpp` currently skips them:
[`TESTING.md`](TESTING.md)). Once lifted, curate the passing ones into
`Test/wast_supported.txt` per the existing refresh recipe. Until then T3 stays
red and progress is measured by T1/T2 — the note must keep saying so.

### T4 — Benchmark (diagnostic)

Per §8.2: a WAT workload plus a Catch2 case that prints `ns/v128-op` for M1–M5
and the native reference. Documented in [`TESTING.md`](TESTING.md) as a manual,
non-gating run.

### Running them

```bash
cd build/Desktop-Debug/Test
./WasmJit '~[spec]'                       # T1 (unit tests) + T2 (wasm_wat modules)
./WasmJit 'simd helpers*'                 # T1 alone, by case-name prefix
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: simd_' -r compact   # T3, one script at a time
WASM_SPEC_SCRIPTS=all ./WasmJit '[spec]'  # T3 sweep, for re-curation
./WasmJit '[simdbench]'                   # T4, opt-in by tag
```

### Indicative survey at the time of writing

| Layer | Script / case | Result | Reading |
|---|---|---|---|
| T1 | `simd helpers: v128.const and *.splat follow the wasm byte layout` | pass | SIMD-L1 |
| T1 | `simd helpers: extract_lane and replace_lane` | pass | SIMD-L1 |
| T1 | `simd helpers: the entry points and the scalar reference agree` | pass | pins the seam SIMD-L2 makes interesting |
| T2 | `simd: a v128 parameter and result round-trip through the JIT` | pass | **closes S4/S5/S9** |
| T2 | `simd: v128.const, splat and lane ops run through the JIT` | pass | SIMD-L1 |
| T3 | the 59 `simd_*.wast` scripts | signal 6, 0 passing commands | still S1 (the ~215 handlers that remain stubs) and S7 |
| T4 | — | — | SIMD-L8 |

Measured on the SIMD-L1 build: `./WasmJit 'simd*'` reports **5 cases, 218
assertions, all passing**; the whole default suite is **91 cases / 1755
assertions** (86 / 1537 before SIMD-L1), so the change added coverage without
regressing the curated set.

Reproduce one T3 row with
`WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: simd_load' -r compact`.

## 10. Milestones

`SIMD-L<n>` are this note's milestones; `SIMD-<n>` are
[`proposals/SIMD.md`](proposals/SIMD.md) §7's, and `S<n>` are its §5 gaps. The
two lists line up: L1–L7 are the *how* for SIMD-0…SIMD-6.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SIMD-L0 — This note** | Write this decision record and the consistency edits to `SIMD_BACKENDS.md`, `proposals/SIMD.md`, `README.md`, `TESTING.md`. No code. | — (the gate) | The five documents agree on the direction, the ABI, the library and the test plan. |
| **SIMD-L1 — Scalar reference + ABI** | ✅ **Done.** `wasm_simd_scalar.{hpp,cpp}` (the reference for every L1 helper, plus the `extern "C"` entry points); `jit_type_set_size_and_alignment(v128, 16, 16)`; `gcStorageAlign` → 16; the two 16-byte zero-value paths corrected; `callSimdHelper`/`callSimdOutHelper` + signature cache; 21 ops wired (const, 6 splats, 8 extracts, 6 replaces); `simd_abi.wat` + `simd_const_lane.wat`; 5 T1/T2 cases. | S4, S5, S9 | ✅ `(param v128)(result v128)` round-trips through the JIT (measured); `v128.const` → `splat` → extract/replace matches the golden vectors; suite 91/1755, zero regressions. |
| **SIMD-L2 — Highway layer + memory/bitwise/lane** | The multi-target TU and the qmake lines (§5.5); helpers for `v128.load`/`store` (+ splat/zero/lane variants), bitwise `and/andnot/or/xor/not/bitselect`, `*.splat`, `extract_lane`, `replace_lane`, `shuffle`, `swizzle`; the D5 peepholes for `load`+op and `const`+op. | S1 (part) | `simd_load_store.wat`, `simd_shuffle.wat` green; T1 green for the whole L2 op set; the JIT links against `libhwy` under qmake. |
| **SIMD-L3 — Integer and comparison ops** | add/sub/mul, neg/abs, `min/max`, shifts, `avgr_u`, saturating add/sub, `q15mulr_sat_s` (Y5), comparisons, `all_true`/`any_true`/`bitmask` (Y9), `extmul`, `extadd_pairwise`, `dot`. | S1 | `simd_arith_i.wat` green; the i32-returning assertions of the `simd_i32x4_*`/`simd_i16x8_*` scripts pass where T3 is reachable. |
| **SIMD-L4 — Float and conversion ops** | arithmetic, `min/max`/`pmin/pmax` (Y1), `sqrt`/`abs`/`neg`, rounding (Y7), conversions (`trunc_sat` Y2, `convert`, `promote`/`demote`, widen/narrow Y6, `i64x2` shifts Y8). | S1 | `simd_arith_f.wat`, `simd_convert.wat` green; T1 green for the whole fixed-width set (the §9 coverage gate holds). |
| **SIMD-L5 — Globals and constant expressions** | `v128.const` in `evalConstantExpr`; `v128` global get/set; `v128` in the globals layout. | S3 | A module with a `v128` global initialised by `v128.const` runs and round-trips. |
| **SIMD-L6 — Relaxed SIMD** | Enumerate the 20 relaxed sub-opcodes; decode them; implement the SD5 scalar results. | S2 | A module using each relaxed family produces a member of the allowed set; T1 checks membership, not equality. |
| **SIMD-L7 — Harness marshalling** | Marshal `v128` arguments/results in `WastScript.cpp` (16-byte values, lane formatting); then curate `simd_*.wast` into `wast_supported.txt`. | S7, S8 | `./WasmJit 'spec: simd_*'` reports real pass/fail counts, and every curated script is listed in the T3 survey. |
| **SIMD-L8 — Benchmark and the D4 decision** | M1–M5 per §8.2; apply the §8.3 gate; record the numbers in this note's §8 and in `TESTING.md`. | — | The note states the measured `ns/v128-op` and whether D4 is built, deferred or rejected. |

Guardrails: the scalar reference stays compiled and green at every step; run
`./WasmJit '~[spec]'` after each milestone; never let a real fix regress the
curated spec set.

## 11. Risks and open questions

| # | Risk / question | Status |
|---|---|---|
| R1 | **Frame growth** on SIMD-dense functions (16 bytes per static `v128` value). | Accepted for L1–L7; mitigate with a height-keyed shared bank or slot reuse only if a real module hits it (§6.2). |
| R2 | **Highway compile time** from per-target re-inclusion (SSE2…AVX3 + EMU128 + SCALAR). | Accepted; `HWY_COMPILE_ONLY_*` is the knob. Measure at L2. |
| R3 | **`f32x4.min/max` NaN semantics** (Y1) are the likeliest silent conformance failure. | Mitigated by T1's unordered-mask cases and T2's `simd_arith_f.wat`; keep both non-negotiable. |
| R4 | **`swizzle` sanity** (Y3) — indices 16…127 must zero, not alias. | Explicit T1 case with indices `16`, `127`, `128`, `255`. |
| R5 | **`jit_insn_store_relative` silently does not write a `jit_value_create`d aggregate.** Found and fixed during SIMD-L1: `v128.const` and both v128 zero-value paths used it and produced uninitialised slots (§6.6). | Fixed (SIMD-L1); the rule "write a struct-typed value through its address" is now in the header comment and enforced by the §6.6 table. |
| Q1 | Does LibJIT's 16-byte aggregate ABI (S9) work *and* beat the pointer ABI? | **Correctness answered (SIMD-L1): the ABI works** — `(param v128)(result v128)` round-trips through the JIT and the C++ mirror `{uint32_t×4}` matches it (§6.6). Whether by-value *beats* by-pointer is still the M2 benchmark at L8. |
| Q2 | How is a `v128` value printed on a test failure? | Lane-wise hex with lane indices, defined at L1 (T1 diagnostics and T3 output both use it). |
| Q3 | Do `v128` globals live as 16 inline bytes in the VM context? | Confirm at L5 against `WasmModuleInstance`'s globals layout and `storageAlign`. |
| Q4 | Does the D1 inline set grow beyond const/load/store/splat/extract/replace? | Decide from M3 at L8. |
| Q5 | Is the deterministic profile ever adopted for relaxed SIMD? | No (SD6); revisit only if cross-machine determinism becomes a requirement. |
| Q6 | Is the SIMDe escape hatch ever needed? | Only if a second shared-library dependency becomes unacceptable; the helper bodies are written against a thin internal wrapper precisely so this stays cheap. |
| Q7 | Does the harness marshal `v128` by pointer or as `WASM::Value`? | `WASM::Value` — it is already `alignas(16)` with a `v128[16]` member; L7 is a formatting task, not an ABI one. |

## 12. References

Engine and library documentation (all paths verified on this machine):

- GNU LibJIT — installed headers inspected for §2 and §6:
  `/usr/local/include/jit/` (`jit-type.h`, `jit-insn.h`, `jit-opcode.h`).
  Upstream API reference: <https://www.gnu.org/software/libjit/doc/libjit.html>
- Highway 1.4.0 — `hwy/highway.h` (the `HWY_EXPORT`/`HWY_DYNAMIC_DISPATCH`
  contract, `HWY_CAPPED`), `hwy/base.h` (`HWY_PUSH_ATTRIBUTES` → the per-target
  pragmas), `hwy/foreach_target.h` (the `HWY_TARGET_TOGGLE` re-inclusion
  protocol), `hwy/per_target.h`, `hwy/ops/{generic_ops,x86_128,x86_avx3,emu128,wasm_128}-inl.h`
  (op coverage), `libhwy.pc` (`-DHWY_SHARED_DEFINE`, `-lhwy`).
  Upstream: <https://github.com/google/highway>
- SIMDe 0.8.2 — `simde/x86/{sse2,ssse3,sse4.1,sse4.2}.h`,
  `SIMDE_X86_SSE2_NATIVE` (the native-forwarding path that makes it a
  drop-in on x86). Upstream: <https://github.com/simd-everywhere/simde>
- xsimd 14.3.0 — `xsimd/types/xsimd_api.hpp` (the API checked for saturating
  ops). Upstream: <https://github.com/xtensor-stack/xsimd>
- GLM 1.0.3 — `glm/simd/integer.h` (two functions, the whole integer SIMD
  surface). Upstream: <https://github.com/g-truc/glm>
- Others surveyed: `sleef.h`, `eigen3.pc`, `/usr/include/Vc`,
  `/usr/include/psimd.h`.

WebAssembly specification (same anchors as the proposal note):

- Syntax — Vector instructions:
  <https://webassembly.github.io/spec/core/syntax/instructions.html>
- Execution — Numerics, incl. the "Relaxed Operations" parameterisation:
  <https://webassembly.github.io/spec/core/exec/numerics.html>
- Appendix — Profiles (the deterministic profile pins every relaxed parameter
  to `0`): <https://webassembly.github.io/spec/core/appendix/profiles.html>

In-repo:

- [`proposals/SIMD.md`](proposals/SIMD.md) — the proposal note: §2 the demanded
  surface, §3 the freedoms, §5 S1–S9 the gaps, §7 SIMD-0…SIMD-7 the milestones
  this note implements.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) — the survey this note turns into a
  decision (its §6 recommendation and §5/W2 sketch are superseded by §6 here).
- [`SAFETY.md`](SAFETY.md) — the allocation/trap policy §6.2 and the memory
  helpers must respect.
- [`TESTING.md`](TESTING.md) — how the four test layers are run, and where the
  `v128` skip (S7) is recorded.
- [`ABI.md`](ABI.md) — the `VMContext*`-as-arg0 convention every helper
  signature in §6.3 follows.
- Source anchors: `LibJit/LibJitTypeTranslation.cpp` (`v128_definition`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (the helper pattern at 489–532 and the
  0xFD stub block from 3876), `LibJit/LibjitModuleCompiler.cpp`
  (`compileFunction`, `PreparedFunctionStack::prepare` call site),
  `WasmBase/WasmType.cpp:4` (what `prepare` does *not* compute),
  `WasmBase/WasmModuleInstance.cpp` (`evalConstantExpr`, struct/global layout),
  `Test/Test.pro` (the `wat`/`wast`/AssemblyScript build rules),
  `Test/wast_supported.txt`.

