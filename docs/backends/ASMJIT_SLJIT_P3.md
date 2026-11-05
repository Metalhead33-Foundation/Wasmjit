# Post-mortem: the AsmJit/SLJIT P3 floating-point slice

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. The log below is appended as each backend lands, so the
document records the parallel development rather than reconstructing it at the
end.**

Scope: scalar `f32`/`f64` in the module compiler — parameters, results,
locals, constants, globals, memory, arithmetic, comparisons, conversions,
reinterpretation and `select`. `v128`/SIMD and the `f16` proposal stay out.

## 0. Development log

### 2026-11-05 — entry 2: SLJIT lands the scalar float slice

- **SLJIT is green first**, as the worked example: `[sljit]` 711 assertions in
  14 cases, of which 206 are the new `[float]` cases over
  `Test/wasm_wat/float_basic.wat`.
- **The toolkit requires the float register count in `scratches`.** The first
  build aborted inside `sljit_emit_enter` (`function_check_arguments`): SLJIT
  counts floating-point scratch registers in the same argument via
  `SLJIT_ENTER_FLOAT(n)`, and a float parameter or result with
  `n == 0` is rejected. The prologue now passes
  `3 | SLJIT_ENTER_FLOAT(max(3, float params))`.
- **`SLJIT_F_EQUAL` is "equal or unordered".** `f32.eq(NaN, NaN)` returned 1
  until the condition became `SLJIT_ORDERED_EQUAL`; `ne` keeps
  `SLJIT_UNORDERED_OR_NOT_EQUAL`, and the ordering comparisons use the
  `SLJIT_ORDERED_*` forms.
- **One slot model, two register classes.** Slot copies stay bitwise
  (`MOV32`/`MOV`), so locals, block transfer and `select` needed no change;
  only function entry/return, calls and global/memory access consult the
  declared kind. Trapping conversions and the semantics-sensitive
  min/max/rounding operations call the same kind of C helper the integer slice
  already uses.
- **Two test-side mistakes, kept in the log because they are ABI traps:** a
  literal `-7` passed to an `i64` parameter deduces `int` and leaves the upper
  half of the register unspecified (it showed up as `4294967289.0`), and an
  `i64` conversion test must pass `INT64_C(...)`.

### 2026-11-05 — entry 3: AsmJit lands the same slice, with a different strategy

- **AsmJit kept its register-based value model.** A float travels through the
  value stack as its IEEE bit pattern in a Gp; it becomes an Xmm only at an
  arithmetic/compare/ABI boundary. The consequence is that `select`, block
  result transfer, locals and the control-frame bookkeeping needed no change
  at all — the whole slice is additive. SLJIT, by contrast, needed the
  declared kind at entry/return/call/global/memory but kept its slot model.
  Two backends, two shapes, one fixture.
- **Bug 1 (ordering): instructions emitted after an `InvokeNode` run after the
  call.** `invoke->set_arg(0, materializeFloat(...))` looked natural but the
  `movd` was appended *after* the call node, so the helper received an
  uninitialised Xmm (`f32.sqrt(9)` returned NaN, `f32.trunc(-1.75)` returned
  -511). Every helper now materialises its arguments before `compiler.invoke`;
  `prepareCallArguments` returns `asmjit::Reg` operands so the ordinary call
  path can do the same.
- **Bug 2 (flags): zeroing a register after `ucomiss` destroys the
  comparison.** The `setcc` sequence read the flags of the `xor_` that was
  meant to clear the destination. All flag registers are now zeroed *before*
  `ucomiss`/`ucomisd`, and the `setcc` pair runs immediately after it.
- **Bug 3 (API): `asmjit::Inst::Id` is `asmjit::x86::Inst::Id`, and
  `InvokeNode::set_arg` takes `Reg`/`Imm`, not `Operand`.** Two compile-time
  corrections rather than design changes.
