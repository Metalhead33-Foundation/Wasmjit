# Post-mortem: the reference slice 1 (`ref.null`, `ref.is_null`, `ref.eq`)

**Snapshot (2026-11-05, working tree on `backendsMh33`). Both backends landed in
  the same session.**

Scope: the first non-SIMD feature after the v128 series — three reference ops,
  one word each, no signature support required because every fixture keeps the
  reference on the operand stack.

**Chunk 2:** `ref.as_non_null` with its null-reference trap.
**Chunk 3:** `ref.func` (a runtime resolver on SLJIT, a code address on
AsmJit) and the non-null paths it unlocks.
**Chunk 4:** `br_on_null`.
**Chunk 5:** imported function references, with the declaration rule fixtured.
Out: `br_on_null`/`br_on_non_null`, GC types, `call_ref`, and a `ref.eq`
fixture (blocked by the type system, §entry 5).

## 0. Development log

### 2026-11-05 — entry 1: a reference is one word, and that is the whole model

- **`ref.null` pushes a zero word**, `ref.is_null` compares it against zero and
  produces an `i32`, `ref.eq` compares two words. Neither backend models *typed*
  nulls: `ref.null func` and `ref.null extern` are the same value, which is what
  `ref.is_null` needs and is honest about what `ref.eq` can see.
- **No signature work was needed.** A reference only reaches a local or a
  parameter through the deferred ABI work (see §2); here the value lives and dies
  on the operand stack, so the existing 8-byte slot machinery carried it.
- **Flag discipline again.** `ref.is_null` and `ref.eq` zero the result register
  *before* the comparing instruction and `setcc` immediately after — the same
  rule as `v128.any_true` and slice P3's comparison bug.

### 2026-11-05 — entry 2: the engine's eqref rule, found twice by validation

The fixture was rejected twice, both times by **validation**, not by a backend:

1. `ref.null extern` then `ref.eq` → `ref.eq requires eqref operands` (an
   `externref` is not an `eqref`);
2. `ref.null func` then `ref.eq` → the same error (a `funcref` is not an
   `eqref` either).

So an `eqref` cannot be constructed from a null alone in this engine; it needs
  `ref.func` (or a GC type). Consequence, recorded rather than papered over:
  **`ref.eq` is lowered in both backends but has no fixture assertion.** The
  fixture keeps `null_is_null` and `null_extern_is_null`, which do run.

### 2026-11-05 — entry 3: `ref.as_non_null`, and why `ref.func` is blocked

- **`ref.as_non_null` is a trap on a zero word.** SLJIT compares R0 against zero,
  calls a new `sljitReportNullReference` and branches to the trap epilogue;
  AsmJit tests the value, `jnz` past the trap code, invokes
  `asmjitReportNullReference` and uses the existing `emitPendingTrapReturn()`.
  Both helpers only call `WASM::reportTrap(TrapKind::NullReference)`; the op
  itself computes nothing, so the backend still owns no reference semantics.
- **`ref.func` cannot be lowered yet, for a structural reason.** A function
  reference must be a *runtime* pointer to the `WASM::Callable`, and only SLJIT
  has a resolver (`Sljit::ModuleCompiler::resolveCallable(vm, index)`). There is
  no shared entry point in `WasmBase/`; AsmJit's call path resolves direct calls
  through code labels, not through `Callable` pointers. Adding a shared resolver
  would be a change to the **embedding surface**, so it is a milestone decision
  (see `docs/API_ABI_STABILITY.md`), not a slice decision — which is why it is
  recorded here and in the closure table instead of being implemented on one
  backend only.
- **Consequence for coverage:** the success path of `ref.as_non_null`, the false
  branch of `ref.is_null`, and `ref.eq` on non-null references all need
  `ref.func`; today only the trap path is asserted. That is the honest state of
  the reference surface.

### 2026-11-05 — entry 4: `ref.func`, and why the two backends (rightly) differ

A previous draft of this note called `ref.func` blocked on a *shared* callable
resolver. That was the wrong instinct: the backends only owe the same
**observable behaviour**, not the same machine code. Each now does what is
natural — and cheapest — for its own toolkit:

- **SLJIT calls its resolver.** `ModuleCompiler::resolveCallable(vm, index)`
  returns the instance's `Callable*` at run time; the emitter is one context
  load, one index materialisation and one helper call. It works for internal and
  imported functions alike, because the resolver looks both up in the context.
- **AsmJit never calls anything.** An internal function reference is the address
  of the compiled body — `lea reg, [functions[i]->label()]`, one instruction and
  no memory traffic; an imported one is `mov reg, imm64` of the already-resolved
  `fnPtr` from `internals.importStorage`. AsmJit already knows both at emission
  time, so a runtime lookup would be strictly worse.

**The addresses differ between backends, deliberately.** Wasm can only compare a
reference and ask whether it is null, and both relations are identical across
the two choices (distinct functions have distinct bodies; the resolver returns
one pointer per index). Nothing user-visible depends on the value, so there is
no reason to make the cheaper backend pay for the other's representation.

### 2026-11-05 — entry 5: `ref.eq` is blocked by the type system, not by us

