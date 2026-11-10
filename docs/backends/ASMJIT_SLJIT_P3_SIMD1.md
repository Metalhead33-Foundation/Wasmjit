# AsmJit/SLJIT v128 slice 1 {#backend_p3_simd1}

*Post-mortem: transport, bitwise, `i32x4` lanes*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session, so the log records
the parallel development as it happened.**

Scope: the first `v128` slice — the value's representation and transport, plus
the operations the core's shared helper ABI already exposes: `v128.const`,
`v128.load`/`store`, `i32x4.splat`/`extract_lane`/`replace_lane`, `v128.not`,
`and`, `andnot`, `or`, `xor`, `bitselect`, `any_true`, and `v128` locals.
Deliberately out: `v128` in signatures or block types, lane arithmetic and
comparisons, the other lane widths and both float shapes, the extending/lane
loads and lane stores, `shuffle`/`swizzle`, and relaxed SIMD.

## 0. Development log

### 2026-11-05 — entry 1: SLJIT, one 16-byte frame cell

- **The slot model was widened instead of forked.** `kSlotBytes` went from
  `sizeof(sljit_sw)` to 16, so a scalar uses the low word of a cell and a
  `v128` uses the whole cell. Every offset, local, stack slot, block transfer
  and frame-size computation already flowed through that one constant, so the
  change is uniform; the price is that every SLJIT frame doubles.
- **A `v128` slot is addressed, not loaded.** Helpers take `const void*`, so
  `slotAddress()` wraps `sljit_get_local_base`. Scalars keep the existing
  `loadSlot`/`storeSlot`; the v128 paths read and write words directly.
- **Bug 1 (stack discipline): `i32x4.splat` pushed without consuming.** The
  result landed one depth above the scalar, so `store_load` passed the scalar
  `0x01020304` as a memory address (an out-of-bounds trap) and
  `local_roundtrip` read the stale cell. The fix is `pop(); push(16);` and then
  using the same depth for the scalar load and the result address.
- **Bug 2 (toolkit register discipline): `sljit_get_local_base` clobbers R0.**
  On x86 SLJIT emulates it as `sljit_emit_op2(SLJIT_ADD, dst, SP, IMM, offset)`,
  whose temporary is R0. The first version did
  `loadSlot(R0, x); slotAddress(R1, out)` and the helper received the slot
  address as `x` (`i32x4.splat(5)` extracted `0x7f50`). The rule is now: R0 is
  materialised **last** among the helper's address arguments. `v128.load` and
  `v128.store` avoid the problem entirely — they move words between the checked
  pointer and the slot's `SLJIT_MEM1(SLJIT_SP)` offset, so only one address is
  ever live.
- **Refusals were moved, not deleted.** `v128` parameters, results, block types
  and `select` are refused by name in the constructor/block signature, so the
  slot widening cannot silently accept a signature the ABI does not lower.

### 2026-11-05 — entry 2: AsmJit, a 16-byte stack buffer behind a Gp

- **The register value model survived unchanged.** A v128 value is the address
  of a 16-byte `new_stack(16, 16)` buffer in a Gp. Everything that already
  worked on Gps — the value stack, block transfer, `select` — keeps working,
  because selecting the address selects the vector. Every operation allocates a
  fresh buffer, so a value is immutable once produced and a loop that re-emits
  one buffer per iteration rewrites it before each use.
- **A `v128` local is a buffer plus a zeroing.** The constructor allocates the
  buffer, writes 16 zero bytes, and records the local's slot width (16) beside
  the register; `local.get` copies into a fresh buffer, `local.set` copies into
  the local's buffer, and `local.tee` copies and keeps the operand's buffer.
- **Bug 3 (API): `setcc` takes a `CondCode`, not an instruction id.**
  `compiler.set(asmjit::x86::Inst::kIdSetne, ...)` does not resolve;
  `asmjit::x86::CondCode::kNE` does. This is the same family as the P3
  `Inst::Id`/`x86::Inst::Id` mistake: the x86 namespace is not optional when
  naming an instruction-shaped constant.
- **The helpers are invoked exactly as declared in the core.** Unary and binary
  bitwise calls share one small emitter each; the splat/lane calls assemble
  their own `FuncSignature` because their scalar argument types differ.

## 1. What the shared ABI forced

The core already declares the `wasm_v128_*` entry points in
`WasmBase/wasm_simd_scalar.hpp` with `const void*`/`void*` parameters and scalar
results by value. That decided both designs: a backend is a transport, not an
implementation of SIMD semantics. Neither backend contains a lane loop, a
saturation rule or a NaN rule for this slice — those stay in the shared layer,
where the scalar oracle can disagree with the vector build. A fourth backend
inherits the same contract and only has to answer one question: where do 16
bytes live?

## 2. What broke, with the failing fixture

