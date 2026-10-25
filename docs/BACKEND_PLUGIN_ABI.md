# Backend Plugin ABI — Dynamically Loaded Code Generators Behind a Minimal C ABI

Status: feasibility study + interface sketch (revision 1 — 2026-10-25;
measurements on `LibjitDispatcher` at `ea4fb30`). **No code, no decision to
implement.** It closes nothing; it prepares the design so that the decision, when
taken, is a build change rather than a redesign.
Audience: anyone who will implement Asmjit or SLJIT, freeze the public ABI
([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md)), or package the library.

The idea, as originally conceived: **load code generators at run time from shared
libraries, the way the 1998 Unreal Engine chose a renderer** (Glide, OpenGL,
Direct3D, software) — a small C ABI exposes functions that return a pointer to a
backend implementation plus metadata (name, version, limits), with optional
settings exchange, so the frontend never knows which renderer it got.

Verdict up front:

| Question | Answer |
|---|---|
| Is loading a backend from a `.so` feasible? | **Yes, high.** The backend is already a shared library; the process already resolves `libjit.so.0` at load time. |
| Is a **stable C ABI** feasible? | **Medium.** Only for *discovery, lifecycle, metadata and a POD signature/layout view*. The compilation seam itself consumes C++ types (`ModuleInstance&`, `ModuleInstanceInternals&`), so a fully C seam is a much larger, separately-versioned commitment (§4.3). |
| Does it demote any class-E blocker? | **No.** It is orthogonal to [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md); it does not change what a backend has to *do*. |
| Is it still worth doing? | **Yes — in two stages.** Freeze the descriptor shape now (even while loading at link time); add `dlopen` behind an opt-in later. That converts a future decision into a build change. |

One observation reframes the whole problem: **the engine already has a C ABI — it
runs backwards.** Generated code calls `wasm_trap_slot()`, `wasm_trap_report()`,
`wasm_stack_check()`, `wasm_exn_slot()`, `wasm_cont_*`, `wasm_atomic_*` and
`wasm_v128_*` as plain `extern "C"` symbols in `WasmBase/`. The new work is the
*frontend → backend* direction, and it is smaller than it looks for exactly the
same reason: the dangerous boundary (code ↔ runtime) is already C.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | How far does the Unreal-renderer analogy go before it breaks? |
| §3 | What does the tree already provide? |
| §4 | What would the interface look like? |
| §5 | What can cross the boundary, and what must not? |
| §6 | What are the boundary rules a plugin must obey? |
| §7 | What are the hard problems that make this "medium" and not "easy"? |
| §8 | What are the alternatives, and when does each win? |
| §9 | What does this do to the milestone and the frozen surfaces? |
| §10 | Plan (`BP-<n>`) |
| §11 | Test survey |
| §12 | References |

## 2. The analogy, and where it breaks

### 2.1 What maps

| Unreal renderer plugin | WasmJit backend plugin |
|---|---|
| Chosen once per game/engine instance | Chosen once per runtime instance |
| Replaces an implementation behind a fixed interface | Replaces a code generator behind `ModuleInstantiator` |
| Exposes name/version + a "can you do X?" capability query | Same: `name`, `version`, capabilities (SIMD, components, continuations) |
| Failure to load = fall back to another renderer | Failure to load = "backend unavailable", not a crash |
| Loaded by name from a directory | Loaded by name from a directory (`libwasmjit-backend-<name>.so`) |

### 2.2 Where it breaks

| Difference | Consequence |
|---|---|
| A renderer owns pixels; a backend **emits code that the core then calls** | The backend must agree with the core on layouts, helpers and the calling convention (`LAYERING.md` §4). A renderer never has to agree with the engine on a struct layout. |
| A renderer can be swapped mid-frame; a backend cannot be swapped mid-module | A backend is a *creation-time* choice: an instance's code belongs to the backend that compiled it, forever (`Callable::fnPtr` is raw code). |
| Renderers are self-contained; a backend is **half of a pair** | The core's helpers (`wasm_v128_*`, `wasm_trap_*`, …) are the other half. The plugin therefore *depends on* `libWasmBase`, and it must be the one shared copy (§7.2). |
| A renderer's ABI is data (vertices, uniforms); a backend's ABI is **C++ objects** on the compilation path | This is the real cost. A pure C seam needs a C projection of `ModuleInstance`, `ModuleInstanceInternals` and the type views (§4.3). |

