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

## 25. SLJIT write path: the six edit sites, and the frame slot nobody counted (2026-11-06)

Reading `beginFunction` and `finish` (`SljitOpcodeDispatcher.cpp:1000-1180`) settles how the out-buffer enters
SLJIT, and turns up one dependency that would have cost another crash.

### How SLJIT receives arguments

`SLJIT_S(n)` are the incoming argument registers: the context is slot 1, and `integerSlot` walks the parameters,
advancing **two** for a `v128` (already implemented). `emitReturn` reads the top operand slot and returns it in
`SLJIT_R0`. So an out-buffer is simply **one more trailing argument slot** -- the same mechanism as a parameter,
not a new one.

### The hidden dependency: the frame is full

`kReservedSlots = 7` -- "context, target, scratch, then four parked argument registers". The buffer pointer must
survive helper calls (R0-R3 are clobbered, and S registers are not held across one), so it needs a frame slot:
**`kReservedSlots` becomes 8, with a `resultBufferSlotOffset()` helper.** Cheap, but invisible from the
signature side, which is exactly the kind of thing that produces a crash two edits later.

### The six edit sites

| # | Site | Change |
|---|---|---|
| 1 | `kReservedSlots` + offset helper | 7 -> 8; add `resultBufferSlotOffset()` |
| 2 | `checkSupported` | drop the `V128` refusal; keep `resultWidth_ = 16` |
| 3 | `beginFunction` -- return type | `resultWidth_ == 16` -> `SLJIT_ARG_RETURN(RET_VOID)` |
| 4 | `beginFunction` -- arguments | trailing `SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, nextSlot)` **before** `savedCount`/`enter`; then store it to the frame slot |
| 5 | `emitReturn` | `resultWidth_ == 16` first: copy four operand slots (16 bytes) from the stack top to the buffer, then `return_void` |
| 6 | `finish` (trap epilogue) | `resultWidth_ == 16` -> `return_void`, not a `MOV` return |

Site 4 must precede the `savedCount`/`sljit_emit_enter` lines -- `savedCount` is `nextSlot - 1`, so an argument
added after it is silently unpreserved.

### Why this one is safe to attempt first

SLJIT routes **nothing** to a buffer today, so every newly-matched case is new behaviour: there is no working path
to displace (contrast §24, where LibJIT had one). That is the property that earns SLJIT step 2, and it is now
stated as the ordering criterion rather than "it was already partly compliant".

### The pin to write with it

A `v128`-result fixture declared as `void (*)(void*, void*)` -- `(context, out)` -- writing 16 bytes at `out`, and
compared against the four expected lanes. Add it beside the two boundary pins so the result rule is measured the
same way the parameter rule is.

Suite **21625 assertions in 731 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 26. P4 step 2 done: SLJIT returns a `v128` through the caller's buffer (2026-11-06)

The six edits of §25 are in, and a `v128` result now crosses SLJIT's boundary.

### What changed

| Site | Change |
|---|---|
| frame | `kReservedSlots` 7 -> 8; new `resultBufferSlotOffset()` (reserved slot 7) |
| `checkSupported` | the `v128`-result refusal is gone |
| prologue | `RET_VOID` when the result is 16 bytes; a trailing `SLJIT_ARG_TYPE_P` added **before** `savedCount`; the pointer parked in the frame slot |
| `emitReturn` | width 16 first: the vector's single 16-byte operand slot is copied to the buffer as two words, then `return_void` |
| trap epilogue | width 16 returns void, so the emitted return matches the declared one |

### The bug SLJIT caught for us

The first attempt aborted on `sljitLir.c:1709` -- `FUNCTION_CHECK_SRC`, an invalid source operand -- inside
`beginFunction`. The cause was an **off-by-one in the register mapping**: argument index `i` arrives in
`S(i - 1)`, and `nextSlot` had already been advanced past the buffer, so its register is `S(nextSlot - 2)`, not
`S(nextSlot - 1)`. The latter is one past the highest register that `savedCount` preserves.

Worth noting *how* it surfaced: SLJIT's own argument checks caught a register-index arithmetic error at compile
time, in the test named by the assert, with the exact line in `sljitLir.c`. That is the value of a backend with
`SLJIT_ARGUMENT_CHECKS` enabled -- the same class of error in a backend that trusts its input would have been a
silent miscompile.

### The test now asserts the capability, not the refusal

`sljit: an unsupported type is refused, never miscompiled` becomes `sljit: a v128 result arrives through the
caller's buffer`, calling `vec` (whose body is `v128.const i32x4 0 0 0 0`) through
`void (*)(void*, void*)` with a buffer **pre-filled with `0xffffffff`** and requiring all four lanes zero. The
pre-fill is what makes it a real test: a function that wrote nothing would fail it.

### State

Suite **21628 assertions in 731 cases**, green (up 3: six new assertions replacing three that encoded the old
refusal); `[headerdoc]` 6/3; `[docs]` 1021/2; `abi-check.sh` OK; Doxygen 0 warnings.

### P4 board after this step

| Step | State |
|---|---|
| 1. SLJIT parameters | **closed** -- compliant, pinned |
| 2. SLJIT results | **closed** -- `v128` result through the buffer, measured |
| 3. AsmJit parameters | next -- typed split of the aggregate |
| 4. LibJIT | driver arity rework, then the split; audit its working single-`v128` result path before widening the trigger |

With steps 1 and 2 closed, SLJIT implements **both halves of the canonical rule** -- two scalar words in, a
caller-supplied buffer out -- and it is the first backend to do so.

## 27. AsmJit's boundary is a hardware vector register — a third convention, measured (2026-11-06)

`Asmjit/AsmjitTypeTranslator.cpp:20` maps a `v128` to a **vector TypeId**, with a comment that states the
current intent plainly:

```cpp
case ValueTypeCode::V128:
    // WasmJit's ABI defines V128 as a 16-byte, 16-byte-aligned value.
    // AsmJit's vector TypeId lets FuncSignature/FuncDetail apply the
    // platform's native vector argument and return convention.
    return asmjit::TypeId::kInt32x4;
```

That is neither two scalar words nor a 16-byte aggregate: it is an **XMM register** on x86-64. So the three
backends implement three different boundaries, all measured now:

| Backend | `v128` parameter crosses as | Integer-only? |
|---|---|---|
| **SLJIT** | two `W` argument slots (two GPRs) | **yes** -- now the canonical shape, and results go through the buffer |
| **LibJIT** | one 16-byte aggregate `jit_type` (two GPRs on SysV) | yes by accident, by-ref on MS x64 |
| **AsmJit** | `kInt32x4` -> a **vector register** | **no** |

### Why this is the sharpest violation

D-ABI-2/D-ABI-3 require an integer-only, non-degenerate boundary so that every target can implement it. A
vector-register boundary is unspecified by the platform vector conventions in exactly the ways that matter: SysV
and MS x64 disagree about whether the value is by-value in `XMM0` or spilled, AAPCS64 has its own rules, and
none of it is expressible in LibJIT's or SLJIT's argument model at all. "Apply the platform's native vector
convention" is the opposite of a canonical ABI -- it is the *absence* of one.

It is also invisible to the test suite for the same reason every other vector defect was: a `v128` crossing to
AsmJit is not called by any boundary test, so nothing compares the two spellings.

### Step 3's edit plan

| # | Site | Change |
|---|---|---|
| 1 | signature construction (`FuncSignature` for a function type) | expand one `V128` parameter into **two `kUInt64`** arguments; the same for the result only once the buffer path exists |
| 2 | prologue | read the two argument words and store them into the 16-byte vector buffer the dispatcher already allocates (`newVectorBuffer`) -- AsmJit already represents a `v128` *internally* as a pointer to 16 bytes, so nothing inside the body changes |
| 3 | `translateValueType`'s `V128` case | keep `kInt32x4` for *internal* type queries, or split it into a boundary-specific helper; the comment must stop saying the native vector convention is the ABI |
| 4 | call sites | re-pack two words from the callee's buffer |

Note how cleanly this fits §19: AsmJit's **internal** `v128` is already "a 16-byte buffer", which is exactly the
stable representation the decision protects. Only the *boundary* spelling changes -- two `kUInt64` words in, the
buffer inside. Nothing about lane access moves.

### Consequence for the code comment

`AsmjitTypeTranslator.cpp:20-23` is the one place in the tree that states the *wrong* ABI as if it were the
decision. It must be corrected in the same commit that changes the signature, not before (the comment currently
describes what the code does).

Suite **21628 assertions in 731 cases**, green; `abi-check.sh` OK (0 removed); Doxygen 0 warnings; tree clean.

## 28. P4 step 3 done: AsmJit's parameter boundary is two scalar words (2026-11-06)

