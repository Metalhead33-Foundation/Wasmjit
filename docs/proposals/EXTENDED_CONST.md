# Extended Constant Expressions — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmModuleInstance::evalConstantExpr`, `WasmType`,
`WasmModule`, or the test harness.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the proposal require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

Extended Constant Expressions is a **Phase 5** proposal, integrated into
WebAssembly **3.0**.

- **Arithmetic in `const` expressions**: `i32.add`, `i32.sub`, `i32.mul`,
  `i64.add`, `i64.sub`, `i64.mul` become legal constant instructions. So
  `(global i32 (i32.add (i32.const 1) (i32.const 2)))` is valid.
- **`global.get` of an imported global** feeding the new arithmetic.
- The extensions apply wherever a `const` expression is evaluated: global
  initialisers, element/data segment offsets, table initialisers, etc.
- The GC proposal **further** extended the constant instruction set with
  `ref.i31`, `struct.new`, `struct.new_default`, `array.new`,
  `array.new_default`, `array.new_fixed`, `any.convert_extern`,
  `extern.convert_any` (already covered by [`GC.md`](GC.md)).

## 3. What the proposal does not demand

- **No evaluation-time requirement.** An implementation may fold the
  expression at instantiation, at compile time, or lazily, as long as the
  result is correct and any trap is observed at the same point.
- **No general constant folding.** Only the listed instructions are legal; the
  engine is free to not implement a general evaluator.
- **No representation requirement** for the resulting value.
- **No requirement to support control flow or locals** in `const` expressions
  (still forbidden).
- **Traps stay the same**: the arithmetic is normal wrap-around integer
  arithmetic, with no new failure modes.

## 4. What we have implemented today

`ModuleInstance::evalConstantExpr` is a small **stack machine** over the raw
bytes of a `const` expression (`WasmModuleInstance.cpp`). It currently handles:

| Opcode | Instruction | Status |
|---|---|---|
| `0x41` / `0x42` / `0x43` / `0x44` | `i32/i64/f32/f64.const` | ✅ |
| `0x23` | `global.get` | ✅ (reads `globalsStorage`) |
| `0xD0` | `ref.null` | ✅ |
| `0xD2` | `ref.func` | ✅ sentinel, resolved by segment application |
| `0xFB …` | GC constructors (`ref.i31`, `struct.new*`, `array.new*`, `any/extern.convert_*`) | ✅ (`GC.md`) |
| `0x6A` / `0x6B` / `0x6C` | `i32.add` / `i32.sub` / `i32.mul` | ❌ **missing** |
| `0x7C` / `0x7D` / `0x7E` | `i64.add` / `i64.sub` / `i64.mul` | ❌ **missing** |
| `0xFD …` | `v128.const` | ❌ (`SIMD.md` S3) |

Everything else throws `InvalidOpcodeException(opcode)`. The evaluator is used
by `initializeGlobals`, `applyActiveSegments`, `bufferInitFromData/Elems`, and
`tableInit`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| EC1 | **The six arithmetic extensions are missing.** `i32/i64.add/sub/mul` in a `const` expression throw `InvalidOpcodeException`, so a valid 3.0 module fails to load. | `evalConstantExpr` switch | High |
| EC2 | **`global.get` of an imported global yields zero.** Imported globals are never resolved (`docs/GC.md` G6 / [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md)), so any `const` expression reading one gets the zero-initialised slot. | `initializeGlobals` | High |
| EC3 | **`v128.const` is not a constant instruction here.** | `evalConstantExpr`; [`SIMD.md`](SIMD.md) S3 | Medium |
| EC4 | **No validation.** A structurally valid but type-invalid `const` expression (e.g. `i32.add` on `f32` bits) is evaluated as raw bits rather than rejected. | no validating front-end | Low |
| EC5 | **`Value::kind` tracking is shallow.** The evaluator sets `kind` per instruction but does not type-check operands; fine for valid modules, undefined for invalid ones. | `evalConstantExpr` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ECD1 | **Keep the byte-level stack machine and add the six arithmetic cases.** | §3: only the listed instructions are legal; a tiny evaluator is enough. | Minimal change, no expression parser. |
| ECD2 | **Evaluate eagerly at instantiation** (already done). | §3: evaluation time is free as long as traps happen at the same point; arithmetic does not trap. | Simple, deterministic. |
| ECD3 | **Compute in wrapped integer arithmetic** (`uint32_t`/`uint64_t` then reinterpret). | §2: the operations are the normal wrap-around ones. | No UB, no overflow handling. |
| ECD4 | **Leave `ref.func` as a sentinel** and resolve it in the segment path. | §3: representation free; the value is only a function reference. | Reuses the existing machinery. |
| ECD5 | **Do not implement a general constant folder.** | §3: not required. | Scope control. |

Guardrail: adding an opcode to the evaluator must keep the stack discipline
(one pop per operand, one push per result) and must not read past the `end`.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **EC-1 — Arithmetic** | Add `0x6A/0x6B/0x6C/0x7C/0x7D/0x7E` to `evalConstantExpr`, with wrapped arithmetic and `Value::kind` handling. | EC1 | A `(global i32 (i32.add (i32.const 1) (i32.const 2)))` resolves to 3. |
| **EC-2 — Imported globals** | Call the existing `ImportResolver::resolveGlobal` from instantiation (see [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md)). | EC2 | A `const` expression reading an imported global sees the exported value. |
| **EC-3 — `v128.const`** | Add the `0xFD` arm (16 bytes) once SIMD lands (SIMD-4). | EC3 | A `v128` global initialised by `v128.const` works. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | Result | Reading |
|---|---|---|
| `int_exprs` | curated | basic constant expressions work |
| `const` | failing (`-0.0` sign issue, `docs/TESTING.md`) | unrelated to this proposal |
| `extended-const.wast` | not present in this snapshot | no dedicated coverage |
| `global` | 10 passed / 63 failed | EC2: imported/global handling broadly broken |

## 9. References

- Proposal overview (`WebAssembly/extended-const`):
  https://github.com/WebAssembly/spec/tree/main/proposals/extended-const
- Spec — Change History (Release 3.0 "Extended Constant Expressions"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Validation (constant expressions):
  https://webassembly.github.io/spec/core/valid/instructions.html
- In-repo: [`docs/GC.md`](GC.md) §2.5, §4.3 (the GC `const` additions),
  [`docs/MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md), [`docs/SIMD.md`](SIMD.md) S3;
  sources `WasmBase/WasmType.hpp` (`Global`, `GlobalType`),
  `WasmBase/WasmModuleInstance.cpp` (`evalConstantExpr`,
  `initializeGlobals`, `applyActiveSegments`),
  `WasmBase/WasmOpcode.hpp` (`Opcode`).
