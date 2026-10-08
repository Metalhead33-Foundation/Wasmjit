# Import/Export of Mutable Globals — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
Audience: anyone touching `WasmModuleInstance` (globals), `WasmImport`,
`WasmRegistryImportResolver`, `WasmModule`, or the test harness.

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

Import/Export of Mutable Globals is a **Phase 5** proposal, integrated into
WebAssembly **2.0**.

- A **mutable** global may be imported and exported (`GlobalType` is
  `mut? valtype`).
- **Import matching** requires the provided global's type and **mutability** to
  match the declaration exactly (an immutable global cannot satisfy a mutable
  import, and vice versa).
- An imported global **aliases** the exporting instance's global: `global.set`
  through the importer is observed by the exporter and by every other importer
  of the same `(module, field)`.
- The JS API exposes `WebAssembly.Global.value` for the same aliasing semantics.

This is the mechanism the threads proposal relies on for sharing a flag/lock
without a linear memory (see [`THREADS.md`](THREADS.md) §2.6).

## 3. What the proposal does not demand

- **No representation requirement.** A global may live in a flat array, a
  struct field, or host memory.
- **No requirement on how aliasing is achieved**, only that it is observable.
- **No requirement to expose globals via a JS API** for a non-web embedder.
- **No requirement to resolve imports lazily**; either binding at instantiation
  or through an indirection is fine.
- **No new trap semantics.**

## 4. What we have implemented today

- `GlobalType { ValueType contentType; bool isMutable; }` and `Global` exist
  and are parsed; exports of kind `Global` are registered.
- `ModuleInstanceInternals::globalsStorage` is a flat `std::vector<Value>`,
  indexed by the Wasm global index space (imports first, then locals), and
  `ctx.globals` points at it.
- `ImportResolver::resolveGlobal(module, field, const GlobalType&)` is declared,
  and `RegistryImportResolver` implements it (including a per-module map of
  exported globals).
- **Import resolution is not wired up.** `resolveGlobal` is *declared and
  defined but never called anywhere* in the instantiator; `initializeGlobals`
  only evaluates locally-defined globals and leaves every imported slot at its
  zero-initialised value.
- **Exports are registered by value.** `registerExports` calls
  `registrar.registerGlobal(moduleName, ex.name, ctx.globals[ex.index])`, which
  copies the `Value`. The resolver's storage is therefore a snapshot, not a
  window into the exporting instance.
- **Import matching ignores the type.** `ModuleRegistry::resolveGlobal` does
  `(void)type;` — it never checks the value type or mutability.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| MG1 | **Imported globals are never resolved.** `resolveGlobal` has no caller; `initializeGlobals` leaves the import slots zero. | `global.wast` 10 passed / **63 failed**; `docs/GC.md` G6 | High |
| MG2 | **The interface cannot alias.** `resolveGlobal` returns `std::optional<Value>` **by value**, and the resolver stores `Value` by value, so even wired up it would copy. Sharing semantics require a `Value*`/global-reference. | `WasmImport.hpp`, `WasmRegistryImportResolver.hpp` | High |
| MG3 | **Export is a snapshot.** `registerExports` copies `ctx.globals[i]`, so a later `global.set` by the exporter is invisible to importers. | `WasmModuleInstance.cpp` | High |
| MG4 | **No import type/mutability check.** `resolveGlobal` ignores `GlobalType`. | `WasmRegistryImportResolver.cpp` | Medium |
| MG5 | **No test coverage of the shared case.** There is no manual test where module B imports A's mutable global and both observe a write. | `Test/main.cpp` | Medium |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MGD1 | **Change `globals` to an array of `Value*`** (like `memories`/`tables`), so an imported global simply points into the exporter's `globalsStorage`. | §3: representation is free; aliasing becomes natural. | Closes MG1/MG2/MG3 with one structural change. |
| MGD2 | **Where a global is immutable, copying is fine**; only mutable globals need aliasing. | §2: immutable values cannot change, so a copy is observationally identical. | Avoids indirection for constant globals. |
| MGD3 | **Bind the pointer at instantiation** (eager), not lazily. | §3: lazy resolution is optional. | Simple, and matches the existing import-resolution step. |
| MGD4 | **Enforce the mutability/type check only in the resolver** (a `memcmp`-style `GlobalType` compare), or skip it as part of the no-validation stance — but then document it. | §2 requires the check for linking; §3/`GC.md` D6 let us require valid input. | Either is defensible; the important part is consistency with the chosen validation stance. |
| MGD5 | **Do not implement `WebAssembly.Global`.** | §3: JS API is web-only. | Scope control. |

Guardrail: an imported global must not dangle — the exporting `ModuleInstance`
must outlive the importer, exactly like the cross-module `Callable::context`
rule in [`ABI.md`](ABI.md).

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MG-1 — Pointer globals** | Represent the global index space as `Value*` (or a small `Global*` with an `isMutable` flag); keep local globals in the instance's storage. | MG2 | `ctx.globals` addresses can point into another instance. |
| **MG-2 — Wire resolution** | Call `resolveGlobal` for each imported global during instantiation and install the returned pointer. | MG1, MG4 | A module importing a global sees the exported value; the type is checked. |
| **MG-3 — Export aliasing** | Export a global by publishing a pointer to the instance's slot, not a `Value` copy. | MG3 | A write through the importer is visible to the exporter. |
| **MG-4 — Test** | Add `shared_global_a/b.wat` + a manual test that writes through B and reads through A. | MG5 | The test asserts the shared value. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `global` | 10 | 63 | 51 | MG1: imported globals and mutations mostly broken |
| `exports0` | (curated) | | | plain exports work |

`global.wast` is not in `Test/wast_supported.txt`; `exports0` is.

## 9. References

- Proposal overview (`WebAssembly/mutable-global`):
  https://github.com/WebAssembly/mutable-global
- Spec — Change History (Release 2.0, mutable global import/export):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Modules (import matching):
  https://webassembly.github.io/spec/core/valid/modules.html
- In-repo: [`docs/GC.md`](GC.md) G6, [`docs/EXTENDED_CONST.md`](EXTENDED_CONST.md) EC2,
  [`docs/THREADS.md`](THREADS.md) §2.6, [`docs/ABI.md`](ABI.md);
  sources `WasmBase/WasmType.hpp` (`GlobalType`, `Global`),
  `WasmBase/WasmModuleInstance.{hpp,cpp}` (`globalsStorage`,
  `initializeGlobals`, `registerExports`), `WasmBase/WasmImport.hpp`,
  `WasmBase/WasmRegistryImportResolver.{hpp,cpp}`.
