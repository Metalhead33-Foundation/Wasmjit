# Unmodelled Value Types — Rejected at Load {#doc_unmodelled_value_types}

Status: investigation note (revision 3 — root cause confirmed empirically; **fix
D implemented** — unmodelled value types are rejected at load. Renamed from
`NULL_JIT_VALUE_DIAGNOSIS.md` so that the core's citations no longer carry a
backend name; the diagnosis itself is unchanged).
Audience: anyone touching LibJIT codegen, the reference-counting ownership work
([`proposals/GC.md`](proposals/GC.md) RC-1/RC-1b), or adding a new value type.

This note records the investigation into the crash that keeps
`kStackOwnsRefs` (`LibJit/LibjitOpcodeDispatcher.cpp`) disabled. It is a
diagnosis only — the code is back in its known-good state.

## 1. Summary

`pushValue` receives a **null `jit_value_t`** whenever a module reads a value
whose type the engine does not model (`exnref` / `nullexnref`). The engine lowers
such a type to `jit_type_void`, and a LibJIT load of that type returns `nullptr`
instead of a value. The null was harmless while `pushValue` merely stored it; the
operand-stack-ownership work made `pushValue` *inspect* every pushed value
(`jit_value_get_type(v)`), which dereferences the null pointer → SIGSEGV.

It is **not** about pointer/tagged types, which were the first two hypotheses.

## 2. Symptom

```
[spec] ref_null: terminated by signal 11 after 0 passing commands
```

Only `ref_null.wast` is affected; every other GC/reference script matches its
baseline. The crash happens during **instantiation** (function compilation), not
at invocation — so the module never gets a chance to run.

## 3. Evidence — backtrace

The spec suite `fork()`s a child per script and runs it in-process
(`Test/SpecSuite.cpp`, `runIsolated`), so the child must be followed in gdb:

```
gdb -batch -ex 'set follow-fork-mode child' -ex run -ex 'bt' \
    --args ./WasmJit 'spec: ref_null'
```

```
Thread 2.1 received signal SIGSEGV
#0  jit_value_get_type (value=0x0)            jit-value.c:758
#1  isRefJitValue (v=0x0)                     LibjitOpcodeDispatcher.cpp:502
#2  OpcodeDispatcher::pushValue (v=0x0)       LibjitOpcodeDispatcher.cpp:586
#3  OpcodeDispatcher::dispatchGlobalGet (arg=2)  LibjitOpcodeDispatcher.cpp:1945
#4  OpcodeDispatcher::dispatchOpcode (GlobalGet) WasmOpcodeDispatcher.cpp:796
#6  ModuleCompiler::compileFunction (funcTypeIdx=5) LibjitModuleCompiler.cpp:106
#8  ModuleInstantiator::instantiate            WasmModuleInstance.cpp:42
#9  ScriptRunner::onModule                     WastScript.cpp:506
```

The failing instruction is `global.get 2` in `ref_null.wast`'s second module —
`(func (export "exnref") (result exnref) (global.get $nullexn))`, where
`$nullexn` is `(global $nullexn nullexnref (ref.null noexn))`.

## 4. Confirmation — a temporary probe

A throwaway probe in `dispatchGlobalGet` printed, right before the crash:

```
[gcprobe] global.get 2: opcode=-12 lowered to jit_type_void
[gcprobe] global.get 2: load returned null jit_value_t
```

`opcode = -12 = -0x0C = 0x74` is **`nullexnref`** — a value type
`WASM::ValueTypeCode` does not define. The probe was removed again afterwards.

## 5. Root-cause chain

1. **Decode.** `WasmBase/WasmType.hpp` `ValueTypeCode` covers the 2.0/3.0 GC,
   reference and stringref types, but **not the exception-handling `exnref`
   (`0x69`) / `nullexnref` (`0x74`) opcodes**. A module that uses them decodes
   the byte into a `ValueTypeCode` the enum does not name.
2. **Lowering.** `LibJitTypeTranslator::translatePrimitiveType` ends in
   `default: return jit_type_void;`. An unnamed opcode therefore lowers to
   **`jit_type_void`** — a valid LibJIT type, but not a *value* type.
3. **Emission.** `OpcodeDispatcher::dispatchGlobalGet` does
   `jit_insn_load_relative(function, slot, 0, jt)` with `jt == jit_type_void`;
   LibJIT returns **`nullptr`** for a load of type `void` instead of a value.
