# Big-Endian Hosts {#prop_big_endian}

*Implementation Notes, Spec Status, and Deliberate Deviations*

Status: design note / implementation reference (revision 1 — 2026-11-17: the
policy layer exists and is tested; every backend still emits native-order
accesses, and no big-endian runner exists, so the target stays **unsupported**
and this note is what a port follows, milestone by milestone).
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

**Snapshot (2026-11-17, working tree on `dfabe47`):** the *decision* layer exists
and is tested; the *conversion* does not exist anywhere.

| Piece | Where | State |
|---|---|---|
| The policy: `Strict` / `Adaptive` / `Nonconformant` | `WasmBase/WasmByteOrder.hpp` | implemented, internal header (see `BE-2`) |
| Policy names, case-insensitive parsing, `WASMJIT_BYTE_ORDER` | `WasmBase/WasmByteOrder.cpp` | implemented; an unknown name stays `Strict` |
| Conversion primitives (`byteSwap` 16/32/64, `byteSwapIf`) | same header | implemented as value-level functions; their involution and known-answer vectors are tested |
| Module evidence (`ByteOrderEvidence`, `byteOrderEvidence`) | same header/cpp | implemented: memories, import/export, shared flag, active and passive data segments |
| The decision table (`decideByteOrderFor`) | same cpp | implemented for **both** host orders, so the big-endian matrix runs on a little-endian build |
| Tests | `Test/ByteOrderTests.cpp` `[byteorder]` | 6 cases / 104 assertions; four fixtures `Test/wasm_wat/byte_order_evidence_*.wat` |
| Byte-order tripwire in the default suite | `Test/wast_supported.txt` | `endianness` and `endianness64` promoted (69 assertions each); `float_memory` was already there |
| Conversion in emitted accesses | — | **absent**: a big-endian build would be silently wrong, which is why the target is *unsupported* rather than *provisional* |
| Runtime switch reaching the backends | — | **absent on purpose** (`BE-2`): an option that changes nothing is a lie |

The engine's own decoding is already order-free: every reader is an
`Elv::Io::DataStream<Ev::Util::Endian::Little>`, and the canonical ABI's
`readUInt`/`writeUInt` assemble bytes by shift, so the *host-side* half of a port
is mostly written already. What is missing is the half the backends emit.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| BE1 | **No emitted access converts.** Both backends emit a native load/store, so a big-endian build reads every multi-byte value reversed | `LibJit/LibjitOpcodeDispatcher.cpp:5251-5280` (`jit_insn_load_relative(…, jit_type_int)`), `Sljit/SljitOpcodeDispatcher.cpp:10330-10357` (`emitMemoryLoad(…, SLJIT_MOV_U32, …)`) | Blocker for any BE target |
| BE2 | **No runtime switch.** `decideByteOrder` is reachable only from tests, so no embedder can ask for a policy and no backend is handed one | `WasmByteOrder.hpp` is in `WASMJIT_INTERNAL_HEADERS`; `RuntimeConfig` has no field | High (deliberate, see §7) |
| BE3 | **Atomics remain native.** The helpers type-pun the guest address, so an RMW swaps on BE | `WasmBase/wasm_atomic.cpp:84,90,96,118` | High |
| BE4 | **The GC header promise is unqualified.** `ABI.md` promises little-endian to embedders while the header is stored natively | `../ABI.md` “GC Object and Heap ABI”; `GXD7` | Medium (an ABI decision at port time) |
| BE5 | **Two of three backends cannot host the port.** The LibJIT this tree links has no big-endian target and no byte-reversal op; `Asmjit/` is `asmjit::x86` end to end | `jit-config.h` (interp/alpha/arm/x86/x86-64); `Asmjit/AsmjitModuleCompiler.cpp` | Blocker; the port is SLJIT-only |
| BE6 | **No runner.** Nothing in this tree has ever executed on a big-endian host, and tier 3 of §8 needs one | — | Blocker for “supported” |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| BED1 | **`Nonconformant`: never convert, on a big-endian host** (implemented, off by default). | It is *not* safe under the specification, and it is documented as a non-conformance rather than a shortcut: data segments, byte-level guest code, `f32`/`i32` punning and any second reader of the memory all break. It is legitimate only in a closed world where the host knows every guest and every byte in every memory is produced and consumed at whole width. | Removes the conversion from a host that has measured it as noise. |
| BED2 | **`Adaptive`: skip when the evidence is closed** (implemented). | §3 gives us no freedom here — the relaxation is a heuristic, not a proof, because no static check sees a guest that writes four bytes with `i32.store8` and reads them back with `i32.load`. The bar is deliberately high (private, unshared memory; no active or passive data segment) and the verdict is reported in `ByteOrderDecision::reason` so a host can log it. | Conformance by default, speed where the module demonstrates it is safe. |
| BED3 | **Keep the GC header little-endian on a big-endian port** (the conservative reading of `GXD7`). | Nothing in the spec observes it, and keeping it preserves one heap layout everywhere, so `gcStructFieldOffset` readers and cross-host artifacts still agree. | No ABI fork between hosts. |
| BED4 | **A backend that cannot convert must refuse to compile, not degrade.** | Silent corruption is the failure class [`../SAFETY.md`](../SAFETY.md) exists to avoid; a refusal is a diagnostic. | A BE build fails visibly on LibJIT and AsmJIT instead of misbehaving. |
| BED5 | **The layer must cost nothing on a little-endian host.** | `byteSwapIf(false, v)` is `v`, and the conversion sits behind a compile-time `if constexpr` on the host order, so today's code generation is unchanged. | The policy cannot regress the only platform that works. |

