# Extended Name Section — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — the parser reads
only the module and function names; every extended subsection is skipped).
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

- `Module::processNameSection` (`WasmBase/WasmModule.cpp`) reads the section
  name, then walks `(subId, subSize)` pairs.
- **Subsection 0** (module name) is stored in `Module::debugName`.
- **Subsection 1** (function names) is stored in `Module::funcNames`.
- Every other subsection is skipped by seeking to `nextSubSection`, so an
  unknown or extended subsection cannot desynchronise the parser.
- `names.wast` is curated and runs to completion (`486 passed / 0 failed /
  0 skipped`, 2026-10-17), but the harness never inspects `debugName` or
  `funcNames`; passing only proves that the section is tolerated.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ENS1 | **Only subsections 0 and 1 are parsed.** The extended ids 3, 5, 6, 7, 8, 9, 12 and 13 are all skipped. | `WasmModule.cpp` (`processNameSection`) | Medium |
| ENS2 | **Even the core name subsections are incomplete.** Local (2), type (4), field (10) and tag (11) names are skipped too. | same | Medium |
| ENS3 | **No consumer.** `debugName` and `funcNames` are written by the parser but never read, so there is no API to look up a name and no diagnostic uses it. | `Test/` has no reference to either field | Medium |
| ENS4 | **`names.wast` does not assert names.** It passes because names are ignored, not because they are parsed. | `spec: names` 486/0/0 | Low |
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
| **ENS-1 — Parse direct maps** | Read subsections 5/6/7/8/9 into per-index name tables. | ENS1 | A hand-built name section round-trips all five maps. |
| **ENS-2 — Parse indirect maps** | Read subsections 3/12/13 and the core 2/4/10/11. | ENS1, ENS2 | Labels, params and tag params are addressable. |
| **ENS-3 — Expose lookup** | Add a `nameOf(kind, index)` API and use it in trap/disassembly diagnostics. | ENS3 | A test reads a function, table, global and label name. |
| **ENS-4 — Assert names** | Add a manual test that checks the stored strings (not just that `names.wast` runs). | ENS4 | The test fails if a subsection is dropped. |

ENS5 is explicitly triaged as not-worth-closing: the engine has no text
parser, and `@name` handling belongs to `wasm-tools`.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `names` | 486 | 0 | 0 | section tolerated; no name is asserted |
| manual name-section test | — | — | — | none yet |

Reading: green here means "the custom section does not break decoding", not
"the names are implemented". ENS-4 is the test that would change that reading.

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
  sources `WasmBase/WasmModule.hpp` (`debugName`, `funcNames`),
  `WasmBase/WasmModule.cpp` (`processCustomSection`, `processNameSection`),
  `Test/wast_supported.txt`.
