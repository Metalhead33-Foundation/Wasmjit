# Stack Switching (Continuations) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — nothing is
implemented; the continuation type and the seven instructions do not decode).
Audience: anyone touching `WasmModule` (types), `WasmType` (heap types),
`WasmOpcode`, `WasmStore`, `WasmModuleInstance`, the JIT compiler, or the
exception-tag machinery.

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

Stack Switching adds typed continuations: a snapshot of execution on a stack that
can be suspended and resumed, enabling coroutines, generators, async/await and
lightweight tasks.

- A new composite type `cont $ft` (opcode `-0x23`, byte `0x5D`), where `$ft` is
  the function type of the continuation.
- Two new heap types: `cont` (`-0x18`) and `nocont` (`-0x0b`).
- Reuse of **tags** from exception handling, but with a continuation signature:
  the tag describes the values passed into and out of a suspension.
- Seven instructions (`0xE0`–`0xE6`):

| Opcode | Instruction | Role |
|---|---|---|
| `0xE0` | `cont.new $ct` | capture a `(ref $ft)` as a new continuation |
| `0xE1` | `cont.bind $ct $ct'` | partially apply a continuation |
| `0xE2` | `suspend $t` | suspend the current continuation on a tag |
| `0xE3` | `resume $ct hdl*` | resume a continuation under handlers |
| `0xE4` | `resume_throw $ct $e hdl*` | resume by throwing into a continuation |
| `0xE5` | `resume_throw_ref $ct hdl*` | the `exnref` form of the same |
| `0xE6` | `switch $ct $t` | symmetric switch to a peer continuation |

- Handler clauses are `(on $tag $label)` or `(on $tag switch)`; a resumed
  continuation is spliced onto the current one (asymmetric) or swapped with a
  peer (symmetric).
- Continuations are **one-shot** in the MVP: resuming a consumed continuation
  traps.

## 3. What the proposal does not demand

- No stack representation: copied stacks, segmented stacks or native stacks are
  all allowed.
- No scheduling policy and no thread semantics; switching is explicit.
- No multi-shot continuations in the MVP.
- No particular handler compilation, no unwind-table requirement.
- No change to the meaning of ordinary calls or exceptions; a continuation is
  not an exception object.

## 4. What we have implemented today

- No `cont` composite type: `Module::processSubtypes` recognizes `0x60` (func),
  `0x5F` (struct) and `0x5E` (array). The continuation prefix `0x5D` falls
  through without consuming its type payload, producing a default-empty subtype
  and desynchronising the rest of the type section.
- No `cont`/`nocont` heap types: `AbstractHeapType` stops at the GC and
  exception hierarchies. `ValueTypeCode::Ref`/`RefNull` still decode the outer
  reference form, so an unknown heap type is not rejected there.
- No `0xE0`–`0xE6` opcodes. `WasmOpcode.hpp` marks `0xD7`–`0xFA` reserved;
  `OpcodeDispatcher::dispatch`'s `default` throws
  `"Invalid or unsupported primary opcode"`.
- Exception tags exist (`TagId`, tag section, imports/exports, `throw` /
  `try_table`), but only with `attribute = 0` (exception); there is no
  continuation-signature variant.
- Execution uses the native C stack. Tail calls have a trampoline
  ([`TAILCALLS.md`](TAILCALLS.md)), but there is no stack capture, copy, restore
  or splice, and the execution model is single-agent
  ([`THREADS.md`](THREADS.md) §3).
- No test: the `switch.wast` in the snapshot is an old `br_table` test, not this
  proposal.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| SS1 | **No continuation composite type.** `0x5D` is neither recognized nor rejected; the type section desynchronises. | `WasmModule.cpp` (`processSubtypes`) | High |
