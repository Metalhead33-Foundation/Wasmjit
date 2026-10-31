#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmCompilationPolicy.hpp"
#include "WasmBase/WasmDiagnostics.hpp"
#include "WasmBase/WasmGcHeap.hpp"
#include "WasmBase/WasmMemory.hpp"
#include "WasmBase/WasmModule.hpp"
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
