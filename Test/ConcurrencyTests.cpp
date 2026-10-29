#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

namespace {

std::string spinModulePath()
{
	return std::string(WASM_TEST_DIR) + "runtime_spin.wasm";
}

} // namespace

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

TEST_CASE("a compiled function is re-entrant across host threads", "[concurrency]")
{
	// MF-1: two agents calling the *same* compiled function. The runtime's
	// per-instance guard is deliberately bypassed here — this is the backend
	// obligation (serialize compilation, keep every per-call value on the stack),
	// not the instance policy.
	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadFile(spinModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const std::optional<WASM::Callable> answer = instance.exportedFunction("answer");
	REQUIRE(answer.has_value());

	std::atomic<int> failures{ 0 };
	const auto body = [&answer, &failures] {
		for (int i = 0; i < 2000; ++i) {
			if (WASM::callCallable<std::int32_t>(*answer) != 42)
				++failures;
		}
	};
	std::thread first(body);
	std::thread second(body);
	first.join();
	second.join();
	REQUIRE(failures.load() == 0);
}

TEST_CASE("a second host thread is refused while a call is inside the instance", "[concurrency]")
{
	// MF-2 under a real call from another thread: the guard is observed while the
	// worker is inside `spin`, so the refusal is deterministic rather than a race.
	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadFile(spinModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	std::atomic<bool> started{ false };
	std::atomic<bool> workerFailed{ false };
	std::thread worker([&runtime, &instance, &started, &workerFailed] {
		started.store(true, std::memory_order_release);
		try {
			runtime->callExport<std::int32_t>(instance, "spin", 500000000);
		} catch (...) {
			workerFailed.store(true);
		}
	});

	while (!started.load(std::memory_order_acquire))
		std::this_thread::yield();
	for (int i = 0; i < 5000000 && !instance.inCall(); ++i)
		std::this_thread::yield();
	REQUIRE(instance.inCall());

	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "answer"),
			Catch::Matchers::ContainsSubstring("already executing a call"));

	worker.join();
	REQUIRE_FALSE(workerFailed.load());
	REQUIRE_FALSE(instance.inCall());
}

TEST_CASE("a safepoint requires a quiescent instance", "[concurrency]")
{
	// MF-4: gcCollect() is legal only when no call is inside the instance.
	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadFile(spinModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	REQUIRE(instance.tryEnterCall());
	REQUIRE_THROWS_WITH(instance.gcCollect(),
			Catch::Matchers::ContainsSubstring("quiescent"));
	// The runtime hook skips an instance that is in a call instead of throwing.
	REQUIRE_NOTHROW(runtime->safepoint());
	instance.leaveCall();
	REQUIRE_NOTHROW(instance.gcCollect());
}

TEST_CASE("concurrent safepoints serialize on the instance lock", "[concurrency]")
{
	// MF-3: the GC queues and counters are shared instance state; two quiescent
	// agents collecting at once must serialize rather than race.
	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadFile(spinModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	std::atomic<int> failures{ 0 };
	const auto body = [&instance, &failures] {
		for (int i = 0; i < 500; ++i) {
			try {
				instance.gcCollect();
			} catch (...) {
				++failures;
			}
		}
	};
	std::thread first(body);
	std::thread second(body);
	first.join();
	second.join();
	REQUIRE(failures.load() == 0);
	REQUIRE(instance.gcPendingCount() == 0);
}
