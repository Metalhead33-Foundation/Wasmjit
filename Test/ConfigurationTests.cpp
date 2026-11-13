#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <Euphemy/Config/EuphConfiguration.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmCompilationPolicy.hpp"
#include "WasmBase/WasmDiagnostics.hpp"
#include "WasmBase/WasmGcHeap.hpp"
#include "WasmBase/WasmMemory.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmParseOptions.hpp"
#include "WasmBase/WasmProfile.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/WasmStore.hpp"

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

/*!
  \file ConfigurationTests.cpp
  \brief The configurable budgets, limits and compilation policy.

  The design is in docs/proposals/CONFIGURATION.md (`CFG-1`…`CFG-9`) and
  docs/proposals/COMPILER_POLICY.md (`CP-1`…`CP-5`). The point of the suite is
  that every knob is *scoped* (one runtime, not one process), *resolved once*
  (a `0` inherits), and *refused where it is set* when it cannot be honoured.

  The last five cases cover the configuration-file reader — the
  `fromSection`/`fromConfiguration` seam over `Euph::Conf` — and they hold it to
  the same three properties: an absent key inherits, the widest `ConfUint` is
  the "unset" sentinel, and a spelling the engine cannot honour is refused by
  name (docs/proposals/CONFIGURATION.md §3, §8.3).
*/

namespace {

constexpr std::uint64_t kPage = 0x10000;
constexpr std::uint64_t kMiB = 1u << 20;

//! Loads a `.wat`-built fixture into `runtime`.
std::shared_ptr<const WASM::Module> loadFixture(WASM::Runtime& runtime, const char* name)
{
	const std::string path = std::string(WASM_TEST_DIR) + name;
	return runtime.loadFile(path);
}

//! True when the current execution state recorded `id`.
bool hasDiagnostic(std::string_view id)
{
	for (const WASM::Diagnostic& record : WASM::recentDiagnostics()) {
		if (record.id == id)
			return true;
	}
	return false;
}

//! Parses an INI document the way an embedder does: one `fromString`, then a
//! `fromConfiguration` per structure.
Euph::Conf::Configuration parseConfiguration(const std::string& text)
{
	return Euph::Conf::Configuration(text);
}

} // namespace

TEST_CASE("the default budget is 4 GiB and is what a zero config inherits", "[config]")
{
	// CFG-1/§10.5: the process default is 4 GiB unless the environment says
	// otherwise, and a `Store` built from a zero-initialised budget resolves
	// every field to it.
	const std::uint64_t processDefault = WASM::Store::allocationBudgetBytes();
	if (std::getenv("WASMJIT_MAX_TOTAL_BYTES") == nullptr)
		REQUIRE(processDefault == (4ull << 30));
	REQUIRE(processDefault > 0);

	WASM::Store store;
	REQUIRE(store.budget().totalBytes == processDefault);
	REQUIRE(store.budget().allLinearMemoriesBytes == processDefault);
	REQUIRE(store.budget().singleLinearMemoryBytes == processDefault);
	REQUIRE(store.budget().gcHeapBytes == processDefault);
	REQUIRE(store.budget().tableBytes == processDefault);
	REQUIRE(store.budget().reservedAddressBytes == 0); // opt-in, off

	// A store with an explicit total keeps the categories inherited.
	WASM::MemoryBudget budget;
	budget.totalBytes = 8 * kMiB;
	WASM::Store explicitStore(budget);
	REQUIRE(explicitStore.budget().totalBytes == 8 * kMiB);
	REQUIRE(explicitStore.budget().allLinearMemoriesBytes == 8 * kMiB);
	REQUIRE(explicitStore.budgetSource() == WASM::MemoryBudgetSource::Explicit);
}

TEST_CASE("a category above the total is refused where it is set", "[config]")
{
	WASM::MemoryBudget budget;
	budget.totalBytes = 1 * kMiB;
	budget.gcHeapBytes = 2 * kMiB;
	REQUIRE_THROWS_AS(WASM::Store{budget}, std::invalid_argument);
}

TEST_CASE("a per-runtime budget refuses a module that does not fit", "[config]")
{
	// The fixture declares a 4 MiB memory; 1 MiB cannot hold it, and the
	// refusal names the budget rather than failing with a bare `-1`.
	WASM::RuntimeConfig config;
	config.memory.totalBytes = 1 * kMiB;
	auto runtime = WASM::Runtime::create("libjit", config);
	auto module = loadFixture(*runtime, "config_budget.wasm");
	WASM::RegistryImportResolver imports;

	WASM::clearDiagnostics();
	REQUIRE_THROWS_WITH(runtime->instantiate(*module, imports),
			Catch::Matchers::ContainsSubstring("budget"));
	REQUIRE(hasDiagnostic(WASM::DiagnosticId::kMemoryBudgetExceeded));

	// The same module fits the default budget, in a second runtime that shares
	// the process but not the ceiling (CFG-1's whole point).
	auto standard = WASM::Runtime::create("libjit");
	auto sameModule = loadFixture(*standard, "config_budget.wasm");
	WASM::RegistryImportResolver otherImports;
	REQUIRE_NOTHROW(standard->instantiate(*sameModule, otherImports));
	REQUIRE(runtime->memoryBudget().totalBytes == 1 * kMiB);
	REQUIRE(standard->memoryBudget().totalBytes == WASM::Store::allocationBudgetBytes());
}

