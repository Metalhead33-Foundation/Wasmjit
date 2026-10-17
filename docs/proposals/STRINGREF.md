# Reference-Typed Strings (`stringref`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — the proposal is
**not implemented**: only reserved value-type codes exist. `GC.md` §10 keeps the
encoding-level analysis; this note makes the proposal a first-class entry).
Audience: anyone touching `WasmType`, `WasmModule` (sections), `WasmOpcode`,
`LibjitOpcodeDispatcher`, the GC heap, or the host boundary.

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

Reference-Typed Strings is a post-3.0 proposal (it is outside the Wasm 3.0 GC
MVP; `GC.md` §10 places it last).

- One new reference type `stringref`, nullable and opaque (like `externref`).
  A null `stringref` traps on every string instruction except `string.eq`.
- A new **string table section** (id 14), `vec(vec(u8))`, present at most once,
  immediately before the global section (or where it would be). Its payload is
  preceded by a `0x00` placeholder for a future deferred form.
- `string.const` (an `i32` index into that table) is allowed in constant
  expressions.
- String construction: `string.new_utf8`, `string.new_lossy_utf8`,
  `string.new_wtf8`, `string.new_wtf16`.
- Measurement and encoding: `string.measure_utf8` / `_wtf8` / `_wtf16`,
  `string.encode_utf8` / `_lossy_utf8` / `_wtf8` / `_wtf16`.
- Concatenation, equality and validation: `string.concat`, `string.eq`, `string.is_usv_sequence`.
- String views: `string.as_wtf8`, `string.as_wtf16`, `string.as_iter`, plus the
  `stringview_wtf8.*`, `stringview_wtf16.*` and `stringview_iter.*` operations
  (advance, slice, length, get_codeunit, next, rewind, encode).

The binary encoding uses the `0xFB` prefix with sub-opcodes `0x80`–`0xB7`; the
value-type encodings are the ones listed in §5 (STR1).

## 3. What the proposal does not demand

- No internal string representation: WTF-8, WTF-16, JS strings and ropes are all
  allowed; eager copying is permitted.
- No particular time or space complexity (Wasm does not specify complexity).
- No JS API for `stringview_*` in the MVP; that is explicitly post-MVP.
- No GC requirement: a non-GC host may own strings and hand out references.
- No requirement that every opcode be efficient; only that it is correct.

## 4. What we have implemented today

- The value-type codes exist in `WasmBase/WasmType.hpp`:
  `StringRef = -0x19 (0x67)`, `StringViewWtf8 = -0x1A (0x66)`,
  `StringViewWtf16 = -0x1B (0x65)`, `StringViewIter = -0x1E (0x62)`.
- `isModeledValueTypeCode` returns **true** for all four, and
  `LibJit/LibJitTypeTranslation.cpp` lowers them to `jit_type_void_ptr`.
  A module may therefore declare a `stringref` parameter, result, local or
  global and still pass `Module::validateValueTypes()`.
- There is no string table section parser: `SectionType` has no id 14, and
  `Module::processSecetions()` skips an unknown section. `string.const` has no
  constant-expression path.
- There are no string or stringview opcodes. The `0xFB` prefix is decoded as
  `GCOpcode`, whose sub-opcodes start at 0; a `0x80` string sub-opcode reaches
  `dispatchPrefixGC`'s `default` and throws
  "Invalid or unsupported 0xFB GC sub-opcode".
- There is no host string object, no `string.eq` identity, and no producer of a
  `stringref` value anywhere in the engine.

