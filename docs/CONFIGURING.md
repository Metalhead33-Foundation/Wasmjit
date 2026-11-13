# Configuring an Embedded Runtime {#doc_configuring}

*The INI Channel, the Keys It Carries, and the Settings Screen Around It*

Status: embedder guide (revision 1 — 2026-11-13). It records what a configuration
file can set, which keys the readers accept, and the three tasks an embedder
actually performs around them: read the settings, offer the backends in a
selector, and persist the choice. The design record for the knobs themselves is
[`proposals/CONFIGURATION.md`](proposals/CONFIGURATION.md); this note is the
how-to that sits on top of it.
Audience: anyone embedding `WASM::Runtime` and letting a user — or a settings
file — decide how the engine runs.

The engine owns no file format. It parses no `.toml`, no `.json` and no INI of its
own, it has no default path, and nothing in it opens a configuration file. What it
has is a **reader**: `fromSection` / `fromConfiguration` on `RuntimeConfig`, on
the five settings families inside it
(`MemoryBudget`, `StoreLimits`, `ExecutionLimits`, `CompilationPolicy`,
`ContinuationSettings`), on `ProfileConfig` and on `ParseOptions` — each taking an
`Euph::Conf` INI section. The file, its path, the moment it is read and the
meaning of a *Save* button stay the embedder's. This note documents both halves of
that split so a settings screen can be written without reading the headers. It
contains **no engine code**: every sketch below is caller-side and is not part of
the library.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Who owns what — file, syntax, keys, pointers, backend name |
| §3 | Which keys are read, what each means, and what absence means |
| §4 | A complete file that configures everything |
| §5 | Recipe: read the settings and create the runtime |
| §6 | Recipe: a backend selector for a GUI |
| §7 | Recipe: remember the choice, and read it back |
| §8 | What a file cannot set, and where that setting goes instead |
| §9 | What the reader does that will surprise you |
| §10 | What pins this |
| §11 | References |

## 2. Who owns what

| Question | Owner | Where it is decided |
|---|---|---|
| Which file, at which path, read when | **embedder** | the engine has no configuration path and never opens one; a portable host reads its own file next to its own settings |
| The syntax — sections, `;` comments, quoting, escapes | `Euph::Conf` | `Configuration::fromString` / `Configuration(std::istream&)`; §9 lists the edges |
| What a key means, its default, and what a wrong value does | **engine** | the `fromSection`/`fromConfiguration` readers; §3 is their reference |
| Which backend is selected | **embedder** | `WASM::Runtime::create(name, config)` — the name is a parameter, never a file key, because choosing a code generator is the embedder's act (`docs/BACKEND_PLUGIN_ABI.md` `BP-2`) |
| Borrowed pointers — `hostBridge`, `sink`, `sinkUserData` | **embedder** | assigned on the struct after the read; §8 |
| Per-agent limits | the runtime | `Runtime::createAgent()` applies `config.execution` (and `config.sink`) to every agent it makes |

**Precedence, unchanged.** The engine's rule is *explicit argument > environment >
default*. A value read from a file **is** the explicit argument: `fromConfiguration`
returns the struct the caller passes to `create()`, so nothing outranks it. What a
file leaves out is exactly what a default-constructed struct leaves out — which is
where the environment-seeded process defaults live (`WASMJIT_MAX_TOTAL_BYTES`,
`WASMJIT_MAX_CONTINUATIONS`, `WASMJIT_CONT_STACK_BYTES`, `WASMJIT_LEGACY_EH`). That
is the whole point of the additive rule in §3: adding a settings file to an
existing build cannot change the behaviour of a build that has none.

**Two prefixes are one vocabulary.** The keys carry the diagnostic spelling: the
profile, concurrency, optimisation level, strictness, interpreter fallback and
continuation provider all read the strings the engine's own `*Name()` helpers
print, so a line in a log and a line in a file are the same words.

## 3. The keys

