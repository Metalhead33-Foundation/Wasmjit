# Reference-Typed Strings (`stringref`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 7 — the value types,
section 14, `string.const`, the core string instructions, the full
`stringview_*` facility, the GC-array variants, the embedder bridge and
reference-counted string lifetime are implemented. The engine keeps its own
reserved value-type encodings because the proposal's current ones collide with
GC — see §5 STR1 / §6 STRD5).
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
- No JavaScript string semantics. The separate `wasm:js-string` host-import
  proposal operates on `externref`, not on `stringref`; see
  [`JS_INTEGRATION.md`](JS_INTEGRATION.md).

## 4. What we have implemented today

- The four value-type codes are **modelled** nullable references:
  `StringRef = -0x19 (0x67)`, `StringViewWtf8 = -0x1A (0x66)`,
  `StringViewWtf16 = -0x1B (0x65)`, `StringViewIter = -0x1E (0x62)`. The
  engine uses these reserved encodings rather than the proposal's current
  `0x64`/`0x63` because those collide with GC `ref`/`ref null` (§5 STR1).
- A `stringref` is a `StringObject*` (`WasmBase/WasmString.hpp`): a sequence of
  Unicode scalar values and isolated surrogates stored as code points. UTF-8,
  WTF-8 and WTF-16 are derived on demand. The Store owns every string
  (`Store::strings()`); pointers are stable while the object is retained.
  Strings, views and iterators are **reference-counted** (`StringStore::retain`/
  `release`), and the runtime's `refClone`/`refDrop` barriers drive those counts
  exactly as they drive GC-object counts. A zero count frees the object and
  credits its allocation-budget charge. Host-created strings (`createFrom*`) are
  owned (count 1) and must be released by the host; internal `create` results
  are unowned like a fresh GC aggregate.
- **Section 14** is parsed (`SectionType::String`, `processStringSection`):
  the `0x00` form byte, `vec(vec(u8))`, WTF-8 validation per literal, and the
  positional rule (at most one, in the global slot). A pathological literal
  count is rejected.
- **`string.const`** (`0xFB 0x82`) is accepted in constant expressions and in
  function bodies. Literals are materialised from section 14 into
  `ModuleInstanceInternals::stringLiterals` before globals are evaluated, so a
  `(global stringref (string.const 0))` works and function bodies can push the
  literal pointer. The validator checks the literal index.
- **Core instructions** are lowered (`0xFB` sub-opcodes are LEB128, so
  `0x80`…`0x8E` are two bytes on the wire): `string.new_utf8`,
  `new_lossy_utf8`, `new_wtf8`, `new_wtf16`; `measure_utf8`/`_wtf8`/`_wtf16`;
  `encode_utf8`/`_lossy_utf8`/`_wtf8`/`_wtf16`; `concat`, `eq`,
  `is_usv_sequence`. The null rule is centralised in the runtime helpers:
  every string instruction traps on a null reference except `string.eq`
  (null == null, null != a string). Encoding failures trap with the new
  `TrapKind::InvalidString`.
- The **views** are implemented: `string.as_wtf8` / `as_wtf16` / `as_iter`
  produce Store-owned `StringViewObject` / `StringIteratorObject` handles.
  WTF-8 and WTF-16 views are stateless and take explicit positions; the
  iterator is mutable. All view operations (`0x90`–`0xA4`) are lowered,
  including the multi-value `stringview_wtf8.encode_*` pair, the WTF-8/WTF-16
  position treatment, the two-byte alignment check on `stringview_wtf16.encode`,
  and `slice` returning a new stringref.
- The **GC-array variants** (`0xB0`…`0xB7`) are lowered:
  `new_utf8_array`, `new_lossy_utf8_array`, `new_wtf8_array`, `new_wtf16_array`,
  `encode_utf8_array`, `encode_lossy_utf8_array`, `encode_wtf8_array` and
  `encode_wtf16_array`. The validator requires the immediate to name an array of
  packed `i8` (`i16` for WTF-16) and, for the encoders, a mutable array. The
  runtime reads and writes the array's inline elements, traps on a null array,
  on negative or out-of-range `start`/`end`, and when the encoded string does
  not fit.