With `ref.func` available, `ref.eq` was the next candidate. The engine rejects
it every way this slice tried:

| Operands | Result |
|---|---|
| `ref.null extern` vs itself | rejected: not an `eqref` |
| `ref.null func` vs itself | rejected: not an `eqref` |
| `ref.null any` vs itself | rejected: `anyref` is a **supertype** of `eqref` |

`ref.eq` requires operands of type `(ref null eq)`, and the only way to build a
value of that type in this engine is a **GC** construction (`struct.new`,
`array.new`, …). So `ref.eq` is lowered in both backends, **unfixtured**, and the
blocker is the GC work in the closure table — not the callable resolver. The
earlier assumption (`ref.func` would unlock it) was wrong and is corrected here
rather than left to mislead the next session.

### 2026-11-05 — entry 6: `br_on_null` (and why `br_on_non_null` is not here)

- **`br_on_null` is a conditional branch plus a kept value.** The reference is
  popped conceptually — on the null path it branches to the label with nothing,
  on the non-null path it stays on the stack. SLJIT mirrors `br_if`'s shape: a
  compare whose *non-null* arm skips, an unconditional jump registered on the
  target frame's `endJumps` (or bound to the loop head). AsmJit uses
  `test` + `jz(label)`, and relies on its existing frame bookkeeping: it requires
  the label to carry **no results**, and the operand stack to be exactly
  `stackHeight + 1` so the kept reference cannot be mistaken for a branch value.
- **`br_on_non_null` needs a label that carries the reference**, i.e. a block
  with a parameter of reference type. AsmJit's structured control flow still
  supports no block parameters (a pre-existing gap, listed in the closure table),
  so implementing it on SLJIT alone would create a capability divergence for no
  gain. It stays refused by name in both backends until block parameters land.
- **A fixture lesson about AsmJit's block stack.** The first version of the
  `br_on_null` exports used `return` inside the block; AsmJit then reported
  `structured block leaves an unexpected operand stack shape` because the block's
  fallthrough stack no longer matched its frame. The working shape uses a local
  and a fallthrough assignment, which is also the better test: the null path
  yields `0`, the non-null path `7`, and both are asserted.

### 2026-11-05 — entry 7: imported function references, and the declaration rule

Both backends carry a path for `ref.func` on an **imported** index — SLJIT's
  resolver looks it up in the context, AsmJit emits `mov reg, imm64` of the
  already-resolved `fnPtr` from `importStorage` — and neither was exercised. A
  new fixture closes that: it imports `(func $imp (result i32))`, references it
  with `ref.func`, checks `ref.is_null` is `0`, and calls it to prove the import
  itself is live (`42`). Registered with `imports.registerFunction("host", "f",
  callable)` where the native is `int32_t f(WASM::VMContext*)` — the context is
  the first parameter even when it is unused, which is the convention the
  existing native-import tests use.

**The engine enforces the spec's declaration rule**: `ref.func $imp` failed
  validation with `undeclared function reference 0 ($imp)` until the fixture
  gained `(elem declare func $imp)`. That is worth recording because the earlier
  `ref.func` fixture only referenced *exported* functions, which are declared by
  their export — so the rule was invisible until an import was referenced.

### 2026-11-05 — entry 8: a negative result — the SLJIT parameter bound

SLJIT refuses functions with more than three parameters. The bound is a single
  constant (`kMaxWasmParameters = 3`) and the argument types are built by hand
  with `SLJIT_ARG_VALUE`, so it looked like a cheap win: raise it to five and add
  a fixture. **It crashed the process** (`SIGILL`, 2 assertions in, before any
  assertion failed). The likely cause is the saved-register side: the prologue
  keeps the context in `S0` and one saved register per parameter, so five
  parameters need six saved registers plus the separate float count, and the
  toolkit does not take that naively — the prologue either succeeds with an
  invalid mapping or the encoding truncates.

Reverted in full: constant, messages, fixture and the two test cases. The suite
  is back to its pre-attempt count.

**Second attempt, same day — and the saved-register count does not explain it.**
  `SLJIT_NUMBER_OF_SAVED_REGISTERS` is **6** on Linux x86-64 (`S0..S5`), so the
  context plus five parameters is exactly the ceiling and four should be
  comfortable. Retried with an **SLJIT-only** test so AsmJit could not be the
  culprit: five parameters crashed, and **four parameters crashed as well**, both
  with `SIGILL` right after the session's first assertions. So the bound of
  three is *not* explained by the saved-register file, and the fault is inside
  SLJIT's own prologue/epilogue for those shapes.

**What the next attempt must do:** instrument `sljit_emit_enter`'s return value
  and its `saveds`/`arg_types` arguments, and check whether the call path's
  `SLJIT_ARG_VALUE(..., i + 2)` encoding is at fault, before touching the
  constant again. Until then the three-parameter refusal stands and is the safe
  behaviour: it refuses, it does not miscompile. The item is recorded in the closure table as
  **attempted-and-reverted** so nobody spends the same hour twice; the next
  attempt should start by checking `SLJIT_NUMBER_OF_SAVED_REGISTERS` against
  `1 + parameterCount` and by reading what `sljit_emit_enter` actually validates,
  rather than by raising the constant.

