# Backend Layering — What `WasmBase/` May Know, and What Only a Backend May {#doc_layering}

Status: policy note (revision 2 — 2026-10-29: the last non-empty citation was
renamed away and the check became an automated test; revision 1 recorded the rule
that `WasmBase/` is backend-agnostic, the vocabulary for talking about a backend
in core code, and the state of the tree after the first pass).
Audience: anyone editing `WasmBase/`, adding or filling in a backend, or writing
a comment anywhere on the loader / instantiator / codegen path.

This note exists because the rule was real but unwritten, and the core had
already drifted: **48 comment sites in `WasmBase/`** spoke in one backend's terms
(10 naming `LibJIT` outright, 38 using "JIT" as its synonym), including three
that named backend *types* in the rationale for a *core* rule. None of them broke
the build, which is exactly why they survived. It contains **no code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What is the rule? |
| §3 | Why does it exist? |
| §4 | What does the core own that a backend must agree with? |
| §5 | How do I refer to a backend from core code? |
| §6 | How do I check compliance? |
| §7 | What is the state of the tree? |
| §8 | Where is the tension, and what is deliberately left alone? |
| §9 | References |

## 2. The rule

**`WasmBase/` must not name, include, link, or describe any specific backend.**

That means, in core code and comments:

- no backend *names* — `LibJIT`, `libjit`, `LibJit*`, `AsmJit`, `SLJIT`;
- no backend *headers* — no `#include <jit/jit.h>`, and nothing that pulls one in
  transitively;
- no backend *types or API* — `jit_type_t`, `jit_function_t`, `jit_value_t`,
  `jit_context_t`, `jit_insn_*`, `jit_type_create_*`, …;
- no backend *build coupling* — `-ljit` belongs in `LibJit/LibJit.pro` and
  nowhere in `WasmBase.pro`;
- and, easily forgotten, **no backend *internals in prose***. A comment that says
  "the JIT forces the alignment with `jit_type_set_size_and_alignment`" is a leak
  even though it compiles perfectly and no test can see it.

What the core **may** (and must) do is state the *contract a backend has to
satisfy*: the calling convention, the shared layouts, the PODs generated code
touches. Those are core facts; the backend is the party that has to honour them.

Backends (`LibJit/`, and the `Asmjit/`/`Sljit/` scaffolds), the non-compiling
`WasmStub/` pretty-printer, and the harness (`Test/`) are all free to name
whatever they like.

## 3. Why the rule exists

1. **The backend set is deliberately plural and isn't finished.** `LibJit/` is
   the implementation path today; `Asmjit/` and `Sljit/` exist as scaffolds
   precisely so that a second and third code generator can be filled in later
   (see [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) and, for the SIMD angle,
   [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §3). A core that names LibJIT is a core
   that has to be edited in many places — with no test able to catch a miss —
   the day another backend executes anything.
2. **The core is where the *contract* lives, and a contract that names its only
   current implementation is not a contract.** "The runtime and `LibJitTypeTranslator`
   must agree" is a statement about one class; "the runtime and each backend's
   lowered layout must agree, and both derive from `WasmGcHeader.hpp`" is a
   statement the next backend can actually act on.
3. **It is what makes the shared layers testable.** The SIMD helper layer
   (`wasm_simd_scalar.{hpp,cpp}`) is backend-neutral by design — every backend
   *can* call the same `extern "C"` entry points, and one that lowers `v128` on
   its own vector instructions must still match their semantics (§4) — so its
   reference implementation *has* to live in the core. If the core were allowed
   to be LibJIT-shaped, that layer would have nowhere to live.

## 4. What the core owns, and what a backend must agree with

What a backend **must provide** (the four hooks, the ABI it emits, the helpers it calls, the packaging) is enumerated in
[`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md): this section is the contract it
must not violate, that note is the work order it implements.

The core is not merely "allowed" to define these — it is the only place they can
live, because every backend and the runtime both depend on them:

| Contract | Stated in | A backend must |
|---|---|---|
| Calling convention: a leading `VMContext*`, the `Callable` layout, who owns a store entity | `WasmValue.hpp`, `WasmVMContext.hpp`, [`ABI.md`](ABI.md) | generate functions with that exact ABI, and treat `Callable::context`/`rawFnPtr` as the core defines them |
| GC object layout: header words, per-type size/alignment, array data offset | `WasmGcHeader.hpp` (`gcStorageBytes` / `gcStorageAlign` / `gcArrayDataOffset`), invariant H1 in [`proposals/GC.md`](proposals/GC.md) | derive its lowered aggregate layout from those helpers, so field offsets cannot drift |
| The POD views generated code may touch: `VMContext`, `LinearMemory`, `TableInstance` | `WasmVMContext.hpp`, `WasmMemory.hpp`, `WasmTable.hpp` (standard-layout + first-field `static_assert`s) | access them only through those layouts |
| The trap entry point | `WasmTrap.hpp` ([`SAFETY.md`](SAFETY.md) §2.4) | call it with the same kind/message convention instead of inventing a failure mode |
| The continuation provider contract: `wasm_cont_new` / `resume` / `suspend`, status codes, one-shot and trap/exception transfer | `WasmContinuation.hpp` ([`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) §10.6) | emit the calls and branch on the documented status; do not implement a provider in the backend or change the status layout |
| Core-module instantiation: component execution instantiates embedded modules only through `ModuleInstantiator` | `WasmModuleInstance.hpp`, `WasmComponentRuntime.hpp` | implement the `ModuleInstantiator` hooks; `ComponentRuntime` must not be made to depend on one code generator |
| The SIMD helper ABI (`const void*` / `void* out`, immediates as scalars) | `wasm_simd_scalar.hpp` | lower `v128` ops onto those entry points, or provide its own but keep the same semantics |
| Unmodelled value types are rejected at load, so no backend is ever handed one | `WasmType.hpp`, `WasmModule.cpp` | nothing — this is the core protecting it |

