# Compilation Hints and Relaxed Dead-Code Validation — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — PP-1 is
implemented: the three compilation-hint sections parse into inert metadata. The
validator still has an unreachable-stack model and does not claim the relaxed
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

- `Module::processCustomSection` recognizes `name`,
  `metadata.code.branch_hint`, `metadata.code.compilation_priority`,
  `metadata.code.instr_freq` and `metadata.code.call_targets`; every other
  custom section is skipped by its section size.
- The three compilation-hint families share the branch-hint wire shape
  (function index, a vector of `(offset, length, payload)` records) and are
  parsed by `Module::processCompilationHintSection` into
  `compilationPriorityHints`, `instructionFrequencyHints` and
  `callTargetHints` (`CompilationHint { offset, payload }`). The payload is kept
  raw — the families differ only in how a consumer would read it — and nothing
  in the engine consults it (PPD1/PPD4).
- The parser copies the rest of the section into a bounded buffer before
  decoding, so a declared hint length can never read past the section
  (`docs/SAFETY.md` §2.2) and a hostile hint can at most be rejected.
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
| ~~PP1~~ | ~~**Compilation hints are ignored.**~~ **Done (PP-1):** all three families parse into inert, section-bounded metadata; no consumer acts on them. | `WasmModule.cpp` (`processCompilationHintSection`); `WasmModule.hpp` | Closed |
| PP2 | **The relaxed dead-code algorithm is not explicitly adopted or measured.** The validator's unreachable stack is close, but handlers still push values and perform type checks after an unreachable point; there is no dedicated conformance claim. | `WasmValidator.cpp` (`popAny`, `setUnreachable`, `endBlock`, instruction handlers); `VALIDATION.md` calls the stack polymorphic. | Medium |
| PP3 | **No relaxed-dead-code coverage.** The three hint sections have a hand-built fixture, but there is no fixture for the proposal's "skip every pop-dependent check" cases. | `Test/main.cpp` (hint fixture); no dead-code fixture. | Medium |


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PPD1 | **Parse compilation hints but never act on them.** The records are stored as inert metadata; no codegen path reads them. | §3: hints have no semantics and no required action. | Forward compatibility without a second heuristic layer. |
| PPD2 | **Store the payload bytes without decoding each family** (a priority byte, a frequency byte, or callee/percentage pairs). | §3: each hint family is independent and optional. | No speculative interpretation; a consumer can decode what it needs. |
| PPD3 | **Keep validation conservative until a concrete dead-code fixture fails.** | §3: relaxation is permitted, not required; a stricter validator still accepts all reachable-valid modules. | No accidental acceptance of a malformed reachable path. |
| PPD4 | **Never let a hint change decoding, validation or results.** | §3: hints are non-semantic; `SMALL_PROPOSALS.md` SPD3 already uses this rule for branch hints. | A malformed or hostile hint can at most be ignored. |

Guardrail: relaxed dead-code validation must still check immediates and label
indices; "dead" is not "unparsed". Compilation hints must never be used as a
substitute for the actual function index/offset validation.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PP-1 — Compilation hints** | ✅ `processCompilationHintSection` parses the shared record shape into the three inert maps with section-bounded lengths; results are unchanged with and without the sections. | PP1 | Done: the hand-built three-section module parses and still returns 42. |
| **PP-2 — Relaxed dead code** | Adopt the proposal's skip-pops/pushes rule in the validator's unreachable path, keeping syntactic checks, and add the proposal's edge fixtures. | PP2 | The relaxed fixture accepts a type-stack-dependent operation in dead code while still rejecting a bad immediate or label. |
| **PP-3 — Tests** | ◑ The hint fixture is in place (with and without the sections, plus an over-long-length rejection); the differential dead-code fixture is not. | PP3 | Partially done: the hint test fails if a hint changes results; the relaxed/conservative differential is still missing. |

PP-1 is optional in practice; PP-2 is only worth doing if a real module or a
curated proposal script needs it, since the current validator already accepts
`unreached-valid`.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `unreached-valid` | existing | existing | existing | curated; exercises current unreachable typing, not the proposal's full relaxed algorithm |
| `compilation hints parse as inert metadata and change no results` | 25 assertions | 0 | 0 | all three sections parse; results are identical with and without them |
| `a compilation hint cannot claim bytes beyond its section` | 1 assertion | 0 | 0 | a declared length larger than the section is rejected (SAFETY) |
| current `metadata.code.branch_hint` | existing | existing | existing | branch hints are parsed and deliberately non-semantic (`SMALL_PROPOSALS.md` §4) |

Reading: the hint fixture grades PP-1, including the bounded-length rule. No
test yet distinguishes relaxed from conservative dead-code validation.

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
  `WasmBase/WasmModule.hpp` (`branchHintsByFuncIndex`,
  `compilationPriorityHints`, `instructionFrequencyHints`, `callTargetHints`),
  `WasmBase/WasmModule.cpp` (`processCustomSection`,
  `processBranchHintSection`, `processCompilationHintSection`),
  `WasmBase/WasmValidator.cpp` (`popAny`, `setUnreachable`, `dispatchBrTable`),
  `Test/wast_supported.txt`.

