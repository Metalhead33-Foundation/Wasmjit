#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "AgentFixture.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/wasm_atomic.hpp"
#include "WasmBase/WasmRuntime.hpp"

#include <atomic>
#include <memory>
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

std::string gcRefcountModulePath()
{
	return std::string(WASM_TEST_DIR) + "gc_refcount.wasm";
}

std::unique_ptr<WASM::Runtime> agentsRuntime()
{
	WASM::RuntimeConfig config;
	config.concurrency = WASM::ConcurrencyModel::Agents;
	return WASM::Runtime::create("libjit", config);
}

WASM::ModuleInstance& instantiateAgentFixture(WASM::Runtime& runtime,
											  WASM::RegistryImportResolver& imports)
{
	auto module = runtime.loadBytes(AgentFixture::moduleBytes());
	return runtime.instantiate(*module, imports);
}

} // namespace

// docs/MULTI_AGENT_FEASIBILITY.md §6: the execution model is an opt-in
// restriction the embedder selects and the engine validates, never a promise a
// module can rely on.

TEST_CASE("the execution model is a validated restriction", "[concurrency]")
{
	REQUIRE(WASM::supportsConcurrency(WASM::ConcurrencyModel::Single));
	REQUIRE(WASM::supportsConcurrency(WASM::ConcurrencyModel::Agents));
	REQUIRE(std::string(WASM::concurrencyModelName(WASM::ConcurrencyModel::Single)) == "single");
	REQUIRE(std::string(WASM::concurrencyModelName(WASM::ConcurrencyModel::Agents)) == "agents");

	// Both models are selectable; `Single` stays the default.
	WASM::RuntimeConfig config;
	config.concurrency = WASM::ConcurrencyModel::Agents;
	REQUIRE_NOTHROW(WASM::Runtime::create("libjit", config));
	REQUIRE(WASM::RuntimeConfig{}.concurrency == WASM::ConcurrencyModel::Single);

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
// ── Multi-agent execution (docs/proposals/THREADS.md TH-5) ─────────────────
//
// `ConcurrencyModel::Agents` admits several agents into one instance at the
// same time. These cases prove the pieces the design note lists as the TH-5
// blockers: a real cross-agent waiter table (MA-4), concurrent entry (MA-2),
// shared global atomics (SET-3), per-agent trap state (TRAP-6) and a
// multi-mutator-visible GC path (MF-3/MF-7).

TEST_CASE("two agents on one instance: acquire-release message passing",
		  "[multiagent][concurrency]")
{
	// ARA-3 / MA-4: `consume` spins on an acquire load of the flag while
	// `produce` publishes a plain value and then the flag with a release store.
	// The two calls are inside the same instance at the same time, which
	// `ConcurrencyModel::Single` refuses.
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = instantiateAgentFixture(*runtime, imports);

	std::atomic<bool> consumerStarted{ false };
	std::atomic<bool> failed{ false };
	std::atomic<std::int32_t> consumed{ -1 };

	std::thread consumer([&runtime, &instance, &consumerStarted, &consumed, &failed] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		consumerStarted.store(true, std::memory_order_release);
		try {
			consumed.store(runtime->callExport<std::int32_t>(instance, "consume"));
		} catch (...) {
			failed.store(true);
		}
	});

	while (!consumerStarted.load(std::memory_order_acquire))
		std::this_thread::yield();
	// `consume` cannot finish: the published flag is still zero, so the
	// consumer is inside the instance when the producer enters.
	for (int i = 0; i < 5000000 && !instance.inCall(); ++i)
		std::this_thread::yield();
	REQUIRE(instance.inCall());

	{
		auto producerAgent = runtime->createAgent();
		WASM::AgentScope scope(*producerAgent);
		REQUIRE_NOTHROW(runtime->callExport<void>(instance, "produce", std::int32_t(1234)));
	}

	consumer.join();
	REQUIRE_FALSE(failed.load());
	REQUIRE(consumed.load() == 1234);
	REQUIRE_FALSE(instance.inCall());
}