## 1. What the shared ABI forced

Nothing — no helper is involved. The reference ops are pure register/slot work,
  which is why they were the right non-SIMD slice to take while the `v128`
  signature seam is deferred.

## 2. **Deferred decision — must be revisited before this saga can close**

The `v128` function-boundary work was analysed and **deliberately deferred**; the
  analysis is [`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md). It is
  not a footnote: it blocks closure of the AsmJit/SLJIT saga, because three
  documented surfaces still refuse it.

**The saga cannot be closed, and the final saga post-mortem cannot be written,
  until each item below is either implemented or accepted in writing as a
  permanent refusal:**

| Item | State | Why it blocks closure |
|---|---|---|
| `v128` parameters | **SLJIT landed** (two argument slots); AsmJit pending | the AsmJit half needs two `kUInt64` signature entries per parameter and the two-slot index mapping |
| `v128` results | **SLJIT permanently refused** (LibJIT returns it directly via a struct type; SLJIT has neither aggregates nor RAX:RDX); AsmJit pending two return registers | see [`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md) §7 and §9 |
| Wasm→Wasm calls with `v128` | deferred | SLJIT's `SLJIT_ARGS*` are fixed-arity; AsmJit's per-type signature cache needs the two-slot form |
| `ref.eq` fixture | lowered, unfixtured | needs a `(ref null eq)` value, which only **GC construction** can produce |
| `br_on_non_null` | `onUnhandledOpcode` | needs a label carrying the reference, i.e. **block parameters** (AsmJit gap) |
| GC types and barriers | not started | the P4 gate in `WRITING_A_BACKEND.md` §6 |
| exceptions | not started | P4 |
| `call_ref`, continuations | not started | P5 |
| atomics, `f16`, relaxed SIMD, bulk memory on a memory64 | refused by name | proposal-level, still open |
| SLJIT: >3 parameters | refused by name | **two attempts, both crash** (entry 8): five parameters and even four `SIGILL` with an SLJIT-only test; the saved-register count does not explain it |
| AsmJit: multi-value block results; block parameters | refused by name | pre-existing capability gaps that predate this saga; also blocks `br_on_non_null` |

**Closure rule for whoever writes the final saga post-mortem:** every row above
  must appear in it with one of two verdicts — *implemented, with the test that
  proves it* or *permanently refused, with the toolkit limitation named*. A row
  left implicit is a reason not to write the final note yet.

## 3. What the toolkit could not express

Nothing in this slice. The next reference ops need two things the backends do
  not have yet: a callable-resolver call at the `ref.func` site (SLJIT already
  has `ModuleCompiler::resolveCallable`; AsmJit has the equivalent in its call
  path), and — for anything carrying a reference through a signature — the
  deferred ABI work.

## 4. What the tests did not catch

- **`ref.eq` is unasserted** (entry 5) — the one op in the surface with no
  coverage at all, and it will stay that way until GC values exist.
- **Reference identity across a boundary is untested**: the fixture proves an
  imported reference is non-null, not that `ref.func $imp` equals the reference
  another module holds for the same function.
- **Typed-null distinctions are not modelled**, so nothing asserts that
  `ref.null func` and `ref.null extern` coincide on both backends.
- **References in locals/parameters/blocks** are refused, not tested.

## 5. What was slower, and by how much

**Not measured.** Three word-sized operations; no helpers, no allocation.

## 6. What a fourth backend should copy, and what it must not

Copy: one-word references, the flag discipline, and refusal-by-name for anything
  that would carry a reference through a signature until the ABI work lands.

Do not copy: assuming `ref.eq` is testable with nulls — the engine requires
  `eqref` operands; and do not let a lowered-but-untested opcode fall out of the
  residual table.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `br_on_non_null` | `onUnhandledOpcode` | blocked on AsmJit block parameters |
| `ref.eq` | lowered, **unfixtured** | needs a GC-constructed `(ref null eq)` |
| `ref.eq` | lowered, **unfixtured** | needs an `eqref`; pairs with `ref.func` |
| GC types/structs/arrays, `call_ref`, continuations | refused by name | P4/P5 |
| exceptions | refused by name | P4 |
| `v128` signatures | refused by name | **deferred decision, must be revisited** (§2) |
| atomics, `f16`, relaxed SIMD, bulk memory on memory64 | refused by name | proposal-level |

## 8. Sign-off

- Chunks: (1) `ref.null`, `ref.is_null`, `ref.eq`; (2) `ref.as_non_null`;
  (3) `ref.func`; (4) `br_on_null`; (5) imported function references. 6 hooks
  per backend, with `ref.eq` lowered but unfixtured and `br_on_non_null` refused
  on AsmJit block parameters.
- Commit: working tree on `backendsMh33`.
- Commands and counts: `./Test/WasmJit '[ref]'` — 65 assertions in 4 cases; the
  full suite — 21062 assertions in 697 cases; `[headerdoc]` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- **Carried forward explicitly:** the deferred `v128` decision in §2. It must be
  resolved before the saga closes.
