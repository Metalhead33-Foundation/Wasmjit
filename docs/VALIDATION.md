# Validation {#doc_validation}

Status: design + status note, revision 3 (2026-11-03: the decoder sweep added
UTF-8 names, the core version check, section order/size, limits flags and
mutability; the GC aggregate and unreachable `select`/`br_table` rules landed,
and the all-scripts sweep now fails only the `linking*` store-lifetime cases).
Revision 2 (2026-11-01) absorbed `UNMODELLED_VALUE_TYPES.md` — the rule it
established is a validation rule, so it now lives here; the full crash
investigation is preserved in git history.
Audience: anyone touching the validating front end, or adding a value type.

`WasmBase/WasmValidator.cpp` is the validating front end. `Module::fromFile`
calls `validate()` after decoding, so a module is checked once and every
consumer (the JIT, the stub backend, an embedder) sees the same rejection.
`Module::validate()` is also public.

## Architecture

The validator is an `OpcodeDispatcher`. The existing dispatcher already reads
every instruction and immediate (including the multi-byte prefixes), so a rule
never re-parses bytes — it is just one `dispatch*` override. `readCode` drives
the function body; the validator keeps an operand type stack and a control-frame
stack, and throws `std::runtime_error` on the first failed rule.

## What is checked

| Layer | Rules |
| --- | --- |
| Module | type indices name function types; function/code count consistency; data-count consistency; start function `[] -> []`; duplicate export names and export index bounds; memory/table limits (min ≤ max, unknown limits flags, a byte-budget page cap — 65536 pages at 64 KiB or 2^32 at one byte — shared memories declare a max, table32 fits in 32 bits); tag types name function types and may carry a result signature (control tags, `STACK_SWITCHING.md` SS-2); `throw`/`try_table` consume the parameter signature and ignore the results, and tag attributes must be 0; global/table initialisers and active element/data offsets (typed constant expressions); element/table type agreement, element item expressions and table initialiser typing; selected functions in element segments; empty modules |
| Control | `block`/`loop`/`if`/`else`/`end` with typed params/results and exact stack height; `br`/`br_if`/`br_table` (operand values checked against every target; the unreachable stack is polymorphic)/`return`; `call`/`call_indirect`/`call_ref`; tail calls including result-type matching; `try_table` catch labels (payload, `exnref`, arity); `br_on_null`/`br_on_non_null`/`br_on_cast`/`br_on_cast_fail` |
| Numeric | i32/i64/f32/f64 unary, binary, comparison, conversion, sign-extension and saturating forms, with exact operand/result types |
| Memory | declared index type (`i32`/`i64`), alignment ≤ natural, 32-bit offset range, loads/stores, `memory.size/grow/init/copy/fill` |
| Table | table64 index type, `get/set/grow/size/fill/copy/init`; table initialiser typing; `table.init` element-segment/table type match |
| SIMD | 194 arithmetic/comparison/conversion shapes taken from `wasm_simd_ops.inc`, plus const/bitwise/splat/bitmask, loads/stores, lanes/shuffle/swizzle, and the relaxed ternary forms (madd/nmadd, laneselect, dot-add) |
| Atomics | loads/stores/RMW/cmpxchg/wait/notify with access-size alignment and operand types |
| References | `ref.null`/`ref.func`/`ref.is_null`/`ref.eq`/`ref.as_non_null`; `ref.func x` requires `x` to be declared outside function bodies/start; `call_ref $t` requires `(ref null $t)`; `ref.test`/`ref.cast` (both nullability forms) require the operand to be a reference in the target's hierarchy; continuation references are not castable (`cont`/`nocont`/concrete `cont $ft` targets are rejected, `STACK_SWITCHING.md` SS-1); full subtyping (nullability, func/extern/exn/any hierarchies, concrete-type subtyping and canonical structural type equivalence); untyped `select` is numeric/vector only; definite initialization of non-defaultable locals |

## Conservative policy

An opcode without a rule (parts of the GC aggregate/array surface) sets
`typeTainted`: the rest of the body is still scanned for immediate checks, but
the operand stack is no longer trusted and no type error is reported there. The
**multibyte array memarg** and the three **More Array Constructors** do have
rules: a bit-5 memarg validates the array type, numeric/packed/vector element,
alignment/offset and (for stores) mutability, and `array.new_array` /
`array.new_memory` / `array.new_table` validate the element relation and the
source array/memory/table index (`GC_EXTENSIONS.md` GX-1/GX-2). The older
one-element `array.*`/`struct.*` forms remain tainted. A `try_table` **with
catches** is tainted after its catch labels are checked,
because a catch injects its payload into an outer label, which the linear stack
does not model. A validator must never reject a module it could not check — the
cost is coverage, measured below, not correctness.

