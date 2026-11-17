# AsmJit backend {#backend_asmjit}

*The third code generator: AsmJit registration, the universal emission seam and
the byte-order conversion*

This directory contains a native WebAssembly backend built on AsmJit. It
implements the four `ModuleInstantiator` hooks and executes the shared lowering
slice with AsmJit’s register allocator.

**Status (2026-11-17, working tree):** **production-ready within the declared
envelope, converting, and now target-independent at the source level** —
`WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345
'~[tail]~[compilation-policy]'` reports **23,139 assertions in 857 cases,
green**. Every emitted instruction goes through AsmJIT's Universal JIT, every
guest access converts when the byte-order decision says so, and the translation
units name no host instruction, register or memory type — so a second target is
an AsmJit build configuration, not a change here (`BE-7`). The envelope is still
x86-64 Linux / SysV AMD64 with the external AsmJit library available at build
time, and the documented refusals are the same three as SLJIT’s (see
`Sljit/Sljit.md` "Known limitations"). See
[`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md) §12 for the verdict
and §8 for the open register and the defect ledger. It is still not a
*code-quality* drop-in replacement for GNU LibJIT.

## What exists

- `AsmjitContext` owns an `asmjit::JitRuntime` and the executable allocations
  created through it.
- `compileReturn42()` emits a tiny x86/x64 function, installs it into executable
  memory, and returns its entry point.
- `compileLaneWiseI32x4()` uses AsmJit's function compiler and register
  allocator to emit a host-ABI function that adds or multiplies four 32-bit
  lanes. The inputs and output are ordinary pointers; the implementation does
  not assume a particular host argument-register convention.
- `TypeTranslator` maps Wasm integer, floating-point, reference, and V128
  values to AsmJit `TypeId`s and builds C-declaration function signatures. It
  prepends `VMContext*`, maps V128 to AsmJit's 128-bit vector type, and uses a
  trailing caller-owned result buffer for multi-value results.
- `ModuleCompiler` implements `translateTypes`, `declareFunctions`,
  `compileFunctions`, and `callStartFunction`. It declares all internal function nodes before compiling bodies, preserving
the two-pass instantiation contract and enabling forward and mutually recursive
direct calls. Each generated function emits the required
  `wasm_stack_check(VMContext*)` prologue call.
- The body compiler covers the same lowering slice as LibJIT, including the
  Custom Descriptors family (`struct.new_desc`, `struct.new_default_desc`,
  `ref.get_desc`, `ref.cast_desc_eq(_null)`, `br_on_cast_desc_eq(_fail)`), whose
  descriptor operand is retained into the new object's header slot and released
  from the operand stack, and trap-site attribution: `unreachable` reports at
  its own instruction and a helper-raised trap is annotated with the calling
  function's index, offset and `$name` before the frame returns.
- The scalar core supports i32/i64/f32/f64 parameters, results, and
  locals; i32/i64 constants and local access; i32.wrap_i64, i64.extend_i32_s/u and i64.extend8/16/32_s; i64 arithmetic (`add/sub/mul/div/rem`),
  bitwise operations, shifts, rotations, comparisons, and `eqz`; i32 arithmetic,
  bit counts, sign extension, comparisons, bitwise operations, shifts, and rotations;
  scalar `f32`/`f64` constants, arithmetic, `min`/`max`, `abs`/`neg`/`sqrt`,
  rounding (`ceil`/`floor`/`trunc`/`nearest`), `copysign`, all comparisons, the
  checked and saturating integer conversions, `demote`/`promote`, and
  reinterpretation. A float travels through the value stack as its IEEE bit
  pattern in a Gp and materialises in an Xmm only at an arithmetic/compare/ABI
  boundary, so `select`, block transfer and locals are the same code as for
  integers;
  `drop`, untyped and typed scalar `select`; i32/i64/f32/f64 globals; void and scalar/`v128`-result `block`/`loop`/`if`/`else`, `br`/`br_if`/`br_table`,
  branches to the implicit function label, and explicit/implicit returns; direct calls
  (native imports, forward calls and mutual recursion), typed `call_indirect`,
  with scalar signatures including native and cross-instance Wasm imports;
  direct/indirect/reference tail-call opcodes with constant-stack coverage
  (`[asmjit]` and the generic suite);
  and memory32/memory64 i32/i64/f32/f64 loads/stores, including signed/unsigned narrow extensions,
  `memory.size`/`memory.grow` at both index widths, checked memory32 `memory.copy` (with overlap-safe `memmove` semantics), `memory.fill`, `memory.init`, `data.drop`, `table.init`, `elem.drop`, `table.size`, and checked overlap-safe `table.copy`.
  Memory accesses use a checked helper that observes the shared-memory size
  accessor and reports out-of-bounds traps through the pending-trap protocol.
  Function nodes are declared in one CodeHolder before any bodies are emitted,
  so direct-call labels resolve across the entire module. Result-bearing blocks
  support scalar and `v128` results and block parameters; `br_table` still
  requires identical target stack heights/signatures. The `unreachable`
  instruction reports a trap and supplies a limited single-result placeholder,
  but general unreachable-stack polymorphism remains unsupported. Cross-instance
  Wasm imports are covered by tests. Globals are lowered. The complete shared
  SIMD surface, the reference family, GC construction and access, the Custom
  Descriptors family, exceptions and continuations all execute; `v128`
  signatures are implemented (two `uint64_t` words, per
  [`docs/ABI.md`](../docs/ABI.md)),
  and the opt-in frame ABI is available on the same terms as the other two
  backends. What remains are the named refusals, not a missing instruction
  family; `f16` has no stable encoding.
- `Test/TestBackend.cpp` exposes an `AsmJitSession` through
  `WASMJIT_BACKEND=asmjit`, including i32 invocation marshalling, aggregate
  layout calculation, and the existing lane-wise SIMD diagnostic interface.
- `Test/AsmjitModuleCompilerTests.cpp` instantiates WAT fixtures and executes i32/i64 arithmetic, comparison, bitwise and memory operations,
  control-flow, direct/indirect-call (including i32/i64 native and cross-instance Wasm imports, mutual recursion, and i32 tail-call opcodes), result-bearing control flow, `br_table`, unreachable traps, i32/i64 globals, and memory32
  cases, including signed/unsigned i32/i64 narrow loads, `select`, overlap-safe bulk copies,
  fills, passive data/element segment initialization/drop semantics, table sizing/copying, zero-length boundary operations, and out-of-bounds traps.
- Packed `i8`/`i16` storage retains its narrow 8-/16-bit representation.
  Arithmetic lowering must explicitly extend those values to `i32` before
  applying Wasm integer operations; the storage translator does not silently
  widen their in-memory layout. `compilePackedI8Extend()` and
  `compilePackedI16Extend()` emit signed and unsigned extension probes using
  `movsx`/`movzx`, with tests for high-bit-set inputs.
- `compileV128I32x4Add()` emits a real XMM-backed packed addition using
  `movdqu` and `paddd`, with a test covering lane wraparound.
- `compileV128Bitwise()` emits XMM-backed V128 AND, OR, and XOR operations
  using `pand`, `por`, and `pxor`. Tests compare all 16 output bytes against
  the corresponding scalar bitwise operation.
- `Test/AsmjitTypeTranslatorTests.cpp` covers scalar signatures, opaque
  references, V128-by-value signatures, packed storage widths, multi-value
  result-buffer placement, and rejection of packed function signatures (which
  are invalid Wasm).
- The context releases all generated entry points at destruction. If tracking
  an allocation fails after executable memory is allocated, it releases that
  allocation before propagating the host-side exception. Module compilation
  emits a module into one CodeHolder and installs it only after every function
  body succeeds, so a failed module leaves no partially installed code.
- The backend registers as `asmjit` through `AsmjitBackend.cpp`
  (`AsmjitBackend.cpp`, `WASMJIT_REGISTER_BACKEND`), so `WASM::Runtime::create("asmjit")`
  reaches it the same way the LibJIT and SLJIT backends are reached, and the
  keep-alive `BackendInstance` holds the shared `Context`. `ModuleCompiler`
  overrides `releaseInstance`, which returns the instance's one installed
  allocation through `Context::release` on `Runtime::unload`; a second instance
  of the same module keeps its own allocation and stays callable.
- `Test/AsmjitBackendTests.cpp` checks that emitted machine code executes,
  returns the expected scalar value, performs lane-wise, vector, and
  packed-storage operations, and that the registry reaches the backend by name.
- The dependency is confined to this subproject. `WasmBase/` remains
  backend-agnostic and does not include or link AsmJit.

The module compiler covers the same lowering slice as the other two backends and
runs the generic suite to completion. The remaining precision limits are the
ones recorded in [`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md)
§30 — notably reference ownership through a struct field or array element is a
borrow, not an owned count — and they are leaks, never wrong results.

