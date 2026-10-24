#include "WasmContinuation.hpp"
#include "WasmAgent.hpp"
#include "WasmExn.hpp"
#include "WasmStack.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"
#include "WasmModuleInstance.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

// Optional AddressSanitizer fiber annotations. These are weak so a normal
// build links without libasan; when present, A's stack switch updates ASAN's
// fake-stack/real-stack pairing.
extern "C" {
void __sanitizer_start_switch_fiber(void** fakeStackSave, const void* bottom,
		std::size_t size) __attribute__((weak));
void __sanitizer_finish_switch_fiber(void* fakeStack, const void** bottomOld,
		std::size_t* sizeOld) __attribute__((weak));
}

namespace WASM {

namespace {

constexpr std::size_t kContinuationStackReserve = 64 * 1024;

std::mutex& budgetMutex()
{
	static std::mutex mutex;
	return mutex;
}

bool& budgetInitialized()
{
	static bool initialized = false;
	return initialized;
}

ContinuationBudget& budgetStorage()
{
	static ContinuationBudget budget;
	return budget;
}

std::atomic<std::uint32_t>& liveCount()
{
	static std::atomic<std::uint32_t> count{0};
	return count;
}

std::atomic<std::int32_t>& providerStorage()
{
	static std::atomic<std::int32_t> provider{
		static_cast<std::int32_t>(ContinuationProvider::NativeStacks)};
	return provider;
}

void parseBudgetFromEnvironment()
{
	if (budgetInitialized())
		return;
	budgetInitialized() = true;

	const char* maxLive = std::getenv("WASMJIT_MAX_CONTINUATIONS");
	if (maxLive != nullptr && *maxLive != '\0') {
		char* end = nullptr;
		const unsigned long value = std::strtoul(maxLive, &end, 0);
		if (end != maxLive)
			budgetStorage().maxLive = static_cast<std::uint32_t>(value);
	}

	const char* stackBytes = std::getenv("WASMJIT_CONT_STACK_BYTES");
	if (stackBytes != nullptr && *stackBytes != '\0') {
		char* end = nullptr;
		const unsigned long value = std::strtoul(stackBytes, &end, 0);
		if (end != stackBytes && value != 0)
			budgetStorage().stackBytes = static_cast<std::uint32_t>(value);
	}
}

ContinuationBudget budgetSnapshot()
{
	std::lock_guard<std::mutex> lock(budgetMutex());
	parseBudgetFromEnvironment();
	return budgetStorage();
}

bool reserveContinuationSlot()
{
	std::lock_guard<std::mutex> lock(budgetMutex());
	parseBudgetFromEnvironment();
	if (liveCount().load(std::memory_order_relaxed) >= budgetStorage().maxLive)
		return false;
	liveCount().fetch_add(1, std::memory_order_relaxed);
	return true;
}

void releaseContinuationSlot()
{
	liveCount().fetch_sub(1, std::memory_order_relaxed);
}

struct Worker;

// A `(ref cont)` value is a refcounted handle. Several handles may point at one
// worker; the one-shot rule is enforced by the worker's generation (a suspended
// continuation is a new continuation identity).
struct Continuation {
	std::shared_ptr<Worker> worker;
	std::uint64_t generation;
	std::atomic<std::uint32_t> refs{1};

	Continuation(std::shared_ptr<Worker> w, std::uint64_t gen);
	~Continuation() = default;

	Continuation(const Continuation&) = delete;
	Continuation& operator=(const Continuation&) = delete;
};

std::mutex& registryMutex()
{
	static std::mutex mutex;
	return mutex;
}

std::unordered_set<Continuation*>& registry()
{
	static std::unordered_set<Continuation*> handles;
	return handles;
}

Continuation* findContinuation(const void* ref)
{
	std::lock_guard<std::mutex> lock(registryMutex());
	auto it = registry().find(const_cast<Continuation*>(static_cast<const Continuation*>(ref)));
	return it == registry().end() ? nullptr : *it;
}

enum class WorkerState {
	New,
	Running,
	Suspended,
	Completed,
	Trapped,
	Exception,
	Cancelled,
};

ModuleInstance* instanceFor(VMContext* context);
void dropContinuationValues(const std::uint8_t* kinds, std::uint32_t count,
		void* values, VMContext* context);
void invokeScalarI32(void* target, void* context,
		const ContinuationValue* args, std::uint32_t argCount,
		ContinuationValue* results, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds);

struct Worker : std::enable_shared_from_this<Worker> {
	const ContinuationProvider provider;
	Callable target{};
	VMContext* creationContext = nullptr;
	std::uint32_t paramCount = 0;
	std::uint32_t resultCount = 0;
	bool takesArg = false;
	bool returnsValue = false;
	ContinuationInvokeFn invoke = nullptr;
	std::vector<std::uint8_t> paramKinds;
	std::vector<std::uint8_t> resultKinds;
	std::vector<ContinuationValue> boundValues;
	std::vector<std::uint8_t> boundKinds;
	std::vector<ContinuationValue> resumeValues;
	std::vector<std::uint8_t> resumeKinds;
	std::vector<ContinuationValue> resultValues;
	std::vector<ContinuationValue> tagValues;
	std::vector<std::uint8_t> tagKinds;
	std::vector<void*> roots;

	void addRoot(void* ref)
	{
		if (ref == nullptr || creationContext == nullptr)
			return;
		roots.push_back(instanceFor(creationContext)->refClone(ref));
	}

	void dropRoots()
	{
		if (creationContext != nullptr) {
			auto* instance = instanceFor(creationContext);
			for (void* ref : roots)
				instance->refDrop(ref);
		}
		roots.clear();
	}

	ExecutionState execution;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> consumed{false};

	std::int32_t resumeArg = 0;
	std::int32_t suspendTag = 0;
	std::int32_t suspendValue = 0;
	std::int32_t resultValue = 0;

	bool trapped = false;
	TrapKind trapKind = TrapKind::Internal;
	std::string trapMessage;
	void* pendingException = nullptr;

	Worker(ContinuationProvider provider, Callable* target, VMContext* creationContext,
			std::uint32_t paramCount, bool takesArg, bool returnsValue)
		: provider(provider), target(target == nullptr ? Callable{} : *target),
		  creationContext(creationContext), paramCount(paramCount),
		  takesArg(takesArg), returnsValue(returnsValue) {}

	virtual ~Worker()
	{
		dropRoots();
		dropContinuationValues(boundKinds.data(), static_cast<std::uint32_t>(boundValues.size()),
				boundValues.data(), creationContext);
		dropContinuationValues(tagKinds.data(), static_cast<std::uint32_t>(tagValues.size()),
				tagValues.data(), creationContext);
		dropContinuationValues(resultKinds.data(), static_cast<std::uint32_t>(resultValues.size()),
				resultValues.data(), creationContext);
		const std::uint8_t* resumeKindsPtr =
				resumeKinds.empty() ? paramKinds.data() : resumeKinds.data();
		dropContinuationValues(resumeKindsPtr, static_cast<std::uint32_t>(resumeValues.size()),
				resumeValues.data(), creationContext);
		releaseContinuationSlot();
	}

