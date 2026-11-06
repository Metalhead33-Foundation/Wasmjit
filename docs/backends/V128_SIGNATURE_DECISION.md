> **DEFERRED by decision (2026-11-05).** This note must be revisited at the
> end of the saga, before the SLJIT/AsmJit saga can be closed and the final
> saga post-mortem written. §11 records the long-term, ABI-breaking plan (treat
> `v128` returns as lowered multi-returns on every backend, LibJIT included) and
> the deferred `v128` block types. See
> [`ASMJIT_SLJIT_P3_REFS1.md`](ASMJIT_SLJIT_P3_REFS1.md) §2 for the closure
> checklist.

# Decision note: `v128` across the function boundary (P3's last item)

**Snapshot (2026-11-05, working tree on `backendsMh33`). Status: analysed, not
implemented. No ABI code was started, on purpose — see §5.**

**Revision 4 (2026-11-06):** **Part III (§21–§27)** is an impact study grounded in
this tree: the host ABI already passes `v128` by value *including results*, the
fixtures and the **spec scripts** exercise one-to-four `v128` parameters, and a
`v128`-native import is mechanically expressible — so the boundary case is a
**conformance blocker**, not a niche one. It rejects the dual-ABI option (§24, on
reference identity and call-site cost) and confirms the single by-reference ABI
(§11) as the end state.

**Revision 3 (2026-11-06):** §18.1 now records the **adjacent-backend rule** — a
backend never embeds another toolkit, so the former "preferred" SLJIT adapter via
AsmJit is **rejected**; the same rule *permits* mixing Assembler and Compiler
inside `Asmjit/`, which is now that backend's path of record for `v128` results.
SLJIT's only remaining routes are the §11 ABI plan, a static C++ thunk per
signature (possible, not planned), or the existing named refusal.

**Revision 2 (2026-11-06):** the former standalone `V128_ABI_ALTERNATIVES.md` is
folded in as **Part II (§12–§20)**, with every toolkit claim it made fact-checked
against this tree. That check produced two corrections: the AsmJit **Builder**
row no longer asserts ``no register allocator`` (the header does not say so), and
a new, checkable finding — `Sljit/` and `Asmjit/` contain **no platform guards at
all**, so the landed SysV-x86-64 `v128` parameter mapping is currently
unconditional. Part II also adds the hybrid **edge-adapter** design, which reaches
the same end state as §11 without breaking the public by-value ABI.