## Build dependency

AsmJit is consumed straight from the system installation: the sources include
`<asmjit/asmjit.h>` and `Asmjit.pro` links `-lasmjit`. On Gentoo, `equery f
asmjit` lists what that install provides — the headers under
`/usr/include/asmjit`, `libasmjit.so` under `/usr/lib64`, and CMake package
files under `/usr/lib64/cmake/asmjit`. It ships **no** `asmjit.pc`, so the
subproject does not use pkg-config: a `PKGCONFIG += asmjit` line fails
configure on such a host.

The sources target the installed AsmJit 1.21 API (`asmjit::Out<>` for output
parameters such as `invoke`'s `InvokeNode*`, and `ASMJIT_ARCH_X86` for the
target-architecture check).

## Byte order

A big-endian host is **served, not refused**. This backend emits the same byte
reversal SLJIT does (docs/proposals/BIG_ENDIAN.md `BE-7`, closing `BED4`'s AsmJIT
half). The reversal is decided once, while the code is generated, so a
little-endian build emits nothing extra and its code is byte-identical to what it
always was (`BED5`).

The *conversion* and the *reach* are now both complete: the conversion is
policy-driven at every guest access, and the source names no host type, so
building it for a big-endian AArch64 host is an AsmJit configuration question
rather than a change to this code ("nothing is host-spelled" below). No such
build has been run — that is `BE-5`'s runner — so the target remains
*provisional* in [`docs/ABI.md`](../docs/ABI.md) terms.

