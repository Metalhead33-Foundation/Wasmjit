# The C ABI Contract: C Linkage, Portability and the GC/Object ABI {#doc_abi_portability}

**Status:** decision, adopted 2026-11-06. This document states a contract, not a
snapshot; measured per-backend states carry a dated stamp.

This is the answer to three questions that were previously implicit in code and
nowhere in prose:

1. **What calling convention do our compiled functions use?** The host C ABI, with
   C linkage, always.
2. **What does "portable" mean here?** The same canonical signature is callable from
   C++ on every supported target, on x86-64 and AArch64, on Linux and Windows alike.
3. **What is the canonical ABI for GC heap objects?** A layout and an allocation
   entry-point set owned by `WasmBase/`, which no backend may reinterpret.

The per-value-kind signature detail and the `VMContext*` rules that were already
documented live in [`ABI.md`](ABI.md); this document adds the convention, the portability
rule, and the target survey that `ABI.md` never covered. The two are read together:
`ABI.md` says *what* the signature is, this document says *which* ABI realises it and
what a backend may and may not do while lowering it.

## 1. The decision

| # | Decision |
|---|---|
| **D-ABI-1** | Every compiled Wasm function has a **C signature with C linkage**: a C++ embedder declares it in a header and calls it with the ordinary call operator. Nothing about the call requires a Wasm-aware caller. |
| **D-ABI-2** | The convention is **the host C ABI of the build target** — System V AMD64, Microsoft x64, AAPCS64, the RISC-V psABI, a MIPS ABI — never an ABI invented by a backend and never one a C compiler on that target could not produce. Callers and callees are both ordinary C functions as far as the machine is concerned. |
| **D-ABI-3** | **Portability outranks performance.** Where the two conflict, the portable lowering wins. A faster lowering that only holds on one ABI is either refused or accepted as an explicitly opt-in fast path with the portable one retained as its definition (the same rule [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) already applies to helper-versus-native SIMD). |
| **D-ABI-4** | **Backends may diverge in implementation** — that is the whole point of having several. What they may not diverge in is (a) the **canonical signature** for a given Wasm function type and (b) the ABI being **non-degenerate** (§4). How a backend gets there (register allocation, spills, helper calls, instruction selection) is entirely its own business. |
| **D-ABI-5** | **No x86-64-only assumptions in the contract.** Targets are named and surveyed (§5). A combination that no test can exercise on the development machine is carried as **provisionally portable (theory)** rather than dropped, and that label is stated wherever the claim appears. |
| **D-ABI-6** | The ABI is expressed **per value kind, in terms C can express** (§3). A value kind that C cannot express in a signature must be lowered to something C *can* express, or be refused for C-callable exports — never passed in a privately agreed convention. |
| **D-ABI-7** | The **GC object layout and the allocation entry points are part of the ABI** and are owned by `WasmBase/` (§8). A backend may choose how to emit an element access; it may not choose a different layout. |

## 2. What "portability" means here

> If a Wasm function is compiled to native machine code, it is **equally callable from
> the native C++ side** on Linux and Windows alike, on x86-64 and AArch64 alike: the
> same declaration, derived only from the Wasm function type, works on every target.

That is the definition this project holds itself to. It has four consequences that are
easy to state and easy to violate:

- **Same signature, same declaration.** The embedder's header does not change per
  backend and does not change per target. `int32_t f(WASM::VMContext*, int32_t, int64_t)`
  means the same thing everywhere.
- **Same memory meaning.** Wasm linear memory and the GC heap are **little-endian**,
  whatever the host's native byte order (§6).
- **Same object layout.** A `struct`'s field *n* is at the same byte offset everywhere,
  because the layout is computed by `WasmBase/` from the type, not by the backend
  (§8). Layout is not an implementation detail a backend may optimise.
- **Same traps.** A trap is a status, not a `longjmp`, and its kind is part of the ABI
  ([`SAFETY.md`](SAFETY.md) §2.6). Two backends trapping on the same Wasm operation
  report the same kind.

What portability does **not** mean, stated to avoid an impossible reading:

- It does not mean **identical machine code** or identical register use. That is
  explicitly permitted to differ (D-ABI-4).
- It does not mean **identical performance**. A backend may be much faster on one
  target; that is expected and is not a portability failure.
- It does not mean every target is **tested**. It means every target is **specified**,
  and the untested ones are labelled as such (D-ABI-5).


## 3. The canonical C signature

One row per Wasm value kind. This table is the whole of the signature contract; the
`arg0` rule and the multi-result buffer come from [`ABI.md`](ABI.md) and are repeated
here only so this document is readable on its own.

| Wasm kind | C type in the signature | Notes |
|---|---|---|
| (arg 0) | `WASM::VMContext*` | Always present, always first, on every function. |
| `i32` | `int32_t` | Also the payload type of `i31ref` before tagging (see §10). |
| `i64` | `int64_t` | 64 bits even where the host word is 32 bits; on ILP32 this occupies two slots. |
| `f32` | `float` | Passed in the ABI's floating-point class, never reinterpreted as an integer. |
| `f64` | `double` | Same. |
| `funcref`, `externref`, `anyref`, `eqref`, `structref`, `arrayref`, … | `void*` | An opaque, GC-managed reference. Never dereferenced by the embedder; ownership in §8.5. |
| `v128` | **two `uint64_t` parameters**, low word first — *at the native boundary only* | **Decided 2026-11-06.** Two scalar `uint64_t` are register-passed on *every* supported ABI (SysV `rdi`/`rsi`, MS x64 `rcx`/`rdx`, AAPCS64 `x0`/`x1`, RISC-V `a0`/`a1`), where a 16-byte *aggregate* is passed **by reference** on MS x64 (§5.4). A `v128` result uses the multi-value buffer as `lo, hi`. See [`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md) §28. |
| no results | `void` return | |
| one result | that kind's C type | Returned in the ABI's return register. |
| two or more results | trailing `void* results` | No aggregate is ever returned by value; [`ABI.md`](ABI.md) "Multiple Results" is normative. |

Two rules fall out of the table and are worth naming:

- **A single return value is never a struct.** Multi-value already uses the buffer form,
  so the C return type is always a scalar or `void`. This keeps the return path identical
  on ABIs that differ sharply on aggregate returns (§5).
- **`v128` is settled as two scalar words**, not as an aggregate: the two-word spelling is the
  only one register-passed on every supported ABI, and it survives the MS x64 rule that passes
  16-byte aggregates by reference.

**Where this rule applies — and where it does not.** The two-word spelling is the **native C ABI**
of a JIT-compiled function: the boundary an embedder crosses when it calls compiled code, and the
boundary every helper and the libffi cache use. It is **not** the wasm ABI. Inside the Wasm type
system a `v128` is one canonical 128-bit value, and any code compiled **to wasm** — a guest in Rust,
C, AssemblyScript, Zig, Kotlin, MoonBit or WIT — must use that canonical type. **The conversion
between the two happens at instantiation, inside this runtime**, never in a binding or a header.

That distinction is the rule for generated bindings: a **WasmJIT-facing** emitter (`cpp-typedefs`,
`cpp-source` — code the embedder copies into its own process) speaks this document; a
**guest-facing** emitter (C header, Rust, AssemblyScript, Zig, Kotlin, MoonBit, WIT, dynamic) speaks
the wasm ABI. The same language can appear on both sides, which is why the rule is per *consumer*
and not per language.

## 4. Non-degeneracy: what a backend may not do

D-ABI-4 permits implementation freedom *provided* the ABI stays non-degenerate. A
degenerate ABI is one that only its own emitter can call. These are the liberties a
backend may **not** take, in the order they are likely to tempt one:

| Forbidden | Why |
|---|---|
| **Hidden parameters** not in the canonical signature (an extra context, scratch area, thread pointer) | The embedder's declaration would be wrong and the call would corrupt the frame. Everything a function needs beyond its Wasm parameters arrives through `VMContext*`. |
| **Results in registers the C ABI does not use for returns** | The caller's compiler will not look there. This is why multi-value uses a buffer rather than a register pair. |
| **Aggregate returns by value** for multi-value functions | ABIs disagree violently about them; the buffer form removes the disagreement. |
| **A stack alignment other than the target ABI's** | Helper calls and embedder callbacks inherit it; misalignment faults on SSE/NEON and is undefined elsewhere. |
| **Reliance on the SysV red zone on a target that has none** | MS x64 has **no red zone**. Code written assuming 128 bytes below `%rsp` are safe will corrupt its caller's frame on Windows. |
| **Clobbering callee-saved registers the ABI does not save** | A callee that clobbers them breaks its C caller. If a backend uses them, it saves them in the prolog, per the target's callee-saved set. |
| **A private convention leaking into an export or an import** | Internal calls between two compiled functions may use anything both sides agree on; an **export** must be callable per §3 and an **import** must be callable as the host declared it. |
| **Traps by `longjmp`/`setjmp` across the C boundary** | A trap is a returned status ([`SAFETY.md`](SAFETY.md) §2.6); unwinding through a C caller is not an option. |

The permitted freedoms are correspondingly broad, and they are what D-ABI-4 protects:
register allocation, spill policy, instruction selection, whether an element access is
inline or a helper call, whether a call is direct or through a resolved pointer, and how
a loop is emitted — all of that is the backend's choice.

## 5. The target survey

### 5.1 Only four things actually differ

Across every ABI in scope the differences collapse into four questions, which is why
a survey this small is sufficient:

1. **Which registers carry integer/pointer arguments, and how many.**
2. **Which registers carry floating-point arguments**, and whether that is a separate
   register file.
3. **How aggregates and vectors are classified** — in registers or by reference — because
   this is where `v128` lives or dies (§5.4).
4. **Frame details**: shadow space, red zone, stack alignment at the call, and which
   registers are callee-saved.

### 5.2 The table

Rows marked *provisional* are specified from the normative document but **not exercised
by any test on this machine** (x86-64 Linux); they are carried under D-ABI-5.

| ABI | Targets | Integer args | FP args | Stack align at call | ≤16-byte aggregate | Frame extras |
|---|---|---|---|---|---|---|
| **System V AMD64** | x86-64 Linux, *BSD, macOS | `rdi rsi rdx rcx r8 r9` | `xmm0`–`xmm7` | 16 | Classified INTEGER/SSE; register-passed | 128-byte **red zone**, no shadow space |
| **Microsoft x64** | x86-64 Windows | `rcx rdx r8 r9` | `xmm0`–`xmm3` | 16 | **By reference** unless 1/2/4/8-byte scalar | 32-byte **shadow space**, **no red zone** |
| **AAPCS64** *(provisional)* | AArch64 Linux, *BSD, macOS | `x0`–`x7` | `v0`–`v7` | 16 | In `x` registers unless HFA/HVA (then `v`) | no red zone |
| **Windows ARM64** *(provisional)* | AArch64 Windows | `x0`–`x7` | `v0`–`v7` | 16 | Differs from AAPCS64 for several composite shapes | no red zone |
| **RISC-V LP64D** *(provisional)* | riscv64 Linux | `a0`–`a7` | `fa0`–`fa7` | 16 | ≤ 2×XLEN in registers, else by reference | no red zone |
| **MIPS n64 / o32** *(provisional)* | mips64 / mips32 | `a0`–`a7` / `a0`–`a3` | `f12`–`f19` | 16 / 8 | By rule; several shapes by reference | BE variants exist (§6) |
| **i386 SysV (cdecl)** *(provisional)* | x86-32 Linux | all on the stack | all on the stack | 16 | By value on the stack | no red zone |

### 5.3 What survives all of them unchanged

Everything in §3 except `v128` is portable **without any per-target special case**:

- `i32`, `i64`, `f32`, `f64` and pointers are register-passed everywhere, in a fixed
  order, in the kinds of register C expects.
- A **single scalar return** is in a register everywhere; `void` is nowhere a problem.
- The **multi-value buffer** is one extra pointer argument, appended last — portable as
  long as it really is last and really is a pointer (the current rule,
  [`ABI.md`](ABI.md) "Multiple Results").
- **References as `void*`** are just pointers.

That is the useful result of the whole survey: the canonical signature was chosen in a
shape that only has one genuinely contested member.

### 5.4 The `v128` landmine — and why the deferral is not procrastination

> **Resolved 2026-11-06.** The four-way conflict is avoided rather than settled on one of its
> four horns: `v128` is spelled as **two `uint64_t` parameters, low word first**, which is
> register-passed on SysV, MS x64, AAPCS64 and RISC-V alike — because it is *two scalars*, not an
> aggregate, so the MS x64 by-reference rule never applies. A `v128` result travels in the
> multi-value buffer as `lo, hi`. The analysis below is kept as the record of why the other three
> spellings were rejected.

`v128` has two C-expressible spellings, and they disagree across the survey:

| Spelling | SysV AMD64 | AAPCS64 | RISC-V | MS x64 |
|---|---|---|---|---|
| `struct { uint64_t lo, hi; }` (aggregate) | two INTEGER eightbytes → **2 GPRs** | composite, not HFA/HVA → **2 `x` regs** | exactly 2×XLEN → **2 `a` regs** | **by reference** (hidden pointer) |
| a 16-byte vector type (`__m128`, `float32x4_t`) | one `xmm` | one `v` (Q) register | no vector class in the base psABI → by reference | `xmm0` (and only as `__m128`) |

So there is **no single C-expressible `v128` shape that is register-passed on all four**.
The aggregate spelling is portable on SysV, AArch64 and RISC-V and becomes a *hidden
pointer* on Windows x64; the vector spelling is register-passed on SysV/AArch64/Windows
and unsupported by the base RISC-V psABI, and C has no portable name for it. The
deferred decision in [`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md)
§11 is therefore a real four-way conflict with four viable resolutions: declare the
aggregate and document the Win64 by-reference consequence; pass `v128` through memory
on every target (fully portable, costs a copy); refuse `v128` in C-callable exports
while keeping it internal; or spell it as a target vector type per platform.