One section carries everything. Every key names its **type in its first letter** —
`u` unsigned, `b` boolean, `s` string — because that is how the reader types a
value it parses; a line whose key begins with anything else makes
`Configuration::fromString` throw. The engine's keys are deliberately disjoint
across families, so a single `[WasmJit]` section holds the profile, the five
settings families and the parser policy at once, and `RuntimeConfig::fromSection`
reads exactly that set.

The column **absent** is the whole contract: a key that is not there leaves the
field at the value the header documents, which is the value a build without a
settings file has always used.

### 3.1 The runtime (`RuntimeConfig` own keys)

| Key | Field | Meaning | Absent |
|---|---|---|---|
| `sProfile` | `profile` | A named language profile, spelled as `profileName()` reports it: `full`, `deterministic`, `scalar`, `deterministic+scalar` (`docs/proposals/PROFILES.md`) | the full profile `{}` |
| `uExcludedMarkers` | `profile.excludedMarkers` | A raw `ProfileMarker` bit set, **OR-ed into** the named profile: `1` nondeterministic (N), `2` threads (T), `4` vectors (V). It lets a file name the intersection `{N,V}`, which no named helper spells | nothing added |
| `sConcurrency` | `concurrency` | `single` or `agents`; a per-instance entry discipline, not a scheduler (`docs/proposals/THREADS.md` `MF-6`) | `single` |
| `uAllocationBudgetBytes` | `allocationBudgetBytes` | Compatibility spelling of the total budget. Prefer `uTotalBytes`; a value that disagrees with a non-zero `uTotalBytes` is refused at `create()` | unset |

**A profile is a query, not a promise.** `sProfile = deterministic` names a real
profile that this engine does **not** claim alone — `{N,T}` without `V` needs
`v128` NaN normalisation (`PRD2`). The reader maps the spelling faithfully;
`supportsProfileConfig()` is the call that says whether the result is claimable, so
a settings screen should disable the options it reports `false` for.

### 3.2 The memory budget (`MemoryBudget`)

Every key is `u`, every value is bytes.

| Key | Field | Caps | Absent / `0` |
|---|---|---|---|
| `uTotalBytes` | `totalBytes` | Everything this runtime's store may commit | the process default — `Store::allocationBudgetBytes()`, i.e. 4 GiB unless `WASMJIT_MAX_TOTAL_BYTES` says otherwise |
| `uAllLinearMemoriesBytes` | `allLinearMemoriesBytes` | All linear memories together | the resolved total (no separate cap) |
| `uSingleLinearMemoryBytes` | `singleLinearMemoryBytes` | One linear memory | the resolved total |
| `uGcHeapBytes` | `gcHeapBytes` | Mapped GC-heap regions | the resolved total |
| `uTableBytes` | `tableBytes` | Committed table slots | the resolved total |
| `uStringBytes` | `stringBytes` | Store-owned strings (`stringref`) | the resolved total |
| `uExceptionBytes` | `exceptionBytes` | Store-owned exception objects | the resolved total |
| `uContinuationStackBytes` | `continuationStackBytes` | Continuation stacks | the resolved total |
| `uReservedAddressBytes` | `reservedAddressBytes` | *Reserved*, uncommitted address space (`CFG-3`) | `0` — reservations are not counted |

Three rules decide what a value here *does*, and none of them is enforced by the
reader — the budget is resolved once, where it is used
(`docs/proposals/CONFIGURATION.md` §9):

- **A category is a maximum under the total, never a replacement for it.**
  Categories do not borrow from one another, so the same allocations in the same
  order hit the same refusal.
- **A category above the total is refused where it is set.** `WASM::Store`'s
  constructor throws `std::invalid_argument`, so `Runtime::create` fails with a
  named message instead of failing at the first allocation.
- **A module that does not fit is refused at instantiation**, not at `create`: the
  throw names the budget and `WASM.MEMORY.BUDGET_EXCEEDED` is recorded (`CFG-7`).
  A failed `memory.grow` stays `-1`, silently.