What converts, and what does not:

| Access | Rule |
|---|---|
| Every linear-memory and GC-array element load/store (`i32.load`, `i64.store`, the extending forms, `f32`/`f64` bit patterns) | converts when the instance's decision says so — one funnel, `emitMemoryLoad`/`emitMemoryStore` |
| Packed array elements and struct fields (`array.get_u`, `struct.get`, `array.new_fixed`, `struct.set`, `struct.new`) | converts: they hold guest values like any other slot |
| The GC header's length word | **always** little-endian on every host (`BED3`), so it converts by *target* rather than by policy, and emits nothing on a little-endian build |
| `v128` loads and stores | not converted: the proposal leaves the lane convention to the implementation, and SLJIT does the same |
| Globals, table slots, the VM context, stack slots | not converted: they are host-native storage the guest never sees as bytes |
| Atomics, `memory.copy`/`fill`/`init`, data and element segments | already converted in the shared helpers (`BE-4`) and the host-side appliers (`BED3`) |

`BE-7`'s remaining work therefore ended with the emission model: once the
vocabulary was universal there was a place to put the reversal, and the same
`Emitter` methods that emit a load now emit the conversion with it.

One trap is worth recording, because it cost a test cycle: **a rotate with a
16-bit operand is a 32-bit rotate here.** `ror r16, 8` — the natural halfword
reversal — is promoted to its 32-bit register, so it rotates the whole doubleword
and leaves the value at bits 16–31. `reverse16` is therefore a `bswap` of the low
doubleword followed by a shift down, which is two universal instructions and no
sub-register surprise. The `halfword_signed`/`halfword_unsigned` exports in
`Test/wasm_wat/byte_order_roundtrip.wat` exist because of it: they fail if the
reversal runs after the sign extension, widens the value, or rotates at the wrong
width.