- The **embedder bridge** (`StringStore::createFromUtf8` / `createFromWtf8` /
  `createFromWtf16`, and `copyToUtf8` / `copyToWtf8` / `copyToWtf16`) creates
  host strings in the Store and copies them back. A wasm `stringref` parameter
  or result is passed to `callCallable` as a `StringObject*`. Malformed host
  input returns nullptr and reports `valid = false`; allocation failure still
  traps with `OutOfMemory`. A successful `createFrom*` result is owned (count
  1) and the host releases it with `StringStore::release`.
- The `WasmStub` listing prints the core, view and GC-array string instructions.
- The tests use hand-built modules because `wasm-tools` 1.252 cannot parse
  `stringref` text: parsing/round-trip, a `string.const` global, the core
  instructions through the JIT, `new_utf8`/`new_wtf16` + `encode_*` through
  linear memory, strict-vs-lossy decoding, the three view position models, and
  the multi-value view encoders.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| STR1 | **The proposal's current value-type encodings collide with GC.** The proposal's binary format lists `stringref = 0x64`, `stringview_wtf8 = 0x63`, `stringview_wtf16 = 0x62`, `stringview_iter = 0x61`; `0x64`/`0x63` are GC `ref`/`ref null` in the final spec. The engine keeps its own reserved encodings (`0x67`/`0x66`/`0x65`/`0x62`, §6 STRD5) until the proposal resolves this. | `WasmType.hpp`; `proposals/stringref/Overview.md` "Binary encoding"; §10.3 of `GC.md` | Medium |
