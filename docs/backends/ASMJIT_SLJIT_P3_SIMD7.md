# Post-mortem: the v128 slice 7 (float lane access)

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: `f32x4.splat`/`extract_lane`/`replace_lane` and the same three for
`f64x2` — six hooks, but the first ops whose helpers take a **float argument**
or **return one**. Every earlier SIMD helper passed pointers and integers only.

## 0. Development log

### 2026-11-05 — entry 1: the argument-class rule, not the argument index

- **A mixed signature numbers each register class separately.** For
  `wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out)`
  the integer arguments are numbered among themselves: `a → R0`, `lane → R1`,
  `out → R2`, while the float travels in `FR0`. The obvious mistake — reading
  the signature left to right and putting `out` in `R3` — is why the emitter
  says so in a comment. SLJIT's ABI macro encodes the class per argument, so
  `SLJIT_ARGS4V(P, 32, F32, P)` is correct and `out` is indeed R2.
- **Three new shapes, one per operation,** all small variations on the
  integer lane access from slice 1:
  `(F32/F64, P)` for splat, `(F32/F64, P, 32)` returning a float for extract,
  and `(P, 32, F32/F64, P)` for replace. `loadFloatSlot`/`storeFloatSlot`
  already existed from the scalar float slice, so no new slot machinery.

### 2026-11-05 — entry 2: AsmJit's value model makes materialisation the whole job

- **AsmJit carries a float as its bit pattern in a Gp** (the P3 model). So the
  three operations are conversions around the call: `materializeFloat` before
  the invoke for splat/replace, and `bitsOfFloat` after it for extract.
- **The P3 ordering bug applies here and is avoided by construction.** An
  instruction created after `compiler.invoke(...)` runs *after* the call, so
  `invoke->set_arg(0, materializeFloat(...))` would hand the helper an
  uninitialised Xmm — exactly the bug slice P3 recorded. Both helpers call
  `materializeFloat` into a local **before** appending the invoke node, and say
  so in a comment. The extract side needed `set_ret(0, xmm)` followed by
  `bitsOfFloat`, which is the same pattern the scalar float calls already use.
- **The scalar width is the only difference between the f32 and f64 paths**
  (4 vs 8, `kFloat32` vs `kFloat64`), so each operation is one helper taking a
  width rather than six near-identical bodies.

### 2026-11-05 — entry 3: observing floats without trusting float comparison

The fixture returns floats (the scalar ABI is fine; only `v128` signatures are
refused), but the tests do not stop at `==`:

- `f32x4_extract_negzero_bits` returns the **bits** of an extracted `-0.0`
  (`0x80000000`), because `-0.0 == 0.0` would pass a value comparison.
- `f32x4_splat_bits` returns the bits of a splatted `1.5` (`0x3FC00000`), which
  also checks that splat replicates the pattern rather than a rounded value.
- The A/B case compares the two backends' float returns with `memcmp`, not
  `==`, so a sign-of-zero or NaN-payload divergence cannot hide behind
  equality.

The slice was green on the first run.

## 1. What the shared ABI forced

Two things, both already in the contract rather than added for this slice:

- **scalar results by value** — extract returns `float`/`double`, so each
  backend's float-return path (FR0 / Xmm0) is reused;
- **bit-exact scalar arguments** — splat and replace take the value whose IEEE
  pattern is already in the frame/register, with no conversion in the backend.

The backend still owns no float semantics: it does not interpret the value at
all, it only moves 4 or 8 bytes into or out of the helper's float register.

## 2. What broke, with the failing fixture

Nothing. The three two lines below are the failure modes this slice was shaped
to avoid, and how:

| Failure mode | Avoided by |
|---|---|
| `out` in R3 because it is the fourth argument | the per-class numbering rule, stated in the emitter and exercised by `replace` |
| an Xmm argument defined *after* the invoke node | `materializeFloat` before `compiler.invoke`, as P3 requires |
| a float comparison hiding `-0.0` | the `*_bits` exports and `memcmp` in the A/B case |

## 3. What the toolkit could not express

- **Nothing new.** SLJIT had float slot load/store and float argument/return
  classes; AsmJit had `materializeFloat`/`bitsOfFloat` and Xmm results from the
  scalar float slice. The slice reuses both.

## 4. What the tests did not catch

- **NaN payload preservation.** `splat(NaN)` then `extract` should preserve the
  payload; the fixture never puts a NaN through lane access (only through a
  comparison in slice 6).
- **Lane index extremes.** The fixture uses lanes 1, 2 and 3; lane 0 and the
  last lane of each shape are covered incidentally, but no out-of-range or
  boundary-heavy pattern is.
- **A replaced lane leaving its neighbours untouched** beyond one neighbour in
  `f32x4_replace_neighbour`; `f64x2` has the same check on lane 1 only.
- **Replace on a vector produced by a helper** (the fixture always replaces in a
  `v128.const`); it would exercise out-buffer aliasing rather than a fresh
  constant buffer.
- **Denormals and infinities** through splat/extract.
- **Alignment**, as in every slice since 1.

## 5. What was slower, and by how much

**Not measured.** Extract and replace each move one Xmm↔Gp pair in AsmJit; the
SLJIT path uses its existing float slot moves. Six helper calls in total.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the per-class argument numbering rule for mixed signatures, written next to
  the emitter;
- materialising a float argument **before** appending the call node, and moving
  a float result back into the value model afterwards;
- at least one bit-level assertion (`-0.0`, a splat pattern) and a `memcmp`
  A/B comparison for floats;
- one width-parameterised helper per operation instead of an f32/f64 pair.

Do not copy:

- reading a mixed signature left to right and numbering all arguments
  together;
- comparing float results with `==` when the question is bit-exactness;
- assuming a float lane extract is "just another integer extract" — it changes
  which register class the value lives in.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers exist; next slice |
| `i8x16.shuffle`/`swizzle`, relaxed SIMD | `onUnhandledOpcode` | transport-only helpers exist |
| `v128` parameters/results, block types, `select_t` | refused by name | the remaining P3 item; an ABI decision |
| `f16` load/store and the `f16` proposal | `onUnhandledOpcode` | not started |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

With this slice the **whole scalar-lane SIMD surface** — integer and float
lane access, arithmetic, comparisons, conversions — is lowered in both
backends. What remains is the transport families and vector signatures.

## 8. Sign-off

- Slice: v128 slice 7 (float lane access: `f32x4`/`f64x2`
  `splat`/`extract_lane`/`replace_lane`) in both backends; six hooks each.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 800 assertions in 15 cases;
  `[sljit]` 1924 in 30, `[asmjit]` 1261 in 31; the full suite — 20809
  assertions in 689 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