## Emission model: `ujit` first, the host compiler second

Instruction emission goes through **AsmJit's Universal JIT** (`asmjit::ujit`)
rather than through `asmjit::x86` directly — milestone `BE-7` in
[`docs/proposals/BIG_ENDIAN.md`](../docs/proposals/BIG_ENDIAN.md). The reason was
portability, not style: `ujit` is the only part of AsmJit that abstracts x86 and
AArch64, and the byte-order conversion could only be emitted once this backend's
instruction vocabulary was target-independent.

The shape of the seam is small because `ujit` was designed that way:

- `ujit::UniCompiler` **wraps** the `asmjit::host::Compiler` this backend already
  used, and `ujit::Gp`, `ujit::Vec`, `ujit::Mem` *are* the host's register and
  memory types. Registers, stack slots, labels, function nodes and
  `invoke`/`set_arg` therefore do not change at all.
- One `UniCompiler` is constructed per `CodeHolder` in `compileFunctions`, and
  `add_func`/`end_func` **hook and unhook** it per body. Emission inside a body
  is legal only between those two calls, which is why the loop calls them through
  the universal layer.
- `AsmjitEmitter.hpp` is the only place an instruction is spelled. A site with a
  universal instruction calls it and nothing else; a site without one emits
  through `host()` and appears in the table below.

### What was converted

The conversion is finished: **every instruction this backend emits goes through
`AsmjitEmitter.hpp`**, and no file spells an x86 instruction or an `Inst::kId*`
any more. `Test/AsmjitBackendTests.cpp` holds the ledger
(`asmjit: the host-specific emission set only shrinks`, `[asmjit][ujit]`): it
reads both backend translation units and fails if a `compiler.<instruction>(`
call, an unlisted `host().<instruction>(` call or an instruction id appears.
Only the *structural* calls are allowed to stay — `new_gp32`, `new_xmm`,
`new_stack`, `new_label`, `bind`, `ret`, `invoke`, `new_func`, `finalize` — and
those are ABI and register plumbing, which `ujit` delegates rather than replaces.
Two of them (`new_gp32`, `new_xmm`) are also the x86 spelling of a portable call;
"what is still host-spelled" below is the residue the ledger does not see.

The slices, in the order they landed:

| Family | Universal form | Slice |
|---|---|---|
| Integer immediates, moves, loads and stores | `mov`/`movImm` → `kMov`, `kLoad*`, `kStore*`, `kStoreZero*` | S1 |
| Integer arithmetic and logic | `add`/`sub`/`and_`/`or_`/`xor_`/`imul`/`dec` → `kAdd`/`kSub`/`kAnd`/`kOr`/`kXor`/`kMul` | S1 |
| Shifts and rotates | `shl`/`shr`/`sar`/`rol`/`ror` → `kSll`/`kSrl`/`kSra`/`kRol`/`kRor` | S1 |
| Sign and zero extension | `movsx*`/`movzx*` → `kLoadI8/I16/I32`, `kLoadU8/U16/U32`, `kSll`+`kSra` | S1 |
| Bit counts | `bsf`/`bsr` → `clz`/`ctz` (`kCLZ`/`kCTZ`) | S1 |
| Carry chains (`i64.add128`/`sub128`) | `adc`/`sbb` → `kAdd` + `ucmp_lt` + `select` | S1 |
| Comparisons, branches, `setcc` | `test`/`cmp`/`jcc`/`set` → `UniCondition` + `j`/`select` | S2 |
| Address formation | `lea` → host `lea` **by decision** (see below) | S3 |
| Scalar float binary and compares | `addss`…`divsd`, `ucomiss`/`ucomisd` → `kAddF32S`…`kDivF64S`, `kCmpEqF32S`…`kCmpGeF64S` | S4 |
| Vector arithmetic, compares, moves | `p*` → `kAndU32`/`kAddU8`/`kAvgrU8`/`kMinU8`/`kCmpLtI8`/`kLoad128_U32`/`kStore128_U32`/`kMovU32`… | S5 |
| Diagnostic probes (`AsmjitContext.cpp`) | the same vocabulary, through the same façade | S6 |
| Guest accesses and the GC header word | `guestLoad*`/`guestStore*`, `headerLoad32` → `kLoad*`/`kStore*` plus `kBSwap`/`kRor` | S7 |
| Sub-register views, sign and zero extension, flag materialisation | `mov` made total (width change through a slot), `movsx*`/`movzx*` as shift pairs, `selectFlag` without a subject | S8 |
| Host *types* and memory operands | `x86::Gp`/`Mem`/`Vec`/`Compiler`, `*_ptr(...)`, `CondCode`, `new_xmm` → the `ujit` aliases, `ujit::mem_ptr`, a relation enum over `cmp_eq`/`scmp_*`/`ucmp_*`, `new_vec128` | S9 |
| Address formation and the return-42 probe | `lea` → `ujit`'s `lea`; `x86::Assembler` + `eax` → the same `ujit` function path the backend uses | S9 |

### What the conversion bought

This was not a rename. Four site families came out *shorter and simpler*, and the
simplification is the same property that makes the port possible — one
instruction, one meaning:

- **Variable shifts need no fixed register.** The host lowering stored the value
  to a stack slot, moved the count into `CL`, shifted the slot and loaded it
  back. The universal form is one instruction: `shl dst, src, count`
  (`kSll` → `shlx` on this host).
- **Bit counts need no zero branch.** `bsr`/`bsf` are undefined for zero, so the
  code branched around a zero and adjusted the result with `^ 31`. `clz`/`ctz`
  are defined for zero as the register width, which is exactly wasm's answer, so
  the branch and the adjustment are gone.
- **Float compares need no parity game.** `ucomiss` + two `setcc` + `and`/`or`
  per predicate became one IEEE-754 predicate (`kCmpEqF32S`…) plus a lane move;
  NaN behaviour comes from the operation instead of from `kP`/`kNP`.
- **Vector compares need no operand swap.** `pandn`'s destination trick, the
  `pcmpgt` operand swap for `lt_s`/`ge_s`, and the all-ones register used to
  invert a mask all disappeared, because `kCmpLtI8`/`kCmpGeI8` name the
  predicate and the signedness.

### The last host-side path, and why it is conditional

The host-specific set is now **one guarded instruction family**, and it is
optional rather than required:

| Entry | Why it is host-side, and what happens without it |
|---|---|
| `psllw`/`psraw`/`psrlw`/`pslld`/`psrad`/`psrld` by a per-lane count (6 sites, inside `#if defined(ASMJIT_UJIT_X86)`) | the universal shift vocabulary is **immediate-count only** (`v_slli_i16`, `v_srai_i32`, …), so wasm's per-lane variable shift has no universal operation at all. `Emitter::hasVariableVectorShift()` answers the capability at compile time; a target without it takes the shared `wasm_simd` helper path, which is the same path `WASMJIT_SIMD_NATIVE=0` already uses. It is the one thing to file upstream |

Two entries that used to be here are gone, and neither was what the earlier note
claimed:

- **`lea` was never host-only.** `ujit` has `lea(dst, mem)`, including for a stack
  slot whose displacement is not final until the frame is laid out — which is
  exactly why the operand is passed through rather than decomposed. The façade
  now calls `universal_.lea` and the frame-relative sites work unchanged.
