# `LF2-ABI` — Cross-language ABI and host-binding amendment {#lf2_abi}

Status: cross-cutting `LF-2` amendment, revision 3 (2026-11-04). Revision 1
(2026-11-03) added the ABI/interop dimension the cluster reports did not treat
as one subject: how a language **sees WasmJit’s exports and synthetic host
modules**, how the C/C++ runtime and a JavaScript bridge **see the language’s
exports**, and where each language sits on the scalar-to-GC/reference difficulty
scale. Revision 2 added the host-side counterpart: native C++ objects and C
structs in [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §13. Revision 3
points at §14 (GC references across modules and generated headers) and §15
(host-to-guest templates and SWIG/Glue). It does **not** change any `LF-2`
verdict; those remain in the cluster reports.

This is deliberately an amendment to the language-feasibility set, not a new
language charter. The host-binding contract it elaborates is
[`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12 (“Host bindings: guide,
generator, or embedder?”).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What are the two directions, and what is a “synthetic module”? |
| §3 | Which ABI is in play: the core `Callable` ABI or the component Canonical ABI? |
| §4 | How does a guest language see WasmJit exports and synthetic host modules? |
| §5 | How do the C/C++ runtime and a JS bridge see a language’s exports? |
| §6 | The scalar-to-GC/reference difficulty matrix |
| §7 | Per-language notes |
| §8 | What is easy, what needs an adapter, and what is not an engine gap |
| §9 | Consequences for the guide/generator plan |
| §10 | Open questions and tests that would settle them |
| §11 | References |

## 2. The two directions, and the synthetic module

There are two independent paths. A language can be easy in one and hard in the
other.

**Direction A — engine → guest.** The embedder has a `ModuleInstance` or
`ComponentInstanceHandle` and calls an exported function. For a core module this
is `ModuleInstance::exportedFunction()` → `WASM::callCallable`; for a component
it is `ComponentFunction::callValues` / `callAsync`. The language’s own export
signature meets the engine’s outer ABI at this point.

**Direction B — guest → host.** The guest imports a function, memory or table.
For a core module the import is resolved by `(module, field)` through
`ImportResolver`/`RegistryImportResolver`; for a component it is a
`ComponentImport` in `ComponentHostImports`.

A **synthetic module** is the core-module form of that second path: a namespace
consisting only of host-provided entities. It is **not** a parsed `Module` and
has no code section. The resolver maps its name/field pairs to:

- `WASM::Callable` entries (including `nativeImport<&fn>()` trampolines);
- Store-owned `LinearMemory*` and `TableInstance*` views;
- the component equivalents: a `ComponentImport` carrying a host
  `ComponentFunction`, `ComponentInstanceValue`, value, type, component or core
  module.

The guest sees ordinary imports; it cannot tell whether the provider is a real
module, a native C/C++ function, a JS adapter, or another guest module. That
uniformity is the point of the seam, and it is why “synthetic module” needs no
new engine object.

## 3. Which ABI is in play?

Two different call surfaces must not be conflated.

### 3.1 Core module ABI — the `Callable` boundary

Every JIT-compiled Wasm function has an outer ABI beginning with
`VMContext*`:

```
Ret f(VMContext* ctx, <lowered params...>);
```

`Callable::fnPtr` is that outer entry (a backend driver for a compiled
function), and `Callable::context` selects the `VMContext*` for the call kind.
The guest language’s exported source signature does **not** normally contain
`VMContext*`; the backend’s driver bridges from the outer `VMContext*` call to
the guest body. A host import, by contrast, **does** see the caller’s
`VMContext*` as its first argument; `nativeImport<&fn>()` requires that shape.

The rest of the core ABI:

- multi-result is `void f(VMContext*, params..., void* results)`;
- `v128` is a 16-byte aggregate;
- a reference is one pointer-sized word;
- traps are pending status, not a C++ throw across generated frames;
- a function import/reference carries a canonical `TypeId`;
- a cross-module import burns the callee’s `VMContext*` into generated code, so
  the callee instance must outlive its callers.
### 3.2 Component ABI — Canonical lift/lower

A component function is **not** the raw `Callable` shape. The engine’s
`ComponentFunction` exposes flattened core `params`/`results`, structured
`paramTypes`/`resultTypes`, `CanonicalAbiOptions`, and indirect
parameter/result flags. Values cross through `ComponentValue` and the
Canonical ABI:

- strings and lists use a memory pointer/length pair and a `realloc`;
- records/tuples/variants/options/results/flags/enums have defined layouts;
- resource handles are `own`/`borrow` table handles;
- streams/futures/tasks/error-context are component async values;
- a component host import is a `ComponentImport`, not a `Callable`.

A language that only has the core ABI can still be hosted through a component
adapter, but that adapter must implement the Canonical ABI; the component
runtime does not guess a language’s object representation.

## 4. How a language sees WasmJit exports and synthetic modules

For **core modules**, the guest’s compiler decides the source spelling. The
engine’s import/export metadata remains the authority:

| Language family | Guest spelling of an import/export | What the engine sees |
|---|---|---|
| C/C++ | `extern "C"` import; exported `extern "C"`/`__attribute__((export_name))` | a `Callable` and a `TypeId`; scalar/pointer ABI |
| Rust | `extern "C"` / `#[no_mangle]` or `#[export_name]` | same |
| Zig | `extern "c"` / `export fn` | same |
| AssemblyScript | `@external`/import declarations; `export function` | a `Callable`; strings/arrays are linear-memory pointers, not WasmGC values |
| Kotlin/Wasm | `@WasmExport`/`@JsExport`; `wasmWasi` imports | WasmGC functions and GC references; `wasmJs` additionally needs the JS bridge |
| MoonBit | `pub fn`/extern declarations on `wasm-gc` | WasmGC functions and refs |
| Go/TinyGo | `//go:wasmimport` / `//go:wasmexport` | a `Callable`; Go runtime and `_initialize` matter |
| Dart | `dart:*` imports; `$invokeMain` export | a `Callable` only at the runtime entry; JS-glue imports |
| Lua (guest VM) | C API through the shim | ordinary `Callable` entry points; VM state is opaque |

A **synthetic module** is seen by the guest exactly like a normal import module.
For C/C++/Rust/Zig/Go the adapter is usually a small `extern` shim; for
AssemblyScript it is an `@external` declaration; for Kotlin/Dart it may be a JS
runtime binding rather than a plain C function. The provider’s implementation
is invisible to the guest, but the ABI is not.


## 5. How the C/C++ runtime and a JS bridge see a language’s exports

### 5.1 C/C++ host → guest

The C/C++ host calls `Runtime::callExport` or `callCallable` with the
`Callable` returned by `exportedFunction()`. It therefore sees the **outer**
`VMContext*` ABI, not the guest compiler’s source signature. The backend driver
bridges to the guest function.

The practical consequences are:

- A guest export is a plain C-callable entry point, not a C++ class, template,
  exception or `std::string`. Those are source-language conveniences and stay
  inside the guest unless the adapter copies them.
- Scalars are direct (`i32`, `i64`, `f32`, `f64`, `v128` by the documented
  aggregate rule).
- A language string/list is a **protocol** (usually a pointer/length pair in
  guest memory plus an allocator), not a C++ `std::string`.
- A reference is an opaque pointer-sized value. Owning, retaining or releasing
  it is the engine’s `refClone`/`refDrop` contract, not the guest language’s
  native ownership model.
- Multi-results need a caller-owned result buffer; they are not a C++ tuple
  returned by value.
- A guest exception that becomes an engine exception/trap is observed at the
  call boundary, after the generated frames return.

### 5.2 C/C++ synthetic module → guest

A host C/C++ function registered as a core import is a **native import**:

```cpp
void hostLog(WASM::VMContext* ctx, int32_t value);
resolver.registerFunction("env", "log", WASM::nativeImport<&hostLog>());
```

It must use the import’s exact Wasm signature with `VMContext*` first, report a
trap rather than throwing across generated code, and obtain per-instance state
from `ctx->hostData` or `ctx->hostBridge`. It has no C++ object identity visible
to the guest. A host function that wants to receive or return a guest GC object
must treat it as an opaque engine reference and use the reference/host-bridge
contract; it cannot fabricate a C++ object with the same address.

### 5.3 JS bridge

The engine has no JS engine. A JS bridge is an embedder adapter over the same
seams:

- core-module imports become `Callable`/`nativeImport` entries with JS scalar
  conversions and memory copies;
- `externref` uses `HostBridge` retain/release/equals (and the host’s own
  identity rules);
- component values use `ComponentFunction`/`ComponentValue`, with the Canonical
  ABI handling strings, lists, records, resources and async values;
- JS Promises/event loops are the bridge’s scheduling, not the engine’s.

A JS bridge therefore sees **component-level structured values**, not the raw
guest-language object graph. Kotlin `wasmJs` and Dart `dart:*` make this concrete:
their modules import JS helpers and `(ref extern)` globals, so they are not
directly callable from a bare C++ host without that bridge. `wasmWasi` Kotlin,
Zig, C/C++, Rust, AssemblyScript and TinyGo expose a much smaller C/scalar
surface.
## 6. The scalar-to-reference difficulty scale

The scale is about **adapter work**, not about whether a language is “good”.
`A` is direct, `B` needs a memory/layout/allocator adapter, `C` needs a
language-runtime/GC/JS/async bridge, and `?` means the study did not measure it
(no verdict is implied).

| Value crossing the boundary | C/C++ | Rust | Zig | AssemblyScript | Kotlin/Wasm | MoonBit | Go/TinyGo | Dart | Lua VM |
|---|---|---|---|---|---|---|---|---|---|
| `i32` / `i64` / `f32` / `f64` argument or result | A | A | A | A | A (WASI) | A | A | A | A |
| Multi-value result | B | B | B | B | B | B | B | B | B |
| Pointer + length string/list in linear memory | B | B | B | B (UTF-16 AS string) | C | C | B | C | B |
| `v128` by value | B | ? | ? | ? | ? | ? | ? | ? | ? |
| `externref` / `(ref extern)` | B | B | B | C (JS interop) | C | C | ? | C | ? |
| GC-managed object/array/string reference | C | C | C | C (AS-managed object) | C | C | C | C | C |
| `exnref` / guest exception value | C | C | C | C | C | C | C | C | C |
| Resource / stream / future / task | C | C | C | C | C | C | C | C | C |
| Host synthetic module returning a GC ref | C | C | C | C | C | C | C | C | C |
| Host synthetic module returning a scalar | A | A | A | A/B | A/B | A/B | A | A/B | A |

The pattern is stable:

- The `C` verdict in the C/C++ column means “the guest-side adapter is
  runtime-specific”, not “a C host cannot hold the value”. A C **host** can
  carry an opaque `externref`/GC pointer; a C **guest** cannot declare a typed
  `(ref $T)` import without a GC-capable toolchain. See
  [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §14.
- **A** ends at scalars and at pointers the host already owns.
- **B** begins when the value has a **guest-memory representation**: strings,
  lists, records, tuples, result buffers, `v128`, and language-specific object
  layouts. The adapter must know the layout, the allocator/`realloc`, and who
  owns the bytes.
- **C** begins when the value is a **reference with runtime identity**:
  WasmGC objects, `externref`, exceptions, resources, streams/futures/tasks.
  The engine can move an opaque reference, but only the originating runtime (or
  the component Canonical ABI adapter) understands it.

This is the same line as
[`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12: `A`/`B` are the
descriptor-and-template generator’s territory; `C` is where the embedder’s
runtime adapter is unavoidable.

`stringref` is a separate post-3.0 reference type, not a C string or a GC
`struct`; its host spelling is an opaque Store-owned reference and its
conversion rules are in [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
§14.7.

## 7. Per-language notes

- **C/C++ (`S1`)** — A clean scalar/pointer arm; `extern "C"` exports and
  imports, one guest memory, C++ objects only behind opaque pointers or
  validated handles. GC refs and `exnref` are C, not the C++ ABI; C++
  exceptions and `std::string` do not cross. The synthetic-module direction is
  the easiest in the set: a `nativeImport` with `VMContext*` first.
- **Rust (`S2`)** — Scalar and pointer shapes are A/B; `extern "C"` plus
  explicit ownership. Rust’s allocator is guest-side. `wasm-bindgen` is a JS
  adapter, not this engine’s ABI. GC references and exceptions are C; a Rust
  panic is not a Wasm exception unless the language adapter translates it.
- **Zig (`S2`)** — `export fn`/`extern` and a C-shaped ABI, usually A/B. No
  allocator unless the module brings one; GC/exception/component values are C.
- **AssemblyScript (`S3`)** — Scalars are A; strings and arrays are
  linear-memory pointers (B), with UTF-16 string conventions and an AS runtime.
  Managed objects are AS-runtime objects, not WasmGC refs; crossing them to a
  host is C unless the AS runtime supplies an adapter. `--exportRuntime` is a
  language-runtime feature, not a WasmJit flag.
- **Kotlin/Wasm (`S4`)** — `wasmWasi` scalar exports are A/B and the measured
  arm. WasmGC objects/strings are C. `wasmJs` is a separate C case: its hundreds
  of JS/`wasm:js-string` imports and imported `(ref extern)` globals require the
  JS bridge before any export is callable from a bare C++ host.
- **MoonBit (`S4`)** — WasmGC module; scalar host hook works. GC values and
  component resources are C; a binding needs a MoonBit/WasmGC-aware adapter.
- **Go/TinyGo (`S2`/`L8`)** — `//go:wasmimport`/`//go:wasmexport`, scalar/pointer
  A/B. Go's runtime initialization (`_initialize`) and scheduler are
  embedder-visible preconditions. Go heap objects are not WasmGC refs; crossing
  them is C.
- **Dart (`S4`)** — `dart:*` JS-glue imports and a single runtime entry point
  make it a C case for a generic host; a Dart-aware JS/Wasm bridge is required.
- **Lua / guest VMs (`S5`)** — The C API is A/B; VM state is an opaque pointer.
  The VM's scripting values do not cross the Wasm boundary as typed values;
  that is the VM's own C API, not the Canonical ABI.

## 8. What this means for “synthetic modules”

A synthetic module is easy **only when its interface is scalar/pointer A/B**.
That is the useful design target for host-side native C/C++:

- expose scalars, `externref`-style opaque handles, and explicit pointer/length
  pairs;
- keep GC objects and language strings behind the language runtime’s own API;
- do not expect a guest’s `std::string`, Rust `String`, AssemblyScript `string`,
  Kotlin object or Dart string to arrive as a C++ object;
- do not expect a host C++ object to arrive as a guest GC object.

The engine can carry the opaque reference; it cannot make two runtimes agree on
what the object means. That agreement is the binding adapter's job, and is the
main reason a binding **guide/generator** is more realistic than a universal
automatic bridge.

The host-side counterpart is [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
§13: opaque handles (A), typed handle tables with generated stubs (B), and
host-runtime `externref`/finalizer boxes (C), including the reasons a native
C++ object image in linear memory, a `std::pmr`-backed GC, and an engine
reflection registry are non-options. For GC references specifically, §14 covers
what an importing module and its generated C/Rust/Zig/Go header see, the
same-Store `TypeId` rules, mixed GC/non-GC links and `stringref`; §15 covers
host-to-guest templates and the SWIG/Glue boundary.

## 9. Consequences for the guide and generator plan

The matrix turns the host-binding recommendation in
[`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12 into a language-planning
rule:

1. **Start with A.** A generated scalar/pointer declaration adapter is useful for
   every language in the set, including the ones that later need C work. It is
   also the easiest to test against `[backendabi]` and the language PoCs.
2. **Treat B as a per-language string/list/record adapter.** The generator must
   emit the language’s memory protocol, not a C `std::string`. C/C++, Rust, Zig,
   AssemblyScript, Go and the Lua C API are the first B targets.
3. **Do not pretend C is generatable from headers.** A WasmGC object, an
   `externref`, an exception payload or a component resource needs a runtime
   adapter. That is where SWIG/Glue can help a *C++ host*, but only behind a
   language/component-specific layer.
4. **Keep the engine metadata neutral.** `Module::importDescriptors()` /
   `exportDescriptors()`, `ComponentFunction` and `ComponentValueType` are the
   schema; no language name or header parser belongs in `WasmBase/`.
5. **Use the measured cluster reports as the evidence anchor.** Do not turn a
   green scalar arm into a claim about GC, exceptions or resources. `LF2_S1`,
   `LF2_S2`, `LF2_S3`, `LF2_S4` and `LF2_S5` remain the source for what was
   actually built and run.

## 10. Open questions and tests

The following would turn a `?` in §6 into evidence:

- a scalar **host synthetic module** called from each available language arm,
  with the imported function’s signature checked against the descriptor;
- a **string/list round-trip** in C/C++, Rust, Zig, AssemblyScript and Go, with
  the allocator and ownership written down;
- a **WasmGC reference** crossing in Kotlin and MoonBit, including what the host
  may legally do with the opaque reference;
- an **`externref`** crossing through `HostBridge`, including retain/release;
- a **component structured value** and a **resource handle** through the
  `ComponentFunction`/`Canonical ABI` path;
- a **JS bridge** arm for Kotlin `wasmJs` and Dart, showing which imports and
  `(ref extern)` globals are required;
- an **exception path** for each language that can throw across its own runtime,
  distinguishing a guest trap, a pending exception and a host adapter error.

Until those tests exist, the matrix is a design map, not a new set of language
verdicts.

## 11. References

- [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12 — host-binding guide,
  descriptor-driven generation, per-language limits and the embedder fallback;
  §13 — native C++ objects/C structs, opaque handles, typed handle tables,
  `externref`/finalizer boxes, and the engine-vs-embedder boundary; §14 — GC
  struct/array references across modules and generated headers; §15 — host-to-
  guest templates, class-size registration and SWIG/Glue.
- [`ABI.md`](../ABI.md) — `VMContext*`, multi-result buffers, `v128`, references,
  traps, host imports and module metadata descriptors.
- [`WasmBase/WasmValue.hpp`](../../WasmBase/WasmValue.hpp) — `Callable` and
  `callCallable`.
- [`WasmBase/WasmNativeImport.hpp`](../../WasmBase/WasmNativeImport.hpp) — the
  native-import trampoline and the `VMContext*`-first contract.
- [`WasmBase/WasmComponentValue.hpp`](../../WasmBase/WasmComponentValue.hpp) —
  component values, function metadata and host imports.
- [`WasmBase/WasmModule.hpp`](../../WasmBase/WasmModule.hpp) — import/export
  descriptors and structural type views.
- [`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md), [`LF2_S2_LLVM.md`](LF2_S2_LLVM.md),
  [`LF2_S3_AS.md`](LF2_S3_AS.md), [`LF2_S4_WASMGC.md`](LF2_S4_WASMGC.md),
  [`LF2_S5_GUEST_VMS.md`](LF2_S5_GUEST_VMS.md) and
  [`LF2_L6_L12.md`](LF2_L6_L12.md) — the measured language arms.
- [`LF2_INDEX.md`](LF2_INDEX.md) — the verdict table and reading order.