**No interpreter fallback.** The core must not grow a Wasm interpreter. In this
note a backend is a code generator — or the non-compiling pretty-printer,
`WasmStub/` — never an interpretation path hidden in the core. If
interpretation is ever required, it is the backend's own configuration (for the
current backend, GNU LibJIT already provides an interpreter alongside its
compiler), not a new WasmJit interpreter. This is a project decision, not a
layering preference:
[`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) §10.15 (D-3).

## 5. Vocabulary: how to refer to a backend from core code

Name the **obligation**, not the implementer.

| Instead of (backend-specific) | Write (backend-neutral) |
|---|---|
| the JIT / LibJIT | the backend; the code generator |
| JIT-generated code, JIT-compiled function | generated code; a compiled function |
| JIT-visible view | backend-visible view |
| …for JIT ABI compatibility | …for backend ABI compatibility |
| `jit_type_void`, `jit_value_t` | a void/absent value; a null backend handle |
| `LibJitTypeTranslator::translateStruct` | the backend's lowered struct layout |
| the JIT forces 16-byte alignment with `jit_type_set_size_and_alignment` | a backend must lower the type with 16-byte size *and* alignment |
| `jit_function_t` handles | backend function handles |

## 6. How to check

```bash
grep -rn -iE '\b(libjit|asmjit|sljit|jit_type|jit_insn|jit_function|jit_value|jit_context)\b|jit/jit\.h|\bJIT\b' WasmBase/ WasmStub/
```

Expected result: **empty** — no backend name, no backend type, no "the JIT" used
as a synonym for a backend. The word boundaries matter: without them `asmjit`
matches the project's own `WASMJIT_*` macros, because the comparison is
case-insensitive.

The check is enforced by the `[layering]` test (`Test/LayeringTests.cpp`), which
scans `WasmBase/` for the same vocabulary on every suite run, so drift fails the
default run instead of waiting for a reviewer's grep.

That grep cannot catch the one rule that is not about names: the core must not
implement an interpreter (§4). A decode-and-execute loop in `WasmBase/` is a
review rejection whatever it is called.

## 7. State of the tree

First pass, 2026-10-16 (the change that added this note):

| Before | After |
|---|---|
| 10 explicit `LibJIT` mentions (4 in the new SIMD helper layer, 6 pre-existing) | 0 |
| 3 of those named backend *types* (`jit_function_t`, `jit_type_void`, `jit_value_t`) in comments | 0 |
| ~38 comments using "JIT" where they meant "the one backend that exists" | 0 — reworded to the §5 vocabulary |
| No rule anywhere, so nothing to check against | this note, + a rule line in `AGENTS.md`, + a [`README.md`](../README.md) pointer |

No code changed behaviour: every edit was a comment (verified — no `WasmBase/`
file includes a backend header or names a backend type in code, then or now),
and the full suite was green before and after.

Layering follow-up, 2026-10-22: the executable component runtime moved from
`LibJit/ComponentRuntime.{hpp,cpp}` to
`WasmBase/WasmComponentRuntime.{hpp,cpp}`. It now takes a
`WASM::ModuleInstantiator&` instead of a `jit_context_t`, and `libffi` moved
from `LibJit.pro` to `WasmBase.pro`; component execution is therefore available
to every backend that implements the instantiator hooks. The runtime still owns
the component graph, canonical ABI and async state; only core-module
instantiation crosses the backend seam.

Layering follow-up, 2026-10-29: the last non-empty result of the §6 check was the
five citations of a backend-named diagnosis note. The note is now
`docs/UNMODELLED_VALUE_TYPES.md`, the five core sites were re-pointed, and the
check is **empty**; `Test/LayeringTests.cpp` (`[layering]`) enforces it on every
suite run. The same pass removed the stray `libSljit.so*` files that had been
sitting in `Sljit/`, outside `build/`.

## 8. Deliberate tensions and waivers

- **`docs/UNMODELLED_VALUE_TYPES.md` was the last non-empty result — resolved.**
  Its former name contained "JIT", so five core sites cited a backend-named file
  and the §6 check was non-empty by exactly those five lines. The note was
  renamed and the sites re-pointed (2026-10-29); a citation is not a dependency,
  and the note itself stays legitimately backend-specific (it diagnoses a null
  backend value handle and is the recorded rationale for the *core* rule "reject
  unmodelled value types at load"). The §6 check is now empty, and the
  `[layering]` test keeps it that way.
- **"JIT" as the project's own domain term** (this is a JIT library; `WasmStub/`
  is its non-compiling sibling) is fine in prose that is describing the *project*.
  It is not fine in core code, because in this tree "the JIT" has historically
  been a synonym for "the LibJIT backend", and that is the habit §5 is for.

## 9. References

- [`ABI.md`](ABI.md) — the calling convention and the embedder-facing contracts.
- [`SAFETY.md`](SAFETY.md) — the trap/abort policy every backend inherits.
- [`proposals/GC.md`](proposals/GC.md) — invariant H1 (runtime layout ≡ backend
  layout), which is the clearest example of a core-owned contract.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md), [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) —
  what each backend can do for SIMD, and why the shared helper layer lives in
  the core.
- `AGENTS.md` — the rule in one line, for anyone (or any
  agent) starting work here.