- **The return-42 probe was rewritten, not dropped.** It asked whether the CPU can
  execute what AsmJit produces; going through the same `ujit` function path the
  backend uses makes it answer that question about the code the backend actually
  emits, instead of about a bare host assembler.

### Width discipline: no sub-register views

The blocker the first audit found was the **81 sub-register views** (`.r32()`,
`.r8()`, `.r16()`, `.r64()`) that survived in these files after the instruction
conversion. They are gone — 0 in the compiler, 0 in the seam — because the rule
they expressed ("reinterpret this register at another width") turned out not to
exist in the universal vocabulary at all. Establishing that took two spikes
against AsmJit 1.21, and the findings are the design:

| Question | Answer |
|---|---|
| `mov r64, r32` (widen) | **rejected** — `InvalidInstruction` |
| `mov r32, r64` (narrow) | **rejected**, too: ujit's register move is *same-width only* |
| `shl r64, r32, imm` | asserts `dst.size() == a.size()` — shifts are same-width as well |
| `store_u8/u16/u32(mem, reg)` with a wider `reg` | **legal**, truncates by the memory operand's size |
| `load_u8/16/32` into a wider register | **legal**, zero-extends |
| `select dst64, Imm, Imm, flag` from a 32-bit subject | **legal** — no temp and no view needed |

So a width change is a **memory** operation in the universal vocabulary, and the
seam says so once:

- `Emitter::mov(Gp, Gp)` is *total*: same width is one `kMov`; a width change
  stores at the narrower width and loads at that width, which truncates or
  zero-extends exactly as the x86 view did. One store-forwarded pair, in one
  place, so no call site reasons about widths.
- **Sign extension is arithmetic, never a load.** A four-byte slot written once
  and read once is *promoted* by the register allocator, and the promoted copy
  loses the extension's sign — this cost one debugging cycle and a wrong
  `i64.extend32_s`. `movsx32`/`movsx8`/`movsx16` therefore copy and then shift up
  and back down at the destination's own width.
- `selectFlag` lost its parameter: the condition already carries the tested
  width, so the flag is selected straight into the destination at whatever width
  it has.

Two consequences worth knowing. `Emitter::mov` now allocates a stack slot when the
widths differ, so a width-changing copy is two instructions instead of one — the
price of expressing it portably, and exactly where a target with `uxtw`/`mov w, x`
would do it in one. And `i64.load32_s`-style accesses are unaffected, because a
widening *load* is a single universal instruction (`kLoadI32`).

### Nothing is host-spelled

The residue the first audit found — **416 `asmjit::x86::` spellings** across the
compiler and the context — is now **zero**, so this backend's source names only
`asmjit::ujit` aliases and no host type at all. What it took, and what it taught:

| Was | Now | Note |
|---|---|---|
| `asmjit::x86::Gp` / `Mem` / `Vec` (238) | `asmjit::ujit::Gp` / `Mem` / `Vec` | exact aliases — the same types on x86 |
| `x86::qword_ptr` / `dword_ptr` / `word_ptr` / `byte_ptr` / `ptr` (160) | `asmjit::ujit::mem_ptr(base, disp)` | `mem_ptr` carries **no size**, which is the one real API change |
| `x86::xmmword_ptr` (6) | `ujit::mem_ptr(...)` | the 128-bit helpers take the operand, not its width |
| `x86::CondCode` (`kE`, `kNB`, …) (21) | a local `Compare` relation enum over `cmp_eq`/`scmp_*`/`ucmp_*` | the factories carry the signedness; no host flag name survives |
| `asmjit::x86::Compiler` (9) | `asmjit::ujit::BackendCompiler` | alias of `host::Compiler` |
| `new_xmm(...)` (24) | `new_vec128(...)` | the portable allocation call |
| `x86::Assembler` + `x86::eax` (2) | the same `ujit` function path the backend uses | the return-42 probe |