Guardrail: `Strict` is the default on every path (configuration file, environment,
embedder default), a name the engine does not recognise resolves to `Strict`, and
`Nonconformant`/`Adaptive` may only relax **linear-memory access conversion** —
never the canonical ABI's byte order where the host is the writer and the guest is
third-party, and never handle or resource semantics.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| `BE-1` | Policy layer: enum, names, environment, primitives, evidence, decision table, `[byteorder]` tests | — (done) | 6 cases / 104 assertions green; the BE matrix runs on LE |
| `BE-2` | Plumb the policy into `RuntimeConfig` and the configuration-file reader, report the decision on the instance, promote `WasmByteOrder.hpp` to the public tier | BE2 | an embedder can set `WASMJIT_BYTE_ORDER=adaptive` and read back what was applied |
| `BE-3` | Emit the conversion: one helper per backend around load/store (scalar widths, `f32`/`f64`, SIMD load/store); SLJIT first, using `SLJIT_REV*`, which s390x compiles to one instruction | BE1, and BE5's LibJIT/AsmJIT half by refusal | a fixture that stores with `store8`s and loads a word returns the same value on a BE host |
| `BE-4` | Converted atomic RMW in `wasm_atomic.cpp` (swap inside the helper, which already receives the width) | BE3 | atomics pass the spec scripts on a BE host |
| `BE-5` | A runner: `qemu-system-s390x` (whole system) or `qemu-user-static` plus an s390x rootfs; tier 3 of §8 wired to it | BE6 | the `[spec]` run is green on the runner |
| `BE-6` | Flip the target matrix to *provisional*, state per-backend support, re-stamp `../ABI.md` | BE4 (with a decision), BE6 | `ABI.md` has a big-endian row with a runner behind it |
| `BE-7` | **Deferred:** replace `Asmjit/`'s direct `asmjit::x86` emission with AsmJIT's Universal JIT (<https://asmjit.com/doc/group__asmjit__ujit.html>) so one backend can target other ISAs — the precondition for any AsmJIT big-endian path, since upstream supports big-endian AArch64 | — | recorded here, not scheduled |

**Why `BE-2` did not ship with `BE-1`.** `BE-1` decides, `BE-2` advertises, `BE-3`
honours. A `RuntimeConfig` field no backend reads is an option that silently does
nothing: the embedder sets `nonconformant`, sees no change, and cannot tell
whether the engine is honouring it or ignoring it. The switch lands in the commit
that first changes emitted code, so “set the switch” and “the switch does
something” are one release.


## 8. Indicative test survey

Three tiers, and an honest statement of what each can prove.

| Tier | Runs on | Proves | Today |
|---|---|---|---|
| 1 — the decision | any host | the policy matrix, the evidence extraction, the primitive vectors and the parsing; the whole big-endian decision table via `decideByteOrderFor(…, ByteOrder::Big)` | `[byteorder]`: 6 cases / 104 assertions, green |
| 2 — the no-op property | any host | on a little-endian host every policy behaves identically and the conversion compiles away; after `BE-3`, that the emitted code for a fixture is byte-identical under `Strict` and `Nonconformant` | the property is asserted; the code-identity half needs `BE-3` |
| 3 — end to end | a big-endian host only | that a backend actually converts, and that the guest sees the order the specification requires | `endianness` and `endianness64` (69 assertions each, default set) plus `float_memory` are the tripwire; nothing runs them on BE yet |

**How to get a runner.** There is no big-endian image on the common forges, so the
two realistic routes are `qemu-system-s390x` (whole system, correct but slow) and
`qemu-user-static` with a Debian s390x chroot, which can build and run the test
binary itself: the core needs a C++ compiler, libffi and the JIT libraries, and
`Test/` is the only part that wants Qt.

**What tiers 1 and 2 are worth.** They pin everything a little-endian host *can*
pin — that the policy is inert where it must be, that the decision matches the
intended table, that the primitives are correct — and they cannot pin that a
backend emits the conversion. That is tier 3's job, so until `BE-5` exists, `BE-3`
is justified by review against `[byteorder]` and the spec scripts rather than by
execution.

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
  `LibJit/LibjitOpcodeDispatcher.cpp`, `Sljit/SljitOpcodeDispatcher.cpp`,
  `WasmBase/wasm_atomic.cpp`, `WasmBase/WasmGcHeader.hpp`,
  `extern/sljit/sljit_src/sljitNativeS390X.c` (`lrv`/`lrvg`/`strvg`),
  `Test/ByteOrderTests.cpp`, `Test/wasm_wat/byte_order_evidence_*.wat`,
  `Test/wast_supported.txt`.