## 3. What the tree already provides

| Fact | Value | Evidence |
|---|---|---|
| Backends are already shared libraries | `TEMPLATE = lib`, `CONFIG += shared` | `LibJit/LibJit.pro`, `Asmjit/Asmjit.pro`, `Sljit/Sljit.pro` |
| They already have versioned sonames | `libLibJit.so.1.0.0`, `libLibJit.so.1` → `.so` | `ls build/Desktop-Debug/LibJit` |
| The executable already resolves them by soname | `NEEDED libLibJit.so.1`, `libWasmBase.so.1` | `readelf -d build/Desktop-Debug/Test/WasmJit` |
| …through hard-coded rpath entries | `-Wl,-rpath,$ORIGIN/../LibJit`, etc. | `Test/Test.pro` |
| A backend selector already exists | `WASMJIT_BACKEND` env var; only `libjit` is built | `Test/TestBackend.cpp` |
| A backend-neutral façade already exists | `TestBackend::Session` (`instantiator`, `aggregateLayout`, `createLaneWiseOp`, `invoke`) | `Test/TestBackend.hpp` |
| The product seam exists | `ModuleInstantiator`: 4 pure virtuals + a shared `instantiate()` | `WasmBase/WasmModuleInstance.hpp` |
| A code↔runtime C ABI already exists | `wasm_trap_slot`, `wasm_trap_report`, `wasm_stack_check`, `wasm_exn_slot`, `wasm_cont_*`, `wasm_atomic_*`, `wasm_v128_*` | `WasmBase/*.hpp` (`extern "C"` blocks) |
| Everything is exported | **23,284 dynamic symbols** (1,764 functions), no visibility flag | `nm -D --defined-only libWasmBase.so.1.0.0` |
| Nothing is installed | no `INSTALLS`, no `.pc`, qmake-default `VERSION` only | every `.pro` |

Two rows matter most: the **selector** already exists (so the concept is accepted),
and **everything is exported** (so symbol hygiene is not cosmetic — it is what
makes a declared boundary possible at all).

## 4. What the interface would look like

### 4.1 The single required export

```c
/* wasmjit_backend.h — the only header a plugin author includes */
#define WASMJIT_BACKEND_ABI_VERSION 1u

/* The one symbol a loader looks up with dlsym(). */
WASMJIT_EXPORT const wasmjit_backend_desc*
wasmjit_backend_query(uint32_t host_abi_version);

/* Optional: a human-readable reason the last query failed. */
WASMJIT_EXPORT const char* wasmjit_backend_last_error(void);
```

A plugin that does not export `wasmjit_backend_query` is not a backend. A plugin
that exports it but answers with a different `abi_version` is refused **before any
of its function pointers are called** — the version check must not depend on
calling into the plugin.

### 4.2 The descriptor

```c
typedef struct wasmjit_status {
    int32_t     code;      /* 0 = ok; negative = a trap-like failure */
    const char* message;   /* borrowed, valid until the next plugin call */
} wasmjit_status;

typedef struct wasmjit_capabilities {
    uint32_t flags;            /* SIMD, COMPONENTS, CONTINUATIONS, PROFILES, ... */
    uint32_t max_params;       /* the backend's own signature limit */
    uint32_t max_results;
    uint32_t reserved;
    const char* const* target_names;   /* "x86_64-linux-gnu", ... */
} wasmjit_capabilities;

typedef struct wasmjit_backend_desc {
    uint32_t              abi_version;       /* == WASMJIT_BACKEND_ABI_VERSION */
    uint32_t              core_abi_version;  /* the core ABI it was built against */
    const char*           name;              /* "libjit", "asmjit", "sljit" */
    const char*           version;           /* the backend's own version */
    wasmjit_capabilities  capabilities;

    /* lifecycle: one backend object per process or per runtime */
    wasmjit_status (*create)(const wasmjit_host* host, void** out_backend);
    void           (*destroy)(void* backend);

    /* one session per compilation context (per thread) */
    wasmjit_status (*create_session)(void* backend, void** out_session);
    void           (*destroy_session)(void* session);

    /* POD queries the frontend needs for marshalling */
    wasmjit_status (*aggregate_layout)(void* session,
            const uint8_t* field_kinds, uint32_t field_count,
            wasmjit_layout* out);
    wasmjit_status (*invoke)(void* session,
            const wasmjit_callable* callable,
            const wasmjit_signature* signature,
            void* const* arguments,
            void* result, uint32_t result_capacity,
            wasmjit_trap* out_trap);
} wasmjit_backend_desc;
```