| SS2 | **No `cont`/`nocont` heap types.** `AbstractHeapType` has no continuation hierarchy. | `WasmType.hpp` (`AbstractHeapType`) | High |
| SS3 | **No continuation instructions.** `0xE0`–`0xE6` reach the primary dispatch default and throw. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | High |
| SS4 | **No continuation runtime.** The engine executes on the native C stack; there is no capture/restore/splice and no one-shot bookkeeping. | `LibJit/LibjitModuleCompiler.cpp`, `WasmModuleInstance.cpp` | High |
| SS5 | **Tags carry no continuation signature.** The tag parser stores `attribute` and a payload type index only; `attribute` is always 0. | `WasmModule.cpp` (`processTagSection`), `WasmType.hpp` (`Tag`) | Medium |
| SS6 | **No test.** The snapshot has no continuation script (`switch.wast` is unrelated). | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SSD1 | **Reject `0x5D` and `0xE0`–`0xE6` with a named error** until the feature exists. | §3: the stack representation and encoding are free; `SAFETY.md` forbids guessing at unknown bytes, and silent desync is worse. | The current boundary is explicit (planned, see SS-0). |
| SSD2 | **Reuse the exception-tag identity, add a continuation signature variant.** | §3: handler/tag mechanics are free; `EXCEPTIONS.md` EX-2 already gives tags a process-wide `TagId`. | One tag registry for both proposals. |
| SSD3 | **Keep continuations one-shot.** | §3: multi-shot is explicitly out of the MVP. | No stack-copying collector. |
| SSD4 | **Do not emulate continuations with heap-allocated state machines.** | §3 leaves the mechanism free, but §2 requires suspending the actual execution state; a state-machine rewrite changes observable one-shot/splice semantics. | No false conformance. |
| SSD5 | **Let the backend choose the stack representation** (native copy or heap stack) behind a core contract. | §3: stack representation is free; `../LAYERING.md` keeps the contract in the core and the implementation in the backend. | LibJIT and future backends can differ. |

Guardrail: a continuation must resume exactly once. If the runtime cannot
observe a consumed continuation, it must reject the feature rather than let a
second `resume` read a dead stack.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SS-0 — Honest rejection** | Detect `0x5D` in the type section and `0xE0`–`0xE6` in code; fail with a named error. | SS1, SS3 | A continuation module fails cleanly, without desync. |
| **SS-1 — Types** | Add `cont $ft`, the `cont`/`nocont` heap types and their subtyping to `WasmType`/`WasmTypeRegistry`. | SS1, SS2 | The type section decodes and type-checks a continuation type. |
| **SS-2 — Tag signatures** | Extend the tag entry with the continuation signature form, keeping the exception form. | SS5 | A `(tag (cont ...))` decodes and imports/exports by `TagId`. |
| **SS-3 — Runtime stacks** | Add the capture/restore/splice primitive and one-shot state behind a backend contract. | SS4 | A simple suspend/resume round-trip runs. |
| **SS-4 — Instructions** | Implement `cont.new`, `cont.bind`, `suspend`, `resume*`, `switch` and handlers. | SS3 | The proposal's positive tests pass. |
| **SS-5 — Tests** | Add the stack-switching scripts (none in the current snapshot). | SS6 | The scripts are curated. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no stack-switching script; `switch.wast` is a `br_table` test |

Reading: there is nothing to grade. The first useful test is SS-0's
"reject without desync" case, followed by SS-3's suspend/resume round-trip.

## 9. References

- Proposal repository and explainer (`WebAssembly/stack-switching`):
  https://github.com/WebAssembly/stack-switching
  (https://github.com/WebAssembly/stack-switching/blob/main/proposals/stack-switching/Explainer.md)
- Spec — Exception tags (the reused identity):
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`EXCEPTIONS.md`](EXCEPTIONS.md) (tags, `TagId`), [`TAILCALLS.md`](TAILCALLS.md)
  (native-stack trampoline), [`THREADS.md`](THREADS.md) (single-agent model),
  [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), [`../ABI.md`](../ABI.md),
  [`../LAYERING.md`](../LAYERING.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`AbstractHeapType`, `Subtype`),
  `WasmBase/WasmModule.cpp` (`processSubtypes`, `processTagSection`),
  `WasmBase/WasmOpcode.hpp`, `WasmBase/WasmOpcodeDispatcher.cpp`,
  `LibJit/LibjitModuleCompiler.cpp`.
