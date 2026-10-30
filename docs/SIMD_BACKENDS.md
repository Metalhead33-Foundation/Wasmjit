# SIMD Lowering Across Backends — Research Note: LibJIT, SLJIT, AsmJit, and the Native-Helper Middle Path {#doc_simd_backends}

Status: research note (revision 3 — records that the current implementation path
is LibJIT only while AsmJit and SLJIT remain deferred-but-planned; the decision
that follows from this survey now lives in [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md)).
Audience: anyone about to start a milestone from
[`proposals/SIMD.md`](proposals/SIMD.md) §7, or anyone touching the backend
abstraction (`Project.pro`, the `*OpcodeDispatcher` families, the type
translators).

This note exists to answer one question *before* any SIMD code is written: **can
the backends we have lower `v128`, and if not, what is the cheapest conforming
way to get there?** It is the companion to
[`proposals/SIMD.md`](proposals/SIMD.md) — that note says *what the spec
demands and where we may deviate*; this one says *what the engines in and around
the tree can actually do about it*. It contains **no new code** and proposes no
milestones of its own; its output is a decision on the lowering strategy and a
laxness budget.

**Scope (revision 3).** The current implementation path is **GNU LibJIT only**,
but AsmJit and SLJIT are **deferred, not cancelled**: both remain planned
backends and both are expected to be leveraged for SIMD once implemented
([`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §3). §3.2/§3.3 and the third and fourth rows
of §2 are therefore *forward-looking* background, not history. The decision
LibJIT feeds into is recorded in [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md), which also
supersedes §5/W2 (the `alloca` scratch bank) and §6 (the recommendation ranking)
below.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Short answer: do LibJIT / SLJIT / AsmJit support SIMD? |
| §3 | What each engine actually offers (evidence) |
| §4 | What "SIMD support" would have to mean per backend |
| §5 | The workarounds when a backend has no SIMD (LibJIT) |
| §6 | The path of least resistance |
| §7 | How much can we play fast and loose? |
| §8 | What this implies for `proposals/SIMD.md` |
| §9 | Open questions to settle when the code starts |
| §10 | References |

## 2. Short answer

| Backend | `v128` as a value type | Vector registers / instructions | Verdict |
|---|---|---|---|
| **GNU LibJIT** (installed, primary) | ✗ none — no vector type exists anywhere in the API | ✗ none — no packed/SSE/AVX opcode exists | **No SIMD. The only way is to synthesise `v128` out of scalar primitives and lower every op to a call.** |
| **SLJIT** | ✗ (it has no typed-value model at all) | ✓ full — `SLJIT_SIMD_REG_128`/`SLJIT_SIMD_REG_256` widths, `SLJIT_VRn` registers, `sljit_emit_simd_*` | **Yes, at the register/instruction level** — but it is not wired into this repo and would need the whole value/operand model on top. |
| **AsmJit** | ✗ (assembler/IR + register allocation, not a value model) | ✓ complete — MMX/SSE…SSE4/AVX/AVX-512 (`Vec` = Xmm/Ymm/Zmm, `KReg`, `Tmm`), AArch64 NEON | **Yes, at the instruction level** — same caveat, plus the backend does not exist yet. |

Two consequences fall out immediately:

1. **No available backend can be handed a `v128` and asked for a vector op.**
   LibJIT is the only backend that is actually built and linked, and it has
   *zero* vector surface. So the `v128` representation and the per-op lowering
   are ours to design at the WasmJit layer, whatever engine is underneath.
2. **"Add SIMD support to the backend" is the wrong framing for the near term.**
   The right framing is "define a `v128` value/ABI and a lowering that works on
   LibJIT today and survives a move to SLJIT/AsmJit later." That is exactly what
   [`proposals/SIMD.md`](proposals/SIMD.md) SD1/SD2/SD7 already assume.

**Recommendation (detail in §6) — adopted and shipped:** on LibJIT, lower SIMD
to native helper functions over a 16-byte payload, keep the
`{i32,i32,i32,i32}` representation, and treat hardware vector fast paths as an
optional later optimisation. It is what [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md)
records as the decision.
Helpers are conformant, backend-neutral, and cost no LibJIT features that do not
exist. They are the permanent *baseline* and the test oracle — and, for a backend
that has real vector registers, a fallback rather than the only path (§6, §8).

## 3. What each engine actually offers

### 3.1 GNU LibJIT — no SIMD, and no hook to add it

Evidence, read from the installed headers on this machine
(`/usr/local/include/jit/`, 28 headers, built 2026-02-04):

- **Type kinds are scalar-only.** `jit-type.h` defines primitives
  (`jit_type_void` … `jit_type_nfloat`, `jit_type_void_ptr`) and the composite
  constructors `jit_type_create_struct`, `jit_type_create_union`,
  `jit_type_create_signature`, `jit_type_create_pointer`,
  `jit_type_create_tagged`. `JIT_TYPE_STRUCT`, `JIT_TYPE_UNION`,
  `JIT_TYPE_SIGNATURE`, `JIT_TYPE_PTR` and `JIT_TYPE_FIRST_TAGGED` are the only
  non-primitive kinds — **there is no `JIT_TYPE_VECTOR` and no packed type.**
- **The opcode table is scalar-only.** `jit-opcode.h` defines **438**
  `JIT_OP_*` opcodes, ending at `JIT_OP_LOAD_RELATIVE_STRUCT` /
  `STORE_RELATIVE_STRUCT` / `JIT_OP_MEMCPY` / `JIT_OP_ALLOCA` /
  `JIT_OP_JUMP_TABLE`. The widest shapes are `..._LONG`, `..._FLOAT64` and
  `..._STRUCT`; **no opcode mentions SIMD, packed, MMX, SSE or AVX.**
- A `grep -w -i` for `simd|vector|packed|xmm|ymm|zmm|mmx|sse|avx|neon` across
  *all* installed LibJIT headers returns **nothing** (the only hits from a naive
  grep are the letters `sse` inside the LGPL boilerplate word "Lesser"). This is
  not "SIMD is undocumented" — SIMD is absent from the interface.
- What *does* exist is enough to build a lowering by hand:
  - `jit_type_create_struct` / `jit_type_set_size_and_alignment`
    (a `v128` can be a 16-byte, 16-byte-aligned synthetic type);
  - `jit_insn_load_relative` / `jit_insn_store_relative` (lane access into a
    struct/temp), `jit_insn_load_elem` / `jit_insn_store_elem` (memory lanes);
  - `jit_insn_alloca`, and `JIT_OP_MEMCPY` for 16-byte moves;
  - `jit_insn_call_native` — the existing escape hatch this codebase already
    uses for GC and bulk-memory helpers;
  - `jit_value_create` (aggregates) and the four scalar constant constructors.

So LibJIT can express the *storage* and *control flow* of `v128` but not a
single vector arithmetic op. This is precisely the case
[`proposals/SIMD.md`](proposals/SIMD.md) §3 anticipates: "no hardware SIMD is
required … a 16-byte struct in memory is a valid representation."

### 3.2 SLJIT — real SIMD, but a register-level API that is not wired up

SLJIT's own README lists "SIMD" as a first-class feature (alongside tail calls,
unaligned memory access and atomics) and supports x86 32/64, ARM 32/64, RISC-V,
s390x, PowerPC, LoongArch and MIPS. The API surface, read from
`sljit_src/sljitLir.h` and the upstream SIMD test suite
(`test_src/sljitTestSimd.h`):

- **Register types:** `SLJIT_SIMD_REG_128` and `SLJIT_SIMD_REG_256` (the widths
  exercised by the upstream test suite), combined with element types
  `SLJIT_SIMD_ELEM_8/16/32/64` and the `SLJIT_SIMD_FLOAT` flag. Vector registers
  are `SLJIT_VR0…`, saved vector registers are `SLJIT_VS0…`, and
  `SLJIT_TMP_DEST_VREG` is the scratch vector.
- **Data movement:** `sljit_emit_simd_mov(...)`, with `SLJIT_SIMD_STORE`
  selecting the store direction and the `SLJIT_SIMD_REG_*` width argument;
  `sljit_emit_simd_replicate(...)` for broadcasts; lane moves and per-lane
  memory access exist in the same family.
- **Arithmetic/logic:** `sljit_emit_simd_op1(...)` and
  `sljit_emit_simd_op2(...)` (the latter changed shape as recently as
  2024-06-10 per `API_CHANGES`), plus sign/extend/type-conversion helpers.
- **Lifecycle:** vector registers must be declared at function entry
  (`sljit_emit_enter(..., SLJIT_ENTER_VECTOR(n), ...)`), and
  `SLJIT_NUMBER_OF_SAVED_VECTOR_REGISTERS` / `sljit_get_register_index(
  SLJIT_SIMD_REG_128, SLJIT_VR0)` let a backend query what the host provides.
  x86 additionally exposes a `SLJIT_ENTER_USE_VEX` mode.

**But**: SLJIT is a *register allocator + instruction emitter*, not a compiler
with values. There is no type system, no automatic allocation for vector values,
no struct-by-value call ABI, and no portable "opaque 16-byte value". Adopting it
means writing a whole new operand model, calling convention and register
allocator at our layer (the same work the current LibJIT backend does for
scalars, redone for vectors).

**State of `Sljit/` (2026-10-30):** the toolkit is now vendored as
`extern/sljit` and a **provisional** backend compiles it into `libSljit.so`
(registered as `sljit`, with `[sljit]` coverage). That backend lowers a
straight-line integer subset only — no `v128`, no calls, no control flow, no
memory access — so this note's SIMD conclusions are unchanged: the shared helper
layer is still the portable baseline, and a *native* `v128` lowering on SLJIT
remains future work ([`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §9 lists the
exact subset).

| Fact | Evidence |
|---|---|
| `Sljit/` contains four source units and `Sljit.pro` compiles `extern/sljit/sljit_src/sljitLir.c` into the library (no external `-lsljit`) | `Sljit/Sljit.pro` |
| The backend is selectable: `Runtime::create("sljit")` and `WASMJIT_BACKEND=sljit` | `Test/SljitBackendTests.cpp` (`[sljit]`) |
| It executes `i32`/`i64` straight-line bodies and refuses everything else with a null `Callable::fnPtr` | `Sljit/SljitOpcodeDispatcher.cpp`, `Sljit/README.md` |

### 3.3 AsmJit — real SIMD, complete instruction coverage, no backend yet

AsmJit is "a library for low-latency machine code generation" and is explicitly
*not* a value model. What it has, verified from the repository:

- **x86 backend:** instructions carry their ISA requirement in the generated
  tables — e.g. `kIdPshufb //!< Instruction 'pshufb' {SSSE3}`,
  `kIdPmulld {SSE4_1}`, `kIdPsllq {MMX|SSE2}`, `kIdPmovsxbw {SSE4_1}`
  (`asmjit/x86/x86_globals.h`). That is MMX, SSE, SSE2, SSE3, SSSE3, SSE4,
  AVX/AVX2 and AVX-512 coverage.
- **Vector register classes:** `asmjit/x86/x86_operand.h` declares `Vec` (the
  Xmm/Ymm/Zmm family), `Mm` (MMX), `KReg` (AVX-512 masks) and `Tmm` (AMX tiles),
  including vector-index (VSIB) addressing (`Mem(base, const Vec& index, ...)`).
- **Register allocation for vector registers exists:**
  `asmjit/x86/x86_ra_pass.cpp` is the x86 register-allocation pass of the
  `Compiler` layer; the `Builder` emits IR and `Compiler` allocates, so vector
  values can in principle live in `xmm/ymm/zmm` instead of being spilled.
- **AArch64 NEON** is covered by the `arm`/`a64` backends.

Same caveats as SLJIT, more strongly: AsmJit is an *assembler and IR allocator*
with no `v128` analogue, no wasm-aware calling convention, and no
`OpcodeDispatcher`. Adopting it means a new backend of the same size as the
LibJIT one, plus whatever the vector lowering needs. And `Asmjit/` is still the same empty shell:

| Fact | Evidence |
|---|---|
| `Asmjit/Asmjit.pro` is `TEMPLATE = lib` with no `SOURCES` and no `LIBS += -lasmjit` | `Asmjit/Asmjit.pro` |
| `build/Desktop-Debug/Asmjit/libAsmjit.so.1.0.0` is 14 528 bytes, no exported API | build tree |
| No AsmJit headers or library are installed | no `asmjit.h` / `asmjit/core.h` under `/usr/include`, `/usr/local/include` |

## 4. What "SIMD support" would have to mean per backend

The word "SIMD" hides at least six different capabilities. A backend that has
one need not have the others, and *none* of our candidate backends has the ones
that matter at the WasmJit layer.

| Capability | LibJIT | SLJIT | AsmJit | WasmJit must supply |
|---|---|---|---|---|
| **C1** A 16-byte value type with a defined memory layout | ✗ (synthetic struct only) | ✗ (no types) | ✗ (no types) | **yes** — `WasmType`/`WasmValue` already do |
| **C2** Vector registers and per-op instructions | ✗ | ✓ | ✓ | — |
| **C3** A portable `v128` calling convention (params, returns, spilling) | ◑ (aggregate ABI only, unverified at 16 bytes) | ✗ (write your own) | ✗ (write your own) | **yes** (SIMD-0 / S9) |
| **C4** Lane extraction/insertion and lane-typed memory access | ◑ (relative + element load/store) | ✓ (lane ops) | ✓ (full ISA) | **yes** — as helper calls on LibJIT |
| **C5** A lowering for 236 fixed-width + 20 relaxed ops | ✗ | ✗ (only primitives) | ✗ (only primitives) | **yes** — the bulk of the work |
| **C6** Runtime CPU feature detection + fast/portable dispatch | ✗ | ✗ | ✗ (traits query only) | **yes** (optional, §5/W5) |

Reading the table: C1/C3/C5/C6 are WasmJit-layer work by necessity. C2 is the
only capability LibJIT lacks *and* SLJIT/AsmJit have. That is why switching
backend does not make SIMD cheap *today* — neither of those backends exists, and
C1/C3/C5 are ours either way — while switching *strategy* (helpers) does. It is
also why that switch pays later: C2 is register residency and per-op
instructions, which a helper call cannot provide on *any* backend, and a backend
that has it is expected to use it instead of, or in addition to, the helpers
(§6, and the SIMD contract row in [`LAYERING.md`](LAYERING.md) §4).

## 5. The workarounds when a backend has no SIMD (LibJIT)

Six options, in increasing order of payoff and of cost. All are conformant under
[`proposals/SIMD.md`](proposals/SIMD.md) §3.

| # | Workaround | Mechanism | Cost | Limits |
|---|---|---|---|---|
| **W1** | **One native helper per op.** `v128` stays the `{i32×4}` struct; each `SIMDOpcode` lowers to `jit_insn_call_native` against a C helper that takes/returns the payload (by value or via pointer). | The pattern already used for GC and bulk memory (`wasm_table_fill_impl`, …). | One helper per op (~256), plus marshalling per call. | A call and memory round-trips per op; no register residency. |
| **W2** | **Memory staging.** Keep `v128` operands in a scratch area of the wasm frame and pass a pointer; the helper reads/writes 16 bytes in place. | `jit_insn_alloca` / a frame slot + `jit_insn_address_of`. | Sidesteps the unverified 16-byte aggregate ABI (S9). | An extra store/load per op; the scratch area must be tracked per live value. |
| **W3** | **Force the synthetic type's size/alignment.** Create the struct, then `jit_type_set_size_and_alignment(v128, 16, 16)`. | One line in `LibJitTypeTranslation.cpp`. | Resolves the alignment half of S4 (JIT 4-aligned vs `Value alignas(16)`). | Needs a matching rule in the GC struct layout, or S4 returns. |
| **W4** | **Scalar lane splitting in JIT IR.** Load/store each `i32` lane with `jit_insn_load_relative`/`store_relative` and use existing scalar `jit_insn_*` arithmetic per lane. | No helpers, no ABI change. | IR size and compile time grow with the lane count. | Only works for genuine per-lane ops. Shuffle, swizzle, `bitmask`, `all_true`/`any_true`, `pmin/pmax`, `dot`, conversions and reductions have no per-lane decomposition without cross-lane traffic. |
| **W5** | **Hybrid helper library.** Implement the helpers in C; give the hot ones SSE2/AVX2 bodies and pick at run time with `__builtin_cpu_supports`. | The helpers stay ordinary C functions; no JIT-level SIMD needed. | One branch per call, or a function-pointer table initialised once. | Only helps ops that dominate a real workload; still a call per op. |
| **W6** | **Drop execution; implement detection/validation only.** | — | — | Not a wasm engine. Rejected; recorded only so it is not rediscovered. |

Two notes that matter for correctness rather than speed:

- **W4 is the tempting trap.** "Just split into four `i32`s" appears to avoid
  helpers entirely, but the moment an op needs *cross-lane* semantics
  (`i8x16.shuffle`/`swizzle`, `i32x4.bitmask`, `v128.any_true`, `pmin/pmax`,
  `dot_i16x8_s`, all the widen/narrow/extmul forms) the JIT has no way to express
  it, and the fallback is a helper anyway. W1/W2 + W5 dominates W4; W4's only
  real use is as an *inline* form of the trivial per-lane cases if a
  microbenchmark ever justifies it.
- **W3 was not optional once `v128` crosses a call by value — and it is done.**
  `WASM::Value` is `alignas(16)` and the lowered type started out 4-aligned; the
  two are now reconciled at 16 (`jit_type_set_size_and_alignment(v128, 16, 16)`
  plus `gcStorageAlign` → 16, SIMD-L1), which is what makes "`v128` round-trips
  through a function" (SIMD-0's done-when) provable rather than assumed.

## 6. The path of least resistance

> **Superseded (revision 2).** The decision is now recorded in
> [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §4–§6. This section is kept as the reasoning
> that led to it — history, not plan.

**Recommendation: W1 (+ W3), with W5 as the only permitted optimisation — keep
the current `{i32×4}` representation, lower every SIMD op to a native helper
call, and put hardware fast paths inside the helpers behind runtime feature
detection.** Concretely:

| Rank | Path | Why it is the least resistance |
|---|---|---|
| **1** | **W1 helpers on LibJIT** | The only backend that exists is LibJIT; the helper mechanism already exists and is used by two other subsystems; it needs **zero** new LibJIT features; it is exactly SIMD.md SD1. |
| **2** | **W3 alignment fix** (fold into SIMD-0) | Small, removes a latent ABI trap, and is a prerequisite for by-value `v128`. |
| **3** | **W5 hardware fast paths inside the helpers** | Keeps the portable helper as the reference and the fallback; no change to the JIT, the opcode decoder or the ABI; satisfies SIMD.md SD2 / SIMD-7. |
| — | *(deferred)* W2 memory staging as the helper ABI | Adopt only if the 16-byte by-value aggregate ABI proves unreliable (S9); it changes the *helper signatures*, not the plan. |
| — | *(deferred)* Real vector lowering on SLJIT or AsmJit | The correct long-term answer for performance, but blocked on those backends existing at all — and the C3/C5 work that helpers do is a prerequisite for it, not wasted by it. |

Why the helper layer is not a stopgap — and not the only lowering:

1. **They are backend-neutral, and they are not exclusive.** A helper table is a
   set of `extern "C"` C functions. SLJIT and AsmJit can call them the same way
   LibJIT does with `jit_insn_call_native`, so any future backend gets a
   *correct* SIMD path for free. What a helper call cannot give any backend is
   what a backend with vector registers has: `v128` values living in those
   registers, and per-op instructions instead of a call. That path is expected
   (`SIMD_LIBJIT.md` §3; the contract row in [`LAYERING.md`](LAYERING.md) §4 —
   "or provide its own but keep the same semantics"), and it reuses everything
   that is expensive to build: the semantics layer, the `v128` ABI, the 236-op
   mapping and this test oracle. What must not be built twice is a *second
   semantics layer* — which is why none of those four is optional.
2. **They separate the two hard parts.** The `v128` ABI (C3) and the ~256
   opcode→semantics mapping (C5) are WasmJit-layer problems under *any*
   strategy; only the *quality* of the generated code is backend-specific. Doing
   the cheap strategy first buys conformance and test coverage — which is where
   every current SIMD gap lives (§8) — before spending anything on code quality.
3. **They fit the safety posture.** Helpers are ordinary library code, so
   [`SAFETY.md`](SAFETY.md) applies directly: input-derived bounds, trap-vs-`abort`
   and the allocation budget are all visible in C we control, not buried in an
   opaque JIT. A `v128.load` helper is a 16-byte move wrapped in the *existing*
   memory-bounds check — the model the bulk-memory work already validated.

Effort, as a sanity check (order of magnitude, not an estimate):

| Work item | Shape | Notes |
|---|---|---|
| `v128` ABI decision + alignment (SIMD-0) | small | one type rule, one manual test |
| helper library skeleton + call plumbing | small | mirrors the existing helper call sites |
| memory + bitwise + splat/lane/shuffle (~40 ops) | medium | mechanical; helpers are short |
| integer ops (~90 ops) | medium–large | many are `min/max/avgr/sat` one-liners |
| float ops + conversions (~90 ops) | large | the real work is rounding/NaN/conversion corners |
| relaxed (20 ops) | small | scalar results are permissive (SD5) |
| harness marshalling (SIMD-6) | medium | blocks measurement, not execution |
| hardware fast paths (SIMD-7) | optional | inside the helpers, never behind the JIT |

**The one thing to avoid** is letting the *helper-vs-intrinsics* choice leak
into the opcode decoder, the type translator or the operand stack. The decoder
(236 `SIMDOpcode` entries, immediates already correct) and the type/value layer
are done and strategy-independent; the strategy belongs entirely in
`LibJIT::OpcodeDispatcher` plus a new helper translation unit.

## 7. How much can we play fast and loose?

The user-facing question is whether the proposal can be "played fast and loose"
versus strictly conformed. The honest decomposition is that **looseness is free
in exactly four places and fatal in one**, and the place where it is fatal is
where most of the ops live.

### 7.1 A laxness budget, by layer

| Level | What it covers | How loose can we be? | Authority |
|---|---|---|---|
| **L0 — fixed-width lane semantics** | the *bits produced* by the ~236 fixed-width ops: lane numbering, shuffle/swizzle mapping, `trunc_sat` vs `trunc_trap`, NaN/zero handling of `min/max`, `roundTiesToEven`, `bitmask`/`all_true` derivation | **Not loose at all.** Every one of these is fixed by the spec, and the spec test suite checks them bit-for-bit. | [`proposals/SIMD.md`](proposals/SIMD.md) §2.1–§2.3 |
| **L1 — relaxed semantics** | the 20 relaxed ops | **Maximally loose.** Any result from the allowed set is conformant; the deterministic profile is optional. | §2.4 / SD5 / SD6 |
| **L2 — representation** | how a `v128` is stored: 4×`u32`, a `__m128`-shaped buffer, 2×`u64`, alignment, spilling, ABI | **Free, as long as the byte order is right.** The spec fixes the bit layout (bit 0 = LSB of byte 0, little-endian) and nothing else. | §3 bullets 2, 5; SD7 |
| **L3 — code quality** | helpers vs intrinsics, vectorisation of adjacent ops, register residency, `i8x16.shuffle` lowering (helper loop vs `pshufb`) | **Completely free.** The spec "fixes results, never machine instructions"; no auto-vectorisation is required. | §3 bullets 1, 3, 6, 7; SD1, SD2, SD4 |
| **L4 — validation** | lane immediates (0–31, 0–15, …), the `align ≤ natural` memarg rule, shuffle indices | **Not negotiable, and already enforced:** `Module::validate()` rejects an out-of-range lane index (`Validator::requireLane`), a shuffle index beyond the vector, and `alignment > natural` ([`VALIDATION.md`](VALIDATION.md), SIMD row). An out-of-range lane immediate used as-is would be silent out-of-bounds lane access, unlike an unsupported opcode, which fails loudly. | §2.2; §7 guardrails |

So the practical rule is: **be maximally loose at L1–L3, be strict at L0, and
be *deliberately* strict at L4 for lane immediates** — the cost is a range check
per SIMD instruction at validation time, and the alternative failure mode is a
memory-safety bug rather than a
wrong number.

### 7.2 What we may skip or defer (and what it buys)

| # | We may… | Because | Buys |
|---|---|---|---|
| F1 | Lower every op to a scalar helper | §3: no hardware SIMD required; per-lane scalar code or even a helper call is conformant | A portable implementation; no ISA matrix |
| F2 | Never claim or exploit register residency | §3: no register-allocation or vectorisation contract | No new ABI or liveness machinery |
| F3 | Use unaligned 16-byte moves everywhere | §3: unaligned access is legal when `align < natural`; x86 tolerates it | One `memcpy`-shaped helper per access; no alignment pathologies |
| F4 | ~~Keep the 4-aligned `{i32×4}` representation~~ **Not taken:** the shape `{i32×4}` stayed, but its alignment is forced to 16 (W3/SIMD-L1), because a `v128` that crosses a call boundary by value must match `WASM::Value`'s `alignas(16)`. | §3: a 16-byte struct in memory is a valid representation | No change to the existing type/value layer (SD7) |
| F5 | Implement `i8x16.shuffle` with a 16-iteration loop | §3: jump table, byte moves or `pshufb` are all fine | Correctness first, no `pshufb` feature gate |
| F6 | Pick the plain scalar result for every relaxed op | §2.4/§3: any allowed result is conformant | Reproducible results, trivial code (SD5) |
| F7 | Skip the deterministic profile, or adopt it later | §3: adopting it is optional | No cross-machine determinism work until wanted (SD6) |
| F8 | Skip any `v128` JS/host API | §3: web-embedding only, not required here | Scope control (SD8) |
| F9 | Order the work by what producers emit | Conformance is all-or-nothing only if we claim a 3.0 profile | Fastest path to a usable subset (SD9) |
| F10 | Not preserve NaN payloads | §3: arithmetic NaN payloads are not reproducible, as in scalar wasm | No payload plumbing |

### 7.3 Where looseness stops

These are *not* candidates for fast-and-loose, however tempting, because the
curated suite (once SIMD-6 unblocks it) checks them exactly:

1. **Lane numbering and byte order.** `v128.const` byte *i* is bit `8i`; there is
   no "native word order" freedom. This is the single most likely source of
   endianness bugs and it is fully observable.
2. **`i8x16.shuffle` index mapping.** Indices 0–15 read the first operand,
   16–31 the second. A "reversed" or "clamped" interpretation is wrong, not
   loose.
3. **`swizzle` vs `shuffle` out-of-range behaviour.** `swizzle` zeroes
   out-of-range lanes; `shuffle` cannot have out-of-range indices (validation).
   Do not conflate them with relaxed `i8x16.relaxed_swizzle`, which *is* loose.
4. **`trunc_sat_*` vs `trunc_f*`.** The saturating forms saturate; the trapping
   forms must trap (the project's scalar `trunc_f*` trap path already exists and
   was fixed once). Do not lower both to the same helper.
5. **Floating-point rounding and NaN rules.** `roundTiesToEven`, wasm NaN
   canonicalisation and "no trapping" apply to the fixed-width float ops exactly
   as to scalar wasm; a helper that uses `nearbyint` with a different rounding
   mode is wrong.
6. **Lane immediates.** Per §7.1/L4, validate at decode: it is the cheapest
   memory-safety guard in the whole feature.

### 7.4 The convenient middle

Because L1 (relaxed) is loose by construction and L2/L3 are free, the *only*
place where the project has to be genuinely strict is L0, and that is a
self-contained problem: ~236 pure functions of 16-byte inputs with no traps, no
allocation and no side effects. That is the best possible shape for the
helper strategy in §6 — a table of pure functions, each verifiable against a
handful of `simd_*.wast` assertions, with the looseness concentrated in the
place the spec already declares implementation-defined.

## 8. What this implies for `proposals/SIMD.md`

Nothing in this survey justified re-cutting the proposal note; it *does* confirm
its strategy and sharpen why. **Status:** these edits were folded into
[`proposals/SIMD.md`](proposals/SIMD.md) revision 5 (§4 rewritten, S4 closed by
SIMD-L1, SIMD-8 added for a future backend's native lowering), so the table below
is the record of the review rather than a to-do list. IDs follow the proposal
note's scheme.

| Where | Change implied | Why |
|---|---|---|
| §3 | Add a bullet: "no backend in the tree has a `v128` value type; only LibJIT exists and it has no vector surface at all." | Makes the freedom concrete instead of theoretical; it is the premise of SD1. |
| §4.2 | Note that the 236 stub handlers are the *whole* remaining execution problem, and that they are pure functions of 16 bytes. | Reframes the work as a helper table rather than 236 independent features. |
| §5 / S4, S9 | Fold the alignment question (W3) into S4/S9 rather than leaving it a latent risk: the JIT type must be 16-byte aligned if `v128` crosses a call boundary by value. | `WASM::Value` is `alignas(16)`, the JIT type is 4-aligned; SIMD-0's done-when depends on it. |
| §6 / SD1 | Keep SD1, but say precisely what is permanent: the **semantics layer, the `v128` ABI, and the helper entry points as the portable baseline and the test oracle**. What is *not* permanent is the helper *call* as the only lowering: a backend with native vector registers is expected to lower `v128` itself — instead of, or in addition to, calling the helpers — keeping the same semantics. [`LAYERING.md`](LAYERING.md) §4 already permits exactly that ("or provide its own but keep the same semantics"), and [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §3 expects it of AsmJit/SLJIT. | Stops the helper layer being treated as throwaway *without* putting a future backend's vector registers out of reach — which "a later backend adds vectors *behind* the helpers" would have done. Add SIMD-8 for that path. |
| §6 / SD2 | As-is — the hybrid helper library (W5) is exactly SD2. | Confirmed feasible without JIT-level SIMD. |
| §6 / SD7 | As-is, with W3 making the choice explicit. | Recommended representation stays `{i32×4}` + unaligned access. |
| §7 / SIMD-0 | Add "and set the lowered type to 16-byte alignment" to the work column. | Cheapest fix for S4/S9. |
| §7 / SIMD-7 | As-is; W5 *is* SIMD-7, so the milestone becomes "add SSE2/AVX2 bodies inside the existing helpers". | No new milestone needed. |
| §9 | Link this note. | Provenance for the strategy. |

Also worth recording in the proposal's own terms: this survey adds no new gap
and no new deviation. Every option in §5 is already sanctioned by SD1/SD2/SD7;
the only genuinely *new* recommendation is L4 (validate lane immediates at
decode) in §7.1, which the proposal's §6 guardrails already call for.

## 9. Questions that had to be settled when the code started

Each one changed a helper signature or a type rule, so they were written down
rather than assumed. **All eight are settled**; the answers are the ones the
implementation actually took ([`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §4–§6).

| # | Question | Options | Answer as shipped |
|---|---|---|---|
| Q1 | Helper ABI: by-value `{i32×4}` struct return, or pointer out-parameter? | by-value / pointer (W2) | **Both**, chosen per shape: `callSimdHelper` returns the 16-byte aggregate directly where LibJIT's aggregate ABI handles it, and `callSimdOutHelper` passes an out-pointer for the forms that need one (loads, splats, lane inserts). |
| Q2 | One helper per opcode, or a few generic helpers with an op selector? | ~256 small helpers / ~8 generic ones | **One per opcode**, generated from `WasmBase/wasm_simd_ops.inc`, which is also what the differential table iterates. |
| Q3 | Where does the helper translation unit live? | `WasmBase/` (shared) / `LibJit/` (private) | **Shared** (`WasmBase/wasm_simd_vector.*`, `wasm_simd_scalar.*`), so another backend reuses it without depending on `LibJit/`. |
| Q4 | Are helper calls batched per basic block, or emitted one per opcode? | per opcode / batched | **Per opcode.** Batching is still an optimisation that must not change semantics. |
| Q5 | Does a `v128` live in a GC struct as 16 inline bytes or as a reference? | inline / boxed | **Inline**, `storageBytes = 16` and `storageAlign = 16` (`gcStorageAlign`). |
| Q6 | Do we adopt the deterministic profile for relaxed SIMD from the start? | yes / later | **Later** (SD6): SD5's scalar results are conformant and reproducible on one machine. |
| Q7 | When does hardware dispatch get initialised — per module, per store, or lazily? | per module / per store / lazy singleton | **Neither:** each helper is dispatched through Highway's own `HWY_EXPORT` table, which resolves to the best target the CPU supports on first call. There is no engine-side feature-detection singleton. |
| Q8 | Does the harness marshal `v128` by pointer or by `WASM::Value`? | either | **`WASM::Value`-shaped 16-byte slots** (`alignas(16)`), decoded lane by lane in `Test/WastScript.cpp`. It was a marshalling/formatting task, not an ABI one. |

## 10. References

Engine documentation and sources:

- GNU LibJIT — API reference (the type chapter documents the struct/union/
  pointer/tagged constructors and no vector type):
  https://www.gnu.org/software/libjit/doc/libjit.html
  Installed headers inspected for this note: `/usr/local/include/jit/`
  (`jit-type.h`, `jit-insn.h`, `jit-opcode.h`, `jit-intrinsic.h`).
- SLJIT — README (lists "SIMD" among the supported features and the target
  architectures): https://github.com/zherczeg/sljit
  · main header (primary API documentation): `sljit_src/sljitLir.h`
  · SIMD test suite (the concrete API surface used above):
  `test_src/sljitTestSimd.h`
  · API change log (e.g. the 2024-06-10 `sljit_emit_simd_op2()` change):
  `API_CHANGES`
- AsmJit — project page: https://asmjit.com · repository:
  https://github.com/asmjit/asmjit
  · `asmjit/x86/x86_globals.h` (instruction IDs with ISA tags),
  `asmjit/x86/x86_operand.h` (`Vec`/`Mm`/`KReg`/`Tmm`),
  `asmjit/x86/x86_ra_pass.cpp` (vector register allocation),
  `asmjit-testing/tests/asmjit_test_assembler_x86.cpp`.

WebAssembly specification:

- Syntax — Vector instructions:
  https://webassembly.github.io/spec/core/syntax/instructions.html
- Execution — Numerics, including the "Relaxed Operations" parameterisation:
  https://webassembly.github.io/spec/core/exec/numerics.html
- Appendix — Profiles (the deterministic profile pins every relaxed parameter
  to `0`): https://webassembly.github.io/spec/core/appendix/profiles.html

In-repo:

- [`proposals/SIMD.md`](proposals/SIMD.md) — the proposal note this survey
  informs: §3 is the freedom every workaround here leans on, §5 (S1–S9) are the
  gaps the strategy has to close, §7 (SIMD-0…SIMD-7) is the plan.
- [`SAFETY.md`](SAFETY.md) — the policy a helper-based lowering inherits
  (bounds, trap-vs-`abort`, allocation budget).
- [`TESTING.md`](TESTING.md) — how `v128` values cross the harness boundary (they no
  longer need to be skipped) and the
  spec harness, and the curated `wast_supported.txt` workflow SIMD-6 feeds.
- [`PROPOSALS.md`](PROPOSALS.md) — the note conventions this survey does *not*
  claim to follow (it surveys engines, not a wasm proposal).