	virtual ContinuationStatus resume(std::uint64_t expectedGeneration,
			const ContinuationValue* args, std::uint32_t argCount,
			void* injectedException) = 0;
	virtual std::int32_t suspend(std::int32_t tag,
			const ContinuationValue* args, std::uint32_t argCount,
			const std::uint8_t* argKinds,
			ContinuationValue* results, std::uint32_t resultCapacity,
			std::uint32_t* resultCount, const std::uint8_t* resultKinds) = 0;
	// Symmetric handoff: capture this worker as a fresh continuation identity
	// and transfer to `peer` with `args` (which already ends in the captured
	// continuation reference). The provider marks this worker Suspended before
	// returning; switchTo returns after the peer switches back or completes.
	virtual std::uint64_t beginSwitch() = 0;
	virtual ContinuationStatus switchTo(Worker* peer, std::uint64_t peerGeneration,
			const ContinuationValue* args, std::uint32_t argCount) = 0;
	// Bind a prefix of the remaining arguments.
	virtual bool bindArguments(const ContinuationValue* values,
			std::uint32_t count) = 0;

	// Run the target once on the current stack/state. A suspend inside the
	// target returns here only after the matching resume.
	void executeTarget();
	WorkerState outcomeState() const;
};

thread_local Worker* t_currentWorker = nullptr;

Continuation::Continuation(std::shared_ptr<Worker> w, std::uint64_t gen)
	: worker(std::move(w)), generation(gen)
{
	std::lock_guard<std::mutex> lock(registryMutex());
	registry().insert(this);
}

ContinuationStatus statusForState(WorkerState state)
{
	switch (state) {
	case WorkerState::Suspended: return ContinuationStatus::Suspended;
	case WorkerState::Completed: return ContinuationStatus::Completed;
	case WorkerState::Trapped:   return ContinuationStatus::Trapped;
	case WorkerState::Exception: return ContinuationStatus::Exception;
	default:                     return ContinuationStatus::Invalid;
	}
}
ModuleInstance* instanceFor(VMContext* context)
{
	return context == nullptr ? nullptr : reinterpret_cast<ModuleInstance*>(context);
}

void dropContinuationValues(const std::uint8_t* kinds, std::uint32_t count,
		void* values, VMContext* context)
{
	if (kinds == nullptr || values == nullptr || context == nullptr)
		return;
	auto* instance = instanceFor(context);
	auto* slots = static_cast<ContinuationValue*>(values);
	for (std::uint32_t i = 0; i < count; ++i) {
		if (kinds[i] == static_cast<std::uint8_t>(ContinuationValueKind::Ref))
			instance->refDrop(reinterpret_cast<void*>(slots[i].words[0]));
	}
}

// Scalar compatibility invoker used by the old `wasm_cont_new` wrapper and its
// tests. The generated-code path supplies a backend invoker instead.
void invokeScalarI32(void* target, void* context,
		const ContinuationValue* args, std::uint32_t argCount,
		ContinuationValue* results, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds)
{
	(void)paramKinds;
	(void)resultKinds;
	Callable copy = *static_cast<Callable*>(target);
	if (copy.context == nullptr)
		copy.context = static_cast<VMContext*>(context);
	switch (argCount) {
	case 0:
		if (resultCount != 0)
			results[0].words[0] = static_cast<std::uint64_t>(
					callCallable<std::int32_t>(copy));
		else
			callCallable<void>(copy);
		break;
	case 1:
		if (resultCount != 0)
			results[0].words[0] = static_cast<std::uint64_t>(
					callCallable<std::int32_t>(copy, static_cast<std::int32_t>(args[0].words[0])));
		else
			callCallable<void>(copy, static_cast<std::int32_t>(args[0].words[0]));
		break;
	case 2:
		if (resultCount != 0)
			results[0].words[0] = static_cast<std::uint64_t>(
					callCallable<std::int32_t>(copy,
							static_cast<std::int32_t>(args[0].words[0]),
							static_cast<std::int32_t>(args[1].words[0])));
		else
			callCallable<void>(copy,
					static_cast<std::int32_t>(args[0].words[0]),
					static_cast<std::int32_t>(args[1].words[0]));
		break;
	default:
		throw std::runtime_error("scalar continuation arity is not supported");
	}
}



void Worker::executeTarget()
{
	trapped = false;
	trapMessage.clear();
	pendingException = nullptr;
	resultValue = 0;
	resultValues.clear();
	if (resultCount != 0)
		resultValues.resize(resultCount);

	if (target.fnPtr == nullptr) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = "continuation target has no entry point";
		std::snprintf(execution.trapSlot.message, sizeof(execution.trapSlot.message),
				"%s", trapMessage.c_str());
		execution.trapSlot.kind = static_cast<int>(trapKind);
		execution.trapSlot.pending = 1;
		return;
	}
	if (invoke == nullptr) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = "continuation target has no invoker";
		std::snprintf(execution.trapSlot.message, sizeof(execution.trapSlot.message),
				"%s", trapMessage.c_str());
		execution.trapSlot.kind = static_cast<int>(trapKind);
		execution.trapSlot.pending = 1;
		return;
	}

	std::vector<ContinuationValue> callArgs = boundValues;
	callArgs.insert(callArgs.end(), resumeValues.begin(), resumeValues.end());
	boundValues.clear();
	boundKinds.clear();
	resumeValues.clear();
	if (callArgs.size() > paramCount)
		callArgs.resize(paramCount);
	if (callArgs.size() != paramCount) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = "continuation argument count does not match its type";
		std::snprintf(execution.trapSlot.message, sizeof(execution.trapSlot.message),
				"%s", trapMessage.c_str());
		execution.trapSlot.kind = static_cast<int>(trapKind);
		execution.trapSlot.pending = 1;
		return;
	}

	try {
		invoke(&target, creationContext, callArgs.data(),
				static_cast<std::uint32_t>(callArgs.size()),
				resultValues.empty() ? nullptr : resultValues.data(), resultCount,
				paramKinds.empty() ? nullptr : paramKinds.data(),
				resultKinds.empty() ? nullptr : resultKinds.data());
		if (!resultValues.empty())
			resultValue = static_cast<std::int32_t>(resultValues[0].words[0]);
	} catch (const Trap& trap) {
		trapped = true;
		trapKind = trap.kind();
		trapMessage = trap.message();
	} catch (const std::exception& error) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = error.what();
	} catch (...) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = "continuation worker failed";
	}

	if (!trapped && execution.pendingException != nullptr)
		pendingException = execution.pendingException;
	if (!trapped && execution.trapSlot.pending != 0) {
		trapKind = static_cast<TrapKind>(execution.trapSlot.kind);
		trapMessage.assign(execution.trapSlot.message,
				strnlen(execution.trapSlot.message, sizeof(execution.trapSlot.message)));
	}
}

WorkerState Worker::outcomeState() const
{
	if (trapped || execution.trapSlot.pending != 0)
		return WorkerState::Trapped;
	if (pendingException != nullptr)
		return WorkerState::Exception;
	return WorkerState::Completed;
}

struct BWorker final : Worker {
	std::mutex mutex;
	std::condition_variable cv;
	pthread_t thread{};
	bool threadStarted = false;
	bool stackReserved = false;
	std::uint32_t stackBytes = 0;
	bool cancelRequested = false;
	WorkerState state = WorkerState::New;
	BWorker* switchPeer = nullptr;