`translateFunctionType` now expands a `v128` parameter into **two `kUInt64`** arguments, low word first:

```cpp
for (int word = 0; word < 2; ++word) {
    if (!signature.can_add_arg())
        throw std::runtime_error("AsmJit ABI translation: function has too many arguments");
    signature.add_arg(asmjit::TypeId::kUInt64);
}
continue;
```

and `translateValueType`'s `V128` comment now says what the type *is* rather than what the ABI used
to be: the native vector type is the **internal** materialisation (lane operations, result register) and
deliberately **not** the argument boundary.

### A test was enshrining the wrong ABI

`"AsmJit lowers V128 as a native 128-bit vector ABI value"` asserted `arg(1) == kInt32x4` -- i.e. the suite
required the vector-register boundary. It is now `"AsmJit passes V128 as two scalar words, not a vector
register"`, expecting `arg_count() == 3` with `arg(1)`/`arg(2)` as `kUInt64`. That is the same pattern as
SLJIT's refusal test in §26: **a test naming a non-canonical convention as if it were the ABI**, which is why no
amount of passing tests ever surfaced it.

### What deliberately did not change

| Thing | State | Why |
|---|---|---|
| `translateValueType(V128)` -> `kInt32x4` | kept | it is the internal/vector type for lane ops; the boundary is now elsewhere |
| the `v128` **result** | still `kInt32x4` | it must move to the caller's buffer, but only with the write path (§24) |
| prologue assembly + call-site re-pack | **not done** | AsmJit does not claim `v128` functions today, so there is no working path to keep in step yet |

The last row is the honest limit of this step: the *signature* is canonical, the *body plumbing* (reading two
argument registers into the 16-byte buffer, and re-packing at call sites) belongs with the commit that first
compiles a `v128`-signature function in AsmJit -- together with fixing the prologue's
`newIntegerRegister(..., V128)` path, which would currently pick a 32-bit register for a 16-byte parameter.

### Where the three backends stand now

| Backend | parameter boundary | result boundary |
|---|---|---|
| **SLJIT** | two `W` slots -- **canonical, with a pin** | caller's buffer -- **canonical, measured** |
| **LibJIT** | 16-byte aggregate, one slot | aggregate (works today); buffer once the trigger is safe |
| **AsmJit** | two `kUInt64` -- **canonical, asserted** | native vector register (remaining divergence) |

Suite **21629 assertions in 731 cases**, green; `[headerdoc]` green; `[docs]` green; `abi-check.sh` OK
(0 removed); Doxygen 0 warnings; tree clean.

## 29. LibJIT step 4: all four sites located, and the conversion problem that explains the order (2026-11-06)

The driver's arity is *not* the blocker. `jit_function_create(context, sig)` at
`LibjitModuleCompiler.cpp:424` creates the driver **with the function's own signature**, so a two-slot `v128`
parameter appears in the driver automatically. What has to change is every place that assumes "one signature slot
per wasm parameter". All four are now located:

| # | Site | Today | Needed |
|---|---|---|---|
| 1 | `LibJitTypeTranslation.cpp:205-207` | one param per wasm param | a `v128` pushes **two `jit_type_ulong`** |
| 2 | `LibjitModuleCompiler.cpp:184-185` | `locals.push_back(jit_value_get_param(fn, i + 1))` | assemble the aggregate local from two params |
| 3 | `LibjitModuleCompiler.cpp:465-486` | `callArgs[1 + p] = get_param(driver, p + 1)` | walk two slots for a `v128` param |
| 4 | `LibjitOpcodeDispatcher.cpp:4566-4571` (**and every other call site**) | `callArgs[1 + i] = stackArgs[i]` | **split** an aggregate value into two words |

Sites 1-3 are index arithmetic -- the same shape SLJIT's off-by-one took, and mechanically the same fix. **Site 4
is different in kind**, and it is why this backend is last.

### Site 4: libjit has no way to reinterpret a 16-byte aggregate as two words

The callee's operand is an aggregate *value* (`jit_value_t` of the `v128` struct type). The callee now wants two
`jit_type_ulong` arguments. LibJIT gives two routes, both poor:

| Route | Mechanism | Cost |
|---|---|---|
| address + loads | `jit_insn_address_of(value)`, then two `jit_insn_load_relative(..., 0 / 8, jit_type_ulong)` | forces the value into memory at every call, and `address_of` requires the value to have a home |
| field extraction + shifts | `jit_insn_extract` the two `int` fields, shift and `or` into a `long`, twice | four instructions per split, and the shift insns' exact behaviour on halves is the one unknown §20 left open |

The prologue (site 2) has the mirror problem in reverse and is cheaper: a **store** into the aggregate's memory
(`jit_insn_store_relative(fn, local, 0/4/8/12, half)`), because a local has an address by construction. That
asymmetry is the finding: *storing* into an aggregate is easy, *reinterpreting* one as scalar arguments is not.

### So step 4 is a primitive, not an edit

LibJIT needs one shared helper -- `splitAggregate(arg) -> {lo, hi}` -- used by every call site, implemented once
over `address_of` + two loads. Every call path (`call`, `call_indirect`, imports, the direct tail-call fallback)
currently builds `callArgs` independently, so the helper must land before any of them is changed:

1. add `splitAggregate`, used by nothing yet -- **no behaviour change, verifiable alone**;
2. change site 2 (prologue assembly) and site 1 (translation) together, with the pin;
3. change site 3 (driver) and site 4 (all call sites) via the helper;
4. only then widen `hasResultBuffer` for the `v128` result (§24).

That ordering is the whole lesson of this study applied once more: the **consumer of the new shape must exist and
be provable before the producer emits it**. Attempts 1 and 2 failed because the producer changed first.

### Why LibJIT is last, stated in one line

SLJIT needed a frame slot; AsmJit needed a signature expansion; LibJIT needs a **conversion primitive that libjit
does not provide**, plus four cooperating sites -- and it is the only backend where the *internal* representation
(`{int, int, int, int}`, read directly by every lane operation) is shared with the boundary.

Suite **21629 assertions in 731 cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 30. Attempt 4 measured: the cheap prologue route is dead (2026-11-06)

Sites 1-3 of §29 were implemented and reverted. The measurement is worth recording because it eliminates the
cheapest hypothesis for site 2.

### What was tried

| Site | Change |
|---|---|
| 1 | a `v128` parameter pushes two `jit_type_ulong` into the signature |
| 2 | the prologue creates the aggregate local and stores the two words at offsets 0 and 8 |
| 3 | the driver walks *slots* (a new `paramSlots`), not wasm parameters, in both the result-buffer index and the forwarding loop |

Sites 1 and 3 are index arithmetic done correctly this time -- `paramSlots` is computed from the FuncType and
used consistently in the three places that had `paramCount`. Site 2 used the cheapest possible assembly:

```cpp
jit_insn_store_relative(fn, slot, 0, jit_value_get_param(fn, signatureSlot));
jit_insn_store_relative(fn, slot, 8, jit_value_get_param(fn, signatureSlot + 1));
```

### Result

`[libjit][v128][order]`: **assertion 1 passed, assertion 2 segfaulted.** The first call (`scalar_then_vector`)
worked -- so the signature split, the slot walk and the ABI are right -- and the *second* call
(`vector_then_scalar`, where the vector is **followed** by a scalar) died.

That is a precise signal: the failure tracks the aggregate local's use, not the boundary. The likely cause is
that `jit_insn_store_relative` with a `jit_type_ulong` source over a struct whose fields are `int` is not a
sound reinterpretation in libjit -- it does not derive the field-wise semantics the aggregate's type declares.
Writing 8 bytes into an `{int, int, int, int}` is a *layout* operation, and libjit types values, so the type
system has no reason to accept it.

### What this eliminates, and what it leaves

