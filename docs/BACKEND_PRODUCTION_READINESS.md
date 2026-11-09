# Backend Production Readiness {#doc_backend_production_readiness}

*What “production-ready” means for LibJIT, SLJIT and AsmJIT, measured*

Status: **snapshot (2026-11-09, working tree after the descriptor/trap/import batch)**.
This note owns the per-backend production verdict. The curated capability ledger
([`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §27 and §30) owns individual
implemented features; the two answer different questions.

Audience: anyone choosing a backend for an embedder, anyone planning to promote
SLJIT or AsmJIT, and anyone reviewing a status sentence in another note.

## 1. Verdict

| Backend | Production status | Recommended use |
|---|---|---|
| **LibJIT** | **Production-ready within the declared envelope** | The default embedder choice: single-agent, one instance per module, x86-64 Linux/SysV AMD64, QVM-style game/engine use. |
| **SLJIT** | **Production-ready within the same envelope, with the documented refusals below** | The second conformant code generator: no GNU LibJIT dependency, one executable mapping per function, and a named refusal where a signature cannot be staged. |
| **AsmJIT** | **Production-ready within the same envelope, with the documented refusals below** | The third conformant code generator; needs the external AsmJit library at build time. |

The green suite is now a **three-backend conformance result**, not a LibJIT-only
one: all three runs report the same 22,451 assertions in the same 821 cases.

## 2. Measurement

All commands were run from `build/Desktop-Release` with `--rng-seed 12345`.

| Run | Result |
|---|---|
| Default (`libjit`) suite | **22,451 assertions in 821 cases, green** (exit 0) |
| `WASMJIT_BACKEND=sljit ./Test/WasmJit` | **22,451 assertions in 821 cases, green** (exit 0) |
| `WASMJIT_BACKEND=asmjit ./Test/WasmJit` | **22,451 assertions in 821 cases, green** (exit 0) |

Identical assertion counts matter more than the pass count: every case executes
on every backend, so no case is skipped, refused early or cut short by a crash.

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
- `i31ref` at the C boundary and a literal C-caller conformance test remain open
  (`P2`/`P3` in [`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §10).
- A few invariant `std::abort()` guards remain; concurrent compilation and
  multiple instances per module are rejected by design.

**Verdict:** production-engine quality for a single-agent game/engine embedding
on x86-64 Linux, not a portable production WebAssembly runtime.

## 4. SLJIT — production-ready within the same envelope

SLJIT now completes the generic run without a refusal, an abort or a segfault.
The last blockers were both defects, not gaps:

1. **The Custom Descriptors instruction family** (`0xFB 32..38`) was LibJIT-only.
   `struct.new_desc`, `struct.new_default_desc`, `ref.get_desc`,
   `ref.cast_desc_eq(_null)` and `br_on_cast_desc_eq(_fail)` are now lowered
   through two helpers that mirror the engine's GC ABI.
2. **A native import with four parameters could not be called.** SLJIT's C-call
   form accepts four arguments and `VMContext*` is the first, so
   AssemblyScript's `env.abort(msg, file, line, col)` was refused at compile
   time — which failed three interop cases at instantiation. The backend now
   stages such a call and marshals it through a per-signature libffi closure
   ([`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §30).

Along the way the reference-ownership model was completed for the heap-creating
opcodes: a `struct.new*` result now carries the count the operand stack owns, and
every descriptor operand an instruction consumes is released.

Refusals that remain, each with a named reason and no test-coverage gap:

- an indirect call whose signature carries a `v128` **and** more than three
  parameters, in a module that also imports a native function;
- `return_call_ref` above the register-form argument ceiling;
- a signature above the register-form ceiling is otherwise handled by the
  module-wide frame form, as before.

**Verdict:** production-ready for the same envelope as LibJIT, with the three
refusals above documented rather than hidden.

## 5. AsmJIT — production-ready within the same envelope

AsmJIT completes the generic run without a refusal, an abort or a segfault. The
last blockers were:

1. **The Custom Descriptors family** — the same six opcodes as SLJIT, plus the
   descriptor-ownership rules.
2. **Trap-site attribution.** `unreachable` reported a trap with no location, and
   a helper-raised trap was never annotated, so
   `diagnostic.location.functionIndex` was `kNoDiagnosticIndex`. Both now carry
   the function index, the instruction offset and the interned `$name`.

Refusals that remain are the same three as SLJIT’s.

The backend’s README still describes it as a scalar-first native backend that is
not a drop-in replacement for LibJIT in *code quality*; that remains accurate.

**Verdict:** production-ready for the same envelope, with the documented
refusals and an external AsmJit build dependency.

## 6. Cross-cutting notes

1. **All three backends now run the same case set to completion.** The default
   suite is LibJIT’s suite, but `WASMJIT_BACKEND` selects a backend that now
   produces the same assertion count.
2. **The status documents agree.** This note, both backend READMEs,
   `HANDOFF.md`, `PRODUCTION_GAPS.md` §30, `TESTING.md`’s promotion table and the
   root `README.md` all report the same three-backend result.
3. **Refusal is still not failure isolation, but no wrong-result path remains in
   this snapshot.** A refusal is a compile error naming the function and the
   reason; the disqualifiers (wrong result, module-triggered abort or segfault)
   do not occur in any of the three runs.
4. **Portability is unproven outside x86-64 Linux.** The same `provisional`
   target matrix applies to every backend until a non-SysV runner exists.
5. **Negative-module coverage is opt-in.** `WASM_SPEC_VALIDATE=1` exercises the
   decoder/validator/linker negatives; the default suite does not.

## 7. The promotion gate

A promotion needs all of the following, and the status of each is recorded here:

| # | Criterion | Status |
|---|---|---|
| 1 | `WASMJIT_BACKEND=<name> ./Test/WasmJit` completes **green**, no abort, no segfault | **met for all three** (§2) |
| 2 | The spec suite and the strict `WASM_SPEC_VALIDATE=1` negative sweep pass on that backend | **met**; the runs are recorded in [`TESTING.md`](TESTING.md) “Required runs” |
| 3 | Every failure is fixed or an explicit, documented refusal; no wrong-result or crash path | **met**; the three refusals are listed in §4/§5 |
| 4 | The sanitizer run is recorded and clean for that backend | recorded in [`TESTING.md`](TESTING.md); the LibJIT caches are the only entries in `Test/asan.supp` |
| 5 | The target matrix is exercised beyond x86-64 Linux, or the claim is limited to SysV AMD64 | **limited to x86-64 Linux / SysV AMD64**, explicitly, for all three |
| 6 | README, backend READMEs, `HANDOFF.md`, `VALIDATION.md`, `API_ABI_STABILITY.md` and this note tell the same story | **met** |

Until a non-SysV runner exists, the supported production configuration is **any
of the three backends on x86-64 Linux**, and every portability statement in this
note is scoped to that target.

## 8. What this note does not count as a gap

- Proposals with no stable binary encoding, text-format support, WASI, a
  linker/scheduler, and a bespoke interpreter are scope boundaries, not
  production-readiness gaps ([`SCOPE.md`](SCOPE.md)).
- A backend is allowed to **refuse** an unsupported shape. The disqualifiers
  are a wrong result, a module-triggered abort/segfault, or a silent
  miscompile.
- LibJIT’s opt-in frame form is a policy variant, not evidence against the
  default register form.
- A reference *leak* is recorded as a known precision limit, not a wrong result
  ([`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §30).

## 9. Reproduction

```bash
cd build/Desktop-Release

# The gate, backend by backend
WASMJIT_BACKEND=libjit ./Test/WasmJit --rng-seed 12345
WASMJIT_BACKEND=sljit  ./Test/WasmJit --rng-seed 12345
WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345

# Targeted per-backend slices
./Test/WasmJit '[sljit]'  --rng-seed 12345
./Test/WasmJit '[asmjit]' --rng-seed 12345

# The strict negative sweep, on any one backend
WASM_SPEC_VALIDATE=1 WASMJIT_BACKEND=sljit ./Test/WasmJit
WASM_SPEC_VALIDATE=1 WASMJIT_BACKEND=sljit ./Test/WasmJit '[spec]'
```

All three of the first group are **green** at this snapshot. See
[`PRODUCTION_GAPS.md`](backends/PRODUCTION_GAPS.md) §30 for the defect ledger
that closed them.