`ValidationOptions::relaxedDeadCode` (`WasmValidation.hpp`) opts into the
Relaxed Dead-Code Validation algorithm for one load
(`Module::fromFile(file, options)` or `Module::validate(options)`): after
`unreachable`, pushes and pops are skipped together with every pop-dependent
check, while immediate widths and index bounds still apply. The default stays
conservative (the curated suite still encodes the historical result for three
dead `return_call*` cases, and the proposal permits a stricter engine), so the
negative-module results below do not change.

`ValidationOptions::profile` additionally gates the language profiles
([`proposals/PROFILES.md`](proposals/PROFILES.md) PR-2/PR-4): the scalar profile
rejects `v128` declarations and `0xFD` instructions, and the deterministic
profile rejects shared constructs, atomics and relaxed SIMD. These are
restriction-only checks; they never change what an accepted module does.

## Unmodelled value types are rejected at load

A module may declare a value type this engine cannot lower. The rule is: **refuse
it at load, never compile it into a null backend value.** The reason is a crash
that is worth one paragraph even though the specific type that caused it is now
supported.

A value type the engine does not model was lowered to `jit_type_void`; a backend
load of that type returns a **null** value handle instead of a value. Storing the
null was harmless, but once operand-stack ownership made `pushValue` *inspect*
every pushed value, the null was dereferenced — a SIGSEGV during instantiation,
not at invocation, so the module never ran. That is the whole failure mode, and it
is generic: any unmodelled code reproduces it.

The guard has three parts, all shipped:

| Part | Where | Covers |
|---|---|---|
| `WASM::isModeledValueTypeCode(ValueTypeCode)` | `WasmBase/WasmType.hpp` | the whitelist of codes the engine can lower and run (`stringref` included, as host-reference handles) |
| `Module::validateValueTypes()` | `WasmBase/WasmModule.cpp`, at the end of `Module::fromFile` | the *declared* surface: types, tables, globals, imports, element segments, function locals |
| `OpcodeDispatcher::jitTypeForValueType` throws `notImplemented` | `WasmBase/WasmOpcodeDispatcher.cpp` | value types that appear **only inline** in a body (e.g. a block type), which load-time validation cannot see |

A rejected module fails at **load** with a message naming the code, rather than
segfaulting during instantiation:

```
malformed module: unsupported value type 0x69 (-23) in function type result #3
— this engine does not model it
```

Two notes on what this does and does not mean:

- **The rule is permanent; its original trigger is not.** `exnref`/`nullexnref`
  were the type that exposed it and are modelled now
  ([`proposals/EXCEPTIONS.md`](proposals/EXCEPTIONS.md) EX-1), so the guard is
  policy for a future unmodelled type rather than a live rejection.
- **A refused module is a *failed* spec command**, which is why
  `Test/wast_supported.txt` records why a script was once excluded: the harness
  cannot distinguish "correctly refused" from "failed" for a valid module.

History: this was investigated as `NULL_JIT_VALUE_DIAGNOSIS.md` →
`UNMODELLED_VALUE_TYPES.md`, and the full backtrace, the gdb recipe and the four
candidate fixes are in git history (`git log --follow -- docs/VALIDATION.md`). The
outcome is the three-part guard above; fix "reject at load" was chosen over the
three alternatives (teach the backend a null-tolerant push, model every type, or
leave the feature flag off) because the module is untrusted input and a null
handle is a crash waiting for the next caller.

**Known documentation debt:** ten code comments (and one `notImplemented()`
message) still cite `docs/UNMODELLED_VALUE_TYPES.md`. They are rationale
pointers, not links, and renaming them is a comment-only change to `.cpp`/`.hpp`
files — deliberately left out of the documentation-only pass that moved the
content here.

## Measured coverage

`WASM_SPEC_VALIDATE=1` on the curated set (**snapshot 2026-11-03, working tree
on `cf2c6cf`**):