**Measured state (2026-11-06):** SLJIT's `beginFunction` describes a `v128` parameter as
**two 16-byte integer slots** (`SLJIT_ARG_TYPE_W` twice, `Sljit/SljitOpcodeDispatcher.cpp`)
and AsmJit mirrors that shape. The `v128_param` test passes through both backends
because caller and callee are the *same* backend with the *same* description. Under
D-ABI-1 that is an **internal convention, not yet a C ABI**, and §10 carries it as an
open row.

## 6. Byte order and word size

Two properties of the host that are *not* calling-convention matters but break
portability just as thoroughly:

- **Endianness.** Wasm linear memory and the GC heap are little-endian by definition.
  On a big-endian host (some MIPS and PowerPC configurations) every multi-byte load and
  store must byte-swap, and a GC object's bytes are laid out little-endian regardless of
  the host. **Decision: supported targets are little-endian only, and this is a stated
  rule rather than an accident** — a big-endian port must add swapping at every
  multi-byte access, which is a port project, not a build flag.
- **Word size.** On ILP32 hosts a reference is 4 bytes, `i64` occupies two slots, and
  16-byte alignment for a `v128` can exceed what the ABI guarantees. **Decision: a
  64-bit host is required** (`wasm64` additionally requires it); ILP32 is unsupported
  and any code that assumes an 8-byte pointer must say so. The GC layout expresses its
  pointer-valued slots as *pointer-sized* (§8.1), so the layout adapts rather than
  breaking — but the adaptation is deliberate, not incidental.

