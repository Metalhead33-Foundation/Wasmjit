#include "WasmContinuation.hpp"
#include "WasmAgent.hpp"
#include "WasmExn.hpp"
#include "WasmStack.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"

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

struct Worker {
	const ContinuationProvider provider;
	Callable* target = nullptr;
	VMContext* creationContext = nullptr;
	std::uint32_t paramCount = 0;
	bool takesArg = false;
	bool returnsValue = false;
	std::vector<std::int32_t> boundArgs;

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
		: provider(provider), target(target), creationContext(creationContext),
		  paramCount(paramCount), takesArg(takesArg), returnsValue(returnsValue) {}

	virtual ~Worker()
	{
		releaseContinuationSlot();
	}

	virtual ContinuationStatus resume(std::uint64_t expectedGeneration,
			std::int32_t arg, void* injectedException) = 0;
	virtual std::int32_t suspend(std::int32_t tag, std::int32_t value,
			std::int32_t* result) = 0;
	// Bind a prefix of the remaining arguments. The provider checks that the
	// continuation is still New; a suspended bind is a later extension.
	virtual bool bindArguments(const std::int32_t* values, std::uint32_t count) = 0;

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

void Worker::executeTarget()
{
	trapped = false;
	trapMessage.clear();
	pendingException = nullptr;
	resultValue = 0;

	if (target == nullptr || target->fnPtr == nullptr) {
		trapped = true;
		trapKind = TrapKind::Internal;
		trapMessage = "continuation target has no entry point";
		std::snprintf(execution.trapSlot.message, sizeof(execution.trapSlot.message),
				"%s", trapMessage.c_str());
		execution.trapSlot.kind = static_cast<int>(trapKind);
		execution.trapSlot.pending = 1;
		return;
	}

	Callable targetCopy = *target;
	// A native import carries a null context and normally receives the caller's
	// VMContext. A continuation has no caller frame, so use the context that
	// created it. Defined Wasm functions already carry their own context.
	if (targetCopy.context == nullptr)
		targetCopy.context = creationContext;

	std::vector<std::int32_t> callArgs = boundArgs;
	if (takesArg)
		callArgs.push_back(resumeArg);
	boundArgs.clear();
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
		switch (callArgs.size()) {
		case 0:
			if (returnsValue)
				resultValue = callCallable<std::int32_t>(targetCopy);
			else
				callCallable<void>(targetCopy);
			break;
		case 1:
			if (returnsValue)
				resultValue = callCallable<std::int32_t>(targetCopy, callArgs[0]);
			else
				callCallable<void>(targetCopy, callArgs[0]);
			break;
		case 2:
			if (returnsValue)
				resultValue = callCallable<std::int32_t>(targetCopy, callArgs[0], callArgs[1]);
			else
				callCallable<void>(targetCopy, callArgs[0], callArgs[1]);
			break;
		default:
			throw std::runtime_error("continuation signatures with more than two parameters are not implemented");
		}
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

	BWorker(Callable* target, VMContext* context, std::uint32_t paramCount,
			bool takesArg, bool returnsValue)
		: Worker(ContinuationProvider::HostThreads, target, context, paramCount,
				takesArg, returnsValue) {}

	~BWorker() override;

	ContinuationStatus resume(std::uint64_t expectedGeneration,
			std::int32_t arg, void* injectedException) override;
	std::int32_t suspend(std::int32_t tag, std::int32_t value,
			std::int32_t* result) override;
	bool bindArguments(const std::int32_t* values, std::uint32_t count) override;

	void run();
	bool startThread();
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
			std::int32_t arg, void* injectedException) override;
	std::int32_t suspend(std::int32_t tag, std::int32_t value,
			std::int32_t* result) override;
	bool bindArguments(const std::int32_t* values, std::uint32_t count) override;

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

ContinuationStatus BWorker::resume(std::uint64_t expectedGeneration, std::int32_t arg,
		void* injectedException)
{
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
	resumeArg = arg;
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

std::int32_t BWorker::suspend(std::int32_t tag, std::int32_t value,
		std::int32_t* result)
{
	std::unique_lock<std::mutex> lock(mutex);
	if (cancelRequested || state == WorkerState::Cancelled) {
		reportTrap(TrapKind::Internal, "continuation dropped while suspended");
		return 0;
	}
	suspendTag = tag;
	suspendValue = value;
	generation.fetch_add(1, std::memory_order_relaxed);
	consumed.store(false, std::memory_order_relaxed);
	state = WorkerState::Suspended;
	cv.notify_all();
	cv.wait(lock, [this] { return state != WorkerState::Suspended; });
	if (cancelRequested || state == WorkerState::Cancelled) {
		reportTrap(TrapKind::Internal, "continuation dropped while suspended");
		return 0;
	}
	if (result != nullptr) {
		if (!boundArgs.empty()) {
			*result = boundArgs.front();
			boundArgs.erase(boundArgs.begin());
		} else {
			*result = resumeArg;
		}
	}
	return 1;
}

bool BWorker::bindArguments(const std::int32_t* values, std::uint32_t count)
{
	std::lock_guard<std::mutex> lock(mutex);
	if ((state != WorkerState::New && state != WorkerState::Suspended) || values == nullptr)
		return false;
	if (state == WorkerState::New && boundArgs.size() + count > paramCount)
		return false;
	boundArgs.insert(boundArgs.end(), values, values + count);
	if (state == WorkerState::New)
		takesArg = boundArgs.size() < paramCount;
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

ContinuationStatus AWorker::resume(std::uint64_t expectedGeneration, std::int32_t arg,
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

	const bool first = (state == WorkerState::New);
	resumeArg = arg;
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
	return statusForState(state);
}

std::int32_t AWorker::suspend(std::int32_t tag, std::int32_t value,
		std::int32_t* result)
{
	if (previousState == nullptr) {
		reportTrap(TrapKind::Internal, "suspend outside its continuation thread");
		return 0;
	}
	suspendTag = tag;
	suspendValue = value;
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
	if (result != nullptr) {
		if (!boundArgs.empty()) {
			*result = boundArgs.front();
			boundArgs.erase(boundArgs.begin());
		} else {
			*result = resumeArg;
		}
	}
	return 1;
}

bool AWorker::bindArguments(const std::int32_t* values, std::uint32_t count)
{
	if ((state != WorkerState::New && state != WorkerState::Suspended) || values == nullptr)
		return false;
	if (state == WorkerState::New && boundArgs.size() + count > paramCount)
		return false;
	boundArgs.insert(boundArgs.end(), values, values + count);
	if (state == WorkerState::New)
		takesArg = boundArgs.size() < paramCount;
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

extern "C" {

void* wasm_cont_new(void* callable, void* context, std::uint32_t paramCount,
		std::int32_t takesArg, std::int32_t returnsValue)
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
					static_cast<VMContext*>(context), paramCount, takesArg != 0,
					returnsValue != 0);
		} else {
			worker = std::make_shared<AWorker>(static_cast<Callable*>(callable),
					static_cast<VMContext*>(context), paramCount, takesArg != 0,
					returnsValue != 0);
		}
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

	const ContinuationStatus status = handle->worker->resume(handle->generation, arg,
			nullptr);
	result->status = static_cast<std::int32_t>(status);
	switch (status) {
	case ContinuationStatus::Suspended:
		result->tagId = handle->worker->suspendTag;
		result->value = handle->worker->suspendValue;
		try {
			result->continuation = new Continuation(handle->worker,
					handle->worker->generation.load(std::memory_order_relaxed));
		} catch (...) {
			reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		}
		break;
	case ContinuationStatus::Completed:
		result->value = handle->worker->resultValue;
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

	const ContinuationStatus status = handle->worker->resume(handle->generation, 0,
			exception);
	result->status = static_cast<std::int32_t>(status);
	switch (status) {
	case ContinuationStatus::Suspended:
		result->tagId = handle->worker->suspendTag;
		result->value = handle->worker->suspendValue;
		try {
			result->continuation = new Continuation(handle->worker,
					handle->worker->generation.load(std::memory_order_relaxed));
		} catch (...) {
			reportTrap(TrapKind::OutOfMemory, "continuation allocation failed");
		}
		break;
	case ContinuationStatus::Completed:
		result->value = handle->worker->resultValue;
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

std::int32_t wasm_cont_suspend(std::int32_t tagId, std::int32_t value,
		std::int32_t* result)
{
	Worker* worker = t_currentWorker;
	if (worker == nullptr) {
		reportTrap(TrapKind::Internal, "suspend outside a continuation");
		return 0;
	}
	return worker->suspend(tagId, value, result);
}

} // extern "C"

} // namespace WASM