TEST_CASE("two agents on one instance: a wait is woken by another agent's notify",
		  "[multiagent][concurrency]")
{
	// MF-5 / MA-4: the waiter parks on address 8 expecting 0 with an infinite
	// timeout; the notifier waits until the waiter has announced itself
	// (`mark_ready`) and then calls `memory.atomic.notify` *without* changing
	// the value. A return of 0 can therefore only mean a real wake-up; the
	// single-agent stub would have returned 2 (timeout) and hung the test.
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = instantiateAgentFixture(*runtime, imports);

	std::atomic<std::int32_t> waitResult{ -1 };
	std::atomic<bool> failed{ false };
	std::thread waiter([&runtime, &instance, &waitResult, &failed] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		try {
			runtime->callExport<void>(instance, "mark_ready");
			waitResult.store(runtime->callExport<std::int32_t>(instance, "wait_flag"));
		} catch (...) {
			failed.store(true);
		}
	});

	bool announced = false;
	for (int i = 0; i < 5000000 && !announced; ++i) {
		announced = runtime->callExport<std::int32_t>(instance, "ready") == 1;
		std::this_thread::yield();
	}
	REQUIRE(announced);

	// Retry until the waiter has actually registered: a notify that arrives
	// before the park wakes nobody (and returns 0).
	uint32_t woke = 0;
	for (int i = 0; i < 5000000 && woke == 0; ++i)
		woke = static_cast<uint32_t>(runtime->callExport<std::int32_t>(instance, "notify_only"));

	REQUIRE(woke == 1);
	waiter.join();
	REQUIRE_FALSE(failed.load());
	REQUIRE(waitResult.load() == 0); // "ok": woken by the notify
}

TEST_CASE("two agents on one instance: shared global atomics compose",
		  "[multiagent][concurrency]")
{
	// SET-3 under agents: two agents hammer the same *shared global* with
	// `global.atomic.rmw.add`; the final value must be the exact sum.
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = instantiateAgentFixture(*runtime, imports);

	constexpr int kPerThread = 500;
	std::atomic<int> failures{ 0 };
	const auto body = [&runtime, &instance, &failures] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		try {
			for (int i = 0; i < kPerThread; ++i)
				(void)runtime->callExport<std::int32_t>(instance, "bump");
		} catch (...) {
			++failures;
		}
	};
	std::thread first(body);
	std::thread second(body);
	first.join();
	second.join();
	REQUIRE(failures.load() == 0);
	REQUIRE(runtime->callExport<std::int32_t>(instance, "counter")
			== 2 * kPerThread);
}

TEST_CASE("two agents on one instance: a trap belongs to its agent only",
		  "[multiagent][concurrency]")
{
	// TRAP-6 / TH-5: the pending-trap slot lives in `ExecutionState`, so one
	// agent's `unreachable` is invisible to the other. A shared slot would make
	// the worker fail as soon as the trapper ran.
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = instantiateAgentFixture(*runtime, imports);

	std::atomic<bool> trapped{ false };
	std::atomic<bool> workerFailed{ false };

	std::thread trapper([&runtime, &instance, &trapped] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		try {
			runtime->callExport<void>(instance, "boom");
		} catch (...) {
			trapped.store(true);
		}
	});
	std::thread worker([&runtime, &instance, &workerFailed] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		try {
			for (int i = 0; i < 2000; ++i) {
				if (runtime->callExport<std::int32_t>(instance, "answer") != 42)
					workerFailed.store(true);
			}
		} catch (...) {
			workerFailed.store(true);
		}
	});
	trapper.join();
	worker.join();

	REQUIRE(trapped.load());
	REQUIRE_FALSE(workerFailed.load());
	// A fresh agent on the calling thread sees neither the trap nor stale state.
	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);
}