	BWorker(Callable* target, VMContext* context, std::uint32_t paramCount,
			bool takesArg, bool returnsValue)
		: Worker(ContinuationProvider::HostThreads, target, context, paramCount,
				takesArg, returnsValue) {}

	~BWorker() override;

	ContinuationStatus resume(std::uint64_t expectedGeneration,
			const ContinuationValue* args, std::uint32_t argCount,
			void* injectedException) override;
	std::int32_t suspend(std::int32_t tag,
			const ContinuationValue* args, std::uint32_t argCount,
			const std::uint8_t* argKinds,
			ContinuationValue* results, std::uint32_t resultCapacity,
			std::uint32_t* resultCount, const std::uint8_t* resultKinds) override;
	std::uint64_t beginSwitch() override;
	ContinuationStatus switchTo(Worker* peer, std::uint64_t peerGeneration,
			const ContinuationValue* args, std::uint32_t argCount) override;
	bool bindArguments(const ContinuationValue* values, std::uint32_t count) override;

	void run();
	bool startThread();
	ContinuationStatus resumeSwitchPeer(std::uint64_t expectedGeneration,
			const ContinuationValue* args, std::uint32_t argCount,
			void* injectedException);
};

struct AWorker final : Worker {
	ucontext_t context{};
	ucontext_t resumer{};
	bool contextReady = false;
	void* mapping = nullptr;
	std::size_t mappingBytes = 0;
	void* stackBase = nullptr;
	std::size_t stackSize = 0;
	bool stackReserved = false;
	std::thread::id owner{};
	ExecutionState* previousState = nullptr;
	Worker* previousWorker = nullptr;
	bool cancelRequested = false;
	WorkerState state = WorkerState::New;
	AWorker* switchPeer = nullptr;
	ucontext_t switchContext;
	bool switchContextValid = false;
	bool switchResume = false;
	void* fakeStack = nullptr;
	void* resumerFakeStack = nullptr;
	const void* resumerBottom = nullptr;
	std::size_t resumerSize = 0;

	AWorker(Callable* target, VMContext* context, std::uint32_t paramCount,
			bool takesArg, bool returnsValue)
		: Worker(ContinuationProvider::NativeStacks, target, context, paramCount,
				takesArg, returnsValue) {}

	~AWorker() override;

	ContinuationStatus resume(std::uint64_t expectedGeneration,
			const ContinuationValue* args, std::uint32_t argCount,
			void* injectedException) override;
	std::int32_t suspend(std::int32_t tag,
			const ContinuationValue* args, std::uint32_t argCount,
			const std::uint8_t* argKinds,
			ContinuationValue* results, std::uint32_t resultCapacity,
			std::uint32_t* resultCount, const std::uint8_t* resultKinds) override;
	std::uint64_t beginSwitch() override;
	ContinuationStatus switchTo(Worker* peer, std::uint64_t peerGeneration,
			const ContinuationValue* args, std::uint32_t argCount) override;
	bool bindArguments(const ContinuationValue* values, std::uint32_t count) override;

	static void entryThunk();
	void entry();
	bool prepareStack();
	void switchToResumer();
	bool cancelSuspended();
	void sanitizerFinishInFiber();
	void sanitizerStartToResumer();
	void sanitizerStartToFiber();
	void sanitizerFinishInResumer();
};

thread_local AWorker* t_startingAWorker = nullptr;
BWorker::~BWorker()
{
	if (threadStarted) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			cancelRequested = true;
			if (state == WorkerState::Suspended || state == WorkerState::Running) {
				state = WorkerState::Cancelled;
				cv.notify_all();
			}
		}
		pthread_join(thread, nullptr);
		threadStarted = false;
	}
	if (stackReserved)
		Store::global().releaseBytes(stackBytes);
}

ContinuationStatus BWorker::resume(std::uint64_t expectedGeneration,
		const ContinuationValue* args, std::uint32_t argCount,
		void* injectedException)
{
	if (switchPeer != nullptr)
		return resumeSwitchPeer(expectedGeneration, args, argCount, injectedException);
	std::unique_lock<std::mutex> lock(mutex);
	if (cancelRequested || state == WorkerState::Cancelled) {
		lock.unlock();
		reportTrap(TrapKind::Internal, "continuation was cancelled");
		return ContinuationStatus::Invalid;
	}
	if (generation.load(std::memory_order_relaxed) != expectedGeneration
			|| consumed.load(std::memory_order_relaxed)) {
		lock.unlock();
		reportTrap(TrapKind::Internal, "continuation has already been consumed");
		return ContinuationStatus::Invalid;
	}
	if (state != WorkerState::New && state != WorkerState::Suspended) {
		lock.unlock();
		reportTrap(TrapKind::Internal, "continuation is not resumable");
		return ContinuationStatus::Invalid;
	}
	if (injectedException != nullptr && state == WorkerState::New) {
		lock.unlock();
		reportTrap(TrapKind::Internal, "cannot throw into a fresh continuation");
		return ContinuationStatus::Invalid;
	}

	const bool start = (state == WorkerState::New);
	consumed.store(true, std::memory_order_relaxed);
	state = WorkerState::Running;
	resumeValues.clear();
	if (argCount != 0 && args != nullptr)
		resumeValues.assign(args, args + argCount);
	if (!resumeValues.empty())
		resumeArg = static_cast<std::int32_t>(resumeValues.front().words[0]);
	execution.pendingException = injectedException;
	if (start) {
		if (!startThread()) {
			state = WorkerState::Cancelled;
			lock.unlock();
			reportTrap(TrapKind::OutOfMemory, "continuation worker could not start");
			return ContinuationStatus::Invalid;
		}
	} else {
		cv.notify_all();
	}
	cv.wait(lock, [this] { return state != WorkerState::Running; });
	return statusForState(state);
}

std::int32_t BWorker::suspend(std::int32_t tag,
		const ContinuationValue* args, std::uint32_t argCount,
		const std::uint8_t* argKinds,
		ContinuationValue* results, std::uint32_t resultCapacity,
		std::uint32_t* resultCount, const std::uint8_t* resultKinds)
{
	std::unique_lock<std::mutex> lock(mutex);
	if (cancelRequested || state == WorkerState::Cancelled) {
		reportTrap(TrapKind::Internal, "continuation dropped while suspended");
		return 0;
	}
	suspendTag = tag;
	suspendValue = argCount == 0 ? 0 : static_cast<std::int32_t>(args[0].words[0]);
	tagValues.assign(args, args + argCount);
	tagKinds.clear();
	if (argCount != 0 && argKinds != nullptr)
		tagKinds.assign(argKinds, argKinds + argCount);
	resumeKinds.clear();
	if (resultCapacity != 0 && resultKinds != nullptr)
		resumeKinds.assign(resultKinds, resultKinds + resultCapacity);
	generation.fetch_add(1, std::memory_order_relaxed);
	consumed.store(false, std::memory_order_relaxed);
	state = WorkerState::Suspended;
	cv.notify_all();
	cv.wait(lock, [this] { return state != WorkerState::Suspended; });
	if (cancelRequested || state == WorkerState::Cancelled) {
		reportTrap(TrapKind::Internal, "continuation dropped while suspended");
		return 0;
	}

	dropRoots();
	std::vector<ContinuationValue> out = boundValues;
	out.insert(out.end(), resumeValues.begin(), resumeValues.end());
	boundValues.clear();
	boundKinds.clear();
	resumeValues.clear();
	if (resultCount != nullptr)
		*resultCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(out.size(), resultCapacity));
	if (results != nullptr && resultCapacity != 0) {
		for (std::uint32_t i = 0; i < *resultCount; ++i)
			results[i] = out[i];
	}
	return 1;
}

