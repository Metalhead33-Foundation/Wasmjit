# Compilation Hints and Relaxed Dead-Code Validation — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — both proposals are
unimplemented as such. The parser ignores all compilation-hint sections, and the
validator has an unreachable-stack model but does not claim the relaxed
dead-code algorithm.)

Audience: anyone touching `WasmModule` (custom sections), `WasmValidator`,
`WasmOpcodeDispatcher`, or the validation test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (two parser/validator proposals in
one note). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do Compilation Hints and Relaxed Dead-Code Validation demand? |
| §3 | What do they *not* demand? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

### 2.1 Compilation Hints

Compilation Hints adds optional, non-semantic custom sections following the
branch-hint shape. Each section is named `metadata.code.*`, is keyed by
function index, and carries `(byte offset, hint length, values)` records:

| Section | Meaning |
|---|---|
| `metadata.code.compilation_priority` | compilation order and optimization priority/hotness |
| `metadata.code.instr_freq` | instruction-level frequency, used mainly for inlining |
| `metadata.code.call_targets` | likely callees of a `call_indirect`/`call_ref`, with percentages |

The hints are advice only: ignoring them, or acting on them, cannot change
observable results. The proposal also defines text annotations for toolchains.

### 2.2 Relaxed Dead-Code Validation

The proposal relaxes validation of syntactically dead code. In dead code, any
type-stack constraint that depends on a pop is skipped, including the pop
itself. For example, `ref.is_null` in dead code does not require a nullable
reference on the type stack.

Syntactic restrictions remain: binary-format constraints, immediate widths,
`local.get` local-index bounds and similar type-stack-independent checks are
still performed. The proposal describes the validator as carrying an explicit
"bottom"/unreachable result rather than a fully polymorphic operand stack, and
gives an algorithm that skips pushes and pops while unreachable.

## 3. What the proposal does not demand

- **No runtime behaviour from Compilation Hints.** The sections have no
  semantics, no validation effect, and no required engine action.
- **No fixed consumer policy.** An engine may ignore all hints, use only one
  family, or change its heuristics per build.
- **No new instructions or types from Relaxed Dead-Code Validation.** It only
  changes which validation failures are reported for unreachable code.
- **No relaxation of syntactic checks.** Immediate bounds, label indices and
  other type-stack-independent checks still apply.
- **No requirement to accept every dead-code sequence.** An engine may remain
  conservative; the proposal describes a permitted relaxation, not a new
  runtime feature.
- **No text-format support in this engine.** Both proposals are consumed as
  binary modules; text annotations belong to `wasm-tools`, and this engine will
  never implement WAT parsing.

## 4. What we have implemented today

- `Module::processCustomSection` recognizes `name` and
  `metadata.code.branch_hint`; every other custom section is skipped by its
  section size. There is no parser or storage for `compilation_priority`,
  `instr_freq` or `call_targets`.
- The branch-hint parser already demonstrates the shared wire shape:
  function index, a vector of `(offset, length, payload)` records. Reusing that
  decoder for the new sections is straightforward; acting on the hints is not
  required.
- `WasmValidator` tracks an `unreachable` flag on each control frame.
  `popAny()` returns "unknown" when the operand stack is at the frame height and
  the frame is unreachable, so some pops are already skipped. `br_table` has a
  special unreachable path, citing `unreached-valid.wast`; `unreached-valid` is
  curated.
- That is close to the proposal, but the validator still models pushes and runs
  type-dependent checks in several handlers; it was not written as the
  proposal's "bottom type" algorithm. There is no named mode or comment claiming
  relaxed validation.
- There is no dedicated compilation-hint test, and no test that distinguishes
  the relaxed algorithm from the current conservative one.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| PP1 | **Compilation hints are ignored.** `processCustomSection` only dispatches `metadata.code.branch_hint`; the three sections are skipped without storage or policy. | `WasmModule.cpp` (`processCustomSection`); `WasmModule.hpp` has only branch hints. | Low (hints are optional) |
