# Study: the ABI refactor's blast radius, and portability before performance {#doc_abi_refactor_study}

**Status:** study, 2026-11-06. No code has been changed for this. It bounds the work before
it starts, because "everything" is not a plan.

This note answers two questions asked before authorising the `v128` / register-and-stack
refactor:

1. **What is "everything"?** A layer-by-layer inventory of what a canonical `v128` ABI
   actually touches.
2. **How do we achieve portability first and performance second**, given that every backend
   may implement the contract however it likes as long as the ABI and API stay compatible?

It sits downstream of [`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) (which owns the contract) and
[`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md) (which owns the
deferred decision). This note owns only the **work**: what moves, in what order, and what
proves each step.

## 1. Method, and why the count is not the estimate

The inventory was taken with `grep -rl v128` per area and by reading the subproject list. The
counts are *mentions*, not *sites that must change* — a file can mention `v128` in a comment
and need no edit, while a file that never mentions it can still be in the path (the plugin
descriptor, the ABI baselines). The counts show **where the mass is**; each row's "why" is the
actual work.

Measured mentions: `WasmBase` **26** files, `LibJit` 5, `Sljit` 5, `Asmjit` 4, `Test` **66**,
`docs` **50**.

## 2. What "everything" means — the blast radius

### 2.1 The layers, in dependency order

| # | Layer | Units that must change | Why |
|---|---|---|---|
| **L1** | **ABI definition** (the contract) | `WasmBase/WasmValue.hpp`, `WasmBackendAbi.hpp`, `WasmBackendSession.hpp`, `WasmGcHeader.hpp`, `wasm_simd_scalar.hpp`, `wasm_simd_vector.hpp`, `wasm_f16.hpp`, `WasmContinuation.hpp` | These *are* the value transport today. Each states or assumes how a `v128` crosses a boundary. |
| **L2** | **Core plumbing that moves values** | `WasmModuleInstance.{hpp,cpp}`, `WasmCallCache.{hpp,cpp}`, `WasmOpcodeDispatcher.{hpp,cpp}`, `WasmModule.cpp`, `wasm_simd_fused.cpp`, `WasmGcHeap.hpp` | Dispatcher hook signatures, the call cache's stored signatures, the fused trampolines, and every core helper that takes or returns a vector. |
| **L3** | **The three production backends** | `LibJit/` (9 files incl. `LibJitTypeTranslation`, `LibjitModuleCompiler`), `Sljit/` (prologue `beginFunction`, the `S0..Sn` copy, `emitHelper` sites), `Asmjit/` (`AsmjitTypeTranslator`, `ModuleCompiler` prolog + invokes) | Each owns its own argument transport: LibJIT builds `jit_type_t` signatures, SLJIT describes slots with `SLJIT_ARGSn`, AsmJit builds `FuncSignature`. A canonical `v128` signature is a different object in each. |
| **L4** | **Scaffolding backends** | `WasmStub/StubOpcodeDispatcher.{hpp,cpp}`, `ExampleBackend/` | The stub must keep compiling against the new headers; the example is the *template a future backend copies*, so it must show the new convention or it will teach the old one. |
| **L5** | **The plugin seam** | `WasmBackendPlugin.hpp` (descriptor), `LibJit/LibJitPlugin.cpp`, `docs/BACKEND_PLUGIN_ABI.md` | A descriptor carries an ABI version and a capability set; a canonical vector ABI bumps both. |
| **L6** | **Binding tooling** | `Tools/Bindings/**`, the `Bindings` subproject | A binding generator emits host-side signatures. Whatever it emits for `v128` today becomes part of the public ABI the moment the core changes. |
| **L7** | **Test infrastructure** | `Test/helper.cpp`, `Test/SljitBackendTests.cpp` and every `[parity]` case, the spec harness, `Test/wasm_wat/*` fixtures that pass or return vectors | 66 files mention `v128`. The parity cases are the *proof* the refactor preserves behaviour, so they must exist **before** the backends move. |
| **L8** | **ABI baselines** | `Test/abi/libWasmBase.exports`, `libLibJit.exports`, `README.md`, `./Test/abi-check.sh` | The additive-only rule (`G2`) makes the baseline diff the refactor's audit trail; a changed symbol set must be a recorded decision. |
| **L9** | **Documentation** | `ABI.md`, `ABI_PORTABILITY.md`, `WRITING_A_BACKEND.md` (§3, §15), `API_ABI_STABILITY.md` (`S12`), `EMBEDDING_SURFACE.md`, `BACKEND_PLUGIN_ABI.md`, `SIMD_BACKENDS.md`, `SIMD_LIBJIT.md`, `backends/V128_SIGNATURE_DECISION.md`, this note | 50 files mention `v128`. Documentation is compiled here: `[docs]`, `[headerdoc]` and Doxygen fail on drift, which is what keeps them honest. |

### 2.2 What is provably *not* in the blast radius

Naming these is half the value, because it is what makes the work finite:

- **`extern/`** — SLJIT, AsmJit and libjit are submodules and are never modified.
- **Loader, validator, parser, type store** — `WasmModule`, `WasmTypeStore`, `WasmCanonicalType`,
  `WasmValidator` deal in *types*, not transport. `v128` is already a value type to them.
- **GC object layout** — `WasmGcHeader.hpp` already defines a 16-byte, 16-aligned `v128` slot. The
  refactor changes how a vector **crosses a call**, not how it is stored.
- **The trap/status mechanism** — orthogonal; it is `R0`-shaped in every backend and does not care
  about vector registers.
- **The spec suite** — it asserts behaviour, not calling conventions.

### 2.3 The honest size

Four of the nine layers are small and mechanical (L4, L5, L8, L9 — a stub, a descriptor, a baseline
 diff, prose). Two are medium (L3 backends, L7 tests). Three are the actual project (L1, L2, L6): the
contract, the plumbing, and the tooling that emits it. **This is mostly a core-and-test project with
three backend ports attached — not a backend project.**

## 3. The five change classes (what actually changes, independent of layer)

Every edit in the nine layers belongs to one of five classes. This is the taxonomy the waterfall
phases are built on.

| Class | What changes | Where it lands |
|---|---|---|
| **C1 — Signature** | How a `v128` parameter and result are spelled in a C-callable signature: one canonical shape, chosen once (`P1`), expressed in every backend's own notation. | L1, L3, L9 |
| **C2 — Transport inside the frame** | How a vector reaches the callee's frame and leaves it: SLJIT's `S0..Sn` copy, AsmJit's `FuncDetail`, LibJIT's `jit_type_t` — three different mechanisms, one contract. | L3 |
| **C3 — Helper ABI** | Whether a lane/arithmetic helper takes `const void*` (portable, today) or a vector by value (fast, optional). The portable form stays normative. | L1, L2, L9 |
| **C4 — Observability** | Call-cache signatures, plugin descriptor version, ABI baselines, binding output: everything that *records* the signature rather than executing it. | L2, L5, L6, L8 |
| **C5 — Proof** | Parity tests, spec fixtures, and the negative cases that show a wrong convention is refused rather than miscompiled. | L7 |

## 4. Waterfall phases, with an exit criterion each

The order is forced by the dependency arrows, not by preference: **C5 before C1** (you cannot verify a
contract you have not specified in a test), and **C1 before C2/C3/C4**.

| Phase | Work | Exit criterion (the gate to the next phase) |
|---|---|---|
| **P0 — Freeze the decision** | Write the canonical `v128` signature into `ABI_PORTABILITY.md` §3 and close `P1`; state the Win64 by-reference consequence explicitly. | The decision is in prose, with its per-target consequence table. |
| **P1 — Prove the contract without a backend** | A core-level test that constructs a `Value` holding a `v128`, passes it through the canonical boundary, and asserts the bytes. No codegen. | The test passes on the current core; it is the specification the backends will be held to. |
| **P2 — Parity fixtures first** | Fixtures that *pass* and *return* vectors, plus a `[parity]` case asserting both backends agree — written against today's internal convention. | The fixtures fail loudly the moment a backend's convention changes without the other. |
| **P3 — Core plumbing (C4 before C1)** | Call-cache signatures, plugin descriptor version, binding emission — mechanical, and they make the later change visible. | `abi-check.sh` shows an intentional, recorded diff; the plugin refuses an old version. |
| **P4 — One backend at a time (C2)** | LibJIT first (it owns both sides of every call and cannot get the ABI wrong), then SLJIT (positional), then AsmJit (typed). | Full suite green after each; the other two backends untouched and still passing their own tags. |
| **P5 — Optional fast paths (C3)** | A backend may take a vector by value where its target's ABI makes that safe, behind a capability flag. | The portable path still passes the P2 parity case, *with the fast path disabled*, on every backend. |
| **P6 — Documentation and closure** | L9 in one sweep: `ABI.md`, `WRITING_A_BACKEND.md` §3/§15, `S12` row, plugin note, decision note's §11. | `[docs]`, `[headerdoc]`, Doxygen all clean; the decision note is no longer banner-marked DEFERRED. |

**Why LibJIT goes first in P4:** [`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §7.1 established that LibJIT
owns both sides of every call and bridges with a closure. It is therefore the one backend that *cannot*
introduce a calling-convention bug — so it is the cheapest place to discover that the *core* side of the
contract is wrong, before two positional/typed backends are changed on top of it.

## 5. Portability first, then performance — the two-tier rule

The contract has **two tiers**, and the ordering between them is not negotiable:

| Tier | Name | Status | Who may change it |
|---|---|---|---|
| **T1** | **the portable path** | **normative** — it *defines* correctness | nobody, without a decision note |
| **T2** | **a fast path** | **optional** — a per-backend, per-target optimisation | any backend, within the rules below |

### 5.1 The rules

| # | Rule |
|---|---|
| **R1** | **T1 defines correct.** A fast path that disagrees with T1 is a bug, not an optimisation. |
| **R2** | **Every fast path is disableable**, and with it disabled the backend must pass the *same* `[parity]` case. A fast path with no disable switch is not admissible. |
| **R3** | **Selection is by capability, never by backend name.** A backend declares what it can do; the core and the tests ask the capability. Code that says "if AsmJit then …" is a layering violation. |
| **R4** | **T1 is never deleted.** It is the reference semantics, the test oracle, and the fallback for targets nobody has ported. |
| **R5** | **Order of landing:** T1 green on *every* backend, then T2. A fast path may not land while any backend's portable path is untested. |
| **R6** | **A fast path must be measured**, and the note that introduces it must state the delta and the cost it imposes on the targets that do not get it (the precedent is [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md)'s helper-versus-native comparison). |
| **R7** | **No fast path may change the C-visible signature** ([`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §4). Performance may change *how* a value reaches a register, never *what a caller must write*. |

