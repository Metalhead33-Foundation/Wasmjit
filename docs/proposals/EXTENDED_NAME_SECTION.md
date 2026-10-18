# Extended Name Section — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — every core and
extended name subsection is parsed and exposed through `Module::findName`;
diagnostics do not consume the names yet).
Audience: anyone touching `WasmModule` (custom sections), `WasmSection`, or any
tooling that wants to read names.

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

The proposal extends the well-known `name` custom section. Names have no
semantics; the proposal adds the missing **subsections** and the matching text
annotations:

| Subsection id | Contents |
|---|---|
| 3 | label names (an indirect map per function; a new source-order label index space) |
| 5 | table names |
| 6 | memory names |
| 7 | global names |
| 8 | element-segment names |
| 9 | data-segment names |
| 12 | parameter names (indirect map per **type** index) |
| 13 | tag parameter names (indirect map per **tag** index) |

The text format's `@name` annotations must also be allowed on labels, tables,
memories, globals, element segments and data segments, so a name can be
attached wherever an identifier binds. The proposal does not change execution.

## 3. What the proposal does not demand

- No runtime behaviour, no validation and no effect on results.
- No requirement to store any name; a custom section may be ignored wholesale.
- No required API for name lookup; names are for debuggers and tooling.
- No fixed consumer format beyond the subsection encoding.
- No requirement to parse the text format (this engine consumes binary modules
  produced by `wasm-tools`; WAT parsing is permanently outside its scope).

## 4. What we have implemented today

- `Module::processNameSection` (`WasmBase/WasmModule.cpp`) copies the name
  payload into a bounded buffer and parses each subsection from its own
  `ConstBufferDevice`, so every LEB and string is bounded by the subsection
  (and then by the section). An unknown subsection is skipped by `subSize`; a
  `subSize` past the section end is rejected.
- **Subsection 0** (module name) is stored in `Module::debugName` and
  **subsection 1** (function names) in `Module::funcNames`.
- **Direct maps** are parsed for subsections 4 (type), 5 (table), 6 (memory),
  7 (global), 8 (element segment), 9 (data segment) and 11 (tag); each lands in
  a `std::map<uint32_t, std::string>` on `Module`.
- **Indirect maps** are parsed for subsections 2 (local), 3 (label), 10
  (field), 12 (parameter) and 13 (tag parameter); each is a
  `std::map<uint32_t, std::map<uint32_t, std::string>>` keyed by the outer
  index first.
- `Module::findName(NameKind, index, subIndex = 0)` is the public lookup. It
  returns `nullptr` for an absent subsection or index; names are metadata and
  are never validated against an index space (`ENSD2`).
- `names.wast` remains curated and runs to completion (`486 passed / 0 failed /
  0 skipped`, 2026-10-17). The manual `name_section` fixture now asserts the
  stored strings, so passing no longer only proves tolerance.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~ENS1~~ | ~~**Only subsections 0 and 1 are parsed.**~~ **Done (ENS-1/ENS-2):** direct maps 4/5/6/7/8/9/11 and indirect maps 2/3/10/12/13 are parsed, including the extended ids 3, 5, 6, 7, 8, 9, 12 and 13. | `WasmModule.cpp` (`processNameSection`); `name_section` manual test | Closed |