- **The "unsupported" fixture had to move.** `asmjit_i32_unsupported.wat` used
  an f32 signature to force the rollback path; floats are now supported, so it
  uses `v128` (still outside the scalar slice). The SLJIT refusal test had
  already moved to `v128`. This is the P3 equivalent of entry 2's div→float
  move, and it is the expected cost of a slice landing.
- **Bug 4 (SLJIT block signatures): the value-type branch kept the old
  i32/i64 filter.** `block (type $f32_result)` worked, `block (result f32)`
  did not, because the two encodings take different paths in
  `blockSignature`. The fixture now contains both shapes, which is why the
  block-result probe exists.
- **Counts after both backends:** `[sljit]` 1093 assertions in 15 cases,
  `[asmjit]` 806 assertions in 22 cases, and the cross-backend float case 101
  assertions. The fixture is shared by construction: one `.wat`, two
  generators, and an A/B case that compares outcomes rather than expectations.

### 2026-11-05 — entry 1: scope and the shared semantic decisions

- **One fixture set again.** `Test/wasm_wat/float_basic.wat` is the P3
  counterpart of `asmjit_i32_basic.wat`: every scalar opcode family, the same
exports, the A/B test can compare both generators over it.
- **NaN and rounding rules are copied from the LibJIT backend, which defines
  them for this engine:** `min`/`max` return a quiet NaN when either operand is
  NaN and are sign-of-zero aware; `trunc`/`nearest` quiet a NaN and
  `nearest` is ties-to-even (`nearbyint`); `copysign` is `std::copysign`.
- **Trapping conversions are C helpers, not raw toolkit conversions.**
  `i32.trunc_f32_s` and friends must trap on NaN and on out-of-range input;
  SLJIT's `CONV_S32_FROM_F32` and x86's `cvttss2si` do not trap. Both backends
  call the same shape of checked helper.
- **SLJIT keeps its slot model.** Float slots are copied bitwise with the
  integer moves; only the operations that must know the ABI class (function
  entry/return, calls, global/memory access) consult the declared kind. That
  keeps the change local to the places where a register class matters.
- **AsmJit needs a value that can be a Gp or an Xmm.** Its emitter currently
  stores `Gp` in `values`; the slice introduces a small tagged value so a
  mixed signature and `select` work without a second operand stack.

## 1. What the shared ABI forced

- **Two independent register sequences.** Integer arguments go in the integer
  argument registers and float arguments in the float ones, each indexed from
  its own zero. Both backends had to implement that rule, and neither could
  reuse its integer argument loop verbatim: SLJIT counts `FR0..` separately
  from `S0..`, AsmJit materialises `Xmm` operands while the body keeps bit
  patterns in `Gp`.
- **The return register is class-specific too.** A float result returns in the
  float return register; the trap epilogue must therefore zero *that* register
  and return with the float move, in both backends.
- **Helper calls follow the C ABI, not the Wasm ABI.** The shared semantic
  helpers take and return C `float`/`double`; SLJIT passes them in `FR0/FR1`,
  AsmJit in XMM0/XMM1, and both read the result from the float return register
  before copying it back into the slot/`Gp`.
- **The single-result rule survived unchanged.** Multi-value results still
  refuse, so the result-tuple layout and the trailing out-buffer parameter
  were not touched by this slice.

## 2. What broke, with the failing fixture

1. **SLJIT rejected every float signature at entry** until the prologue passed
   `SLJIT_ENTER_FLOAT(n)`; the argument check aborted the process in
   `sljit_emit_enter`. Fixture: `float_basic`, first instantiation.
2. **`SLJIT_F_EQUAL` is unordered-inclusive.** `f32.eq(NaN, NaN)` returned 1
   until the condition became `SLJIT_ORDERED_EQUAL`.
3. **AsmJit materialised helper arguments after the call node.** `f32.sqrt(9)`
   returned NaN and `f32.trunc(-1.75)` returned -511; the exact-value cases
   exposed it immediately, and the fix is to materialise before `invoke`.