std::uint64_t BWorker::beginSwitch()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != WorkerState::Running) {
		reportTrap(TrapKind::Internal, "switch outside a running continuation");
		return 0;
	}
	state = WorkerState::Suspended;
	consumed.store(false, std::memory_order_relaxed);
	return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

ContinuationStatus BWorker::resumeSwitchPeer(std::uint64_t expectedGeneration,
		const ContinuationValue* args, std::uint32_t argCount,
		void* injectedException)
{
	BWorker* caller = static_cast<BWorker*>(t_currentWorker);
	if (caller == nullptr || caller->provider != provider) {
		reportTrap(TrapKind::Internal, "cross-provider switch is not supported");
		return ContinuationStatus::Invalid;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (generation.load(std::memory_order_relaxed) != expectedGeneration
				|| consumed.load(std::memory_order_relaxed)) {
			reportTrap(TrapKind::Internal, "continuation has already been consumed");
			return ContinuationStatus::Invalid;
		}
		if (state != WorkerState::Suspended) {
			reportTrap(TrapKind::Internal, "continuation is not resumable");
			return ContinuationStatus::Invalid;
		}
		consumed.store(true, std::memory_order_relaxed);
		state = WorkerState::Running;
		resumeValues.clear();
		if (argCount != 0 && args != nullptr)
			resumeValues.assign(args, args + argCount);
		if (!resumeValues.empty())
			resumeArg = static_cast<std::int32_t>(resumeValues.front().words[0]);
		execution.pendingException = injectedException;
	}
	// The caller is blocked in `switchTo` -> `caller->resume`, waiting on
	// `caller->cv` for its state to leave Running. The caller already marked
	// itself Suspended in beginSwitch, so notifying is enough to release it.
	std::unique_lock<std::mutex> callerLock(caller->mutex);
	caller->cv.notify_all();
	caller->cv.wait(callerLock, [caller] {
		return caller->state == WorkerState::Running;
	});
	return ContinuationStatus::Completed;
}

ContinuationStatus BWorker::switchTo(Worker* peer, std::uint64_t peerGeneration,
		const ContinuationValue* args, std::uint32_t argCount)
{
	if (peer == nullptr || peer->provider != provider) {
		reportTrap(TrapKind::Internal, "cross-provider switch is not supported");
		return ContinuationStatus::Invalid;
	}
	BWorker* target = static_cast<BWorker*>(peer);
	switchPeer = target;
	const ContinuationStatus status = target->resume(peerGeneration, args, argCount, nullptr);
	if (state == WorkerState::Running) {
		switchPeer = nullptr;
		dropRoots();
		return ContinuationStatus::Completed;
	}
	switchPeer = nullptr;
	return state == WorkerState::Suspended ? ContinuationStatus::Suspended : status;
}

bool BWorker::bindArguments(const ContinuationValue* values, std::uint32_t count)
{
	std::lock_guard<std::mutex> lock(mutex);
	if ((state != WorkerState::New && state != WorkerState::Suspended) || values == nullptr)
		return false;
	const std::vector<std::uint8_t>& kinds =
			state == WorkerState::New ? paramKinds : tagKinds;
	if (boundValues.size() + count > kinds.size())
		return false;
	boundValues.insert(boundValues.end(), values, values + count);
	boundKinds.insert(boundKinds.end(), kinds.begin() + boundValues.size() - count,
			kinds.begin() + boundValues.size());
	if (state == WorkerState::New)
		takesArg = boundValues.size() < paramCount;
	generation.fetch_add(1, std::memory_order_relaxed);
	consumed.store(false, std::memory_order_relaxed);
	return true;
}

void* bWorkerEntry(void* argument)
{
	static_cast<BWorker*>(argument)->run();
	return nullptr;
}

void BWorker::run()
{
	t_currentWorker = this;
	{
		ExecutionStateScope scope(execution);
		std::uintptr_t lowest = 0;
		std::uintptr_t highest = 0;
		if (currentThreadStackBounds(lowest, highest)
				&& highest - lowest > kContinuationStackReserve * 2) {
			execution.stackLimit = lowest + kContinuationStackReserve;
		} else {
			// Do not fall back to the instance's VMContext limit: it belongs to
			// a different stack mapping. The pthread stack size bounds us.
			execution.stackLimit = 1;
		}

		executeTarget();
		const WorkerState next = outcomeState();
		t_currentWorker = nullptr;
		std::lock_guard<std::mutex> lock(mutex);
		state = next;
		cv.notify_all();
	}
}

bool BWorker::startThread()
{
	const ContinuationBudget budget = budgetSnapshot();
	stackBytes = std::max<std::uint32_t>(budget.stackBytes, 64 * 1024);
	if (!Store::global().reserveBytes(stackBytes))
		return false;

	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes) != 0) {
		Store::global().releaseBytes(stackBytes);
		return false;
	}
	if (pthread_attr_setstacksize(&attributes, stackBytes) != 0) {
		pthread_attr_destroy(&attributes);
		Store::global().releaseBytes(stackBytes);
		return false;
	}
	const int created = pthread_create(&thread, &attributes, &bWorkerEntry, this);
	pthread_attr_destroy(&attributes);
	if (created != 0) {
		Store::global().releaseBytes(stackBytes);
		return false;
	}
	stackReserved = true;
	threadStarted = true;
	return true;
}

AWorker::~AWorker()
{
	bool safeToFree = true;
	if (contextReady && (state == WorkerState::Suspended || state == WorkerState::Running))
		safeToFree = cancelSuspended();
	if (mapping != nullptr && safeToFree) {
		munmap(mapping, mappingBytes);
		mapping = nullptr;
		if (stackReserved)
			Store::global().releaseBytes(mappingBytes);
	} else if (mapping != nullptr) {
		// A continuation is thread-affine: a stack suspended on another host
		// thread cannot be unwound here. Leak the mapping rather than free a
		// stack that still contains live frames.
		std::fprintf(stderr,
				"WasmJit: continuation dropped on a foreign host thread; "
				"leaving its native stack mapped\n");
	}
}

