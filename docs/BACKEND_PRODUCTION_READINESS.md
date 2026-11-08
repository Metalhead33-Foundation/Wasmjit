# Backend Production Readiness {#doc_backend_production_readiness}

*What “production-ready” means for LibJIT, SLJIT and AsmJIT, measured*

Status: **snapshot (2026-11-07, working tree on `195e1ea`)**. This note owns the
per-backend production verdict. The curated capability ledger
([`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §27) owns individual implemented
features; the two answer different questions.

Audience: anyone choosing a backend for an embedder, anyone planning to promote
SLJIT or AsmJIT, and anyone reviewing a status sentence in another note.

## 1. Verdict

| Backend | Production status | Recommended use |
|---|---|---|
| **LibJIT** | **Production-ready within the declared envelope** | The only backend recommended for real embedding: single-agent, one instance per module, x86-64 Linux/SysV AMD64, QVM-style game/engine use. |
| **SLJIT** | **Not production-ready — provisional** | Targeted feature work and experiments. Do not offer it as a production backend. |
| **AsmJIT** | **Not production-ready — experimental** | Native-code experiments and a narrow slice. Do not offer it as a production backend. |

The green default suite is **LibJIT's suite**. It is not a three-backend
conformance result. A backend that passes a targeted tag can still fail the
generic module set, and both non-LibJIT backends currently do.

## 2. Measurement

All commands were run from `build/Desktop-Release` with
`--rng-seed 12345`.

| Run | Result |
|---|---|
| Default (`libjit`) suite | **22,442 assertions in 821 cases, green** |
| `[sljit]` targeted tests | **2,677 assertions in 69 cases, green** |
| `[asmjit]` targeted tests | **1,778 assertions in 61 cases, green** |
| `WASMJIT_BACKEND=sljit ./Test/WasmJit` | **10 / 12 cases passed, 2 failed**, then **segfaulted** (run stops early) |
| `WASMJIT_BACKEND=asmjit ./Test/WasmJit` | **16 / 21 cases passed, 5 failed**, then **segfaulted** |

The two non-LibJIT runs stopped early, so their failure counts are **lower
bounds**, not complete totals. “41 failures and an abort” is already enough to
disqualify a backend from production; the unmeasured remainder is additional
risk, not a reason to discount the measured part.

## 3. LibJIT — production-ready, within a declared scope

The G1 milestone in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.1 was
written for LibJIT, and it is met for the stated use case:

- broad proposal coverage, SIMD, exceptions, GC reclamation, continuations,
  atomics and tail calls;
- the trap path is a pending status converted to `WASM::Trap` at the host
  boundary; generated code does not unwind through C++;
- the embedding surface (`WASM::Runtime`, umbrella header, versioning, install,
  out-of-tree consumer) exists;
- one agent at a time is the default contract, with the opt-in agent model an
  additive extension.

The production claim is still **scoped**, not universal:

- **Only x86-64 Linux / SysV AMD64 is tested.** MS x64, AArch64 (Linux/macOS),
  Windows ARM64 and RISC-V are specified but *provisional*; big-endian and
  ILP32 hosts are unsupported by decision
  ([`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §9.3).
- The strict negative-module sweep is opt-in (`WASM_SPEC_VALIDATE=1`); the
  default suite is not the strict validator gate.
- The sanitizer run is a documented required run but is not recorded as part of
  the ordinary green result.
- `i31ref` at the C boundary and a literal C-caller conformance test remain open
  (`P2`/`P3` in [`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §10).
- A few invariant `std::abort()` guards remain; concurrent compilation and
  multiple instances per module are rejected by design.

**Verdict:** production-engine quality for a single-agent game/engine embedding
on x86-64 Linux, not a portable production WebAssembly runtime.

## 4. SLJIT — why it is not production-ready

SLJIT has substantial implemented coverage, and its targeted `[sljit]` tests
pass. The generic backend run does not. The failures include both expected
refusals and behaviour that is unacceptable in production:

### 4.1 General conformance failures

The `WASMJIT_BACKEND=sljit` run now completes 331 cases before the component
structured-value case crashes: 287 pass, 44 fail. The first batch closed
`func`, `float_memory64`, `table_fill`, the integer lane families, `br_table`
arity and the legacy-EH rethrow path. The largest remaining block is the
five-argument `simd_load32_lane` shape (`the SLJIT backend cannot pass 5
argument slots`) plus the unmeasured failures after the component crash.

### 4.2 Missing lowerings

`table.fill` and the integer lane families are implemented. Still missing or
restricted: relaxed dot product (`dispatchI16x8RelaxedDotI8x16I7x16S`), some
shuffle/transport shapes, and any signature that needs more than four SLJIT
argument slots (the register form refuses it; the module-wide frame form is the
intended escape hatch).

### 4.3 Miscompiles and structural limits

`float_memory64` now preserves bit patterns, `br_table` accepts differing
target arities and multi-result function labels, and the 16-byte `v128` result
path passes both the shuffle fixture and the three/four-parameter ABI fixture.
The remaining process-level defect is the component structured-value crash;
the targeted SIMD and tail-call regressions are green.

### 4.4 Process safety

The run still aborts with `SIGSEGV` in `component script marshals structured
values`. A backend that can abort the host process on module-derived input is
not production-safe regardless of how many targeted features pass.

**Verdict:** not production-ready. The targeted matrix is useful evidence of
implemented slices, not a general conformance or safety claim.

## 5. AsmJIT — why it is not production-ready

AsmJIT is explicitly experimental, and the generic run confirms why:

- the run reaches 331 cases: 266 pass and 65 fail before the component
  structured-value case segfaults;
- `spec: func` is now green (`100 passed / 0 failed`), including the frame-form
  function-label branches;
- `table.fill` and the integer lane families are implemented, but several SIMD
  scripts still report missing lane/transport opcodes (`dispatchI8x16ExtractLaneU`,
  `dispatchI64x2ExtractLane` and others);
- the `imports` script reports executable-memory allocation failures and trap
  classification mismatches;
- `start.7.wasm` fails with `AsmJit executable-memory allocation failed: 10`;
- tail-call tests **segfault**.

The backend's own README describes it as an experimental scalar subset and not
a drop-in replacement for LibJIT; that remains accurate.

**Verdict:** not production-ready. Its narrow targeted tests are useful for
native-lowering experiments, not for an embedder production decision.

## 6. Cross-cutting blockers

These are not opcode gaps; they make the current status hard to trust even where
a feature happens to work:

1. **The default suite is LibJIT-only.** `WASMJIT_BACKEND` selects a backend,
   but ordinary “green” means LibJIT green. SLJIT and AsmJIT need their own
   full-suite gate before either can be called production.
2. **The status documents disagreed.** The README and backend READMEs still
   described older slices, `HANDOFF.md` said no backend capability gap remained,
   and `VALIDATION.md` still described the removed `try_table` taint. This note
   and the corrections linked from it are the reconciliation.
3. **Refusal is not failure isolation.** A missing lowering that leaves a
   function uncallable is acceptable for an experimental backend; a wrong result,
   a process abort or a segfault is not. SLJIT and AsmJIT currently have all
   three classes in their generic runs.
4. **Portability is unproven outside x86-64 Linux.** The same `provisional`
   target matrix applies to every backend until a non-SysV runner exists.
5. **Negative-module coverage is opt-in.** Passing the default suite does not
   prove the validator rejects the strict negative corpus.

## 7. What would make SLJIT or AsmJIT production-ready

A promotion needs all of the following, not a subset:

1. `WASMJIT_BACKEND=<name> ./Test/WasmJit` completes **green**, with no abort
   and no segfault.
2. The spec suite and the strict `WASM_SPEC_VALIDATE=1` negative sweep pass on
   that backend.
3. Every failure is either fixed or an explicit, documented refusal; there are
   no wrong-result or crash paths.
4. The sanitizer run is recorded and clean for that backend.
5. The target matrix is exercised beyond x86-64 Linux, or the production claim
   is explicitly limited to SysV AMD64.
6. README, backend READMEs, `HANDOFF.md`, `VALIDATION.md`,
   `API_ABI_STABILITY.md` and this note tell the same story.

Until then the supported production configuration is **LibJIT on x86-64 Linux**,
and SLJIT/AsmJIT remain development backends.

## 8. What this note does not count as a gap

- Proposals with no stable binary encoding, text-format support, WASI, a
  linker/scheduler, and a bespoke interpreter are scope boundaries, not
  production-readiness gaps ([`SCOPE.md`](SCOPE.md)).
- A backend is allowed to **refuse** an unsupported shape. The disqualifiers
  are a wrong result, a module-triggered abort/segfault, or a silent
  miscompile.
- LibJIT's opt-in frame form is a policy variant, not evidence against the
  default register form.

## 9. Reproduction

```bash
cd build/Desktop-Release

# Default (LibJIT)
./Test/WasmJit --rng-seed 12345

# Targeted per-backend slices
./Test/WasmJit '[sljit]'  --rng-seed 12345
./Test/WasmJit '[asmjit]' --rng-seed 12345

# The gate a backend must pass before it is production
WASMJIT_BACKEND=sljit  ./Test/WasmJit --rng-seed 12345
WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345
```

The last two are **not green** at this snapshot. That is the gap this note
records; see [`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §29 for the ledger entry.
