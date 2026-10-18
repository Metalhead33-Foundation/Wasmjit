# Custom Annotation Syntax for the Wasm Text Format — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — **binary-only is a
permanent architectural rule. This project will never implement WAT parsing or
an annotation frontend.** The proposal is tooling-side only: `wasm-tools` (or
another producer) consumes annotations and emits a binary. The engine's binary
parser handles only the `name` section and `metadata.code.branch_hint`, and
skips every other custom section by size.)

Audience: anyone touching `Test/Test.pro`/WAT fixture generation, `WasmModule`
custom sections, the extended name section, or tooling that emits annotations.

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

The proposal adds a general annotation syntax to the **WebAssembly text
format**. An annotation is a balanced parenthesised form:

> **Permanent scope rule.** This engine is binary-only. It will **never** parse
> WAT, implement `(@...)` syntax, or accept a text module. The grammar below is
> documented so the binary/tooling boundary is unambiguous; it is not an engine
> feature request and has no future milestone here.

```
annot      ::= "(@" annotid annotelem* ")"
annotid    ::= idchar+ | name
annotelem  ::= keyword | reserved | uN | sN | fN | string | id | "(" annotelem* ")"
```

- An annotation may appear anywhere whitespace is allowed. No whitespace is
  allowed inside the initial `(@annotid` delimiter.
- `annotid` categorises the extension, like a custom-section name. By
  convention, an annotation corresponding to a custom section uses the same id.
- Symbolic identifiers are extended with quoted names: `$"..."`; identifiers
  denote names, and all identifier comparisons use the denoted name.
- The Appendix defines two standard uses:
  - `(@name "name")` after a binder for a construct that the name section can
    name;
  - `(@custom "section-name" place? datastring)` to express an arbitrary custom
    section, with `before`/`after` placement relative to a named section or
    `before first`/`after last`. Placement relative to a section requires that
    the section exists in the encoded module.

The proposal only affects the text format. It does not add a binary section,
instruction, value type or runtime behaviour. An implementation may ignore
annotations it does not understand, and the spec does not prescribe their
syntax or semantics beyond the balanced form.

## 3. What the proposal does not demand

- No binary-format change. The encoded module has no annotation section.
- No runtime, validation or execution semantics.
- No required interpretation of `@name`, `@custom` or extension annotations.
- No requirement that an engine parse the text format at all. **This project
  does not and will never implement WAT parsing; that is a permanent design
  boundary, not a missing feature.**
- No required custom-section ordering/overlap policy; the proposal leaves
  overlap with explicit custom sections implementation-defined.
- No required enforcement of `@custom` placement or the “section must exist”
  rule on a binary-only consumer.
- No need to preserve annotations across a text→binary→text round trip unless a
  tool chooses to do so.
- No requirement to implement the proposal to remain conformant: a binary
  consumer can ignore it.

## 4. What we have implemented today

- The engine is **binary-only by permanent design**. It has no WAT parser and
  will never gain one. `WasmModule::processCustomSection`
  (`WasmBase/WasmModule.cpp`) reads the custom-section name and dispatches only
  `name` and `metadata.code.branch_hint`; every other custom section is skipped
  by `section.offset + section.size`. That is the correct binary fallback for
  annotation-generated custom sections the engine does not consume.
- `Module::processNameSection` stores the module name (§0) and function names
  (§1); the other name subsections, including names attached by `@name`, are
  skipped. See [`EXTENDED_NAME_SECTION.md`](EXTENDED_NAME_SECTION.md) ENS1–ENS4.
- The test build invokes `wasm-tools` to convert `.wat`/`.wast` fixtures to
  binary before the engine sees them (`Test/Test.pro`). Any `(@...)` annotation
  is therefore interpreted (or ignored) by `wasm-tools`, not by this engine.
- `Test/wasm_wat/branch_hint_loop.wat` documents the analogous
  `@metadata.code.branch_hint` annotation; the test asserts the resulting binary
  hint, not an annotation parser.
- There is no text parser, annotation grammar implementation, `@custom`
  placement logic or annotation-specific test, and **none will be added**: WAT
  parsing is permanently outside this engine.

## 5. Gaps