TEST_CASE("a per-memory cap binds below the total", "[config]")
{
	// CFG-2: one memory may be capped without touching the total.
	WASM::MemoryBudget budget;
	budget.totalBytes = 16 * kMiB;
	budget.singleLinearMemoryBytes = 1 * kMiB;
	WASM::Store store(budget);

	REQUIRE_THROWS_WITH(store.createLinearMemory(64, 64, false, false, kPage,
							  WASM::MemoryAllocationPolicy::Growable),
			Catch::Matchers::ContainsSubstring("per-memory"));

	// 8 pages fit the per-memory cap and the total.
	const WASM::Store::MemoryId id =
			store.createLinearMemory(8, 64, false, false, kPage,
					WASM::MemoryAllocationPolicy::Growable);
	WASM::LinearMemory* memory = store.memory(id);
	REQUIRE(memory != nullptr);
	REQUIRE(store.allocatedLinearBytes() == 8 * kPage);

	// A grow beyond the per-memory cap is a failed grow, not an exception.
	REQUIRE_FALSE(store.growMemory(memory, 64));
	REQUIRE(memory->memorySize == 8 * kPage);
}

TEST_CASE("a GC-heap cap refuses a region and credits nothing", "[config]")
{
	WASM::MemoryBudget budget;
	budget.totalBytes = 16 * kMiB;
	budget.gcHeapBytes = 64 * 1024;
	WASM::StoreLimits limits;
	limits.gcMinRegionBytes = 4096;
	limits.gcBlocksPerRegion = 1;
	WASM::Store store(budget, limits);
	REQUIRE(store.limits().gcMinRegionBytes == 4096);
	REQUIRE(store.limits().gcBlocksPerRegion == 1);

	WASM::GcHeap::RegionSet* set = store.gc().createSet();
	std::size_t allocated = 0;
	for (;;) {
		void* block = store.gc().allocate(*set, 16);
		if (block == nullptr)
			break;
		++allocated;
		REQUIRE(allocated < 100000); // the cap must bind, not loop forever
	}
	REQUIRE(allocated > 0);
	REQUIRE(store.allocatedGcBytes() <= 64 * 1024);
	store.gc().releaseSet(set);
}

TEST_CASE("a released memory and table give their bytes back", "[config]")
{
	// CFG-9/CFG14: before this milestone a destroyed instance kept its memories
	// charged for the store's lifetime.
	WASM::Store store;
	const std::uint64_t before = store.allocatedBytes();
	const WASM::Store::MemoryId memoryId =
			store.createLinearMemory(4, 16, false, false, kPage,
					WASM::MemoryAllocationPolicy::Growable);
	const WASM::Store::TableId tableId = store.createTable(8, 16);
	WASM::LinearMemory* memory = store.memory(memoryId);
	WASM::TableInstance* table = store.table(tableId);
	REQUIRE(memory != nullptr);
	REQUIRE(table != nullptr);
	REQUIRE(store.allocatedBytes() == before + 4 * kPage + 8 * sizeof(WASM::Callable*));

	REQUIRE(store.releaseTable(table));
	REQUIRE(store.allocatedTableBytes() == 0);
	REQUIRE(store.releaseMemory(memory));
	REQUIRE(store.allocatedLinearBytes() == 0);
	REQUIRE(store.allocatedBytes() == before);
	// The view is gone from the store's index space, and a second release is a
	// harmless false rather than a double credit.
	REQUIRE(store.memory(memoryId) == nullptr);
	REQUIRE_FALSE(store.releaseMemory(memory));
}

