# Constant-Time (`CT-Wasm`) — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 1 — the proposal is an
early extension; no secret type, secret memory flag or constant-time instruction
exists here).
Audience: anyone touching `WasmType` (value/memory types), `WasmOpcode`, the JIT
backends, or an embedder that needs side-channel guarantees.

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

CT-Wasm makes secrecy explicit in the type system so that cryptographic code can
be written with side-channel resistance.

- Two new secret numeric types, `s32` and `s64`, alongside `i32`/`i64`.
- A **secret memory flag** in the memory type: `memtype ::= secret? limits`.
  Secret loads/stores are valid only on a memory whose type is secret.
- A secret counterpart for every numeric operation that can be implemented in
  constant time on commodity hardware (add, sub, bitwise, shifts, comparisons
  where specified). `div` and `rem` are **excluded**.
- `iNN.declassify : sNN -> iNN` and `sNN.classify : iNN -> sNN` as the only
  explicit boundary between secret and public values.
- Control flow is **not** extended to secrets: branching on a secret is not
  allowed, and secret values cannot be used as memory indexes.
- These instructions must execute in constant time with respect to the secret
  operands (their timing must not depend on secret values).

## 3. What the proposal does not demand

- No particular lowering: masking, hardware constant-time instructions, or a
  compiler pass are all acceptable.
- No representation of `s32`/`s64` beyond their width and secrecy.
- No constant-time guarantee for public values or for excluded ops
  (`div`, `rem`, floating-point).
- No JS API and no new host interface.
- No validation strategy beyond the typing rules (the project still assumes
  valid input; `VALIDATION.md`).

## 4. What we have implemented today

- There are no `s32`/`s64` value types. `ValueTypeCode` has only `i32`, `i64`,
  `f32`, `f64`, `v128` and the reference types.
- `Module::validateValueTypes()` rejects a value type that
  `isModeledValueTypeCode` does not know, with a named
  "unsupported value type 0x.." error. An unknown secret encoding in a
  signature, local, global, table or element type therefore fails at load.
- `MemoryType` (`WasmType.hpp`) carries only `indexType` and `limits`; there is
  no secret bit and no behavior keyed on one. Unknown limit flags are preserved
  but otherwise ignored.
- There are no secret arithmetic or load/store opcodes and no
  `classify`/`declassify`; an unknown primary or `0xFC` sub-opcode hits the
  dispatcher's `default` and throws.
- The JIT emits ordinary native operations; no pass reasons about secrecy and
  no timing guarantee is made.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| CT1 | **No secret value types.** `s32`/`s64` are rejected (safely) as unmodelled value types. | `WasmType.hpp` (`ValueTypeCode`), `WasmModule.cpp` (`validateValueTypes`) | High |
| CT2 | **No secret memory flag.** `MemoryType` has no secret field and no memory operation checks one. | `WasmType.hpp` (`MemoryType`, `Limits`) | High |
| CT3 | **No secret instructions.** Arithmetic, load/store, `classify` and `declassify` do not exist. | `WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp` | High |
| CT4 | **No constant-time guarantee.** Even for public i32/i64 code, the timing depends on the host and on the backend's lowering. | `LibJit/LibjitOpcodeDispatcher.cpp`; no CT pass anywhere | High |
| CT5 | **No validation of the secrecy rules** (branching on secrets, secret indexes). | `WasmValidator.cpp` | Medium |
| CT6 | **No tests.** The CT-Wasm test suite is not part of the snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| CTD1 | **Reject `s32`/`s64` rather than alias them to `i32`/`i64`.** | §3: representation is free, but `SAFETY.md` makes it clear that silently dropping secrecy is a security bug, not a performance choice. | A secret module fails loudly. |
| CTD2 | **Make no constant-time claim for public code.** | §3: the guarantee is attached to secret types and specific ops. | No misleading security promises. |
| CTD3 | **Keep `classify`/`declassify` as explicit boundary instructions** when the types land. | §3: the lowering is free; §2 deliberately makes secrecy explicit. | Auditable transitions. |
| CTD4 | **Let the backend choose the CT primitive** (masking vs. hardware). | §3: lowering is free. | Portability across backends. |
| CTD5 | **Do not add a `secret` bit to every operation as a runtime flag.** | §3: representation is free; §2 makes secrecy static, and a runtime flag would leak through branches. | Compile-time checking and no per-op cost. |

Guardrail: an engine that cannot guarantee constant time must not accept a
module that relies on it. Until CT-1…CT-4 land, the only correct behaviour is
the current rejection.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CT-0 — Keep the rejection** | Ensure every secret type/opcode is rejected with a named error (no silent aliasing). | CT1, CT3 | A CT-Wasm module fails at load with a clear message. |
| **CT-1 — Secret types** | Add `s32`/`s64` to the type system and the validator's secrecy rules. | CT1, CT5 | A secret signature decodes and type-checks. |
| **CT-2 — Secret memory** | Add the memory flag and restrict secret loads/stores to it. | CT2 | A secret load on a public memory is rejected. |
| **CT-3 — Secret ops** | Implement the constant-time arithmetic and classify/declassify set. | CT3 | The arithmetic tests produce the same values as their public counterparts. |
| **CT-4 — Verify timing** | Add a lowering/verification pass (or documented backend contract) for constant time. | CT4 | A timing harness shows no secret-dependent branch or memory index. |
| **CT-5 — Tests** | Wire the proposal's tests and a manual timing smoke test. | CT6 | The CT tests run in the default suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| (none) | — | — | — | no CT-Wasm script in the testsuite snapshot |

Reading: nothing to grade; the current rejection path is the only tested
behaviour and it is covered indirectly by the unmodelled-value-type error.

## 9. References

- Proposal overview (`WebAssembly/constant-time`):
  https://github.com/WebAssembly/constant-time/blob/main/proposals/constant-time/Overview.md
- In-repo: [`../SAFETY.md`](../SAFETY.md), [`../VALIDATION.md`](../VALIDATION.md),
  [`../ABI.md`](../ABI.md), [`THREADS.md`](THREADS.md) (the only other proposal
  with an explicit execution-model restriction); sources
  `WasmBase/WasmType.hpp` (`ValueTypeCode`, `MemoryType`),
  `WasmBase/WasmModule.cpp` (`validateValueTypes`),
  `WasmBase/WasmOpcode.hpp`, `LibJit/LibjitOpcodeDispatcher.cpp`.
