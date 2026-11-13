#include "WasmCompilationPolicy.hpp"
#include "WasmContinuation.hpp"
#include "WasmParseOptions.hpp"
#include "WasmProfile.hpp"
#include "WasmRuntime.hpp"
#include "WasmStore.hpp"

#include <Euphemy/Config/EuphConfiguration.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

/*!
  \file WasmConfiguration.cpp
  \brief The configuration-file readers: the `Euph::Conf` seam, in one place.

  Every `fromSection`/`fromConfiguration` factory declared in the configuration
  headers is defined here, so the coupling to the vendored INI reader
  (`Euphemy/Config`) is one translation unit rather than a dependency of eight
  public headers. Those headers name `Euph::Conf::ConfigurationSection` and
  `Euph::Conf::Configuration` only as forward-declared references.

  Three rules hold across every reader, and each is restated beside the reader it
  belongs to as well:

  * **An absent key keeps the field's default.** The reader is *additive*: it
    never invents a value a `RuntimeConfig{}` would not have. The reader's
    getters record the default they are given and return it, which is why each
    reader takes a `const` section and works on a copy — the caller's
    configuration is never written to.
  * **`0`, and the widest `Euph::Conf::ConfUint`, mean "the documented
    default".** A 32-bit `ConfUint` cannot express the 4 GiB process budget, so
    the widest representable value is the sentinel a file may name for "unset".
  * **A spelling that names no enum value is refused.** A typo in
    `sLevel = fulll` fails at load with `std::invalid_argument` naming the key
    and the accepted spellings, because a configuration that silently compiles
    at a different optimisation level is what this surface exists to prevent
    (docs/proposals/CONFIGURATION.md §2.1 rule 3, "refused where it is set").
*/

namespace WASM {
namespace {

//! The widest value a `u`-prefixed key can carry, read as "unset".
constexpr Euph::Conf::ConfUint kInherit = std::numeric_limits<Euph::Conf::ConfUint>::max();

//! Trims, lowercases and unifies `-`/`_`, so `FALLBACK_ONLY` and
//! `fallback-only` are one spelling.
std::string normalise(const std::string_view value)
{
	std::string text;
	text.reserve(value.size());
	for (const char character : value) {
		if (character >= 'A' && character <= 'Z')
			text.push_back(static_cast<char>(character - 'A' + 'a'));
		else if (character == '_')
			text.push_back('-');
		else
			text.push_back(character);
	}

	const auto isSpace = [](const char character) {
		return character == ' ' || character == '\t' || character == '\r'
				|| character == '\n';
	};
	std::size_t begin = 0;
	while (begin < text.size() && isSpace(text[begin]))
		++begin;
	std::size_t end = text.size();
	while (end > begin && isSpace(text[end - 1]))
		--end;
	return text.substr(begin, end - begin);
}

//! Reads a `u`-prefixed key; `0` and `kInherit` both mean "the field's
//! documented default".
std::uint64_t readUint(Euph::Conf::ConfigurationSection& section, const char* key,
		const Euph::Conf::ConfUint def = 0)
{
	const Euph::Conf::ConfUint raw = section.getUInteger(key, def);
	if (raw == kInherit)
		return 0;
	return raw;
}

//! Reads an `s`-prefixed key as a normalised word; empty when it is absent.
std::string readName(Euph::Conf::ConfigurationSection& section, const char* key)
{
	const std::string_view raw = section.getString(key, "");
	return normalise(raw);
}

//! Reads a section out of a parsed configuration by value; a configuration that
//! carries no section of that name yields an empty one, so every key falls back
//! to its default instead of the read failing.
Euph::Conf::ConfigurationSection sectionOf(const Euph::Conf::Configuration& config,
		const std::string_view name)
{
	const auto found = config.find(name);
	if (found == config.end())
		return Euph::Conf::ConfigurationSection{};
	return found->second;
}

//! Reads `key` as one of `values`, compared against the stable spelling `name`
//! reports. An absent key leaves `out` alone; anything else that matches no
//! value is refused, naming the key and the spellings it accepts.
template <typename Enum, typename NameFn>
void readEnum(Euph::Conf::ConfigurationSection& section, const char* key,
		const std::initializer_list<Enum> values, NameFn name, Enum& out)
{
	const std::string text = readName(section, key);
	if (text.empty())
		return;

	std::string accepted;
	for (const Enum value : values) {
		if (text == name(value)) {
			out = value;
			return;
		}
		if (!accepted.empty())
			accepted += ", ";
		accepted += '"';
		accepted += name(value);
		accepted += '"';
	}
	throw std::invalid_argument(std::string("WasmJit configuration: ") + key
			+ " must be one of " + accepted + ", not \"" + text + '"');
}

} // namespace

//! The profile a named `sProfile` asks for; the full profile when absent.
ProfileConfig ProfileConfig::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	// The reader's getters record a default for a key they did not find, so the
	// work is done on a copy: the caller's configuration is never written to.
	Euph::Conf::ConfigurationSection working(section);
	ProfileConfig config;
	Profile named = Profile::Full;
	readEnum(working, "sProfile",
			{ Profile::Full, Profile::Deterministic, Profile::Scalar,
					Profile::DeterministicScalar },
			profileName, named);
	switch (named) {
	case Profile::Full:
		config = fullProfile();
		break;
	case Profile::Deterministic:
		config = deterministicProfile();
		break;
	case Profile::Scalar:
		config = scalarProfile();
		break;
	case Profile::DeterministicScalar:
		config = deterministicScalarProfile();
		break;
	}
	// A raw marker set composes with the named profile: no named helper spells
	// `{N,V}`, and an embedder that measured one further exclusion should not
	// have to abandon the named profile to express it.
	config.excludedMarkers |= static_cast<std::uint32_t>(readUint(working, "uExcludedMarkers"));
	return config;
}