bool AWorker::prepareStack()
{
	const ContinuationBudget budget = budgetSnapshot();
	const long rawPage = sysconf(_SC_PAGESIZE);
	if (rawPage <= 0)
		return false;
	const std::size_t page = static_cast<std::size_t>(rawPage);
	std::size_t usable = std::max<std::uint32_t>(budget.stackBytes, 64 * 1024);
	usable = (usable + page - 1) & ~(page - 1);
	const std::size_t total = usable + page;
	if (!Store::global().reserveBytes(total))
		return false;

	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
	flags |= MAP_STACK;
#endif
	void* mapped = mmap(nullptr, total, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (mapped == MAP_FAILED) {
		Store::global().releaseBytes(total);
		return false;
	}
	if (mprotect(mapped, page, PROT_NONE) != 0) {
		munmap(mapped, total);
		Store::global().releaseBytes(total);
		return false;
	}
	if (getcontext(&context) != 0) {
		munmap(mapped, total);
		Store::global().releaseBytes(total);
		return false;
	}

	mapping = mapped;
	mappingBytes = total;
	stackBase = static_cast<std::uint8_t*>(mapped) + page;
	stackSize = usable;
	stackReserved = true;
	context.uc_stack.ss_sp = stackBase;
	context.uc_stack.ss_size = stackSize;
	context.uc_stack.ss_flags = 0;
	context.uc_link = nullptr;
	makecontext(&context, &AWorker::entryThunk, 0);
	contextReady = true;
	owner = std::this_thread::get_id();
	execution.stackLimit = stackSize > kContinuationStackReserve * 2
			? reinterpret_cast<std::uintptr_t>(stackBase) + kContinuationStackReserve
			: 1;
	return true;
}

void AWorker::entryThunk()
{
	AWorker* self = t_startingAWorker;
	if (self == nullptr)
		std::abort();
	t_startingAWorker = nullptr;
	self->sanitizerFinishInFiber();
	self->entry();
}

void AWorker::sanitizerFinishInFiber()
{
	if (__sanitizer_finish_switch_fiber != nullptr)
		__sanitizer_finish_switch_fiber(fakeStack, nullptr, nullptr);
}

void AWorker::sanitizerStartToResumer()
{
	if (__sanitizer_start_switch_fiber != nullptr)
		__sanitizer_start_switch_fiber(&fakeStack, resumerBottom, resumerSize);
}

void AWorker::sanitizerStartToFiber()
{
	if (__sanitizer_start_switch_fiber != nullptr)
		__sanitizer_start_switch_fiber(&resumerFakeStack, stackBase, stackSize);
}

void AWorker::sanitizerFinishInResumer()
{
	if (__sanitizer_finish_switch_fiber != nullptr)
		__sanitizer_finish_switch_fiber(resumerFakeStack, nullptr, nullptr);
}

void AWorker::entry()
{
	executeTarget();
	state = outcomeState();
	switchToResumer();
	std::abort(); // a completed continuation must never be resumed
}

void AWorker::switchToResumer()
{
	sanitizerStartToResumer();
	setCurrentExecutionState(previousState);
	t_currentWorker = previousWorker;
	swapcontext(&context, &resumer);
}

ContinuationStatus AWorker::resume(std::uint64_t expectedGeneration,
		const ContinuationValue* args, std::uint32_t argCount,
		void* injectedException)
{
	if (contextReady && owner != std::this_thread::get_id()) {
		// A suspended native stack is not tied to the OS thread that first ran
		// it: the engine TLS and backend state are switched with the stack. Keep
		// the owner only for diagnostics; serial migration is allowed.
		owner = std::this_thread::get_id();
	}
	if (!contextReady && !prepareStack()) {
		reportTrap(TrapKind::OutOfMemory, "continuation stack could not be allocated");
		return ContinuationStatus::Invalid;
	}
	if (generation.load(std::memory_order_relaxed) != expectedGeneration
			|| consumed.load(std::memory_order_relaxed)) {
		reportTrap(TrapKind::Internal, "continuation has already been consumed");
		return ContinuationStatus::Invalid;
	}
	if (state != WorkerState::New && state != WorkerState::Suspended) {
		reportTrap(TrapKind::Internal, "continuation is not resumable");
		return ContinuationStatus::Invalid;
	}
	if (injectedException != nullptr && state == WorkerState::New) {
		reportTrap(TrapKind::Internal, "cannot throw into a fresh continuation");
		return ContinuationStatus::Invalid;
	}

	if (switchPeer != nullptr) {
		AWorker* peer = switchPeer;
		const bool asSwitchTarget = switchResume;
		switchResume = false;
		consumed.store(true, std::memory_order_relaxed);
		state = WorkerState::Running;
		resumeValues.clear();
		if (argCount != 0 && args != nullptr)
			resumeValues.assign(args, args + argCount);
		if (!resumeValues.empty())
			resumeArg = static_cast<std::int32_t>(resumeValues.front().words[0]);
		execution.pendingException = injectedException;
		setCurrentExecutionState(&execution);
		t_currentWorker = this;
		sanitizerStartToFiber();
		if (asSwitchTarget) {
			// The peer's `switch` is resuming us. Keep this worker's original
			// resumer: when it returns, it belongs to the prompt that started
			// its switch, not to the target that resumed it. The tail runs on
			// the peer's stack and must restore the peer's execution state.
			auto* tailState = &peer->execution;
			Worker* tailWorker = peer;
			peer->switchContextValid = true;
			swapcontext(&peer->switchContext, &peer->resumer);
			sanitizerFinishInFiber();
			setCurrentExecutionState(tailState);
			t_currentWorker = tailWorker;
			return statusForState(state);
		}
		// An explicit resume of a switch caller (the B-style `resume`
		// hand-back). The current caller becomes the resumer, and its saved
		// stack is the context captured by the peer's switch.
		previousState = &peer->execution;
		previousWorker = peer;
		auto* tailState = previousState;
		Worker* tailWorker = previousWorker;
		if (!switchContextValid)
			swapcontext(&resumer, &peer->resumer);
		else
			swapcontext(&resumer, &switchContext);
		sanitizerFinishInFiber();
		setCurrentExecutionState(tailState);
		t_currentWorker = tailWorker;
		return statusForState(state);
	}

	const bool first = (state == WorkerState::New);
	resumeValues.clear();
	if (argCount != 0 && args != nullptr)
		resumeValues.assign(args, args + argCount);
	if (!resumeValues.empty())
		resumeArg = static_cast<std::int32_t>(resumeValues.front().words[0]);
	consumed.store(true, std::memory_order_relaxed);
	state = WorkerState::Running;
	execution.pendingException = injectedException;
	previousState = &currentExecutionState();
	previousWorker = t_currentWorker;
	std::uintptr_t lowest = 0;
	std::uintptr_t highest = 0;
	if (currentThreadStackBounds(lowest, highest)) {
		resumerBottom = reinterpret_cast<const void*>(lowest);
		resumerSize = highest - lowest;
	}
	setCurrentExecutionState(&execution);
	t_currentWorker = this;
	if (first)
		t_startingAWorker = this;
	sanitizerStartToFiber();
	swapcontext(&resumer, &context);
	sanitizerFinishInResumer();
	setCurrentExecutionState(previousState);
	t_currentWorker = previousWorker;
	previousState = nullptr;
	previousWorker = nullptr;
	const ContinuationStatus result = statusForState(state);
	return result;
}

std::int32_t AWorker::suspend(std::int32_t tag,
		const ContinuationValue* args, std::uint32_t argCount,
		const std::uint8_t* argKinds,
		ContinuationValue* results, std::uint32_t resultCapacity,
		std::uint32_t* resultCount, const std::uint8_t* resultKinds)
{
	if (previousState == nullptr) {
		reportTrap(TrapKind::Internal, "suspend outside its continuation thread");
		return 0;
	}
	suspendTag = tag;
	suspendValue = argCount == 0 ? 0 : static_cast<std::int32_t>(args[0].words[0]);
	tagValues.assign(args, args + argCount);
	tagKinds.clear();
	if (argCount != 0 && argKinds != nullptr)
		tagKinds.assign(argKinds, argKinds + argCount);
	resumeKinds.clear();
	if (resultCapacity != 0 && resultKinds != nullptr)
		resumeKinds.assign(resultKinds, resultKinds + resultCapacity);
	generation.fetch_add(1, std::memory_order_relaxed);
	consumed.store(false, std::memory_order_relaxed);
	state = WorkerState::Suspended;
	switchToResumer();
	sanitizerFinishInFiber();
	setCurrentExecutionState(&execution);
	t_currentWorker = this;
	if (cancelRequested) {
		reportTrap(TrapKind::Internal, "continuation dropped while suspended");
		return 0;
	}

	dropRoots();
	std::vector<ContinuationValue> out = boundValues;
	out.insert(out.end(), resumeValues.begin(), resumeValues.end());
	boundValues.clear();
	boundKinds.clear();
	resumeValues.clear();
	if (resultCount != nullptr)
		*resultCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(out.size(), resultCapacity));
	if (results != nullptr && resultCapacity != 0) {
		for (std::uint32_t i = 0; i < *resultCount; ++i)
			results[i] = out[i];
	}
	return 1;
}

