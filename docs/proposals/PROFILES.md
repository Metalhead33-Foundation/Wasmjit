# Language Profiles — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — PR-1…PR-5 are
implemented: a marker/configuration model, the scalar gate, and the
deterministic∩scalar profile with canonical NaN normalisation. The spec's
`{N,T}` alone is not claimed; see §6/PRD6).
Audience: anyone touching feature gating, the validator, float semantics,
SIMD/threads availability, or a host that needs a restricted Wasm subset.

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

Profiles are **coarse-grained language subsets** for ecosystems with different
requirements (determinism, no threads, no vectors, small footprint).

- A profile is defined by a set of **markers**. A construct or rule annotated
  with a marker is omitted from that profile; a profile is the intersection of
  its markers' exclusions.
- Profiles may restrict the abstract syntax, the binary/text syntax and the
  execution rules, but they never add or change behaviour.
- Profile definitions live in an appendix of the spec, with a short summary of
  what is omitted.
- The initial profiles are **full** (`{}`), **deterministic** (excludes
  non-deterministic behaviour, e.g. the NaN choice; the example is `{N,T}`)
  and **scalar** (excludes vector types and instructions, `{V}`).
- An implementation declares which profile(s) it supports; profiles are not
  feature detection and not versioning.

## 3. What the proposal does not demand

- No alternate semantics: a profile can only restrict.
- No binary metadata section or runtime API in this proposal; a declaration is
  an out-of-band implementation statement.
- No mandatory list beyond the initial three; the number of profiles should stay
  small.
- No per-instruction feature flags; markers are coarse and ecosystem-wide.
- No change to a full-profile implementation: supporting everything is always
  conformant.

## 4. What we have implemented today

- The engine is a **full-profile** implementation of the features it has: core
  2.0, reference types, SIMD and relaxed SIMD, GC, exceptions, typed function
  references, multi-value, multi-memory, memory64, tail calls and single-agent
  threads/atomics.
- `WasmBase/WasmProfile.hpp` models the proposal's markers and configurations:
  `ProfileMarker` (`N` non-determinism, `T` threads, `V` vectors),
  `ProfileConfig` (a set of excluded markers), and the helpers `fullProfile()`,
  `deterministicProfile()` (`{N,T}`), `scalarProfile()` (`{V}`) and
  `deterministicScalarProfile()` (`{N,T,V}`). A profile is the intersection of
  its markers' exclusions (§2).
- `ValidationOptions::profile` carries the configuration into validation.
  `Module::fromFile(file, options)` gates at load, and
  `ModuleInstantiator::instantiate(module, resolver, bridge, profile)` gates
  again before allocating or compiling, so a module loaded under the full
  profile cannot bypass the exclusions.
- **Scalar gate (PR-4):** a `v128` value type is rejected wherever it can be
  declared (function signatures, struct fields, array elements, globals, tables,
  element segments and function locals), and `OpcodeDispatcher::onSimdOpcode`
  rejects every `0xFD` instruction — including a SIMD op in a module that
  declares no `v128` type.
- **Deterministic gates (PR-2):** `T` rejects shared memories, shared tables,
  shared globals, shared composite types, shared data/element segments and every
  `0xFE` atomic instruction (`onAtomicOpcode`). `N` rejects relaxed SIMD
  (`isRelaxedSimdOpcode`, the `0xFD 0x100`–`0x113` tail).
- **Deterministic NaNs (PR-3):** when `N` is excluded, the LibJIT backend
  canonicalises every NaN at a point where it becomes observable — `f32/f64`
  stores, `i32.reinterpret_f32`/`i64.reinterpret_f64`, `global.set`, and every
  function result (single and multi-value) — using the helpers in
  `WasmBase/WasmDeterminism.hpp` (canonical `0x7fc00000` / `0x7ff8000000000000`).
  Every other value passes through bit-exactly.
- **Capability:** `supportsProfile(Full|Scalar|DeterministicScalar)` is true and
  `supportsProfile(Deterministic)` is false; `supportsProfileConfig` accepts any
  configuration except a deterministic one that still admits vectors (§6/PRD6).
- There is no profile metadata section (the proposal defines none). A profile is
  configuration plus an out-of-band capability statement, not a selector carried
  in the module (§3, PRD4).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~PR1~~ | ~~**No profile declaration or selector.**~~ **Done (PR-1):** `WASM::supportedProfile()` declares the full profile and `supportsProfile()` refuses the narrower ones; the *selector* half remains open with PR-2. | `WasmBase/WasmProfile.hpp`; `Test/main.cpp` | Closed (declaration) |