This note exists so the next session starts from the facts rather than from the
refusal table. It is a *decision record*, not a slice post-mortem: no slice
landed, so §14.5 of [`WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) does not
apply.

## 1. The host ABI the core already fixes

`Test/TestBackend.cpp` defines the value and calls every compiled function with
it by value:

```c
struct AbiV128 { uint32_t lane[4]; };            // 16 bytes
Ret (*fnPtr)(void* context, AbiV128, ...);
```

On SysV x86-64 that is **two INTEGER eightbytes**:

| Position | Where it travels |
|---|---|
| first `v128` parameter | two consecutive integer argument registers (e.g. RDI+RSI after `context`) |
| second `v128` parameter | the next two |
| `v128` result | **RAX:RDX** (the only 16-byte integer-class return pair) |

A function with one `v128` parameter therefore consumes **two** argument slots,
and every later parameter shifts by two. That mapping — not the value
representation, which five slices already proved — is the whole difficulty.

## 2. AsmJit: expressible, but the signature mapping is the work

- A `v128` parameter needs **two** `kUInt64` entries in the `FuncSignature`,
  while the Wasm signature has **one** parameter. Everything that indexes
  arguments by Wasm parameter number must learn the two-slot rule: the function
  prologue (`set_arg`), the call lowering (`prepareCallArguments`), and the
  per-type signature cache.
- A `v128` result needs **two** return registers: `signature.add_ret(kUInt64)`
  twice, then `set_ret(0, low)` and `set_ret(1, high)`. AsmJit maps the first
  integer return to RAX and the second to RDX, which is exactly SysV.
- The value model is ready: a result is already a 16-byte buffer, so the return
  path is "load two qwords from the buffer into the two return registers".

**Verdict: doable in this backend, and the second-largest piece of work in the
seam after the parameter-slot mapping.**

## 3. SLJIT: parameters yes, results no

- **Parameters are expressible.** `SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, n)` can
  declare two word slots for one Wasm parameter; the prologue copies
  `SLJIT_S(k)` and `SLJIT_S(k+1)` into the two words of the parameter's
  16-byte local cell. `savedCount` and the slot numbering must count argument
  slots, not parameters.
- **Results are not expressible.** SLJIT returns a single word in R0 (RAX). A
  SysV 16-byte struct returns in **RAX:RDX**, and SLJIT offers no way to place a
  value in RDX: its register file maps R1 to RSI, not RDX, and there is no
  aggregate-return form of `sljit_emit_return`. The only workarounds are hand
  encoding x86 (`sljit_emit_op_custom`) or changing the core's host ABI — both
  are out of bounds: the first is fragile machine code in a portable backend,
  the second would alter a frozen surface for the sake of one toolkit.

**Verdict: `v128` parameters are a real slice; `v128` results are a documented
SLJIT toolkit limitation and must be refused by name, not approximated.**

## 4. What the parity rule implies

`WRITING_A_BACKEND.md` §4 and `LAYERING.md` §4 allow a backend to *refuse* what
its toolkit cannot express, as long as the refusal is by name and the core
reports "no compiled body". So the seam splits into three slices:

1. **`v128` parameters, both backends.** SLJIT: two word slots per parameter.
   AsmJit: two `kUInt64` signature entries per parameter and the two-slot index
   mapping everywhere. Gate: a host call passing an `AbiV128` to
   `(func (param v128) (result i32))`, plus a scalar-first variant
   `(param i32 v128)` that catches a slot-numbering mistake.
2. **`v128` results, AsmJit only.** Two return registers. SLJIT refuses with the
   §3 reason in `unsupportedReason_`. The A/B case must therefore assert
   *agreement on the refusal*, not on the value.
3. **Wasm→Wasm calls with `v128` arguments/results.** Both backends need a
   second call shape: SLJIT's `SLJIT_ARGS*` macros are fixed-arity, and AsmJit's
   per-type signature cache must hold the two-slot form.

Do them in that order: (1) is the one that unlocks the fixture, (2) is small but
divergent, (3) is the larger and riskiest.

## 5. Why nothing was implemented in this session

The parameter-slot mapping touches the prologue, the call path and the signature
cache in **both** backends at once. Starting it without the budget to finish
would leave the ABI seam half-migrated — the one state in which the failure mode
is a *silent* argument mismatch rather than a clean refusal. The project rule is
explicit that a surface is either honoured or refused by name, so an unfinished
migration here is worse than no migration.

## 6. The cheapest remaining win, for contrast

`select_t` on `v128` is still refused (both backends, `SIMD1`'s residual table).
It needs no ABI change: SLJIT selects two words with the existing
`sljit_emit_select`, and AsmJit can select the two **buffer addresses** because a
v128 value is immutable once produced — selecting the pointer selects the value.
That is a genuine one-hook slice and should be taken before the seam.

## 7. Register-map evidence: why SLJIT results are impossible (2026-11-05)

`extern/sljit/sljit_src/sljitNativeX86_common.c` holds the authoritative map for
Linux x86-64 (machine numbers: 0=rax, 1=rcx, 2=rdx, 6=rsi, 7=rdi, 8=r8 … 15=r15):

```c
/* Args: rdi(=7), rsi(=6), rdx(=2), rcx(=1), r8, r9. Scratches: rax(=0), r10, r11 */
static const sljit_u8 reg_map[SLJIT_NUMBER_OF_REGISTERS + 4] =
	{ 0, 0, 6, 7, 1, 8, 11, 10, 12, 5, 13, 14, 15, 3, 4, 2, 9 };
