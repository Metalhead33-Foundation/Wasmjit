# Backend Plugin ABI {#doc_backend_plugin_abi}

*Dynamically Loaded Code Generators Behind a Minimal C ABI*

Status: implementation reference (revision 3 — 2026-11-13). `BP-1`…`BP-5` are
shipped: the POD views, the name-based registry, the `dlopen` loader, its refusal
tests, and the plugin packaging. §4 now quotes the interface as it is, including
the C signature/layout views that closed the last gap this note tracked. The
**full C compilation seam is still deliberately not taken** (Design 1, §4.3).
Audience: anyone who will implement another code generator, freeze the public ABI
([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md)), or package the library.

The idea, as originally conceived: **load code generators at run time from shared
libraries, the way the 1998 Unreal Engine chose a renderer** (Glide, OpenGL,
Direct3D, software) — a small C ABI exposes functions that return a pointer to a
backend implementation plus metadata (name, version, limits), with optional
settings exchange, so the frontend never knows which renderer it got.

Verdict, re-read against the shipped tree:

| Question | Answer |
|---|---|
| Is loading a backend from a `.so` feasible? | **Yes — and it ships.** `WASM::loadBackendPlugin(name)` and `loadBackendPluginFile(path)` exist, and `[plugin]` loads a real plugin built in its own directory. |
| Is a **stable C ABI** feasible? | **Medium, and taken only where it pays.** Discovery, lifecycle, metadata and the POD signature/layout view are C. The compilation seam itself consumes C++ types (`ModuleInstance&`, `ModuleInstanceInternals&`), so a fully C seam is a much larger, separately-versioned commitment (§4.3). |
| Does it demote any class-E blocker? | **No.** It is orthogonal to [`proposals/THREADS.md`](proposals/THREADS.md); it does not change what a backend has to *do*. |
| Is it still worth doing? | **Done, in two stages.** The descriptor and the loader are frozen surfaces (`BP-2`…`BP-5`); a later, purely additive change would be the full C seam of Design 2, which nothing currently needs. |


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
| §4 | What **is** the interface, field by field? |
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

## 3. What the tree already provided (the baseline this study started from)