| ~~PR2~~ | ~~**No deterministic profile.**~~ **Done (PR-3):** the `N` marker gates relaxed SIMD and canonicalises every observable scalar NaN (stores, reinterprets, `global.set`, returns). | `WasmDeterminism.hpp`; `LibjitOpcodeDispatcher.cpp`; `WasmValidator.cpp` | Closed |
| ~~PR3~~ | ~~**No scalar profile.**~~ **Done (PR-4):** `V` rejects `v128` declarations and every `0xFD` instruction. | `WasmValidator.cpp` (`checkProfile`, `onSimdOpcode`) | Closed |
| ~~PR4~~ | ~~**No marker-level gates.**~~ **Done (PR-2):** `T` rejects shared memories/tables/globals/types/segments and every `0xFE` atomic instruction; the hooks are shared with the base dispatcher. | `WasmValidator.cpp` (`checkProfile`, `onAtomicOpcode`); `WasmOpcodeDispatcher.hpp` | Closed |
| ~~PR5~~ | ~~**No tests.**~~ **Done (PR-5):** capability assertions, positive/negative fixtures for scalar and deterministic gates, and a bit-exact NaN-normalisation test. | `Test/main.cpp`; `Test/wasm_wat/profile_*.wat` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PRD1 | **Be a full-profile engine (`{}`).** | §2 defines the full profile as `{}`; §3 permits supporting everything. | No configuration burden for the default. |
| PRD2 | **Claim the full, scalar and deterministic∩scalar profiles, and refuse `{N,T}` alone.** | §3: profiles restrict, and a false claim is worse than a missing one. The NaN rules and gates now exist for the scalar surface. | Honest capability reporting. |
| PRD3 | **Treat a profile as configuration, not as new semantics.** | §2 (restrict only) and §3 (no alternate semantics). | One code path, optional gates. |
| PRD4 | **Do not invent a metadata section.** | §3: the proposal defines none; a custom section would be non-standard. | Interoperability. |
| PRD5 | **Keep relaxed SIMD's legal non-determinism out of the deterministic profile.** | §2's deterministic profile excludes non-deterministic rules; the `N` gate rejects the relaxed range. | The profile definition stays sound. |
| PRD6 | **Offer deterministic only as the `{N,T,V}` intersection (deterministic+scalar).** A SIMD float NaN can be observed byte-wise (`v128.store`), so deterministic SIMD would need per-lane normalisation. | §2 explicitly allows declaring an intersection; `supportsProfileConfig` refuses `{N,T}` without `V`. | An honest, fully normalised deterministic subset instead of a claim the engine cannot keep. |

Guardrail: a module accepted under the full profile must behave identically
under any subset profile that accepts it. A gate may reject a module, but must
never change its results.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PR-1 — Declare the profile** | ✅ `WasmProfile.hpp` exposes `Profile`, `supportedProfile()`, `supportsProfile()` and `profileName()`; the full profile is the only claim (PRD1/PRD2). | PR1 | Done: the embedder can query the profile at compile time or run time. |
| **PR-2 — Marker gates** | ✅ `ProfileConfig` (markers N/T/V) reaches validation through `ValidationOptions::profile`; `checkProfile` rejects shared constructs, and `onSimdOpcode`/`onAtomicOpcode` reject the prefixed instructions. `instantiate` re-validates under the profile. | PR1, PR4 | Done: `{T}` rejects a shared memory and an atomic op; the full profile accepts both. |
| **PR-3 — Deterministic floats** | ✅ Canonicalise every observable scalar NaN (`0x7fc00000` / `0x7ff8000000000000`) at stores, reinterprets, `global.set` and function returns; `N` also rejects relaxed SIMD. | PR2 | Done: a signalling NaN written by `reinterpret` round-trips bit-exactly under full and becomes the canonical NaN under `{N,T,V}`. |
| **PR-4 — Scalar mode** | ✅ `V` rejects `v128` declarations (types, fields, globals, tables, elements, locals) and every `0xFD` instruction. | PR3 | Done: both a `v128` type and an undeclared-type SIMD op are rejected in scalar mode. |
| **PR-5 — Tests** | ✅ Capability assertions, the scalar/deterministic gate fixtures and the bit-exact NaN test. | PR5 | Done: four test cases run in the default suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-24):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `the engine declares the full profile` | 19 assertions | 0 | 0 | full/scalar/deterministic+scalar claimed; `{N,T}` alone refused |
| `the scalar profile rejects vector types and SIMD instructions` | 7 assertions | 0 | 0 | a `v128` declaration and an undeclared-type SIMD op are rejected; `add_core` runs |
| `the deterministic profile rejects threads and relaxed SIMD` | 9 assertions | 0 | 0 | shared memory, atomics and relaxed SIMD are rejected; `add_core` runs under `{N,T,V}` |
| `the deterministic profile canonicalises observable NaNs` | 23 assertions | 0 | 0 | full preserves a signalling payload through reinterpret/return; `{N,T,V}` canonicalises store, reinterpret, `global.set`, single and multi-value returns (f32 and f64) |
| (no script) | — | — | — | no profiles script in the snapshot |

Reading: the gates are graded by load-time rejection with named messages, and
PR-3 by bit-exact payload comparison rather than a timing or cross-host test
(the canonical payload is fixed, so the assertion is host-independent).

## 9. References

- Proposal overview (`WebAssembly/profiles`):
  https://github.com/WebAssembly/profiles/blob/main/proposals/profiles/Overview.md
- Spec — Profiles appendix:
  https://webassembly.github.io/spec/core/appendix/profiles.html
- In-repo: [`SIMD.md`](SIMD.md) (relaxed SIMD and the deterministic appendix),
  [`THREADS.md`](THREADS.md) (the execution model a deterministic profile would
  restrict), [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md),
  [`../VALIDATION.md`](../VALIDATION.md); sources `WasmBase/WasmProfile.hpp`
  (`Profile`, `ProfileMarker`, `ProfileConfig`, `supportsProfileConfig`),
  `WasmBase/WasmValidation.hpp` (`ValidationOptions::profile`),
  `WasmBase/WasmDeterminism.hpp` (canonical NaN helpers),
  `WasmBase/WasmOpcodeDispatcher.*` (`onSimdOpcode`/`onAtomicOpcode`),
  `WasmBase/WasmValidator.cpp` (`checkProfile`), `WasmBase/WasmOpcode.hpp`
  (`isRelaxedSimdOpcode`), `WasmBase/WasmModuleInstance.cpp` (`instantiate`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`canonicalizeFloat`).
