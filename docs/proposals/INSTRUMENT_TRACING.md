# Instruction Tracing (`i32.itt`) — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_instrument_tracing}

Status: design note / implementation reference (revision 2 — engine-reserved
encoding, NOP lowering and optional compile-time callback implemented; the
upstream proposal still assigns no encoding).
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

- The engine uses a **documented provisional encoding**: `0xFC 0x100`, followed
  by a LEB128 `uint32_t` immediate (`MiscOpcode::I32Itt`). The upstream overview
  does not assign one, so this is an engine extension, not a claim of upstream
  binary compatibility.
- `WasmOpcodeDispatcher::dispatchPrefixMisc` reads the immediate and calls
  `dispatchI32Itt`.
- The validator treats it as a pure marker: no operand-stack, control-flow or
  trap effect.
- The JIT lowers it to nothing; it calls the optional
  `WASM::setTraceCallback` hook at **compile time**, so a run has no extra
  branch or per-marker cost. The stub backend prints the marker.
- `Test/main.cpp` builds a module with `i32.itt 7` before `i32.const 42`,
  asserts the callback observes `7`, and asserts the function still returns
  `42`.

Because the proposal does not assign an encoding, this is not an upstream
conformance claim; it is the same "reserved encoding until the proposal settles"
policy used for `stringref` (`STRINGREF.md` STR1/STRD5).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| IT1 | **No upstream encoding to pin.** The proposal specifies the spelling and semantics but not the opcode/immediate bytes; the engine keeps its provisional `0xFC 0x100` extension. | `instrument-tracing` Overview; `WasmOpcode.hpp` (`I32Itt`) | Informational |
| ~~IT2~~ | ~~**No decoder/dispatch arm.**~~ **Done:** `0xFC 0x100` reads a LEB128 immediate and dispatches `dispatchI32Itt`. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | Closed |
| ~~IT3~~ | ~~**No trace hook.**~~ **Done:** `WASM::setTraceCallback` receives the immediate at compile time; the JIT emits no run-time marker code. | `WasmTracing.hpp`, `LibJit/LibjitOpcodeDispatcher.cpp` | Closed |
| ~~IT4~~ | ~~**No test.**~~ **Done:** the hand-built `i32.itt` test checks the callback and result equality. | `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| ITD1 | **Lower the marker to a plain NOP** once an encoding exists. | §3: no runtime effect is demanded. | Conformance with zero codegen cost. |
| ITD2 | **Ignore markers entirely** rather than emit trace records. | §3: the engine is free to implement the marker for its platform; it may also do nothing. | No tooling dependency in the engine. |
| ITD3 | **Use a documented reserved encoding until the upstream one is stable.** | §3: no encoding is demanded yet; `SAFETY.md` forbids guessing at unknown bytes, but an explicit engine extension is not a guess. | The marker can be exercised without claiming an upstream opcode. |
| ITD4 | **Offer an optional compile-time trace callback** instead of hard-coding one tool's format. | §3: format and API are free. | Embedders can plug in their own profiler. |

Guardrail: the marker must never change the result of a function. If an
encoding is added, a test must assert identical results with and without the
marker.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **IT-1 — Pin the encoding** | Track the proposal until an opcode byte and immediate encoding are assigned. | IT1 | The upstream proposal has a binary encoding; the engine can then replace its provisional `0xFC 0x100`. |
| **IT-2 — Decode as NOP** | ✅ Added `MiscOpcode::I32Itt` (engine-reserved `0xFC 0x100`), immediate decoding and validator no-op. | IT2 | A module containing the marker runs with unchanged results. |
| **IT-3 — Optional hook** | ✅ `WASM::setTraceCallback` receives the immediate during JIT compilation; no per-execution cost. | IT3 | The test observes the marker value. |
| **IT-4 — Test** | ✅ Hand-built module plus callback/result-equality test. | IT4 | The manual suite covers the marker. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `i32.itt is a run-time NOP...` | 6 assertions | 0 | 0 | callback sees immediate `7`; function still returns `42` |

Reading: the manual fixture is the only available test because the upstream
proposal has no test script or encoding. It grades both required properties:
the marker is observable at compile time and disappears from execution.

## 9. References

- Proposal overview (`WebAssembly/instrument-tracing`):
  https://github.com/WebAssembly/instrument-tracing/blob/main/proposals/instrument-tracing/Overview.md
- In-repo: [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) §4 (branch hints, the
  other deliberately non-semantic metadata), [`EXTENDED_NAME_SECTION.md`](EXTENDED_NAME_SECTION.md),
  [`../SAFETY.md`](../SAFETY.md); sources `WasmBase/WasmOpcode.hpp`,
  `WasmBase/WasmOpcodeDispatcher.cpp`.