The unsized operand was the interesting half, because the seam had been deriving
the access width *from* the memory operand's metadata (`accessWidth(mem.size(),
reg.size())`) — and sizes are an x86 convenience: `x86::Mem` has `set_size()`,
`a64::Mem` does not. So a sized operand is not portable, and the width now lives
where it belongs — in the seam: `store8/16/32/64` and `loadZ8/16/32` name it
outright, and the sites whose access is *narrower* than the register
(`array.copy`'s packed moves, the zero-initialised GC fields, the `i32` half of a
global cell) use them. Everything else keeps an unsized operand and the seam
derives the width from the register, which is what the size always meant.

Three ledger rules keep it closed: no `asmjit::x86` in any of the three files, no
sub-register view, and no `host()` call — plus an exact count of six for the
guarded vector shifts, so the host set cannot grow silently.

`BE-7`'s claim is therefore now the strong one: **the source is
target-independent.** A second-target build needs an AsmJit with that target
enabled, not a change to this code.

### Error reporting

`ujit`'s emit entry points return `void`, so a rejected emission is reported by
`BackendCompiler::finalize()` rather than at the call site. The backend still
checks `finalize()` and still refuses to install a module whose code did not
finalise; what it loses is the per-site message. That is recorded rather than
hidden, because it is a diagnostic regression, not a correctness one.

### What the conversion left open

Nothing in the emission path: the vocabulary is universal, the source names no
host type, and the byte reversal rides on the same guest accessors. Two things are
worth knowing rather than fixing:

- **A per-lane variable-count vector shift** has no universal operation (the
  vocabulary offers immediate counts only). It is the one guarded host path, it is
  capability-gated, and a target without it takes the portable helper route.
- **A register-to-register extension** has no universal operation either, so the
  façade re-expresses it as a shift pair, and a same-width-changing `mov` costs a
  store-forwarded pair. Two instructions instead of one, and nothing
  target-specific.

The byte-order half is closed for this backend; the port as a whole still needs a
big-endian runner (docs/proposals/BIG_ENDIAN.md `BE-5`), which is a hardware and
CI question rather than an emission one.

## Known limitations and next steps

1. **Reference ownership is exact and measured** since 2026-11-11: a reference
   read out of a struct field or array element takes its own count, `select`
   releases the arm it did not pick, `drop` releases the stack’s count, and a
   native import’s reference result is cloned. `gc_ownership.wat` (73 assertions)
   and `native_ref_result_probe.wat` (13) assert the counts here, so `GAP-4` and
   `GAP-5` are closed.
2. **A frame-form tail trampoline.** `return_call_ref` above the register-form
   ceiling is a semantic call + return here
   ([`docs/API_ABI_STABILITY.md`](../docs/API_ABI_STABILITY.md) §8, `GAP-2`).
3. **Portability.** Only x86-64 Linux / SysV AMD64 is exercised; the target
   matrix in [`docs/ABI.md`](../docs/ABI.md) keeps the
   other rows *provisional*.
4. **A wide `v128` indirect call in a module with a native import** is *accepted*
   since 2026-11-11 (`GAP-11`, `wide_v128_indirect_probe.wat`). `ref_test`’s
   `ref_test_extern` case answered `0` where the spec says `2`; that was the
   extern-box conversion replacing its operand without taking the stack’s count,
   and it is **fixed** here and in SLJIT (`GAP-15`, 2026-11-11), with
   `gc_extern_box.wat` as the fixture. What remains open is the full-suite
   `SIGSEGV` (`GAP-16`): a case that moves with the run, passes alone, and
   disappears when `WasmBase` is built `-O2` — so it is a build-configuration
   dependency on uninitialised state, not a refusal.
5. Meet every applicable item in
   [`docs/WRITING_A_BACKEND.md`](../docs/WRITING_A_BACKEND.md) before calling the
   backend complete.

GNU LibJIT, SLJIT and AsmJIT execute the same generic suite and are
production-ready within the same envelope. LibJIT remains the most mature in
*code quality*, not the only full-featured backend.