TEST_CASE("reserved address space is counted only when the budget asks", "[config]")
{
	// CFG-3: MP-1's reservation stays free by default; opting into
	// `reservedAddressBytes` bounds it without touching committed bytes.
	WASM::MemoryBudget uncounted;
	uncounted.totalBytes = 8 * kMiB;
	WASM::Store free(uncounted);
	const auto freeId = free.createLinearMemory(1, 128, false, false, kPage,
			WASM::MemoryAllocationPolicy::Reserved);
	REQUIRE(free.memory(freeId) != nullptr);
	REQUIRE(free.reservedAddressBytes() == 0);
	REQUIRE(free.allocatedLinearBytes() == kPage); // only the commit is charged

	WASM::MemoryBudget counted = uncounted;
	counted.reservedAddressBytes = 2 * kMiB;
	WASM::Store bounded(counted);
	const auto first = bounded.createLinearMemory(1, 16, false, false, kPage,
			WASM::MemoryAllocationPolicy::Reserved);
	REQUIRE(bounded.memory(first) != nullptr);
	REQUIRE(bounded.reservedAddressBytes() == 16 * kPage);
	// A second 2 MiB reservation does not fit beside the first.
	REQUIRE_THROWS_WITH(bounded.createLinearMemory(1, 32, false, false, kPage,
							  WASM::MemoryAllocationPolicy::Reserved),
			Catch::Matchers::ContainsSubstring("reservedAddressBytes"));
	REQUIRE(bounded.releaseMemory(bounded.memory(first)));
	REQUIRE(bounded.reservedAddressBytes() == 0);
}

TEST_CASE("store limits are clamped to what the implementation honours", "[config]")
{
	WASM::StoreLimits limits;
	limits.gcInlineClassBytes = 5;          // rounds up to the 16-byte floor
	limits.gcBlocksPerRegion = 100000;      // clamped
	limits.gcCycleCandidates = 1;           // clamped up to the floor
	limits.maxStringBytes = 0;              // the default
	WASM::Store store(WASM::MemoryBudget{}, limits);
	REQUIRE(store.limits().gcInlineClassBytes == 16);
	REQUIRE(store.limits().gcBlocksPerRegion == 4096);
	REQUIRE(store.limits().gcCycleCandidates == 16);
	REQUIRE(store.limits().maxStringBytes == WASM::kMaxStringBytes);
}


TEST_CASE("the compilation policy is a request a backend may refuse", "[compilation-policy]")
{
	// The vocabulary and the capability rule (CP-1/CPD6).
	REQUIRE(std::string(WASM::optimizationLevelName(WASM::OptimizationLevel::Full)) == "full");
	REQUIRE(std::string(WASM::compilationStrictnessName(WASM::CompilationStrictness::Lax)) == "lax");
	REQUIRE(std::string(WASM::interpreterFallbackName(WASM::InterpreterFallback::FallbackOnly))
			== "fallback-only");

	WASM::BackendCompilationCapabilities basics;
	REQUIRE(WASM::supportsOptimizationLevel(basics, WASM::OptimizationLevel::None));
	REQUIRE(WASM::supportsOptimizationLevel(basics, WASM::OptimizationLevel::Basic));
	REQUIRE_FALSE(WASM::supportsOptimizationLevel(basics, WASM::OptimizationLevel::Full));

	WASM::CompilationPolicy interpreting;
	interpreting.fallback = WASM::InterpreterFallback::Always;
	REQUIRE_FALSE(WASM::supportsCompilationPolicy(basics, interpreting));

	// libjit reports `Full` and no interpreter, so `Always` is refused at the
	// boundary with a message naming the field.
	WASM::RuntimeConfig policyConfig;
	policyConfig.compilation = interpreting;
	REQUIRE_THROWS_WITH(WASM::Runtime::create("libjit", policyConfig),
			Catch::Matchers::ContainsSubstring("interpret"));

	WASM::RuntimeConfig full;
	full.compilation.level = WASM::OptimizationLevel::Full;
	REQUIRE_NOTHROW(WASM::Runtime::create("libjit", full));
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime->compilationCapabilities().maxLevel == WASM::OptimizationLevel::Full);
	REQUIRE_FALSE(runtime->compilationCapabilities().interprets);
}