4. **Consumer.** `pushValue(nullptr)`. Before the ownership work this was
   `valueStack.push_back(nullptr)` — harmless, because nothing ever looked at
   the value. With `kStackOwnsRefs` enabled it calls
   `isRefJitValue(v)` → `jit_value_get_type(nullptr)` → dereference null →
   **SIGSEGV**.

So the trigger is a **null JIT value for an unmodelled type**, not a type
mismatch.

## 6. Why it stayed hidden

- At the time the engine had **no validating front-end** (`docs/proposals/GC.md`
  D6, since superseded): it compiled every function it decoded, including ones
  whose types it could not represent. `Module::validate()` now rejects such a
  module at load ([`VALIDATION.md`](VALIDATION.md)), which is why the fix below
  is a load-time rejection.
- Nothing *observed* the value. A null `jit_value_t` was pushed, popped and
  stored as an opaque handle, so the engine produced a broken function silently
  instead of failing. The ownership work is the first consumer that inspects
  every pushed value, so it exposed a latent bug rather than introducing one.

## 7. Blast radius

`global.get` is just the site that happened to be first. The same null can be
produced anywhere an unmodelled type is lowered to a value:

- `jitTypeForValueType` / `translateType` return `jit_type_void` for any
  `ValueTypeCode` the enum lacks, so **locals** (`ModuleCompiler::compileFunction`
  creates a local per declared type), **block params/results**
  (`createSlotsForTypes`), **function params/results**
  (`translateFunctionSignature`) and `ref.null exn`
  (`zeroConstantForType(jit_type_void)`) are all reachable.
- Any LibJIT `jit_insn_load*` / `jit_value_create_nint_constant` with a non-value
  type can yield null, not only loads.
- Any *future* consumer that inspects a stack value — the RC-5 clone/drop, a
  type check, a validator — will hit the same null.
- **Now modelled:** the `stringref`-family codes are nullable host references
  (`docs/proposals/STRINGREF.md` STR-1/STR-3). They lower to a `void*` handle
  to a Store-owned `StringObject`, not to an absent value, so they no longer
  exercise this path.

## 8. Candidate fixes (evaluated; **D** is the one that shipped — §11)

| # | Fix | Cost | Trade-off |
|---|---|---|---|
| A | Treat `v == nullptr` as "not a reference" in `isRefJitValue` / `pushValue` / `emitDropValue` / `emitDropRefLocals`. | one guard, a few lines | Unblocks the ownership work immediately, but the engine still emits a silent null operand for an unmodelled type. |
| B | At each lowering site, if the type is `jit_type_void` where a value is expected, call the existing `notImplemented("...")` instead of emitting. | a handful of sites | Turns silent garbage into a clear compile-time error; needs an audit of the sites. |
| C | Change `translatePrimitiveType`'s `default` to `jit_type_void_ptr` (opaque word), as it already does for unknown *heap* types. | one line | Broadest/smallest diff and loads then yield a real value, but the engine would "run" unmodelled types generally, which is only correct for shapes like `ref.null exn`. |
| D | Reject unmodelled value types at decode/instantiate (a validating front end). | large | The right long-term answer, and what the suite's `assert_invalid` cases expect. **Implemented** (`isModeledValueTypeCode` + `Module::fromFile`); §11. |

Suggested order at the time: **A** (unblock RC-5 now) + **B** (a clear
diagnostic), with **C** or **D** as the principled follow-up. What actually
happened was **D** directly. Whichever is chosen has to re-run `ref_null` and the
rest of the reference/GC scripts; §11 records that run.

## 9. Reproduction recipe

```bash
# 1. the operand-stack ownership layer is `kStackOwnsRefs = true` now; to
# reproduce the original crash, flip it to false first
sed -i 's/kStackOwnsRefs = true/kStackOwnsRefs = false/' LibJit/LibjitOpcodeDispatcher.cpp
/usr/bin/env MAKE=/usr/bin/make /usr/bin/make -C build/Desktop-Debug -j16

# 2. follow the forked spec child and get the crash site
cd build/Desktop-Debug/Test
gdb -batch -ex 'set follow-fork-mode child' -ex run -ex 'bt' --args ./WasmJit 'spec: ref_null'
```

To see the lowering directly, add to `dispatchGlobalGet` (and remove after):

