#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "WasmBase/WasmRuntime.hpp"

#include <string>

// docs/MULTI_AGENT_FEASIBILITY.md §6: the execution model is an opt-in
// restriction the embedder selects and the engine validates, never a promise a
// module can rely on.

TEST_CASE("the execution model is a validated restriction", "[concurrency]")
{
	REQUIRE(WASM::supportsConcurrency(WASM::ConcurrencyModel::Single));
	REQUIRE_FALSE(WASM::supportsConcurrency(WASM::ConcurrencyModel::Agents));
	REQUIRE(std::string(WASM::concurrencyModelName(WASM::ConcurrencyModel::Single)) == "single");
	REQUIRE(std::string(WASM::concurrencyModelName(WASM::ConcurrencyModel::Agents)) == "agents");

	WASM::RuntimeConfig config;
	config.concurrency = WASM::ConcurrencyModel::Agents;
	REQUIRE_THROWS_WITH(WASM::Runtime::create("libjit", config),
			Catch::Matchers::ContainsSubstring("not implemented"));

	// The scheduler table is embedder-owned state; the core keeps it null and
	// never calls it (§6.2).
	WASM::HostAgentScheduler scheduler;
	REQUIRE(scheduler.spawn == nullptr);
	REQUIRE(scheduler.userData == nullptr);
}

TEST_CASE("a per-runtime allocation budget is refused until it is implemented", "[concurrency]")
{
	WASM::RuntimeConfig config;
	config.allocationBudgetBytes = WASM::Store::allocationBudgetBytes() + 4096u;
	REQUIRE_THROWS_WITH(WASM::Runtime::create("libjit", config),
			Catch::Matchers::ContainsSubstring("allocationBudgetBytes"));
}