**The widest value means "unset".** A 32-bit `Euph::Conf::ConfUint` cannot hold
4 GiB, so `0` **and** `4294967295` both read as "inherit". The consequence for a
settings screen: a cap of exactly `4294967295` bytes cannot be expressed, and it
does not need to be — the total already enforces that ceiling. A host that needs
numbers wider than a 32-bit `ConfUint` carries builds its budget in code, or the
tree is built with `LARGE_CONF_DATA`.

### 3.3 The store's sub-allocators (`StoreLimits`)

Tuning knobs, every one `u`, every one `0` = the built-in default. These were
compile-time constants until `CFG-6`; a value the implementation cannot honour is
clamped where it is used, never silently dropped, and each clamp is documented on
the field.

| Key | Field | Meaning | `0` |
|---|---|---|---|
| `uMaxStringBytes` | `maxStringBytes` | Hard cap on one string's bytes; clamped to `[1 KiB, 1 GiB]` | 16 MiB |
| `uGcInlineClassBytes` | `gcInlineClassBytes` | Objects at or below this size share a segregated region; clamped to `[16, 1024]` and rounded down to the 16-byte granularity, which is an ABI invariant | 1024 |
| `uGcMinRegionBytes` | `gcMinRegionBytes` | Lower bound on a newly mapped GC region | 64 KiB |
| `uGcBlocksPerRegion` | `gcBlocksPerRegion` | Target live blocks per segregated region | 64 |
| `uGcCycleCandidates` | `gcCycleCandidates` | Cycle-collection candidate cap per instance | 4096 |

### 3.4 The per-agent limits (`ExecutionLimits`)

Applied by `Runtime::createAgent()` to every agent that runtime makes, and
`uStackReserveBytes` additionally reaches the backend through
`ModuleInstantiator::setStackReserveBytes` at creation.

| Key | Field | Meaning | `0` |
|---|---|---|---|
| `uStackReserveBytes` | `stackReserveBytes` | Host stack reserve; the guards in `docs/SAFETY.md` §2.8 are sized against it | `kDefaultStackReserveBytes` |
| `uDeferStackCapacity` | `deferStackCapacity` | Deferred-cleanup slots per execution state; clamped to `[1, kDeferCapacity]` because the record array is inline — registering a cleanup must never allocate | `ExecutionState::kDeferCapacity` |
| `uDiagnosticRingCapacity` | `diagnosticRingCapacity` | Records kept in the agent's diagnostic ring | `DiagnosticRing::kDefaultCapacity` |
| `uFuel` | `fuel` | Coarse step budget, charged at function entry and every loop back-edge (`CFG-8`); exhaustion reports `TrapKind::OutOfFuel` | unmetered |

**`uFuel` is a backend obligation, not a counter.** The check is lowered by code
generation, so a runtime created over a backend whose
`BackendCompilationCapabilities::fuelMetering` is false **refuses** a non-zero
`uFuel` at `create()` — it does not quietly ignore it. This is the first thing a
selector should grey out (§6).

### 3.5 The compilation policy (`CompilationPolicy`)

All three are `s`, spelled as `optimizationLevelName()`,
`compilationStrictnessName()` and `interpreterFallbackName()` report them
(`docs/proposals/COMPILER_POLICY.md`).

| Key | Field | Values | Absent |
|---|---|---|---|
| `sLevel` | `level` | `none`, `basic`, `full` | `basic` |
| `sStrictness` | `strictness` | `strict` — a failure to meet the request is an error; `lax` — regress one degree and book `WASM.COMPILE.REDUCED_OPTIMIZATION` | `strict` |
| `sInterpreterFallback` | `fallback` | `never`, `fallback-only` (interpret what failed to compile), `always` (never compile) | `never` |

The interpreter is **the backend's own** facility, never one the core supplies
(`docs/proposals/STACK_SWITCHING.md` §9.15 D-3). `create()` refuses by name a
level above `compilationCapabilities().maxLevel`, and a fallback on a backend that
cannot interpret (`CPD6`) — so a file that asks for `full` on a backend that only
produces `basic` fails at startup with both levels named, rather than silently
optimising less than asked. After instantiation, `achievedOptimizationLevel()` and
`achievedCompilationPolicy()` report what was actually produced.