### 5.2 What this means concretely for `v128`

The decision note's §11 plan (lower a `v128` return as a multi-return through the results buffer) is
exactly a **T1** design: it is C-expressible, it is the same on every target, and it costs a
store/reload. What it buys is that all three backends can implement it *today*, including the ones
whose targets have no vector register class (RISC-V base psABI).

Each backend's **T2** then looks like this — all three different, all three allowed:

| Backend | T1 (portable) | T2 (fast, optional, capability-gated) |
|---|---|---|
| **LibJIT** | a `jit_type_t` aggregate / pointer | native vector type where libjit supports one for the target |
| **SLJIT** | two `W` slots (today's shape, made canonical) | a vector argument where the target ABI passes one in a single register (AAPCS64 `v`, SysV `xmm`) |
| **AsmJit** | a 16-byte aggregate in the `FuncSignature` | a 16-byte vector `TypeId`, which lets its allocator use `xmm`/`ymm` directly |

Note what T2 does *not* do: it does not change any function's C declaration. A caller still writes the
same signature; only the machine's use of registers differs, within what the target ABI permits.

### 5.3 The performance-proposal template

Any future T2 change must answer these, in this order, in its note:

1. **What does it speed up**, measured — with the benchmark and both numbers.
2. **Which targets** get it, and which fall back — named, not implied.
3. **How is it disabled**, and does the portable path still pass the parity case with it off.
4. **Does the C-visible signature change?** If yes, it is not a T2 change — it is a T1 decision and
   needs a decision note, not a patch.
5. **What does it cost** the targets that do not get it (code size, a copy, a helper call).

## 6. What must *not* change, whoever implements what

These are already frozen and this refactor must leave them alone — they are the reason the work is
bounded:

- C linkage, and `VMContext*` as arg0 on every compiled function ([`ABI.md`](ABI.md)).
- The trailing `void* results` buffer for multi-value returns.
- The GC object layout and the allocator call shape ([`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §8).
- Trap kinds as returned statuses, never host kills ([`SAFETY.md`](SAFETY.md) §2.6).
- The four-argument ceiling on helper calls, and the rule that a helper either derives its arguments
  from the object or receives them packed.

A change that breaks one of those is out of scope for this refactor, whatever its performance case.

## 7. Risks, with the mitigation each one already has

| Risk | Why it is real here | Mitigation |
|---|---|---|
| **Two conventions alive at once** | §25/§48/(§34): every partial conversion in this arc left a tree that *compiled* and was wrong | The parity case is the gate: a half-converted backend fails it, not the suite |
| **A positional ABI site silently mis-assembled** | SLJIT's `SLJIT_ARGSn` caused three separate investigations (§30, §44, §49) | Invariants in primitives (`emitTrap`, `emitPendingTrapCheck`), plus the `[parity]` tag on every transport test |
| **Baseline diff hides an accidental change** | `abi-check.sh` compares symbol *sets* | Phase P3 records the intended diff *before* the signature change, so a later surprise is attributable |
| **Binding output drifts** | `Tools/Bindings` emits from the core's own declarations | Regenerate in P3 and diff; a binding that emits a vector differently from the core is a defect by definition |
| **Docs drift** | 50 files mention `v128` | `[docs]`, `[headerdoc]` and Doxygen fail the build on drift — the mechanism already exists |
| **The one site that resists conversion** | `Unreachable` still cannot move to `emitTrap` (§49, §50) | It is the best available canary: if the new convention fixes it, the convention is sound |

## 8. Open questions this study deliberately leaves open

1. **Which T1 shape** — the two-`u64` aggregate, the memory/pointer form, or "refuse `v128` in
   C-callable exports". The decision note owns this; the study only requires that it be *one* shape
   on all targets.
2. **Does the binding generator emit vectors today?** If it does, its output is already part of the
   public surface and Phase P3 must decide whether that is intended.
3. **Plugin descriptor versioning** — a bump with no deprecation window is a hard break for an
   out-of-tree backend. The plugin note must decide.
4. **The measured cost of T1.** Without it, no T2 proposal can satisfy R6, and the refactor would be
   optimising on faith.

## 9. References

- [`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) — the contract, `D-ABI-1..7`, open row `P1`.
- [`ABI.md`](ABI.md) — the signature, arg0, the multi-value buffer.
- [`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md) — the deferred
  decision and its §11 plan.
- [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §3, §12, §15 — the obligations a backend must meet,
  and the lessons a fourth author inherits.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) — the frozen surfaces, including `S12`.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the descriptor that must version-bump.
- [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) — the precedent for a measured helper-versus-native choice.
- [`backends/ASMJIT_SLJIT_P3_GC1.md`](backends/ASMJIT_SLJIT_P3_GC1.md) — the arc this refactor
  inherits: 51 entries of gaps, reversals and lessons, which is why the phases above insist on a
  proof before a patch.

## 10. Progress log (waterfall)

| Phase | State | Evidence |
|---|---|---|
| **P0 — Freeze the decision** | **DONE 2026-11-06** | `ABI_PORTABILITY.md` §3 row and §5.4 note; open row `P1` closed; [`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md) §28 with the per-target table and the cost stated up front |
| **P1 — Prove the contract without a backend** | **DONE 2026-11-06** | `Test/HeaderTierTests.cpp`: `[v128][abi][layout]`, 8 assertions — storage size/alignment, the two-word decomposition round-trip, and the little-endian word order on LE hosts |
| **P2 — Parity fixtures first** | **partly pre-existing** | A `v128` *parameter* already passes through both backends and is asserted in `[parity]` (`v128_param` fixture); a *returning* fixture cannot exist until a return convention does — that is P4's first observable |
| **P3 — Core plumbing (C4)** | not started | call-cache signatures, plugin descriptor version, binding emission |
| **P4 — One backend at a time (C2)** | not started | LibJIT first, then SLJIT, then AsmJit |
| **P5 — Optional fast paths (C3)** | not started, and blocked by R6 (needs the T1 measurement first) | — |
| **P6 — Documentation closure** | partly pre-existing | this note, `S12`, and the decision note's §28 already reference each other; the sweep lands with P4 |

### The decision, in one line

> `v128` is passed as **two `uint64_t` parameters, low word first**, and returned through the
> multi-value buffer as `lo, hi` — the only spelling that is register-passed on SysV AMD64, MS x64,
> AAPCS64 and RISC-V alike, because it is two scalars rather than a 16-byte aggregate (and MS x64
> passes aggregates by reference).

### Why P0/P1 came first, measured

Three times in the arc the same order decided the outcome: a predicate tested without a backend cost
one patch, while the same work started at a backend cost three rewrites (§12, §30, §37). P1 is that
discipline applied to a calling convention: the decomposition is now pinned by a test that cannot fail
for a codegen reason, so when P4 starts, any failure is a *backend* failure and nothing else.

### A doc defect P1 caught immediately

`ABI_PORTABILITY.md` §8.1 named `gcStorageAlignment`, which does not exist — the function is
`gcStorageAlign`. The test failed to compile against the documented name, which is exactly what a
contract-first step is for: the prose and the code disagreed, and the disagreement surfaced in minutes
instead of during a backend port.

## 11. P3 findings: two places already record a *different* vector convention (2026-11-06)

P3's job is to make the signature visible before changing it. It found two surfaces that already
encode a vector convention — and both contradict the P0 decision.

### F1 — the libffi call cache builds an aggregate, not two scalars

`WasmBase/WasmCallCache.cpp` classifies a `v128` as a **16-byte, 16-aligned struct of four `i32`**:

```cpp
ffi_type* v128Type()
{
static ffi_type* const elements[] = {
		&ffi_type_uint32, &ffi_type_uint32, &ffi_type_uint32, &ffi_type_uint32, nullptr };
	static ffi_type type{16, 16, FFI_TYPE_STRUCT, const_cast<ffi_type**>(elements)};
	return &type;
}
```

On SysV this happens to land in the same two GPRs as two `uint64_t` — which is why nothing has broken.
On **MS x64 it is passed by reference**, while the decision requires two registers. So the libffi fast
path and the compiled-function path would use different conventions on Windows, and the fast path is
the one an embedder reaches first through `ModuleCallCache`.

**Why it was not fixed in this pass:** the fix is not `v128Type()`. A `v128` parameter must push **two**
`ffi_type_uint64` entries into `argTypes` (two sites, `WasmCallCache.cpp` lines ~114 and ~219) **and**
the `ffi_call` value marshalling must supply two `uint64_t` slots where it currently supplies one
16-byte block. Changing the types without the marshalling produces a call that libffi cannot encode —
a silent mismatch of exactly the kind this arc keeps finding. Both halves move together, with a test
that calls a vector-taking export through the cache and compares against the compiled path.

### F2 — the binding generator emits an aggregate too

`Tools/Bindings` already emits a vector type, in four places:

| File | Emission |
|---|---|
| `EmitCpp.cpp` (×2) | `struct wasm_v128 { std::uint8_t bytes[16]; };` |
| `EmitDynamic.cpp` (×3) | `ctypes.c_ubyte * 16`, `uint8_t[16]`, `ubyte[16]` |

That is the aggregate spelling the decision rejected, published to every binding consumer. It must
become the canonical pair — two `uint64_t` (or the language's `uint64` pair) in `lo, hi` order — or the
generated bindings will disagree with the runtime on Windows for the same reason as F1.

### What F1 and F2 mean for the phase order

P3 was scheduled as "mechanical, and they make the later change visible". It is visible: **two subsystems
that are not backends already disagree with the decision.** That strengthens the study's §2.3 conclusion
(the refactor is a core-and-test project, not a backend project) and adds a requirement to P4: a backend
may not be converted until the call cache and the bindings emit the same convention, or the tests would
compare two different ABIs and call the difference a parity failure.

### Revised P3 exit criterion

> The call cache and the binding generator emit the canonical **two-word** vector convention, and a test
> calls one vector-taking export **through the libffi cache** and one **through the compiled path**,
> asserting identical results. The plugin/runtime version constants are bumped with a recorded note.

That is the gate P4 inherits; it is not yet met.

### F1's exact fix, specified for the next pass

Read against `WasmCallCache.cpp`, the change is five coordinated edits — which is why it is written
down rather than half-applied:

1. Add `std::vector<ValueTypeCode> paramKinds;` to `ModuleFfiEntry`.
2. Populate it at both entry-construction sites (module cache ~line 114, component cache ~line 219).
3. Expand the arg-type array: a `V128` parameter pushes **two** `&ffi_type_uint64` instead of one
   aggregate (both sites).
4. In `ModuleCallCache::call`, replace `arguments[i + 1u] = args[i]` with a slot-walking loop that
   pushes `bytes` and `bytes + 8` for a vector parameter; `arguments` is already sized from
   `entry.argTypes.size()`, so the count stays consistent once (3) and (4) move together.
5. A test that calls a `v128`-taking export **through the cache** and **through the compiled path** and
   asserts identical bits — the parity case P4 will otherwise be unable to interpret.

Steps (3) and (4) are inseparable: types without marshalling is a call libffi cannot encode, and
marshalling without types writes past the argument array.

### F1 and F2: what was fixed, and what remains (2026-11-06, later the same day)

**F1 — the libffi call cache: fixed.** `ModuleFfiEntry` now carries `paramKinds`, the argument-type
array expands a `V128` parameter into **two `ffi_type_uint64`** entries, and `ModuleCallCache::call`
walks argument slots so a vector supplies `bytes` and `bytes + 8`. The five coordinated edits landed
together, as §11 required.

Proof, and it is a real one: `[callcache][v128][abi][parity]` calls `v128_param.wasm` **through the
cache** and **through the compiled function pointer**, asserting equal results for `lane1` and for
`lane3_after_scalar` — the latter chosen because a scalar precedes the vector, so a slot-numbering
error cannot hide. The pre-existing `module call cache maps v128 by value` case still passes, which is
the expected outcome on SysV: two integer words and a 16-byte integer-pair aggregate occupy the same
registers there. **The change is observable on MS x64, which is exactly why it was worth making.**

**F2 — the binding generator: half fixed, deliberately.** `EmitCpp` declared `struct wasm_v128` twice
and **never used it in a signature**; both declarations now emit the canonical pair with a comment
saying a parameter is two `uint64_t`, never the struct. One test pinned the old text
(`BindingSchemaTests.cpp`, "host-side callable typedefs follow the engine ABI"); the expectation was
updated **with a comment citing the decision**, because the test pins the ABI spelling and the ABI
spelling changed by decision.

**Still open:** `EmitDynamic.cpp` maps a vector in **five** places to a 16-byte array
(`ctypes.c_ubyte * 16`, `uint8_t[16]`, `ubyte[16]`, `array[16, uint8]`, `StaticArray(UInt8, 16)`).
Those are dynamically-typed FFI spellings and each needs its parameter *and* value marshalling moved
together — the same rule as F1 — and there is no output test for them yet. That is the remaining P3
work, and it is specified here rather than half-applied.

### P3 status

| Surface | State |
|---|---|
| libffi call cache (module) | **fixed**, parity-tested |
| component call cache | unchanged — it has its own `AbiCoreType` ABI and no `v128` path |
| C++ binding typedefs | **fixed** (canonical pair emitted, expectation updated) |
| dynamic bindings (5 sites) | open, specified |
| plugin/runtime version bump | still to do with P4, when the compiled convention actually moves |

Suite **21560 assertions in 726 cases**, green.

## 12. P4, backend one: LibJIT — the exact migration (specified 2026-11-06)

LibJIT is first because it owns both sides of every call and bridges with a closure
([`ABI_PORTABILITY.md`](ABI_PORTABILITY.md) §7.1): it cannot get the *calling convention* wrong, so any
failure it produces is a failure of the **core side** of the contract. That makes it the cheapest place
to be told we are wrong.

### What it has today

- `OpcodeDispatcher::v128Type()` builds a 16-byte `jit_type_t`; a `v128` parameter, local and block
  value are all that one struct type, and a vector-typed local is zeroed by a helper
  (`wasm_v128_zero`) because a struct-typed slot is otherwise uninitialised.
- Parameters are read **positionally**: `jit_value_get_param(fn, i + 1)` (index 0 is the context), and
  the same positional assumption appears in the import thunk (`callArgs[1 + p] = get_param(thunk, p + 1)`).

### The migration, in four parts

| Part | Change | Hazard |
|---|---|---|
| **1. Signature** | a `v128` parameter contributes **two** `jit_type_ulong` entries instead of one 16-byte struct | none — it is construction |
| **2. Parameter binding** | `locals.push_back(get_param(fn, i + 1))` becomes a **param-index walk**: each Wasm parameter advances the index by 2 for a vector and 1 otherwise; the two halves are combined into the internal 16-byte value | **this is the slot-numbering hazard**, and it is the one that cost SLJIT three rounds (§30, §44, §49) |
| **3. Import thunk** | `callArgs[1 + p]` obeys the same walk in reverse: a vector argument supplies its two words | same hazard, mirrored |
| **4. Proof** | `[libjit]` vector tests plus the existing A/B case; a fixture that puts a scalar *before* and *after* the vector (`lane3_after_scalar` is the model) | a wrong walk shifts every later parameter and is otherwise invisible |

### Why the walk is written down as a *data* structure, not as arithmetic

The SLJIT experience is unambiguous: three separate corruption investigations came from hand-maintained
parameter/register arithmetic (§30 mis-packed arguments, §44 a clobbered pointer, §49 a park in the
wrong place). The LibJIT migration must therefore precompute, once per function, a
`std::vector<ParamSlot> { wasmIndex, isVector, firstIndex }` and use *it* in all three places (binding,
thunk, and any later re-indexing) — one source of truth for a mapping that is otherwise recomputed by
hand in three loops. That is the `payloadSlotOffset` lesson (invariant in one place) applied to
parameter layout.

### The gate

> LibJIT passes its own tag, the A/B vector case, and a fixture with a scalar **after** a vector
> parameter — with the internal struct representation unchanged, so the migration touches only the
> boundary.

Not started: this section is the specification the next pass executes, because the change is exactly the
class of edit that must not be half-applied. Suite **21560 assertions in 726 cases**, green.

### F2 second pass: the emitters and the conformance harness must move together (2026-11-06)

The C typedef (`EmitShared.cpp`) and the Rust struct (`EmitRust.cpp`) were both changed to the
canonical pair — `{ uint64_t lo; uint64_t hi; }` and `{ pub lo: u64, pub hi: u64 }` — with comments
saying a vector parameter is emitted as two scalars, never as the struct.

**Three tests failed, and they are the good kind:** they *generate* the C header, compile it with gcc
and **run a conformance program** against it (`BindingSchemaTests.cpp` lines 157 and 497, reached from
cases at 1116 and 1187). Changing the emitted type without changing what the conformance program does
with a vector argument is exactly the "types moved, marshalling did not" mistake this whole phase
exists to prevent — so the change was reverted rather than shipped with a broken gate.

What the next pass must do, in one commit:

1. Emit the canonical pair in `EmitShared.cpp` (the C typedef), `EmitRust.cpp`, and `EmitWeb.cpp`.
2. Update the emitted parameter lists so a vector parameter is **two scalar parameters**, not one
   struct-typed parameter — in `EmitShared.cpp`'s kind→type mapping (three sites), `EmitDynamic.cpp`
   (five sites), `EmitWeb.cpp`, and `EmitRust.cpp`.
3. Update the conformance program's vector handling in the same commit, so the compile-and-run gate
   stays green *while* the spelling changes.

### Inventory of vector spellings after this pass

| Emitter | State |
|---|---|
| `EmitCpp.cpp` | **canonical** — emits `struct wasm_v128 { uint64_t lo, hi; }`, expectation updated |
| `EmitShared.cpp` | aggregate (C typedef + three kind→type sites) — reverted, specified above |
| `EmitRust.cpp` | aggregate — reverted, specified above |
| `EmitWeb.cpp` | aggregate — not attempted |
| `EmitDynamic.cpp` | aggregate in **five** sites — not attempted |

The pattern across this phase is consistent and worth stating once: **every surface that records a
signature must move with the surface that marshals it.** The call cache taught it with libffi (types
and values), the emitters teach it with the conformance gate (declaration and program). That is why P4
cannot begin until the emitters are consistent: a backend converted against a cache or a header that
still speaks the old convention would pass its own tests and fail on the generated interface.

## 13. The one ABI break this phase has taken, and why it is safe to take

`WASM::ModuleFfiEntry` gained `paramKinds`, so a public POD struct changed size and layout. Recorded
as **B1** in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2 rather than left implicit, because
`S11` freezes the entry records and a struct layout is not a symbol: `abi-check.sh` reports **0 removed
and 149 added** (the additions are the new inline helpers in `WasmBackendAbi.hpp`, which become weak
symbols), so a symbol-level gate would have called this change clean while an embedder built against
the old header would have read the wrong offsets.

**Why this break is justified**, in the terms the milestone requires: the two-word vector decision is
unimplementable without it (the cache cannot expand a parameter into two argument slots unless it knows
which parameters are vectors), the break is a *size* change in one record rather than a signature
change, and the migration is trivially mechanical for an embedder (rebuild). It is additive in spirit —
no field was removed, renamed or reordered — which is why the symbol set only grew.

**The general lesson:** the additive-only rule measures the wrong thing for PODs. A gate that compares
export sets will always pass a struct that grew. Struct layouts need their own audit — a `sizeof`/
offset assertion, or a designated-initialiser policy — and this phase is the reason to think so: the
first ABI break of the refactor was invisible to the gate that exists to catch ABI breaks.

### The gap B1 exposed is now closed

`[callcache][abi][layout]` pins the `ModuleFfiEntry` record: declaration order, the monotonic offsets,
`paramKinds`'s position immediately after `paramCount`, the size covering the last member, and
`sizeof % alignof == 0`. That is the audit `abi-check.sh` cannot perform, and it is a test rather than
a convention — an accidental field insertion or reorder now fails in CI instead of silently changing an
embedder's offsets.

The assertions are deliberately **relational** (each offset greater than the previous) rather than
literal, because a literal layout is stdlib- and platform-specific. The comment says so, so nobody
"strengthens" them into a number that fails on the next toolchain. The one literal that *should*
exist — a generated offsets baseline checked by `abi-check.sh` — remains the follow-up, and is the
honest generalisation: **a symbol gate needs a layout companion, or PODs are unguarded by construction.**

### The canonical mapping has its first consumer (2026-11-06)

`WasmCallCache.cpp` no longer tests `== ValueTypeCode::V128` and pushes a literal `2`. Both loops —
the argument-type expansion and the value marshalling — now ask the core:

```cpp
const std::uint32_t slots = argSlotCount(valueKindOf(parameter.value.opcode));
```

That matters beyond tidiness. A mapping that exists but has no consumer is an **untested claim**; a
mapping with one consumer is a *contract*. The call cache is the right first consumer because it is the
only place today that already had to know the rule, and its parity case (`[callcache][v128][abi][parity]`,
cache versus compiled pointer, including a scalar-before-vector export) now exercises the shared function
rather than a copy of it.

It also removed the last hand-written `2` from the ABI path: the number now lives in exactly one place,
where the decision note's prose points.

## 14. Emitter unification: the complete edit list (2026-11-06)

This is the gate to backend conversion, and it is a *single* commit because three tests generate the C
header, compile it with gcc **and run a generated conformance program against it**. Splitting it
produces exactly the failure the tests are there to catch — demonstrated once, reverted once (§11).

### The inventory, with line numbers

| File | Sites | Change |
|---|---|---|
| `Tools/Bindings/EmitShared.cpp` | 205, 468, 577 (`ValueKind::V128` → `"wasm_v128"`) | map to **two** parameter types, not one |
| | 416 (the C typedef) | `{ uint64_t lo; uint64_t hi; }` |
| | 372, 481, 915, 1044 (the four `params` loops) | emit a vector parameter as **two scalars** |
| `Tools/Bindings/EmitDynamic.cpp` | 25, 111, 232, 292, 351 | five type spellings: `ctypes.c_ubyte * 16`, `uint8_t[16]`, `ubyte[16]`, `array[16, uint8]`, `StaticArray(UInt8, 16)` — each must become the language's `uint64` pair |
| `Tools/Bindings/EmitRust.cpp` | 30 | `pub struct wasm_v128 { pub lo: u64, pub hi: u64 }` |
| `Tools/Bindings/EmitWeb.cpp` | the kind→type map | same rule |
| `Tools/Bindings/EmitCpp.cpp` | 287, 452 | **already canonical** (§11) |
| `Test/BindingSchemaTests.cpp` | `write("main.cpp", mainSource)` (~135) | the conformance program's vector argument and its checks |
| | 1439 | the pinned C++ typedef expectation — already updated |

### Order inside the commit

1. `EmitShared` typedef + kind→type + the four parameter loops (the C and generic paths).
2. `EmitCpp` — verify the already-canonical declaration still matches what the shared path emits.
3. `EmitDynamic`, `EmitWeb`, `EmitRust` — the language-specific spellings.
4. `mainSource` in the test: construct two `uint64_t` per vector argument and check the same values.
5. Regenerate and run: `[bindings]` must pass the compile-and-run gate, then the full suite.

### The acceptance gate

> `[bindings]` passes **including the gcc compile-and-run conformance cases**; the generated C header
declares a vector parameter as two `uint64_t`; and `[callcache][v128][abi][parity]` still passes, so the
cache, the generated header and the compiled function all speak one convention.

### Why this is written rather than started

It is five files plus a generated program, and its own gate iterates (generate → compile → run). A
half-applied version leaves either a red tree or a green tree that no longer tests the convention — the
two outcomes this phase exists to avoid. The inventory above is what makes the next pass a single
mechanical sweep instead of a second investigation.

### A negative result worth recording: the union does not sidestep the sweep

Hypothesis: `adapter.h` (C, still `bytes[16]`) and the C++ typedefs (now `lo, hi`) disagree, and a union
providing **both** views would let the C header change without touching the conformance program:

```c
typedef union wasm_v128 { struct { uint64_t lo; uint64_t hi; }; uint8_t bytes[16]; } wasm_v128;
```

**It does not work.** With that emission the generated program fails to *compile*
(`BindingSchemaTests.cpp:157`), so the disagreement is not about field names — something in the generated
adapter or conformance source depends on the type in a way a union does not satisfy (the anonymous inner
struct, the aggregate-ness of the value at the call, or the way the adapter's declaration is matched
against the JIT's signature). Reverted.

**What the next pass must do first, and it is one command:** read the compiler error from the generated
program (the test writes it to `compile.log` under its temp directory and prints it via `INFO`). That
message names the construct; everything after it is mechanical. Guessing at the cause — which this
attempt did — costs a build and teaches nothing, while the error text costs nothing and teaches
everything.

That is the same rule this whole phase keeps producing, in its cheapest form: **when a gate fails, read
what the gate printed.**

### The C header moves alone after all — the union was the problem, not the rename (2026-11-06)

The note above said the next pass should read the gate's output before guessing. Doing exactly that
settled it in one build: with `typedef struct wasm_v128 { uint64_t lo; uint64_t hi; }` the `[bindings]`
compile-and-run cases **pass** (937 assertions), and the full suite is green.

So the earlier three failures were caused by the **union spelling** —
`union wasm_v128 { struct { ... }; uint8_t bytes[16]; }` — whose anonymous inner struct is not something
the generated adapter accepts, not by renaming the fields. Two lessons, both cheap:

1. **A failed experiment does not mean the target is unreachable.** The union failed; the plain struct
   did not. The recorded negative result was accurate about the union and over-general in its
   conclusion, and reading the compiler output separated the two in one build.
2. **"It failed before" is not evidence.** The first attempt bundled the typedef rename with a Rust
   change and reported three failures; the rename alone is fine. Bundling two changes made a correct
   edit look guilty.

### Emitter inventory after this pass

| Emitter | State |
|---|---|
| `EmitCpp.cpp` | **canonical** |
| `EmitShared.cpp` — C typedef | **canonical** (`{ uint64_t lo; uint64_t hi; }`) |
| `EmitRust.cpp` | **canonical** |
| `EmitShared.cpp` — kind→type (205, 468, 577) and the four parameter loops | aggregate: a vector parameter is still emitted as one type |
| `EmitDynamic.cpp` (5), `EmitWeb.cpp` | aggregate |

The remaining work is now **parameter expansion only**: the type *declarations* agree everywhere they can,
and what is left is emitting two scalars where one type is emitted today.

### Rust parameters expand; the bug was my edit's span, not the design (2026-11-06)

`emitRustParams` now emits a vector parameter as **two scalars**:

```rust
pub fn f(arg0_lo: u64, arg0_hi: u64) -> i32
```

Two failed attempts came first, both reporting four failures on **non-vector** signatures. The cause was
the edit, not the emitter: the replacement span stopped one line short, orphaning the
`out << " /* borrowed */"` statement — so *every* parameter, vector or not, gained a stray comment and
the emitted text stopped matching. Reading `emitRustParams` showed `first = false;` sits **before** the
name emission, which made the correct span exactly three lines (name, guard, comment) and the wrong span
exactly two.

That is the third time in this arc that a mechanical edit failed for a span or anchor reason rather than a
design reason (§28's glued declarations, §34's wrong brace, §43's 21-for-6 deletion). The rule stands and
is cheap: **after replacing a span, read the two lines on each side of it** — an orphan is invisible in a
diff that looks right and fatal in a test that does not.

Binding emitters after this pass: `EmitCpp` declarations canonical, `EmitShared` C typedef canonical,
`EmitRust` type and **parameters** canonical. Remaining: `EmitShared`'s shared parameter loops (4),
`EmitDynamic` (5 spellings), `EmitWeb`.

## 15. A correction from review: the two-word rule is the *native* ABI, not the wasm one (2026-11-06)

**The objection, and it is right:** a `v128` in a *guest-side* binding — Rust or C compiled **to wasm** —
must use the **canonical wasm type** (`u128`, or `core::arch::wasm32::v128` in Rust). The two-`uint64_t`
rule is the **native C ABI of a JIT-compiled module**, i.e. the boundary where WasmJit has already lowered
the vector into machine registers. Between two wasm modules there is no such lowering: `v128` is a single
128-bit value in the wasm type system, and whoever compiles the guest decides how it reaches a register.

So there are **two boundaries with two different canonical spellings**:

| Boundary | `v128` spelling | Why |
|---|---|---|
| **native C ABI** (JIT-compiled export, libffi cache, generated C header calling *into* compiled code) | **two `uint64_t`**, low first | the machine has no portable 16-byte vector ABI (MS x64 passes aggregates by reference) |
| **wasm ABI** (a guest module's own imports/exports, any language compiled to wasm) | **the wasm `v128` type** (`u128`, `wasm_v128`, `[u8; 16]` by value, `v128` in C with the wasm toolchain's own convention) | the wasm type system already defines it, and the guest's compiler lowers it |

### The defect this exposes in the binding generator

`Direction` appears **nowhere** in `EmitRust.cpp` or `EmitShared.cpp`: the emitters produce one shape for
both boundaries. Whatever they choose is therefore wrong for one of them — and my last change (expanding
Rust parameters into `argN_lo: u64, argN_hi: u64`) was made **without** establishing which boundary the
Rust output serves. It has been reverted, because shipping a possibly-wrong ABI is worse than shipping
none.

**What the tool needs:** every emitter's parameter and result mapping must be a function of
`(Direction, Language)`, not of `Language` alone. Concretely:

- `HostToGuest` (native caller → JIT-compiled export): **two `uint64_t`** per vector — the decision of §28;
- `GuestToHost` (wasm guest → host import): the **wasm `v128`** type of that language.

That is a design change in the binding tool, and it is the *correct* framing of P3: the decision note's
rule is a **native-boundary** rule, and `ABI_PORTABILITY.md` §3 should say so in those words.

### What stands, and what is now uncertain

| Change | Status |
|---|---|
| `EmitCpp` / `EmitShared` **typedefs** → `{ uint64_t lo; uint64_t hi; }` | stands for the native header they serve; a guest-side C header would need the wasm spelling and does not exist in this tool yet |
| `EmitRust` **type** → `{ pub lo: u64, pub hi: u64 }` | **same caveat** — it is right for a native host, wrong for a Rust-to-wasm guest |
| `EmitRust` **parameters** → two scalars | **reverted**; needs the direction split first |

This is the first correction in the refactor that came from review rather than from a test, and it is the
most valuable kind: the tests were all green while the *design* was under-specified.

### Resolution of §15: this generator emits *native-side* surfaces only

Checking how `direction` is actually used settles it. `EmitCpp` reads
`schema.value("direction", "g2h")` and uses it for **nothing except which functions to emit**:
`emitImports = direction != "h2g"`, `emitExports = direction != "g2h"`. The vector spelling is not
conditioned on it — and that is correct, because **both directions in this tool are native-side**:

| Direction | What the generated declaration describes | Which ABI |
|---|---|---|
| `g2h` | a **host** function that JIT-compiled guest code calls | native C ABI — the call is made *from* machine code |
| `h2g` | a **function pointer** into JIT-compiled code that the host calls | native C ABI — the call is made *into* machine code |
| `g2g` | not applicable: the emitters refuse it and ask for a projected schema | — |

So every signature this tool emits crosses the native boundary, and the **two-word rule applies uniformly**
to both directions. The earlier correction stands as a *conceptual* clarification — a guest-side SDK (Rust or
C compiled **to wasm**) would need the canonical wasm `v128` — but that is a **different product surface**
that this tool does not emit, and it is now written down as such rather than as a defect in the current
output.

**Consequences:**

- The `EmitCpp` / `EmitShared` / `EmitRust` **type** declarations (`{ lo, hi }`) are correct for every schema
  this tool accepts.
- The **Rust parameter split** (`argN_lo: u64, argN_hi: u64`) is correct for the same reason, and has been
  re-applied with that reasoning in the code comment rather than left reverted.
- A future guest-side emitter must consult `direction` **and** the target (wasm vs native) — a `(direction,
  target, language)` mapping — because for that surface the two-word rule would be wrong. That is recorded
  here so nobody "fixes" the current emitters into it.

### What the exchange itself produced

The correction came from review, not from a test, and the *resolution* also came from reading the code —
`EmitCpp`'s two uses of `direction`. Two lessons, both already in the post-mortem's list but worth the
repetition: **a rule stated without its boundary is under-specified**, and **the fastest way to settle a
design question is to read what the code already decided.**

## 16. The rule that resolves §15: two families, chosen by *consumer*, not by language (2026-11-06)

The ABI a generated binding must express is decided by **who consumes the generated code**, not by the
language it is written in:

| Family | Emitters | Must match | Why |
|---|---|---|---|
| **WasmJIT-facing** — code the embedder copies into their own C++ application | `cpp-typedefs`, `cpp-source` | **WasmJIT's ABI** (native: a vector is **two `uint64_t`**, low first) | the embedder's process calls the JIT-compiled code directly; there is no runtime in between to convert anything |
| **Guest-facing** — code describing the module's own interface | C header, Rust, AssemblyScript, Zig, Kotlin, MoonBit, WIT, the dynamic/ctypes emitters | **the WASM ABI** (a vector is the canonical wasm `v128`) | the conversion from the wasm ABI to WasmJIT's native ABI happens **at instantiation**, inside the runtime — never in the binding |

That single sentence — *the conversion happens at instantiation* — is the test that settles any future
question of this shape. If the generated code runs in the guest's world (or describes it), it speaks the
wasm ABI. If it is compiled into the embedder's world, it speaks WasmJIT's.

### What that means for the changes already made

| Change | Verdict |
|---|---|
| `EmitCpp` emits `{ uint64_t lo; uint64_t hi; }` | **correct** — WasmJIT-facing, and the embedder copies it |
| `EmitShared`'s C-header typedef → `{ uint64_t lo; uint64_t hi; }` | **wrong side**: the C header is guest-facing, so it must keep the canonical wasm spelling |
| `EmitRust` type/parameters → the pair | **wrong side**, same reason |

Both have been reverted to the committed state.

### The obstacle the revert exposed, which is the real finding

The binding **conformance harness compiles the guest-side C header and the WasmJIT-side C++ adapter into
one program** (`BindingSchemaTests.cpp`, the gcc compile-and-run cases). While both spelled a vector
`bytes[16]` that worked; the moment the two families diverge *correctly*, the harness fails to compile —
three failures, immediately.

So the split cannot be completed by editing emitters alone. It needs one of:

1. **a translation layer** in the generated adapter — the WasmJIT-side code converts between the guest-side
   `wasm_v128` and the native pair at the boundary, which is precisely what the runtime does for real;
2. **two separate conformance programs**, one per family, each compiled against only its own header.

Option 1 is the truth-preserving one: it models the runtime's own job, so the test would exercise the
conversion rather than assume it away.

### Lesson

**"Which ABI?" is answered by a question, not by a lookup table: _who compiles this code, and who calls
it?_** The same language appears on both sides of the boundary in this project — C is guest-facing in the
C header and WasmJIT-facing in the C++ adapter — which is exactly why a per-language rule could not work,
and why this needed a real answer rather than a revision of the previous one.

## 17. The translation layer, specified against real anchors (2026-11-06)

§16 established that the two families must diverge; the conformance harness is what stops them. The fix is a
translation layer **in the generated adapter**, because that is precisely the job the runtime performs for
real — so the test exercises the conversion instead of assuming it away.

### Where it goes

The adapter is emitted by `emitCppAdapterImpl` (`Tools/Bindings/EmitCpp.cpp:429`); its call site is the
`reinterpret_cast<Target>(callable->fnPtr)` emission at `:414`. The generated shape today is
"cast the pointer, forward the arguments, return the result". It becomes:

```cpp
// for each exported function taking or returning a vector
Target target = reinterpret_cast<Target>(callable->fnPtr);
return target(context, arg0, guest.arg1.bytes /* wasm side */ ... );
            // ^ guest-side `wasm_v128` split into the native pair here
```

### What the layer must do, exactly

| Position | Guest-side value | Native argument it becomes |
|---|---|---|
| parameter | one `wasm_v128` (`bytes[16]`, little-endian) | **two `uint64_t`**: `lo` = bytes 0-7, `hi` = bytes 8-15 |
| result (single) | one `wasm_v128` | two words read from the return, packed back into `bytes[16]` |
| result (multi-value) | the trailing buffer | unchanged — the buffer is `lo, hi` at byte offsets 0 and 8, which is the same bytes |

Note the third row: **the buffer case needs no conversion at all**, because "`lo` then `hi`" and "bytes 0-7
then 8-15" are the same memory. That is a useful invariant to state, and a reason the two-word decision was
the right one: the only place the two ABIs differ is *how the value travels*, never *what the bytes are*.

### What it must not do

- **No byte swapping.** The word order is defined by bit position (§28 of the decision note), not by
  register number, so `lo` is always bytes 0-7 on every target — including a future big-endian port, which
  would change the JIT's load/store, not the binding.
- **No conversion on the guest-facing side.** A guest-facing emitter that converted would be modelling the
  runtime in the guest, which is exactly the confusion §16 resolved.
- **No conversion for non-vector parameters.** The layer is a no-op unless a signature mentions `v128`.

### The gate

> With the C header on the **wasm** spelling and the C++ adapter on the **native** pair, the gcc
> compile-and-run conformance cases pass, and `[callcache][v128][abi][parity]` still passes — three
> independent paths (generated adapter, libffi cache, direct pointer) agreeing on one convention.

### State

Not implemented; specified against line anchors so the next pass edits rather than investigates. Suite
**21591 assertions in 728 cases**, green, tree clean.

### Correction to §16's inventory: the tree kept the canonical spelling (2026-11-06)

The §16 table said the guest-facing emitters had been "reverted to the committed state". Verifying the tree
before the next edit shows otherwise: the committed state *is* canonical, because the revert I attempted was
against `EmitShared.cpp`/`EmitRust.cpp` while those files' committed content already carried the two-word
spelling from the earlier commits. Current, measured state:

| Site | In the tree today | Per the consumer rule it should be |
|---|---|---|
| `EmitCpp` (`cpp-typedefs`, `cpp-source`) | `{ uint64_t lo; uint64_t hi; }` | **correct** — WasmJIT-facing |
| `EmitShared:416` C header typedef | `{ uint64_t lo; uint64_t hi; }` | **wasm spelling** (guest-facing) |
| `EmitShared:486` Rust parameters | `argN_lo: u64, argN_hi: u64` | **wasm spelling** (guest-facing) |
| `EmitRust` struct | see above | **wasm spelling** (guest-facing) |

The suite is green with the guest-facing emitters on the native spelling because **no fixture in the
binding tests passes a `v128` across that boundary** — the conformance schemas are scalar. So the rule is
violated in the tree and *undetectable by the current tests*, which is the same class of gap as B1: a
correct-looking green suite over a specification the tests never exercise.

**What that changes about the next step:** before moving the guest-facing emitters, a **fixture is needed
that puts a vector through the generated binding** — otherwise the move cannot be verified and the
"translation layer" would be fixing a path nothing tests. That fixture is now the first task, ahead of the
emitter edits and ahead of the adapter conversion, because it is what makes both observable.

### Item 1 done: a vector now travels through the binding tests, and it exposed today's header

`[bindings][v128][abi]` builds a schema from the **existing** `v128_param` fixture and pins the emitted
text. The first run needed a deliberate mismatch to print the declaration, and the measurement is worth
recording because it names the exact inconsistency:

```c
/* G2H import: lane1 */
int32_t wasmjit_lane1(wasm_v128 arg0);
```

So today the **guest-facing** C header declares **one aggregate parameter** — `wasm_v128` — whose
*definition* is the **native pair** (`{ uint64_t lo; uint64_t hi; }`). That is neither spelling done right:

| Correct for the guest side | Correct for the native side | What the header does today |
|---|---|---|
| one parameter of the wasm `v128` type | **two** scalar parameters (`lo`, `hi`) | one parameter whose type is the native pair |

It compiles and needs no conversion because an aggregate of two `u64` and a 16-byte vector share layout on
SysV — the same coincidence that hid the MS x64 divergence in the libffi cache (§11).

The test now pins `wasmjit_lane1(wasm_v128 arg0)`, so moving this header to the wasm side — or splitting the
parameter into two scalars — **fails a test** instead of changing silently. That is what item 1 was for.

### Revised order for the remaining items

With the pin in place the order is forced:

1. **Adopt the wasm spelling in the guest-facing header** (`EmitShared:416` back to a 16-byte type) — now
   visible to the pin, and it is the *single* change that makes the header's parameter consistent.
2. **Adapter translation** (`EmitCpp.cpp:414`) so the WasmJIT-side code converts at the call.
3. **Rust and the dynamic/web emitters** follow the same rule.

Suite **21596 assertions in 729 cases**, green.

### Item 2 done: the guest-facing C header is on the wasm spelling (2026-11-06)

`EmitShared:416` emits `typedef struct wasm_v128 { uint8_t bytes[16]; }` again — the **wasm** side of the
boundary — and the whole suite passes: `[bindings]` **942 assertions in 44 cases**, full suite **21596 in
729**.

The earlier three failures that blocked this change do **not** reproduce. Reading the gate's output (the
rule from §46) showed zero compiler errors: the generated conformance program compiles with the guest-side
header on the wasm spelling and the adapter on the native pair, because the adapter carries **its own**
type and converts nothing — the two headers are never reconciled in one translation unit.

That is worth stating plainly, because it corrects a conclusion I had drawn and repeated: **the emitter
families can diverge today; no translation layer is required for the tests to pass.** The translation
layer §17 specifies is still the *right* thing for a real embedder (which will pass a native pair to an
adapter that must hand a guest-side vector onward), but it is not a precondition for the split.

### Where the two families now stand

| Site | Spelling | Correct per the consumer rule |
|---|---|---|
| `EmitCpp` `cpp-typedefs` / `cpp-source` | `{ uint64_t lo; uint64_t hi; }` | **yes** — WasmJIT-facing |
| `EmitShared:416` C header | `{ uint8_t bytes[16]; }` | **yes** — guest-facing |
| `EmitShared:486` Rust parameters | `argN_lo: u64, argN_hi: u64` | **no** — guest-facing, should be one wasm vector |
| `EmitRust:32` struct | `{ pub lo: u64, pub hi: u64 }` | **no** — same |

The two remaining sites are the Rust pair, and they now have a visible target: the same change that just
worked for C, applied to Rust, with the `[bindings][v128][abi]` pin extended to assert the Rust spelling too.

### Item 3 done: Rust is on the wasm side too, and the pin asserts it (2026-11-06)

`EmitRust` emits the 16-byte type again (`pub struct wasm_v128 { pub bytes: [u8; 16] }`) and a vector
parameter is **one** parameter, not a `lo`/`hi` pair. The pin grew the assertions that make the move
permanent rather than incidental:

```cpp
CHECK(rust.find("wasm_v128") != std::string::npos);
CHECK(rust.find("arg0_lo") == std::string::npos);
```

Suite: `[bindings][v128]` **6 assertions**, full suite **21597 in 729 cases**, green.

### The two families, now aligned with the rule

| Site | Spelling | Consumer | Correct? |
|---|---|---|---|
| `EmitCpp` `cpp-typedefs` / `cpp-source` | `{ uint64_t lo; uint64_t hi; }` | WasmJIT-facing | **yes** |
| `EmitShared` C header | `{ uint8_t bytes[16]; }` | guest-facing | **yes** |
| `EmitRust` type + parameters | `{ pub bytes: [u8; 16] }`, one param | guest-facing | **yes** |
| `EmitDynamic` (5 spellings), `EmitWeb` | 16-byte arrays / `wasm_v128` | guest-facing | already the wasm shape — needs only the pin |

Which means the emitter split your review demanded is **done for the two languages that had it wrong**, and
the remaining two emitters were already on the correct side; what they lack is an assertion, not a change.

### What this sequence demonstrated

Three items, three builds, zero reverts: pin first (which *measured* the inconsistency), then the C header,
then Rust — each one visible to the pin, each one verified by the same gate. Compare with the earlier
attempts, which had no pin and no measurement and lost two rounds to guessing. **The fixture was not
preparation for the work; it was the thing that made the work possible.**

### The native side has the same inconsistency, and it now has an address (2026-11-06)

With the guest side settled, the pin's logic applies symmetrically: `cpp-typedefs` and `cpp-source` emit

```cpp
struct wasm_v128 { std::uint64_t lo; std::uint64_t hi; };
```

and then declare a vector parameter as **one** `wasm_v128` — an aggregate whose type is the native pair. On
SysV that is two GPRs; on **MS x64 it is passed by reference**, which is precisely the divergence the
decision exists to remove. So the WasmJIT-facing emitters are *still* wrong in the way §28 was written to
prevent, and it is invisible on this machine for the third time (the libffi cache, the C header, now this).

### The exact change, in three places

| Site | Today | Should be |
|---|---|---|
| `EmitCpp.cpp:33` (typedef parameter list) | `cType(param) << " " << name` | for a vector: `uint64_t name_lo, uint64_t name_hi` |
| `EmitCpp.cpp:505` (adapter signature) | same | same |
| the adapter's forwarding call | `target(context, arg0)` | `target(context, arg0_lo, arg0_hi)` |

A single helper — `emitCppParam(out, entry, i, param)` — used at both declaration sites, plus the forwarding
update, is the whole change. It is exactly the shape that worked four times today (pin → measure → one edit
per site), which is why it is written down rather than attempted in the last minutes of a session: the
**body** and the **declarations** must move together or the conformance compile fails, and that needs one
full generate → compile → run cycle to verify.

### Test to add first, per the lesson that keeps paying

Extend `[bindings][v128][abi]` to emit `CppTypedefs` for `v128_param` and assert the parameter list
contains **two** `uint64_t` and no `wasm_v128` parameter — the same two-sided assertion that made the Rust
move safe (`wasm_v128` present, `arg0_lo` absent, then inverted here).

### Session close: where the refactor stands

| Phase | State |
|---|---|
| **P0** | decision and its **boundary**, normative |
| **P1** | proof with no backend |
| **P3** | cache fixed + parity-tested + consuming the shared rule; **guest-facing emitters correct and asserted across 14 languages**; the **native-facing parameter split** located in three places, with the test that must precede it |
| **P4** | LibJIT specified; `argSlotCount`/`mapArgSlots` implemented, tested, in use |
| **Post-mortem** | §16, four corrections, and the sequence that produced them |

Suite **21609 assertions in 729 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings.

### Correction: the native split is four sites, and the fourth is a contract (2026-11-06)

Reading `EmitCpp.cpp:495-545` to implement the three-site change found a fourth dependency: the generated
adapter does not call the function pointer directly, it forwards through a variadic helper:

```cpp
callExport<int32_t>("lane1", arg0);           // or callExportVoid / callExportWithResults
```

So a two-parameter signature is not achievable by editing the declaration alone: `callExport<T>` must be
**invoked with the two words**, and its own contract — how it binds host-side arguments to the compiled
function's signature — lives in the runtime header the adapter includes, not in the emitter. Changing the
declaration without it would produce an adapter that compiles and calls with the wrong arity.

That makes the native-side change **four** coordinated edits:

| # | Site | Change |
|---|---|---|
| 1 | `EmitCpp.cpp:33` | typedef parameter list: two `uint64_t` for a vector |
| 2 | `EmitCpp.cpp:505` | adapter signature: same |
| 3 | `EmitCpp.cpp:517` (forwarding) | `callExport(…, name_lo, name_hi)` |
| 4 | the `callExport` family's contract | must accept the pair; read it before editing 1-3 |

Item 4 is the reason this stopped rather than proceeded: it is a **contract question, not an edit**, and the
arc's entire record says contracts are settled before edits — P1 exists because of exactly this.

### Where P3 and P4 stand, without euphemism

- **P3**: guest side done and asserted across 14 languages; native side **located (4 sites) and blocked on
  `callExport`'s contract**; cache fixed and parity-tested.
- **P4**: not started. LibJIT's four parts are specified; nothing in any backend has changed for this
  refactor. **No compiled function anywhere uses the new convention yet.**

Suite **21609 assertions in 729 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

### Contract settled, and P3's native split done (2026-11-06)

**The contract, read rather than assumed:** `Runtime::callExport` forwards to `callCallable`, which is a
**direct cast**:

```cpp
using Fn = Ret (*)(VMContext*, std::decay_t<Args>...);
Fn fn = reinterpret_cast<Fn>(callable.fnPtr);
fn(callable.context, std::forward<Args>(args)...);
```

So **the caller's C++ argument list *is* the declaration of the callee's C signature.** There is no
marshalling layer, no conversion, and nothing to satisfy beyond agreeing with it. The "fourth site" of the
previous note was therefore never a blocker: `callExport(name, lo, hi)` compiles and works by construction.

That is the stable contract, and it is one sentence: *whoever calls a compiled function must spell its
arguments exactly as the emitted signature does, because the cast is unchecked.*

### The change that followed

| Site | Change |
|---|---|
| `EmitCpp.cpp` — new `emitCppParam` helper | emits `uint64_t name_lo, uint64_t name_hi` for a vector, the plain declaration otherwise |
| `emitCppParams` (typedef parameter list) | uses the helper, with braces around the two-statement body |
| adapter signature | uses the helper |
| adapter forwarding | passes `name_lo, name_hi` to `callExport` |

The pin now asserts **both** families in one case: `wasm_v128` present and `arg0_lo` absent for the guest
side; `uint64_t arg0_lo, uint64_t arg0_hi` present and `wasm_v128 arg0` absent for the native side.
`[bindings][v128]` is **20 assertions**; `[bindings]` **955**; full suite **21611 in 729 cases**, green.

Two mechanical mistakes on the way — the helper landed after its first use, and the brace-less `for`
swallowed only one of two inserted statements — both caught immediately because the *pin* defined what
success looked like.

## 18. P4 split into subtasks, and subtask 1 landed (2026-11-06)

P4 is not one change. Reading LibJIT's prolog showed why: with a split signature a `v128` parameter
arrives as **two `jit_type_ulong`**, but a LibJIT local is **one 16-byte aggregate**, so the binding cannot
merely pick a different index — it must **synthesise the aggregate from two scalars**. That makes four
subtasks, each with its own gate:

| # | Subtask | Gate | State |
|---|---|---|---|
| **1** | **the pin**: a fixture with a vector adjacent to a scalar in **both** orders | passes on today's convention | **done** |
| 2 | signature: a `v128` parameter becomes two `jit_type_ulong` | pin still green | — |
| 3 | prolog: build the 16-byte aggregate from the two halves | pin green, `[libjit]` green | — |
| 4 | import thunk: the same walk and packing outward | full suite + A/B | — |

### Subtask 1

`v128_boundary_order.wat` has `(i32, v128)` and `(v128, i32)` exports; the test calls both through the
compiled pointer and asserts the *other* parameter survives — `13` from lane 2 of `{11,12,13,14}`, and `21`
from the scalar after the vector. Every existing vector fixture has the vector alone in its signature, which
is why nothing has ever exercised this adjacency.

`[libjit][v128][order]`: **7 assertions, green** on today's convention; full suite **21618 in 730 cases**.

The second export is the important one: after the split, the scalar's own index moves, so a change that only
fixes the vector's index passes the first export and fails the second. That is the pin doing for P4 what it
did for the emitters — defining success before the edit exists.

### Correction to the P4 split: subtasks 2 and 3 are one commit (2026-11-06)

The split said subtask 2 (signature) had the gate "pin still green". **That gate is unreachable.**

Splitting the signature changes what `jit_value_get_param(fn, index)` returns: a `v128` parameter becomes
two slots, so every *subsequent* parameter shifts. If the prolog still reads one parameter per index — which
it does, `for (i) locals.push_back(jit_value_get_param(fn, i + 1))` — then any function with a vector
before another parameter reads garbage, and the pin fails on its **second** export while its first still
passes. That is precisely the failure the pin was built to expose, and it means the two edits are one unit:

| Unit | Change | Gate |
|---|---|---|
| **2+3** | signature split **and** the prolog's aggregate synthesis | pin green **and** `[libjit]` green |
| 4 | import thunk | full suite + A/B |

The lesson is the plan's, not the code's: **a subtask boundary drawn through a pair of edits that must be
simultaneous produces an unreachable gate** — and an unreachable gate is worse than no gate, because it
tempts a partial landing that the suite would not catch until the pin was already written.

### Why this is where the session stops

Unit 2+3 is: change the signature construction, and in the prolog build a 16-byte aggregate from two scalar
parameters for every vector parameter (LibJIT's aggregate-construction path, plus loading each half into the
right field at the right offset). It touches the prolog of *every* LibJIT function and its verification is a
compile-run loop through libjit's own IR. That is not a change to start with no verify budget left, and the
honest close is the same one this arc has used five times: specification and pin in the tree, edit not
started.

Suite **21618 assertions in 730 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

### Unit 2+3 attempted: two crashes, and the second names the real fix (2026-11-06)

Attempt 1 — split the signature and store the two `jit_type_ulong` parameters into the aggregate with
`jit_insn_store_relative` at offsets 0 and 8:

> **SIGSEGV.** The aggregate's declared type is `{ int, int, int, int }` (`v128_definition`), so an 8-byte
> store does not match it.

Attempt 2 — change the representation to `{ ulong, ulong }` so the stores match, keeping 16 bytes:

> **SIGFPE**, and `[libjit]` too. The representation is not private to the prolog: **every lane access in
> the backend reads the four 32-bit lanes of that aggregate**, so re-typing it as two 64-bit words breaks
> the SIMD path wholesale, not just the boundary.

Reverted. Suite **21618 assertions in 730 cases**, green.

### What that proves about the fix

The aggregate must **stay** `{ int, int, int, int }`. The boundary words must therefore be **split into
32-bit halves inside the prolog** — `lo` into offsets 0 and 4, `hi` into 8 and 12 — rather than stored as
whole 64-bit values. That is the inverse of the mistake both attempts made: they changed the *representation*
to fit the boundary, when the representation is shared and only the *boundary* may change.

That is the same rule as the emitter work, one layer down: **convert at the boundary, never in the thing the
boundary touches.** The emitter translated a vector at the call rather than re-typing the ABI; the prolog
must assemble a vector from words rather than re-type the aggregate.

### Where P4 stands

| Unit | State |
|---|---|
| 1 — the adjacency pin | **done**, 7 assertions, green |
| 2+3 — signature + prolog | **attempted twice, reverted**; the fix is now known: keep the 4×i32 aggregate and split each boundary word into two halves |
| 4 — import thunk | pending |

## 19. Recorded decision: local representation is per-backend; only the boundary is canonical (2026-11-06)

Raised in review: *isn't "four 32-bit integers" the representation that made SLJIT and AsmJit unviable in
the first place?* It is not, and the distinction must be written down or it will be re-litigated every time a
backend's v128 shape comes up.

**What was unviable, precisely:**

1. **`v128` *returns* in a single-return backend.** A backend whose function returns one register cannot hand
   back 16 bytes. This is why returns are deferred and why the decision routes them through the results
   buffer.
2. **A 16-byte aggregate at the *boundary*.** Two GPRs on SysV by coincidence, **by reference on MS x64**, and
   differing register-pair rules per target. This is the divergence D-ABI-2/D-ABI-3 forbid.

**What was never the problem:** a `v128` occupying 16 bytes in a stack slot or aggregate local *inside* a
function. Every backend already does that for GC elements, struct fields and spills, and D-ABI-4 explicitly
allows each backend to choose its implementation.

### The decision, and the invariant that makes it safe

> **Local representation is per-backend implementation. Only the boundary is canonical.** SLJIT may keep two
> `W` slots, AsmJit its aggregate, LibJIT four `i32`; each converts at the call. The invariant that makes
> this legal is byte identity: `lo` = lanes 0-1, `hi` = lanes 2-3, on little-endian, so the conversion is a
> re-interpretation and never a shuffle.

That is Option A. Option B — one canonical internal representation (two `u64`) everywhere — is a coherent
end state but a much larger change: every lane operation in every backend would become masking and shifting.
It is P5 territory (performance and consistency), not a prerequisite for P4, and unit 2+3's second attempt is
exactly what applying B to one backend without the lane-op rewrite looks like.

**Consequence for unit 2+3:** the specified fix stands — keep LibJIT's `{ int, int, int, int }` and split each
boundary word into two 32-bit halves in the prolog. No representation change anywhere.

## 20. Second ordering correction: units 2, 3 and 4 are one unit (2026-11-06)

The pin's exports make **no calls**, yet splitting the signature crashed it. That rules out the exports'
bodies and points at the path every invocation traverses: LibJIT's **driver entry**, which `Callable::fnPtr`
is set to for compiled functions and which reads parameters by index:

```cpp
jit_value_get_param(driver, paramCount + 1)   // LibjitModuleCompiler.cpp:465
```

With two slots per vector parameter, that index is wrong for **every function in the module**, whether or not
it calls anything. So the driver/thunk is not "subtask 4, afterwards" — it is on the same critical path as the
signature:

| Unit | Contents | Why indivisible |
|---|---|---|
| **2+3+4** | signature split · prolog aggregate synthesis · driver/thunk indexing | the driver is on every call path, so a split signature breaks it before any export runs |

Both corrections to this split have the same cause, and it is worth naming as a rule: **a subtask boundary
drawn through edits that are simultaneous by construction produces an unreachable gate.** The first correction
merged 2 and 3; this one merges 4 as well. The remaining sequence is therefore two units, not four:

1. **2+3+4** — signature, prolog, driver/thunk; gate: pin green *and* `[libjit]` green.
2. **SLJIT**, then **AsmJit**, each against the same pin extended per backend.

### The implementation, now fully specified

With the aggregate kept as `{ int, int, int, int }` (§19), the prolog converts **at the boundary only**:

- parameter `lo` → aggregate offsets 0 and 4; parameter `hi` → offsets 8 and 12;
- the halves come from shifts of each `jit_type_ulong` parameter, not from re-typing anything;
- the driver duplicates the same walk so its index arithmetic matches;
- nothing in the SIMD path changes, because the aggregate's shape and lane offsets are untouched.

That is the third attempt's complete brief. Two attempts failed for reasons now understood; this one has no
unknown left except whether libjit's shift insns behave as expected on the halves — which the pin answers in
one build.

Suite **21618 assertions in 730 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 21. The "unknown" was not the shifts — P4's order inverts (2026-11-06)

Reading `LibjitModuleCompiler.cpp:440-500` to fix the third edit found the real obstacle, and it is not
libjit's shift instructions.

### LibJIT's tail-call driver is one slot per wasm parameter by construction

The driver is a trampoline with a **fixed five-slot `void*` signature** (`thunkSig`, line 350) that forwards
to the raw entry:

```cpp
std::vector<jit_value_t> callArgs(1 + paramCount + (hasResultBuffer ? 1 : 0));
for (p) callArgs[1 + p] = jit_value_get_param(driver, p + 1);
jit_insn_call_indirect(driver, fnPtr, sig, callArgs.data(), ...);
```

A split `v128` needs **two values** in that `call_indirect`, and the driver has one slot to take them from. Its
own signature would have to become arity-per-function — a rework of the trampoline, not an index correction.
That is why the pin crashed with no calls in it: the driver wraps **every** compiled entry.

### Consequently the phase order inverts, and so does its justification

LibJIT was first because "it owns both sides of every call and cannot get the calling convention wrong". That
is still true — and irrelevant, because it has the **largest structural obstacle**: a hand-written tail-call
driver whose argument shape is fixed. The measured picture:

| Backend | Current `v128` parameter boundary | Work for P4 |
|---|---|---|
| **SLJIT** | **already two `W` slots** (`beginFunction` emits `SLJIT_ARG_TYPE_W` twice) | a **pin**, and the canonical rule becomes the documented one rather than an internal convention |
| **AsmJit** | one 16-byte aggregate in `FuncSignature` | split the parameter into two `uint64_t`; typed, so mechanical |
| **LibJIT** | one 16-byte `jit_type_t` **plus the fixed-arity driver** | driver rework first, then the split |

### Revised P4 order

1. **SLJIT** — pin the two-slot convention (it already complies), then add the result path.
2. **AsmJit** — the typed split, mechanical.
3. **LibJIT** — rework the driver to derive its arity from the function signature, *then* split.

This is the third plan correction on P4 and the largest: the backend that was scheduled first is the one that
needs the most work, and the reason it was scheduled first — that it cannot get the ABI wrong — is exactly why
it did not need to be first.

Suite **21618 assertions in 730 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

### P4 step 1 done: SLJIT's parameter boundary already is the canonical rule (2026-11-06)

`[sljit][v128][order][abi]` calls `v128_boundary_order.wasm` through a pointer declared with the **canonical**
shapes:

```cpp
using ScalarFirst = std::int32_t (*)(void*, std::int32_t, std::uint64_t, std::uint64_t);
using VectorFirst = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::int32_t);
```

Both pass — `13` from lane 2 of `{11,12,13,14}` and `21` from the scalar after the vector. That is the
strongest form of the claim available: the test does not pass *because* a 16-byte aggregate happens to share
SysV's register pair (which is how every earlier vector test passed); it passes because SLJIT's `beginFunction`
really does describe a vector parameter as **two word slots**.

So **SLJIT needs no code change for `v128` parameters.** The rule that was an internal convention is now the
documented one, and it is asserted.

### What remains for SLJIT

Only the **result** path: a `v128` result through the multi-value buffer (the decision's half that no backend
implements). Everything else in P4 for SLJIT is done by having been right already.

### The pin's value, stated once more

Two backend pins now exist and both were written *before* the corresponding change:

| Pin | Proves | Cost |
|---|---|---|
| `[libjit][v128][order]` | LibJIT's current one-slot boundary, in both orders | 7 assertions |
| `[sljit][v128][order][abi]` | SLJIT **already** matches the canonical two-word boundary | 7 assertions |

Suite **21625 assertions in 731 cases**, green.

## 22. P4 step 2 scoped: SLJIT *refuses* `v128` results; LibJIT crashes (2026-11-06)

Reading the result path (`SljitOpcodeDispatcher.cpp:938-1060`) turns up a distinction the plan did not make,
and it is the most useful one in the study.

### SLJIT declines; LibJIT crashes

```cpp
if (resultKind_ == WASM::ValueTypeCode::V128) {
    unsupportedReason_ = "the SLJIT backend does not lower v128 results yet";
    resultKind_ = WASM::ValueTypeCode::Void;
    resultWidth_ = 0;
    return;
}
```

SLJIT refuses a `v128` result **by name, in the support check, before emitting anything**. LibJIT does not:
it commits to the aggregate and the crash arrives at run time, through the driver. Same gap, two very
different failure modes -- and the clean one is not an accident of effort, it is where the check lives.

### Meanwhile SLJIT's parameter side is confirmed in source

`beginFunction` builds the argument list with exactly the two-slot walk the pin measures:

```cpp
if (paramWidths_[i] == 16) {
    argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, nextSlot);
    argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, nextSlot + 1);
    nextSlot += 2;
}
```

So the pin and the source agree, which is the position a pin is supposed to put a claim in.

### What step 2 actually needs, now that the shape is known

| Need | State |
|---|---|
| a way to carry 16 bytes out | **absent** -- SLJIT implements no multi-value buffer at all (`allReturns.size() > 1` is refused too) |
| a return-register path | impossible for 16 bytes: one result register |
| the buffer pointer's location | the decision's convention: through the multi-value buffer |

So step 2 is not "add a width-16 case to `emitReturn`". It is: **introduce the results buffer into SLJIT**, which is
simultaneously the multi-value support SLJIT lacks. The `v128` result is the first *user* of that machinery, not
a parallel feature -- which is a strong argument for building it once, as multi-value, and letting `v128` fall out
of it.

That reframing is the deliverable of this step: **SLJIT's `v128` result and SLJIT's multi-value support are the
same project**, and the decision's results-buffer rule is what unifies them.

Suite **21625 assertions in 731 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 23. Step 2's template is already in the tree — and the trigger needs generalizing, not inventing (2026-11-06)

Step 2 was scoped as "introduce the results buffer into SLJIT". Reading `WasmBase/WasmValue.hpp:70-131` and
`LibjitModuleCompiler.cpp:540-575` shows the buffer **already exists** and is already documented:

- `Callable::tailEntry`'s thunk takes `(VMContext*, void*, uint64_t* argSlots, uint64_t* resultSlots,
  void* resultBuffer)`;
- `TailCallState::resultBuffer` is the out-buffer, published once by the driver;
- **a multi-result function returns via a trailing `void*` parameter** and returns void —
  `docs/proposals/MULTI_VALUE.md` MV5, already implemented by LibJIT.

So SLJIT does not need new machinery. It needs to **adopt an existing convention** — and the one thing the
convention gets wrong for our purposes is its **trigger**:

```cpp
const bool hasResultBuffer = ft.results.size() > 1;   // LibJIT, everywhere
```

### The generalization, and why it is the right one

The buffer is not about *count*; it is about **a result that does not fit one return word**. Multi-value is the
case that first forced it; a `v128` is the same case with one element:

```cpp
// contract predicate, backend-agnostic (WasmBase):
//   a result tuple needs the out-buffer when it has more than one element,
//   or when any element is wider than one return word (v128).
bool needsResultBuffer(const FuncType& ft);
```

With that predicate, `v128`-result support is **the multi-value path applied to a single 16-byte value**: the
signature gains a trailing `void*`, the callee writes 16 bytes at that address, the caller supplies it. Nothing
about registers, nothing about a second return register, nothing per-backend except where the 16 bytes land.

### This unifies the decision with MV5 — the strongest result of the session

`V128_SIGNATURE_DECISION.md` §28's results rule ("a `v128` result goes through the results buffer") and MV5
("a multi-result tuple is written into the caller's buffer") are **the same rule**, because a `v128` is a tuple
of one that does not fit. That is why no backend needs a divergent return path, and why "v128 returns are
permanently deferred" was never a property of the ABI — only of the *trigger* the implementations chose.

### Step 2, restated as an executable brief

1. Add `needsResultBuffer(ft)` to `WasmBase` (replacing LibJIT's three inline `results.size() > 1` sites).
2. SLJIT: emit the trailing `void*`, write the 16 bytes at it, `sljit_emit_return` with `RET_VOID`.
3. SLJIT caller side: pass the buffer in `dispatchCall`.
4. Pin: a `v128`-result fixture, both backends, appended to `v128_boundary_order.wasm`'s module.

Suite **21625 assertions in 731 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 24. The trigger cannot be widened before the write path exists — measured (2026-11-06)

Step 2's item 1 was "add `needsResultBuffer(ft)` to `WasmBase`, replacing LibJIT's three inline
`results.size() > 1` sites". The predicate was added; **the three replacements were reverted**, and this
section records why, because it is the fifth plan correction and the cleanest one yet.

### What happened

| Step | Result |
|---|---|
| `needsResultBuffer(ft)` added to `WasmBase/WasmValue.hpp`, beside `tailCallSlotsSupported` | builds clean |
| three LibJIT sites switched to it (372, 436, `LibjitOpcodeDispatcher.cpp:4563`) | builds clean, **suite segfaults** |
| the three sites reverted to `results.size() > 1` | **21625 assertions in 731 cases, green** |

So the suite depends on the *old* trigger somewhere, and the segfault is not in the new predicate — it is in
what the widened trigger routes. A widened trigger says "you need the out-buffer" **before the code that writes
into that buffer for this case exists**, so a path that used to work (LibJIT returning the value through its own
type system) is replaced by one that has not been written.

### The rule this adds, and it is the general form of §20

> **A predicate may be widened only when every newly-matched case has a complete path behind it.** Widening a
trigger is not a refactor; it is a switch that re-routes values, and partially-implemented routing is worse than
no routing.

That is §20's rule ("a subtask boundary drawn through simultaneous edits produces an unreachable gate") applied
to a *predicate* instead of a subtask: both are orderings, both fail by routing something before its
destination exists.

### What is kept, and what the next unit must do

- **Kept:** `needsResultBuffer(ft)` in `WasmBase` — it states the contract correctly (§23) and names no backend.
- **Next:** implement the *write* path for the newly-matched case first — in SLJIT, where nothing is routed to a
  buffer today, so there is no working behaviour to displace; then, for LibJIT, only after its single-`v128`
  result path is known to be redundant.

That ordering is also why SLJIT remains step 2 and LibJIT last: SLJIT has the *least* working behaviour to
break, which — inverted from the original plan's reasoning — is exactly the property that should determine order.

Suite **21625 assertions in 731 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.