`ANN1` and `ANN2` are **not engine gaps**. They are closed by the permanent
binary-only decision (`ANND1`, milestone `ANN-0`). The only remaining items are
the binary `@name` metadata path (`ANN3`) and a custom-section tolerance test
(`ANN4`).

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~ANN1~~ | **No text parser or annotation grammar.** **Done (ANN-0):** WAT parsing is permanently out of scope; the engine will never accept `(@id ...)` or a text module. | `WasmModule` is binary-only; `Test/Test.pro` uses `wasm-tools`. | N/A — closed by decision |
| ~~ANN2~~ | **No annotation-to-custom-section round-trip in the engine.** **Done (ANN-0):** `@custom` placement and text round-tripping are producer/tooling work; the engine only consumes the resulting binary. | No `@custom` handling or text writer in `WasmBase/`; tests use `wasm-tools`. | N/A — tooling-owned |
| ANN3 | **`@name` has only the current partial binary name support.** Even the standard name annotation lands in name subsections 0/1 only; the rest are skipped. | `WasmModule.cpp` (`processNameSection`); `EXTENDED_NAME_SECTION.md` ENS1/ENS2. | Medium (shared with ENS) |
| ANN4 | **No fixture proving unknown-custom-section tolerance.** The suite does not assert that an annotation-generated custom section leaves code/data decoding unchanged. | `Test/wast_supported.txt`; `Test/wasm_wat/`; no annotation fixture. | Low |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ANND1 | **Never implement WAT parsing or the annotation grammar.** Binary-only is permanent. | §2/§3: the proposal is text-only, and an engine need not parse text at all. | No second frontend, no text attack surface, and no ambiguous “test-only” parser. |
| ANND2 | **Treat annotations as producer syntax, not engine input.** `@name`, `@custom` and extension annotations are handled by `wasm-tools`/the embedder before the binary is loaded. | §3: the runtime only sees the encoded module. | One canonical input format and one parser (the binary one). |
| ANND3 | **Skip unknown custom sections by their size.** This is the binary fallback for annotation-generated sections the engine does not consume. | §3: annotations have no runtime meaning; custom sections are length-delimited. | Forward compatibility and no desynchronisation. |
| ANND4 | **Never let annotations affect identity, validation or execution.** Names and hints are metadata. | §3: the proposal prescribes no semantics. | No semantic dependency on tooling input. |
| ANND5 | **Use `@name`-generated binary names only for diagnostics, never for lookup identity.** | §3 and `EXTENDED_NAME_SECTION.md` ENSD3. | Names can improve errors without changing behaviour. |

Guardrail: there is **no** WAT/annotation parser to add later. No feature flag,
test helper, or “small” frontend may reintroduce one. An embedder that needs to
consume WAT or annotations must run a separate producer/tool first and hand the
engine a binary.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **ANN-0 — Permanent binary-only rule** | ✅ Record in this note (and the linked notes) that WAT parsing, `(@...)` syntax and annotation frontends will **never** be implemented; the binary-only rule is permanent. | ANN1, ANN2 | This note states “never”, and no plan or code path depends on a WAT/annotation parser. |
| **ANN-1 — Name integration** | Let `@name`-generated binary names flow through the existing extended-name work (ENS-1/ENS-2) and expose them only for diagnostics. | ANN3 | A name attached with `@name` is readable through the name API; no identity changes. |
| **ANN-2 — Custom-section tolerance** | Add a test fixture built with `wasm-tools` that uses `(@custom ...)`; check that the engine skips the resulting section and decodes code/data unchanged. | ANN4 | The fixture passes with the annotation-generated section present and absent. |

There is deliberately no milestone for a WAT/annotation frontend. The only
producer-side work is outside this repository: a tool such as `wasm-tools`
accepts `(@...)`, applies the name/custom-section Appendix semantics, and emits
a binary for the engine.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `names` | 486 | 0 | 0 | binary name section tolerated; not annotations |
| `branch_hint_loop` | manual | — | — | `@metadata.code.branch_hint` is consumed by `wasm-tools`; the test grades the binary hint |
| annotations / `@custom` | — | — | — | no fixture and no text parser |

Reading: the suite proves custom-section tolerance and branch-hint parsing, not
the annotation grammar. `ANN-1`/`ANN-2` are the first tests that would cover the
binary-name and custom-section paths; there is no annotation-parser test because
there will never be an annotation parser in this engine.

## 9. References

- Custom Annotation Syntax for the Wasm Text Format:
  https://github.com/WebAssembly/annotations/blob/main/proposals/annotations/Overview.md
- Spec — Text format and custom sections:
  https://webassembly.github.io/spec/core/text/index.html
  https://webassembly.github.io/spec/core/appendix/custom.html
- In-repo: [`EXTENDED_NAME_SECTION.md`](EXTENDED_NAME_SECTION.md),
  [`PARSER_PROPOSALS.md`](PARSER_PROPOSALS.md) (compilation hints and relaxed
  dead-code validation), [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) §4 (branch
  hints), [`INSTRUMENT_TRACING.md`](INSTRUMENT_TRACING.md); sources
  `WasmBase/WasmModule.hpp` (`debugName`, `funcNames`,
  `branchHintsByFuncIndex`), `WasmBase/WasmModule.cpp`
  (`processCustomSection`, `processNameSection`,
  `processBranchHintSection`), `Test/Test.pro` (`wasm-tools` conversion),
  `Test/wasm_wat/branch_hint_loop.wat`.