`wasmjit_host` is the **callback table the core hands to the plugin** — allocator,
diagnostics emitter, trap reporter, and the helper-function addresses the plugin is
allowed to call. It is versioned with the same `abi_version` and is the only way a
plugin reaches the core.

### 4.3 The seam question, stated honestly

`ModuleInstantiator`'s hooks take `WASM::ModuleInstance&`,
`WASM::ModuleInstanceInternals&` and `WASM::Module&` — C++ types with container
members and no stable layout. Two designs follow:

| Design | What crosses the C ABI | What the plugin sees | Cost | When it wins |
|---|---|---|---|---|
| **1 — C discovery, C++ seam** | Descriptor, metadata, lifecycle, and the POD `invoke`/`aggregate_layout` helpers. The compilation seam is reached through a C++ `ModuleInstantiator*` the plugin returns as `void*`. | The core's **backend SDK headers** ([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6.1); the C++ ABI must match | Low — this is today's `WASMJIT_BACKEND` selection plus `dlopen` | Always, for in-tree backends |
| **2 — Full C seam** | Everything, including compilation: the core passes **opaque handles** plus accessor functions for each field a backend needs | Only C headers; no C++ ABI dependence | High: a new, permanently versioned C surface mirroring ~a dozen core types | Only if an out-of-tree backend built by a different compiler is a goal |

**Recommendation: Design 1 now; Design 2 only if someone asks for it.** Design 1
delivers everything that matters today — load by name, black-box testing,
contained failure, per-runtime backend choice — and its ABI burden is the one we
have to freeze anyway (`EMBEDDING_SURFACE.md` §7). Design 2 buys binary
portability across compilers, which no C++ engine can promise for its own public
headers either.

### 4.4 The one POD the core is missing, and should gain regardless

`wasmjit_signature` (param/result kind arrays) and `wasmjit_layout` (size,
alignment, field offsets) are needed by *any* backend-neutral invoker. The harness
already has both concepts — `TestBackend::AggregateLayout` and its `AbiV128`
workaround. Promoting them into the core produces the same artifact `ABI.md`
needs for `AAS2`: a POD signature view an embedder, a harness and a plugin can all
read. That makes `BP-1` useful even if `dlopen` never ships.

## 5. What can cross the boundary

| Crosses | Examples | Notes |
|---|---|---|
| POD structs by value/pointer | the descriptor, status, capabilities, layout, trap record, POD signature view | Layout pinned by `static_assert` in the header |
| Opaque handles | `void*` backend / session / instance / internals | Ownership stated per handle |
| Function pointers (both ways) | the `wasmjit_host` table; the plugin's vtable | The core's table is `const` and outlives the plugin |
| `Callable` (POD) | `{ void* fnPtr; VMContext* context; uint32_t localTypeIdx; TypeId typeId; }` | Already the universal handle; `fnPtr`/`context` are the ABI |
| `VMContext*` | arg0 of every compiled function | The plugin may read the fields `LAYERING.md` §4 permits |

| Must **not** cross | Why | Escape hatch |
|---|---|---|
| C++ exceptions | Unwinding through a foreign frame is UB, and the engine's trap model already forbids it (`SAFETY.md` §2.4) | Return `wasmjit_status`; the core turns it into a trap/diagnostic |
| RTTI / `typeid` / `dynamic_cast` | Type identity is per runtime, not per ELF object | Use `TypeId` |
| STL containers in signatures | ABI-unstable across compilers and versions | Pass (`pointer`, `count`) pairs |
| `std::string` / `std::function` / `std::pmr` | Same, plus allocator identity | Borrowed `const char*`, function pointers |
| Templates instantiated on both sides | Two copies, two ABIs | Put the template behind an `extern "C"` thunk |
| Ownership of core objects | Double free / use-after-free | The core owns core objects, the plugin owns its handles; every `create` has a `destroy` on the same side |

## 6. Boundary rules a plugin must obey

1. **Link the one shared `libWasmBase`**, never a private copy (§7.2). Verify with
   `readelf -d` in the loader and refuse a plugin that carries its own core.
2. **`core_abi_version` must equal the host's.** Refuse at query time, before any
   call.
3. **No global mutable state.** One plugin handle per runtime; sessions are per
   compilation context. Anything else makes a runtime non-isolatable.