TEST_CASE("a lax policy regresses rather than failing, and books it", "[compilation-policy]")
{
	// CP-2/CP-3: `Strict` may refuse the `F1` shape at `Full`; `Lax` must
	// instantiate it anyway, at a lower degree, and report the regression.
	const auto load = [](WASM::Runtime& runtime) {
		return loadFixture(runtime, "compiler_policy.wasm");
	};

	WASM::RuntimeConfig strictConfig;
	strictConfig.compilation.level = WASM::OptimizationLevel::Full;
	strictConfig.compilation.strictness = WASM::CompilationStrictness::Strict;
	auto strict = WASM::Runtime::create("libjit", strictConfig);
	auto strictModule = load(*strict);
	WASM::RegistryImportResolver strictImports;

	bool strictFailed = false;
	try {
		strict->instantiate(*strictModule, strictImports);
	} catch (const std::exception&) {
		strictFailed = true; // libjit's optimizer rejected the shape
	}

	WASM::RuntimeConfig laxConfig;
	laxConfig.compilation.level = WASM::OptimizationLevel::Full;
	laxConfig.compilation.strictness = WASM::CompilationStrictness::Lax;
	auto lax = WASM::Runtime::create("libjit", laxConfig);
	auto laxModule = load(*lax);
	WASM::RegistryImportResolver laxImports;

	WASM::clearDiagnostics();
	WASM::ModuleInstance* instance = nullptr;
	REQUIRE_NOTHROW(instance = &lax->instantiate(*laxModule, laxImports));
	REQUIRE(instance != nullptr);
	// The export runs either way.
	REQUIRE(lax->callExport<std::int32_t>(*instance, "answer") == 42);

	if (strictFailed) {
		// The ladder ran: the achieved degree is below the request, and the
		// regression is a Warning record rather than a silent success.
		REQUIRE(lax->achievedOptimizationLevel() != WASM::OptimizationLevel::Full);
		REQUIRE(hasDiagnostic(WASM::DiagnosticId::kCompileReducedOptimization));
	} else {
		// libjit accepted `Full`: nothing regressed, and nothing was booked.
		REQUIRE(lax->achievedOptimizationLevel() == WASM::OptimizationLevel::Full);
		REQUIRE_FALSE(hasDiagnostic(WASM::DiagnosticId::kCompileReducedOptimization));
	}

	// `None` is a level every backend can serve.
	WASM::RuntimeConfig noneConfig;
	noneConfig.compilation.level = WASM::OptimizationLevel::None;
	auto none = WASM::Runtime::create("libjit", noneConfig);
	auto noneModule = load(*none);
	WASM::RegistryImportResolver noneImports;
	WASM::ModuleInstance& noneInstance = none->instantiate(*noneModule, noneImports);
	REQUIRE(none->callExport<std::int32_t>(noneInstance, "answer") == 42);
	REQUIRE(none->achievedOptimizationLevel() == WASM::OptimizationLevel::None);
}

TEST_CASE("an agent honours the configured execution limits", "[config]")
{
	WASM::RuntimeConfig config;
	config.execution.deferStackCapacity = 7;
	config.execution.diagnosticRingCapacity = 4;
	auto runtime = WASM::Runtime::create("libjit", config);
	std::unique_ptr<WASM::Agent> agent = runtime->createAgent();
	REQUIRE(agent->execution().deferCapacity() == 7);
	REQUIRE(agent->execution().diagnostics.capacity == 4);

	// A request above the inline ceiling is clamped, not refused: the array is
	// the ceiling because registering a cleanup must never allocate.
	WASM::ExecutionLimits huge;
	huge.deferStackCapacity = 4096;
	WASM::Agent clamped(huge);
	REQUIRE(clamped.execution().deferCapacity() == WASM::ExecutionState::kDeferCapacity);
}