4. **AsmJit zeroed comparison-flag registers after `ucomiss`.** `f32.eq(1,1)`
   returned 0 and the ordered comparisons were inverted; the zeroing moved
   before the compare.
5. **The "unsupported" rollback fixture moved twice.** `i32.eqz` served before
   P0-P2, `f32` served through P0-P2, and P3 needs `v128`. Both suites now use
   the vector signature, and the fixture comment records why.

## 3. What the toolkit could not express

| Constraint | Consequence | Accepted or temporary |
|---|---|---|
| SLJIT has no sqrt/ceil/floor/round/min/max float opcodes | those operations call the shared C helpers; add/sub/mul/div/abs/neg/copysign are native | accepted |
| SLJIT float scratch registers are counted in `scratches` via `SLJIT_ENTER_FLOAT` | one prologue argument encodes two register files | accepted |
| AsmJit emits nodes in program order | an argument materialised after `invoke` runs after the call; every path now prepares operands first | accepted; it is the node model, not a defect |
| AsmJit `set_arg` takes `Reg`/`Imm`, not `Operand` | prepared arguments are a `vector<Reg>` | accepted |
| x86 has no unordered-aware `setcc` for the four ordered relations | `ucomiss` + two `setcc` + `and`/`or` per comparison | accepted; branch-free |
| no `f16` load/store in either backend | `f32.load_f16`/`store_f16` stay unhandled | temporary; the proposal is separate |

## 4. What the tests did not catch

The exact-value cases caught all four implementation bugs; the A/B case would
not have, because two identical miscompiles compare equal. Conversely, the
exact-value cases cannot catch *drift* when a fixture changes in both suites at
once, which is what the A/B case is for. The two are complementary and this
slice needed both: the exact cases found the bugs, and the A/B case now pins
the agreement over 20 probes.

## 5. What was slower, and by how much

**Not measured yet.** No benchmark has been run for this slice.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the one-fixture, two-generator A/B case, opened with the slice rather than
  after it;
- the shared semantic helpers (NaN, signed zero, ties-to-even) copied verbatim
  from the reference backend — the rules are the engine's, not the toolkit's;
- a representation choice made per toolkit: SLJIT keeps slots and consults the
  declared kind at the boundary; AsmJit keeps Gp bit patterns and materialises
  Xmm at the boundary. Both are coherent and neither leaks into the core.

Do not copy:

- `set_arg(materialize(...))` in a node-based emitter, or any instruction
  emitted after the call node that the call is supposed to consume;
- flag-register zeroing between a compare and its `setcc`;
- an "unsupported" fixture chosen from the family the next slice will land —
  pin the refusal to a proposal that is not next.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| `v128`/SIMD | **partially lowered** — transport, bitwise, `i32x4` lane access, the integer lane arithmetic/comparison families, shifts, saturating arithmetic, min/max, abs and popcnt; float lanes, narrowing/widening, shuffle and `v128` signatures are still unhandled | see [`ASMJIT_SLJIT_P3_SIMD1.md`](ASMJIT_SLJIT_P3_SIMD1.md), [`ASMJIT_SLJIT_P3_SIMD2.md`](ASMJIT_SLJIT_P3_SIMD2.md) and [`ASMJIT_SLJIT_P3_SIMD3.md`](ASMJIT_SLJIT_P3_SIMD3.md) |
| `f16` load/store | function left without `fnPtr` | `f32.load_f16`/`store_f16` stay unhandled |
| references/GC, exceptions, continuations, atomics | function left without `fnPtr` | unchanged from P0–P2 |

## 8. Sign-off

- Slice: P3 scalar floating point (f32/f64) in both backends.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `[sljit]` 1093 assertions in 15 cases, `[asmjit]` 806 in
  22, the float A/B case 101 assertions, the full suite 19928 assertions in 672
  cases, and Doxygen exit 0 with no warnings. (Counts at this slice's sign-off;
  the later memory64 slice added cases.)