```

Decoded through `SLJIT_R0 = 1`, `SLJIT_S0 = 13`, `SLJIT_SP = 14`, `TMP_REG1 = 15`,
`TMP_REG2 = 16`:

| SLJIT name | index | machine register |
|---|---|---|
| `SLJIT_R0..R3` | 1–4 | rax, rsi, rdi, rcx |
| `SLJIT_S0..S5` | 13, 12, 11, 10, 9, 8 | rbx, r15, r14, r13, rbp, r12 |
| `SLJIT_SP` | 14 | rsp |
| **`TMP_REG1`** | **15** | **rdx** |
| `TMP_REG2` | 16 | r9 |

**RDX is SLJIT's own scratch register.** It is not exposed to the backend, it is
not one of `R0..R3`, and SLJIT's instruction selection may use it between any two
user operations. Pre-loading RDX before `sljit_emit_return` would therefore rely
on the liveness of an internal register that the backend cannot reason about —
the same class of mistake as hand-encoding a `ret`.

**Conclusion (final): `v128` results cannot be expressed on SLJIT with this call
ABI.** Not "not yet": SysV returns the 16-byte integer pair in RAX:RDX, SLJIT
owns RAX as `R0` and RDX as `TMP_REG1`, and it has no aggregate-return form.
AsmJit *can* express it (two `kUInt64` returns ⇒ RAX:RDX), so if results are
implemented at all they are an **AsmJit-only capability**, refused by name on
SLJIT with this note as the reason.

**Parameters and block types are unaffected**: parameters travel in the argument
registers that SLJIT copies into `S0..` at entry (two slots per `v128`), and block
types are internal 16-byte value movement — neither touches RDX.

**The parameter ceiling remains a separate puzzle.** The same table shows six
saved registers, so context + five parameters *should* fit, yet both four and five
parameters produced `SIGILL` in an SLJIT-only test (`REFS1` entry 8). The table
also shows `SLJIT_S0..S5` are the callee-saved file and the context already
occupies `S0`; the next attempt must instrument `sljit_emit_enter` rather than
extrapolate from the register count.

## 8. What landed: SLJIT `v128` parameters (2026-11-05)

The first piece of the seam is in, and it is the one §7 left untouched.

- **A `v128` parameter occupies two argument slots.** The SysV boundary passes
  the 16-byte integer pair in two consecutive GPRs, so the prologue now builds
  `arg_types` from a *slot* counter (`SLJIT_ARG_VALUE(..., nextSlot)`) instead of
  a parameter counter, emits two `SLJIT_ARG_TYPE_W` entries for a vector, and
  derives `savedCount` from the slot count (`nextSlot - 1`) rather than from
  `1 + parameterCount`. Every later parameter therefore shifts by two, which is
  the actual ABI rule — and the one thing a naive mapping gets wrong.
- **The prologue copies both halves** into the two words of the parameter's
  16-byte local cell, from `SLJIT_S(slot)` and `SLJIT_S(slot + 1)`. The integer
  and float slot counters stay independent, as SLJIT's call ABI requires.
- **The fixture deliberately puts a scalar first** (`(param i32 v128)`), because
  a slot-numbering mistake is invisible when the vector is the first parameter.
  It is called through the raw host ABI (`int32_t (*)(void*, AbiV128Arg)`), which
  is the only way to exercise the boundary without a `v128` result.
- **The change re-derived the argument list for every function**, not just the
  vector ones, so it was gated on the full suite rather than the SIMD tags:
  21110 assertions in 700 cases, all backend tags green, Doxygen clean.

**Still open in the seam:** AsmJit `v128` parameters (two `kUInt64` signature
entries per parameter plus the two-slot index mapping in the prologue, the call
path and the per-type signature cache), `v128` results (AsmJit only — §7), and
`v128` block types (needs AsmJit block results, a pre-existing gap).

## 9. What LibJIT does — and why the pointer idea is scratched (2026-11-05)

The reference backend answers the question directly: **LibJIT supports `v128`
returns, with no workaround at all**.

```c
case WASM::ContinuationValueKind::V128: {
    jit_type_t fields[4] = { jit_type_int, jit_type_int, jit_type_int, jit_type_int };
    jit_type_t type = jit_type_create_struct(fields, 4, 0);
    jit_type_set_size_and_alignment(type, 16, 16);
    return type;
}
```

and in the signature builder (`LibjitOpcodeDispatcher.cpp`):

```c
const bool multiResult = resultCount > 1;
    jit_type_t returnType = resultCount == 0 ? jit_type_void
            : resultCount == 1 ? resultTypes[0] : jit_type_void;
    if (multiResult)
        params.push_back(jit_type_void_ptr);