### 3.6 The continuation settings (`ContinuationSettings`)

| Key | Field | Meaning | Absent / `0` |
|---|---|---|---|
| `uMaxLive` | `maxLive` | Live continuations this runtime may have | the process default, seeded by `WASMJIT_MAX_CONTINUATIONS` |
| `uStackBytes` | `stackBytes` | Per-continuation stack request (mmap size for `native-stacks`, pthread stack size for `host-threads`) | the process default, seeded by `WASMJIT_CONT_STACK_BYTES` |
| `sProvider` | `provider` | `inherit`, `native-stacks`, `host-threads` | `inherit` |

`inherit` is resolved when a *continuation* is created, not when the runtime is, so
a host that changes the process default later still affects a runtime that never
asked for its own — and a runtime that did ask keeps its value (`CFG-4`). A
continuation over budget reports `WASM.CONTINUATION.BUDGET_EXHAUSTED` and
`cont.new` yields null, which the generated code turns into a trap.

### 3.7 The parser policy (`ParseOptions`)

| Key | Field | Meaning | Absent |
|---|---|---|---|
| `bLegacyExceptionHandling` | `legacyExceptionHandling` | Translate the superseded Phase-3 exception form instead of refusing it — the **permissive** opt-in (`docs/proposals/LEGACY_EXCEPTIONS.md` `LE-6`) | `false` |
| `bLegacyExceptionRecovery` | `legacyExceptionRecovery` | Rewrite only the bodies that need it and book `WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN` — the **recovery** default | `true` |

Both default to the field defaults, so a file records only a deliberate departure.
`ParseOptions` is the one family that is **not** a runtime setting: see §8.

### 3.8 How a value is spelled