ProfileConfig ProfileConfig::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

MemoryBudget MemoryBudget::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	MemoryBudget budget;
	// The total's *parser* default is the widest `ConfUint` rather than `0`, so
	// the recorded default is the same "unset" sentinel a file may write. Both
	// end up as `0`, which is what lets the budget inherit the process default.
	budget.totalBytes = readUint(working, "uTotalBytes", kInherit);
	budget.allLinearMemoriesBytes = readUint(working, "uAllLinearMemoriesBytes");
	budget.singleLinearMemoryBytes = readUint(working, "uSingleLinearMemoryBytes");
	budget.gcHeapBytes = readUint(working, "uGcHeapBytes");
	budget.tableBytes = readUint(working, "uTableBytes");
	budget.stringBytes = readUint(working, "uStringBytes");
	budget.exceptionBytes = readUint(working, "uExceptionBytes");
	budget.continuationStackBytes = readUint(working, "uContinuationStackBytes");
	budget.reservedAddressBytes = readUint(working, "uReservedAddressBytes");
	return budget;
}

MemoryBudget MemoryBudget::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}
StoreLimits StoreLimits::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	StoreLimits limits;
	limits.maxStringBytes = readUint(working, "uMaxStringBytes");
	limits.gcInlineClassBytes = static_cast<std::uint32_t>(
			readUint(working, "uGcInlineClassBytes"));
	limits.gcMinRegionBytes = readUint(working, "uGcMinRegionBytes");
	limits.gcBlocksPerRegion = static_cast<std::uint32_t>(
			readUint(working, "uGcBlocksPerRegion"));
	limits.gcCycleCandidates = static_cast<std::uint32_t>(
			readUint(working, "uGcCycleCandidates"));
	return limits;
}

StoreLimits StoreLimits::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

ExecutionLimits ExecutionLimits::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	ExecutionLimits limits;
	limits.stackReserveBytes = readUint(working, "uStackReserveBytes");
	limits.deferStackCapacity = static_cast<std::uint32_t>(
			readUint(working, "uDeferStackCapacity"));
	limits.diagnosticRingCapacity = static_cast<std::uint32_t>(
			readUint(working, "uDiagnosticRingCapacity"));
	limits.fuel = readUint(working, "uFuel");
	return limits;
}

