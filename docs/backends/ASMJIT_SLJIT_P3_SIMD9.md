# Post-mortem: the v128 slice 9 (`i8x16.shuffle`, the five-argument helper)

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: `i8x16.shuffle` — one opcode, but the only helper in the SIMD ABI with
five parameters and two 64-bit immediate selectors. With it the two backends
lower **every non-relaxed SIMD opcode the core exposes**, apart from `v128` in
function signatures, block types and `select_t`.

## 0. Development log

### 2026-11-05 — entry 1: SLJIT cannot pass five arguments

`wasm_i8x16_shuffle(a, b, selLo, selHi, out)` has five parameters, and SLJIT's
call ABI stops at four: `SLJIT_ARGS4` is the last macro, so there is no
five-argument `sljit_emit_icall` and no fifth argument register to use.

Two shapes were possible; one was chosen:

- **Rejected: raise the scratch count and pass R4 as a raw argument.** There is
  no encoding for it in the ABI macro, so this would mean hand-building
  `arg_types` — fragile and still undocumented by the toolkit.
- **Chosen: a forwarding adapter with four arguments.** The two selector words
  are written to a **selector block** in two newly reserved frame cells, and the
  adapter is

  ```c
  extern "C" void sljitShuffleAdapter(const void* a, const void* b,
          const uint64_t* selectors, void* out)
  {
      wasm_i8x16_shuffle(a, b, selectors[0], selectors[1], out);
  }
  ```

  The adapter contains **no semantics** — it re-shapes the argument list and
  nothing else, so the shared helper remains the single definition of the
  operation. That distinction is the layering rule: a backend may move values
  into the shape a helper expects, and may not decide what the helper computes.

- **A shuffle cannot write in place.** Unlike every other binary op in these
  slices, a shuffle may read *any* byte of either operand, so the result cannot
  be written into `a`'s cell while `a` is still being read. The emitter pushes a
  scratch cell, writes there, and copies the 16 bytes down afterwards.

### 2026-11-05 — entry 2: a frame-layout change is a whole-backend change

Reserving two more cells (`kReservedSlots` 3 → 5) shifts **every** stack slot in
**every** SLJIT function by 32 bytes. Nothing in the tagged `[sljit]` suite
would have noticed a mistake in the *other* direction (a slot that overlapped
something it should not have) unless the whole suite ran — which is why the gate
for this slice was `./Test/WasmJit`, not `[simd]`. The full run covered it.

### 2026-11-05 — entry 3: I overwrote a tracked fixture, and only the full suite said so

Creating `Test/wasm_wat/simd_shuffle.wat` **replaced a fixture that was already
tracked**: an existing backend-agnostic test, `simd: shuffle and swizzle run
through the JIT`, loads a resource with that name and expects
`shuffle_u8_0`…`swizzle_u8_6`. My file had four different exports, so two
`REQUIRE`s failed.

That the failure surfaced at all is the point: the test carries **no**
`[sljit]`/`[asmjit]` tag (it runs against the default backend, i.e. LibJIT), so
`[simd]`, `[sljit]` and `[asmjit]` were all green while the full suite was red.
Recovery was `git checkout --` on the fixture plus a new name
(`simd_shuffle_imm.wat`).

Two process rules, both now part of the slice checklist:

- **check `git status` for the fixture directory before adding a `.wat` file** —
  the loader resolves fixtures by base name, so a duplicate name silently
  substitutes one fixture for another;
- **run the full suite, not the tags**, before calling a slice done.

### 2026-11-05 — entry 4: the case that matters, and a fixture error in it

The first version of the two-shuffles-in-one-function export failed WAT
validation (`values remaining at end`) because the first result was never
`drop`ped. With that fixed, the case does what it was written for: it proves the
selector block is **rewritten per shuffle**, so two shuffles with different
selectors in one body do not share stale selectors.

## 1. What the shared ABI forced

- **Five parameters, two of them 64-bit immediates.** AsmJit takes the whole
  signature directly (`kUIntPtr, kUIntPtr, kUInt64, kUInt64, kUIntPtr`); SLJIT
  needs the adapter in §0.1. The *operation* is unchanged on both sides.
- **The selectors are immediates, not operands.** They are compile-time bytes in
  the instruction stream, so neither backend reads them from the Wasm stack.
  AsmJit passes them as `Imm`; SLJIT stages them in the frame because it has no
  way to pass two 64-bit immediates alongside three pointers.

## 2. What broke, with the failing fixture

| Symptom | Cause | Fixture |
|---|---|---|
| two `REQUIRE` failures outside every tag | a new fixture **overwrote** a tracked one of the same name (§0.3) | existing `simd_shuffle` test |
| `type mismatch: values remaining at end` | the two-shuffle export forgot to `drop` the first result | `shuffle_twice` |

No lowering defect was found: both backends produced the expected bytes on the
first run of the corrected fixture.

## 3. What the toolkit could not express

- **SLJIT: a five-argument call.** Handled by the adapter; the alternative (a
  hand-built `arg_types` with a fifth register) was rejected as undocumented
  territory.
- **SLJIT: an in-place shuffle.** Handled by the scratch cell plus a 16-byte
  copy; the cost is one extra cell and four moves per shuffle.
- **AsmJit: nothing.** The node-based emitter takes the five-argument signature
  as-is.

## 4. What the tests did not catch

- **Selector patterns that read one byte repeatedly** (`0 0 0 0 …`) are covered;
  a pattern that mixes both operands *in reverse block order* is not.
- **More than two shuffles in one function** — two is covered, which pins the
  block rewrite, but a longer sequence would also be worth one assertion.
- **The frame cost** of the two reserved cells (32 bytes per SLJIT function) is
  not measured against the allocation budget.
- **Alignment**, as in every slice since 1.

## 5. What was slower, and by how much

**Not measured.** SLJIT pays two immediate stores, one `lea`, one 16-byte copy
and 32 bytes of frame per shuffle; AsmJit pays the call.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the adapter pattern when a helper has more parameters than the toolkit's call
  ABI supports: forward, never re-implement;
- a **distinct** result cell for any operation whose reads and writes may
  overlap (shuffle is the first such op in this series);
- the selector block in reserved frame cells, rewritten at every use;
- the two-shuffle fixture case;
- the full-suite gate whenever a frame layout or prologue changes.

Do not copy:

- hand-building `arg_types` to smuggle an extra argument;
- writing a shuffle result into one of its inputs;
- a new fixture name without checking that the loader's resource name is not
  already taken.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| relaxed SIMD | `onUnhandledOpcode` | helpers exist; not scheduled |
| `v128` parameters/results, block types, `select_t` | refused by name | **the last P3 item**; an ABI decision, not a lowering |
| `f16` load/store and the `f16` proposal | `onUnhandledOpcode` | not started |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 9 (`i8x16.shuffle`) in both backends, with an SLJIT
  forwarding adapter and a two-cell selector block.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 988 assertions in 19 cases;
  `[sljit]` 2112 in 34, `[asmjit]` 1355 in 33; the full suite — 20997
  assertions in 693 cases (including the previously-collided
  `simd: shuffle and swizzle run through the JIT`);
  `./Test/WasmJit '[headerdoc]'` — green; `doxygen Doxyfile` — exit 0 with no
  warnings.