| ~~STR2~~ | ~~**No string table section.**~~ **Done (STR-2):** `SectionType::String` (14) is parsed with the `0x00` form byte, `vec(vec(u8))`, WTF-8 validation and the positional rule; a pathological count is rejected. | `WasmSection.hpp`, `WasmModule.cpp` (`processStringSection`) | Closed |
| ~~STR3~~ | ~~**No string/stringview instructions.**~~ **Done (STR-3/STR-4):** the core instruction set and the full `stringview_*` facility are decoded, typed, lowered and executed; `string.const` is const-eligible. | `WasmOpcode.hpp` (`StringOpcode`), `LibjitOpcodeDispatcher.cpp`, `WasmValidator.cpp` | Closed |
| ~~STR4~~ | ~~**Declared string types are accepted but unimplemented.**~~ **Done (STR-1/STR-3):** all four codes are modelled as nullable host references, and the instructions produce real `StringObject*`/view values. The old load-time rejection (`STR-0`) is superseded. | `WasmType.hpp`, `WasmString.*`, `WasmModuleInstance.cpp` | Closed |
| ~~STR5~~ | ~~**No embedder string bridge.**~~ **Done (STR-7):** `StringStore` creates strings from host UTF-8/WTF-8/WTF-16 and copies them back; `callCallable` carries a `StringObject*` as a `stringref`. Malformed input returns nullptr instead of trapping. | `WasmString.hpp`, `Test/main.cpp` | Closed |
| ~~STR6~~ | ~~**No test coverage.**~~ **Done (STR-5):** sixteen hand-built tests cover parsing, the section, `string.const`, the core JIT path, memory round-trips, strict-vs-lossy decoding, the view positions, the multi-value view encoders, the GC-array bridge, the embedder bridge and lifetime. No upstream `stringref` script exists in the snapshot (and `wasm-tools` 1.252 cannot emit one), so the tests are hand-built. | `Test/main.cpp` | Closed (manual) |
| ~~STR7~~ | ~~**The `stringview_*` facility is not lowered.**~~ **Done (STR-4):** `string.as_*` and every view operation (`0x90`–`0xA4`) are typed, lowered and tested, including multi-value encodes and the position treatment. | `WasmOpcode.hpp`, `WasmString.*`, `LibjitOpcodeDispatcher.cpp`, `WasmValidator.cpp` | Closed |
| ~~STR8~~ | ~~**The GC-array variants are not lowered.**~~ **Done (STR-6):** `string.new_*_array` / `encode_*_array` (`0xB0`–`0xB7`) share the GC array layout, validate the packed element type and mutability, and read/write inline elements with the spec's bounds traps. | `StringOpcode`, `WasmValidator.cpp`, `LibjitOpcodeDispatcher.cpp` | Closed |
| ~~STR9~~ | ~~**Strings are never freed before Store teardown.**~~ **Done (STR-8):** `StringObject`, `StringViewObject` and `StringIteratorObject` are reference-counted in `StringStore`; `ModuleInstance::refClone`/`refDrop` and the GC walker call `retain`/`release`, a zero count frees the object and credits its budget, and views/iterators retain the string they borrow. | `WasmString.hpp`/`.cpp`, `WasmModuleInstance.cpp`, `LibjitOpcodeDispatcher.cpp` | Closed |
| STR10 | **Literals are materialised per instance.** Each `ModuleInstance` decodes section 14 into fresh `StringObject`s; identical literals in two instances are not shared. | `ModuleInstance::initializeStringLiterals` | Low (deliberate) |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| STRD1 | **Keep the four type codes as reserved placeholders** and model them as nullable host references. | §3: representation is free; the exact-match hierarchy is the proposal's MVP shape. | No `string` heap type or subtype machinery. |
| STRD2 | **Accept a declared string type and implement it**, rather than keeping the old load-time rejection. | §3 permits a subset; the implemented surface is correct and any future instruction still fails loudly (`onUnhandledOpcode`). | Real values and real failures, with no silent null. |
| STRD3 | **Do not model host strings as wasm values.** A `stringref` is a handle to a Store-owned `StringObject`; the embedder bridge copies bytes in and out, and there is no JS string runtime or zero-copy adapter. | §3: representation and host API are free. | The engine need not implement a JS string runtime. |
| STRD4 | **Centralise the null rule** ("trap on every instruction except `string.eq`") in the helpers rather than in every dispatch arm. Implemented. | §3: the lowering is free; §2's rule is uniform. | One check, no per-op duplication. |
| STRD5 | **Use the engine's reserved encodings (`0x67`/`0x66`/`0x65`/`0x62`) instead of the proposal's current `0x64`/`0x63`/`0x62`/`0x61`.** | §3: the wire encoding is the proposal's, not a semantic guarantee; the current values make `stringref` indistinguishable from GC `ref` and `ref null`. | The engine stays internally consistent until the proposal resolves the collision (`STR1`). |
| STRD6 | **Represent strings as code points**, deriving UTF-8/WTF-8/WTF-16 on demand. | §3: no internal representation is required; all encodings are deterministic functions of the code-point sequence. | Exact round-trips and a single implementation of `measure_*`, `encode_*`, `eq` and `concat` (including the surrogate-combining boundary rule). |
| STRD7 | **Reference-count strings, views and iterators in `StringStore`.** Counts start at zero like a fresh GC aggregate; the runtime's `refClone`/`refDrop` barriers retain/release, a view/iterator retains the string it borrows, and a zero count frees the object and credits its budget. | §3: ownership and lifetime are free; `SAFETY.md`'s budget remains the hard backstop. | A long-lived Store stays bounded without a collector or a root scan. Host-created strings are owned (count 1) and released explicitly. |
| STRD8 | **Model WTF-8/WTF-16 views as stateless handles over the code-point string, and the iterator as a mutable position.** `slice` returns a new stringref. | §3: view representation and position bookkeeping are free; the instructions' observable results are the positions and the returned strings. | No materialised WTF-8/WTF-16 buffer per view; one code-point source of truth, with positions translated per operation. |

