# Backend Layering — What `WasmBase/` May Know, and What Only a Backend May

Status: policy note (revision 1 — records the rule that `WasmBase/` is
backend-agnostic, the vocabulary for talking about a backend in core code, and
the state of the tree after the first pass).
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

The core is not merely "allowed" to define these — it is the only place they can
live, because every backend and the runtime both depend on them:

| Contract | Stated in | A backend must |
|---|---|---|
| Calling convention: a leading `VMContext*`, the `Callable` layout, who owns a store entity | `WasmValue.hpp`, `WasmVMContext.hpp`, [`ABI.md`](ABI.md) | generate functions with that exact ABI, and treat `Callable::context`/`rawFnPtr` as the core defines them |
| GC object layout: header words, per-type size/alignment, array data offset | `WasmGcHeader.hpp` (`gcStorageBytes` / `gcStorageAlign` / `gcArrayDataOffset`), invariant H1 in [`proposals/GC.md`](proposals/GC.md) | derive its lowered aggregate layout from those helpers, so field offsets cannot drift |
| The POD views generated code may touch: `VMContext`, `LinearMemory`, `TableInstance` | `WasmVMContext.hpp`, `WasmMemory.hpp`, `WasmTable.hpp` (standard-layout + first-field `static_assert`s) | access them only through those layouts |
| The trap entry point | `WasmTrap.hpp` ([`SAFETY.md`](SAFETY.md) §2.4) | call it with the same kind/message convention instead of inventing a failure mode |
| The SIMD helper ABI (`const void*` / `void* out`, immediates as scalars) | `wasm_simd_scalar.hpp` | lower `v128` ops onto those entry points, or provide its own but keep the same semantics |
| Unmodelled value types are rejected at load, so no backend is ever handed one | `WasmType.hpp`, `WasmModule.cpp` | nothing — this is the core protecting it |

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
grep -rn -i 'libjit\|jit_type\|jit_insn\|jit_function\|jit_value\|jit_context\|\bJIT\b' WasmBase/
```

Expected result: **only the five `docs/NULL_JIT_VALUE_DIAGNOSIS.md` citations**
covered by the §8 waiver — no backend name, no backend type, no "the JIT" used as
a synonym for a backend. The same check on `WasmStub/` must be completely empty:
it is a non-compiling backend and must not acquire a code generator's vocabulary
by accident.

## 7. State of the tree

First pass, 2026-10-16 (the change that added this note):

| Before | After |
|---|---|
| 10 explicit `LibJIT` mentions (4 in the new SIMD helper layer, 6 pre-existing) | 0 |
| 3 of those named backend *types* (`jit_function_t`, `jit_type_void`, `jit_value_t`) in comments | 0 |
| ~38 comments using "JIT" where they meant "the one backend that exists" | 0 — reworded to the §5 vocabulary |
| No rule anywhere, so nothing to check against | this note, + a rule line in [`AGENTS.md`](../AGENTS.md), + a [`README.md`](../README.md) pointer |

No code changed behaviour: every edit was a comment (verified — no `WasmBase/`
file includes a backend header or names a backend type in code, then or now),
and the full suite was green before and after.

## 8. Deliberate tensions and waivers

- **`docs/NULL_JIT_VALUE_DIAGNOSIS.md` is cited from five core sites and its
  name contains "JIT".** The note itself is legitimately backend-specific — it
  diagnoses a null backend value handle — and it is the recorded rationale for a
  *core* rule ("reject unmodelled value types at load"), so the core needs to be
  able to point at it. A citation is not a dependency, so this is left as-is and
  flagged here: renaming the note to something backend-neutral (e.g.
  `UNMODELED_VALUE_TYPES.md`) and re-pointing the five sites plus `README.md`
  would close it, if the naming inconsistency is judged to matter more than the
  churn. This is the *only* remaining hit of the §6 check.
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
- [`../AGENTS.md`](../AGENTS.md) — the rule in one line, for anyone (or any
  agent) starting work here.

