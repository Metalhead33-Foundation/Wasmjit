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
