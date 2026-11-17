# Big-Endian Hosts {#prop_big_endian}

*Implementation Notes, Spec Status, and Deliberate Deviations*

Status: design note / implementation reference (revision 2 — 2026-11-17: the
policy is public and honoured. The SLJIT backend converts the accesses it emits,
the atomic helpers convert the values they exchange, the GC header keeps one
layout on every host, AsmJIT converts like SLJIT (`BE-7`), and LibJIT — which
has neither a big-endian target nor a reversal op — refuses instead of
misreading it. No big-endian runner exists, so the target stays **unsupported**
and this note is what the remaining milestones — a runner, then the matrix —
follow).
Audience: anyone touching a backend's load/store emission, the atomic helpers,
`WasmGcHeader`, `WasmMemory`, or the byte-order switch an embedder sees.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`MEMORY_CONTROL.md`](MEMORY_CONTROL.md).

**This note is not about a WebAssembly proposal.** Big-endian support is a
*portability* decision, so §2 quotes the two specifications that pin byte order
and §3 lists the layout freedom they leave — which is what makes §6 legitimate
where it is legitimate and illegitimate where it is not.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the specifications demand about byte order? |
| §3 | What do they leave to us? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the specifications demand

- **Linear memory is little-endian.** Loads and stores are defined through the
  core specification's `il`/`bytes` byte interpretations (§4.3.3 “Bytes”), and
  the binary format states it in words for constants: “Floating-point values are
  encoded directly by their IEEE 754 bit pattern in little endian byte order.”
  There is no host-order parameter anywhere in the semantics.
- **The canonical ABI is little-endian.** Its `load_int`/`store_int` convert with
  `'little'`, and its string lowering writes `utf-16-le`, so a host that lowers
  a `list<u8>` into guest memory writes little-endian bytes there.
- **The guest is compiled by somebody else.** This is the asymmetry that decides
  the whole question. Our engine may choose how to compile the guest's own
  `i32.load`; it may **not** choose how the guest's toolchain emitted it, and it
  may not choose what the *host* side of the canonical ABI writes, because that
  must agree with the guest's compiler.
- **Data segments are untyped.** A segment is a byte string. The same four bytes
  are legitimately read as an `f32`, as an `i32` and as four `u8`s
  (`Test/wasm_wat` has no monopoly on this: the spec suite's `float_memory.wast`
  does exactly that), so no pass can rewrite a segment into another byte order.

Consequence: on a host whose own order is big-endian, every multi-byte access the
engine **emits** has to convert, and so does every multi-byte value the **host**
writes into guest memory. Nothing else in the engine needs to care.

## 3. What the specifications do not demand

- **The GC object header.** The GC proposal fixes no object layout, so
  `TypeId`@0 / refcount@4 / descriptor@8 is our ABI, not a quotation —
  [`GC_EXTENSIONS.md`](GC_EXTENSIONS.md) `GXD7` records that latitude.
- **The frame form and the `v128` word order.** Both are host-internal, and
  [`../ABI.md`](../ABI.md) already states the `v128` rule by *bit position*
  precisely so it survives a port.
- **How a conversion is implemented.** One instruction or three is the backend's
  business: s390x has `lrv`/`lrvg`/`strvg`, ppc64 `lwbrx`/`stwbrx`, BE MIPS
  `lwl`/`lwr`, BE AArch64 `rev`, and a backend without any of them can shift.
- **The policy switch itself.** Whether a host asks for conformance or for speed
  is an embedding decision, bounded by §6's guardrails.
- **32-bit and big-endian jointly.** Nothing forces a port to cover ILP32;
  [`../ABI.md`](../ABI.md) lists them together only because both are “not
  exercised”.

## 4. What we have implemented today

**Snapshot (2026-11-17, working tree on `dba097c`):** the policy is public and
honoured. SLJIT converts the accesses it emits, the atomic helpers convert the
values they exchange, the GC header keeps one layout on every host, and the
backend that cannot convert — LibJIT — refuses the module instead of misreading
it. SLJIT and AsmJIT both convert. No
big-endian runner exists, so the target stays **unsupported** and tier 3 of §8
is still what an authoritative answer needs.

