# Language Profiles — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / forward-looking reference (revision 2 — the engine now
*declares* the full profile through `WASM::supportedProfile()`; selection,
marker gates and deterministic/scalar modes remain open).
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

- The engine is effectively a **full-profile** implementation of the features it
  has: core 2.0, reference types, SIMD and relaxed SIMD, GC, exceptions, typed
  function references, multi-value, multi-memory, memory64, tail calls and
  single-agent threads/atomics.
- There is no profile selector, no marker table and no way to reject a feature
  because a profile excludes it.
- There is no deterministic mode: float NaN results come from the host
  instruction, and relaxed SIMD deliberately chooses one legal result. There is
  no canonical-NaN normalisation.
- There is no scalar-only mode: a `v128` module is always accepted.
- There is no profile metadata section (the proposal does not define one).
  The supported profile is instead a compile-time capability statement in
  `WasmBase/WasmProfile.hpp`: `supportedProfile()` returns `Profile::Full`, and
  `supportsProfile()` answers false for `Deterministic` and `Scalar` (PRD2).
  This is an out-of-band declaration, not a selector (§3).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~PR1~~ | ~~**No profile declaration or selector.**~~ **Done (PR-1):** `WASM::supportedProfile()` declares the full profile and `supportsProfile()` refuses the narrower ones; the *selector* half remains open with PR-2. | `WasmBase/WasmProfile.hpp`; `Test/main.cpp` | Closed (declaration) |
| PR2 | **No deterministic profile.** NaN propagation is host-defined; relaxed SIMD is non-deterministic by design. | `LibJit` float lowering; `SIMD.md` relaxed section | Medium |
| PR3 | **No scalar profile.** SIMD is compiled whenever present. | `WasmOpcode.hpp` (`PrefixSIMD`), `LibjitOpcodeDispatcher.cpp` | Medium |
| PR4 | **No marker-level gates.** There is no way to reject threads, GC or exceptions as out-of-profile. | `WasmValidator.cpp` | Low |
| PR5 | **No tests.** There is no profile script in the snapshot. | `extern/WasmTestsuite` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PRD1 | **Be a full-profile engine (`{}`).** | §2 defines the full profile as `{}`; §3 permits supporting everything. | No configuration burden for the default. |
| PRD2 | **Do not claim deterministic or scalar support** until the gates and NaN rules exist. | §3: profiles restrict, and a false claim is worse than a missing one. | Honest capability reporting. |
| PRD3 | **Treat a profile as configuration, not as new semantics.** | §2 (restrict only) and §3 (no alternate semantics). | One code path, optional gates. |
| PRD4 | **Do not invent a metadata section.** | §3: the proposal defines none; a custom section would be non-standard. | Interoperability. |
| PRD5 | **Keep relaxed SIMD's legal non-determinism out of the deterministic profile.** | §2's deterministic profile excludes non-deterministic rules; §3 does not require them. | The profile definition stays sound. |

Guardrail: a module accepted under the full profile must behave identically
under any subset profile that accepts it. A gate may reject a module, but must
never change its results.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PR-1 — Declare the profile** | ✅ `WasmProfile.hpp` exposes `Profile`, `supportedProfile()`, `supportsProfile()` and `profileName()`; the full profile is the only claim (PRD1/PRD2). | PR1 | Done: the embedder can query the profile at compile time or run time. |
| **PR-2 — Marker gates** | Add a configuration of allowed markers and reject a module that uses an excluded construct. | PR1, PR4 | A `scalar` configuration rejects a `v128` module. |
| **PR-3 — Deterministic floats** | Normalise NaNs and pin relaxed-SIMD choices in a deterministic mode. | PR2 | The same inputs produce bit-identical results across hosts. |
| **PR-4 — Scalar mode** | Gate vector types and instructions when the scalar profile is selected. | PR3 | A SIMD module is rejected in scalar mode. |
| **PR-5 — Tests** | Add profile-positive and profile-negative tests. | PR5 | Both configurations run in the suite. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `the engine declares the full profile` | 7 assertions | 0 | 0 | `supportedProfile()` is `Full`; `Deterministic` and `Scalar` are explicitly not claimed |
| (no script) | — | — | — | no profiles script in the snapshot |

Reading: the declaration is graded directly. The SIMD scripts are full-profile
tests; the deterministic-profile appendix is a spec reference, not a test here.

## 9. References

- Proposal overview (`WebAssembly/profiles`):
  https://github.com/WebAssembly/profiles/blob/main/proposals/profiles/Overview.md
- Spec — Profiles appendix:
  https://webassembly.github.io/spec/core/appendix/profiles.html
- In-repo: [`SIMD.md`](SIMD.md) (relaxed SIMD and the deterministic appendix),
  [`THREADS.md`](THREADS.md) (the execution model a deterministic profile would
  restrict), [`../SIMD_BACKENDS.md`](../SIMD_BACKENDS.md),
  [`../VALIDATION.md`](../VALIDATION.md); sources `WasmBase/WasmProfile.hpp`
  (`Profile`, `supportedProfile`, `supportsProfile`, `profileName`),
  `WasmBase/WasmOpcode.hpp`, `WasmBase/WasmValidator.cpp`, `WasmBase/WasmType.hpp`
  (`v128`).