```cpp
if (jt == jit_type_void)
    std::fprintf(stderr, "global.get %u: opcode=%d -> jit_type_void\n",
                 (unsigned)arg, (int)gt.contentType.opcode);
jit_value_t loaded = jit_insn_load_relative(function, slot, 0, jt);
if (loaded == nullptr)
    std::fprintf(stderr, "global.get %u: load returned null\n", (unsigned)arg);
```

## 10. References

- [`proposals/GC.md`](proposals/GC.md) RC-1/RC-1b — the ownership model and the
  `kStackOwnsRefs` flag this note explains.
- `WasmBase/WasmType.hpp` — `ValueTypeCode` as it was during the diagnosis
  (`exnref`/`nullexnref` are modelled now; see the follow-up above).
- `LibJit/LibJitTypeTranslation.cpp` — `translatePrimitiveType` `default:
  return jit_type_void;`.
- `LibJit/LibjitOpcodeDispatcher.cpp` — `pushValue`, `isRefJitValue`,
  `dispatchGlobalGet`.
- [`SAFETY.md`](SAFETY.md) §2.4 (traps, not aborts), and
  [`VALIDATION.md`](VALIDATION.md) for the validating front end that later
  replaced `proposals/GC.md`'s D6 stance.
- LibJIT: `jit_insn_load_relative` with a `void` type returns null.

## 11. Resolution — fix D implemented

Fix **D** was chosen and implemented rather than A/B/C: the engine now refuses a
module that declares a value type it cannot model.

- `WASM::isModeledValueTypeCode(ValueTypeCode)` (`WasmBase/WasmType.hpp`) is the
  whitelist of codes the engine can lower and run. The `stringref` family is
  included as host-reference handles (`docs/proposals/STRINGREF.md`).
- `Module::validateValueTypes()` (`WasmBase/WasmModule.cpp`) walks the declared
  types, tables, globals, imports, element segments and function locals, and
  throws for any other code. It runs at the end of `Module::fromFile`, so the
  module is rejected at **load**.
- `OpcodeDispatcher::jitTypeForValueType` additionally throws (`notImplemented`)
  for an unmodelled code, which covers value types that only appear **inline in a
  function body** (e.g. a block type) and so are not visible at load.

Effect on `ref_null.wast`: instead of segfaulting it now fails with

```
malformed module: unsupported value type 0x69 (-23) in function type result #3
— this engine does not model it (docs/UNMODELLED_VALUE_TYPES.md)
```

(The formatter printed the enum's two's-complement byte at the time; it now
prints the signed-LEB wire byte, so the same code reads `0x69` rather than
`0xE9`. The same formatter also covers any future unassigned code, e.g.
`unsupported value type 0x7A (…)`; `stringref` itself is modelled now and
produces a real host handle — see `STRINGREF.md` STR-1/STR-3.)

At that point `ref_null` had to be dropped from `Test/wast_supported.txt`,
because the spec suite treats a refused module as a failed command. It is back in
the list now (see the follow-up below), and the inline note at the top of
`wast_supported.txt` records why it was once excluded.

With the null source removed, the operand-stack ownership layer
(`kStackOwnsRefs`) was **enabled** and the suite was green at that point (1446
assertions / 78 test cases; it has grown a lot since), with every GC/array/table
spec script at its baseline.

## Follow-up: the rejection is no longer triggered by `exnref` (EX-1)

The guard is still the right policy for a genuinely unmodelled type — a module
must be refused at load rather than compiled into a null `jit_value_t`. But the
particular type that triggered it was `exnref`/`nullexnref`, and those are
**modelled now** (`docs/proposals/EXCEPTIONS.md` EX-1):

- `ValueTypeCode::ExnRef` (0x69) and `NullExnRef` (0x74), with
  `AbstractHeapType::Exn`/`NoExn` and the bottom-type encodings corrected to the
  final 3.0 numbering (the old draft values collided with `exn` at 0x69);
- `LibJitTypeTranslator` lowers both to `void*`, and `isModeledValueTypeCode`
  accepts them, so `Module::fromFile` no longer refuses such a module;
- `ref_null.wast` runs **34 passed / 0 failed / 0 skipped** and is curated.
  (The harness now compares `(ref.null …)` *results* against a null pointer,
  which is what turned its 32 skips into real assertions.)

The failure mode itself — an unmodelled code lowering to `jit_type_void` and
pushing a null backend handle — is unchanged and still guarded; a future
unmodelled type will be rejected exactly as described above.