std::uint64_t AWorker::beginSwitch()
{
	if (state != WorkerState::Running) {
		reportTrap(TrapKind::Internal, "switch outside a running continuation");
		return 0;
	}
	state = WorkerState::Suspended;
	consumed.store(false, std::memory_order_relaxed);
	return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

ContinuationStatus AWorker::switchTo(Worker* peer, std::uint64_t peerGeneration,
		const ContinuationValue* args, std::uint32_t argCount)
{
	if (peer == nullptr || peer->provider != provider) {
		reportTrap(TrapKind::Internal, "cross-provider switch is not supported");
		return ContinuationStatus::Invalid;
	}
	AWorker* target = static_cast<AWorker*>(peer);
	switchPeer = target;
	target->switchResume = target->state != WorkerState::New;
	const ContinuationStatus status = target->resume(peerGeneration, args, argCount, nullptr);
	if (state == WorkerState::Running) {
		switchPeer = nullptr;
		dropRoots();
		return ContinuationStatus::Completed;
	}
	switchPeer = nullptr;
	return state == WorkerState::Suspended ? ContinuationStatus::Suspended : status;
}

bool AWorker::bindArguments(const ContinuationValue* values, std::uint32_t count)
{
	if ((state != WorkerState::New && state != WorkerState::Suspended) || values == nullptr)
		return false;
	const std::vector<std::uint8_t>& kinds =
			state == WorkerState::New ? paramKinds : tagKinds;
	if (boundValues.size() + count > kinds.size())
		return false;
	boundValues.insert(boundValues.end(), values, values + count);
	boundKinds.insert(boundKinds.end(), kinds.begin() + boundValues.size() - count,
			kinds.begin() + boundValues.size());
	if (state == WorkerState::New)
		takesArg = boundValues.size() < paramCount;
	generation.fetch_add(1, std::memory_order_relaxed);
	consumed.store(false, std::memory_order_relaxed);
	return true;
}

bool AWorker::cancelSuspended()
{
	if (state == WorkerState::Running)
		return false;
	if (state != WorkerState::Suspended || !contextReady)
		return true;
	owner = std::this_thread::get_id();
	previousState = &currentExecutionState();
	previousWorker = t_currentWorker;
	std::uintptr_t lowest = 0;
	std::uintptr_t highest = 0;
	if (currentThreadStackBounds(lowest, highest)) {
		resumerBottom = reinterpret_cast<const void*>(lowest);
		resumerSize = highest - lowest;
	}
	cancelRequested = true;
	state = WorkerState::Running;
	setCurrentExecutionState(&execution);
	t_currentWorker = this;
	sanitizerStartToFiber();
	swapcontext(&resumer, &context);
	sanitizerFinishInResumer();
	setCurrentExecutionState(previousState);
	t_currentWorker = previousWorker;
	previousState = nullptr;
	previousWorker = nullptr;
	return true;
}
} // namespace

ContinuationBudget continuationBudget() noexcept
{
	return budgetSnapshot();
}

void setContinuationBudget(ContinuationBudget budget) noexcept
{
	std::lock_guard<std::mutex> lock(budgetMutex());
	budgetStorage() = budget;
	budgetInitialized() = true;
}

ContinuationProvider continuationProvider() noexcept
{
	const std::int32_t value = providerStorage().load(std::memory_order_relaxed);
	return value == static_cast<std::int32_t>(ContinuationProvider::HostThreads)
			? ContinuationProvider::HostThreads
			: ContinuationProvider::NativeStacks;
}

void setContinuationProvider(ContinuationProvider provider) noexcept
{
	providerStorage().store(static_cast<std::int32_t>(provider),
			std::memory_order_relaxed);
}

std::uint32_t liveContinuationCount() noexcept
{
	return liveCount().load(std::memory_order_relaxed);
}

bool isContinuationRef(const void* ref) noexcept
{
	return ref != nullptr && findContinuation(ref) != nullptr;
}