| ~~ENS2~~ | ~~**Even the core name subsections are incomplete.**~~ **Done (ENS-2):** local (2), type (4), field (10) and tag (11) names are parsed too. | same | Closed |
| ENS3 | **No diagnostic consumer yet.** `Module::findName(NameKind, index, subIndex)` is the public lookup and the test reads it, but trap/disassembly diagnostics still print raw indices. | `WasmModule.hpp` (`findName`); `Test/main.cpp` | Low |
| ~~ENS4~~ | ~~**`names.wast` does not assert names.**~~ **Done (ENS-4):** `Test/wasm_wat/name_section.wat` + `extended name section: parsed names are exposed by kind and index` assert every subsection's strings; `name section: indirect maps and unknown subsections are bounded and skipped` covers 12/13 and unknown-id skipping. | `Test/main.cpp`, `Test/wasm_wat/name_section.wat` | Closed |
| ENS5 | **No text-format annotation support.** The engine has no `.wat` parser and will never have one; `@name` handling belongs to the toolchain that produces the binary. | `Test/Test.pro` uses `wasm-tools` | Informational |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ENSD1 | **Skip unknown subsections by `subSize`** (the current behaviour). | §3: names are metadata; the size prefix makes skipping exact. | Forward compatibility with future subsections. |
| ENSD2 | **Store names without validating indices**; validate only at lookup. | §3: no execution effect; a name for an out-of-range index is still just a string. | The parser never rejects an otherwise valid module for a bad name. |
| ENSD3 | **Use names only in diagnostics** (trap text, disassembly), never in identity or dispatch. | §3; names are not semantic identity. | Better error messages without changing behaviour. |
| ENSD4 | **Do not implement the text annotations.** | §3; the engine is binary-only. | No second parser to maintain. |

Guardrail: a malformed name subsection must never change decoding of the code
or data sections. The parser must keep honoring `subSize` even for ids it does
not know.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **ENS-1 — Parse direct maps** | ✅ Read subsections 4/5/6/7/8/9/11 into per-index name tables. | ENS1 | Done: `name_section` asserts type, table, memory, global, element, data and tag names. |
| **ENS-2 — Parse indirect maps** | ✅ Read subsections 2/3/10/12/13 and the core 2/4/10/11. | ENS1, ENS2 | Done: local, label, field, parameter and tag-parameter names are addressable; a hand-built fixture covers 12/13. |
| **ENS-3 — Expose lookup** | ◑ `Module::findName(NameKind, index, subIndex)` is public and tested; trap/disassembly diagnostics still print raw indices. | ENS3 | The API half is done; a diagnostic half remains. |
| **ENS-4 — Assert names** | ✅ `Test/wasm_wat/name_section.wat` + two manual tests check the stored strings and the 12/13/unknown-id paths. | ENS4 | Done: the tests fail if a subsection is dropped or desynchronised. |

ENS5 is explicitly triaged as not-worth-closing: the engine has no text
parser, and `@name` handling belongs to `wasm-tools`.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `names` | 486 | 0 | 0 | section tolerated; the manual fixture is what asserts names |
| `name_section` manual test | 17 assertions | 0 | 0 | every parsed subsection is read back through `findName` |
| hand-built 12/13 + unknown id | 7 assertions | 0 | 0 | indirect maps work and an unknown subsection is skipped by size |
| `custom_section` | 3 assertions | 0 | 0 | an annotation-generated custom section leaves code decoding unchanged |

Reading: `names.wast` still means "the custom section does not break decoding";
`name_section` and the hand-built fixture are the tests that prove the names
are actually stored.

## 9. References

- Proposal overview (`WebAssembly/extended-name-section`):
  https://github.com/WebAssembly/extended-name-section/blob/main/proposals/extended-name-section/Overview.md
- Spec — Custom sections and the name section:
  https://webassembly.github.io/spec/core/appendix/custom.html
- In-repo: [`INSTRUMENT_TRACING.md`](INSTRUMENT_TRACING.md) (other non-semantic
  metadata), [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) §4 (branch hints),
  [`PARSER_PROPOSALS.md`](PARSER_PROPOSALS.md) (compilation hints and relaxed
  dead-code validation), [`ANNOTATIONS.md`](ANNOTATIONS.md) (the text-format
  annotation grammar);
  sources `WasmBase/WasmModule.hpp` (`NameKind`, `findName`, the name maps),
  `WasmBase/WasmModule.cpp` (`processCustomSection`, `processNameSection`),
  `Test/wasm_wat/name_section.wat`, `Test/wast_supported.txt`.