Guardrail: `isModeledValueTypeCode` is a *lowering* statement, not a
conformance claim. The core, the views, the GC-array variants, the embedder
bridge and reference-counted lifetime are supported; only the proposal-side
encoding collision (`STR1`) remains, and it is external to this engine.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **STR-0 — Honest rejection (superseded)** | ✅ Historically rejected declared string types. Superseded by STR-1/STR-3: the core is implemented, so this policy is no longer in force. | — | Historical only. |
| **STR-1 — Encodings** | ◑ The four reserved codes decode, round-trip and lower; `stringref` declarations are accepted. Aligning them to the proposal's current `0x64`/`0x63`/`0x62`/`0x61` is **blocked** by the GC `ref`/`ref null` collision (`STRD5`). | STR1 | The reserved-code round-trip passes; the current-encoding half waits for the proposal. |
| **STR-2 — String table** | ✅ `SectionType::String` (14), the `0x00` form byte, `vec(vec(u8))`, WTF-8 validation, the global-slot rule, and `string.const` in constant expressions. | STR2 | `(global stringref (string.const 0))` works. |
| **STR-3 — String core** | ✅ `new_*`, `measure_*`, `encode_*`, `concat`, `eq`, `is_usv_sequence` over the Store-owned `StringObject`, with the centralised null rule and `InvalidString` traps. | STR3, STR4 | The core instruction tests pass and `stringref` values round-trip through linear memory. |
| **STR-4 — Views** | ✅ `string.as_wtf8`/`as_wtf16`/`as_iter`, every `stringview_*` operation, multi-value WTF-8 encodes, the position treatment, the WTF-16 alignment check and `slice`. | STR7 | The view fixtures pass, including the position and multi-value cases. |
| **STR-5 — Tests** | ✅ Sixteen hand-built tests cover parsing, section 14, `string.const`, the core JIT path, memory round-trips, strict-vs-lossy decoding, the three view position models, the multi-value encoders, the GC-array bridge, the embedder bridge and lifetime. No upstream script exists in the snapshot. | STR6 | The tests run in the default suite. |
| **STR-6 — GC-array variants** | ✅ `new_*_array` / `encode_*_array` (`0xB0`–`0xB7`) over the GC array's inline elements, with packed-element and mutability validation and the spec's bounds/capacity traps. | STR8 | The multibyte and GC-array fixtures pass. |
| **STR-7 — Embedder bridge** | ✅ `createFromUtf8`/`createFromWtf8`/`createFromWtf16` and `copyToUtf8`/`copyToWtf8`/`copyToWtf16`, with `StringObject*` passed through `callCallable`. | STR5 | A host string survives a call round-trip. |
| **STR-8 — Lifetime** | ✅ Reference-counted strings, views and iterators (`StringStore::retain`/`release`), driven by `refClone`/`refDrop` and the GC walker; zero count frees and credits the budget. Host strings are owned. | STR9 | A long-lived Store creating many short strings stays bounded: the direct RC test and the JIT `drop`-a-concat test observe the count return to baseline. |