| Piece | Where | State |
|---|---|---|
| The policy: `Strict` / `Adaptive` / `Nonconformant` | `WasmBase/WasmByteOrder.hpp` | implemented, **embedder tier** (`BE-2`) |
| Policy names, case-insensitive parsing, `WASMJIT_BYTE_ORDER` | `WasmBase/WasmByteOrder.cpp` | implemented; an unknown name stays `Strict` |
| Conversion primitives (`byteSwap`, `byteSwapIf`, `headerWordFromGuest`, `swapValue`) | same header | implemented as value-level functions; involution and known-answer vectors tested |
| Module evidence (`ByteOrderEvidence`, `byteOrderEvidence`) | same header/cpp | implemented: memories, import/export, shared flag, active and passive data segments |
| The decision table (`decideByteOrderFor`) | same cpp | implemented for **both** host orders, so the big-endian matrix runs on a little-endian build |
| The runtime switch: `RuntimeConfig::byteOrder`, `sByteOrder`, the instance's report | `WasmRuntime.hpp`, `WasmConfiguration.cpp`, `WasmModuleInstance.hpp` | implemented: an embedder sets it, reads back `ModuleInstance::byteOrderDecision()` |
| The diagnostic host order (`WASMJIT_BYTE_ORDER_HOST`) | `WasmByteOrder.cpp` | implemented; it changes what is emitted and nothing else (§8) |
| Conversion in emitted scalar accesses | `Sljit/SljitOpcodeDispatcher.cpp` (`emitGuestSwap`) | implemented for every 2/4/8-byte integer and `f32`/`f64` load and store |
| Conversion of the GC header's length word in emitted code | same file (`emitHeaderWordLoad`, 7 call sites) | implemented: converts on a big-endian build whatever the policy |
| Converted atomic RMW | `WasmBase/wasm_atomic.cpp` | implemented at the helper's value boundary (load, store, RMW, cmpxchg, wait); `table.atomic` reads a *pointer* and deliberately does not convert |
| The GC header's TypeId, refcount and array length | `WasmGcHeader.hpp` (`readWord`/`writeWord`), `WasmModuleInstance.cpp` (`GcRefcount`) | implemented: little-endian on every host, refcount through a coded-word CAS |
| Data segment → array element | `WasmModuleInstance.cpp` (`copySegmentIntoElements`) | implemented: segment bytes are decoded into native element values once |
| Refusal instead of degradation | `LibjitModuleCompiler.cpp` | implemented (`BED4`): LibJIT refuses a big-endian host in `translateTypes`. AsmJIT stopped refusing once `BE-7` gave it the reversal |
| The AsmJIT emission seam | `Asmjit/AsmjitEmitter.hpp` | **in progress** (`BE-7`): one `asmjit::ujit::UniCompiler` per code holder hooks each body, integer immediates already emit through it, and the 681 remaining host instruction sites are a shrinking budget `[asmjit][ujit]` enforces (`Asmjit/Asmjit.md` "Emission model") |
| Tests | `Test/ByteOrderTests.cpp` `[byteorder]` | 13 cases / 161 assertions; five fixtures `Test/wasm_wat/byte_order_*.wat` |
| Byte-order tripwire in the default suite | `Test/wast_supported.txt` | `endianness` and `endianness64` promoted (69 assertions each); `float_memory` was already there |
| SIMD `v128` blocks and lanes | `WasmBase/wasm_simd_*` | **absent**: the vector image convention is not pinned, so no conversion is emitted for a 16-byte block (§5 `BE7`) |

Two consequences of `BED3` are worth naming because they are not obvious. The
header's **refcount** cannot simply be byte-swapped around a read: a reversed
*sum* is not the sum of reversed terms, so the runtime keeps the coded word for
the CAS and the decoded value for the arithmetic (`GcRefcount`). And an **array
element** holds a native value while a data segment is a byte image, so the
decode happens once, at `array.new_data`/`array.init_data`, rather than at every
later element access.