- **The type letter is mandatory and fixed by the key's first character.** `u` for
  every number, `b` for the two booleans, `s` for every name. Writing
  `sLevel = 2` does not select level 2 — it fails, because `sLevel` is a string key
  and `2` is not one of its spellings. (`Euph::Conf` also types `i` and `f`; the
  engine's keys never use them.)
- **Names are case-insensitive, and `_` and `-` are the same character**, so
  `sInterpreterFallback = FALLBACK_ONLY`, `fallback-only` and `Fallback-Only` are
  one value. Surrounding whitespace is trimmed.
- **Numbers are decimal.** There is no `0x`, no `1_000` and no `4GiB` — a suffix is
  the reader's problem, not a unit (§9).
- **A name that matches nothing is refused** with `std::invalid_argument` naming
  the key and the spellings it accepts. Show that message; it is the only report a
  typo ever gets.
- **An absent key is not an error; an absent section is not an error.** A
  configuration that carries no `[WasmJit]` block at all reads as "every default".

## 4. A file that configures everything

```ini
; WasmJit settings — hand-edited by the user, read once at startup.
[WasmJit]

; language subset and execution model
sProfile     = scalar
sConcurrency = single

; memory: a half-gigabyte runtime with a 256 MiB per-memory ceiling
uTotalBytes              = 536870912
uSingleLinearMemoryBytes = 268435456
uStringBytes             = 4194304
uReservedAddressBytes    = 0

; tuning
uMaxStringBytes    = 8388608
uGcBlocksPerRegion = 64

; per agent
uStackReserveBytes = 1048576
uFuel              = 0

; compilation
sLevel               = basic
sStrictness          = strict
sInterpreterFallback = never

; continuations
uMaxLive    = 16
uStackBytes = 524288
sProvider   = inherit

; parser policy — see §8, this one is not a runtime setting today
bLegacyExceptionHandling = false
bLegacyExceptionRecovery = true
```

Read top to bottom, this file says: run the scalar subset single-agent, budget half
a gigabyte with a 256 MiB per-memory ceiling, keep strings under 4 MiB, give each
agent a 1 MiB stack and no fuel limit, compile at `basic` strictly, allow 16
continuations of 512 KiB, and recover legacy exception bodies while refusing
unconditional rewriting. Every line is optional; deleting any one of them leaves
that knob at the default this note documents in §3.

## 5. Recipe: read the settings, then create the runtime

```cpp
#include <WasmBase/WasmRuntime.hpp>
#include <Euphemy/Config/EuphConfiguration.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

// The vendored reader expects the *host executable* to define its configuration
// object — the same contract every consumer of the installed SDK satisfies
// (docs/EMBEDDING_SURFACE.md §6.2). One definition, in one translation unit, or
// the link fails with an undefined `GLOBAL_CONFIGURATION`.
#include <Euphemy/Config/GlobalConfig.hpp>
Euph::Conf::Configuration GLOBAL_CONFIGURATION;

// 1. Read the file *you* own. The engine has no path, no default file name and no
//    opinion about when this runs; `fromString(std::istream&)` or the
//    `Configuration(std::istream&)` constructor is the whole API.
Euph::Conf::Configuration loadSettings(const std::filesystem::path& path)
{
	Euph::Conf::Configuration settings;
	std::ifstream file(path);
	settings.fromString(file);
	return settings;
}

// 2. Turn the section into the struct a runtime is created with. The call is a
//    pure read — `settings` stays `const`, because the reader copies the section
//    instead of recording defaults into your object.
WASM::RuntimeConfig readRuntimeConfig(const Euph::Conf::Configuration& settings)
{
	WASM::RuntimeConfig config = WASM::RuntimeConfig::fromConfiguration(settings);
	// A borrowed pointer cannot come out of text; §8 owns this half.
	config.hostBridge = &hostBridge;
	config.sink       = &diagnosticSink;
	return config;
}

// 3. Create. A value the engine cannot honour has already thrown — at step 2 for
//    an unparsable name, at this line for a budget or a policy the selected
//    backend cannot serve — and the message names the field.
std::unique_ptr<WASM::Runtime> makeRuntime(const Euph::Conf::Configuration& settings,
		std::string_view backend)
{
	return WASM::Runtime::create(backend, readRuntimeConfig(settings));
}
```

Two habits make the result debuggable, and both are supported by accessors rather
than by re-reading the file:

```cpp
// What was resolved, not what was asked. `memoryBudget()` is the budget the
// store actually enforces; the source says whether the file, the environment or
// the built-in default supplied it (`CFG-5`).
const WASM::MemoryBudget& budget = runtime->memoryBudget();
log("total=%llu source=%s", static_cast<unsigned long long>(budget.totalBytes),
		WASM::memoryBudgetSourceName(runtime->memoryBudgetSource()));

// What the backend can do, and what it actually produced after instantiation.
const WASM::BackendCompilationCapabilities capabilities = runtime->compilationCapabilities();
log("max-level=%s interprets=%d fuel=%d", WASM::optimizationLevelName(capabilities.maxLevel),
		capabilities.interprets, capabilities.fuelMetering);
log("requested=%s achieved=%s", WASM::optimizationLevelName(runtime->config().compilation.level),
		WASM::optimizationLevelName(runtime->achievedOptimizationLevel()));
```

**Failure handling is a startup decision, not a runtime one.** A settings file is
user input: a wrong `sLevel` spelling is an `std::invalid_argument` from the
reader, and a budget above the total or a level above the backend's capability is
an exception from `create()`. Show the message and fall back to
`RuntimeConfig{}` — never silently continue with a half-applied configuration,
because the reader never applies anything half-way: it returns a whole struct or
throws.

## 6. Recipe: a backend selector

A code generator is chosen by **name**, never by an enum a frontend would have to
be rebuilt for (`docs/BACKEND_PLUGIN_ABI.md` `BP-2`). That makes a selector a
`std::vector` of names plus, per row, whatever the caller can say about it.

### 6.1 What can be listed without running anything

| Source | What it gives | What it costs |
|---|---|---|
| `WASM::backendNames()` | every backend linked into the tree, sorted, always including the built-in `null` baseline | nothing — a registry read |
| `WASM::listBackendPlugins()` | every *compatible* plugin on the search path, as `BackendPluginInfo`: `name`, `version`, `abiVersion`, `coreAbiVersion`, `buildFlags`, `capabilities`, `maxParams`, `maxResults`, `path` | one descriptor read per candidate, then the library is closed again |
| `WASM::hasBackend(name)` | whether a **stored** name is still selectable (registered or loadable) | nothing |
| `runtime->compilationCapabilities()` | `maxLevel`, `interprets`, `fuelMetering` for the selected backend | creating a runtime — which compiles nothing until `instantiate`, so a capabilities column may create one per candidate and destroy it |

Probing a plugin is safe by construction: `probeBackendPlugin` reads the
descriptor, copies every value out and closes the image, so **no code from the
plugin runs** (`docs/BACKEND_PLUGIN_ABI.md` §7 rule 8). That is what makes an
eager "show me what is installed" list acceptable in a settings screen — but it
also means a probe cannot tell you what a plugin *compiles*; only a runtime can.

### 6.2 The list itself

```cpp
struct BackendChoice {
	std::string name;    // what `Runtime::create` takes, and what you store
	std::string label;   // what the row shows
	std::string detail;  // version / ABI / source, for a tooltip
};

std::vector<BackendChoice> selectableBackends()
{
	std::vector<BackendChoice> choices;
	for (const std::string& name : WASM::backendNames())
		choices.push_back({ name, name, "built in" });

	// A plugin's *own* name is not its file name, and a plugin may also be linked
	// into the tree. Deduplicate on the name the selector will store, and prefer
	// the plugin's version string as the detail.
	for (const WASM::BackendPluginInfo& plugin : WASM::listBackendPlugins()) {
		const auto found = std::find_if(choices.begin(), choices.end(),
				[&](const BackendChoice& choice) { return choice.name == plugin.name; });
		const std::string detail = plugin.version + " (plugin ABI "
				+ std::to_string(plugin.abiVersion) + ")";
		if (found != choices.end())
			found->detail = detail;
		else
			choices.push_back({ plugin.name, plugin.name, detail });
	}
	return choices;
}
```

### 6.3 Keeping the rest of the dialog honest

The capabilities decide which *other* controls may be offered, and the answer
arrives with the runtime:

- `fuelMetering == false` → disable the fuel field; a non-zero `config.execution.fuel`
  is refused at `create()`.
- `maxLevel` below `full` → do not offer `sLevel = full`; it is refused at
  `create()` rather than degraded, unless `sStrictness = lax` is the point.
- `interprets == false` → do not offer `sInterpreterFallback = fallback-only` or
  `always`; the core supplies no interpreter of its own.

`supportsOptimizationLevel(capabilities, level)` and
`supportsCompilationPolicy(capabilities, policy)` answer all three from a
capability set, so the same two calls gate the dialog and predict the `create()`
refusal. When the dialog closes, the selected name goes into the file as *your*
key (§7) and into `Runtime::create` — the same string in both places, which is
what stops a settings screen from contradicting itself.

## 7. Recipe: remember the choice

The engine stores no backend name and reads none: `Runtime::create` insists on a
non-empty name, because choosing a code generator is the embedder's decision
(`BP-2`), and the empty string is refused. So the name is **your** key in the same
section, read and written with the same reader.

```cpp
// Save: a string key, so `s`. `setString` creates the element if the file did not
// have one; `getSections()` is the mutable map.
void saveBackend(Euph::Conf::Configuration& settings, std::string_view backend)
{
	settings.getSections()["WasmJit"].setString("sBackend", backend);
}

// Read: `getString` on a `Configuration` is non-const, because the reader records
// the default it was given. Validate, and never fall through to "" — `create`
// refuses it.
std::string readBackend(Euph::Conf::Configuration& settings, std::string_view fallback)
{
	const std::string name(settings.getString("WasmJit", "sBackend", ""));
	if (!name.empty() && WASM::hasBackend(name))
		return name;
	return std::string(fallback); // the backend this build ships
}

// Write the whole file back. `operator<<` takes a mutable reference and emits
// `[section]` headers and `key=value` lines, i.e. exactly what `fromString` reads.
void saveSettings(Euph::Conf::Configuration& settings, const std::filesystem::path& path)
{
	std::ofstream file(path);
	file << settings;
}
```

Four facts make that round trip behave:

- **Store the name, not a path.** A stored name is resolved through
  `WASMJIT_BACKEND_PATH`, the working directory and the executable's directory, so
  a plugin that moved to another install keeps working; a stored path does not.
  `backendPluginFileName(name)` is the same name turned into the file the loader
  looks for (`libwasmjit-backend-<name>.so`), so one string identifies the
  backend, its plugin file and its line in a log — which is exactly what a
  settings file should carry.
- **`hasBackend()` is the migration check.** A user who removes a plugin leaves a
  name behind that this build cannot select; `hasBackend` turns that into a
  fallback instead of a startup exception.
- **The file you write is only what the file had.** The reader *copies* the section
  (§5), so it never records the defaults it used into your `Configuration`. For a
  complete template on disk, write the keys yourself with `setUint` / `setBool` /
  `setString` — or start from the listing in §4.
- **The stream operator writes values raw.** It adds no quotes and escapes
  nothing, so a string value containing `;`, or a line whose value begins with
  `[`, does not survive a save/read cycle. Backend names and enum spellings are
  plain identifiers, so this bites only if you store free-form text in the same
  section.

## 8. What a file cannot set

| Not a key | Why | Where it goes instead |
|---|---|---|
| `hostBridge`, `sink`, `sinkUserData` | borrowed pointers into the embedder; text cannot carry them | assign on the struct after the read (§5). `createAgent()` copies `sink`/`sinkUserData` into every agent the runtime makes |
| the backend name | selection is the embedder's act, and `Runtime::create` takes it as its first argument | §7 — your own `sBackend` key, validated with `hasBackend()` |
| `ParseOptions` for a runtime-loaded module | `Runtime::loadFile` / `loadBytes` take no `ParseOptions`; a core module they load is parsed with `ParseOptions::fromEnvironment()` | export `WASMJIT_LEGACY_EH` before the process starts, or drive `Module::fromFile(device, options, store)` yourself — and note that such a module lives in *your* store, while `Runtime::instantiate` only accepts the modules the runtime loaded |
| components' `ComponentOptions` | a per-load feature policy, not a runtime setting | pass it to `loadComponentFile` / `loadComponentBytes` |
| anything not in §3 | the reader is additive and does not police the file | it is ignored, silently — there is no unknown-key diagnostic, so your own keys (`sBackend`) cost nothing and are never mistaken for the engine's |

The last row is the one to plan around: **a key the engine does not know is not an
error.** A typo in the engine's namespace (`uTotalByte`) therefore reads as "not
set" and the default applies. If a settings screen needs to catch that, compare the
keys it wrote against the keys the file returned — `ConfigurationSection`'s
container is a `std::map` and is iterable for exactly this.

## 9. What the reader does that will surprise you

These are properties of `Euph::Conf`, not of the engine, but they decide what a
settings file may contain — and not one of them is reported by the engine.

- **Only `;` starts a comment.** `#` is *data*: a line beginning with `#` is read
  as a key, so `# note` fails as `Malformed line!` (no `=`) and `#note=1` fails the
  type check. Port a file from elsewhere and the first `#` comment is a thrown
  exception.
- **A value may be quoted on the way in, never on the way out.** `sBackend =
  "sljit"` stores `sljit` (outer quotes stripped, escapes resolved), which is why
  §7's writer — which adds no quotes — can still be read by hand. A value
  containing `;` is the case that breaks, because the reader cuts the line at the
  first unquoted `;`.
- **The first occurrence wins, for a section and for a key.** A second `[WasmJit]`
  block is parsed and dropped, and a repeated key keeps its first value
  (`emplace` semantics in both `Configuration` and `ConfigurationSection`). So
  "append an override at the end of the file" does not work; a settings screen must
  rewrite the element it wants to change.
- **A number is not validated.** The value is converted with `std::from_chars` and
  the error code is discarded, so an empty value, `1_000` or `0x10` is not
  diagnosed as such. Validate what the user types before it reaches a section; the
  engine's reader can only report a *name* it does not recognise.
- **Strings are interned in a process-wide pool that is never reclaimed.**
  `SizedString::string_pool` holds `STRING_POOL_SIZE` bytes for the whole process
  and its exhaustion is not checked. Parse a configuration once per change — never
  per frame — and do not keep a growing log of parsed `Configuration` objects. In
  exchange, the `std::string_view` that `getString` returns stays valid for the
  lifetime of the process, so this guide still copies it into a `std::string`
  only where it needs an owning name.
- **Names are `std::map<std::string, …, std::less<>>`.** A `string_view` lookup
  does not allocate, and iteration is sorted — which is why a file written back by
  §7 comes out in alphabetical order rather than in the order it was read.

## 10. What pins this

| Check | Pins |
|---|---|
| `[config]` in `Test/ConfigurationTests.cpp` — *a runtime configuration is read from a configuration file* | every family parses out of one section; the profile composes a named profile with a raw marker set; the compilation knobs accept the diagnostic spellings |
| *an absent key and an absent section both keep the defaults* | the additive rule, i.e. §3's "absent" column |
| *the widest configuration integer is the budget's "inherit"* | the `4294967295` sentinel and `MemoryBudgetSource::Default` |
| *a misspelled configuration value is refused by name* | §3.8's refusal, and that nothing is applied half-way |
| *a configuration section can be read one family at a time* | the `ConfigurationSection` overload, which is what an embedder with its own INI layout uses |
| `[headerdoc]` | the doc comments beside each reader, which are the contract this note summarises |
| `doxygen Doxyfile` | that the tables above and the header documentation render, not rot |

The authoritative survey, including what each case asserts, is
[`proposals/CONFIGURATION.md`](proposals/CONFIGURATION.md) §8.3. **If this note and
a reader's doc comment disagree, the header is right and this note is the bug** —
the procedure is [`FRESHNESS.md`](FRESHNESS.md) §7.

## 11. References

- [`proposals/CONFIGURATION.md`](proposals/CONFIGURATION.md) — the design record for the budgets, limits and the reader (`CFG-1`…`CFG-9`, §3, §8.3).
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) — `WASM::Runtime`, the header tiers, and why a vendored type may be *named* by a signature but never *included* by a header (§6.2).
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — selection by name (`BP-2`), the plugin descriptor, and the probe rule (§7 rule 8) that makes `listBackendPlugins()` safe.
- [`proposals/COMPILER_POLICY.md`](proposals/COMPILER_POLICY.md) — `sLevel`/`sStrictness`/`sInterpreterFallback`, the refusal at `create()` (`CPD6`), and `WASM.COMPILE.REDUCED_OPTIMIZATION`.
- [`proposals/PROFILES.md`](proposals/PROFILES.md) — what a named profile excludes, and why `deterministic` is not claimed alone (`PRD2`).
- [`proposals/THREADS.md`](proposals/THREADS.md) — `single` vs `agents` as an entry discipline (`MF-6`), not a scheduler.
- [`proposals/LEGACY_EXCEPTIONS.md`](proposals/LEGACY_EXCEPTIONS.md) — the two `ParseOptions` switches and the diagnostic the recovery books (`LE-6`).
- [`SAFETY.md`](SAFETY.md) — why a budget is finite and refusal is a trap or `-1`, and what `uStackReserveBytes` sizes (§2.8).
- [`FRESHNESS.md`](FRESHNESS.md) — what a note may claim about the tree, and the procedure when code changes a documented surface (§7).
- `extern/MhLib/Euphemy/Config/EuphConfiguration.hpp`, `EuphConfigurationElement.hpp` — the INI reader this note leans on: sections, typed elements, the string pool.