TEST_CASE("a per-agent step budget traps a runaway loop", "[config]")
{
	// CFG-8: metering is selected when the runtime compiles the instance and
	// charged per agent. A loop back-edge and a function entry each cost one
	// unit, so the bound is deterministic for this fixture.
	WASM::RuntimeConfig config;
	config.execution.fuel = 64;
	auto runtime = WASM::Runtime::create("libjit", config);
	auto module = loadFixture(*runtime, "runtime_spin.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	auto agent = runtime->createAgent();
	WASM::AgentScope scope(*agent);

	try {
		(void)runtime->callExport<std::int32_t>(instance, "spin", 1000000);
		FAIL("the step budget did not stop the loop");
	} catch (const WASM::Trap& trap) {
		REQUIRE(trap.kind() == WASM::TrapKind::OutOfFuel);
	}
	REQUIRE(agent->fuelRemaining() == 0);

	// A backend that has not implemented the CFG-8 lowering refuses the
	// configuration instead of silently running unmetered.
	WASM::RuntimeConfig unsupported;
	unsupported.execution.fuel = 1;
	REQUIRE_THROWS(WASM::Runtime::create("sljit", unsupported));
}

TEST_CASE("continuation limits and provider are per runtime", "[config]")
{
	// CFG-4: the process-wide free functions stay the *default*, and a runtime
	// that asks for its own value keeps it. Inheritance is resolved when a
	// continuation is created, so a default changed after the runtime exists
	// still reaches the runtimes that did not ask.
	struct Restore {
		WASM::ContinuationProvider previous;
		~Restore() { WASM::setContinuationProvider(previous); }
	} restore{WASM::continuationProvider()};

	WASM::RuntimeConfig inheriting;
	auto inherited = WASM::Runtime::create("libjit", inheriting);
	WASM::setContinuationProvider(WASM::ContinuationProvider::HostThreads);
	REQUIRE(inherited->continuationProvider() == WASM::ContinuationProvider::HostThreads);

	WASM::RuntimeConfig own;
	own.continuation.provider = WASM::ContinuationProvider::NativeStacks;
	own.continuation.maxLive = 3;
	own.continuation.stackBytes = 256 * 1024;
	auto ownRuntime = WASM::Runtime::create("libjit", own);
	REQUIRE(ownRuntime->continuationProvider() == WASM::ContinuationProvider::NativeStacks);
	REQUIRE(ownRuntime->continuationBudget().maxLive == 3);
	REQUIRE(ownRuntime->continuationBudget().stackBytes == 256 * 1024);
	// The runtime that asked for nothing still follows the process default.
	REQUIRE(inherited->continuationBudget().maxLive == WASM::continuationBudget().maxLive);
	REQUIRE(inherited->continuationBudget().stackBytes == WASM::continuationBudget().stackBytes);

	// `Inherit` is a configuration sentinel: asking the *default* to inherit
	// itself leaves it alone, and it is not a provider.
	WASM::setContinuationProvider(WASM::ContinuationProvider::Inherit);
	REQUIRE(WASM::continuationProvider() == WASM::ContinuationProvider::HostThreads);
	REQUIRE(std::string(WASM::continuationProviderName(WASM::ContinuationProvider::Inherit))
			== "inherit");

	// The live-continuation counter and its ceiling are per store.
	WASM::Store& store = ownRuntime->store();
	REQUIRE(store.liveContinuationCount() == 0);
	REQUIRE(store.reserveContinuationSlot(2));
	REQUIRE(store.reserveContinuationSlot(2));
	REQUIRE_FALSE(store.reserveContinuationSlot(2));
	REQUIRE(store.liveContinuationCount() == 2);
	store.releaseContinuationSlot();
	REQUIRE(store.liveContinuationCount() == 1);
	REQUIRE(store.reserveContinuationSlot(2));
	store.releaseContinuationSlot();
	store.releaseContinuationSlot();
	REQUIRE(store.liveContinuationCount() == 0);
}


TEST_CASE("a runtime's continuation ceiling binds at cont.new", "[config]")
{
	// The end-to-end form of CFG-4: the `cont.new` path resolves the store the
	// creating VMContext belongs to, so a runtime with `maxLive = 1` refuses the
	// second continuation while the first is alive — and reports why.
	WASM::RuntimeConfig config;
	config.continuation.maxLive = 1;
	auto runtime = WASM::Runtime::create("libjit", config);
	auto module = loadFixture(*runtime, "config_budget.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	WASM::Callable target{};
	target.fnPtr = reinterpret_cast<void*>(+[](WASM::VMContext*) -> std::int32_t {
		return 5;
	});

	WASM::clearDiagnostics();
	WASM::clearPendingTrap();
	void* first = WASM::wasm_cont_new(&target, instance.context(), 0, 0, 1);
	REQUIRE(first != nullptr);
	void* second = WASM::wasm_cont_new(&target, instance.context(), 0, 0, 1);
	REQUIRE(second == nullptr);
	REQUIRE(hasDiagnostic(WASM::DiagnosticId::kContinuationBudgetExhausted));
	// A refused `cont.new` leaves the pending trap for the generated caller to
	// check; a test that expects the refusal must consume it.
	REQUIRE(WASM::trapPending());
	REQUIRE(WASM::pendingTrapKind() == WASM::TrapKind::OutOfMemory);
	WASM::clearPendingTrap();

	// Releasing the first gives the slot back, and the release also credits the
	// store's own counter.
	REQUIRE(WASM::continuationRefRelease(first));
	REQUIRE(runtime->store().liveContinuationCount() == 0);
	void* third = WASM::wasm_cont_new(&target, instance.context(), 0, 0, 1);
	REQUIRE(third != nullptr);
	REQUIRE(WASM::continuationRefRelease(third));
	REQUIRE(runtime->store().liveContinuationCount() == 0);
}


TEST_CASE("the compilation ladder prefers an interpreter over a lower degree", "[compilation-policy]")
{
	// CP-4's core rule, as a pure decision: an interpreter is *exact*, so a
	// backend that has one is asked to use it before the degree is lowered.
	WASM::BackendCompilationCapabilities compiledOnly; // Basic, no interpreter
	WASM::BackendCompilationCapabilities interpreting; // Basic + interpreter
	interpreting.interprets = true;

	WASM::CompilationPolicy strict;
	strict.level = WASM::OptimizationLevel::Full;
	REQUIRE_FALSE(WASM::nextCompilationAttempt(strict, compiledOnly).has_value());

	WASM::CompilationPolicy lax;
	lax.level = WASM::OptimizationLevel::Full;
	lax.strictness = WASM::CompilationStrictness::Lax;

	// No interpreter: one degree down, then one more, then stop.
	const auto basic = WASM::nextCompilationAttempt(lax, compiledOnly);
	REQUIRE(basic.has_value());
	REQUIRE(basic->level == WASM::OptimizationLevel::Basic);
	REQUIRE(basic->fallback == WASM::InterpreterFallback::Never);
	const auto none = WASM::nextCompilationAttempt(*basic, compiledOnly);
	REQUIRE(none.has_value());
	REQUIRE(none->level == WASM::OptimizationLevel::None);
	REQUIRE_FALSE(WASM::nextCompilationAttempt(*none, compiledOnly).has_value());

	// With an interpreter: the *same* degree, interpreted, before any regression.
	const auto interpreted = WASM::nextCompilationAttempt(lax, interpreting);
	REQUIRE(interpreted.has_value());
	REQUIRE(interpreted->level == WASM::OptimizationLevel::Full);
	REQUIRE(interpreted->fallback == WASM::InterpreterFallback::FallbackOnly);
	// The interpreter was the last resort: a failure there is not a degree problem.
	REQUIRE_FALSE(WASM::nextCompilationAttempt(*interpreted, interpreting).has_value());

	// `Always` never compiles, so there is no degree to regress.
	WASM::CompilationPolicy always = lax;
	always.fallback = WASM::InterpreterFallback::Always;
	REQUIRE_FALSE(WASM::nextCompilationAttempt(always, interpreting).has_value());
}

TEST_CASE("the remaining allocation categories can be capped too", "[config]")
{
	// Every category the store charges has its own counter and ceiling:
	// strings, exception objects and continuation stacks join linear memory,
	// tables and the GC heap (docs/proposals/CONFIGURATION.md CFG-2).
	WASM::MemoryBudget budget;
	budget.totalBytes = 8 * kMiB;
	budget.stringBytes = 48 * 1024;
	WASM::Store store(budget);
	REQUIRE(store.budget().stringBytes == 48 * 1024);
	REQUIRE(store.budget().exceptionBytes == 8 * kMiB); // inherited
	REQUIRE(store.budget().continuationStackBytes == 8 * kMiB);

	std::vector<std::uint32_t> codePoints(4096, 'x'); // 16 KiB of code points
	WASM::StringObject* first = store.strings().create(codePoints);
	REQUIRE(first != nullptr);
	REQUIRE(store.allocatedStringBytes() > 0);
	WASM::StringObject* second = store.strings().create(codePoints);
	REQUIRE(second != nullptr);
	// A third 16 KiB string does not fit the 48 KiB string cap, while the 8 MiB
	// total would still accept it: the category binds first. The refusal is a
	// trap, so consume it before asserting.
	WASM::clearPendingTrap();
	WASM::StringObject* third = store.strings().create(codePoints);
	const bool trapped = WASM::trapPending();
	const WASM::TrapKind kind = WASM::pendingTrapKind();
	WASM::clearPendingTrap();
	REQUIRE(third == nullptr);
	REQUIRE(trapped);
	REQUIRE(kind == WASM::TrapKind::OutOfMemory);
	REQUIRE(store.allocatedStringBytes() <= 48 * 1024);

	// A category larger than the total is still refused where it is set.
	WASM::MemoryBudget bad;
	bad.totalBytes = 1 * kMiB;
	bad.continuationStackBytes = 2 * kMiB;
	REQUIRE_THROWS_AS(WASM::Store{bad}, std::invalid_argument);
}

TEST_CASE("a runtime configuration is read from a configuration file", "[config]")
{
	// The file channel, one section, every family. The keys carry their type in
	// their first letter because that is how the reader types a value
	// (`ConfigurationSection::insert`); the values are the stable spellings the
	// engine's own `*Name()` helpers report.
	const Euph::Conf::Configuration file = parseConfiguration(
			"[WasmJit]\n"
			"sProfile = scalar\n"
			"sConcurrency = agents\n"
			"uExcludedMarkers = 1\n"
			"uTotalBytes = 268435456\n"
			"uSingleLinearMemoryBytes = 67108864\n"
			"uMaxStringBytes = 8388608\n"
			"uGcBlocksPerRegion = 32\n"
			"uStackReserveBytes = 1048576\n"
			"uDeferStackCapacity = 8\n"
			"uFuel = 1000000\n"
			"sLevel = Full\n"
			"sStrictness = Lax\n"
			"sInterpreterFallback = fallback_only\n"
			"uMaxLive = 8\n"
			"uStackBytes = 262144\n"
			"sProvider = host-threads\n"
			"uAllocationBudgetBytes = 268435456\n");

	const WASM::RuntimeConfig config = WASM::RuntimeConfig::fromConfiguration(file);

	// `scalar` is `{V}`; `uExcludedMarkers = 1` adds `N`, and the two compose into
	// an intersection no named helper spells.
	REQUIRE(config.profile.excludes(WASM::ProfileMarker::Vectors));
	REQUIRE(config.profile.excludes(WASM::ProfileMarker::NonDeterministic));
	REQUIRE_FALSE(config.profile.excludes(WASM::ProfileMarker::Threads));
	REQUIRE(config.concurrency == WASM::ConcurrencyModel::Agents);

	REQUIRE(config.memory.totalBytes == 268435456u);
	REQUIRE(config.memory.singleLinearMemoryBytes == 67108864u);
	REQUIRE(config.memory.gcHeapBytes == 0); // absent, so no separate cap

	REQUIRE(config.store.maxStringBytes == 8388608u);
	REQUIRE(config.store.gcBlocksPerRegion == 32u);
	REQUIRE(config.execution.stackReserveBytes == 1048576u);
	REQUIRE(config.execution.deferStackCapacity == 8u);
	REQUIRE(config.execution.fuel == 1000000u);

	// Spelling is case-insensitive and `_`/`-` are interchangeable, so the file
	// may read like prose while the vocabulary stays the diagnostic one.
	REQUIRE(config.compilation.level == WASM::OptimizationLevel::Full);
	REQUIRE(config.compilation.strictness == WASM::CompilationStrictness::Lax);
	REQUIRE(config.compilation.fallback == WASM::InterpreterFallback::FallbackOnly);

	REQUIRE(config.continuation.maxLive == 8u);
	REQUIRE(config.continuation.stackBytes == 262144u);
	REQUIRE(config.continuation.provider == WASM::ContinuationProvider::HostThreads);
	REQUIRE(config.allocationBudgetBytes == 268435456u);

	// A borrowed pointer cannot come out of a text file: the file channel leaves
	// the three host-owned members alone.
	REQUIRE(config.hostBridge == nullptr);
	REQUIRE(config.sink == nullptr);
	REQUIRE(config.sinkUserData == nullptr);

	// Parse options read from the same section, as an embedder would.
	const WASM::ParseOptions options = WASM::ParseOptions::fromConfiguration(file);
	REQUIRE_FALSE(options.legacyExceptionHandling);
	REQUIRE(options.legacyExceptionRecovery);
}

TEST_CASE("an absent key and an absent section both keep the defaults", "[config]")
{
	// An empty file configures nothing: every field stays what a default-
	// constructed struct has, so a host can ship one `[WasmJit]` section and grow
	// it key by key without changing today's behaviour in between.
	const Euph::Conf::Configuration file = parseConfiguration("[WasmJit]\n");

	const WASM::RuntimeConfig config = WASM::RuntimeConfig::fromConfiguration(file);
	REQUIRE(config.profile.isFull());
	REQUIRE(config.concurrency == WASM::ConcurrencyModel::Single);
	REQUIRE(config.memory.totalBytes == 0);
	REQUIRE(config.memory.allLinearMemoriesBytes == 0);
	REQUIRE(config.memory.singleLinearMemoryBytes == 0);
	REQUIRE(config.memory.gcHeapBytes == 0);
	REQUIRE(config.memory.tableBytes == 0);
	REQUIRE(config.memory.stringBytes == 0);
	REQUIRE(config.memory.exceptionBytes == 0);
	REQUIRE(config.memory.continuationStackBytes == 0);
	REQUIRE(config.memory.reservedAddressBytes == 0);
	REQUIRE(config.store.maxStringBytes == 0);
	REQUIRE(config.store.gcInlineClassBytes == 0);
	REQUIRE(config.store.gcMinRegionBytes == 0);
	REQUIRE(config.store.gcBlocksPerRegion == 0);
	REQUIRE(config.store.gcCycleCandidates == 0);
	REQUIRE(config.execution.stackReserveBytes == 0);
	REQUIRE(config.execution.deferStackCapacity == 0);
	REQUIRE(config.execution.diagnosticRingCapacity == 0);
	REQUIRE(config.execution.fuel == 0);
	REQUIRE(config.compilation.level == WASM::OptimizationLevel::Basic);
	REQUIRE(config.compilation.strictness == WASM::CompilationStrictness::Strict);
	REQUIRE(config.compilation.fallback == WASM::InterpreterFallback::Never);
	REQUIRE(config.continuation.maxLive == 0);
	REQUIRE(config.continuation.stackBytes == 0);
	REQUIRE(config.continuation.provider == WASM::ContinuationProvider::Inherit);
	REQUIRE(config.allocationBudgetBytes == 0);

	// A configuration that names no such section at all reads the same way: the
	// section is created on demand, so "the file has no `[WasmJit]`" is a default,
	// never an error.
	const Euph::Conf::Configuration other = parseConfiguration("[Unrelated]\nuThing = 7\n");
	const WASM::RuntimeConfig alsoDefault = WASM::RuntimeConfig::fromConfiguration(other);
	REQUIRE(alsoDefault.memory.totalBytes == 0);
	REQUIRE(alsoDefault.compilation.level == WASM::OptimizationLevel::Basic);

	// The default parse options are the field defaults: recovery on, permissive
	// rewriting off.
	const WASM::ParseOptions options = WASM::ParseOptions::fromConfiguration(other);
	REQUIRE_FALSE(options.legacyExceptionHandling);
	REQUIRE(options.legacyExceptionRecovery);
}

TEST_CASE("the widest configuration integer is the budget's \"inherit\"", "[config]")
{
	// A 32-bit `ConfUint` cannot hold 4 GiB, so the widest value a file can name
	// is read as "unset" — the same meaning `0` has — and the budget resolves to
	// the process default rather than to 4294967295 bytes.
	const Euph::Conf::Configuration file = parseConfiguration(
			"[WasmJit]\n"
			"uTotalBytes = 4294967295\n"
			"uStringBytes = 4294967295\n");

	const WASM::MemoryBudget budget = WASM::MemoryBudget::fromConfiguration(file);
	REQUIRE(budget.totalBytes == 0);
	REQUIRE(budget.stringBytes == 0);

	WASM::Store store(budget);
	REQUIRE(store.budget().totalBytes == WASM::Store::allocationBudgetBytes());
	REQUIRE(store.budgetSource() == WASM::MemoryBudgetSource::Default);

	// The same budget, as an embedder would pass it on to a runtime.
	WASM::RuntimeConfig config;
	config.memory = WASM::MemoryBudget::fromConfiguration(file);
	config.memory.totalBytes = 1 * kMiB;
	auto runtime = WASM::Runtime::create("libjit", config);
	REQUIRE(runtime->memoryBudget().totalBytes == 1 * kMiB);
}

TEST_CASE("a misspelled configuration value is refused by name", "[config]")
{
	// A typo must not silently compile at `Basic`: the refusal names the key and
	// the spellings it accepts, which is what makes the file debuggable.
	const Euph::Conf::Configuration level = parseConfiguration("[WasmJit]\nsLevel = warp\n");
	try {
		WASM::RuntimeConfig::fromConfiguration(level);
		FAIL("a misspelled optimisation level must be refused");
	} catch (const std::invalid_argument& error) {
		const std::string message(error.what());
		REQUIRE(message.find("sLevel") != std::string::npos);
		REQUIRE(message.find("basic") != std::string::npos);
		REQUIRE(message.find("full") != std::string::npos);
	}

	const Euph::Conf::Configuration provider =
			parseConfiguration("[WasmJit]\nsProvider = threads\n");
	REQUIRE_THROWS_AS(WASM::ContinuationSettings::fromConfiguration(provider),
			std::invalid_argument);

	const Euph::Conf::Configuration profile = parseConfiguration("[WasmJit]\nsProfile = tiny\n");
	REQUIRE_THROWS_AS(WASM::RuntimeConfig::fromConfiguration(profile), std::invalid_argument);

	// A refused value means nothing is applied half-way: the factory returns a
	// whole struct or throws.
	const Euph::Conf::Configuration both = parseConfiguration(
			"[WasmJit]\nuFuel = 500\nsConcurrency = many\n");
	REQUIRE_THROWS_AS(WASM::RuntimeConfig::fromConfiguration(both), std::invalid_argument);
}

TEST_CASE("a configuration section can be read one family at a time", "[config]")
{
	// The section overload is what an embedder with its own INI layout uses: any
	// `Euph::Conf::ConfigurationSection` works, not only one from a whole
	// `Configuration`.
	Euph::Conf::ConfigurationSection section;
	section.setUint("uMaxStringBytes", 1u << 20);
	section.setUint("uGcCycleCandidates", 128);
	section.setBool("bLegacyExceptionHandling", 1);

	const WASM::StoreLimits limits = WASM::StoreLimits::fromSection(section);
	REQUIRE(limits.maxStringBytes == 1u << 20);
	REQUIRE(limits.gcCycleCandidates == 128);
	REQUIRE(limits.gcInlineClassBytes == 0); // untouched

	const WASM::ParseOptions options = WASM::ParseOptions::fromSection(section);
	REQUIRE(options.legacyExceptionHandling);
	REQUIRE(options.legacyExceptionRecovery);

	// The other families see a section that names none of their keys, so they
	// read their defaults rather than someone else's values.
	const WASM::MemoryBudget budget = WASM::MemoryBudget::fromSection(section);
	REQUIRE(budget.totalBytes == 0);
	const WASM::CompilationPolicy policy = WASM::CompilationPolicy::fromSection(section);
	REQUIRE(policy.level == WASM::OptimizationLevel::Basic);
	REQUIRE(policy.strictness == WASM::CompilationStrictness::Strict);
}

