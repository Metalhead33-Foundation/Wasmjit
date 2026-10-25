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
WasmBase/wasm_simd_vector.hpp   # per-target bodies; toggled guard (see below)
WasmBase/wasm_simd_vector.cpp   # the only file that knows about Highway's dispatch
```

(An earlier draft called these `wasm_simd_hwy.*`; §6.5 records why the core
named them after the layer instead of the library.)

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
| `WasmBase/wasm_simd_vector.{hpp,cpp}` | The vectorised bodies, compiled once per CPU target, plus every `extern "C"` entry point and the `HWY_EXPORT` dispatch tables. The only place where the SIMD library is visible. | SIMD-L2 |
| `WasmBase/wasm_simd_scalar.{hpp,cpp}` | The **scalar reference** implementation of every helper — no library, always compiled, never called by a backend. The oracle of §9. Also declares the entry-point contract (the `extern "C"` block), because that contract is core-owned. | SIMD-L1 |
| `WasmBase/wasm_simd_ops.inc` | The op list: one line per op, expanded **once per layer** (reference, vector bodies, entry points, and the test table). No include guard, by design. | SIMD-L3 |
| `LibJit/LibjitOpcodeDispatcher.{hpp,cpp}` | Turns the `notImplemented(__func__)` stubs into helper calls: `callSimdHelper`/`callSimdOutHelper` (§6.4), the `emitSimd*` shapes, and the per-op handlers. | SIMD-L1…L4 |
| `LibJit/LibJitTypeTranslation.cpp` | The 16-byte alignment line (§6.1) | SIMD-L1 |
| `WasmBase/WasmModuleInstance.cpp` | `evalConstantExpr` `v128.const` arm; `gcStorageAlign` | SIMD-L5 / L1 |
| `Test/wasm_wat/simd_*.wat` | Hand-written lane-semantics modules | SIMD-L1+ |

Two things about this layout are worth stating explicitly, because they were
*decided during* SIMD-L2 rather than before it:

1. **The vector TU is named after the layer, not after the library.** An earlier
   draft of this note called it `wasm_simd_hwy.*`. That would have baked an
   implementation choice into a core filename — and this note's own §5.3 keeps
   the library swappable (SIMDe is the documented escape hatch), so the name
   would have been a lie the day it was used. The rename is deliberate.
2. **Which layer owns an entry point is decided by whether a vector path pays.**
   `wasm_simd_vector.cpp` implements the entry points that have one — the
   bitwise ops, whole-vector load/store, the broadcasts, shuffle and swizzle —
   and for the rest *forwards to the reference*: for a single-lane move or a
   widening copy, the byte move **is** the optimal lowering, so a second
   implementation would be duplicated code with nothing to gain. The consequence
   for §9 is that only the former group can be differentially tested; the
   forwarding group is pinned by golden vectors, and the test comments say so.
   At SIMD-L4 that rule stopped being a list of hand-written exceptions: the op
   list gained a `FORWARD` shape, so an op that delegates says so *in the list*,
   and its entry point (and its deliberate absence from the differential) comes
   from the same mechanism as everything else. Exactly one pair of ops uses it —
   `i32x4.trunc_sat_f64x2_{s,u}_zero` — for the reason in §11 R10: the library
   has no truncating float64→int32 conversion at all, so a "vector path" here
   would be a second hand-written implementation of the spec rather than a
   mapping onto the library, which is precisely what the rule calls delegation
   for.

   This rule is *inside* the helper layer: it decides which of the two
   core-owned implementations answers a call. Above it sits the backend-level
   equivalent — these entry points are what LibJIT calls, and a backend with
   native vector registers may lower `v128` itself instead, keeping the same
   semantics ([`LAYERING.md`](LAYERING.md) §4; `proposals/SIMD.md` SIMD-8). That
   path is deliberately not one of §10's milestones: it belongs to the backend
   that has the registers.
3. **The per-lane op set is one list, expanded four ways.** At 133 ops in
   SIMD-L3 alone, four hand-written layers (reference, per-target bodies, entry
   points and the test table) would be hundreds of near-identical functions that
   can silently drift as the list grows. `wasm_simd_ops.inc` is that list: each
   layer defines the shape macros it implements, includes it, and `#undef`s them
   again. The win is not only brevity — `Test/main.cpp` builds its differential
   table from the *same* file, so an op cannot exist in the engine without
   appearing in the differential test. The §9 coverage gate is structural now,
   rather than a promise somebody has to remember to keep.

   Three mechanics of that file cost real time to discover, and are worth knowing
   before editing it:

   - **A layer defines only the shapes it implements.** The file is included
     whole, so every inclusion would otherwise have to name every shape — and
     forgetting one leaves those lines unexpanded, which surfaces as an "expected
     constructor" error in an unrelated file. It therefore defaults any shape the
     caller did not define to nothing, and undoes exactly those defaults on the
     way out so the next inclusion starts clean.
   - **An expression may not name a macro parameter.** The preprocessor
     blue-paints them, so `T` or `TOut` written inside a field reaches the
     compiler as a bare identifier. Fields also have to be comma-free at the top
     level, or the comma splits the argument list.
   - **Fields name the library op `hn::`-prefixed — except the binary shapes and
     the reductions.** The binary shapes call their field with the two loaded
     vectors and let ADL find it, which is what makes *our* helpers reachable
     while the library's ops resolve exactly as they do inside the library. The
     reductions take a complete expression, because `v128.any_true` ends in a
     comparison and `hn::(x != 0)` is not valid syntax.

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
| `(param v128 v128 v128)` and `(param v128 ×4)` | round-trip exactly **through a direct call** — the stack-passed third and fourth aggregates are handled correctly by the generated code (SIMD-L7's `simd_abi_args.wat`) |
| `jit_apply` with ≥ 3 `v128` arguments | **does not work** — the third aggregate, which the ABI stack-passes, arrives damaged. This is libjit's *dynamic-call* path disagreeing with the ABI its own generated code follows; the callee side is verified correct by the row above, so the limitation is in the harness's `jit_apply` use, not in our lowering. **Worked around at SIMD-L7:** the harness calls one-to-four-`v128` signatures through the host C ABI directly (`ScriptRunner::invoke`), which is what `WASM::callCallable` does and what the callee expects; every SIMD spec script now passes. A signature mixing `v128` with scalars still goes through `jit_apply`, so the defect is *bounded*, not fixed — and it is libjit's to fix. |
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
| **Y1** | `f32x4`/`f64x2` `min`/`max` (and `pmin`/`pmax`) | `minps`/`Min` returns the **second** operand when either is NaN, and for `±0` returns the second operand. wasm requires a **canonical NaN** when either input is NaN, and *min* must pick `-0` / *max* must pick `+0`. **Confirmed at SIMD-L4, with two extra traps the hazard note did not anticipate**: the bitwise `Or`/`And` that orders the zeros does *not* preserve a NaN in the `And` case (clearing its exponent bits yields an ordinary number — my first `max` had exactly that bug, and the differential caught it), and the library's float `Ne` is the **ordered** not-equal, so it cannot even be used to *detect* a NaN operand. | Implemented: an explicit NaN step (`Not(Eq(v, v))`, then `Add` of the operands to propagate), then the ordering, then the zero rule. `pmin`/`pmax` stay the spec's plain `b < a ? b : a`, which is why they *don't* propagate — the opposite of `min`/`max`. |
| **Y2** | `i32x4.trunc_sat_f32x4_{s,u}`, `_f64x2_{s,u}_zero` | `cvttps2dq` yields `0x80000000` for NaN and for out-of-range — indistinguishable from a legitimate `-2^31`. | NaN → `0`; `≥ 2^31` → `INT32_MAX`; `≤ -2^31 - 1` → `INT32_MIN`; unsigned forms clamp to `[0, UINT32_MAX]`. Fix up the raw conversion. |
| **Y3** | `i8x16.swizzle` | `pshufb`/`BitShuffle` zeroes a lane only when the index's **bit 7** is set; wasm zeroes every index `≥ 16`, so indices 16…127 would silently alias lanes 0…15. | One **unsigned** range mask: `idx <= 15` takes the looked-up byte, everything else takes zero. That single select also covers 128…255, so pshufb's bit-7 rule never has to be reproduced *or* relied on (§11 R7) — which matters because the SSE2 and SSSE3 paths differ there. **The single most likely silent bug in the whole feature.** |
| **Y4** | `i8x16.shuffle` | Indices 0–15 select operand 0, 16–31 operand 1; a shuffle primitive that takes "one selector vector" invites an off-by-16 or a reversed concatenation. | Build the two-operand concatenation explicitly; verify against the oracle. |
| **Y5** | `q15mulr_sat_s` | `pmulhrsw`/`MulFixedPoint15` rounds `(a*b + 0x4000) >> 15` but yields `0x8000` for the `-32768 × -32768` corner, where wasm requires saturation to `0x7fff`. **Confirmed** — and it is an inconsistency *inside* the library: its SSE2 path demotes with `ReorderDemote2To` and saturates, while the SSSE3+ path is the bare instruction and wraps, so the same op disagrees with itself across targets. | Implemented at SIMD-L3 as a one-lane select on the only input pair that can overflow. Note that a differential test **cannot** see this: both layers would wrap identically, which is why the golden vector (`q15_sat_lane0` in `simd_arith_i.wat`) is what pins it. |
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

This table was the *model* the design was justified with. §8.4 then measured it,
and the measurement moved the cost out of the slot and into the call — so the
right-hand column now records both.

| Component | Modelled cost | Measured (§8.4) |
|---|---|---|
| Helper call | 2–5 cycles + predictor pressure | **~12 cycles for the whole crossing**, and it is nearly all of it: `v128.load` behind one call is 4.02 ns, `i32x4.add` 4.30 ns, i.e. the work is below the noise. Two nested calls (entry point → Highway's dispatched body), one indirect branch each. |
| Argument setup | ~2 instructions | as modelled — `lea`/immediate frame offsets |
| The actual SIMD op | 1–4 cycles | ~1 cycle; invisible at this resolution (`paddd` behind a call) |
| Slot round-trip | ~4–10 cycles | present but **minor** — the 16-byte store/load is a couple of cycles against a ~12-cycle call |

So the boundary is **roughly 40× the inner op** (M1 = 4.10 ns/op, the no-call
bound 0.099), and the *call*, not the slot, is the expensive part. That is why §6
fixes the boundary and §5 refuses to optimise the op, and why the only realistic
step change is not "a better library" but **removing calls**: **D4 (one call for
a run of ops)** or the **D5 peepholes** (fusing `load`/`const` into the consumer,
which removes a call rather than a copy).

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

### 8.4 Measured (SIMD-L8)

Recorded 2026-10-17 on x86-64, `build/Release` (the project's release
configuration: `-O2`, GCC 15), 200 000 iterations × 6 `v128` ops per iteration,
best of seven, one representative run — repeat runs land within ±8 %, so read the
figures as bound-resolving, not three-digit-precise. Run it with
`./WasmJit '[.simdbench]'` (the leading dot keeps it out of the default run — T4);
the workload is
[`Test/wasm_wat/simd_bench.wat`](../Test/wasm_wat/simd_bench.wat):

| Variant | What it is | ns per `v128` op |
|---|---|---|
| **M0a** | one helper call, `v128.load` only (a 16-byte copy behind it) | **4.02** |
| **M0b** | one helper call, `i32x4.add` only | **4.30** |
| **M1** | shipped design: the wasm loop through the JIT, pointer helpers | **4.10** |
| **M3** | the D1 route: lane-wise scalar IR, one *direct* call per op | **3.35** |
| **M4** | native C calling the same helpers (no JIT in the loop) | **4.71** |
| **M4n** | native C, no calls at all (auto-vectorised) — the upper bound | **0.099** |

Readings, in order of how much they change the picture:

1. **The call *is* the cost.** M1 (4.10) sits between M0a (4.02) and M0b (4.30):
   a whole per-op budget is one helper call, and the SIMD work behind it is
   below the resolution of the measurement. M1/M4n = **42×**, i.e. 97.6 % of the
   loop is `helper-call share`.
2. **The lowering is not the problem.** M1 (4.10) beats M4 (4.71): the JIT emits
   the same work C++ does and calls the entry points directly instead of through
   the PLT. There is no JIT-side inefficiency to recover — which is exactly why
   §5 refuses to optimise the op and §6 fixes the boundary instead.
3. **§8.1's model was wrong about *where* the cycles go.** It predicted the
   boundary as "call 2–5 cycles + slot round-trip 4–10"; measured, the whole
   crossing is ~12 cycles at ~3 GHz and the arithmetic is ~1. The slot traffic is
   not the expensive half — the call is (two nested calls: the entry point, then
   Highway's dispatched body, and one indirect branch each).
4. **M3 says an ABI-level fix has limited headroom.** Lane-wise scalar IR — four
   loads, four adds, four stores *instead of one SIMD instruction* — is 18 %
   **faster** (3.35 vs 4.10), purely because it is one direct call rather than a
   dispatched one. So anything that removes a call wins more than anything that
   makes the callee cheaper. Q4's answer follows: the D1 set should **not** grow
   (the win is the call mechanics, not the IR), and D5's peepholes are the cheap
   lever — they remove a call, not work.
5. **A release build found a real defect in the relaxed ops — not a performance
   one.** `relaxed_madd`/`nmadd` were implemented as `Add(Mul(a, b), c)`, which
   GCC contracts into an FMA on the FMA-capable Highway targets while the
   reference (pinned by a `volatile`) stays unfused: debug and release then
   returned *different values for the same module*, and both the `simd relaxed`
   differential and the `relaxed_madd_nmadd` spec script failed — in release
   only. The four ops now forward to the reference, which makes the permitted
   choice structural. The benchmark is what forced the release build, and the
   release build is what found it: a suite that only ever runs unoptimised cannot
   see this class of bug.
6. **Unbuilt variants, stated plainly.** M2 (by-value 16-byte struct helpers) was
   not measured: the ABI *works* (§6.6, verified at L1) but measuring "faster"
   needs by-value signatures plumbed through the helper layer, which is a change
   to §6.3 rather than a probe. M5 (peepholes applied) is not measurable because
   the peepholes are not built (§5's L2 row defers them). Both are one
   benchmark run away once built.

**The gate (§8.3), applied as far as the built variants allow.** The condition is
call + slot traffic > 50 % of loop time *after* the peepholes. Before them the
measured share is **97.6 %**, and a peephole can only *remove* calls, never add
any: at most it fuses `load`+op, which for this workload takes 6 calls per
iteration to 3, leaving one call — ~4 ns — per op against a ~4.1 ns budget. The
gate therefore holds for **any** peephole outcome, so D4's *justification* does
not depend on M5. Its **verdict is deferred anyway**, for a reason the numbers
make plain: the same measurement says a peephole is a small, contained change
worth up to 2× on memory-fed loops, while D4 is a new design (a run script, a
bank layout, a different helper ABI). Building the expensive thing before
measuring the cheap one would be a guess dressed as a decision. Order: peephole
(M5) → re-measure → D4.



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
| ✅ `simd_load_store.wat` | `v128.load`/`store` incl. unaligned offsets, `load*_splat`, `load32/64_zero`, lane load/store |
| ✅ `simd_const_lane.wat` | `v128.const` byte order (Y11), splat, extract/replace lane (signed and unsigned) |
| ✅ `simd_arith_i.wat` | **Built at SIMD-L3 (67 exports).** Comparisons in both signednesses, `add`/`sub`/`mul`, `neg`/`abs`, shifts incl. counts `≥` the lane width, `min/max`, `avgr_u`, the saturating forms, `popcnt`, `q15mulr_sat_s` (Y5), the reductions `all_true`/`any_true`/`bitmask` in every width (Y9), and the width-changing family — `extend`/`extmul` `_low` vs `_high`, `extadd_pairwise`, `dot`, `narrow`'s half order and clamping |
| ✅ `simd_arith_f.wat` | **Built at SIMD-L4 (32 exports).** Float comparisons (incl. the unordered `ne`), `add`/`sub`/`mul`/`div`, `abs`/`neg`/`sqrt`, the rounding family, the four min/max flavours with the `±0` and NaN rules (Y1), and the `f64x2` counterparts — every value projected as a bit pattern, so a NaN's payload and a zero's sign survive the harness |
| ✅ `simd_shuffle.wat` | `i8x16.shuffle` two-operand mapping (Y4), `i8x16.swizzle` out-of-range (Y3) |
| ✅ `simd_convert.wat` | **Built at SIMD-L4 (19 exports).** The float↔int conversions at the ends of their ranges: `trunc_sat` (NaN, ±2^31, 2^32 and the largest value below it), `convert`, `convert_low` (with marker lanes proving the high half is ignored), `promote`, and `demote`/`trunc_sat_f64x2_*_zero` (with marker lanes proving the unfilled lanes are zero). Every value read off V8 (case `simd: the float/int conversions run through the JIT`) |
| ✅ `simd_abi.wat` | `(param v128) (result v128)` round-trip; `v128` locals; `v128` global get/set |

These are the modules the milestone "done when" clauses refer to.

### T3 — Spec conformance (`simd_*.wast`)

The upstream SIMD scripts became gradeable once the harness learned to marshal
`v128` arguments and results (`decodeV128` / `compareV128` in
`Test/WastScript.cpp`, [`TESTING.md`](TESTING.md) Part 3). **All 66 `simd_*` /
relaxed scripts are now curated in `Test/wast_supported.txt` and pass with
`failed=0`**, so T3 is green; the per-script rows are in the survey below and the
refresh recipe is the one in [`TESTING.md`](TESTING.md).

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
./WasmJit '[.simdbench]'                  # T4, opt-in (hidden tag: the dot keeps it out of the default run)
```

### Indicative survey at the time of writing

| Layer | Script / case | Result | Reading |
|---|---|---|---|
| T1 | `simd helpers: v128.const and *.splat follow the wasm byte layout` | pass | SIMD-L1 |
| T1 | `simd helpers: extract_lane and replace_lane` | pass | SIMD-L1 (golden vectors: one implementation, §6.5) |
| T1 | `simd helpers: the entry points and the scalar reference agree` | pass | pins the seam |
| T1 | `simd helpers: bitwise ops agree with the scalar reference` | pass | first genuine differential (vectorised vs reference) |
| T1 | `simd helpers: memory loads and stores` | pass | differential + golden for the widening/zero/lane forms |
| T1 | `simd helpers: shuffle and swizzle` | pass | hazards Y3/Y4 pinned by boundary vectors |
| T1 | `simd helpers: per-lane arithmetic and comparisons (differential)` | pass | driven by the op list (§6.5), so an op cannot be uncovered |
| T1 | `simd helpers: signedness corners are pinned` | pass | golden: `min`/`max`/compare signedness, `abs`/`neg` of the most negative value |
| T1 | `simd helpers: the saturating, rounding and count corners` | pass | golden: saturation in both signednesses, `avgr_u` rounding, `popcnt`, Y5's two `q15mulr_sat_s` corners — **cross-checked against an independent engine** (§11 R8) |
| T1 | `simd helpers: shift masking, narrowing layout and the widening family` | pass | golden: counts taken mod the lane width, `narrow`'s half order, `bitmask` bit order, the wrapping `dot` sum |
| T2 | `simd: a v128 parameter and result round-trip through the JIT` | pass | **closes S4/S5/S9** |
| T2 | `simd: v128.const, splat and lane ops run through the JIT` | pass | SIMD-L1 |
| T2 | `simd: bitwise ops run through the JIT` | pass | SIMD-L2 |
| T2 | `simd: memory loads and stores run through the JIT` | pass | SIMD-L2, incl. an unaligned memarg and the bounds-check path |
| T2 | `simd: shuffle and swizzle run through the JIT` | pass | SIMD-L2 |
| T2 | `simd: integer arithmetic and comparisons run through the JIT` | pass | **SIMD-L3**, driven by `simd_arith_i.wat` (67 exports: comparisons, `min`/`max`, shifts, saturating, `avgr_u`, `popcnt`, `q15mulr_sat_s`, the reductions and the whole width-changing family) |
| T1 | `simd helpers: the float corners the hardware gets wrong` | pass | golden: hazard Y1 (±0 ordering, NaN propagation) and `nearest`'s ties-to-even — **from an independent engine**, not from arithmetic of mine |
| T2 | `simd: float arithmetic and comparisons run through the JIT` | pass | **SIMD-L4**, driven by `simd_arith_f.wat` (32 exports, all as bit patterns: NaN comparisons, the zero rules, NaN propagation vs `pmin`/`pmax`, ties-to-even, and the `f64x2` families) |
| T2 | `simd: the float/int conversions run through the JIT` | pass | **SIMD-L4**, driven by `simd_convert.wat` (19 exports at the ends of the ranges; two of them `FORWARD`, which is why their conformance rests on this case rather than on a differential) |
| T3 | the 66 `simd_*` / relaxed scripts | **all 66 fully pass** (curated) | **S7 closed.** `v128` globals (SIMD-L5) and the ≥ 3-operand call path (SIMD-L7) were the last two causes; neither needed a change to the generated code — one was a missing constant-expression arm, the other libjit's `jit_apply` |
| T4 | `simd benchmark: ns per v128 op (SIMD-L8)` | 4.10 ns/v128-op (M1) | **SIMD-L8.** 42× off the no-call bound; 97.6 % of it is the helper call — see §8.4 |

Current measurement: `./WasmJit 'simd*'` reports **24 cases, 6147 assertions, all
passing**, and the whole default suite is **440 cases / 10982 assertions**. The
milestone-by-milestone progression was 102 cases / 4196 assertions after L3, 104 /
7246 after L4's float half, 105 / 7421 at L4, and it has only grown since — added
coverage, no regressions. **All 236 fixed-width handlers execute**, and the op
list holds 185 of them (§6.5): the rest are the memory, lane and bitwise ops
that predate it.

The `.wat` modules are checked with `wasm-tools validate --features all` before
they are run, so a T2 failure is a runtime bug rather than a malformed input.

Reproduce one T3 row with
`WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: simd_load' -r compact`.

## 10. Milestones

`SIMD-L<n>` are this note's milestones; `SIMD-<n>` are
[`proposals/SIMD.md`](proposals/SIMD.md) §7's, and `S<n>` are its §5 gaps. The
two lists line up: L1–L7 are the *how* for SIMD-0…SIMD-6. `SIMD-8` (native
lowering in a future backend) deliberately has no `L` counterpart — that path
belongs to the backend that has the vector registers, not to this LibJIT-only
note (§6.5).

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SIMD-L0 — This note** | Write this decision record and the consistency edits to `SIMD_BACKENDS.md`, `proposals/SIMD.md`, `README.md`, `TESTING.md`. No code. | — (the gate) | The five documents agree on the direction, the ABI, the library and the test plan. |
| **SIMD-L1 — Scalar reference + ABI** | ✅ **Done.** `wasm_simd_scalar.{hpp,cpp}` (the reference for every L1 helper, plus the `extern "C"` entry points); `jit_type_set_size_and_alignment(v128, 16, 16)`; `gcStorageAlign` → 16; the two 16-byte zero-value paths corrected; `callSimdHelper`/`callSimdOutHelper` + signature cache; 21 ops wired (const, 6 splats, 8 extracts, 6 replaces); `simd_abi.wat` + `simd_const_lane.wat`; 5 T1/T2 cases. | S4, S5, S9 | ✅ `(param v128)(result v128)` round-trips through the JIT (measured); `v128.const` → `splat` → extract/replace matches the golden vectors; suite 91/1755, zero regressions. |
| **SIMD-L2 — Highway layer + memory/bitwise/lane** | ✅ **Done** except the peepholes (see below). The multi-target TU + the qmake lines (§5.5) landed as `wasm_simd_vector.*` (§6.5); `*.splat` moved to a real vector broadcast; all 22 `v128.load*`/`store*` ops, the 6 bitwise ops, `shuffle` and `swizzle` are implemented and tested. **Deferred: the D5 peepholes** (`load`+op, `const`+op fusion). | S1 (part) | ✅ `simd_load_store.wat` and `simd_shuffle.wat` green; T1 green for the whole L2 op set; `libWasmBase` links `libhwy.so.1` and the entry points dispatch at run time. |
| ✅ **SIMD-L3 — Integer and comparison ops** | **Done — all 133 ops.** The X-macro op list (§6.5); all 36 comparisons; the 23 `add`/`sub`/`mul`/`min`/`max` forms; the 8 `neg`/`abs` forms; the 12 shifts; saturating `add`/`sub` (8); `avgr_u` (2); `popcnt` (1); the reductions `all_true`/`bitmask`/`any_true` (9); and the width-changing family `narrow`/`extend`/`extmul`/`extadd_pairwise`/`dot`/`q15mulr_sat_s` (34). Each is differentially tested against the reference by construction, and the corners a differential cannot see are pinned by golden vectors (three of which corrected *the expectation*, never the code — §11 R8). | S1 | ✅ `simd_arith_i.wat` green (67 exports); T1 green for the whole op list; **184 of 236** handlers execute, and every remaining stub is a float op. |
| ✅ **SIMD-L4 — Float and conversion ops** | **Done — all 52 ops.** The 12 float comparisons (with the unordered `ne` the library lacks), the 14 unary float ops (`ceil`/`floor`/`trunc`/`nearest` — ties to *even* — plus `abs`/`neg`/`sqrt`), the 16 binary ones (`add`/`sub`/`mul`/`div` and the four min/max flavours with wasm's NaN and `±0` rules — hazard Y1), and the 10 conversions (4 `trunc_sat`, 2 `convert`, 2 `convert_low`, `demote`/`promote`). Eight of the conversions are vectorised; two are marked `FORWARD` in the op list, because the library has no truncating float64→int32 conversion at all (§11 R10). | S1 | ✅ `simd_arith_f.wat` and `simd_convert.wat` green (51 exports between them, all V8-checked); T1 green for every float op; **236 of 236 handlers execute**. |
| ✅ **SIMD-L5 — Globals and constant expressions** | ✅ **Done.** `v128.const` decodes in `evalConstantExpr` into the `Value`'s 16 bytes (little-endian, hazard Y11); `dispatchGlobalGet`/`Set` reuse `wasm_v128_load`/`store` on the global's `Value`, whose `v128` member is at offset 0 — so no new LibJIT mechanic was needed, and `globalSlotAddress` replaced the three copies of the address arithmetic that `global.get`/`set` and the GC barrier path each had. | S3 | ✅ `simd_globals.wat` covers immutable and mutable globals, a `v128` through a local, and read-modify-write across calls; the eight previously-blocked spec scripts pass. |
| ✅ **SIMD-L6 - Relaxed SIMD** | ✅ **All 20 sub-opcodes implemented.** Nine are strict-helper reuse (SD5) and ride the op-list differential; the other eleven are hand-written entry points — `relaxed_swizzle` and the four `relaxed_laneselect` delegate to the strict entry points (zeroing swizzle, `v128.bitselect`), the two dots have per-target bodies (the hardware `maddubs` form), and the four `madd`/`nmadd` **forward to the reference** because the vector form was contracted into an FMA at `-O2` while the reference was not — a debug/release divergence caught by §8.4's release build. | S2 | ✅ Relaxed modules decode and run; T1 covers all 20 (op list + the `simd relaxed` table) and T2 covers the hand-written shapes through `simd_relaxed.wat`. |
| ✅ **SIMD-L7 — Harness marshalling** | ✅ **Done for every signature the engine compiles.** `WastScript.cpp` decodes `v128` arguments lane by lane (integer lanes as signed decimals, float lanes as bit patterns) and compares results lane-wise with the per-lane `nan:canonical`/`nan:arithmetic` policies. The ≥ 3-operand limit is worked around rather than accepted: a signature of one-to-four `v128` parameters — with no result, a `v128` result, or one scalar result — is called through the host C ABI instead of `jit_apply` (§6.6), which is the ABI the generated code was written for. All 66 `simd_*` / relaxed scripts are curated with `failed=0`. | S7, S8 | ✅ `./WasmJit 'spec: *'` reports real counts; the 66 curated scripts run in the default suite and none abort. |
| ✅ **SIMD-L8 — Benchmark and the D4 decision** | ✅ **Measured; D4 justified but deliberately not built yet.** `[simdbench]` reports **M1 = 4.10 ns per `v128` op** against a no-call bound of 0.099 (42×), with a single helper call costing 4.02–4.30 ns — so 97.6 % of the loop is the call, not the SIMD work, and the JIT is at parity with native C doing the same thing (4.10 vs 4.71). §8.3's gate therefore holds for any peephole outcome, which makes D4's justification independent of M5; its verdict is deferred in favour of the cheaper peephole, which the same numbers say is worth up to 2× on memory-fed loops. M2 (by-value helpers) and M5 are unmeasured because they are unbuilt, and §8.4 says so explicitly. | — | ✅ The note states the measured `ns/v128-op` per variant, revises §8.1's cost model to match, and records the D4 verdict and the order of the next two steps. |

**Why the D5 peepholes were deferred rather than built in L2.** Fusing
`v128.load`+op or `v128.const`+op saves one slot round-trip, but it needs
*lookahead*: the dispatcher is a single pass over the bytecode that emits each
instruction as it decodes it, so fusion means buffering a decoded instruction
and reconstructing its operands — real complexity in the hottest and most
error-prone part of the backend. §8 gates that peephole on the benchmark showing
the boundary cost dominating. **That benchmark now exists (SIMD-L8) and the
answer is unambiguous**: the crossing is ~12 cycles per op, the arithmetic ~1,
so a peephole is worth building — and worth ~2× here, since it removes half the
calls of a memory-fed loop. The *reason* it pays also changed: it was deferred to
avoid a slot copy, and the measurement says the slot copy was never the cost.

Guardrails: the scalar reference stays compiled and green at every step; run
`./WasmJit '~[spec]'` after each milestone; never let a real fix regress the
curated spec set.

## 11. Risks and open questions

| # | Risk / question | Status |
|---|---|---|
| R1 | **Frame growth** on SIMD-dense functions (16 bytes per static `v128` value). | Accepted for L1–L7; mitigate with a height-keyed shared bank or slot reuse only if a real module hits it (§6.2). |
| R2 | **Highway compile time** from per-target re-inclusion (SSE2…AVX3 + EMU128 + SCALAR). | Observed at SIMD-L2: the vector TU is compiled once per enabled target and a full build stays well under a minute on this machine. `HWY_COMPILE_ONLY_*` remains the knob if that changes. |
| R3 | **`f32x4.min/max` NaN semantics** (Y1) are the likeliest silent conformance failure. | Mitigated by T1's unordered-mask cases and T2's `simd_arith_f.wat`; keep both non-negotiable. |
| R4 | **`swizzle` sanity** (Y3) — indices 16…127 must zero, not alias. | Explicit T1 case with indices `16`, `127`, `128`, `255`. |
| R5 | **`jit_insn_store_relative` silently does not write a `jit_value_create`d aggregate.** Found and fixed during SIMD-L1: `v128.const` and both v128 zero-value paths used it and produced uninitialised slots (§6.6). | Fixed (SIMD-L1); the rule "write a struct-typed value through its address" is now in the header comment and enforced by the §6.6 table. |
| R6 | **Hand-computed golden vectors are error-prone.** During SIMD-L2 the differential layer passed on the first run both times, while two *test expectations* I derived by hand were wrong (`load8x8_s` lane 7, and `load16_lane`'s partial-lane overwrite). | Recorded as the lesson it is: the differential proves the implementation agrees with the reference, and says nothing about a hand-derived expected value. Golden vectors must come from the spec, and a wrong expectation gets fixed in the test — never in the code. |
| R7 | **Hazard Y3 is simpler than §7 first described.** The original rule was "saturate the index, *or* handle pshufb's bit-7 rule". As built, one range select covers both: indices 128…255 are `≥ 16` too, so the same mask that fixes 16…127 zeroes them — the target's bit-7 behaviour never has to be reproduced *or* depended on, which also removes an SSE2-vs-SSSE3 difference. | Implemented (SIMD-L2); §7's Y3 row updated. The select uses the **unsigned** minimum, which the note calls out because the signed one would classify 128…255 as in-range. |
| R8 | **The hand-computed expectations were wrong twice more — and the library disagrees with itself.** Writing SIMD-L3's goldens, two expectations were wrong (the `q15mulr_sat_s` `min×max` corner, and a chained example where I took `sub_sat_s(100, 100)` to be 127); the implementation was right both times. That is the third and fourth occurrence of R6, and the second was caught *only* because of the change below. Separately, the differential's blind spot is now concrete rather than theoretical: hazard Y5 is invisible to it, because both layers wrap the same way, and the random inputs never produced the one pair that would have exposed it. | Adopted: **a golden is cross-checked against an independent engine before it is written down**, not after a failure. `node` (V8) is installed and implements the shipped spec; it settled `q15mulr_sat_s` after a `wat2wasm`, and it checked all 67 expectations of `simd_arith_i.wat` in one run. `wasm-interp`, `spectest-interp` and `wasmer` are installed as alternatives. The gap that leaves is named rather than papered over: an engine check covers a module's *scalar* semantics, so it validates what a golden asserts but not the JIT path that produced it — that is what T2 is for. |
| R9 | **A periodic test input hid a whole class of bug, and the library's float `Ne` is the wrong comparison.** Two findings from SIMD-L4 that only the *input design* could have produced. (1) The byte patterns the differential fed were periodic every 8 bytes, which makes a vector's two halves identical — so the reference's `extend_high`/`extmul_high` read the *lower* half while the vector read the upper one, and 184 passing assertions said nothing about it. The float corners (added for the float ops) broke the periodicity and it failed immediately; the two forms are now separate shapes, so the reference cannot read the wrong half at all. (2) Highway's float `Ne` is `_CMP_NEQ_OQ` — the **ordered** not-equal, false when an operand is NaN — while wasm's `ne` is unordered and true. A library that is right about `eq`/`lt`/`gt`/`le`/`ge` and wrong about `ne` is exactly the shape a "trust the library" rule gets wrong. | (1) The input families are now deliberately *non*-periodic, and §9's T1 note says why: an input that cannot distinguish the halves cannot test them. (2) `wasmFNe` implements `Not(Eq(a, b))`; §7's Y1 row records it, and the golden pins it (`cmp_ne_nan`). The general lesson is the one R6 keeps teaching from a different angle: a test that agrees with the implementation is not evidence until you know what it would have taken to disagree. |
| R10 | **The library has no truncating float64→int32 conversion, and its saturation is exact where it exists.** Two facts about mapping wasm's conversions onto Highway, both found by trying to write the mapping. (1) There is no `DemoteTo`/`ConvertTo`/`TruncateTo` form for f64→i32 at all — its conversions are same-width (`ConvertTo`: i32↔f32, i64↔f64), same-kind (`PromoteTo`/`DemoteTo`) or *rounding* (`NearestInt`). `i32x4.trunc_sat_f64x2_*_zero` therefore has no vector path to map onto, so those two ops are delegated to the reference and marked `FORWARD` in the list (§6.5). (2) For the eight conversions that *do* map, the library's float→int `ConvertTo` is documented to saturate and its ends are exact — which the differential test then checked against my clamping reference at ±2^31, ±2^32 and the largest representable value below each, rather than my taking it on faith. | (1) The `FORWARD` shape, and a note in the list beside the two ops. (2) No change was needed — but the *claim* is now tested, and `simd_convert.wat` pins the same ends through the JIT with values read off V8 (`trunc_sat_u_near` = `0xffffff00` is the one that would show an off-by-one). |
| Q1 | Does LibJIT's 16-byte aggregate ABI (S9) work *and* beat the pointer ABI? | **Correctness answered (SIMD-L1): the ABI works** — `(param v128)(result v128)` round-trips through the JIT and the C++ mirror `{uint32_t×4}` matches it (§6.6). *Beating* is still unmeasured: M2 needs by-value signatures plumbed through the helper layer, which is a change to §6.3 rather than a probe (§8.4). The headroom it competes for is bounded by the measured crossing — 97.6 % of a 4.10 ns op is the call, so a by-value ABI can only win by eating into that, and the cheaper way to eat into it is to remove calls (peepholes, then D4). |
| Q2 | How is a `v128` value printed on a test failure? | Lane-wise hex with lane indices, defined at L1 (T1 diagnostics and T3 output both use it). |
| Q3 | Do `v128` globals live as 16 inline bytes in the VM context? | Confirm at L5 against `WasmModuleInstance`'s globals layout and `storageAlign`. |
| Q4 | Does the D1 inline set grow beyond const/load/store/splat/extract/replace? | **No — answered by M3 at L8.** Lane-wise scalar IR for one per-lane op is 3.35 ns against the helper's 4.10, but the 18 % comes from issuing a *direct* call instead of a dispatched one, not from the IR: the IR does four loads, four adds and four stores where the helper does one `paddd`. Growing the D1 set would pay the IR-size and compile-time cost §5 D1 warns about to buy a difference that a call-mechanics fix would give for free. |
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

