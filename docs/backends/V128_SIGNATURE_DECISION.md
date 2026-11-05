> **DEFERRED by decision (2026-11-05).** This note must be revisited at the
> end of the saga, before the SLJIT/AsmJit saga can be closed and the final
> saga post-mortem written. See
> [`ASMJIT_SLJIT_P3_REFS1.md`](ASMJIT_SLJIT_P3_REFS1.md) §2 for the closure
> checklist.

# Decision note: `v128` across the function boundary (P3's last item)

**Snapshot (2026-11-05, working tree on `backendsMh33`). Status: analysed, not
implemented. No ABI code was started, on purpose — see §5.**

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
