# `S1` — C# through `componentize-dotnet` {#lf4_s1}

Status: `LF-4` arm report (revision 2 — 2026-11-16). Arm `S1` of
[`LF4_INDEX.md`](LF4_INDEX.md); toolchain pinned in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2. **Verdict: 🟨 Yellow** — the toolchain
produces a valid, self-describing component with the guide's own steps. The
engine used to reject it at load (`GAP-20`); with `GAP-20`, `GAP-21` and `GAP-22`
closed it loads, validates and instantiates as far as the host-import seam, where
the embedder-owned WASI 0.2 shim is the remaining blocker.

Audience: anyone planning to ship C# components to WasmJit, and whoever
sequences `LF-3`.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was run, in which order? |
| §3 | Where the current templates differ from the guide |
| §4 | The artifact: size, hash, WIT, canon definitions |
| §5 | What the engine did, and why |
| §6 | What it would take to run it |
| §7 | What this arm does not answer |
| §8 | References |

## 2. What was run

The upstream C# page's steps, verbatim where they still apply:

```
dotnet new install BytecodeAlliance.Componentize.DotNet.Templates
dotnet new componentize.wasi.cli -o adder
cd adder
```

`wit/component.wit` — the guide's two-world package, unchanged:

```wit
package docs:adder@0.1.0;

interface add {
    add: func(x: u32, y: u32) -> u32;
}

world example {
    export add;
}

world hostapp {
    import add;
}
```

The project file gets the `Wit` item and becomes a library:

```xml
<OutputType>Library</OutputType>
...
<ItemGroup>
  <Wit Update="wit/component.wit" World="example" />
</ItemGroup>
```

`Program.cs` is removed, `Component.cs` implements the generated interface, and
`dotnet build -c Release` produces
`bin/Release/net10.0/wasi-wasm/native/adder.wasm`.

The arm **stops at the library component**: the guide's second half (a `hostapp`
command component composed with `wac plug`) is a *component-to-component* test,
not an engine test, and the engine has no WASI to run the composed command. That
half is not part of this arm's bar.

## 3. Where the current templates differ from the guide

Two drifts, both from the pinned versions rather than from the engine:

| Guide says | This toolchain generates / needs |
|---|---|
| `public class AddImpl : IAdd` | `public class AddExportsImpl : IAddExports`, namespace `ExampleWorld.wit.Exports.docs.adder.v0_1_0` (wit-bindgen 0.58.0) |
| `World="example"` in the `Wit` item | unchanged, but the item must be inside the single `<Project>` root: appending it after `</Project>` fails with `MSB4025` |

The generated interop calls `AddExportsImpl.Add(...)` by name, so the class name
is not a free choice; the build error names the expected symbol, which is how the
name was found.

## 4. The artifact

| Property | Value |
|---|---|
| Size | 2,227,102 bytes |
| SHA-256 | `9e60c4b176ce290a4e0f186c0da3573da910b862d24cc35cd47d604eb9b03938` |
| `wasm-tools validate --features all` | exit 0 |
| Export | `docs:adder/add@0.1.0` → `add: func(x: u32, y: u32) -> u32` |
| Canon definitions | one `canon lift` for the export, 25 `canon lower`s for the WASI imports, 6 resource drops |

The import list is the NativeAOT runtime's, not the adder's: **18 WASI 0.2.6
interfaces** — `wasi:io/{poll,error,streams}`, `wasi:clocks/{monotonic,wall}`,
`wasi:cli/{environment,exit,stdin,stdout,stderr,terminal-*}`,
`wasi:filesystem/{types,preopens}`, `wasi:random/random`. A C# component in this
configuration is a whole managed runtime in a component, and it says so in its
own WIT.

## 5. What the engine did

**First run, commit `6164c53`:**

```
status=error: component validation: canonical option `memory` is required
```

That load failure was `GAP-20` ([`LF4_INDEX.md`](LF4_INDEX.md) §5.1): the
validator's `flatSlots` counted a 64-bit scalar as two flat slots, and the
NativeAOT component's WASI imports include `wasi:clocks`
(`now: func () -> instant`, a `u64`), so a lowering that is valid without a
`memory` option tripped the rule. `wasm-tools` accepted the same bytes.

**Second run, commit `1ec91f8`**, after `GAP-20`, `GAP-21` and `GAP-22` were
closed ([`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §4.9):

```
loaded path=…/publish/adder.wasm backend=libjit size=2227102
features=baseline,values,async,threads,component-imports,memory64,error-context
status=error: component runtime: missing host import `wasi:io/poll@0.2.6`
```

The artifact now loads, passes validation and enters `instantiateComponent`,
where it stops on the first `wasi:*` interface the host does not supply. This arm
is also where `GAP-21` mattered: its imports are aliased out of imported
interface instances, the shape the validator rejected as having “no closed
identity” once `GAP-20` stopped masking it
([`LF4_INDEX.md`](LF4_INDEX.md) §5.2).

Because the failure is still inside instantiation, the 18 declared imports are
never resolved and the arm says nothing yet about its own `add`.

## 6. What it would take to run it

1. **A WASI 0.2 host shim** — class D, the embedder's ([`../SCOPE.md`](../SCOPE.md)
   §3). The component surface already accepts `ComponentHostImports`; what is
   missing is an implementation of `wasi:io/poll`, `wasi:io/error`,
   `wasi:io/streams` (resources + streams),
   `wasi:cli/{stdin,stdout,stderr,exit,environment,terminal-*}`,
   `wasi:clocks`, `wasi:filesystem` and `wasi:random`. The `adder` call itself
   needs none of them; the runtime's `_start`-equivalent might.
2. **A host-side resource story** — `wasi:io/error` and `wasi:io/streams` export
   resource types and `wasi:io/poll` is the first import this component asks
   for; where a host implementation of a resource lives is the first design
   question a shim must answer ([`LF4_INDEX.md`](LF4_INDEX.md) §5.4).

The three engine defects are closed, so the only open item is embedder work.
Nothing here is a goal change, which is why the verdict is 🟨 Yellow rather than
🟥 Red.

## 7. What this arm does not answer

- Whether the Canonical ABI handles C#'s lifted types beyond `u32` (strings,
  lists, records, resources) — untested.
- Whether `NativeAOT-LLVM`'s `cabi_realloc` and post-return behave under the
  engine's loader — untested, because load does not complete.
- Whether the composed `hostapp` command from the guide's second half runs — it
  needs `wac plug` **and** a WASI implementation, and it is not this arm's bar.
- Performance, start-up, size trimming — out of scope.

## 8. References

- [`LF4_INDEX.md`](LF4_INDEX.md) §4–§5 — the verdict tables and the three findings.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2–§5 — versions, hash, transcript,
  reproduction.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary” and “VMContext
  Selection Rules”.
- [`../SCOPE.md`](../SCOPE.md) §3 — WASI as the embedder's surface.
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1, §8.1 — the class of
  `GAP-20`.
- [The upstream C# page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/csharp.html),
  retrieved 2026-11-16.


The export itself is the simplest possible lift:

```
(func $add (;25;) (type 38) (canon lift (core func $docs:adder/add@0.1.0#add)))
```

No options — and correctly so: `(u32, u32) -> u32` flattens to `(i32, i32) -> i32`
and needs no linear memory.