bool continuationRefRetain(void* ref) noexcept
{
	if (ref == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(registryMutex());
	auto it = registry().find(static_cast<Continuation*>(ref));
	if (it == registry().end())
		return false;
	(*it)->refs.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool continuationRefRelease(void* ref) noexcept
{
	if (ref == nullptr)
		return false;
	Continuation* dead = nullptr;
	{
		std::lock_guard<std::mutex> lock(registryMutex());
		auto it = registry().find(static_cast<Continuation*>(ref));
		if (it == registry().end())
			return false;
		Continuation* handle = *it;
		if (handle->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			registry().erase(it);
			dead = handle;
		}
	}
	delete dead;
	return true;
}

HostContinuation::HostContinuation(void* ref, bool takeOwnership) noexcept
{
	if (!isContinuationRef(ref))
		return;
	if (takeOwnership) {
		ref_ = ref;
	} else if (continuationRefRetain(ref)) {
		ref_ = ref;
	}
}

HostContinuation::~HostContinuation()
{
	reset();
}

HostContinuation::HostContinuation(HostContinuation&& other) noexcept
	: ref_(other.ref_)
{
	other.ref_ = nullptr;
}

HostContinuation& HostContinuation::operator=(HostContinuation&& other) noexcept
{
	if (this != &other) {
		reset();
		ref_ = other.ref_;
		other.ref_ = nullptr;
	}
	return *this;
}

void HostContinuation::reset() noexcept
{
	if (ref_ != nullptr) {
		continuationRefRelease(ref_);
		ref_ = nullptr;
	}
}

bool HostContinuation::resume(std::int32_t arg, ContinuationResumeResult* result) noexcept
{
	if (ref_ == nullptr || result == nullptr)
		return false;
	wasm_cont_resume(ref_, arg, result);
	// `resume` consumes the identity; release the host's handle regardless of
	// the status. A suspended replacement is in `result->continuation`.
	continuationRefRelease(ref_);
	ref_ = nullptr;
	return true;
}

std::int32_t HostContinuation::suspendCurrent(std::int32_t tag, std::int32_t value,
		std::int32_t* resumedValue) noexcept
{
	return wasm_cont_suspend(tag, value, resumedValue);
}

HostContinuationScheduler::~HostContinuationScheduler()
{
	clear();
}

bool HostContinuationScheduler::submit(void* continuation, std::int32_t arg,
		bool takeOwnership) noexcept
{
	if (!isContinuationRef(continuation))
		return false;
	if (!takeOwnership && !continuationRefRetain(continuation))
		return false;
	tasks_.push_back(Task{continuation, arg});
	return true;
}

void HostContinuationScheduler::clear() noexcept
{
	for (const Task& task : tasks_) {
		if (task.continuation != nullptr)
			continuationRefRelease(task.continuation);
	}
	tasks_.clear();
}

bool HostContinuationScheduler::step() noexcept
{
	if (tasks_.empty())
		return false;
	Task task = tasks_.front();
	tasks_.pop_front();

	HostContinuation handle(task.continuation, true);
	ContinuationResumeResult result;
	if (!handle.resume(task.arg, &result))
		return true;

	if (result.status == static_cast<std::int32_t>(ContinuationStatus::Suspended)) {
		if (result.continuation != nullptr) {
			if (hooks_.onSuspended != nullptr)
				hooks_.onSuspended(hooks_.userData, result.continuation,
						result.tagId, result.value);
			else
				continuationRefRelease(result.continuation);
		}
	} else if (hooks_.onCompleted != nullptr) {
		hooks_.onCompleted(hooks_.userData, result.status);
	}
	return true;
}

std::size_t HostContinuationScheduler::run() noexcept
{
	std::size_t count = 0;
	while (step())
		++count;
	return count;
}

extern "C" {

void* wasm_cont_new(void* callable, void* context, std::uint32_t paramCount,
		std::int32_t takesArg, std::int32_t returnsValue)
{
	const std::uint8_t i32Kind = static_cast<std::uint8_t>(ContinuationValueKind::I32);
	const std::uint8_t* paramKinds = takesArg != 0 ? &i32Kind : nullptr;
	const std::uint8_t* resultKinds = returnsValue != 0 ? &i32Kind : nullptr;
	return wasm_cont_new_ex(callable, context, invokeScalarI32, paramCount,
			returnsValue != 0 ? 1u : 0u, paramKinds, resultKinds);
}

void* wasm_cont_new_ex(void* callable, void* context, ContinuationInvokeFn invoke,
		std::uint32_t paramCount, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds)
{
	if (callable == nullptr) {
		reportTrap(TrapKind::NullFunctionReference, "null continuation target");
		return nullptr;
	}
	if (!reserveContinuationSlot()) {
		reportTrap(TrapKind::OutOfMemory, "continuation budget exhausted");
		return nullptr;
	}

	std::shared_ptr<Worker> worker;
	try {
		if (continuationProvider() == ContinuationProvider::HostThreads) {
			worker = std::make_shared<BWorker>(static_cast<Callable*>(callable),
					static_cast<VMContext*>(context), paramCount, paramCount != 0,
					resultCount != 0);
		} else {
			worker = std::make_shared<AWorker>(static_cast<Callable*>(callable),
					static_cast<VMContext*>(context), paramCount, paramCount != 0,
					resultCount != 0);
		}
		worker->invoke = invoke;
		worker->resultCount = resultCount;
		if (paramKinds != nullptr)
			worker->paramKinds.assign(paramKinds, paramKinds + paramCount);
		if (resultKinds != nullptr)
			worker->resultKinds.assign(resultKinds, resultKinds + resultCount);
	} catch (...) {
		releaseContinuationSlot();
		reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		return nullptr;
	}
	try {
		return new Continuation(std::move(worker), 0);
	} catch (...) {
		// The worker's destructor releases the reserved live slot.
		reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		return nullptr;
	}
}

void* wasm_cont_bind(void* continuation, const std::int32_t* values,
		std::uint32_t count)
{
	std::vector<ContinuationValue> wide(count);
	for (std::uint32_t i = 0; i < count; ++i)
		wide[i].words[0] = static_cast<std::uint64_t>(values[i]);
	return wasm_cont_bind_ex(continuation, wide.data(), count);
}

void* wasm_cont_bind_ex(void* continuation, const ContinuationValue* values,
		std::uint32_t count)
{
	if (continuation == nullptr) {
		reportTrap(TrapKind::NullReference, "null continuation");
		return nullptr;
	}
	Continuation* handle = findContinuation(continuation);
	if (handle == nullptr) {
		reportTrap(TrapKind::Internal, "invalid continuation reference");
		return nullptr;
	}
	if (!handle->worker->bindArguments(values, count)) {
		reportTrap(TrapKind::Internal, "continuation cannot bind these arguments");
		return nullptr;
	}
	try {
		return new Continuation(handle->worker,
				handle->worker->generation.load(std::memory_order_relaxed));
	} catch (...) {
		reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		return nullptr;
	}
}

void wasm_cont_resume(void* continuation, std::int32_t arg,
		ContinuationResumeResult* result)
{
	ContinuationValue argValue;
	argValue.words[0] = static_cast<std::uint64_t>(arg);
	ContinuationValue resultValue;
	ContinuationValue tagValue;
	wasm_cont_resume_ex(continuation, &argValue, 1, &resultValue, 1,
			&tagValue, 1, result);
}

void wasm_cont_resume_ex(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationValue* tagValues,
		std::uint32_t tagCapacity, ContinuationResumeResult* result)
{
	if (result == nullptr)
		return;
	*result = ContinuationResumeResult{};
	if (continuation == nullptr) {
		reportTrap(TrapKind::NullReference, "null continuation");
		return;
	}
	Continuation* handle = findContinuation(continuation);
	if (handle == nullptr) {
		reportTrap(TrapKind::Internal, "invalid continuation reference");
		return;
	}

	const ContinuationStatus status = handle->worker->resume(handle->generation,
			args, argCount, nullptr);
	result->status = static_cast<std::int32_t>(status);
	switch (status) {
	case ContinuationStatus::Suspended:
		result->tagId = handle->worker->suspendTag;
		result->tagValueCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(handle->worker->tagValues.size(), tagCapacity));
		result->tagValues = tagValues;
		result->tagKinds = handle->worker->tagKinds.empty()
				? nullptr : handle->worker->tagKinds.data();
		for (std::uint32_t i = 0; i < result->tagValueCount; ++i)
			tagValues[i] = handle->worker->tagValues[i];
		handle->worker->tagValues.clear();
		result->value = result->tagValueCount == 0 ? 0
				: static_cast<std::int32_t>(tagValues[0].words[0]);
		try {
			result->continuation = new Continuation(handle->worker,
					handle->worker->generation.load(std::memory_order_relaxed));
		} catch (...) {
			reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		}
		break;
	case ContinuationStatus::Completed:
		result->resultCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(handle->worker->resultValues.size(), resultCapacity));
		result->resultValues = results;
		for (std::uint32_t i = 0; i < result->resultCount; ++i)
			results[i] = handle->worker->resultValues[i];
		handle->worker->resultValues.clear();
		result->value = result->resultCount == 0 ? 0
				: static_cast<std::int32_t>(results[0].words[0]);
		break;
	case ContinuationStatus::Trapped:
		reportTrap(handle->worker->trapKind, handle->worker->trapMessage.c_str());
		break;
	case ContinuationStatus::Exception:
		wasm_exn_set_pending(handle->worker->pendingException);
		break;
	case ContinuationStatus::Invalid:
		break;
	}
}

void wasm_cont_resume_throw(void* continuation, void* exception,
		ContinuationResumeResult* result)
{
	ContinuationValue resultValue;
	ContinuationValue tagValue;
	wasm_cont_resume_throw_ex(continuation, exception, &resultValue, 1,
			&tagValue, 1, result);
}

void wasm_cont_resume_throw_ex(void* continuation, void* exception,
		ContinuationValue* results, std::uint32_t resultCapacity,
		ContinuationValue* tagValues, std::uint32_t tagCapacity,
		ContinuationResumeResult* result)
{
	if (result == nullptr)
		return;
	*result = ContinuationResumeResult{};
	if (continuation == nullptr) {
		reportTrap(TrapKind::NullReference, "null continuation");
		return;
	}
	Continuation* handle = findContinuation(continuation);
	if (handle == nullptr) {
		reportTrap(TrapKind::Internal, "invalid continuation reference");
		return;
	}
	if (exception == nullptr) {
		reportTrap(TrapKind::NullReference, "null exception");
		return;
	}

	const ContinuationStatus status = handle->worker->resume(handle->generation,
			nullptr, 0, exception);
	result->status = static_cast<std::int32_t>(status);
	switch (status) {
	case ContinuationStatus::Suspended:
		result->tagId = handle->worker->suspendTag;
		result->tagValueCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(handle->worker->tagValues.size(), tagCapacity));
		result->tagValues = tagValues;
		result->tagKinds = handle->worker->tagKinds.empty()
				? nullptr : handle->worker->tagKinds.data();
		for (std::uint32_t i = 0; i < result->tagValueCount; ++i)
			tagValues[i] = handle->worker->tagValues[i];
		handle->worker->tagValues.clear();
		result->value = result->tagValueCount == 0 ? 0
				: static_cast<std::int32_t>(tagValues[0].words[0]);
		try {
			result->continuation = new Continuation(handle->worker,
					handle->worker->generation.load(std::memory_order_relaxed));
		} catch (...) {
			reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		}
		break;
	case ContinuationStatus::Completed:
		result->resultCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(handle->worker->resultValues.size(), resultCapacity));
		result->resultValues = results;
		for (std::uint32_t i = 0; i < result->resultCount; ++i)
			results[i] = handle->worker->resultValues[i];
		handle->worker->resultValues.clear();
		result->value = result->resultCount == 0 ? 0
				: static_cast<std::int32_t>(results[0].words[0]);
		break;
	case ContinuationStatus::Trapped:
		reportTrap(handle->worker->trapKind, handle->worker->trapMessage.c_str());
		break;
	case ContinuationStatus::Exception:
		wasm_exn_set_pending(handle->worker->pendingException);
		break;
	case ContinuationStatus::Invalid:
		break;
	}
}