ExecutionLimits ExecutionLimits::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

CompilationPolicy CompilationPolicy::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	CompilationPolicy policy;
	readEnum(working, "sLevel",
			{ OptimizationLevel::None, OptimizationLevel::Basic, OptimizationLevel::Full },
			optimizationLevelName, policy.level);
	readEnum(working, "sStrictness",
			{ CompilationStrictness::Strict, CompilationStrictness::Lax },
			compilationStrictnessName, policy.strictness);
	readEnum(working, "sInterpreterFallback",
			{ InterpreterFallback::Never, InterpreterFallback::FallbackOnly,
					InterpreterFallback::Always },
			interpreterFallbackName, policy.fallback);
	return policy;
}

CompilationPolicy CompilationPolicy::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

ContinuationSettings ContinuationSettings::fromSection(
		const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	ContinuationSettings settings;
	settings.maxLive = static_cast<std::uint32_t>(readUint(working, "uMaxLive"));
	settings.stackBytes = static_cast<std::uint32_t>(readUint(working, "uStackBytes"));

	// There is no `continuationProviderName()` to reuse, so the spellings are
	// listed here; they are the ones this documentation uses everywhere. An
	// absent key — or `inherit` — follows the live process default.
	const struct { const char* name; ContinuationProvider value; } providers[] = {
		{ "inherit", ContinuationProvider::Inherit },
		{ "native-stacks", ContinuationProvider::NativeStacks },
		{ "host-threads", ContinuationProvider::HostThreads },
	};
	const std::string named = readName(working, "sProvider");
	if (!named.empty()) {
		std::string accepted;
		bool matched = false;
		for (const auto& provider : providers) {
			if (!accepted.empty())
				accepted += ", ";
			accepted += '"';
			accepted += provider.name;
			accepted += '"';
			if (named == provider.name) {
				settings.provider = provider.value;
				matched = true;
			}
		}
		if (!matched)
			throw std::invalid_argument("WasmJit configuration: sProvider must be one of "
					+ accepted + ", not \"" + named + '"');
	}
	return settings;
}

ContinuationSettings ContinuationSettings::fromConfiguration(
		const Euph::Conf::Configuration& config, const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

RuntimeConfig RuntimeConfig::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	RuntimeConfig config;
	config.profile = ProfileConfig::fromSection(working);
	readEnum(working, "sConcurrency",
			{ ConcurrencyModel::Single, ConcurrencyModel::Agents },
			concurrencyModelName, config.concurrency);
	config.memory = MemoryBudget::fromSection(working);
	config.store = StoreLimits::fromSection(working);
	config.execution = ExecutionLimits::fromSection(working);
	config.compilation = CompilationPolicy::fromSection(working);
	config.continuation = ContinuationSettings::fromSection(working);
	// The compatibility field keeps its own sentinel: a value here is *forwarded*
	// into `memory.totalBytes` by `Runtime::create`, so a file that leaves it out
	// must stay `0` and must never manufacture a disagreement.
	config.allocationBudgetBytes = readUint(working, "uAllocationBudgetBytes");
	return config;
}

RuntimeConfig RuntimeConfig::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

ParseOptions ParseOptions::fromSection(const Euph::Conf::ConfigurationSection& section)
{
	Euph::Conf::ConfigurationSection working(section);
	ParseOptions options;
	// The parser defaults *are* the field defaults, so a file records only a
	// deliberate departure and an absent key is indistinguishable from an
	// explicit repeat of the default.
	options.legacyExceptionHandling =
			working.getBool("bLegacyExceptionHandling", options.legacyExceptionHandling);
	options.legacyExceptionRecovery =
			working.getBool("bLegacyExceptionRecovery", options.legacyExceptionRecovery);
	return options;
}

ParseOptions ParseOptions::fromConfiguration(const Euph::Conf::Configuration& config,
		const std::string_view sectionName)
{
	return fromSection(sectionOf(config, sectionName));
}

} // namespace WASM
