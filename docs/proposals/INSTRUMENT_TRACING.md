# Instruction Tracing (`i32.itt`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — the proposal is an
early overview, assigns no binary encoding, and nothing is implemented).
Audience: anyone touching `WasmOpcode`, `WasmOpcodeDispatcher`, the JIT compiler,
or an embedder that wants trace hooks.

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

The Instrument and Tracing proposal is an early-stage tooling proposal. Its
whole demanded surface is one instruction, written `i32.itt immediate` in the
overview:

- It is a **no-op for execution**: it changes no result, no control flow and no
  trap behaviour.
- The immediate identifies a trace region: the first occurrence of a given
  immediate starts a trace, the second stops it.
- The engine may use the marker to connect generated machine code back to the
  Wasm source (for profiling, cache analysis, register-allocation studies).
- The overview says an implementation is free to implement the marker for its
  target platform; it does **not** specify a binary encoding, an opcode byte or
  a section.

## 3. What the proposal does not demand

- No pairing requirement: an implementation need not enforce start/stop pairs.
- No runtime semantics beyond "NOP"; no result, trap, memory or stack effect.
- No trace output format, no metadata section, no callback API.
- No encoding yet; the proposal is prose, so a decoder cannot be written against
  it without choosing an encoding first.
- No requirement that the instruction survive optimisation; eliding it is
  allowed as long as execution is unchanged.

## 4. What we have implemented today

- Nothing. `WasmBase/WasmOpcode.hpp` has no `I32Itt` (or similarly named)
  primary opcode.
- An unknown primary byte reaches `OpcodeDispatcher::dispatch`'s `default` and
  throws `"Invalid or unsupported primary opcode"`
  (`WasmBase/WasmOpcodeDispatcher.cpp`).
- There is no trace hook, no source-map metadata and no way for an embedder to
  observe a marker.

Because the proposal does not assign an encoding, there is not even a byte
string that could be used as a conformance input today.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| IT1 | **No encoding to implement.** The proposal specifies the spelling and semantics but not the opcode/immediate bytes. | `instrument-tracing` Overview | Informational |
| IT2 | **No decoder/dispatch arm.** Unknown primary opcodes are rejected, not treated as NOPs. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` (`default`) | Low |
| IT3 | **No trace hook.** Even if the marker decoded, nothing would emit a trace record or expose the immediate to an embedder. | no trace API in `WasmBase` / `LibJit` | Low |
| IT4 | **No test.** There is no script in the testsuite snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ITD1 | **Lower the marker to a plain NOP** once an encoding exists. | §3: no runtime effect is demanded. | Conformance with zero codegen cost. |
| ITD2 | **Ignore markers entirely** rather than emit trace records. | §3: the engine is free to implement the marker for its platform; it may also do nothing. | No tooling dependency in the engine. |
| ITD3 | **Reject the opcode until the encoding is stable.** | §3: no encoding is demanded yet; `SAFETY.md`: an unknown byte must not be guessed at. | No accidental interpretation of future opcodes. |
| ITD4 | **Offer an optional compile-time trace callback** instead of hard-coding one tool's format. | §3: format and API are free. | Embedders can plug in their own profiler. |

Guardrail: the marker must never change the result of a function. If an
encoding is added, a test must assert identical results with and without the
marker.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **IT-1 — Pin the encoding** | Track the proposal until an opcode byte and immediate encoding are assigned. | IT1 | The proposal has a binary encoding. |
| **IT-2 — Decode as NOP** | Add the primary opcode, read the immediate, emit nothing. | IT2 | A module containing the marker runs with unchanged results. |
| **IT-3 — Optional hook** | Expose the immediate to an embedder callback at compile time (no per-execution cost). | IT3 | A test observes both marker values. |
| **IT-4 — Test** | Add a hand-built module plus a result-equality test. | IT4 | The curated/manual suite covers the marker. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no tracing/`i32.itt` script in the testsuite snapshot |

Reading: nothing to grade. The first meaningful test is IT-2's
"same results with and without the marker".

## 9. References

- Proposal overview (`WebAssembly/instrument-tracing`):
  https://github.com/WebAssembly/instrument-tracing/blob/main/proposals/instrument-tracing/Overview.md
- In-repo: [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) §4 (branch hints, the
  other deliberately non-semantic metadata), [`EXTENDED_NAME_SECTION.md`](EXTENDED_NAME_SECTION.md),
  [`../SAFETY.md`](../SAFETY.md); sources `WasmBase/WasmOpcode.hpp`,
  `WasmBase/WasmOpcodeDispatcher.cpp`.