void wasm_cont_switch(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationResumeResult* result)
{
	if (result == nullptr)
		return;
	*result = ContinuationResumeResult{};
	if (continuation == nullptr) {
		reportTrap(TrapKind::NullReference, "null continuation");
		return;
	}
	Continuation* handle = findContinuation(continuation);
	if (handle == nullptr) {
		reportTrap(TrapKind::Internal, "invalid continuation reference");
		return;
	}
	Worker* current = t_currentWorker;
	if (current == nullptr) {
		reportTrap(TrapKind::Internal, "switch outside a continuation");
		return;
	}
	if (current->provider != handle->worker->provider) {
		reportTrap(TrapKind::Internal, "cross-provider switch is not supported");
		return;
	}

	const std::uint64_t generation = current->beginSwitch();
	if (generation == 0)
		return;
	Continuation* captured = nullptr;
	try {
		captured = new Continuation(current->shared_from_this(), generation);
	} catch (...) {
		reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		return;
	}
	std::vector<ContinuationValue> callArgs;
	if (argCount != 0 && args != nullptr)
		callArgs.assign(args, args + argCount);
	ContinuationValue peer;
	peer.words[0] = reinterpret_cast<std::uint64_t>(captured);
	peer.words[1] = 0;
	callArgs.push_back(peer);

	const ContinuationStatus status = current->switchTo(handle->worker.get(),
			handle->generation, callArgs.data(),
			static_cast<std::uint32_t>(callArgs.size()));
	result->status = static_cast<std::int32_t>(status);
	if (status == ContinuationStatus::Completed) {
		result->resultCount = static_cast<std::uint32_t>(
				std::min<std::size_t>(current->resumeValues.size(), resultCapacity));
		result->resultValues = results;
		for (std::uint32_t i = 0; i < result->resultCount; ++i)
			results[i] = current->resumeValues[i];
		result->value = result->resultCount == 0 ? 0
				: static_cast<std::int32_t>(results[0].words[0]);
		current->resumeValues.clear();
	} else if (status == ContinuationStatus::Suspended) {
		// The target completed without handing control back. The current stack
		// is still suspended, so there is no value to return to the caller.
		reportTrap(TrapKind::Internal,
				"switch target completed without switching back");
		result->status = static_cast<std::int32_t>(ContinuationStatus::Invalid);
	} else if (status == ContinuationStatus::Trapped) {
		reportTrap(handle->worker->trapKind, handle->worker->trapMessage.c_str());
	} else if (status == ContinuationStatus::Exception) {
		wasm_exn_set_pending(handle->worker->pendingException);
	}
}

std::int32_t wasm_cont_suspend(std::int32_t tagId, std::int32_t value,
		std::int32_t* result)
{
	ContinuationValue arg;
	arg.words[0] = static_cast<std::uint64_t>(value);
	ContinuationValue out;
	const std::uint8_t i32Kind = static_cast<std::uint8_t>(ContinuationValueKind::I32);
	std::uint32_t resultCount = 0;
	const std::int32_t resumed = wasm_cont_suspend_ex(tagId, &arg, 1, &i32Kind,
			&out, result != nullptr ? 1u : 0u, &resultCount, &i32Kind);
	if (resumed != 0 && result != nullptr && resultCount != 0)
		*result = static_cast<std::int32_t>(out.words[0]);
	return resumed;
}

std::int32_t wasm_cont_suspend_ex(std::int32_t tagId,
		const ContinuationValue* args, std::uint32_t argCount,
		const std::uint8_t* argKinds, ContinuationValue* results,
		std::uint32_t resultCapacity, std::uint32_t* resultCount,
		const std::uint8_t* resultKinds)
{
	Worker* worker = t_currentWorker;
	if (worker == nullptr) {
		reportTrap(TrapKind::Internal, "suspend outside a continuation");
		return 0;
	}
	return worker->suspend(tagId, args, argCount, argKinds, results,
			resultCapacity, resultCount, resultKinds);
}

void wasm_cont_add_root(void* ref)
{
	if (t_currentWorker != nullptr)
		t_currentWorker->addRoot(ref);
}

void wasm_cont_drop_values(void* context, const std::uint8_t* kinds,
		std::uint32_t count, void* values)
{
	dropContinuationValues(kinds, count, values,
			static_cast<VMContext*>(context));
}

} // extern "C"

} // namespace WASM