The engine's own decoding was already order-free: every reader is an
`Elv::Io::DataStream<Ev::Util::Endian::Little>`, and the canonical ABI's
`readUInt`/`writeUInt` assemble bytes by shift. What this revision added is the
half the backends emit, plus the two places where the *host* writes bytes for a
third-party guest to read.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~BE1~~ | ~~**No emitted access converts.** Both backends emit a native load/store, so a big-endian build reads every multi-byte value reversed~~ | `Sljit/SljitOpcodeDispatcher.cpp` `emitGuestSwap`/`emitHeaderWordLoad`; `LibjitModuleCompiler.cpp` and `AsmjitModuleCompiler.cpp` refuse instead | **Done (`BE-3`):** SLJIT emits the reversal for 2/4/8-byte integer and `f32`/`f64` accesses and for the header's length word; the other two backends refuse (`BED4`). The `v128` half is `BE7` below |
| ~~BE2~~ | ~~**No runtime switch.** `decideByteOrder` is reachable only from tests, so no embedder can ask for a policy and no backend is handed one~~ | `RuntimeConfig::byteOrder`; `sByteOrder`; `ModuleInstantiator::setByteOrderPolicy`; `ModuleInstance::byteOrderDecision()`; the header is in the embedder tier | **Done (`BE-2`):** an embedder sets the policy in code, in a file or through `WASMJIT_BYTE_ORDER`, and reads the decision back per instance |
| ~~BE3~~ | ~~**Atomics remain native.** The helpers type-pun the guest address, so an RMW swaps on BE~~ | `WasmBase/wasm_atomic.cpp` (`atomicValueOf`, `swapValue`), `WasmTable.cpp` | **Done (`BE-4`):** every value crossing the helper boundary is reversed on a big-endian build; a table slot is a pointer and keeps its native order |
| ~~BE4~~ | ~~**The GC header promise is unqualified.** `ABI.md` promises little-endian to embedders while the header is stored natively~~ | `WasmGcHeader.hpp` `readWord`/`writeWord`; `GcRefcount`; `copySegmentIntoElements` | **Done (`BE-3`, as `BED3`):** the header is little-endian on every host, the refcount converts through a coded-word CAS, and the data-segment boundary decodes elements |
| BE5 | **Two of three backends cannot host the port.** The LibJIT this tree links has no big-endian target and no byte-reversal op | `jit-config.h` (interp/alpha/arm/x86/x86-64); `Asmjit/AsmjitModuleCompiler.cpp` | **Closed by `BE-7` for AsmJIT:** the backend emits the reversal, and its source no longer names a host instruction, register or memory *type*, so it builds for any target its AsmJit supports (`Asmjit/Asmjit.md` "Nothing is host-spelled"). LibJIT alone refuses (`BED4`); what remains is a big-endian runner (`BE-5`'s other half), not backend work |
| BE6 | **No runner.** Nothing in this tree has ever executed on a big-endian host, and tier 3 of §8 needs one | — | Blocker for “supported” |
| BE7 | **The vector image convention is unwritten.** `wasm_simd_scalar` reads and writes 16-bit/32-bit/64-bit lanes little-endian by shift, while the Highway path stores a vector's lanes in the host's order, so the two agree only on a little-endian host; nothing converts a 16-byte `v128.load` block, and the `v128` frame word order is likewise unpinned | `WasmBase/wasm_simd_scalar.cpp` (`laneU32`, `storeLane`), `WasmBase/wasm_simd_vector.hpp` (`StoreU`/`LoadU`), `ABI.md` “`v128` across the Boundary” | Blocker for SIMD on a port; it cannot be settled by review alone, because both conventions are self-consistent on the only host that runs |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| BED1 | **`Nonconformant`: never convert, on a big-endian host** (implemented, off by default). | It is *not* safe under the specification, and it is documented as a non-conformance rather than a shortcut: data segments, byte-level guest code, `f32`/`i32` punning and any second reader of the memory all break. It is legitimate only in a closed world where the host knows every guest and every byte in every memory is produced and consumed at whole width. **The relaxation covers emitted accesses only:** the atomic helpers convert unconditionally, because their ABI receives no policy and a process-wide switch would be wrong for a per-module decision. | Removes the conversion from a host that has measured it as noise. |
| BED2 | **`Adaptive`: skip when the evidence is closed** (implemented). | §3 gives us no freedom here — the relaxation is a heuristic, not a proof, because no static check sees a guest that writes four bytes with `i32.store8` and reads them back with `i32.load`. The bar is deliberately high (private, unshared memory; no active or passive data segment) and the verdict is reported in `ByteOrderDecision::reason` so a host can log it. | Conformance by default, speed where the module demonstrates it is safe. |
| BED3 | **Keep the GC header little-endian on a big-endian port** (**implemented**). | Nothing in the spec observes it, and keeping it preserves one heap layout everywhere, so `gcStructFieldOffset` readers and cross-host artifacts still agree. The words are read and written through `GcHeader::readWord`/`writeWord`, the count through `GcRefcount` (a CAS on the coded word, because a reversed sum is not the sum of reversed terms), and a data segment is decoded into elements at `array.new_data` rather than at every access. | No ABI fork between hosts. |
| BED4 | **A backend that cannot convert must refuse to compile, not degrade** (**implemented**). | Silent corruption is the failure class [`../SAFETY.md`](../SAFETY.md) exists to avoid; a refusal is a diagnostic. The refusal is unconditional on a big-endian host — not policy-dependent — because those backends also read the GC header's little-endian length word, which no policy relaxes. | A BE build fails visibly on LibJIT and AsmJIT instead of misbehaving. |
| BED5 | **The layer must cost nothing on a little-endian host** (**implemented and measured**). | The conversion is decided while the code is *generated*, so nothing is emitted when the host already agrees with the guest; the core's helpers are behind a compile-time `if constexpr` on the host order. `[byteorder]` measures it: one fixture compiles to the same code size under all three policies. | The policy cannot regress the only platform that works. |

Guardrail: `Strict` is the default on every path (the environment, a
configuration file, the embedder's struct), a name the engine does not recognise
resolves to `Strict`, and `Nonconformant`/`Adaptive` may only relax
**linear-memory access conversion** — never the canonical ABI's byte order where
the host is the writer and the guest is third-party, never the GC header's own
order, and never handle or resource semantics.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| `BE-1` ✅ | Policy layer: enum, names, environment, primitives, evidence, decision table, `[byteorder]` tests | — | 6 cases / 104 assertions green; the BE matrix runs on LE |
| `BE-2` ✅ | Plumb the policy into `RuntimeConfig` and the configuration-file reader, report the decision on the instance, promote `WasmByteOrder.hpp` to the public tier | BE2 | an embedder can set `WASMJIT_BYTE_ORDER=adaptive` and read back what was applied |
| `BE-3` ✅ | Emit the conversion: one helper per backend around load/store (scalar widths, `f32`/`f64`; the GC header's length word); SLJIT first, using `SLJIT_REV*`, which s390x compiles to one instruction. LibJIT refuses (`BED4`); AsmJIT followed in `BE-7` once its vocabulary was universal | BE1, BE4, and `BE5`'s LibJIT half by refusal | a fixture that stores with `store8`s and loads a word returns the same value on a BE host; on LE the three policies emit one code size (`BED5`), and `[byteorder]` pins both |
| `BE-4` ✅ | Converted atomic RMW in `wasm_atomic.cpp` (swap inside the helper, which already receives the width) | BE3 | atomics pass the spec scripts on a BE host |
| `BE-7` ✅ | **Done (2026-11-17):** `Asmjit/` emits through AsmJIT's Universal JIT (<https://asmjit.com/doc/group__asmjit__ujit.html>) — 681 host instruction sites became universal operations in six slices — and the backend now emits the byte-order reversal like SLJIT | BE5's AsmJIT half, and `BED4`'s AsmJIT half | `[asmjit][ujit]` pins the ledger (no raw instruction, no `Inst::kId*`, no `asmjit::x86` spelling, no sub-register view, no `host()` call); `[byteorder]` pins the conversion: under a claimed big-endian host the `Strict` body is larger than the native one, the `Nonconformant` body is identical, and the round-trip still answers. `[asmjit]` 1,909 / `[parity]` 2,684 / `[spec]` 514 and the full suite 23,300 assertions green |
| `BE-5` | A runner: `qemu-system-s390x` (whole system) or `qemu-user-static` plus an s390x rootfs; tier 3 of §8 wired to it, and `BE7`'s vector convention pinned while it runs | BE6, BE7 | the `[spec]` run is green on the runner |
| `BE-6` | Flip the target matrix to *provisional*, state per-backend support, re-stamp `../ABI.md` | BE6 | `ABI.md` has a big-endian row with a runner behind it |

**Why `BE-2` did not ship with `BE-1`.** `BE-1` decides, `BE-2` advertises, `BE-3`
honours. A `RuntimeConfig` field no backend reads is an option that silently does
nothing: the embedder sets `nonconformant`, sees no change, and cannot tell
whether the engine is honouring it or ignoring it. The switch landed in the
commit that first changed emitted code, so “set the switch” and “the switch does
something” are one release.

**Why `BE-5` now follows `BE-7`.** A runner is the only thing that can turn any
of this into a support claim, but it is also the only work here that cannot be
validated on the development host at all — while `BE-7` is ordinary code that
`[asmjit]` checks. Sequencing the unverifiable last keeps a red runner from
blocking merges it cannot inform.

## 8. Indicative test survey

Three tiers, and an honest statement of what each can prove.

| Tier | Runs on | Proves | Today |
|---|---|---|---|
| 1 — the decision | any host | the policy matrix, the evidence extraction, the primitive vectors, the parsing and the configuration key; the whole big-endian decision table via `decideByteOrderFor(…, ByteOrder::Big)` | `[byteorder]`: 13 cases / 161 assertions, green |
| 2 — the no-op property | any host | that on a little-endian host every policy behaves identically, that the three policies compile one fixture to the same code size, and that the two backends which cannot convert refuse when the host order is claimed as big-endian | measured; the refusal is executed through `WASMJIT_BYTE_ORDER_HOST` |
| 2.5 — the emission | any host | that the conversion is *emitted* when the decision asks for it: the claimed-host body is larger and the value round-trips | measured — see the limitation below |
| 3 — end to end | a big-endian host only | that a backend actually converts, and that the guest sees the order the specification requires | `endianness` and `endianness64` (69 assertions each, default set) plus `float_memory` are the tripwire; nothing runs them on BE yet |

**How to get a runner.** There is no big-endian image on the common forges, so the
two realistic routes are `qemu-system-s390x` (whole system, correct but slow) and
`qemu-user-static` with a Debian s390x chroot, which can build and run the test
binary itself: the core needs a C++ compiler, libffi and the JIT libraries, and
`Test/` is the only part that wants Qt.

**What tier 2.5 cannot prove, and why it exists anyway.** Adding a byte reversal
to a little-endian CPU's loads and stores does **not** reproduce big-endian
memory: the storage order stays little-endian, so a `store8`/`load` pair observed
byte-wise would disagree with a real port. The claim therefore evidences the half
that review used to carry alone — that the backend *emits* the reversal, and that
the policy decides it — and leaves the semantics to tier 3. It also cannot
compare two bodies byte for byte: x86-64 spells a helper call as a RIP-relative
displacement, so the same instruction encodes differently in two code blocks.
Code *size* is the address-independent measure, which is why `BED5` is stated as
a size equality and the conversion as a size increase.

**What tiers 1 and 2 are worth.** They pin everything a little-endian host *can*
pin — that the policy is inert where it must be, that the decision matches the
intended table, that the primitives are correct, that a backend which cannot
convert refuses — and they cannot pin that the guest sees the specification's
order. That is tier 3's job, so `BE-3` remains justified by the emission evidence
above plus review against the spec scripts rather than by execution.

## 9. References

- *WebAssembly Core Specification* 3.0, §4.3.3 “Bytes”:
  <https://webassembly.github.io/spec/core/exec/numerics.html>
- *WebAssembly Core Specification* 3.0, “Binary Format — Values”, the float rule:
  <https://webassembly.github.io/spec/core/binary/values.html>
- *Component Model*, “Canonical ABI”:
  <https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md>
- AsmJIT, “Universal JIT” (`BE-7`):
  <https://asmjit.com/doc/group__asmjit__ujit.html>
- In-repo: [`../ABI.md`](../ABI.md) “The C ABI Contract” and “GC Object and Heap
  ABI”, [`../SAFETY.md`](../SAFETY.md), [`../TESTING.md`](../TESTING.md) (the
  byte-order tripwire), [`GC_EXTENSIONS.md`](GC_EXTENSIONS.md) `GXD7`,
  [`../WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md);
  sources `WasmBase/WasmByteOrder.hpp` and `.cpp`,
  `WasmBase/WasmRuntime.hpp`, `WasmBase/WasmConfiguration.cpp`,
  `WasmBase/WasmModuleInstance.cpp` (`GcRefcount`,
  `copySegmentIntoElements`), `WasmBase/WasmGcHeader.hpp`
  (`readWord`/`writeWord`), `WasmBase/wasm_atomic.cpp`,
  `WasmBase/WasmTable.cpp` (a pointer slot is not a guest word),
  `Sljit/SljitOpcodeDispatcher.cpp` (`emitGuestSwap`,
  `emitHeaderWordLoad`), `Sljit/SljitModuleCompiler.cpp`,
  `LibJit/LibjitModuleCompiler.cpp` (refuses),
  `Asmjit/AsmjitModuleCompiler.cpp` (converts, `BE-7`),
  `extern/sljit/sljit_src/sljitNativeS390X.c`
  (`lrv`/`lrvg`/`strvg`), `Test/ByteOrderTests.cpp`,
  `Test/wasm_wat/byte_order_evidence_*.wat`,
  `Test/wasm_wat/byte_order_roundtrip.wat`, `Test/wast_supported.txt`.