```

So a single `v128` result makes the function's return type the **16-byte struct**,
and LibJIT's own ABI layer returns it by value — RAX:RDX on SysV, which is exactly
what the harness's `AbiV128` expects. The hidden pointer is used **only for
multi-result** functions, never for `v128`.

**Consequences:**

1. **There is no workaround to copy into SLJIT.** LibJIT's ability comes from its
   *type system* (struct types with explicit size/alignment); SLJIT has no
   aggregate types and no multi-register return, so it cannot follow. §7 already
   proved the register side (RDX is `TMP_REG1`); §9 proves there is no alternate
   route hiding in the reference backend.
2. **Pointer-lowering `v128` returns is scratched.** It would be a *new* ABI
   (neither LibJIT nor AsmJit uses it), affecting every backend, the harness and
   cross-instance imports — a milestone / embedding-surface change, not a slice.
   The user's rule applies: if the reference backend does not do it, do not invent
   it here.
3. **AsmJit's two-register return is the correct match** — `add_ret(kUInt64)`
   twice plus `set_ret(0/1)` produces the same RAX:RDX pair LibJIT produces, so
   the observable ABI stays identical across all three backends.
4. **SLJIT `v128` results are a permanent, documented refusal** — not a TODO.
   The function is left without `fnPtr`, the core reports "no compiled body", and
   nothing is miscompiled.

## 10. Correction: AsmJit cannot return `v128` either (2026-11-05)

§7 and §9 assumed AsmJit could produce the SysV RAX:RDX pair with two
`kUInt64` returns. **That assumption was wrong**, and the header says so:

```c
struct FuncSignature {          // /usr/include/asmjit/core/func.h:390
  ...
  TypeId _ret = TypeId::kVoid;  // :409  — one return, not a list
  ...
  void set_ret(TypeId ret_type) noexcept { _ret = ret_type; }   // :512
};
```

`grep -c add_ret` on that header returns **0**: AsmJit 1.21 models exactly one
return value. A 16-byte struct return therefore has no representation — the
`kInt32x4` mapping the translator uses is a *vector* return (XMM0), which is the
wrong eightbyte class for a `{uint32_t[4]}` struct anyway.

**The engine already has the only alternative, and it is the one that was
scratched:** for `results.size() > 1` the translator appends a trailing
`kUIntPtr` "caller-provided result buffer" argument, explicitly *"Do not rely on
a platform's aggregate-return ABI."* That is pointer-lowering for multi-results —
and neither LibJIT nor AsmJit uses it for a single `v128`, so extending it to
`v128` would be a new ABI and stays scratched.

### Where that leaves the three items

| Item | SLJIT | AsmJit | LibJIT |
|---|---|---|---|
| `v128` **parameters** | **implemented** (two argument slots) | feasible: the signature's *argument* list is a list, so two `kUInt64` entries work; needs the two-slot index mapping | supported |
| `v128` **results** | **impossible** — RDX is `TMP_REG1`, no aggregate return | **impossible** — one return value only | supported (16-byte struct type) |
| `v128` **block types** | feasible: SLJIT's blocks already carry results/params; needs a 16-byte `transferToBase` | **not possible** — AsmJit's blocks have no results/parameters at all | supported |

So of the two items requested after parameters, **neither is implementable in
AsmJit**, and one of them (block types) is implementable in SLJIT alone. That is a
capability divergence, not an ABI one: the core reports "no compiled body" for the
AsmJit function, exactly as it does for SLJIT `v128` results.

**Consequence for saga closure:** `v128` results on SLJIT/AsmJit are a permanent,
documented limitation of those toolkits (only LibJIT supports them), and `v128`
block types are SLJIT-only. Both belong in the final saga post-mortem as
*permanently refused with the toolkit limitation named*, not as pending work.

## 11. Long-term plan: `v128` returns as a lowered multi-return (DEFERRED)

**Status: deferred by decision (2026-11-05). This is an ABI-breaking, long-term
refactor, to be executed only after the remaining AsmJit and SLJIT backend work
(in this saga: the rest of P3, then P4 references/GC and exceptions, then P5
`call_ref`/continuations) is complete.**

### The plan

Stop relying on any platform's aggregate-return ABI for `v128` altogether, and
**treat a `v128` return as what the engine already knows how to do: a
multi-return**. Concretely, a function returning `v128` would take the same
trailing *caller-provided result buffer* argument that `results.size() > 1`
already uses (`AsmjitTypeTranslator.cpp`: *“Multi-value tuples are written through
the caller-provided trailing result buffer. Do not rely on a platform's
aggregate-return ABI.”*), and write the vector as **four 32-bit integers** into
it.

Properties of that plan:

- **Uniform across all three backends, including LibJIT.** LibJIT can already
  return the 16-byte struct directly, so applying the lowering there is a
  deliberate *unification*, not a necessity — it removes the last
  backend-specific ABI behaviour and makes the three code generators
  interchangeable at the boundary.
- **It reuses an existing convention.** The multi-result buffer is already
  implemented and tested; `v128` becomes a fourth “one value, four words” case in
  the same mechanism instead of a special aggregate rule.
- **It is ABI-breaking.** The host harness (`AbiV128` by value), cross-instance
  imports, `ref.func`-style callables and every embedder that calls a
  `v128`-returning export would have to change. It therefore needs a **milestone
  decision** under [`docs/API_ABI_STABILITY.md`](../API_ABI_STABILITY.md), not a
  slice commit.
- **What it buys:** `v128` returns on SLJIT and AsmJit, which today are
  impossible (§7: RDX is `TMP_REG1`; §10: AsmJit has a single return value), and
  one ABI instead of three behaviours.

### Also deferred: `v128` block types

SLJIT could carry a `v128` block type today with a 16-byte `transferToBase`; AsmJit
cannot, because its blocks carry no results or parameters at all (§10). Rather
than ship a SLJIT-only block type that the ABI plan would then have to revisit,
**`v128` block types are deferred to the same refactor**, and may need their own
workaround — most likely the same “value in four words” lowering applied to block
results, or a slot-based form once AsmJit gains block results.

### Sequencing

1. Finish the AsmJit/SLJIT feature work (P3 remainder, P4, P5).
2. Decide the ABI milestone (`API_ABI_STABILITY.md`) for the uniform
   multi-return lowering.
3. Implement it in all three backends at once, with the existing multi-result
   tests as the template, plus a new `AbiV128`-by-buffer harness form.
4. Only then can `v128` block types and `v128` results on SLJIT/AsmJit be marked
   *implemented* rather than *refused*.

# Part II — Alternatives, fact-checked against this tree (2026-11-06)

This part replaces the former standalone `V128_ABI_ALTERNATIVES.md`. That note was
written **without looking at the code**, flagged its toolkit claims `[verify]`,
and this part resolves or corrects every one of them against the repository. Where
the two parts disagree, **this part wins**, because it cites files.

## 12. Strategic context (unchanged)

SLJIT/AsmJit exist to reach platforms LibJIT does not target. That makes SLJIT the
strategically important backend (it targets many architectures; **AsmJit ships
x86 and AArch64 only — verified**: `/usr/include/asmjit/` contains `x86/` and the
AArch64 set `a64.h`, `arm/a64*.h`, and no AArch32 emitter), and it makes
SysV-x86-64-specific ABI logic a liability.

## 13. "SLJIT = Stack-Less JIT" changes nothing (verified verbatim)

`extern/sljit/sljit_src/sljitLir.h` (lines 52–56) says it in its own words:

> The compiler is more like a platform independent assembler, so there is no
> built-in variable management. Registers and stack must be managed manually
> (the name of the compiler refers to this).

And "a fixed stack space can be allocated for local variables" is listed as an
advantage in the same comment block. The backend already uses it for the 16-byte
`v128` cells, so implementing our own value stack would not touch the real
obstacle.

## 14. The real obstacle is RDX (unchanged, §7 is the evidence)

§7 is the authoritative decoding: `SLJIT_R0` = rax, `SLJIT_R1` = rsi,
`SLJIT_R2` = rdi, `TMP_REG1` = **rdx**. SysV returns a 16-byte `{uint32_t[4]}`
in RAX:RDX; SLJIT owns RAX and hides RDX, and has no aggregate-return form.

## 15. Why LibJIT can and the others cannot (unchanged)

LibJIT returns the struct because it has a type system (`jit_type_create_struct`
with explicit size/alignment) and its ABI layer picks the registers. SLJIT has no
aggregate types; AsmJit's Compiler layer has a single return value (§10).
"Lower-level" is not the axis that matters — **whether the toolkit lets us choose
the physical return registers** is.

## 16. AsmJit has layers — and the Assembler is a real option (verified)

| Layer | Exists in this tree | Notes |
|---|---|---|
| `x86::Compiler` | `asmjit/x86/x86compiler.h` | virtual registers; one return value per `FuncSignature` (§10) |
| `x86::Builder` | `asmjit/x86/x86builder.h` | no allocator is *documented in the header*; **do not assume** it is RA-free without reading it |
| `x86::Assembler` | `asmjit/x86/x86assembler.h` | emits physical registers, so loading RAX and RDX before `ret` is ordinary code |
| `FuncFrame` / `FuncArgsAssignment` | `asmjit/core/func.h` (two declarations) | usable for prologue/epilogue bookkeeping |
| executable memory for an **AsmJit-internal** adapter | `asmjit/core/jitallocator.h`, `jitruntime.h` | `JitAllocator` lets the backend own its own code memory; note the adjacent-backend rule in §18.1 forbids using this to serve the SLJIT backend |

**Correction from the original note:** the Builder row claimed "register
allocation: none". That is **not verifiable from the headers** and is now marked
as an open question rather than a fact. The Assembler row stands.

## 17. Platform caveat — and it is unmitigated in this tree

The original note flagged the platform rules `[verify]`. The ABI rules themselves
are external facts and cannot be checked here, but the **code fact is checkable and
is the important one**: `Sljit/` and `Asmjit/` contain **no platform guards at
all** — `grep -rn '_WIN64\|__aarch64__\|SLJIT_CONFIG_ARM\|kArchARM'` over those
sources returns nothing. So the SysV-x86-64 shape of the two-slot `v128`
parameters, the `S0`/`S1` argument mapping and the RAX/`TMP_REG1` reasoning are
currently **unconditional** in the backends.

That makes this the first real portability debt of the saga: today the backends
are correct only on SysV x86-64 (and would need review on Windows x64, AAPCS64 —
SLJIT ships `sljitNativeARM_32.c`, `sljitNativeARM_TF_32.c`, `sljitNativeARM_64.c`,
so the target exists — and 32-bit targets).

## 18. The hybrid design (recommended direction, unchanged in substance)

Internal cells in memory for `v128` and multi-value traffic, scalars in registers,
and **by-value adapters at the host edge only**, where the C ABI is unavoidable.
Where a toolkit cannot express the by-value edge (SLJIT `v128` results on SysV),
the adapter is emitted another way (§18.1) or the signature is refused by name.

This is §11's end state *without* breaking the public ABI: §11 lowers every edge;
this lowers the internal convention and keeps by-value edges behind adapters.

### 18.1 The adjacent-backend rule, and what it leaves for SLJIT

**Architectural rule (2026-11-06): backend implementations do not mix.** AsmJit
  (or LibJIT) may not be used inside the SLJIT backend, SLJIT may not be used
  inside the AsmJit backend, and so on. A backend is self-contained code
  generation plus its own auxiliary *C++* glue; it is never a front end for a
  neighbouring toolkit. The one permitted combination is **inside** a backend that
  ships both layers: within `Asmjit/`, the **Assembler and the Compiler may be
  mixed** freely, because both are AsmJit.

Consequences, replacing the previous "preferred" option:

- **REJECTED — a SLJIT adapter emitted by the AsmJit Assembler.** This was the
  former first choice and is now forbidden by the rule above, regardless of its
  technical merit. `V128_ABI_ALTERNATIVES.md` proposed it; the proposal is
  withdrawn.
- **ALLOWED, and now the AsmJit path of record:** inside `Asmjit/`, a Compiler-body
  function with a **Compiler + Assembler edge** for `v128`-result signatures. The
  Assembler picks RAX/RDX directly, `JitAllocator` owns the code memory, and both
  layers are the same toolkit, so the rule is not bent. Cost: one code-generation
  path per `v128`-result signature, gated per platform.
- **REJECTED for SLJIT — `sljit_emit_op_custom` to preload RDX.** Independent of
  the mixing rule: RDX is `TMP_REG1`, so SLJIT’s own epilogue may clobber it
  between the custom instruction and `ret`, and hand-encoded machine code pins the
  backend to one toolkit version. Two reasons, either sufficient.
- **POSSIBLE but not planned — a SLJIT-only, statically compiled C++ thunk.** A
  plain C++ function in `Sljit/` (no other toolkit involved, like the existing
  `sljitMemoryAddress` helpers) that has the *external* by-value signature, calls
  the SLJIT body — which would write the 16 bytes into a caller-provided buffer —
  and returns the struct by value itself. It is portable and rule-compliant, but
  it needs **one thunk per distinct Wasm signature**, because only a thunk with the
  exact argument list is ABI-compatible. That is a combinatorial table, and it is
  the same lowering §11 proposes, just applied at the edge instead of by the
  harness. Filed as a fallback if §11 is ever rejected.
- **DEFAULT today:** refuse the signature by name, as §7/§10 already do; the core
  reports "no compiled body" and nothing is miscompiled.

### 18.2 What it gives up, and its risks (corrected)

- **Performance:** `v128` and multi-value traffic goes through memory cells; scalars
  stay in registers. Liftoff-style trade, limited to the vector path.
- **SLJIT SIMD is not hypothetical:** `sljitLir.h` defines `SLJIT_HAS_SIMD` and
  states *"Some SIMD operations are supported by the compiler"*, with twelve
  `sljit_emit_simd` mentions. **Which architectures implement it is not stated in
  the header** — still open, but the API exists.
- **Parameter ceiling:** the unexplained `SIGILL` at four and five parameters
  (`REFS1` entry 8) must be resolved by instrumenting `sljit_emit_enter` **before**
  any register-passing design depends on it. If the real ceiling is three, §18's
  "scalars in registers" needs rethinking.
- **Edge inventory:** every by-value entry path — direct exports, cross-instance
  imports, `ref.func` callables, indirect calls — must be shown to pass through an
  adapter.

### 18.3 Options considered and not chosen (unchanged)

- **One context struct for everything** — portable, but breaks the public by-value
  surface and puts scalars in memory on the hot path.
- **Uniform lowered multi-return on all backends including LibJIT** (§11) — still
  the cleanest end state; needs the `API_ABI_STABILITY.md` milestone decision.
- **An emulator or virtual architecture on top of the toolkits** — not needed;
  what is missing is a defined internal ABI plus edge adapters, which is a
  baseline-compiler design, not an interpreter.

## 19. Next steps (re-ordered by what the code says)

1. **Resolve the 4–5 parameter `SIGILL`** by instrumenting `sljit_emit_enter` — it
   gates every register-passing assumption here.
2. **Audit the landed two-slot `v128` parameters for portability** (§17): they are
   currently unconditional SysV x86-64 logic; either gate them or move them to the
   frame-cell convention.
3. **Read `x86builder.h`** if the Builder layer is ever considered — its RA
   behaviour is undocumented in the header and the original note overstated it.
4. **Prototype one signature**, `(func (param i32) (result v128))`, **inside
   `Asmjit/`**: Compiler body plus an Assembler edge on SysV x86-64 (§18.1), behind
   the existing "no compiled body" refusal. SLJIT has no equivalent to prototype
   until the §11 ABI plan lands.
5. If it holds, amend §7/§10/§11: SLJIT/AsmJit `v128` results move from
   "impossible" to "possible via adapter, host ABI unchanged".

## 20. Provenance of Part II

Everything above was checked in this tree on 2026-11-06:

| Claim | Verdict |
|---|---|
| AsmJit targets x86 + AArch64 only | **verified** (`/usr/include/asmjit/{x86,a64.h,arm/a64*}`) |
| AsmJit Assembler/Builder exist | **verified** (`x86assembler.h`, `x86builder.h`) |
| `FuncFrame`/`FuncArgsAssignment` exist | **verified** (`core/func.h`) |
| Adapter memory can be owned | **verified** (`core/jitallocator.h`, `core/jitruntime.h`) |
| SLJIT has SIMD ops | **verified at API level** (`SLJIT_HAS_SIMD`, 12 `sljit_emit_simd` mentions); per-arch set **open** |
| Builder has no allocator | **not verified** — overstated in the original note, corrected |
| SLJIT "stack-less" prose | **verified verbatim** (`sljitLir.h` lines 52–56) |
| SLJIT ARM/AArch64 backends ship | **verified** (`sljitNativeARM_32.c`, `sljitNativeARM_64.c`) |
| Our backends have no platform guards | **verified**: zero matches for `_WIN64`/`__aarch64__`/`SLJIT_CONFIG_ARM` in `Sljit/`+`Asmjit/` |
| Windows x64 / AAPCS64 struct rules | **external ABI facts**, not checkable here; recorded as portable-target risks |
| LibJIT unmaintained | **external**, not a code claim |

# Part III — Impact study: `v128` on the boundary vs `v128` as a variable (2026-11-06)

Scope: how much does the boundary actually matter, in this project and in
realistic guests — and what would "support it at all costs" cost? Every claim
marked **[repo]** is verified in this tree; **[external]** is not checkable here.

## 21. Yes — `v128` functions travel, and this repository already depends on it

**[repo] The host side is fully implemented, results included.**
`Test/TestBackend.cpp` has a dedicated aggregate path:

```cpp
const AbiV128 value = callWithV128Params<AbiV128>(fnPtr, vm, v128Args, count);
```

It covers `v128` **parameters (1–4, including the stack-passed third and
fourth)** and a `v128` **result**, with scalar results too. Its comment records a
hard-won detail: the path *bypasses* `jit_apply`, "whose third stack-passed
aggregate is damaged on this LibJIT; the generated code itself uses the ordinary
host C ABI". So the engine's contract is by-value aggregate, and LibJIT's
*generated code* satisfies it today.

**[repo] The test corpus exercises it deliberately.**

| Fixture / test | What it pins |
|---|---|
| `Test/wasm_wat/simd_abi_args.wat` | `(param v128 v128 v128) (result v128)` and a four-parameter form; `second`/`third`/`sum3`/`swizzle_third`/`fourth` |
| `Test/main.cpp` "three and four v128 parameters cross the boundary" | the S9 closure: "**several spec scripts pass three `v128` operands to one function**, where the third no longer fits in a vector register and is stack-passed" |
| `Test/main.cpp` "`(param v128) (result v128)` — all four lanes must survive the boundary" | identity, through a local, through a block |
| `Test/wasm_wat/profile_v128_type.wat`, `profile_relaxed_simd.wat`, `Test/consumer/consumer_abi.wat` | `(param v128) (result v128)`, `(result v128)`, `(param i32) (result v128)` |
| `Test/wasm_wat/asmjit_i32_unsupported.wat` | `(param v128) (result v128)` as the **pinned refusal** for the new backends |

**The single most important line for this study** is the one in `main.cpp`: the
Wasm **spec scripts** pass three `v128` operands to one function. That makes
boundary support a **conformance** requirement, not a convenience feature: a
module containing such a function cannot be fully compiled by SLJIT or AsmJit
today (they refuse that function by name; the rest of the module still compiles).

## 22. Can natives be registered with `v128` parameters and results?

**[repo] Mechanically yes, on the host side.** `RegistryImportResolver`
registers a `WASM::Callable { fnPtr, context, localTypeIdx }`; the *type* check is
against the module's declared import type, and the calling convention for an
export is exactly what `callWithV128Params` performs — i.e. a C++ native declared
as

```cpp
AbiV128 kernel(VMContext*, AbiV128, AbiV128);
```

is ABI-compatible with the by-value contract. So an embedder *can* write such a
native today.

**[repo] But the new backends cannot call it.** Both refuse `v128` in a call
signature: AsmJit via `requireScalar(parameter.val, "call parameters")`, SLJIT via
`widthOf(...) != 0` in `emitCall`. So on SLJIT/AsmJit a module that *imports* a
v128-native kernel compiles every other function and refuses the caller. That is
the realistic embedding scenario the boundary blocks: **host-side SIMD kernels**
passed in as imports (image filters, codecs, crypto primitives).

## 23. Can guest languages even produce such functions?

**[external]** (toolchains are not in this tree; the project's LF2 studies are
the place to pin versions):

| Source language | `v128` in a function signature |
|---|---|
| C/C++ (clang, Emscripten) with `<wasm_simd128.h>` / `__m128` | **yes** — vector parameters and returns are ordinary |
| Zig (`@Vector(4, u32)`) | **yes** |
| Rust (`core::arch::wasm32::v128`) | **yes** for the type; some intrinsics remain unstable |
| AssemblyScript | **no** — no language-level SIMD value type |
| Hand-written WAT | yes, but authors rarely do it |

So the scenario is: **any C/C++/Zig/Rust module built with SIMD enabled and a
vector-valued function in its exported or internal surface**. Emscripten's SIMD
output routinely has `v128` parameters (inlined helpers usually keep vectors
internal, but not always). That is a mainstream toolchain, not an exotic one.

## 24. Option A — two ABIs coexisting (by value *and* by reference)

**Verdict: rejected.** Reasons, in order of severity:

1. **Reference identity breaks.** `ref.func f` must yield one stable address per
   function; two entry points means two addresses, so `ref.eq (ref.func f)
   (ref.func f)` could be false. That is a semantic wart in exchange for an ABI
   convenience.
2. **Every call site must choose.** Calls, indirect calls, imports and
   `Callable` registration all need to know which entry point to use — the
   decision leaks into the loader and the embedder API.
3. **Two code paths per v128-signature function**, doubling code size and
   splitting test coverage for exactly the functions that are already hardest.
4. It does **not** remove the hard case: on SysV x86-64 the by-value variant is
   still SLJIT-impossible (§7), so SLJIT would ship only the by-reference one and
   the *host* would still need the by-value form.

## 25. Option B — pick the portable one (by reference), i.e. §11

**Verdict: correct direction, already documented, deliberately deferred.** It is
the uniform lowered multi-return: a `v128` result becomes four words in a
caller-provided buffer, and (symmetrically) a `v128` parameter could travel as a
pointer. Consequences, honestly:

- **Breaks the ABI** for the harness, embedders, imports and `ref.func` callables
  — a milestone decision under `API_ABI_STABILITY.md`, not a slice.
- **Costs performance on the vector path**: values move through memory instead of
  registers (scalars keep their registers; only `v128` and multi-value traffic
  pays). For a 16-byte value the difference is a few moves plus a store/load pair
  per boundary crossing.
- **Buys uniformity**: three backends, every platform, no per-platform adapters,
  and it finally lets SLJIT and AsmJit compile SIMD modules that use the
  boundary.

## 26. Is this niche? The impact assessment

| Scenario | Today on SLJIT/AsmJit | How common |
|---|---|---|
| `v128` **as a variable** (locals, stack, memory, all SIMD ops) | **fully supported** — the whole SIMD surface except the boundary | ubiquitous |
| spec SIMD scripts with 3 `v128` operands **[repo]** | **refused function** | conformance suite — certain |
| C/C++/Zig/Rust SIMD kernel with vector params **[external]** | refused function | mainstream toolchain output |
| embedder registering a v128-native import **[repo]** | caller refused | realistic embedding |
| hand-written WAT / AssemblyScript output | unaffected (no such signatures) | common |

**Not niche.** The boundary case is a *minority* of SIMD code by volume — most
vectors stay inside a function — but it is a **hard blocker** wherever it appears:
there is no fallback, the function simply has no compiled body. And the project's
own conformance suite contains it, which means SLJIT/AsmJit cannot pass the SIMD
spec suite without it regardless of how rare it is in the wild.

**Cost of "support at all costs" (Option B):** one ABI break, one milestone
decision, a modest per-crossing cost on vectors, and a large but mechanical
implementation across three backends. **Cost of doing nothing:** two of three
backends can never run a module whose SIMD surface crosses a function boundary,
and the spec suite stays partially refused. Given the conformance evidence, the
break is justified — but it is still the *last* thing to do, because it re-writes
surfaces that the remaining feature work (GC, exceptions, continuations) will
touch anyway.

## 27. Recommendation (unchanged in sequence, now evidence-based)

1. Keep §11 deferred until P4/P5 land — the ABI change would otherwise be redone.
2. Finish the AsmJit-only Assembler edge (§18.1) as the *interim* capability,
   because it breaks nothing and unblocks the conformance cases on one backend.
3. Then take the milestone decision for the uniform by-reference lowering and
   implement it in all three backends at once, which retires the SLJIT refusal and
   makes the boundary case portable by construction.
4. Do **not** build a dual-ABI (§24): the identity and call-site costs outweigh a
   benefit that the single portable ABI already provides.