| Route for site 2 | Status |
|---|---|
| store two `ulong` words into the aggregate | **dead** -- measured above |
| store four `int` halves (shifts required) | the §20 unknown (libjit's shift insns) remains untested |
| build the aggregate from extracted halves via a shared helper | untested |

So attempt 4 did its job: it converted "the cheap way might work" into "the cheap way does not", for one build
and one pin. The next attempt starts from the two remaining routes, both of which need the `splitAggregate` /
`assembleAggregate` helper pair §29 calls for -- which is why that helper is the first artefact, not the last.

Suite restored to **21629 assertions in 731 cases**, green; tree clean.

## 31. Deep dive: libjit's primitives, a permanent IR test, and attempt 5's exact frontier (2026-11-06)

Reading libjit's own headers (`/usr/local/include/jit/`) answered the questions this study had been guessing at.

### What libjit actually provides

| Primitive | Exists? | Signature / meaning |
|---|---|---|
| `jit_insn_shl` / `shr` / `ushr` | **yes** | `(func, value1, value2)` -- §20's open unknown is closed: halves can be shifted |
| `jit_insn_extract` | **no** | there is no struct-field extraction in the public API |
| `jit_insn_store_relative` | yes | `(func, dest, offset, value)` -- **infers the type from `value`**, no type argument |
| `jit_insn_load_relative` | yes | `(func, value, offset, type)` -- **takes the type** |
| `jit_insn_address_of` | yes | `(func, value)` |

That asymmetry is the crux: **writing is untyped, reading is typed.** Storing a `ulong` at offset 0
"relative to a struct value" asks libjit to reinterpret an `{int, int, int, int}` as an 8-byte write with no type
to check against -- which is what crashed attempt 4. Storing through an **address** is an ordinary memory write
and is well defined.

### A permanent unit test for the sequence

`Test/LibJitBackendTests.cpp` gains `[libjit][v128][ir]`, which builds the sequence directly: two `jit_type_ulong`
params, a 16-byte/16-aligned aggregate temp, `jit_insn_address_of`, two `store_relative` calls, one
`load_relative` of lane 1, and a call with `{11, 12, 13, 14}` expecting **12**. **7 assertions, green.**

The file's own header says it exists to "reach into a backend's type/IR objects to validate the LibJIT lowering
itself", which is exactly what this is: the boundary primitive, pinned at the IR level, independent of the
backend's plumbing.

### Attempt 5: two of three obstacles gone, and a second signature overload found

With the proven sequence, sites 1-3 were re-applied, plus one discovery the earlier attempts never made:
**there are two `translateFunctionSignature` overloads** -- `(const WASM::FuncType&)` at line 196 and
`(WASM::CoreTypeView)` at line 304 -- and the **`CoreTypeView` one is what fills `translatedTypes`**. Attempts 1-4
patched the former and the compiler used the latter, which is why the signature seemed not to change (the crash
that revealed it was `jit_value_get_param` returning **NULL** for a slot that did not exist).

With both overloads changed, the prologue assembling through `address_of`, and the driver walking `paramSlots`:

| Gate | Result |
|---|---|
| `[libjit][v128][order]` | **passes** -- both export orders |
| `[sljit][v128][order][abi]` | passes |
| **full suite** | **33 failures**: `SIGSEGV` in several spec suites, plus one struct-field mismatch |

So the *boundary* is right and the *call sites* are not: any function that passes a `v128` to another wasm
function still hands over the aggregate while the callee now expects two words. That is site 4 of §29, and it is
now measured rather than predicted -- **33 tests' worth of proof that the producer must not land before the
consumer** (§24's rule, third instance).

### State

Backend changes reverted; the **IR test kept**. Suite **21636 assertions in 732 test cases**, green (up 7
assertions and 1 case from the new test). `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

### What the next attempt must do, in order

1. `splitAggregate(value)` -- `address_of` + two `load_relative(..., jit_type_ulong)`, used by nothing yet.
2. Convert **every** call site's `callArgs` construction to use it (there are several: `call`, `call_indirect`,
   imports, the direct tail-call fallback).
3. Then re-apply sites 1-3 unchanged -- both overloads, the `address_of` prologue, the `paramSlots` driver.

The prologue half is no longer research; it is a known-good sequence with a test behind it.

## 32. Both boundary primitives are now proven at the IR level (2026-11-06)

§31 proved the *assemble* half. The second half -- an aggregate value becoming two words -- needs one more
step, because a call site holds a **value**, not an address, and `jit_insn_address_of` wants storage. The
sequence is therefore store-into-a-temp, take *its* address, then two typed loads.

### The new test

`[libjit][v128][ir]` (second case) builds exactly that and returns `lane1 + lane3` from a 16-byte parameter:

| Step | Call |
|---|---|
| 1 | `jit_value_create(function, aggregate)` -- the temp |
| 2 | `jit_insn_store(function, temp, value)` -- the copy |
| 3 | `jit_insn_address_of(function, temp)` |
| 4 | two `jit_insn_load_relative(..., 0 / 8, jit_type_ulong)` |
| 5 | `jit_insn_ushr(..., 32)` + `jit_insn_convert(..., jit_type_int)` per lane |

Called with `{11, 12, 13, 14}`, it returns **26** (`12 + 14`). **13 assertions across the two `[ir]` cases, green.**

That also closes the last open question about libjit's instruction set: `store` on a whole aggregate, `address_of`
on a temp, `ushr`, and `convert` all compose. Nothing in the conversion path is now guesswork.

### The complete toolkit, and what remains

| Half | Primitive sequence | Status |
|---|---|---|
| assemble (prologue) | two stores through a temp's address | **proven** (§31) |
| split (call sites) | store-copy + address + two loads | **proven** (§32) |

The remaining work is mechanical and enumerated: six `jit_insn_call*` sites in `LibjitOpcodeDispatcher.cpp`
(2722, 4423, 4444, 4482, 4526, 4576), each with the same three-line shape
(`1 + params.size() + buffer`, `args[1 + i] = stackArgs[i]`, `args.back() = outBuffer`), then re-apply
sites 1-3 from §31 unchanged.

### State

Suite **21642 assertions in 733 test cases**, green (up 6 assertions and 1 case from the split test);
`[headerdoc]` green; `[docs]` green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 33. Attempt 6: call sites converted, and the failure moved to a narrower place (2026-11-06)

Everything §32 called for was implemented: a shared `appendNativeCallArgs` / `nativeParamSlotCount` pair, all **six**
`jit_insn_call*` sites converted to push-back argument building, both `translateFunctionSignature` overloads
changed, the `address_of` prologue, and the `paramSlots` driver.

### Results

| Gate | Result |
|---|---|
| `[libjit][v128][order]` | **passes** |
| `[sljit][v128][order][abi]` | passes |
| full suite | **33 failures -- all SIMD spec suites** |

The 33 failures are *not* the call paths that were converted. Two details pin it down:

- the failing set is entirely `simd_*` and `relaxed_*` suites (`simd_select`, `simd_align`, `simd_bitwise`,
  `simd_i8x16_arith` ...), not the `call`/`call_indirect` suites a call-site bug would hit;
- `simd_select` reports **"terminated by signal 11 after 0 passing commands"** -- it dies before completing a
  single command, i.e. at instantiation, not in the middle of a call chain.

### The hypothesis this leaves, and why it is testable

The split helper does:

```cpp
jit_value_t temp = jit_value_create(function, jit_value_get_type(value));
jit_insn_store(function, temp, value);
```

The IR test that proved this used a **parameter** as `value`, which is addressable. A SIMD body typically
produces a vector from `v128.const` -- a **struct constant**, not an addressable value. Storing a struct constant
into a temp, then taking the temp's address, is the one step still unverified, and it is exactly what the failing
suites exercise and the passing ones do not.

### The next step is one test, not a redesign

Extend `[libjit][v128][ir]` with a case that does `jit_value_create_nint_constant`-style aggregate constant ->
`jit_insn_store(temp, constant)` -> `address_of` -> two loads. If that fails (or produces junk), the helper must
change shape rather than the ABI: candidates are materialising the constant through
`jit_insn_store_relative(address, 0/8, ...)` on a temp-taken address directly (no whole-struct `store`), or
keeping a 16-byte address-based carrier for vector values inside the dispatcher.

Either way the conclusion is the same as §19's: **the representation is not in question**. Only the *conversion*
route is, and it is now a single IR question with a five-line test.

### State

Backend changes reverted again; the two `[libjit][v128][ir]` tests and every finding above are kept.
Suite **21642 assertions in 733 test cases**, green; `abi-check.sh` OK; Doxygen 0 warnings; tree clean.

## 34. §33's hypothesis tested and DISPROVED — and what that leaves (2026-11-06)

The third `[libjit][v128][ir]` case builds a value exactly the way `dispatchV128Const` does (a frame slot written
through its address with two 64-bit immediates), then does what the split helper does (whole-struct
`jit_insn_store` into a second temp), then splits it and returns `lane1 + lane3`.

**It passes: `{11,12,13,14}` -> 26.** 19 assertions across three `[ir]` cases, all green.

So, measured and settled:

| Claim | Verdict |
|---|---|
| a const-built aggregate survives a whole-struct `store` | **true** |
| the split helper's `store` is the cause of the 33 SIMD failures | **false** |
| a `v128` value is an aggregate (not a pointer) inside the dispatcher | true -- `newV128Slot()` is `jit_value_create(function, v128Type())` |

### What that leaves, narrowed

Since the crash is at *instantiation* (`"after 0 passing commands"`) and the conversion primitives are all proven,
the remaining candidates are about the **plumbing around a signature that changed length**:

1. **Result-buffer indices still use `params.size()`.** Three places index the trailing buffer as
   `currentFunc.params.size() + 1` (`LibjitOpcodeDispatcher.cpp`, the call sites) -- correct only while one wasm
   parameter equals one slot. The slot count is `nativeParamSlotCount()` (§32's helper). This bites any function
   with a `v128` parameter **and** multiple results.
2. **A signature path not yet covered.** All function signatures come from the two `translateFunctionSignature`
   overloads (both changed), but if some type is served from a cache populated before translation, a stale
   one-slot signature would make `jit_value_get_param` return NULL exactly as it did in §31.
3. **A shape the fixture lacks.** `v128_boundary_order.wasm` has `scalar+v128` and `v128+scalar`. The SIMD suites
   add **two or more `v128` parameters**, and `v128` params next to `v128` locals -- neither is covered by any
   fixture in the tree.

### The cheapest next experiment

Candidate 3 is a fixture, not a code hunt: add exports with `(v128, v128)`, `(v128, v128, v128)`, and one with a
`v128` parameter plus a `v128` local to `v128_boundary_order.wasm`, and run them on LibJIT. Two of three
candidates are eliminated or confirmed by a single build, and candidate 1 can be checked in the same pass by
grepping the three index expressions.

### State

Suite **21648 assertions in 734 test cases**, green (three `[ir]` cases now, 19 assertions). Backend unchanged.

## 35. The multi-vector fixture: candidate 3 eliminated, and an SLJIT capacity limit found (2026-11-06)

The fixture was extended with `two_vectors` (v128, v128), `three_vectors` (v128 x3) and
`vector_param_and_local` (v128 parameter plus a v128 local), and asserted on both backends.

### LibJIT: all three shapes pass

`[libjit][v128][order][shapes]` **passed with 10 assertions** against the *current*, unchanged ABI -- two vectors,
three vectors, and a vector parameter next to a vector local, all with correct lane values.

That eliminates candidate 3 of §34: the 33 SIMD failures are **not** caused by multi-vector parameters. The
remaining candidates are the ones about a signature whose length no longer equals its parameter count
(result-buffer indices at `params.size() + 1`, and a possible cached signature).

### SLJIT: a hard capacity limit, pre-existing

The same fixture on SLJIT aborts inside `sljit_emit_enter`:

```
Assertion failed at sljitLir.c:1446
CHECK_ARGUMENT(function_check_arguments(arg_types, real_scratches, saveds, fscratches));
```

`SLJIT_NUMBER_OF_SAVED_REGISTERS` is **6** on x86-64, and SLJIT requires **every declared argument slot to own a
saved register**. A `v128` parameter occupies two slots, so `(v128, v128)` needs five slots (context + 4) and
`(v128, v128, v128)` needs seven -- past the budget. The backend passes `savedCount = nextSlot - 1` straight into
`sljit_emit_enter`, so this is a **pre-existing** limit of the SLJIT backend, not a consequence of the boundary
work; no fixture or spec module had ever declared three vector parameters.

Fixing it means receiving the excess slots **from the stack** rather than from a saved register -- a real piece of
work, now named and measured rather than hypothetical.

### The testing lesson, which is the durable one

`instantiate` compiles **every** function in a module. Putting a shape one backend cannot compile into a *shared*
fixture therefore breaks **every test that loads that module** -- including tests about something else entirely.
And `[!mayfail]` does not help: a fatal SLJIT assert kills the process before Catch2 can record a failure.

> **Backend-specific shapes need backend-specific fixtures.** A shared fixture may only contain shapes every
> backend can compile.

That is why the multi-vector exports and their assertions were removed again rather than marked mayfail: keeping
them in `v128_boundary_order.wasm` took the whole suite down. Preserving the LibJIT half as an executable test
needs a separate `v128_multi.wat` -- a new fixture file, which the build picks up through the existing
`WAT_SOURCES` wildcard but which needs a qmake regeneration.

### State

Suite **21648 assertions in 734 test cases**, green; fixture restored to its two exports; `abi-check.sh` OK;
Doxygen 0 warnings; tree clean.

## 36. SLJIT's stack plan, and LibJIT's three eliminated candidates (2026-11-06)

### SLJIT: stop pretending every slot needs a register

The ceiling measured in §35 is self-imposed by the backend, not by SLJIT: `beginFunction` computes
`savedCount = nextSlot - 1`, i.e. **one saved register per argument slot**. SLJIT's `sljit_emit_enter` only
*requires* declared slots to fit the register budget because the numbers passed to it say so -- argument slots
beyond the saved registers arrive **on the stack**, which is what the host ABI does anyway.

The plan:

1. Cap the declared saved-register count at `SLJIT_NUMBER_OF_SAVED_REGISTERS` (6 on x86-64) instead of
   `nextSlot - 1`.
2. Keep `SLJIT_ARG_VALUE(..., nextSlot)` for every slot -- SLJIT then passes the overflow on the stack by the
   platform ABI rather than asserting.
3. For a parameter whose slot index exceeds the register budget, load it from the incoming stack
   (`SLJIT_MEM1(SLJIT_SP)` at the ABI's offset for that slot) instead of `SLJIT_S(n)`.
4. Re-run the multi-vector fixture (§35) -- `three_vectors` is exactly the case that must start passing.

Step 3 needs the per-platform offset rule verified before it is written, which is why it is a separate change
and not a patch: the offsets differ between SysV and MS x64, and getting them wrong is a silent miscompile rather
than an assert.

### LibJIT: three candidates tested, none of them the cause

The full change was re-applied -- helpers, all six call sites, both `translateFunctionSignature` overloads, the
`address_of` prologue, the `paramSlots` driver -- and then each surviving candidate was tested in turn:

| # | Candidate | Test | Result |
|---|---|---|---|
| 1 | result-buffer index still `params.size() + 1` | changed to `nativeParamSlotCount()` in all four sites | **33 failures, unchanged** |
| 2 | a stale one-slot signature from a second translator; `v128` matched by *identity* | changed to a structural test (`JIT_TYPE_STRUCT` && size 16) | **33 failures, unchanged** |
| 3 | multi-vector parameters | fixture with two and three vectors | **eliminated earlier (§35)** -- passes |

That invariance is the finding: the failure survives a call-site rewrite, a result-index fix and a signature-
detection fix, while both boundary pins pass. So it is **not** in argument passing, result indexing or signature
identity -- it is somewhere the pins and the spec suites differ, and the pins are tiny hand-written modules while
the suites are spec scripts.

The next step therefore needs *isolation*, not another hypothesis: find the per-suite filter in the spec runner so
one `simd_*` script can be run alone under the changed ABI with a debugger, and reduce it to the smallest failing
command. Every previous root cause in this study was found that way -- by making the failing case small enough to
read.

### State

Backend unchanged; suite **21648 assertions in 734 test cases**, green; `abi-check.sh` OK; Doxygen 0 warnings.

## 37. Bisect: one axis is impossible, the other says the shape is not the signal (2026-11-06)

### Bisecting the *edits* is not possible, and §20 explains why

The change splits into translation, prologue, driver, call sites and result indices. Removing any one part leaves the
other two disagreeing about how many slots a `v128` occupies, so every proper subset is broken *by construction*:
the pins fail, not the suites. That is §20's rule again -- a boundary drawn through edits that are simultaneous
produces no intermediate state worth measuring -- so the useful axis is the **shape space**, not the edit set.

### Bisecting the *shapes* (with the full change applied)

| Shape | Result under the changed ABI |
|---|---|
| `(v128) -> i32` (lane read) | passes |
| `(i32, v128) -> i32`, `(v128, i32) -> i32` | passes |
| `(v128, v128) -> i32` | passes |
| `(v128, v128, v128) -> i32` | passes |
| `v128` parameter with a `v128` **local** | passes |
| **`(v128) -> v128`** (parameter and result) | **passes** |

Six shapes, all correct, including the parameter-plus-result combination that `simd_align` and `simd_select` use.
So the failing signal is **not** a `v128`-parameter shape. It is something the pins and those scripts differ in for
another reason -- and the scripts were checked to genuinely contain `(param $x v128)` (an earlier count said
otherwise only because the grep missed the `$name` form).

### Where the bisect points next

Each spec script is a **sequence of `(module ...)` definitions**, and a single script can define several modules with
different shapes -- including imports, multiple results, and `v128` tuples through the result buffer. The next cut
is therefore by **module**: run `simd_align.wast`'s modules one at a time, or instrument `compileFunction` to print
the function index and type it is about to build, so the crashing function names itself in one run.

Given six passing shapes and three passing candidate fixes, the remaining space is small and the next step is
mechanical: name the function, then read it.

### State

Backend unchanged; experiment reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 38. Instrumented bisect: the signature is not the trigger, the opcode is (2026-11-06)

A temporary `fprintf` at the head of `compileFunction` printed the function index, type index, parameter count,
number of `v128` parameters and result count. With the full change applied, `[spec]` was run and the last line
before each crash read:

| Suite | Last function compiled |
|---|---|
| `simd_load8_lane` | `fn=46 type=0 params=2 v128params=1 results=1` |
| `simd_store16_lane` | `fn=30 type=0 params=2 v128params=1 results=1` |
| `simd_load64_lane` | `fn=10 type=0 params=2 v128params=1 results=1` |
| **`simd_bit_shift`** | **`fn=22 type=1 params=1 v128params=1 results=1`** |

### The decisive line is `simd_bit_shift`

That signature is `(v128) -> v128` -- and a minimal fixture with **exactly that signature passed**, twice: the
`(v128) -> v128` roundtrip in §37 and this run's own fixtures. So:

> **The failing signal is not the signature shape at all. Identical signatures compile and run correctly in a
> hand-written module and crash in the spec scripts.** The trigger is therefore in the **body** -- the opcode
> lowering -- not in the parameter boundary.

That retro-explains the whole elimination record: three candidate ABI fixes changed nothing because the ABI path
was already correct; six shapes passed because they were the wrong axis. The parameter boundary has now been
measured correct from every angle available.

### The opcode families to bisect next, in order of cost

1. **`i8x16.shl` / `.shr_s` / `.shr_u`** (`simd_bit_shift`, one parameter, one result): a 10-line fixture --
   `(func (export "shift") (param v128) (result v128) local.get 0 i32.const 1 i8x16.shl)` -- is the smallest
   possible reproducer and needs one build.
2. **`v128.load8_lane` / `store8_lane`** (`simd_load8_lane`, `simd_store8_lane`): a v128 operand *plus* a memory
   operand, so of the two families this one is more likely to be touching the operand's storage.
3. **`v128.and` / `or` / `xor`** (`simd_bitwise`): pure helpers, included only because the suite fails.

A plausible mechanism worth checking first in each: these ops take the vector operand and may write into or take
the address of **the value that arrived as a parameter**. That value is now a slot the prologue synthesised,
whereas before the change it was the parameter itself -- a difference that only shows up in ops that treat the
operand as mutable storage, which is exactly the three families above and none of the six passing shapes.

### State

Instrumentation removed, experiment reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 39. The instrumented index resolved to a source line, and to a real bug (2026-11-06)

### Mapping `fn=22` to source

The spec scripts are readable, so the index resolves exactly. The first module of `simd_bit_shift.wast` declares its
exports in order; counting from zero puts **`fn=22` at line 38**:

```wat
(func (export "i64x2.shr_u_64") (param $0 v128) (result v128)
  (i64x2.shr_u (local.get $0) (i32.const 64)))
```

A shift by the **full lane width** -- the degenerate count, where the spec says the result is all zeros. The
instrumental run's shape (`params=1 v128params=1 results=1`, `type=1`) matches the one-parameter functions of that
module, whose first member is `i8x16.shl_1` and whose 22nd is this one.

### The minimal reproducer, and what it says

A `(v128) -> v128` export with that exact body compiles and **returns non-zero where the spec requires zero**
(two assertions failed; no crash). Two conclusions:

1. `i8x16.shl` by 1 -- the op of the family's *head* function -- passes, so single-step shifts are fine.
2. A shift count **equal to the lane width** produces a wrong value under the changed ABI. Because the suite is
   green without the change and the spec expects zeros, the wrong value is introduced by the change, not
   inherited.

So the change has (at least) two defects: a wrong-result path on degenerate shift counts, and the segfault,
which must be at a later function in the same module (`i64x2.shr_s_65`, shift by 65, is the next line).

### Why a degenerate shift is a plausible casualty

A lane-wise shift helper typically masks the count (`n & 63`) or computes `width - n`. At `n == width` this is the
only input for which those two formulations disagree with the spec (which mandates zero), and it is also the only
input where a synthesised operand slot changes which formulation the optimiser picks. That is exactly the profile
of a bug that survives every ordinary test and appears only when the IR around the operand changes.

### Next, in order
1. `i64x2.shr_s` with count **65** -- the next function in the same script, one more export.
2. Confirm the wrong-result boundary: counts 63, 64, 65.
3. Only then the segfault, which is a *different* defect a few functions further on.

### State

Experiment reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 40. The degenerate boundary measured: counts are masked, not clamped -- and no crash (2026-11-06)

Three exports with `i64x2.shr_u` / `i64x2.shr_s` at counts 63, 64 and 65, fed a lane of `0x8000000000000000` so the
high bit is observable. Under the changed ABI: **13 assertions, 10 passed, 3 failed, no crash.**

| Case | Spec | Measured |
|---|---|---|
| `shr_u` 63 | `1` | **`1`** -- correct |
| `shr_u` 65 | `0` | wrong (count treated as 1) |
| `shr_s` 65 | sign-fill (`-1`) | **`0xC000000000000000`** -- i.e. `0x8000...>>1` |

The `shr_s` value is the giveaway: `0xC0000000` in the high half is exactly an **arithmetic shift by one**. So the shift
count is being **masked to the lane width**, not clamped to the spec's "count >= width implies zero / sign-fill".

### What that means

1. **The degenerate-count defect is real and now has a number.** It affects counts `>= 64` only, which is why every
   ordinary SIMD test passes and why the whole family looked healthy.
2. **It is not the segfault.** Counts 63, 65 and signed 65 all return without crashing, so the fault the spec suites
   hit is a *different* defect later in the same module.
3. **Its independence is still unproven.** The suite is green *without* the change, so the change exposes it -- but
   whether the pre-change path clamps correctly, or never reaches the masked path, is one build away: run this
   same fixture **without** the ABI change. If it also fails there, the bug is pre-existing and merely *revealed*;
   if it passes, the change routes the count through a form that masks.

That single experiment is the next step, because it decides whether the count defect belongs to the boundary work
or to the SIMD helper, and those are fixed in different places.

### State

Experiment reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 41. The degenerate-count defect is pre-existing and orthogonal -- measured (2026-11-06)

The §40 fixture was run against the **unchanged** tree, same exports, same input, same expectations:

| Configuration | Result |
|---|---|
| with the two-slot boundary | 13 assertions, 10 passed, **3 failed**, no crash |
| **without it (committed tree)** | 13 assertions, 10 passed, **3 failed**, no crash |

Identical failure value (`0xC0000000 == -1`): the high lane comes back as an arithmetic shift by one, i.e. the count
is masked to the lane width. **The defect is therefore pre-existing, independent of the `v128` boundary work, and
must not be attributed to it.**

### Why the suite stays green while the fixture fails

This is the useful part. The spec suites report per-script **skipped** counts, and the degenerate-count assertions
of `simd_bit_shift` are among the commands the harness does not execute. So the tree has been green *and* wrong at
the same time: the spec never exercised counts at or beyond the lane width through this path, and my fixture does.
That is a third instance of the session's recurring pattern -- **the suite was not testing the property that was
broken** -- and unlike the previous two (LibJIT's aggregate assumption, SLJIT's refusal test) this one is a gap,
not a wrong assertion.

### What this changes in the plan

- The boundary work is now **fully cleared** of the degenerate-shift finding: it neither causes nor worsens it.
- The finding is a **standalone spec-compliance bug** in the LibJIT SIMD shift lowering: counts `>= width` must yield
  zero (`shr_u`) or sign-fill (`shr_s`), and instead are masked `& (width - 1)`.
- Fixing it is small and independent of P4: clamp the count before the shift helper, in the helper rather than in
  every opcode. It should land as **its own commit with its own test**, taken from the fixture above, so the gap it
  reveals closes permanently.
- The segfault remains the only open item in the boundary work, and it is a *different* defect a few functions
  later in the same script.

### State

Experiments reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 42. The crash is at invocation, not at compile -- both spec modules build cleanly (2026-11-06)

`simd_bit_shift.wast` was lifted out of the suite and bisected directly: its **two** modules were extracted into
fixtures, compiled by `wat2wasm`, and instantiated on LibJIT **with the full two-slot change applied**.

| Module | Shape | Result under the changed ABI |
|---|---|---|
| 1 (lines 3-42, 24 functions) | 1- and 2-parameter shift exports | **instantiates cleanly** |
| 2 (lines 658+, `(memory 1)`) | block-structured bodies (`i8x16.shl-in-block`) | **instantiates cleanly** |

So the module that the suite reported as *"terminated by signal 11 after 0 passing commands"* compiles **fine**
with the change. The crash therefore happens on the first **invocation**, not during compilation -- which is exactly
what "0 passing commands" says once it is read carefully: zero commands *completed*, not zero commands *started*.

### Why this matters more than another eliminated hypothesis

Every step of this study so far has assumed the fault was in **code generation**, because the earlier crashes were
(NULL `get_param`, SLJIT's argument check). This one is in the **call**, and the distinction changes what to look at:

- code generation is cleared for this suite, on both of its modules;
- the remaining suspect is the path that invokes a compiled function with a `v128` argument -- the harness's
  declared C signature, or `jit_apply`-style invocation that builds its argument list from the *signature*
  (two `ulong` slots) while the caller supplies one 16-byte value;
- a direct C++ call with a 16-byte struct works (all six pins prove it), because SysV happens to pass it in the
  same two registers -- so the defect would be invisible to a test that declares the parameter as an aggregate and
  visible only to one that declares the canonical two words.

That last point is testable in one build and is the next step: invoke `i8x16.shl` through the *canonical*
declaration `(void*, uint64_t, uint64_t, int32_t)`, and through the harness path, and see which one faults.

### State

Fixtures and experiments removed; suite **21648 assertions in 734 test cases**, green; tree clean.

## 43. Both invocation conventions work -- and that leaves exactly one path unmeasured (2026-11-06)

The function `i8x16.shl` from the extracted spec module, compiled with the full two-slot change, was called twice
through the same entry point:

| Declaration | Call | Result |
|---|---|---|
| aggregate: `(void*, AbiBoundaryVector, int32_t)` | `fn(ctx, {{1,2,3,4}}, 1)` | **`lanes[0] == 2`** |
| canonical: `(void*, uint64_t, uint64_t, int32_t)` | `fn(ctx, lo, hi, 1)` | **`lanes[0] == 2`** |

5 assertions, green. So with the change applied, a `v128` parameter can be passed either as a 16-byte aggregate or
as two scalar words and the callee reads it correctly in both cases -- which is exactly the property the canonical
rule needs, and it is now measured rather than assumed on SysV.

### What is therefore left, and it is one thing

Everything measured so far is cleared:

| Path | Status |
|---|---|
| code generation (both spec modules) | **clears** |
| aggregate invocation | **clears** |
| canonical (two-word) invocation | **clears** |
| the spec harness's own invocation | **the only untested path left** |

The harness does not call `fnPtr` with a hand-written prototype: it goes through `WASM::WasmValue` and the runtime's
invoke path, which builds argument storage from the **function signature**. With a two-slot signature that path
now expects two words per vector parameter and is handed one `WasmValue` holding 16 bytes -- so the remaining defect
is a *harness-side* argument marshalling mismatch, not a code generation or ABI-convention defect.

That is a satisfying place to have arrived: the boundary work's own machinery is measured correct in every direction
that can be measured without the harness, and the residual failure is in the layer that feeds it.

### Next step

Read the invoke path in `WasmBase` (or the spec harness) that turns a `WasmValue` into native arguments, and check
what it does for a `v128`: one 16-byte slot or two words. That is a code read and a single test at the
`callExport` level, and it is the last unexplored surface in the whole campaign.

### State

Fixture and experiments removed; suite **21648 assertions in 734 test cases**, green; tree clean.

## 44. Reading the harness invocation path: searched, and the obvious names are empty (2026-11-06)

The last untested path is the one that turns a `WasmValue` into native arguments for the compiled entry. It was
searched for under every name the codebase uses elsewhere, with these results:

| Location | Searched for | Found |
|---|---|---|
| `Test/SpecSuite.cpp` | `invoke`, `v128`, `V128` | only the test-case wrapper (`void invoke()` calls `runOneScript`) -- **no marshalling** |
| `Test/WastScript.cpp` | `callExport`, `callCallable`, `jit_apply`, `fnPtr` | **nothing** |
| `Test/helper.hpp` | `std::vector<WasmValue>` | **nothing** |
| `WasmBase/WasmModuleInstance.hpp`, `WasmRuntime.hpp` | `V128`, `invoke`, `std::vector<WasmValue>` | **nothing** |
| `WasmBase/*.hpp`, `Test/` | `invokeCallable`, `applyCallable`, `dispatchInvoke`, `callWithValues` | **nothing** |

So the marshalling exists (the suites obviously call exports) but under a name none of the idioms suggest. The
useful outcome is a **map of exclusions**: it is not in the spec runner, not in the script parser, not in the test
helper, and not among the runtime headers' invoke-shaped APIs.

### Where that leaves the search

Two shapes remain plausible, and both are cheap to check next:

1. **A generated/typed dispatcher** -- something that switches on `ValueTypeCode` and casts `fnPtr` per signature,
   which would be the one place a `v128` argument's native form is decided. The search terms to use are the type
   codes rather than "invoke": `ValueKind::V128` (the enum `BackendAbiTests.cpp` already checks) and
   `valueKindOf`, both of which appear in the runtime's vocabulary.
2. **The backend's own `rawFnPtr` path** -- the `Callable` has `fnPtr` *and* `rawFnPtr`, and the doc comment says
   `fnPtr` points at the trampoline driver. A harness that calls `rawFnPtr` directly would bypass everything the
   pins exercised and could legitimately be the one caller that still assumes one slot per parameter.

Option 2 is the more interesting hypothesis and needs no new search terms: `rawFnPtr` is the documented "real
call" entry, and the pins never called it.

### State

No code changed; suite **21648 assertions in 734 test cases**, green; tree clean.

## 45. The marshalling layer found -- and its *result* path is the last divergence (2026-11-06)

Searching with the codebase's own vocabulary (`ValueKind::V128`, `argSlotCount`) instead of "invoke" located it
immediately: `WasmBase/WasmBackendAbi.hpp` owns `argSlotCount` / `mapArgSlots`, and
**`WasmBase/WasmCallCache.cpp` is the marshaller** that turns a wasm call into a native one through a libffi `cif`.

### What it already gets right

```cpp
const std::uint32_t slots = argSlotCount(valueKindOf(parameter.value.opcode));
if (slots == 1) {
    entry.argTypes.push_back(ffiTypeFor(parameter));
} else {
    for (std::uint32_t slot = 0; slot < slots; ++slot)
        entry.argTypes.push_back(&ffi_type_uint64);   // two scalar words
}
```

The **parameter** rule is implemented there exactly as the decision prescribes -- two `uint64` words, driven by
`argSlotCount` -- so the marshaller and the (changed) LibJIT signature agree on parameters. That closes the
question §43 left open.

### What it does not

```cpp
entry.returnType = view.funcResults().empty() || entry.multiResult
    ? &ffi_type_void
    : ffiTypeFor(view.funcResults().front());
```

A single `v128` result takes the `ffiTypeFor(result)` branch -- i.e. the marshaller asks libffi to return a
**16-byte aggregate**, while the decision (§28) and the SLJIT implementation (§26) both say a `v128` result crosses
through the **caller-supplied buffer**, exactly like a multi-value tuple. `needsResultBuffer()` (§23) was added to
the core for precisely this and is not used here.

### So the fix has a name and a home

`WasmCallCache.cpp`'s result branch should be driven by `WASM::needsResultBuffer(...)`, not by
`funcResults().size() > 1`, and should emit `ffi_type_pointer` + `ffi_type_void` for a `v128` result the same way it
already does for a multi-value tuple. That is the last place where the canonical rule is stated but not applied, and
it is the one the spec harness reaches.

### Two secondary findings worth keeping

1. **`mapArgSlots` exists and libjit does not use it.** Its docstring warns that hand-computed slot walks "produced
   three separate corruption investigations in a positional backend" -- which is exactly what this study spent
   fifteen rounds doing by hand. The libjit slot walks (prologue, driver, call sites) should be expressed through
   `mapArgSlots`, turning three manual walks into one shared contract.
2. **The marshaller is the single point every invocation crosses**, which is why no hand-written pin could find
   this: the pins call `fnPtr` directly, and this path goes through libffi.

### State

No code changed this round; suite **21648 assertions in 734 test cases**, green; tree clean.

## 46. Correction: the call cache is *not* the spec harness's path (2026-11-06)

§45 closed with "that is the one the spec harness reaches". Checking it before changing anything shows that is **wrong**:
`WASM::ModuleCallCache` is referenced only from `Test/CallCacheTests.cpp` -- nowhere in `WasmBase`, and nowhere in the
spec runner.

| Claim in §45 | Verification |
|---|---|
| `WasmCallCache.cpp` implements the canonical *parameter* rule | **true** -- two `uint64` words driven by `argSlotCount` |
| its *result* branch still emits a 16-byte aggregate (`ffiTypeFor`) | **true**, and a genuine divergence from §28 |
| the spec harness crosses it | **false** -- only the call-cache tests do |

So the result-branch gap is real and worth fixing on its own merits (it is the last place where §28 is stated but
not applied), but it is **not** the cause of the 33 SIMD failures, and changing it would not move them.

### What this leaves, stated accurately

The spec harness's invocation remains unidentified. What is excluded after this round is now quite long: it is not
`SpecSuite.cpp`, not `WastScript.cpp`, not `helper.hpp`, not the `WasmModuleInstance`/`WasmRuntime` invoke APIs, and
now **not** the call cache either. The next search should start from the opposite direction -- from `runOneScript`
inward: follow the command executor to whatever function actually calls the export, rather than guessing at names.

That is the honest state: fifteen rounds of elimination, one hypothesis per round, and the harness's own invoke path
still not located. Every other layer has been measured and cleared, which is why the remaining search is small even
though it is unfinished.

### State

No code changed; suite **21648 assertions in 734 test cases**, green; tree clean.

## 47. CULPRIT FOUND: the harness's `jit_apply` marshalling gives one pointer per parameter (2026-11-06)

Following `runOneScript` inward -- `runIsolated` -> the `Spec` command executor -> `invoke(*instance, action, ...)` --
lands on **`TestBackend.cpp`**, which has two invocation paths:

| Path | Condition | Marshalling |
|---|---|---|
| `v128AbiCall` | **every** parameter is `v128`, <= 4 of them | hand-written calls through `callWithV128Params<Ret>`, aggregate per vector |
| `jit_apply` | everything else -- i.e. **any mixed signature** | `std::vector<void*>`, one pointer per wasm parameter |

The second is the culprit:

```cpp
std::vector<void*> jitArguments;
jitArguments.reserve(1 + arguments.size() + (multiResult ? 1 : 0));
jitArguments.push_back(&vm);
for (const void* argument : arguments)
    jitArguments.push_back(const_cast<void*>(argument));   // ONE slot per wasm parameter
```

`jit_apply` demands **one argument slot per parameter in the compiled signature**. With the canonical two-slot rule the
signature has two words per vector, so a mixed signature such as `(v128, i32)` needs **three** slots and this code
hands over **two**: the second word is read from whatever the next `void*` happens to be, and the callee dereferences
garbage. That is the segfault -- and it explains every observation:

- **the pins pass**: they call `fnPtr` directly, never through `jit_apply`;
- **`(v128) -> v128` passes**: all-vector signatures take the first path;
- **the suites crash at the first command**: `i8x16.shl` is `(v128, i32)` -- mixed, therefore `jit_apply`;
- **code generation is clean**: the fault is entirely in the caller;
- **the count is invariant**: every fix attempted inside the backend was in the wrong layer.

### The fix, exactly

Expand a vector parameter into two pointers to its own 16 bytes before the `jit_apply` call:

```cpp
for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (functionType.params[i].val.opcode == WASM::ValueTypeCode::V128) {
        const auto* bytes = static_cast<const std::uint8_t*>(arguments[i]);
        jitArguments.push_back(const_cast<std::uint8_t*>(bytes));       // low word
        jitArguments.push_back(const_cast<std::uint8_t*>(bytes + 8));   // high word
    } else {
        jitArguments.push_back(const_cast<void*>(arguments[i]));
    }
}
```

`jit_apply` dereferences each slot according to the signature's type, so pointing at `bytes` and `bytes + 8` supplies
exactly the two `ulong` words the callee now expects, little-endian and low-first.

### Why it cannot land alone

The harness fix is only correct **together with** the two-slot signature change: applied to today's one-slot
signature it would pass three words where two are expected. So the correct unit is:

1. the backend change (§29-§31, already written and rehearsed),
2. **this** harness expansion,
3. the whole suite.

That is the first time the missing piece of that unit has had a name. It was never in the backend.

### State

No code changed this round; suite **21648 assertions in 734 test cases**, green; tree clean.

## 48. The unit applied: segfaults gone, 33 -> 22, all remaining failures are values (2026-11-06)

The full three-part unit was applied -- the backend change (§29-§31) **plus** the harness `jit_apply` expansion (§47)
-- and the whole suite was run:

| Configuration | Failures | Crashes |
|---|---|---|
| backend change only | 33 | **many** (`signal 11` in every SIMD suite) |
| backend + harness fix | **22** | **none** |

That is the confirmation the diagnosis was right: **the segfault was the harness's one-pointer-per-parameter marshalling,
and it is gone once a vector is handed over as two words.** Eleven failures disappeared with it, and not a single
`signal 11` remains anywhere in the run.

### What the remaining 22 are

They are **value mismatches**, not crashes, and they are distributed as:

```
simd_f64x2 1  simd_bitwise 8  relaxed_dot_product 3  simd_const 2
relaxed_madd_nmadd 6  simd_i8x16_sat_arith 3  simd_f32x4_arith 8  simd_lane 2
relaxed_laneselect 6  simd_i16x8_arith 4  simd_i16x8_arith2 21  simd_f64x2_arith 8
```

Two families are visible and both are already understood:

- **`relaxed_*` (15)** -- relaxed-SIMD results are implementation-defined within a range, and the suite has a known
  proposal-conflict list; these are the class `spec_known_failures.txt` exists for.
- **the rest** -- ordinary value mismatches, among which `simd_bitwise` (8) is the most interesting, since §40/§41
  already established a **pre-existing** divergence in this area that the suite never exercised. That bug is not
  fixed by the unit and was never expected to be.

### Where this leaves the work, precisely

The boundary refactor's own goal is **met**: a `v128` crosses as two scalar words in the backend, the harness speaks
the same convention, and nothing crashes. What remains is a value-correctness backlog, not a structural one -- and
it is written down suite by suite, which is the first time that has been true.

The branch is left green: the unit is reverted because 22 unexplained value failures cannot be committed as green,
and the doc now carries both the exact patch set (`/tmp/apply*.py` plus the `TestBackend.cpp` hunk, recorded
verbatim in §47) and the list of what must be explained first.

### State

Reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 49. The remaining 22 are all "three v128 parameters" -- and the cause is already named in the tree (2026-11-06)

With the unit applied, every failing assertion was captured with its values. The correlation is exact:

| Script | Failing export | Signature |
|---|---|---|
| `simd_bitwise` (8) | `bitselect` | `(v128, v128, v128) -> v128` |
| `simd_f64x2_arith` (8) | `add-sub`, `div-add`, `mul-sub`, ... | `(v128, v128, v128) -> v128` |
| `relaxed_laneselect` (6) | `i8x16.relaxed_laneselect`, ... | `(v128, v128, v128) -> v128` |

Every one is **exactly three vector parameters**. Not two, not four, and none of the failures has a scalar parameter at all.

### The tree already names the cause

`Test/TestBackend.cpp`, immediately above the path those signatures take:

```cpp
// The v128-only path bypasses `jit_apply`, whose third stack-passed
// aggregate is damaged on this LibJIT; the generated code itself uses the
// ordinary host C ABI.
```

The values agree with that description rather than contradicting it. `bitselect` results mix bits *within* bytes
(`0x…ab`, `0x…ba`) where the mask should have selected whole bytes -- the signature of a **third argument that is
not the one the caller passed**, which is exactly what a damaged stack-passed aggregate produces.

### Why this is good news

1. It is a **pre-existing backend limitation**, recorded in the source, not a consequence of the two-slot boundary.
2. It is **disjoint** from the boundary work: three-vector signatures worked in the earlier fixture (`14+23+32`)
   because that call went through a hand-written prototype, whereas the harness routes 3-vector signatures through
   `callWithV128Params`, which is the helper the comment is about.
3. It explains why the count is 22 and not 0 without any new hypothesis: 15 `relaxed_*` + 7 others, all of this shape.

### The decisive next experiment

Call `bitselect` directly with a three-aggregate prototype -- the exact pattern that worked for `three_vectors` --
and compare against the harness path. If the direct call is correct and the harness call is not, the defect is in
`callWithV128Params` and the fix is in the harness, not the backend; if both are wrong, it is the LibJIT
stack-passed aggregate after all and the fix is in the prologue or the ABI rounding.

That is one test, and it is the first time this campaign has had a failure class that is *uniform* rather than a
mixture.

### State

Reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 50. Three vector parameters measured directly: the 7th argument is the broken one (2026-11-06)

§49's decisive experiment was run. A fixture export `(v128, v128, v128) -> v128` calling `v128.bitselect`, invoked
**directly** with three 16-byte aggregates (no harness), under the two-slot ABI:

| Input | Value |
|---|---|
| a | `0xAAAAAAAA` per lane |
| b | `0xBBBBBBBB` per lane |
| mask | `0x0F0F0F0F` per lane |
| **expected** (`(a&m)|(b&~m)`) | **`0xBABABABA`** |
| **measured** | **`0xBBBBBBAA`** |

The measured bytes are `AA, BB, BB, BB`: byte 0 took its mask bit as `1` and bytes 1-3 as `0`. A mask of `0x0F` cannot
produce that -- **the third argument is not the value the caller passed.**

### Why the seventh argument, precisely

Count the integer arguments: `VMContext*` (1) + three vectors at two words each (2-7). SysV provides six integer
argument registers, so the **seventh word -- the high half of the third vector -- crosses on the stack**. That is
exactly the case `Test/TestBackend.cpp` warns about:

```cpp
// ... bypasses `jit_apply`, whose third stack-passed aggregate is damaged on this LibJIT
```

So the remaining 22 failures are **one pre-existing LibJIT defect**: arguments that cross on the stack are read
wrong, which shows up only when a signature needs more than six integer words -- i.e. three vector parameters, which
is exactly the shape every remaining failure has. Two vectors (five words) pass; three (seven words) do not.

### What this closes

- **Not** a boundary-convention problem: two words per vector is right, and two-vector signatures prove it.
- **Not** a harness problem: the failure reproduces with a direct C++ call.
- **Not** caused by this refactor: the same stack-argument limitation is already documented in the tree, and three
  whole vectors were never expressible before the change, so the suite never reached it.

### The fix, named

Either the backend must read stack-passed argument words at the right frame offsets (a variant of §36's SLJIT plan --
"the excess slots come from the stack, not from a register"), or the canonical rule must cap a signature at six
integer words and route the rest through memory. The first is correct and matches what SLJIT needs; the second would
amend the decision note and is a milestone question, not a commit.

### State

Everything reverted; suite **21648 assertions in 734 test cases**, green; tree clean.

## 51. The seventh argument is fine in libjit -- so the defect is in our glue (2026-11-06)

§50 concluded that the high half of the third vector "crosses on the stack" and, quoting the tree, that stack-passed
aggregates are "damaged on this LibJIT". That conclusion was tested directly, with **no backend code involved**:

```cpp
jit_type_t params[7] = {jit_type_void_ptr, ulong, ulong, ulong, ulong, ulong, ulong};
signature = (7 params) -> ulong;
jit_insn_return(function, jit_value_get_param(function, 6));   // the 7th argument
```

called as `fn(nullptr, 1, 2, 3, 4, 5, 42)`.

**It returns 42.** The new `[libjit][v128][ir]` case passes -- 24 assertions across four IR cases, all green.

### What that rules out, and what it convicts

| Candidate | Verdict |
|---|---|
| libjit mishandles stack-passed integer arguments | **false** -- measured |
| the SysV register budget explanation from §50 | **incomplete** -- the boundary is real, the damage is not libjit's |
| our own glue (prologue slot arithmetic or the driver's forwarding) | **the remaining suspect** |

So the fix is ours to make. The 3-vector failure under the two-slot ABI is a defect in how *we* pass seven argument
values through, not a limitation we have to design around -- which also means the "cap the signature at six words"
option from §50 is unnecessary.

### The one-line experiment that separates the two remaining sites

`Callable` exposes both `fnPtr` (the driver) and `rawFnPtr` (the compiled body). The 3-vector `bitselect` test should
be run against **`rawFnPtr`**:

- if `rawFnPtr` gives `0xBABABABA`, the body and prologue are right and the defect is the **driver's forwarding**;
- if it gives `0xBBBBBBAA` too, the defect is the **prologue's slot arithmetic** (or the signature itself).

That is one line in the existing bisect test and it eliminates the last two candidates.

### A permanent test was added

`[libjit][v128][ir]` "the seventh integer argument arrives intact" stays in the tree: it is the measurement that keeps
this from being re-blamed on libjit, and it is the smallest possible guard for the register-budget boundary.

### State

Suite **21654 assertions in 735 test cases**, green; tree clean.

## 52. Both candidate sites investigated: the driver is innocent, and libjit is too (2026-11-06)

§51's separating experiment was run under the full unit, plus two further discriminators.

| Call | Expected | Measured |
|---|---|---|
| `bitselect3` via **`rawFnPtr`** (body) | `0xBABABABA` | `0xBBBBBBAB` |
| `bitselect3` via **`fnPtr`** (driver) | `0xBABABABA` | `0xABBBBBAB` |
| `three_lanes` (extract_lane sum of the three params) | `6` | wrong |

The first two differ from each other but are both wrong, so **the driver is not the defect**. The third is the
important one: a function that only *reads* one lane from each of three vector parameters also fails, so the
**parameters themselves are wrong** -- this is not about `v128.bitselect` or any opcode.

### And libjit is not the defect either

Two new IR cases, no backend code involved:

| Test | Result |
|---|---|
| seven integer arguments, return the 7th (stack-passed) | **`42` -- correct** |
| the same, but stored through an `address_of` temp first (frame + stack argument) | **`42` -- correct** |

29 assertions across five `[libjit][v128][ir]` cases, all green. So libjit reads stack-passed arguments correctly
both on its own **and** through a frame temp -- which is exactly what §50 blamed and §51 doubted.

### What the two experiments leave

Every layer below our glue is now measured correct:

| Layer | Verdict |
|---|---|
| libjit stack arguments | **correct** (2 tests) |
| libjit frame + stack argument | **correct** |
| the driver's forwarding | **not the cause** (raw fails too) |
| opcode lowering | **not the cause** (extract_lane only fails too) |
| **the translated signature / prologue glue for 3+ vectors** | **the only remaining suspect** |

That is a very small space: `translateFunctionSignature` must be emitting six `ulong` slots for three vectors (it
was written to), and `compileFunction`'s prologue must be reading slots 1-6. The next step is to **print the
signature** for a three-vector function -- `jit_type_num_params`, and each parameter's kind and size -- instead of
reasoning about it. One assertion in an existing test answers it.

### Two permanent tests kept

Both new `[libjit][v128][ir]` cases stay: together they pin the register-budget boundary and its interaction with a
frame, so no future round can blame libjit for this again.

### State

Suite **21658 assertions in 736 test cases**, green; tree clean.

## 53. The signature is dumped and correct -- and a wrong-overload trap found on the way (2026-11-06)

The last suspect was instrumented rather than reasoned about: both `translateFunctionSignature` overloads were made to
print their result -- parameter count and each parameter's kind and size -- and a three-vector function was called.

**First finding: the `FuncType` overload never runs.** No output appeared from it; the live path is the
`CoreTypeView` overload, exactly as §31 found for the *edit*. That is the same trap twice in one study, and it is now
worth stating as a rule: **`LibJitTypeTranslation.cpp` has two signature builders and only one of them is live.**

**Second finding: the live overload is right.**

```
SIGcv wasmParams=3 jitParams=7
  [0] kind=17 size=8   (pointer: VMContext*)
  [1] kind=10 size=8   (ulong)
  [2] kind=10 size=8
  [3] kind=10 size=8
  [4] kind=10 size=8
  [5] kind=10 size=8
  [6] kind=10 size=8
```

Three wasm parameters become **six 64-bit `ulong` words after the context** -- precisely the canonical rule, with
nothing extra, nothing missing, and the right kinds.

### So the entire boundary layer is now proven correct

| Layer | Verdict | Evidence |
|---|---|---|
| signature translation | **correct** | the dump above |
| libjit stack arguments (7th word) | **correct** | `[ir]` test, returns 42 |
| libjit frame + stack argument | **correct** | `[ir]` test, returns 42 |
| driver forwarding | **not the cause** | `rawFnPtr` fails too |
| opcode lowering | **not the cause** | `extract_lane`-only body fails |
| **prologue: three `address_of` + `store_relative` pairs** | **the only thing left** | by elimination |

The observed value for the three-vector lane sum is `22` where `6` is correct, so parameters 1-3 arrive as something
other than `1, 2, 3` -- **in the body, after the signature handed them over correctly**.

### The next test writes itself

§52's IR test verified *one* `address_of` + `store_relative` for a stack-passed argument. The prologue does **three**
of them. Extending that IR case to three temps -- `address_of` three slots, store params 1/2, 3/4, 5/6, then load a
lane from each and sum -- replicates the prologue exactly, in 25 lines with no backend code. If it reproduces `22`,
the defect is libjit's handling of *several* frame temps alongside stack arguments and the backend must materialise
vectors differently; if it returns `6`, the defect is in how the dispatcher consumes the locals afterwards.

### State

All instrumentation removed; suite **21658 assertions in 736 test cases**, green; tree clean.