Read this table as **history**: it records the state the study found, before
`BP-0`…`BP-5` and before `ES-4`/`ES-5`. Two rows have since changed and are kept
only so the starting point stays visible: the export row (all symbols were
exported; `ES-4` then added `-fvisibility=hidden` plus `WASMJIT_API`, and the
current count is in [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §10) and the
install row (there was no install; `ES-4`/`ES-5` added the layout,
`wasmjit.pc` and `Test/install-check.sh`). For the shipped state, `EMBEDDING_SURFACE.md`
and `Test/install-check.sh` are authoritative.

| Fact | Value | Evidence |
|---|---|---|
| Backends are already shared libraries | `TEMPLATE = lib`, `CONFIG += shared` | `LibJit/LibJit.pro`, `Asmjit/Asmjit.pro`, `Sljit/Sljit.pro` |
| They already have versioned sonames | `libLibJit.so.1.0.0`, `libLibJit.so.1` → `.so` | `ls build/Desktop-Debug/LibJit` |
| The executable already resolves them by soname | `NEEDED libLibJit.so.1`, `libWasmBase.so.1` | `readelf -d build/Desktop-Debug/Test/WasmJit` |
| …through hard-coded rpath entries | `-Wl,-rpath,$ORIGIN/../LibJit`, etc. | `Test/Test.pro` |
| A backend selector already exists | `WASMJIT_BACKEND` env var; `libjit`, `sljit` and `asmjit` are built and selectable | `Test/TestBackend.cpp` |
| A backend-neutral façade already exists | `TestBackend::Session` (`instantiator`, `aggregateLayout`, `createLaneWiseOp`, `invoke`) | `Test/TestBackend.hpp` |
| The product seam exists | `ModuleInstantiator`: 4 pure virtuals + a shared `instantiate()` | `WasmBase/WasmModuleInstance.hpp` |
| A code↔runtime C ABI already exists | `wasm_trap_slot`, `wasm_trap_report`, `wasm_stack_check`, `wasm_exn_slot`, `wasm_cont_*`, `wasm_atomic_*`, `wasm_v128_*` | `WasmBase/*.hpp` (`extern "C"` blocks) |
| Everything was exported | **23,284 dynamic symbols** (1,764 functions), no visibility flag — *before `ES-4`* | `nm -D --defined-only libWasmBase.so.1.0.0` |
| Nothing was installed | no `INSTALLS`, no `.pc`, qmake-default `VERSION` only — *before `ES-4`/`ES-5`* | every `.pro` |

Two rows matter most: the **selector** already exists (so the concept is accepted),
and **everything is exported** (so symbol hygiene is not cosmetic — it is what
makes a declared boundary possible at all).

## 4. The interface, as shipped

`WasmBase/wasmjit_backend.h` **is** the interface, and has been since `BP-1`.
Everything below is quoted from that header or from a plugin that compiles
against it (`LibJit/LibJitPlugin.cpp`, `Test/ExampleBackend/`); nothing here is a
proposal.

### 4.1 The single required export

```c
/* wasmjit_backend.h — the only header a plugin author includes. */
#define WASMJIT_BACKEND_ABI_VERSION 3u

/* The one symbol a loader looks up with dlsym(). */
typedef const wasmjit_backend_desc* (*wasmjit_backend_query_fn)(uint32_t host_abi_version);

/* Marks a backend's required export as visible in the shared library. */
#if defined(_WIN32)
#  define WASMJIT_BACKEND_EXPORT __declspec(dllexport)
#else
#  define WASMJIT_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

/* Optional: a human-readable reason the last query failed. */
WASMJIT_BACKEND_EXPORT const char* wasmjit_backend_last_error(void);
```

The header declares the *type* of the entry point but not the symbol: exporting
it belongs to the plugin's image. The executing LibJIT backend's implementation
is the whole handshake:

```cpp
extern "C" WASMJIT_BACKEND_EXPORT const wasmjit_backend_desc*
wasmjit_backend_query(uint32_t host_abi_version)
{
	g_lastError = nullptr;
	if (host_abi_version != WASMJIT_BACKEND_ABI_VERSION) {
		g_lastError = "the host speaks an unsupported backend ABI version";
		return nullptr;
	}
	return &g_descriptor;
}
```

A plugin that does not export `wasmjit_backend_query` is not a backend. The
version is **3**, and each increment records a change the loader has to police:
2 added `build_flags`, and 3 added the `ModuleInstantiator::releaseInstance`
virtual slot — the unloader of [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md)
§9 — which changes the vtable a plugin was built against, so a plugin built
against 2 is refused at load time instead of calling through a stale slot.
`WASM::loadBackendPluginFile` performs that check before it touches any other
pointer in the descriptor, and `[plugin]` (`Test/PluginTests.cpp`) covers every
refusal.

### 4.2 The descriptor

```c
/* Versioned host callback table lent to a backend plugin. */
typedef struct wasmjit_host {
	uint32_t abi_version;      /* == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t core_abi_version; /* the host core's ABI version */
	void*    user_data;        /* opaque host state passed to `emit` */
	void   (*emit)(void* user_data, int32_t severity, const char* message);
} wasmjit_host;

/* Versioned backend descriptor returned by wasmjit_backend_query. */
typedef struct wasmjit_backend_desc {
	uint32_t             abi_version;      /* == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t             core_abi_version; /* the core ABI it was built against */
	uint32_t             build_flags;      /* WASMJIT_CURRENT_BUILD_FLAGS (§7.7) */
	uint32_t             reserved;         /* must be 0 */
	const char*          name;             /* the backend's own name */
	const char*          version;          /* the backend's own version */
	wasmjit_capabilities capabilities;     /* what the backend can compile */

	/* Lifecycle. `create` returns one opaque instantiator per call. */
	wasmjit_status (*create)(const wasmjit_host* host, void** out_instantiator);
	void           (*destroy)(void* instantiator);
} wasmjit_backend_desc;
```

A real descriptor is a static initialiser, so the field order in the header is
the field order in every backend:

```cpp
wasmjit_backend_desc g_descriptor = {
	WASMJIT_BACKEND_ABI_VERSION,
	WASMJIT_ABI_VERSION,
	WASMJIT_CURRENT_BUILD_FLAGS,
	0, // reserved
	"libjit",
	WASMJIT_VERSION_STRING,
	wasmjit_capabilities{
		WASMJIT_CAP_SIMD | WASMJIT_CAP_PROFILES | WASMJIT_CAP_EXECUTES,
		0, 0, 0, nullptr},
	&createInstantiator,
	&destroyInstantiator,
};
```

`create` returns the `ModuleInstantiator` as an opaque `void*` and `destroy`
takes it back. `wasmjit_host` is the one callback the core lends — a
diagnostics emitter whose severity is `0..3`; the severity mapping lives in
`WasmBackendRegistry.cpp` (`hostEmit`), and reporting never throws.

**What the descriptor deliberately does not carry.** Each omission is a
consequence of Design 1 (§4.3), not a gap:

| Absent | Instead |
|---|---|
| `create_session` / `destroy_session` | The session is `WASM::BackendSession` ([`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp)), returned through the C++ seam; it names no backend and needs no second C lifecycle. |
| `aggregate_layout` | The rule is the core's: `WASM::computeAggregateLayout` in `WasmBackendAbi.hpp`, and §4.4's C view of its result. |
| `invoke` | Invocation is `BackendSession::invoke`, which owns every calling-convention detail. |
| an allocator or trap reporter in `wasmjit_host` | The plugin allocates with its own toolkit; the trap path is reached through the exported `wasm_trap_*` helper symbols (§7.5). |

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

### 4.4 The POD signature and layout views

A backend-neutral invoker needs four numbers per value — a kind, a size, an
alignment and an offset — and until recently the core published them only in C++
(`WASM::SignatureView`, `WASM::AggregateLayout`, `WASM::FieldLayout` in
`WasmBackendAbi.hpp`). `wasmjit_backend.h` now carries the same views in plain C,
so a plugin, the harness and an embedder can exchange them without sharing an STL
type:

```c
typedef enum wasmjit_value_kind {          /* mirrors WASM::ValueKind */
	WASMJIT_KIND_VOID = 0, WASMJIT_KIND_I32, WASMJIT_KIND_I64, WASMJIT_KIND_F32,
	WASMJIT_KIND_F64, WASMJIT_KIND_V128, WASMJIT_KIND_REF
} wasmjit_value_kind;

typedef struct wasmjit_signature {
	const uint8_t* params;       /* wasmjit_value_kind entries, or NULL */
	uint32_t       param_count;
	const uint8_t* results;
	uint32_t       result_count;
} wasmjit_signature;

typedef struct wasmjit_field_layout {
	uint32_t offset, size, alignment;
} wasmjit_field_layout;

typedef struct wasmjit_layout {
	uint32_t              size, alignment;
	wasmjit_field_layout* fields;      /* caller-owned */
	uint32_t              field_count;
} wasmjit_layout;
```

`WasmBackendAbi.hpp` owns the bridge in both directions — `toWasmjitSignature` /
`signatureView`, `toWasmjitLayout` / `layoutView`, and the `wasmjitKind` /
`valueKindOfWasmjit` pair for the kind codes — and pins the layouts with
`static_assert`s, so the two views cannot drift apart silently. Every conversion
borrows: no array is copied, and the storage stays the caller's.

That closes `AAS2` in the same artifact [`ABI.md`](ABI.md) needs: one POD
signature view an embedder, a harness and a plugin can all read, whether or not
the backend that produced it is in-process. `[backendabi]`
(`Test/BackendAbiTests.cpp`) pins the kind codes and both round trips.

## 5. What can cross the boundary

| Crosses | Examples | Notes |
|---|---|---|
| POD structs by value/pointer | the descriptor (including `build_flags`), status, capabilities, layout, trap record, POD signature view | Layout pinned by `static_assert` in the header |
| Opaque handles | `void*` backend / session / instance / internals | Ownership stated per handle |
| Function pointers (both ways) | the `wasmjit_host` table; the plugin's vtable | The core's table is `const` and outlives the plugin |
| `Callable` (POD) | `{ void* fnPtr; VMContext* context; uint32_t localTypeIdx; TypeId typeId; void* rawFnPtr; void* tailEntry; }` | Already the universal handle; `fnPtr`/`context` are the ABI. `rawFnPtr` is the compiled entry behind `fnPtr`'s driver, and `tailEntry` is the per-type tail-dispatch thunk (docs/TAILCALLS.md TC-5; `WRITING_A_BACKEND.md`). An **additive** field change; a release that removes or reorders one needs a major bump. |
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
4. **Compilation is serialized; execution may be concurrent.** The host never
   calls `compile` on two threads at once (concurrent compilation is rejected as
   a feature — `THREADSAFETY.md` §3.2 R2), and the plugin may cache per-compilation
   state in the session. **Invocation must be re-entrant**: several agents may call
   through the same session concurrently, so the invoker keeps every per-call
   value on the stack, never in the session.
5. **No threads, no signal handlers, no `atexit` games.** A code generator does
   not schedule; a plugin that wants concurrency asks through the host table.
6. **Never `abort()` or throw.** Report a `wasmjit_status` and let the core decide.
7. **Declare capabilities, do not guess.** A backend that cannot do components or
   continuations says so; `Runtime` refuses the module before codegen, the way
   `ProfileConfig` does.
8. **Do not unload while code is live — and, once *used*, never unload at all.**
   A `Callable::fnPtr` points into the plugin forever; the loader must refuse
   `dlclose` (or leak intentionally) while any instance from that plugin exists.
   **Recorded policy (2026-11-01):** a plugin image that has *executed* stays
   mapped until the process ends — `dlclose` is only correct for a plugin that was
   **probed** (name, version, capabilities read from the descriptor) and never
   instantiated. That is why the loader keeps every handle
   (`keepPluginLoaded`), and why `Runtime::unload` frees the *instantiator and its
   compilation context*, not the mapped image: the two are different resources,
   and only the second is unsafe to give back. **The probe-only path now exists
   (2026-11-01):** `probeBackendPlugin(path)` opens, reads the descriptor, copies
   every value and string out, and closes; `listBackendPlugins()` scans
   `WASMJIT_BACKEND_PATH` + `.` + the executable's directory for
   `libwasmjit-backend-*.so` and probes each, skipping incompatible files. That is
   the one legitimate `dlclose` site — a probed plugin never ran, so nothing can
   point into it afterwards. A *loaded-and-used* plugin still stays mapped for the
   process lifetime, which is what `keepPluginLoaded` enforces.
9. **Executable memory is the plugin's**, with a W^X handoff; the core never
   patches code.
10. **Report every refusal through the diagnostic sink.** The loader emits a
    `WASM.BACKEND.PLUGIN` diagnostic (Error) and *then* throws; a successful load
    is a Note. A plugin never reports a failure by crashing (§7.9).
11. **Same toolkit as the host** (§7.1): same compiler, same version, same
    standard library. `build_flags` covers only the debug/sanitizer split; the
    rest is a documented requirement, not a runtime check.
12. **Modules outlive their importers** (§7.4), and only trusted directories are
    searched for plugins (§7.8).

## 7. The hard problems, and how each is handled

> **Just because you technically can does not mean that you should.**
>
> This note describes a boundary that *works*, not a set of operations that are
> *wise*. Each row below says what the engine guarantees and what it deliberately
> leaves to the embedder's judgement.

| # | Problem | Why it is real here | What the engine does | What is left to the embedder |
|---|---|---|---|---|
| 1 | **`libstdc++` / `GLIBCXX` skew** | A `std::string` or `std::vector` crossing the boundary is a version bomb | Nothing STL-shaped crosses (§5), and the descriptor carries the build kind, so a sanitizer/debug mismatch is refused (§7.7) | **Build the core, every backend and the embedding application with the same compiler toolkit** — same compiler, same version, same standard library. That cannot be probed at load time, so it is a soft requirement stated in `wasmjit_backend.h`; anything else is at your own peril |
| 2 | **Two copies of the core** | Two heaps, two type registries, two allocators in one process | **This is a dynamic-linking library, never a static one.** The build refuses a static core, `libLibJit` records `NEEDED libWasmBase.so.1`, and the loader refuses a plugin that embeds a copy | Never static-link the core — not into a backend, not into an embedder that also loads one |
| 3 | **`TypeId` / `TagId` are store-scoped** | A cached id from runtime A uses the wrong registry in runtime B | **By design**: it is the price of the isolation `ES-2` bought — two runtimes cannot reach each other's types or heaps, so a stale id fails loudly instead of silently aliasing an unrelated type | A plugin caches nothing across runtimes; ids travel only with the instance. Running two runtimes in one process is possible *and* discouraged (the rule at the top of this section): one runtime is the normal case, and a second is a deliberate isolation decision. The same goes for parsing one module twice to get two independent copies — it works, and it is usually a mistake |
| 4 | **`Callable` lifetime vs `dlclose`** | A code pointer into an unmapped library crashes on the next call | The loader never closes a handle: there is no unload entry point (rule 8) | Do not `dlclose` a library whose functions you still call — ordinary C common sense. The same common sense applies to modules: **a module must outlive every module that imports and calls its exports**; the engine does not track that for you |
| 5 | **Helper symbol export** | The plugin calls `wasm_v128_*` / `wasm_trap_*`; `ES-4`'s hidden visibility would hide them | The exported helper subset is deliberate and tested: `wasm_trap_*`, `wasm_stack_check`, `wasm_v128_*` (31), `wasm_exn_*`; `[plugin]` asserts they resolve in the process image. Surface S6 of [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2 becomes a link-time requirement of this ABI | A plugin links these helpers; it does not re-export them. String, bit-op and GC-allocation helpers are backend-private and stay hidden |
| 6 | **W^X and JIT hardening** | The plugin allocates executable memory; a hardened embedder may refuse RWX | The core never writes into code, and whether the plugin can allocate at all is a capability question (§4.2, `WASMJIT_CAP_EXECUTES`) | A host that refuses RWX needs a backend that does not require it: LibJIT — the shipped backend — can **interpret instead of compile** (`jit_function_apply` falls back to its interpreter; `JIT_OPTLEVEL_NONE` disables codegen — <https://www.gnu.org/software/libjit/doc/libjit_2.html>). The engine does not pretend to change the host's W^X policy |
| 7 | **Debug/sanitizer mixing** | A plugin built with ASan into a non-ASan frontend (or vice versa) misbehaves subtly | `wasmjit_backend_desc::build_flags` reports the build kind; the loader compares the debug/release and sanitizer bits and refuses a mismatch, naming both sides. A successful load reports its build kind as a note | Keep the flags honest by building with the same flags — the check catches an accident, not a lie |
| 8 | **Loading is arbitrary code execution** | A `.so` from an untrusted directory is a backdoor | Nothing. There is no sandbox, no signature check, no path allow-list | **You decide which plugins are trusted.** `WASMJIT_BACKEND_PATH` is yours to set; never point it at a directory an attacker can write. This is a plugin boundary, not a security boundary |
| 9 | **Failure containment** | A plugin that segfaults takes the game down | Every refusal — missing file, missing export, descriptor or core ABI, build kind, a plugin without its own core, a failing `create` — **emits a `WASM.BACKEND.PLUGIN` diagnostic and then throws**; a successful load emits a note (rule 10). An in-process crash is *not* contained; only §8 option D (a separate process) contains one | Watch the diagnostic sink; treat a crash as a crash. `dlopen` failure and version mismatch are *reported*, never swallowed |

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

**Verdict:** C. C all the way.

## 9. Effect on the milestone and the frozen surfaces

| Question | Answer |
|---|---|
| Does it demote a class-E blocker? | **No.** Orthogonal to [`proposals/THREADS.md`](proposals/THREADS.md): it changes *how* a backend is loaded, not what a backend must do. |
| Does it strengthen G3? | **Yes.** A loaded plugin is a second implementation of the seam that can be tested as a black box — exactly the `AAS10` evidence G3 lacks. |
| Does it add a frozen surface? | **Yes**, and that is the price: `wasmjit_backend.h` becomes surface **S10** of `API_ABI_STABILITY.md` §2.2, with its own `abi_version` (currently **3**: 2 added the build-kind field of §7.7, 3 added the `releaseInstance` slot of the unloader). |
| Does it conflict with `ES-4` (hidden visibility)? | It *depends* on it: the helper ABI must be exported deliberately. Without `ES-4`, the plugin ABI is "23k symbols and hope". |
| Does it require `ES-2` (no global `Store`)? | Not strictly, but §6 rule 1 is unenforceable in practice until the core is one shared object and the singleton is gone. |

## 10. Plan

| Milestone | Work | Class | Done when |
|---|---|---|---|
| **BP-0 — This study** ✅ | The analogy, the descriptor sketch, the boundary rules, the risk register. No code. | — | Linked from `API_ABI_STABILITY.md` and `README.md`. |
| **BP-1 — Freeze the narrow boundary** ✅ | **Landed 2026-10-29:** `WasmBase/WasmBackendAbi.hpp` defines `ValueKind`, `SignatureView`, `FieldLayout`, `AggregateLayout` and `computeAggregateLayout` — standard-layout, layout-pinned by `static_assert`, and the executable form of [`ABI.md`](ABI.md) "The result tuple layout" / "`v128` by Value". `Test/BackendAbiTests.cpp` (`[backendabi]`) asserts the rule. | A | `ABI.md` describes them ✅; the harness's `TestBackend::AggregateLayout` is expressible in terms of the core POD ✅ |
| **BP-2 — Generalise the selector** ✅ | **Landed 2026-10-29:** `WasmBackendRegistry.{hpp,cpp}` (name → factory, `WASMJIT_REGISTER_BACKEND`), the null instantiator as the second implementation, the executing backend registered from its own translation unit, and `Runtime::create(name)`. | A | Two backends (`libjit`, `null`) are selectable by name with no frontend rebuild ✅ |
| **BP-3 — Add `dlopen`** ✅ | **Landed 2026-10-29:** `loadBackendPluginFile` / `loadBackendPlugin` with `RTLD_NOW|RTLD_LOCAL`, the `wasmjit_backend_query` handshake, descriptor-, core-ABI and build-kind checks before any other call (§7.7), a host callback table, contained failure **reported through the diagnostic sink** (§7.9), and the one-core guard: the plugin's `wasm_trap_slot` must resolve to the host's address, otherwise it carries its own core and is refused (a symbol-identity probe rather than a `DT_NEEDED` parse — no libelf, the same failure caught). `Test/ExampleBackend` builds a real plugin in its own directory and `[plugin]` loads it by name. | A | A plugin built in a separate directory loads by name ✅; a wrong-ABI or wrong-build plugin is refused ✅; a plugin with its own core is refused ✅; every refusal is a `WASM.BACKEND.PLUGIN` diagnostic ✅ |
| **BP-4 — Unload policy and containment tests** ✅ | **Landed 2026-10-29:** the loader keeps every plugin handle for the process lifetime (rule 8; there is no unload entry point), and `[plugin]` covers a missing file, a library without the query symbol, an unknown name, a descriptor-ABI mismatch, a core-ABI mismatch and a failing `create` — each a diagnostic-bearing exception, never an abort. | A | Those failure paths are covered by tests and produce diagnostics, never aborts ✅ |
| **BP-5 — Package backends** ✅ | **Landed 2026-10-29; dynamic alias 2026-11-04:** `LibJit.pro` installs `libLibJit.so.<major>` and the `libwasmjit-backend-libjit.so` loader alias into `$$PREFIX/lib`. `wasmjit.pc` deliberately describes only the core and names no backend; the embedder loads the backend through `WASMJIT_BACKEND_PATH` (`libwasmjit-backend-<name>.so`). | A | `make install` yields a runtime that finds its backend in the install prefix ✅ (`Test/install-check.sh`, which links no backend and sets `WASMJIT_BACKEND_PATH`). |

Ordering: `BP-1` first (useful on its own; unblocks `AAS2`), `BP-2` before `BP-3`
(a name-based factory makes `dlopen` a drop-in), `BP-4`/`BP-5` after the loader
works.

**Gap status 2026-10-29: none in scope.** `BP-0…BP-5` are `✅`: the C descriptor,
the name-based selector, the loader with every refusal path, the no-unload
policy and packaging. The nine hard problems in §7 are answered as engine
guarantees or explicit embedder responsibilities; none is an open code gap.

## 11. Test survey

| Evidence | Result | Reading |
|---|---|---|
| Default suite (2026-11-13) | **22,881 assertions / 843 cases, green** | The compiled-in path is the baseline every `BP-*` stage must preserve. |
| Backend selection | `WasmBackendRegistry` names `libjit` and `null`; an unknown name is a named error | The selector is general and the second implementation exists. |
| Plugin load test | **Present** — `Test/ExampleBackend/libwasmjit-backend-example.so`, loaded by path and by name (`[plugin]`) | `BP-3`; the plugin lives in its own directory and the frontend was not rebuilt for it. |
| Wrong-ABI / missing-symbol / failing-create tests | **Present** — descriptor ABI, core ABI, no query symbol, missing file, failing `create` (`[plugin]`) | `BP-3`/`BP-4`; every path is an exception with a reason, never an abort. |
| Build-kind refusal (§7.7) | **Present** — the fixture reports a mismatched `build_flags` on request and is refused; the comparison predicate and its text are unit-tested | `build_flags` is real, not aspirational: debug/release and the sanitizer kind must agree. |
| Helper export (§7.5) | **Present** — `[plugin]` resolves `wasm_trap_slot`, `wasm_trap_report`, `wasm_stack_check`, `wasm_v128_*`, `wasm_exn_*` in the process image | `ES-4`'s hidden visibility did not swallow the plugin ABI. |
| Diagnostic reporting (§7.9) | **Present** — a refused load and a successful load are both observed through a sink (`WASM.BACKEND.PLUGIN`, Error and Note) | Failure containment is a report, and the happy path is visible too. |
| POD layout test | **Present** — `[backendabi]` pins the tuple rule and both C views (§4.4) | `BP-1`. |
| Symbol-hygiene check | **Present** — `ES-4` hid the core's internals (23,919 → 2,545 exported symbols at the time; **3,239 dynamic / 2,110 strong** on 2026-11-13) and deliberately re-exported the helper subset | `ES-4`; the plugin ABI is a declared list, not "everything and hope". |
| "One core" check (`NEEDED` inspection) | **Present as a symbol-identity probe** — the loader compares the plugin's `wasm_trap_slot` with the host's and refuses a private copy | `BP-3`; a plugin with its own core is refused rather than running two heaps and two allocators. |

## 12. References

- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) — the `WASM::Runtime` that calls
  `create(name)`, the header tiers, and the visibility/soname policy this ABI
  depends on.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2/§7.3 — the frozen surfaces
  (S6 and the new S10) and `AAS10`.
- [`LAYERING.md`](LAYERING.md) §4 — the contract rows a plugin must honour.
- [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) — the implementer's checklist
  (the two seams, the ABI obligations, the helper split, the definition of done);
  a plugin is one way to ship what it describes.
- [`ABI.md`](ABI.md) "Wide value ABI", "Callable" — the C-shaped pieces the plugin
  already exchanges, and the helper entry points it calls.
- [`SAFETY.md`](SAFETY.md) §2.4 — no exceptions/`abort` on the trap path, which is
  rule 6.
- [`THREADSAFETY.md`](THREADSAFETY.md) §3.1–§3.2 — the cardinality rule and the
  serialized-compilation / re-entrant-execution split behind rule 4.
- [`proposals/THREADS.md`](proposals/THREADS.md) §5 — the per-backend
  threading rules a plugin inherits.
- [`proposals/SIMD.md`](proposals/SIMD.md) §4.10 — what each engine offers, which is
  what a capability flag has to describe.
- Source anchors: `Test/TestBackend.cpp` (`WASMJIT_BACKEND`), `Test/Test.pro`
  (rpath + link lines), `LibJit/LibJit.pro`, `WasmBase/*.hpp` (`extern "C"`
  helper blocks).

