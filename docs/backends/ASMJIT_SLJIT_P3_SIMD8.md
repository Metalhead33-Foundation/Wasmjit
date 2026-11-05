# Post-mortem: the v128 slice 8 (transport: extending/splat/zero loads, lane loads, lane stores, swizzle)

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: the transport families whose helpers already exist — six extending loads
(`load8x8_s/u`, `load16x4_s/u`, `load32x2_s/u`), four splat loads
(`load8/16/32/64_splat`), two zero loads (`load32_zero`, `load64_zero`), four
lane loads, four lane stores and `i8x16.swizzle`: **21 hooks per backend**.
`i8x16.shuffle` is deferred, with the reason in §3.

## 0. Development log

### 2026-11-05 — entry 1: three emitters, one spill discipline

- **Every load here consumes an address and produces a vector**, so the result
  takes the address cell — the same shape `v128.load` already had. `swizzle` is
  an ordinary binary op and reused `emitV128Binary`/`emitVectorHelper2`
  unchanged.
- **Two pointers must be live for lane loads and stores.** The checked-address
  helper ends with the pointer in R0, but materialising the vector's address
  (`slotAddress`) clobbers R0 on x86. The fix is the slice-1 discipline extended
  by one step: park the pointer in the **reserved scratch slot**, materialise
  the addresses, then reload R0 last. Three emitters now share that pattern.
- **AsmJit needed no new idea at all**: pop the operands, compute the checked
  pointer, allocate a result buffer, invoke with `(pointer, vector, lane,
  buffer)`. The only new thing is a fourth `FuncSignature` shape.

### 2026-11-05 — entry 2: the helper was right; the stack bookkeeping was not

All six extending/splat/zero loads and all four lane stores passed on the first
run. All four **lane loads** failed, and the values were the giveaway: lane 1
read `0x00007ffc` — the stale upper half of a 4-byte cell.

The isolation path was short because the direct probe was reused from slice 5:
a temporary `extern "C"` wrapper printed the four arguments and showed

```
lane src=0x55e19d6ae170 a=0x7ffc75bce5a0 lane=1 out=0x7ffc75bce5a0
```

— the helper received exactly the right pointers and lane. So the defect was my
**stack collapse**: the operand stack is `[address, vector]`, and after a lane
load the result must live in the **address** cell (the pair becomes one entry
there). The first version instead passed the *vector* cell as `out` and then
`pop()`ped, which removed the vector entry and left the 4-byte address cell as
the top of the stack — so `i32x4.extract_lane` read the address cell, including
its stale upper half. The fix: `out` is the address cell, the vector stays the
`a` operand, and the pair collapses with `pop(); pop(); push(16);`.

Two lessons, both recorded:

- **a proven-correct helper means the bug is in the lowering** — the probe
  turned a 30-minute guess into a 2-minute read;
- **"pop the result" is wrong whenever an operation consumes two entries and
  produces one**: the surviving entry must be the one whose cell the helper
  wrote, and the widths must be updated to match.

### 2026-11-05 — entry 3: a fourth fixture-input mistake, and the rule that fixes it

After the lowering was correct, one assertion still failed:
`load32_lane` returned `-1` instead of `0xDEADBEEF`. The fixture had stored
`-1` — copy-paste from the signed/unsigned extension cases above it. The
expectation was right; the *input* was wrong.

This is the fourth slice where a fixture value, not a backend, was the defect
(slices 2, 4 and 6 were lane-map mistakes). The rule this one adds: when a
fixture helper takes its memory value as an argument, give **each export a
distinctive constant** (`0xDEADBEEF`, `0xBEEF`, `0x42`) so a copy-paste is
visible on inspection instead of hiding behind a plausible value.

## 1. What the shared ABI forced

Two shapes, both already declared:

- `(const void* src, void* out)` / `(void* dst, const void* a, uint32_t lane)` /
  `(const void* src, const void* a, uint32_t lane, void* out)` — pointer-and-lane
  signatures the backends can satisfy with the transports they already know;
- **the access width is the backend's business.** The helper does not check
  bounds; the backend passes the *number of bytes the helper reads or writes*
  (1/2/4/8) to its checked-address path. That is why the twelve loads carry a
  width in the dispatch table rather than deriving it from the opcode name.

`i8x16.swizzle` is the first transport op that is pure data movement with two
vector inputs, so it needed no transport emitter at all.

## 2. What broke, with the failing fixture

| Symptom | Cause | Fixture export |
|---|---|---|
| lane 1 read `0x00007ffc` | result written to the vector cell but the stack collapsed to the address cell | the four `*_lane` loads |
| `load32_lane` returned `-1` | the fixture stored `-1`, not the pattern | `load32_lane` |

The probe output quoted in entry 2 is kept here because it is the cheapest
possible disambiguation: four printed arguments separating "wrong call" from
"wrong bookkeeping".

## 3. What the toolkit could not express

- **`i8x16.shuffle` needs five arguments.** `wasm_i8x16_shuffle(a, b, selLo,
  selHi, out)` is `(P, P, W, W, P)`; the prologue currently declares four
  scratch registers, so the emitter would need a fifth argument register (R4)
  and a scratch-count decision that belongs with the function-entry code, not
  with a table row. Deferred deliberately, not forgotten: the helper exists and
  the lowering is a two-line dispatcher once the prologue reserves the extra
  register.
- **Nothing else.** The other 21 hooks fit the existing emitters.

## 4. What the tests did not catch

- **Memory offsets on the transport ops.** Every export uses `offset=0`; the
  `MemArg` offset path is shared with the scalar loads (well covered there) but
  not exercised here.
- **Out-of-bounds traps per width.** No export probes the edge for a 1-byte or
  8-byte access; the checked-address helper is tested elsewhere, but the
  *width* the transport emitter passes is not pinned by a trap test.
- **Lane stores at the last lane** of each shape, and lane loads with a
  non-zero memarg offset.
- **`swizzle` with indices in `0..15` that permute** (the fixture uses index 0
  and 16 only, i.e. identity and zero).
- **memory64 index types** for the transport families.
- **Alignment**, as in every slice since 1.

## 5. What was slower, and by how much

**Not measured.** 21 helper calls plus, on SLJIT only, one store/load pair for
  the pointer spill on lane loads and stores.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the spill discipline for two live pointers: park the checked pointer in a
  reserved slot, materialise addresses, reload last;
- "the result cell is the cell the operand pair collapses to" for
  load-with-address operations;
- passing the helper's **access width** to the bounds check explicitly;
- distinctive constants per fixture export.

Do not copy:

- `pop()` as a way to finish an operation that consumed two entries and
  produced one;
- deriving an access width from an opcode name;
- assuming a five-argument helper fits a prologue that reserves four scratch
  registers.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `i8x16.shuffle` | `onUnhandledOpcode` | needs a fifth argument register and a scratch-count decision (§3) |
| relaxed SIMD | `onUnhandledOpcode` | helpers exist; not scheduled |
| `v128` parameters/results, block types, `select_t` | refused by name | the remaining P3 item; an ABI decision |
| `f16` load/store and the `f16` proposal | `onUnhandledOpcode` | not started |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 8 (extending/splat/zero loads, lane loads, lane stores,
  `i8x16.swizzle`) in both backends; 21 hooks each, `i8x16.shuffle` deferred.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 954 assertions in 17 cases;
  `[sljit]` 2078 in 32, `[asmjit]` 1338 in 32; the full suite — 20963
  assertions in 691 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