## 7. Per-backend status, and how LibJIT avoided the question

**Snapshot (2026-11-06, working tree):** the three backends reach the same contract by
three different routes, and only one of them is C-ABI-shaped by construction.

| Backend | How the entry ABI is fixed | Portable today? |
|---|---|---|
| **SLJIT** | `sljit_emit_enter(compiler, 0, argTypes, ...)` — flags `0` means *no* `SLJIT_ENTER_REG_ARG`, i.e. SLJIT's native C convention. Arguments are described **abstractly** as `SLJIT_ARG_VALUE(kind, slot)`, and SLJIT maps slots to registers per target. | **Yes for all §3 kinds except `v128`** — the abstraction is exactly the portability mechanism D-ABI-2 asks for. The `v128` two-slot description (§5.4) is the exception. |
| **AsmJit** | `compiler.new_func(*record->signature)` with every `FuncSignature` built from `CallConvId::kCDecl` (53 of 53 sites in `Asmjit/AsmjitModuleCompiler.cpp`). `kCDecl` means *the platform's default C convention*, which AsmJit instantiates as SysV or MS x64 per target. | **Yes on x86**, both x64 ABIs, because the convention is not hard-coded. An AArch64 port needs `asmjit::aarch64::Compiler` behind the same seam — permitted inside `Asmjit/` under the adjacent-backend rule ([`LAYERING.md`](LAYERING.md) §4). |
| **LibJIT** | LibJIT **owns both sides of every call**. Functions are created from a `jit_type_t` (`jit_function_create`) and invoked through LibJIT's own driver entry (`fnPtr = driverEntry` in `LibJit/LibjitModuleCompiler.cpp`), with `jit_function_to_closure`/`jit_function_apply` as the bridge to native code. | **Yes, but for a different reason** — see below. |

