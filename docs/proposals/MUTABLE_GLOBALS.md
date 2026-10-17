# Import/Export of Mutable Globals — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 3 — MG-1…MG-5
implemented: imported globals resolve, mutable imports alias the exporter, and
the link-time type/mutability match is enforced).
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
- **Import resolution is wired up (MG-1).** `resolveImports` calls
  `ImportResolver::resolveGlobal` for every imported global and writes the
  result into `globalsStorage` **before** `initializeGlobals` evaluates any
  local initialiser, so a `global.get` of an imported global sees the exported
  value. `initializeGlobals` then resizes the same vector (preserving the
  imported prefix) and fills the locals.
- **Exports are registered by value.** `registerExports` calls
  `registrar.registerGlobal(moduleName, ex.name, ctx.globals[ex.index])`, which
  copies the `Value`. The resolver's storage is therefore a snapshot, not a
  window into the exporting instance.
- **Import matching ignores the type.** `ModuleRegistry::resolveGlobal` does
  `(void)type;` — it never checks the value type or mutability.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~MG1~~ | ~~Imported globals are never resolved.~~ **Done (MG-1):** `resolveImports` calls `ImportResolver::resolveGlobal` for each imported global and installs the result into `globalsStorage` *before* any local initialiser runs. `spectest`'s `global_i32`…`global_f64` are registered so scripts can import them. `global.wast` went from 10/63 to **70 passed / 0 failed**. | `WasmModuleInstance.cpp` (`resolveImports`, `initializeGlobals`) | Closed |
| ~~MG2~~ | ~~**The interface cannot alias.**~~ **Done (R1):** `resolveGlobal` returns a borrowed `Value*`; the instance stores it in `Internals::globalRefs`, so a mutable import aliases the exporter's cell. | `WasmImport.hpp`, `WasmRegistryImportResolver.hpp` | Closed |
| ~~MG3~~ | ~~**Export is a snapshot.**~~ **Done (R1):** `registerExports` publishes the exporter's cell (`registerGlobal(..., Value*, GlobalType)`), so a later `global.set` is visible to importers and vice versa. | `WasmModuleInstance.cpp` | Closed |
| ~~MG4~~ | ~~**No import type/mutability check.**~~ **Done:** `ModuleRegistry::resolveGlobal` compares the published `GlobalType` (opcode + mutability) before returning the cell; reference types are compared permissively. | `WasmRegistryImportResolver.cpp` | Closed |
| ~~MG5~~ | ~~**No test coverage of the shared mutable case.**~~ **Done:** `Test/wasm_wat/shared_global_a/b.wat` write through B and read through A (and back); `shared_global_bad.wat` verifies the mutability mismatch is rejected. | `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MGD1 | **Change `globals` to an array of `Value*`** (like `memories`/`tables`), so an imported global simply points into the exporter's `globalsStorage` — **implemented** as `Value**` + `Internals::globalRefs`. | §3: representation is free; aliasing becomes natural. | Closes MG1/MG2/MG3 with one structural change. |
| MGD2 | **Where a global is immutable, copying is fine**; only mutable globals need aliasing. | §2: immutable values cannot change, so a copy is observationally identical. | Avoids indirection for constant globals. |
| MGD3 | **Bind the pointer at instantiation** (eager), not lazily. | §3: lazy resolution is optional. | Simple, and matches the existing import-resolution step. |
| MGD4 | **Enforce the mutability/type check only in the resolver** (a `memcmp`-style `GlobalType` compare), or skip it as part of the no-validation stance — but then document it. | §2 requires the check for linking; §3/`GC.md` D6 let us require valid input. | Either is defensible; the important part is consistency with the chosen validation stance. |
| MGD5 | **Do not implement `WebAssembly.Global`.** | §3: JS API is web-only. | Scope control. |

Guardrail: an imported global must not dangle — the exporting `ModuleInstance`
must outlive the importer, exactly like the cross-module `Callable::context`
rule in [`ABI.md`](../ABI.md).

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MG-1 — Pointer globals** | ✅ Done (R1): `VMContext::globals` is `Value**`, `Internals::globalRefs` is the index space (imported cells are borrowed, defined cells point into `globalsStorage`), and `globalSlotAddress` loads the cell. | MG2 | `global` **77/0**; shared writes alias. |
| **MG-2 — Wire resolution** | ✅ Done: `resolveImports` resolves every imported global to a cell before local initialisers run. | MG1, MG2 | Done. |
| **MG-3 — Export aliasing** | ✅ Done: `registerExports` publishes the cell and its `GlobalType`. | MG3 | A write through the importer is visible to the exporter. |
| **MG-4 — Test** | ✅ Done: `shared_global_a/b.wat` plus the two tests (shared write and mutability mismatch). | MG5 | The test asserts the shared value and the rejection. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `global` | 77 | 0 | 47 | R1/MG-2/MG-3: imported mutable globals alias the exporter. The 47 skips are 40 `assert_invalid` + 7 `assert_malformed`; strict run: 109 / 12 / 3 |
| `exports0` | (curated) | | | plain exports work |

`global` and `exports0` are curated.

## 9. References

- Proposal overview (`WebAssembly/mutable-global`):
  https://github.com/WebAssembly/mutable-global
- Spec — Change History (Release 2.0, mutable global import/export):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Modules (import matching):
  https://webassembly.github.io/spec/core/valid/modules.html
- In-repo: [`docs/GC.md`](GC.md) G6, [`docs/EXTENDED_CONST.md`](EXTENDED_CONST.md) EC2,
  [`docs/THREADS.md`](THREADS.md) §2.6, [`docs/ABI.md`](../ABI.md);
  sources `WasmBase/WasmType.hpp` (`GlobalType`, `Global`),
  `WasmBase/WasmModuleInstance.{hpp,cpp}` (`globalsStorage`,
  `initializeGlobals`, `registerExports`), `WasmBase/WasmImport.hpp`,
  `WasmBase/WasmRegistryImportResolver.{hpp,cpp}`.
