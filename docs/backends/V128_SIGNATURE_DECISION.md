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
