# Stack Switching (Continuations) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — SS-0 rejection and
SS-1's continuation type/heap model are implemented; the runtime stack
capture/splice and instruction execution remain blocked).
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

- **SS-0 is implemented.** `0xE0`–`0xE6` are named opcodes and
  `dispatchOpcode` rejects every one with a named
  `stack-switching instruction is not implemented` error.
- **SS-1's type model is implemented.** `0x5D` parses as `ContType { funcType }`;
  `CanonTypeKind::Cont`, a canonical `contFunc` reference, `CoreCont` payloads
  and `AbstractHeapType::Cont`/`NoCont` (`0x68`/`0x75`) are represented. A
  `(ref null cont)` function parameter decodes and canonicalises correctly.
- No continuation instructions execute. The seven names are reserved opcodes
  that fail at validation/dispatch rather than reaching the generic unknown
  primary-opcode path.
- Exception tags exist (`TagId`, tag section, imports/exports, `throw` /
  `try_table`), but only with `attribute = 0` (exception); there is no
  continuation-signature variant.
- Execution uses the native C stack. Tail calls have a trampoline
  ([`TAILCALLS.md`](TAILCALLS.md)), but there is no stack capture, copy, restore
  or splice, and the execution model is single-agent
  ([`THREADS.md`](THREADS.md) §3).
- Added the hand-built test
  `stack-switching continuation types decode and opcodes fail clearly`, covering
  the `cont` type, `(ref null cont)`, and the named opcode rejection.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~SS1~~ | ~~**No continuation composite type.**~~ **SS-1 done:** `0x5D` parses into `ContType`, canonicalises to `CanonTypeKind::Cont` with a `contFunc` reference, and no longer desynchronises the type section. | `WasmType.hpp` (`ContType`), `WasmModule.cpp`, `WasmTypeRegistry.cpp` | Closed (model) |
| ~~SS2~~ | ~~**No `cont`/`nocont` heap types.**~~ **SS-1 done:** `AbstractHeapType::Cont`/`NoCont` and `(ref null cont)` canonicalisation are implemented. | `WasmType.hpp` (`AbstractHeapType`), `WasmValidator.cpp` | Closed (model) |
| ~~SS3~~ | ~~**No continuation instructions.**~~ **SS-0 done:** `0xE0`–`0xE6` are named opcodes and fail with a named error. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | Closed (rejection) |
| SS4 | **No continuation runtime.** The engine executes on the native C stack; there is no capture/restore/splice and no one-shot bookkeeping. This is the remaining blocker for executing `cont.new`/`resume`/`suspend`/`switch`. | `LibJit/LibjitModuleCompiler.cpp`, `WasmModuleInstance.cpp` | High |
| SS5 | **Tags carry no continuation signature.** The tag parser stores `attribute` and a payload type index only; `attribute` is always 0. | `WasmModule.cpp` (`processTagSection`), `WasmType.hpp` (`Tag`) | Medium |
| ~~SS6~~ | ~~**No test.**~~ **SS-0/SS-1 test added:** `cont` type and `(ref null cont)` decode, and `0xE0` fails with a named error. | `Test/main.cpp` | Closed (model/rejection only) |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| SSD1 | **Keep `0xE0`–`0xE6` rejected and model `0x5D` only as metadata** until the runtime exists. | §3: the stack representation and encoding are free; `SAFETY.md` forbids guessing the execution semantics, and a named failure is better than a dead stack. | The type surface is usable without pretending execution works. |
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
| **SS-0 — Honest rejection** | ✅ `0xE0`–`0xE6` are named opcodes that fail with a named error. | SS3, SS6 | A continuation module fails cleanly, without desync. |
| **SS-1 — Types** | ✅ `ContType`, `CanonTypeKind::Cont`, canonical `contFunc`, `CoreCont` payloads and `AbstractHeapType::Cont`/`NoCont`. | SS1, SS2 | The type section decodes and canonicalises a continuation type. |
| **SS-2 — Tag signatures** | Blocked on the continuation tag-signature encoding; extend the tag entry while keeping the exception form. | SS5 | A `(tag (cont ...))` decodes and imports/exports by `TagId`. |
| **SS-3 — Runtime stacks** | Blocked: add the capture/restore/splice primitive and one-shot state behind a backend contract. | SS4 | A simple suspend/resume round-trip runs. |
| **SS-4 — Instructions** | Blocked on SS-3; implement `cont.new`, `cont.bind`, `suspend`, `resume*`, `switch` and handlers. | SS3 | The proposal's positive tests pass. |
| **SS-5 — Tests** | SS-0/SS-1 model/rejection tests are in the default suite; positive continuation scripts need SS-3. | SS6 | The scripts are curated. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `stack-switching continuation types...` | 14 assertions | 0 | 0 | SS-0/SS-1: `cont` + `(ref null cont)` decode; `0xE0` fails with a named error |
| (none) | — | — | — | no stack-switching script; `switch.wast` is a `br_table` test |

Reading: the type/rejection surface is graded; the first positive test is SS-3's
suspend/resume round-trip, which remains blocked on a runtime stack model.

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