| Symptom | Cause | Fixture export |
|---|---|---|
| `0x7f50` instead of `5` | `i32x4.splat` pushed without popping; helper wrote the low cell | `splat_extract` |
| out-of-bounds memory access at 64 | the misaligned stack made the scalar the store address | `store_load` |
| `any_true(0) == 1` | the misaligned stack left the vector slot uninitialised | `any_true` |
| garbage from a `v128` local | the local received the misaligned value | `local_roundtrip` |
| `v128.load` read from a wild pointer | `slotAddress(R1)` clobbered the checked pointer in R0 | `store_load` |

The five failures were one stack-discipline bug and one register-discipline
bug; the fixture found both on its first run because each export ends in an
`i32` lane or a boolean, so no `v128` signature was needed to observe them.

## 3. What the toolkit could not express

- **SLJIT has no vector register in this lowering.** Everything goes through the
  helper ABI; there is no attempt at SLJIT's own SIMD support. The frame is also
  wider for every function, including scalar-only ones — the cost of one
  addressing rule for all slots.
- **AsmJit has no `v128` in `FuncSignature` here.** A vector parameter would have
  to be classified against the SysV eightbyte rules (a 16-byte struct of four
  `uint32` is two INTEGER eightbytes, not an Xmm), so the slice refuses `v128`
  signatures outright and tests only through `i32` results. Lowering the
  signature is a separate decision, not an accident of the value model.

## 4. What the tests did not catch

- **Alignment.** The shared helpers assemble lanes byte by byte, so an 8-byte
  aligned slot is safe today. Neither a 16-byte-aligned buffer in AsmJit nor a
  deliberately misaligned slot in SLJIT is tested; a future helper that uses an
  aligned vector load would need a test that pins the alignment.
- **The aliasing rule for AsmJit buffers.** It is argued in a comment (one
  buffer per emission, rewritten before use) but not asserted; a loop carrying a
  `v128` block parameter would be the test, and block types are still refused.
- **Frame growth.** The doubled SLJIT frame is not measured against
  `maxStackDepth_` limits or the allocation budget.
- **`select` on v128.** AsmJit would accept it on addresses; SLJIT refuses it.
  The A/B fixture therefore does not cover it, and the asymmetry is honest only
  because the typed form is refused by both.

## 5. What was slower, and by how much

**Not measured.** Every operation is a helper call in both backends; this slice
optimises for a shared, checkable contract, not for speed. A native lowering can
replace the helpers family by family without changing the representation.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the "16 bytes live somewhere addressable" representation, chosen per toolkit
  (SLJIT: a wider frame cell; AsmJit: a stack buffer behind a Gp);
- the shared helper ABI as the *only* source of SIMD semantics;
- the day-one fixture whose exports all end in a scalar, so the first slice
  needs no vector signature;
- the refusal-by-name for `v128` signatures and block types, so widening a slot
  cannot silently widen the ABI.

Do not copy:

- `push()` without a matching `pop()` when a unary operation changes the value's
  width — the stack depth is the only thing keeping operands aligned;
- treating `sljit_get_local_base` as register-neutral on x86; materialise R0
  last;
- `setcc` with an instruction id instead of a condition code.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| `v128` parameters/results, block types | function left without `fnPtr`, or an unsupported-body report | **deferred decision**: [`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md) |
| `select`/`select_t` on v128 | **landed** — two words on SLJIT, the value address on AsmJit | `select_v128.wat` |
| lane arithmetic/comparisons | **integer families landed in slices 2 and 3** ([`ASMJIT_SLJIT_P3_SIMD2.md`](ASMJIT_SLJIT_P3_SIMD2.md), [`ASMJIT_SLJIT_P3_SIMD3.md`](ASMJIT_SLJIT_P3_SIMD3.md)); float lanes are still unhandled | `simd_lanes.wat`, `simd_int2.wat` |
| `i8x16`/`i16x8`/`i64x2`/`f32x4`/`f64x2` lane access | `onUnhandledOpcode` | the next SIMD slice; slice 2 observes lanes through `i32x4` only |
| extending/splat/lane loads, lane stores, `shuffle`/`swizzle` | `onUnhandledOpcode` | the helpers exist; the lowering is next |
| relaxed SIMD | `onUnhandledOpcode` | not in scope |
| `f16`, references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 1 (transport, bitwise, `i32x4` lane access, `v128` locals) in
  both backends.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 154 assertions in 5 cases
  (SLJIT execution, the AsmJit/SLJIT A/B case, and the three pre-existing
  `[simd]` cases); the full suite — 20163 assertions in 679 cases;
  `./Test/WasmJit '[headerdoc]'` — 6 assertions in 3 cases; `doxygen Doxyfile`
  exit 0 with no warnings.
- The "unsupported" fixture was **not** moved this time: `v128` is no longer the
  next proposal to land, and the pinned refusal stays on the features that are
  still refused by name.