4. **No threads, no signal handlers, no `atexit` games.** A code generator does
   not schedule; a plugin that wants concurrency asks through the host table.
5. **Never `abort()` or throw.** Report a `wasmjit_status` and let the core decide.
6. **Declare capabilities, do not guess.** A backend that cannot do components or
   continuations says so; `Runtime` refuses the module before codegen, the way
   `ProfileConfig` does.
7. **Do not unload while code is live.** A `Callable::fnPtr` points into the
   plugin forever; the loader must refuse `dlclose` (or leak intentionally) while
   any instance from that plugin exists.
8. **Executable memory is the plugin's**, with a W^X handoff; the core never
   patches code.

## 7. The hard problems

| # | Problem | Why it is real here | Mitigation |
|---|---|---|---|
| 1 | **`libstdc++` / `GLIBCXX` skew** between core and plugin | A `std::string` or `std::vector` crossing the boundary is a version bomb | Nothing STL-shaped crosses (§5); plugin and core come from the same tree/release |
| 2 | **Duplicated core globals** if `libWasmBase` is linked statically into both | `Store::global()` would exist **twice** — two type registries, two GC heaps, silent corruption. The most dangerous failure mode of the whole idea. | Hard requirement: exactly one shared `libWasmBase`; the loader checks `NEEDED` and refuses a plugin that embeds a copy. **Until `ES-2` ([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §4.3) removes the singleton, this is a correctness constraint, not a size preference.** |
| 3 | **`TypeId` / `TagId` values are runtime-scoped** | A cached id from runtime A is meaningless in runtime B | The plugin caches nothing across runtimes; ids travel only with the instance |
| 4 | **`Callable` lifetime vs `dlclose`** | A code pointer into an unmapped library crashes on the next call | Rule 7; prefer never unloading in-process |
| 5 | **Helper symbol export** | The plugin calls `wasm_v128_*` / `wasm_trap_*`; `ES-4`'s hidden visibility would hide them | The **helper ABI must be explicitly `WASMJIT_API`-exported**: surface S6 of [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2 becomes a link-time requirement of the plugin ABI |
| 6 | **W^X and JIT hardening** | The plugin allocates executable memory; a hardened embedder may refuse RWX | Capability flag + documented `mmap`/`mprotect` expectation; the core never writes into code |
| 7 | **Debug/sanitizer mixing** | A plugin built with ASan into a non-ASan frontend (or vice versa) misbehaves subtly | The plugin reports its build flags; the loader refuses a mismatch |
| 8 | **Loading is arbitrary code execution** | A `.so` from an untrusted directory is a backdoor | The embedder decides which plugins are trusted; document it, do not pretend to sandbox |
| 9 | **Failure containment** | A plugin that segfaults takes the game down | `dlopen` failure and version mismatch are *reported*; an in-process crash cannot be contained — that is §8 option D |

## 8. Alternatives, and when each wins

| Option | Flexibility | Isolation | ABI burden | Cost | Verdict |
|---|---|---|---|---|---|
| **A. Build-time: one backend per build** | Low (rebuild to switch) | None | None | None | Today's de-facto model |
| **B. Link-time: several backends, choose at startup** | Medium (no rebuild to switch) | None | C++ ABI only | Low — the `WASMJIT_BACKEND` selector generalised | **The right next step** |
| **C. `dlopen` plugin (this note)** | High (ship backends separately; per-runtime choice) | None (same process) | Descriptor + POD signatures; C++ ABI for Design 1 | Medium | **Adopt the *shape* now (`BP-1`), the loader later (`BP-3`)** |
| **D. Separate process + IPC** | High | **Real** (a crash or heap corruption stays contained) | A wire protocol — much larger | High (latency, shared memory, lifecycle) | Only for untrusted content; out of scope |

The Unreal analogy is option **C**, and the historical reason it worked is that a
renderer's interface is *data*: `DrawPolygon` crosses a boundary designed for
crossing. A code generator's interface is `ModuleInstance&`, which was not. That
is why this note recommends freezing a **narrow** boundary — discovery, lifecycle,
metadata, POD signature/layout/invoke — and leaving the compilation seam in C++
until someone proves a need for more.

## 9. Effect on the milestone and the frozen surfaces

| Question | Answer |
|---|---|
| Does it demote a class-E blocker? | **No.** Orthogonal to [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md): it changes *how* a backend is loaded, not what a backend must do. |
| Does it strengthen G3? | **Yes.** A loaded plugin is a second implementation of the seam that can be tested as a black box — exactly the `AAS10` evidence G3 lacks. |
| Does it add a frozen surface? | **Yes**, and that is the price: `wasmjit_backend.h` becomes surface **S10** of `API_ABI_STABILITY.md` §2.2, with its own `abi_version`. |
| Does it conflict with `ES-4` (hidden visibility)? | It *depends* on it: the helper ABI must be exported deliberately. Without `ES-4`, the plugin ABI is "23k symbols and hope". |
| Does it require `ES-2` (no global `Store`)? | Not strictly, but §6 rule 1 is unenforceable in practice until the core is one shared object and the singleton is gone. |

## 10. Plan

| Milestone | Work | Class | Done when |
|---|---|---|---|
| **BP-0 — This study** ✅ | The analogy, the descriptor sketch, the boundary rules, the risk register. No code. | — | Linked from `API_ABI_STABILITY.md` and `README.md`. |
| **BP-1 — Freeze the narrow boundary** | Define the POD `signature` / `layout` / `status` / `capabilities` views **in the core** and document them (they are `AAS2` deliverables too). No loading. | A | `ABI.md` describes them; the harness's `TestBackend::AggregateLayout` is expressible in terms of the core POD. |
| **BP-2 — Generalise the selector** | A name-based backend factory (`Runtime::create("libjit")`) behind the existing `WASMJIT_BACKEND` idea; a null backend as the second implementation (`AAS10`). Still statically linked. | A | Two backends are selectable at startup without recompiling the frontend. |
| **BP-3 — Add `dlopen`** | Loader with the `abi_version` check, the `NEEDED` check (exactly one core), and contained failure. Opt-in; the compiled-in path stays the default. | A | A plugin built in a separate directory loads by name; a wrong-ABI plugin is refused with a message, not a crash. |
| **BP-4 — Unload policy and containment tests** | Refuse unload while code is live; tests for a plugin that fails to load, one with a wrong ABI, one whose `create` returns an error. | A | The three failure paths are covered by tests and produce diagnostics, never aborts. |
| **BP-5 — Package backends** | Install the backend `.so` files and their `.pc` `Requires:` entries alongside the core. | A | `make install` yields a runtime that finds `libwasmjit-backend-libjit.so` in the install prefix. |

Ordering: `BP-1` first (useful on its own; unblocks `AAS2`), `BP-2` before `BP-3`
(a name-based factory makes `dlopen` a drop-in), `BP-4`/`BP-5` after the loader
works.

## 11. Test survey

| Evidence | Result | Reading |
|---|---|---|
| Default suite (2026-10-25) | 11,011 assertions / 442 cases, green | The compiled-in path is the baseline every `BP-*` stage must preserve. |
| `WASMJIT_BACKEND` selection | Implemented; `libjit` is the only value; unknown names throw | The selector exists, the second implementation does not. |
| Plugin load test | **Absent** | `BP-3`. |
| Wrong-ABI / missing-symbol test | **Absent** | `BP-3`/`BP-4`. |
| Symbol-hygiene check | **Absent**; 23,284 exported symbols | `ES-4`; without it the plugin ABI is undeclared. |
| "One core" check (`NEEDED` inspection) | **Absent** | `BP-3`; it guards risk 2, the most dangerous one. |

## 12. References

- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) — the `WASM::Runtime` that calls
  `create(name)`, the header tiers, and the visibility/soname policy this ABI
  depends on.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2/§7.3 — the frozen surfaces
  (S6 and the new S10) and `AAS10`.
- [`LAYERING.md`](LAYERING.md) §4 — the contract rows a plugin must honour.
- [`ABI.md`](ABI.md) "Wide value ABI", "Callable" — the C-shaped pieces the plugin
  already exchanges, and the helper entry points it calls.
- [`SAFETY.md`](SAFETY.md) §2.4 — no exceptions/`abort` on the trap path, which is
  rule 5.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §5 — the per-backend
  threading rules a plugin inherits.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §3 — what each engine offers, which is
  what a capability flag has to describe.
- Source anchors: `Test/TestBackend.cpp` (`WASMJIT_BACKEND`), `Test/Test.pro`
  (rpath + link lines), `LibJit/LibJit.pro`, `WasmBase/*.hpp` (`extern "C"`
  helper blocks).