TEST_CASE("two agents allocate and release GC objects on one instance",
		  "[multiagent][concurrency]")
{
	// MF-7: allocation, reference release and the deferred reclamation queue
	// are the one *correctness* prerequisite for shared GC between agents.
	// The heap allocator and the queue are locked (MF-3), the refcount is
	// atomic (RC-1), so two mutators must finish with a collectable queue and
	// no corruption.
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(gcRefcountModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	constexpr int kAllocs = 20000;
	std::atomic<int> failures{ 0 };
	const auto body = [&runtime, &instance, &failures] {
		auto agent = runtime->createAgent();
		WASM::AgentScope scope(*agent);
		try {
			if (runtime->callExport<std::int32_t>(instance, "alloc",
					std::int32_t(kAllocs)) != 0)
				++failures;
		} catch (...) {
			++failures;
		}
	};
	std::thread first(body);
	std::thread second(body);
	first.join();
	second.join();

	REQUIRE(failures.load() == 0);
	REQUIRE(instance.gcAllocatedObjects() >= std::uint64_t(2 * kAllocs));
	// A quiescent safepoint recycles everything the two mutators queued.
	REQUIRE_NOTHROW(instance.gcCollect());
	REQUIRE(instance.gcPendingCount() == 0);
	REQUIRE(instance.gcReclaimedObjects() >= std::uint64_t(2 * kAllocs));
}

TEST_CASE("reference-slot atomics run the retain/release barrier",
		  "[multiagent][concurrency][gc]")
{
	// SET2: a reference-typed atomic exchange is a pointer update *and* a GC
	// write barrier. These helpers are the one place that does both; this case
	// pins the count arithmetic for all three forms without needing a module
	// that combines GC types with shared-everything atomics (wasm-tools cannot
	// even print the latter yet).
	auto runtime = agentsRuntime();
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(gcRefcountModulePath());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	auto* context = reinterpret_cast<WASM::VMContext*>(&instance);

	const auto make = [&instance]() -> void* {
		void* object = instance.allocateStructObject(64, 0);
		REQUIRE(object != nullptr);
		instance.refClone(object); // the "stack" owns one count
		return object;
	};

	// The slot owns `a`; the stack owns `b`.
	void* a = make();
	void* b = make();
	void* slot = a;
	REQUIRE(instance.gcRefCount(a) == 1);
	REQUIRE(instance.gcRefCount(b) == 1);

	// set: retain the incoming value for the slot, release the overwritten
	// one. The caller still owns its stack value and releases it after.
	void* old = ::wasm_atomic_ref_store(context, &slot, b, WASM_ATOMIC_SEQCST);
	REQUIRE(old == a);
	REQUIRE(slot == b);
	REQUIRE(instance.gcRefCount(a) == 0); // queued, not freed
	REQUIRE(instance.gcRefCount(b) == 2); // slot + the stack's own count
	instance.refDrop(b);                  // the call site drops its stack value
	REQUIRE(instance.gcRefCount(b) == 1);

	// xchg: pure ownership transfer - no count adjustment.
	void* c = make();
	void* previous = ::wasm_atomic_ref_xchg(context, &slot, c, WASM_ATOMIC_SEQCST);
	REQUIRE(previous == b);
	REQUIRE(slot == c);
	REQUIRE(instance.gcRefCount(b) == 1); // now owned by the result
	REQUIRE(instance.gcRefCount(c) == 1); // moved from the stack into the slot
	instance.refDrop(previous);

	// cmpxchg success: the same transfer as xchg.
	void* d = make();
	void* observed = ::wasm_atomic_ref_cmpxchg(context, &slot, c, d, WASM_ATOMIC_SEQCST);
	REQUIRE(observed == c);
	REQUIRE(slot == d);
	REQUIRE(instance.gcRefCount(c) == 1); // the result owns the old slot count
	REQUIRE(instance.gcRefCount(d) == 1); // moved from the stack into the slot
	instance.refDrop(observed);

	// cmpxchg failure: the slot keeps its value (so the result needs its own
	// count) and the never-stored replacement gives up the stack's count.
	void* replacement = make();
	void* notExpected = nullptr;
	observed = ::wasm_atomic_ref_cmpxchg(context, &slot, notExpected, replacement,
											 WASM_ATOMIC_SEQCST);
	REQUIRE(observed == d);
	REQUIRE(slot == d);
	REQUIRE(instance.gcRefCount(d) == 2);            // slot + result
	REQUIRE(instance.gcRefCount(replacement) == 0);  // released by the failure
	instance.refDrop(observed);
	instance.refDrop(d);                             // release the slot itself

	REQUIRE_NOTHROW(instance.gcCollect());
	REQUIRE(instance.gcPendingCount() == 0);
}