### 7.1 Why LibJIT never had this debate

This deserves a straight answer, because it was never a design choice we made.

LibJIT does not expose a calling convention to its user. A `jit_function_t` is an opaque
handle: LibJIT decides for itself how arguments reach the callee, and when native code
must call in — or the function must be called from C++ — it synthesises the bridge
(`jit_function_to_closure`, `jit_function_apply`) or its driver entry, which *is* the
host C ABI. There was therefore never a moment at which we picked SysV over MS x64 for
LibJIT: the question was structurally unavailable, and the answer was fixed by LibJIT's
own lowering.

Two consequences worth recording:

- **LibJIT's compiled entry is a thunk, not a raw C function.** Our `Callable` carries
  the driver entry in `fnPtr` and keeps the raw entry separately (`rawFnPtr`) precisely
  because the two differ. Any claim that "LibJIT is the portable backend" is true only
  in the sense that the seam is hidden, not in the sense that it did more work.
- **That is also a legal strategy for a future backend** (D-ABI-1 only constrains what
  is *externally visible*): a backend may emit the host C ABI directly, or own a thunk
  that adapts to it. Both satisfy the contract; what it may not do is expose a private
  convention as if it were C (§4).

### 7.2 The requirement this puts on a fourth backend

A new backend must state, in its own README:

1. **Target matrix** — which architectures and which OS/ABI combinations it supports,
   with untested combinations labelled *provisional* (D-ABI-5).
2. **Which member of §3 it cannot honour yet**, by name, and how it refuses that export
   (a compile-time refusal, never a silently different convention).
3. **Whether its entry is emitted C ABI or thunked** (§7.1), and where the thunk lives.

## 8. The GC / heap object ABI

This section is the canonical, backend-agnostic and platform-agnostic ABI for GC heap
objects. It exists because the surface was previously documented only as code plus the
invariant list in [`proposals/GC.md`](proposals/GC.md): a backend could read it, but an
embedder or a new backend had nothing to read.

### 8.1 Object layout — owned by `WasmGcHeader.hpp`