| PP2 | **The relaxed dead-code algorithm is not explicitly adopted or measured.** The validator's unreachable stack is close, but handlers still push values and perform type checks after an unreachable point; there is no dedicated conformance claim. | `WasmValidator.cpp` (`popAny`, `setUnreachable`, `endBlock`, instruction handlers); `VALIDATION.md` calls the stack polymorphic. | Medium |
| PP3 | **No focused coverage.** `unreached-valid` exercises existing unreachable typing, but there is no fixture for the proposal's "skip every pop-dependent check" cases or for the three hint sections. | `Test/wast_supported.txt`; `Test/main.cpp`; no hint fixture. | Medium |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PPD1 | **Ignore compilation hints by default.** Parse them only if a consumer asks, or skip the whole section by size. | §3: hints have no semantics and no required action. | Forward compatibility without a second heuristic layer. |
| PPD2 | **Keep branch hints as the only parsed `metadata.code.*` family** until a consumer needs the others. | §3: each hint family is independent and optional. | No dead storage or interpretation. |
| PPD3 | **Keep validation conservative until a concrete dead-code fixture fails.** | §3: relaxation is permitted, not required; a stricter validator still accepts all reachable-valid modules. | No accidental acceptance of a malformed reachable path. |
| PPD4 | **Never let a hint change decoding, validation or results.** | §3: hints are non-semantic; `SMALL_PROPOSALS.md` SPD3 already uses this rule for branch hints. | A malformed or hostile hint can at most be ignored. |

Guardrail: relaxed dead-code validation must still check immediates and label
indices; "dead" is not "unparsed". Compilation hints must never be used as a
substitute for the actual function index/offset validation.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PP-1 — Compilation hints** | Reuse the branch-hint record decoder for `compilation_priority`, `instr_freq` and `call_targets`; store them as inert metadata. Do not change codegen. | PP1 | A hand-built module with all three sections parses; results are unchanged with and without the sections. |
| **PP-2 — Relaxed dead code** | Adopt the proposal's skip-pops/pushes rule in the validator's unreachable path, keeping syntactic checks, and add the proposal's edge fixtures. | PP2 | The relaxed fixture accepts a type-stack-dependent operation in dead code while still rejecting a bad immediate or label. |
| **PP-3 — Tests** | Add a marker/instrumented fixture for hint parsing and a differential fixture for dead code. | PP3 | The tests fail if a hint changes results or if a syntactic check is skipped. |

PP-1 is optional in practice; PP-2 is only worth doing if a real module or a
curated proposal script needs it, since the current validator already accepts
`unreached-valid`.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `unreached-valid` | existing | existing | existing | curated; exercises current unreachable typing, not the proposal's full relaxed algorithm |
| compilation-hint sections | — | — | — | no script or manual fixture; sections are currently skipped |
| current `metadata.code.branch_hint` | existing | existing | existing | branch hints are parsed and deliberately non-semantic (`SMALL_PROPOSALS.md` §4) |

Reading: the branch-hint path is the reusable decoder, not evidence for the
compilation-hint proposal. No test currently distinguishes relaxed from
conservative dead-code validation.

## 9. References

- Compilation Hints:
  https://github.com/WebAssembly/compilation-hints/blob/main/proposals/compilation-hints/Overview.md
- Relaxed Dead-Code Validation:
  https://github.com/WebAssembly/relaxed-dead-code-validation/blob/main/proposals/relaxed-dead-code-validation/Overview.md
- In-repo: [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) §4 (branch hints),
  [`EXTENDED_NAME_SECTION.md`](EXTENDED_NAME_SECTION.md),
  [`ANNOTATIONS.md`](ANNOTATIONS.md) (the text-format annotation grammar),
  [`INSTRUMENT_TRACING.md`](INSTRUMENT_TRACING.md),
  [`VALIDATION.md`](../VALIDATION.md), [`ABI.md`](../ABI.md); sources
  `WasmBase/WasmModule.hpp` (`branchHintsByFuncIndex`),
  `WasmBase/WasmModule.cpp` (`processCustomSection`,
  `processBranchHintSection`), `WasmBase/WasmValidator.cpp`
  (`popAny`, `setUnreachable`, `dispatchBrTable`), `Test/wast_supported.txt`.