| Kind | Binary cases | Handled | Accepted |
| --- | ---: | ---: | ---: |
| `assert_invalid` | 2211 | **2209** rejected | 2 (`tag.wast` known failure) |
| `assert_malformed` | 17 | **17** rejected | 0 |
| `assert_unlinkable` | 127 | **127** rejected | 0 |
| `assert_uninstantiable` | 27 | **27** (26 rejected + 1 start trap) | 0 |
| **total** | **2382** | **2380** | **2 known** |

The two accepted modules are `tag.wast`'s `(tag (result i32))` cases. They are
the exception proposal's stricter rule; the engine accepts result-bearing tags
as stack-switching control tags (`STACK_SWITCHING.md` SS-2), so the conflict is
recorded in `Test/spec_known_failures.txt` and [`SCOPE.md`](SCOPE.md) §6 rather
than hidden.

The **all-scripts** sweep is the wider measure, because `[spec]` also runs the
hidden scripts. At the same snapshot it reports **254 of 257 scripts green**
(`test cases: 257 | 254 passed | 3 failed`). The three failures are `linking`,
`linking0` and `linking3`: they require a failed instantiation to retain the
failed module's function instances, the `StoreOwnedFunction` boundary in
[`SCOPE.md`](SCOPE.md) §6. The decoder/validator cases the sweep used to expose
are green: `binary` (127 malformed cases), the three `utf8-*` scripts (176 each),
`binary-gc`, `binary-leb128`, `struct`/`array`/`array_*` (16 invalid modules) and
`unreached-invalid` (121 cases). `comments` (text format) and `instance`
(module-linking) skip their dependent commands; 946 text-format commands remain
skipped because the loader is binary-only.

The default suite is green; its *current* tally is owned by
[`TESTING.md`](TESTING.md) (and [`FRESHNESS.md`](FRESHNESS.md) §8 prints it), not
repeated here. The curated list holds 189 of the 257 scripts, 61 of them
re-curated when the validator landed, plus `start.wast`.

The `atomic.wait`-on-unshared check is deliberately **not** enforced: the
curated `atomic.wast` treats it as valid (`atomic.notify` likewise), so `T5`
stays a documented single-agent deviation rather than a validator rule.

## Finding

`Module` did not initialise `dataSegmentCount`/`hasDataCount`; the data-count
rule read garbage and rejected valid modules until the constructor was fixed.
The bug was latent because nothing read those fields before.

The active-segment path widened a 32-bit offset by sign extension (`i32.const
-1` became 0xFFFF_FFFF_FFFF_FFFF) and added the length before the bounds check,
so an out-of-bounds `data`/`elem` segment wrapped to a small index and wrote
past the backing array. `elem.wast` exposed it as a double free at teardown.
Offsets are now zero-extended for 32-bit memories/tables (and `i64` for 64-bit
ones), and the bounds check subtracts instead of adding.

## Next

- ~~Type the remaining GC aggregate/array forms~~ — done (2026-11-03):
  `struct.get/set`, `array.get/set/fill/copy/init_data/init_elem` type their
  operands, enforce mutability and element-type agreement, and preserve a
  reference's heap type; `array.new_data`/`array.new_elem` are no longer
  const-eligible. `struct`, `array` and the five `array_*` scripts are green.
- ~~The unreachable `select`/`br_table` holes~~ — done (2026-11-03): a
  `Void` bottom marker keeps the stack height honest, `select` records its
  bottom result, and `br_table` checks target arity and concrete values above
  the frame height. `unreached-invalid` is 121/0 and `unreached-valid` is green.
  The proposal's full relaxed-dead-code algorithm remains an opt-in
  (`ValidationOptions::relaxedDeadCode`), not the default.
- ~~The “declared function reference” rule~~ — done: `dispatchRefFunc` checks the
  module's `C.refs` set (exports, globals, tables and element segments).
- ~~The remaining strict failures~~ — done: the 2026-11-03 decoder sweep added
  UTF-8 name validation, the core version check, the section-id range,
  section order/duplicate/exact-size checks, the pre-type import section,
  single-byte limits flags and strict field/element mutability. The only
  remaining all-scripts failures are `linking*`, which are the store-lifetime
  boundary, not decoder rules.
- ~~Add curated `assert_uninstantiable` coverage~~ — done: `start.wast` and the
  `elem`/`data` segment cases are verified under `WASM_SPEC_VALIDATE=1`.
- The `tag.wast` result-signature conflict is recorded in
  `Test/spec_known_failures.txt`; it re-opens if upstream provides a
  discriminator between control tags and exception tags (or the engine drops
  the SS-2 form).