The single source of truth is [`WasmBase/WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp).
The shape, in little-endian bytes:

```
common:  [u32 TypeId][u32 refcount]                              // 8 bytes
struct:  [u32 TypeId][u32 refcount][void* descriptor] <fields…>  // 16-byte header
array:   [u32 TypeId][u32 refcount][u32 length]      <elements…> // length at offset 8
```

| Property | Rule | Function that owns it |
|---|---|---|
| Tag / refcount | `TypeId` at 0, refcount at 4, both `u32` | `GcHeader::kTypeIdOffset`, `kRefcountOffset` |
| Struct header | 16 bytes, descriptor slot at 8 (null for an ordinary struct) | `GcHeader::kHeaderBytes`, `kDescriptorOffset` |
| Struct field *i* | natural alignment, no repacking | `gcStructFieldOffset(fields, i)` |
| Struct size | data end rounded up to the strictest field alignment | `gcStructSize(fields)` |
| Array length | `u32` at offset 8 | `GcHeader::kArrayLengthOffset` |
| Array data | inline, starting at the element-aligned offset after the header | `gcArrayDataOffset(element)` |
| Element stride / alignment | by storage type: packed `i8` = 1, packed `i16` = 2, `i32`/`f32` = 4, `i64`/`f64`/reference = 8, `v128` = 16 | `gcStorageBytes`, `gcStorageAlign` |
| Byte order | **little-endian**, regardless of host (§6) | this document |
| Pointer-valued slots | pointer-sized (8 on the supported 64-bit hosts) | `WasmGcHeader.hpp` layout comments |
| Object identity | the address of the header word; a reference is a `void*` to it | `WasmValue.hpp` reference representation |

**No backend may compute these offsets itself.** The layout functions are the ABI; a
backend that recomputes an offset from its own assumptions has forked the layout, and
the `[gc][layout]` test exists to catch exactly that.

### 8.2 Allocation entry points — the canonical call shape

| Entry point | Signature |
|---|---|
| struct | `void* allocateStructObject(uint32_t size, uint32_t localTypeIdx, void* descriptor = nullptr)` |
| array | `void* allocateArrayObject(uint32_t localTypeIdx, uint32_t length)` |

The second (type-derived) array form is the canonical one: the allocator derives the
data offset and element size from `localTypeIdx` and writes the header itself, so a
backend passes only what it knows — the type and the length. The four-argument
primitive remains for internal use; backends do not call it, because a backend that
passes its own data offset and element size is again forking the layout.

Why this matters as an *ABI* question and not a style question: the earlier helper
took five arguments, which does not fit the four-argument helper ABI a backend can
encode, and the fix was to make the call shape type-derived rather than to give the
backend a fifth register. **The call shape is part of the contract**, exactly like the
object layout.

### 8.3 Trap kinds are part of the GC ABI

A trap is a returned status with a **kind** ([`SAFETY.md`](SAFETY.md) §2.6), and the
core distinguishes kinds per aggregate: `NullStructureReference` for a struct access on
null, `NullArrayReference` for an array access on null, `OutOfBoundsArrayAccess` for an
index outside an array, `CastFailure` for a failed `ref.cast`. A backend may not
collapse these into a generic `NullReference` — that is an ABI change visible to every
embedder, and it happened silently once
([`backends/ASMJIT_SLJIT_P3_GC1.md`](backends/ASMJIT_SLJIT_P3_GC1.md) §19). The
`[gc]` trap-kind test asserts the four kinds per backend.

### 8.4 What a backend may vary

Everything about *how* an access is emitted: the addressing mode (constant-folded
offset on a base, base+index, a walking pointer), whether it is inline or a helper
call, which register holds the object, whether an access is hoisted, and how a loop is
emitted. What it may not vary: an offset, the header shape, the `TypeId`'s meaning, an
element stride, the byte order, or a trap kind.

### 8.5 Reference ownership (the part backends get wrong)

- A reference **stored into a heap field** (struct field, array element, global, table)
  is retained by the store; overwriting it releases the previous value. The struct
descriptor slot behaves the same way, which is why the allocator retains it
  ([`WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp) header comment).
- A reference held only in a Wasm local or on the operand stack is **not** automatically
  a GC root; the backend is responsible for making live references visible at the
  points where collection can run.
- A reference returned to the embedder is owned by that instance until the embedder
  releases it ([`ABI.md`](ABI.md) "Embedder Guide").

That is the whole of the ownership contract; anything a backend does beyond it (extra
retains, an ownership-side table) is an implementation detail that must not be
observable at the call boundary.

## 9. How this stays true

### 9.1 What already enforces it

- **A/B parity tests** — most new surface is asserted on both backends in one case
  (`[sljit][asmjit][parity]`), which catches a backend that drifts from the canonical
  signature or the canonical layout.
- **`[gc][layout]`** — asserts the layout functions against the header comment, so a
  layout change cannot hide.
- **`[gc]` trap-kind test** — asserts the four kinds per backend (§8.3).
- **`callCallable` / `callCallableWithResults`** — the single embedder entry point, which
  performs the signature cast in one place instead of at every call site.