The encoding-level staging is `GC.md` §10.6 (`SR-1`…`SR-3`); this note's
milestones are the same work seen from the proposal side. The implemented
surface is complete — types, section, core, views, GC arrays, embedder bridge
and lifetime — with only the proposal-side encoding collision (`STR1`) left
open, and that is an external format issue.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no `stringref`/`stringview` script in the testsuite snapshot (`wasm-tools` 1.252 cannot emit one) |
| `stringref types and the string section are parsed (STR-1/STR-2)` | 10 assertions | 0 | 0 | the reserved type codes are modelled and section 14 holds the expected literals |
| `an unmodelled value type is still rejected at load` | 2 assertions | 0 | 0 | an unassigned code (0x68) still fails at the boundary |
| `string.const initializes a stringref global` | 9 assertions | 0 | 0 | `(global stringref (string.const 0))` holds the first literal |
| `string.const, measure, eq and concat run through the JIT` | 20 assertions | 0 | 0 | WTF-8/WTF-16/UTF-8 lengths, equality, concat, and the isolated-surrogate results |
| `string.new_utf8/wtf16 and encode round-trip through memory` | 10 assertions | 0 | 0 | UTF-8 and WTF-16 input round-trip to memory in WTF-8/UTF-8 |
| `string.new_utf8 is strict while new_lossy_utf8 replaces bad bytes` | 11 assertions | 0 | 0 | invalid UTF-8 traps in `new_utf8`; `new_lossy_utf8` yields U+FFFD; `encode_utf8` traps on an isolated surrogate |
| `stringview WTF-8, WTF-16 and iterator positions run through the JIT` | 23 assertions | 0 | 0 | position treatment, advance/slice, WTF-16 length/code unit, iterator next/advance/rewind/slice |
| `stringview encode returns the next position and bytes written` | 14 assertions | 0 | 0 | the multi-value WTF-8 pair and the WTF-16 encode write the expected bytes |
| `string array constructors and encoders bridge to GC arrays` | 21 assertions | 0 | 0 | i8/i16 globals constructed with `array.new_fixed`/`new_default`, both directions of the bridge, and the encoded bytes inspected in place |
| `string array variants enforce bounds, strictness and lossy encoding` | 28 assertions | 0 | 0 | strict/lossy/WTF-8 constructors, out-of-bounds and insufficient-capacity traps, and the lossy/WTF-8 encoder outputs |
| `string array validation checks element type and mutability` | 4 assertions | 0 | 0 | an i16 argument to `new_utf8_array` and an immutable array argument to `encode_utf8_array` are rejected |
| `the embedder bridge creates and copies stringrefs` | 30 assertions | 0 | 0 | host UTF-8/WTF-8/WTF-16 in, `StringObject*` through a wasm call, UTF-8/WTF-8/WTF-16 out (including strict-vs-lossy), and host-owned references released |
| `stringref reference counting releases strings and views` | 10 assertions | 0 | 0 | retain/release frees at zero; a view keeps its string alive after the caller releases it |
| `a discarded string concat result is released through the JIT` | 7 assertions | 0 | 0 | `string.const` borrows are dropped and the concat result is freed by the JIT's `drop` path |

Reading: the upstream snapshot has no stringref script, so the evidence is the
hand-built set above. It covers the whole surface — types, section 14,
`string.const`, the core instructions, memory round-trips, strict/lossy
decoding, the full view facility, the GC-array bridge, the embedder bridge and
reference-counted lifetime. The total default suite is **9,146 assertions /
335 test cases**, green; the string-filtered set is **239 assertions / 16 test
cases**.

## 9. References

- Proposal overview (`WebAssembly/stringref`):
  https://github.com/WebAssembly/stringref/blob/main/proposals/stringref/Overview.md
- Spec — GC and reference types (the substrate):
  https://webassembly.github.io/spec/core/syntax/types.html
- In-repo: [`GC.md`](GC.md) §10 (Beyond 3.0), [`REFERENCE_TYPES.md`](REFERENCE_TYPES.md),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), [`../SAFETY.md`](../SAFETY.md),
  [`../ABI.md`](../ABI.md); sources `WasmBase/WasmType.hpp` (`ValueTypeCode`,
  `isModeledValueTypeCode`), `WasmBase/WasmString.hpp`/`.cpp`
  (`StringObject`, `StringStore`, codecs), `WasmBase/WasmSection.hpp`,
  `WasmBase/WasmModule.cpp` (`processSecetions`, `processStringSection`,
  `parseInitExpr`, `validateValueTypes`), `WasmBase/WasmModuleInstance.cpp`
  (`initializeStringLiterals`, `evalConstantExpr`), `WasmBase/WasmOpcode.hpp`
  (`StringOpcode`), `WasmBase/WasmOpcodeDispatcher.cpp`
  (`dispatchPrefixString`), `WasmBase/WasmValidator.cpp` (typing),
  `LibJit/LibjitOpcodeDispatcher.cpp` (helpers and lowering),
  `WasmStub/StubOpcodeDispatcher.cpp`, `Test/main.cpp`.