`GC.md` §10.3 records the encoding analysis and `H5` tracks the deferral.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| STR1 | **The reserved value-type encodings are stale.** The engine uses `0x67`/`0x66`/`0x65`/`0x62`; the current proposal encodes `stringref` as `0x64`, `stringview_wtf8` as `0x63`, `stringview_wtf16` as `0x62` and `stringview_iter` as `0x61`. | `WasmType.hpp`; current `proposals/stringref/Overview.md` binary format | Medium |
| STR2 | **No string table section.** Section id 14 is not in `SectionType`, so the section is silently skipped and `string.const` has no source of literals. | `WasmSection.hpp`, `WasmModule.cpp` (`processSecetions`) | High |
| STR3 | **No string/stringview instructions.** All `0xFB 0x80`–`0xB7` sub-opcodes hit the GC dispatcher's default. | `WasmOpcode.hpp` (`GCOpcode`), `WasmOpcodeDispatcher.cpp` | High |
| STR4 | **Declared string types are accepted but unimplemented.** A `stringref` signature lowers to `void*`, and nothing ever produces a string, so a function returning `stringref` yields null/garbage instead of trapping or being rejected. | `isModeledValueTypeCode`; `LibJitTypeTranslation.cpp` | High |
| STR5 | **No host string bridge.** Zero-copy JS/WTF-8/WTF-16 interop, which is the proposal's first requirement, has no counterpart here. | no string object in `WasmValue.hpp` / `WasmStore` | Medium |
| STR6 | **No test coverage.** No `stringref` or `stringview` script exists in the testsuite snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| STRD1 | **Keep the four type codes as reserved placeholders.** | §3: representation is free; a code in an enum is not a support claim (`GC.md` §10.3). | No churn when the final encodings land. |
| STRD2 | **Reject a declared string type at load until the section and opcodes exist**, instead of compiling it as a null `void*`. | §3 permits a subset implementation; `SAFETY.md` forbids lowering an unsupported value to a null operand. | Honest failure at the module boundary (planned, see STR-0). |
| STRD3 | **Do not model host strings as wasm values.** A `stringref` is a handle owned by the embedder. | §3: representation and host API are free. | The engine need not implement a JS string runtime. |
| STRD4 | **Centralise the null rule** ("trap on every instruction except `string.eq`") in the helpers rather than in every dispatch arm. | §3: the lowering is free; §2's rule is uniform. | One check, no per-op duplication. |

Guardrail: `isModeledValueTypeCode` is a *lowering* statement, not a
conformance claim. Until STR-0 lands, a module declaring `stringref` must not be
described as supported.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **STR-0 — Honest rejection** | Until the feature exists, refuse a module that declares a string type (the same policy as an unmodelled value type). | STR4 | A `stringref` module fails with a named error, not a null result. |
| **STR-1 — Fix the encodings** | Align the four `ValueTypeCode` constants with the current proposal; add a decode round-trip test. | STR1 | A hand-built type section with `stringref` decodes and round-trips. |
| **STR-2 — String table** | Parse section 14, store the literals, and add `string.const` to `evalConstantExpr`. | STR2 | `(global stringref (string.const 0))` works. |
| **STR-3 — String core** | Implement `new_*`, `measure_*`, `encode_*`, `concat`, `eq`, `is_usv_sequence` over a host-owned string object with the null rule. | STR3, STR4, STR5 | The core instruction tests pass and `stringref` values survive a host round-trip. |
| **STR-4 — Views** | Implement `stringview_wtf8` / `_wtf16` / `_iter` and their operations. | STR3 | The view scripts pass, including isolated-surrogate cases. |
| **STR-5 — Tests** | Add the proposal's scripts and a manual zero-copy test. | STR6 | The curated list includes the stringref scripts. |

The encoding-level staging is `GC.md` §10.6 (`SR-1`…`SR-3`); this note's
milestones are the same work seen from the proposal side, with STR-0 added so
the current state is not silently wrong.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no `stringref`/`stringview` script in the testsuite snapshot |

Reading: there is nothing to grade yet. A module using the reserved type codes
is not exercised by the suite; that is exactly why STR-0 is the first
milestone.

## 9. References

- Proposal overview (`WebAssembly/stringref`):
  https://github.com/WebAssembly/stringref/blob/main/proposals/stringref/Overview.md
- Spec — GC and reference types (the substrate):
  https://webassembly.github.io/spec/core/syntax/types.html
- In-repo: [`GC.md`](GC.md) §10 (Beyond 3.0), [`REFERENCE_TYPES.md`](REFERENCE_TYPES.md),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), [`../SAFETY.md`](../SAFETY.md),
  [`../ABI.md`](../ABI.md); sources `WasmBase/WasmType.hpp` (`ValueTypeCode`),
  `WasmBase/WasmSection.hpp`, `WasmBase/WasmModule.cpp` (`processSecetions`,
  `parseInitExpr`), `WasmBase/WasmOpcode.hpp` (`GCOpcode`),
  `LibJit/LibJitTypeTranslation.cpp`.