### 9.2 What does not exist yet

- **A literal C-caller conformance test.** Every existing test calls through
  `WASM::callCallable`, which is C++ that knows the shape. A test that declares a
  compiled export as an ordinary `extern "C"` function pointer and calls it with no
  Wasm-aware code would prove D-ABI-1 directly. Proposed in §10.
- **Any non-x86 or non-Linux execution.** Windows x64, AArch64 and RISC-V are
  *provisional* (§5.2); the survey is documentation, not evidence.

### 9.3 The target matrix this document commits to

| Target | Status |
|---|---|
| x86-64 Linux (SysV AMD64) | **tested** — the development target |
| x86-64 Windows (MS x64) | **provisional** — specified, untested here |
| AArch64 Linux/macOS (AAPCS64) | **provisional** — specified, untested here |
| AArch64 Windows | **provisional** — specified, untested here |
| RISC-V 64 (LP64D) | **provisional** — specified, untested here |
| 32-bit hosts (ILP32), big-endian hosts | **unsupported**, by decision (§6) |

A future backend or a future CI job may promote a row from provisional to tested; nothing
may demote a tested row without a decision recorded here.

## 10. Open rows

| # | Open item | Consequence | Proposal |
|---|---|---|---|
| ~~**P1**~~ | ~~`v128` parameters and results~~ | **CLOSED 2026-11-06**: two `uint64_t` parameters (low word first), results in the multi-value buffer as `lo, hi`. Register-passed on every supported ABI; no aggregate rule involved. See [`backends/V128_SIGNATURE_DECISION.md`](backends/V128_SIGNATURE_DECISION.md) §28. |
| **P2** | `i31ref` as `void*` | A tagged `i31` is not a valid C pointer; handing one to C breaks D-ABI-1 even though it works by accident | Expose `i31ref` to C callers only as `i32`, or box it at the boundary |
| **P3** | No literal C-caller test (§9.2) | D-ABI-1 is asserted by construction, not by test | Add one test that calls an export through an `extern "C"` declaration |
| **P4** | MS x64 aggregate rule | If P1 picks the two-`u64` aggregate spelling, Windows x64 passes it **by reference**, so the caller's declaration must be `struct` — not two scalars | Document the per-target spelling once P1 is decided; never paper over it |
| **P5** | Big-endian and ILP32 | Unsupported by decision | Revisit only as a port project (§6) |
| **P6** | Windows ARM64 composite rules | Differ from AAPCS64 for several shapes; a `v128` resolution that assumes AAPCS64 may not hold | Fold into P1's decision |
| **P7** | Trap entry point sprawl | Thirteen per-kind wrappers per backend; a global substitution across them once picked the wrong kind | Collapse to one `ReportTrap(kind)` per backend (Gate D in [`backends/GC_PLAN.md`](backends/GC_PLAN.md) §11) |

## 11. Normative references

Calling-convention facts in §5 are taken from the platform ABI documents, not from
observation:

- **System V AMD64 ABI** (x86-64 psABI), the *Calling Conventions* chapter — argument
  classes, the red zone, stack alignment.
- **Microsoft x64 calling convention** and **ARM64 calling convention** (Microsoft
  Learn) — the four integer registers, the 32-byte shadow space, the absence of a red
  zone, and the by-reference rule for aggregates.
- **AAPCS64** (Arm *IHI 0055*, Procedure Call Standard for the Arm 64-bit Architecture)
  — `x0`–`x7`, `v0`–`v7`, and the HFA/HVA rules.
- **RISC-V ELF psABI** — `a0`–`a7`, `fa0`–`fa7`, and the 2×XLEN aggregate rule.
- **MIPS n64 ABI** — `a0`–`a7` and the big-endian variant's existence.

In-tree companions: [`ABI.md`](ABI.md) (the signature and `VMContext*` rules),
[`SAFETY.md`](SAFETY.md) §2.6 (traps as status),
[`LAYERING.md`](LAYERING.md) §4 (the backend contract),
[`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) (the obligations a backend must meet),
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) (which of these surfaces are frozen).
