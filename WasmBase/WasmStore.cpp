#include "WasmStore.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmDiagnostics.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace WASM {

namespace {

// The engine default when neither the environment nor an embedder says
// otherwise (docs/proposals/CONFIGURATION.md §9.5).
constexpr uint64_t kDefaultBudgetBytes = uint64_t{4} << 30; // 4 GiB

// Reads WASMJIT_MAX_TOTAL_BYTES once. Bytes; "0"/"unlimited" means no cap.
uint64_t parseAllocationBudget()
{
	const char* raw = std::getenv("WASMJIT_MAX_TOTAL_BYTES");
	if (raw == nullptr || *raw == '\0')
		return kDefaultBudgetBytes;
	if (std::strcmp(raw, "unlimited") == 0 || std::strcmp(raw, "0") == 0)
		return UINT64_MAX;
	char* end = nullptr;
	const unsigned long long parsed = std::strtoull(raw, &end, 0);
	if (end == raw || parsed == 0)
		return kDefaultBudgetBytes;
	return static_cast<uint64_t>(parsed);
}

// True when the environment (not the built-in default) supplied the total.
bool environmentSuppliesBudget()
{
	const char* raw = std::getenv("WASMJIT_MAX_TOTAL_BYTES");
	return raw != nullptr && *raw != '\0';
}

// `value == 0` means "the fallback", and the result is clamped to [low, high].
uint64_t clampLimit(uint64_t value, uint64_t fallback, uint64_t low, uint64_t high)
{
	if (value == 0)
		value = fallback;
	return std::min(std::max(value, low), high);
}

// Resolves a `StoreLimits`: every `0` takes the built-in default and every
// value is clamped to what the implementation can honour.
StoreLimits resolveLimits(const StoreLimits& requested)
{
	StoreLimits resolved;
	resolved.maxStringBytes =
			clampLimit(requested.maxStringBytes, kMaxStringBytes, 1024, uint64_t{1} << 30);
	// The inline-class threshold is rounded *down* to the 16-byte granularity,
	// which is an ABI invariant (i31 tagging and `v128` fields), and clamped to
	// the segregated-class table's fixed size.
	constexpr uint64_t granularity = 16;
	uint64_t inlineClass = clampLimit(requested.gcInlineClassBytes,
			GcHeap::kMaxInlineClass, granularity, GcHeap::kMaxInlineClass);
	inlineClass -= inlineClass % granularity;
	resolved.gcInlineClassBytes = static_cast<std::uint32_t>(inlineClass);
	resolved.gcMinRegionBytes = clampLimit(requested.gcMinRegionBytes,
			GcHeap::kMinRegionBytes, 4096, uint64_t{1} << 30);
	resolved.gcBlocksPerRegion = static_cast<std::uint32_t>(
			clampLimit(requested.gcBlocksPerRegion, GcHeap::kBlocksPerRegion, 1, 4096));
	resolved.gcCycleCandidates = static_cast<std::uint32_t>(
			clampLimit(requested.gcCycleCandidates, 4096, 16, 1u << 20));
	return resolved;
}

// A resolved budget: every `0` inherited and every category validated.
MemoryBudget resolveBudget(const MemoryBudget& requested)
{
	MemoryBudget budget = requested;
	if (requested.totalBytes == 0)
		budget.totalBytes = Store::allocationBudgetBytes();

	// A zero category means "no separate cap", i.e. the resolved total.
	if (budget.allLinearMemoriesBytes == 0)
		budget.allLinearMemoriesBytes = budget.totalBytes;
	if (budget.singleLinearMemoryBytes == 0)
		budget.singleLinearMemoryBytes = budget.totalBytes;
	if (budget.gcHeapBytes == 0)
		budget.gcHeapBytes = budget.totalBytes;
	if (budget.tableBytes == 0)
		budget.tableBytes = budget.totalBytes;
	if (budget.stringBytes == 0)
		budget.stringBytes = budget.totalBytes;
	if (budget.exceptionBytes == 0)
		budget.exceptionBytes = budget.totalBytes;
	if (budget.continuationStackBytes == 0)
		budget.continuationStackBytes = budget.totalBytes;

	// A category above the total could never bind; refuse it where it is set
	// (docs/proposals/CONFIGURATION.md §9.2 rule 2).
	if (budget.totalBytes != UINT64_MAX) {
		const struct {
			uint64_t value;
			const char* name;
		} categories[] = {
			{ budget.allLinearMemoriesBytes, "allLinearMemoriesBytes" },
			{ budget.singleLinearMemoryBytes, "singleLinearMemoryBytes" },
			{ budget.gcHeapBytes, "gcHeapBytes" },
			{ budget.tableBytes, "tableBytes" },
			{ budget.stringBytes, "stringBytes" },
			{ budget.exceptionBytes, "exceptionBytes" },
			{ budget.continuationStackBytes, "continuationStackBytes" },
			{ budget.reservedAddressBytes, "reservedAddressBytes" },
		};
		for (const auto& category : categories) {
			if (category.value != 0 && category.value > budget.totalBytes)
				throw std::invalid_argument(std::string("MemoryBudget::") + category.name
						+ " exceeds totalBytes: a category is a cap under the total");
		}
	}
	return budget;
}

// Credits `bytes` to a counter without ever underflowing it.
void creditBytes(std::atomic<uint64_t>& counter, uint64_t bytes) noexcept
{
	uint64_t current = counter.load(std::memory_order_relaxed);
	for (;;) {
		const uint64_t next = current > bytes ? current - bytes : 0;
		if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed))
			return;
	}
}

// Records a refused commitment as `WASM.MEMORY.BUDGET_EXCEEDED`
// (docs/proposals/CONFIGURATION.md `CFG-7`). A diagnostic failure is swallowed:
// the exception the caller is about to throw is the outcome that matters.
void reportBudgetRefusal(const char* what, MemoryCategory category, uint64_t requested) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kMemoryBudgetExceeded;
	diagnostic.severity = DiagnosticSeverity::Error;
	diagnostic.phase = DiagnosticPhase::Instantiate;
	diagnostic.message = std::string(what) + " exceeds the allocation budget ("
			+ memoryCategoryName(category) + ", " + std::to_string(requested) + " bytes requested)";
	diagnostic.remediation = "docs/proposals/CONFIGURATION.md";
	emitDiagnostic(std::move(diagnostic));
}

} // namespace

uint64_t Store::allocationBudgetBytes()
{
	static const uint64_t budget = parseAllocationBudget();
	return budget;
}

const char* memoryCategoryName(MemoryCategory category) noexcept
{
	switch (category) {
	case MemoryCategory::Other:
		return "other";
	case MemoryCategory::LinearMemory:
		return "linear-memory";
	case MemoryCategory::Table:
		return "table";
	case MemoryCategory::GcHeap:
		return "gc-heap";
	case MemoryCategory::String:
		return "string";
	case MemoryCategory::Exception:
		return "exception";
	case MemoryCategory::ContinuationStack:
		return "continuation-stack";
	}
	return "other";
}

const char* memoryBudgetSourceName(MemoryBudgetSource source) noexcept
{
	switch (source) {
	case MemoryBudgetSource::Default:
		return "default";
	case MemoryBudgetSource::Environment:
		return "environment";
	case MemoryBudgetSource::Explicit:
		return "explicit";
	}
	return "default";
}

std::atomic<uint64_t>* Store::categoryCounter(MemoryCategory category) noexcept
{
	switch (category) {
	case MemoryCategory::LinearMemory:
		return &linearBytes_;
	case MemoryCategory::Table:
		return &tableBytes_;
	case MemoryCategory::GcHeap:
		return &gcBytes_;
	case MemoryCategory::String:
		return &stringBytes_;
	case MemoryCategory::Exception:
		return &exceptionBytes_;
	case MemoryCategory::ContinuationStack:
		return &continuationBytes_;
	default:
		// `Other`: charged to the total only.
		return nullptr;
	}
}

uint64_t Store::categoryCap(MemoryCategory category) const noexcept
{
	switch (category) {
	case MemoryCategory::LinearMemory:
		return budget_.allLinearMemoriesBytes;
	case MemoryCategory::Table:
		return budget_.tableBytes;
	case MemoryCategory::GcHeap:
		return budget_.gcHeapBytes;
	case MemoryCategory::String:
		return budget_.stringBytes;
	case MemoryCategory::Exception:
		return budget_.exceptionBytes;
	case MemoryCategory::ContinuationStack:
		return budget_.continuationStackBytes;
	default:
		return budget_.totalBytes;
	}
}

bool Store::reserveTotal(uint64_t bytes) noexcept
{
	if (bytes == 0)
		return true;
	const uint64_t budget = budget_.totalBytes;
	if (budget == UINT64_MAX)
		return true;

	uint64_t current = allocatedBytes_.load(std::memory_order_relaxed);
	for (;;) {
		if (current > budget || bytes > budget - current)
			return false;
		if (allocatedBytes_.compare_exchange_weak(current, current + bytes,
												  std::memory_order_relaxed))
			return true;
	}
}

bool Store::reserveBytes(uint64_t bytes, MemoryCategory category)
{
	if (bytes == 0)
		return true;

	std::atomic<uint64_t>* counter = categoryCounter(category);
	if (counter != nullptr) {
		const uint64_t cap = categoryCap(category);
		uint64_t current = counter->load(std::memory_order_relaxed);
		for (;;) {
			if (cap != UINT64_MAX && (current > cap || bytes > cap - current))
				return false;
			if (counter->compare_exchange_weak(current, current + bytes,
											   std::memory_order_relaxed))
				break;
		}
	}

	if (!reserveTotal(bytes)) {
		// The category accepted it but the total did not: credit it back, so
		// the two counters never disagree.
		if (counter != nullptr)
			creditBytes(*counter, bytes);
		return false;
	}
	return true;
}

bool Store::reserveBytes(uint64_t bytes)
{
	return reserveTotal(bytes);
}

void Store::releaseBytes(uint64_t bytes, MemoryCategory category)
{
	if (bytes == 0)
		return;
	if (std::atomic<uint64_t>* counter = categoryCounter(category))
		creditBytes(*counter, bytes);
	creditBytes(allocatedBytes_, bytes);
}

void Store::releaseBytes(uint64_t bytes)
{
	if (bytes == 0)
		return;
	creditBytes(allocatedBytes_, bytes);
}

bool Store::reserveAddressBytes(uint64_t bytes) noexcept
{
	if (bytes == 0 || budget_.reservedAddressBytes == 0)
		return true; // reservations are not counted unless the host opted in
	const uint64_t cap = budget_.reservedAddressBytes;
	uint64_t current = reservedBytes_.load(std::memory_order_relaxed);
	for (;;) {
		if (current > cap || bytes > cap - current)
			return false;
		if (reservedBytes_.compare_exchange_weak(current, current + bytes,
												 std::memory_order_relaxed))
			return true;
	}
}

Store::Store()
	: Store(MemoryBudget{}, StoreLimits{})
{
}

Store::Store(const MemoryBudget& budget)
	: Store(budget, StoreLimits{})
{
}

Store::Store(const MemoryBudget& budget, const StoreLimits& limits)
	: budget_(resolveBudget(budget)),
	  budgetSource_(budget.totalBytes != 0 ? MemoryBudgetSource::Explicit
				: (environmentSuppliesBudget() ? MemoryBudgetSource::Environment
											   : MemoryBudgetSource::Default)),
	  limits_(resolveLimits(limits)), nextMemoryId(0), nextTableId(0), nextTagId(0)
{
}

Store::~Store()
{
	// Destroy retained instances first: their destructors call back into this
	// store (callable ranges, GC regions, owned memories and tables), so the
	// members above must still be alive.
	retainedInstances_.clear();
	for (const StoreOwnedException& exception : exceptions)
		delete[] exception.storage;
}

Store::MemoryId Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
		bool is64, uint64_t pageSize)
{
	return createLinearMemory(initialPages, maxPages, isShared, is64, pageSize,
			MemoryAllocationPolicy::Growable);
}

Store::MemoryId Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
		bool is64, uint64_t pageSize, MemoryAllocationPolicy policy)
{
	// Charge the initial commit against this store's budget (guardrail: an
	// absurd initial size must fail loudly, not allocate).
	if (pageSize == 0 || initialPages > (UINT64_MAX / pageSize))
		throw std::runtime_error("linear memory initial size is not representable");
	const uint64_t initialBytes = initialPages * pageSize;

	// The per-memory cap (`MemoryBudget::singleLinearMemoryBytes`) bounds one
	// memory below the runtime's total (docs/proposals/CONFIGURATION.md CFG-2).
	if (budget_.singleLinearMemoryBytes != UINT64_MAX
			&& initialBytes > budget_.singleLinearMemoryBytes) {
		reportBudgetRefusal("linear memory initial size", MemoryCategory::LinearMemory,
				initialBytes);
		throw std::runtime_error("linear memory exceeds the per-memory budget"
				" (MemoryBudget::singleLinearMemoryBytes)");
	}
	if (!reserveBytes(initialBytes, MemoryCategory::LinearMemory)) {
		reportBudgetRefusal("linear memory initial size", MemoryCategory::LinearMemory,
				initialBytes);
		throw std::runtime_error(
			"linear memory exceeds the allocation budget (RuntimeConfig::memory)");
	}

	// A reserving backing asks for address space up front. That reservation is
	// charged only when the host opted into `reservedAddressBytes`
	// (docs/proposals/CONFIGURATION.md CFG-3); a shared memory caps its own
	// reservation, so the count mirrors what the provider will map.
	uint64_t reservationBytes = 0;
	if ((isShared || policy == MemoryAllocationPolicy::Reserved) && maxPages != UINT64_MAX
			&& maxPages <= (UINT64_MAX / pageSize)) {
		reservationBytes = maxPages * pageSize;
		if (isShared)
			reservationBytes = std::min<uint64_t>(reservationBytes, kMaxSharedMemoryBytes);
	}
	if (!reserveAddressBytes(reservationBytes)) {
		releaseBytes(initialBytes, MemoryCategory::LinearMemory);
		reportBudgetRefusal("linear memory reservation", MemoryCategory::LinearMemory,
				reservationBytes);
		throw std::runtime_error(
			"linear memory reservation exceeds MemoryBudget::reservedAddressBytes");
	}

	const MemoryId id = nextMemoryId.fetch_add(1);

	try {
		// The host's provider is consulted first, then the built-in factory --
		// either because no provider is installed or because it declined this
		// request (docs/proposals/MEMORY_PROVIDER.md `MP-2`).
		std::unique_ptr<StoreOwnedMemory> memory;
		if (MemoryProvider* provider = memoryProvider_.load(std::memory_order_relaxed)) {
			memory = provider->createMemory(initialPages, maxPages, isShared, is64, pageSize,
					policy);
			if (memory != nullptr && memory->getMemory()->hostData != memory.get()) {
				// `Store::growMemory` recovers the owner from `hostData`; a
				// provider that rewrites it would corrupt growth, so refuse it
				// now instead of failing later (MEMORY_PROVIDER.md §4.2 item 4).
				memory.reset();
				throw std::runtime_error("the memory provider returned a backing whose"
						" LinearMemory::hostData is not the backing itself"
						" (docs/proposals/MEMORY_PROVIDER.md §4.2)");
			}
		}
		if (!memory) {
			// A shared memory reserves by construction, so the policy only chooses
			// between the two non-shared backings (MEMORY_PROVIDER.md MP-1).
			if (isShared)
				memory = std::make_unique<SharedLinearMemory>(initialPages, maxPages, is64, pageSize);
			else if (policy == MemoryAllocationPolicy::Reserved)
				memory = std::make_unique<ReservedLinearMemory>(initialPages, maxPages, is64, pageSize);
			else
				memory = std::make_unique<PrivateLinearMemory>(initialPages, maxPages, is64, pageSize);
		}

		// The unique_ptr keeps the StoreOwnedMemory at a stable address, so the
		// LinearMemory* handed out below stays valid across later insertions.
		std::scoped_lock lock(storeMutex_);
		memories.emplace(id, std::move(memory));
		if (reservationBytes != 0)
			memoryReservations_.emplace(id, reservationBytes);
	} catch (...) {
		if (reservationBytes != 0)
			creditBytes(reservedBytes_, reservationBytes);
		releaseBytes(initialBytes, MemoryCategory::LinearMemory);
		throw;
	}
	return id;
}

MemoryAllocationPolicy Store::memoryAllocationPolicy() const noexcept
{
	return memoryPolicy_.load(std::memory_order_relaxed);
}

void Store::setMemoryAllocationPolicy(MemoryAllocationPolicy policy) noexcept
{
	memoryPolicy_.store(policy, std::memory_order_relaxed);
}

void Store::setMemoryProvider(MemoryProvider* provider) noexcept
{
	memoryProvider_.store(provider, std::memory_order_relaxed);
}

MemoryProvider* Store::memoryProvider() const noexcept
{
	return memoryProvider_.load(std::memory_order_relaxed);
}

LinearMemory* Store::memory(MemoryId id)
{
	std::scoped_lock lock(storeMutex_);
	auto it = memories.find(id);
	return it == memories.end() ? nullptr : it->second->getMemory();
}

const LinearMemory* Store::memory(MemoryId id) const
{
	std::scoped_lock lock(storeMutex_);
	auto it = memories.find(id);
	return it == memories.end() ? nullptr : it->second->getMemory();
}

bool Store::growMemory(LinearMemory* memory, uint64_t deltaPages)
{
	if (memory == nullptr || memory->hostData == nullptr)
		return false;
	if (deltaPages == 0)
		return true; // conformance: a zero growth always succeeds
	const uint64_t pageSize = memory->pageSize;
	if (pageSize == 0 || deltaPages > (UINT64_MAX / pageSize))
		return false;
	const uint64_t deltaBytes = deltaPages * pageSize;

	// The per-memory cap applies to the size the memory would reach, not to the
	// delta alone (docs/proposals/CONFIGURATION.md CFG-2).
	if (budget_.singleLinearMemoryBytes != UINT64_MAX
			&& (memory->memorySize > budget_.singleLinearMemoryBytes
					|| deltaBytes > budget_.singleLinearMemoryBytes - memory->memorySize))
		return false;

	// Reserve *before* the allocation attempt, so a grow that would exceed the
	// budget fails as a failed grow (-1) rather than ballooning the host.
	if (!reserveBytes(deltaBytes, MemoryCategory::LinearMemory))
		return false;

	auto* owned = static_cast<StoreOwnedMemory*>(memory->hostData);
	if (!owned->growMemory(deltaPages)) {
		releaseBytes(deltaBytes, MemoryCategory::LinearMemory);
		return false;
	}
	// THREADS.md TH-3/T7: publish the new size and zeroed pages to other host
	// threads. `memory.size` performs the matching acquire load.
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return true;
}

Store::TableId Store::createTable(uint64_t initialSize, uint64_t maxSize)
{
	// Tables are charged at one Callable* per entry, same guardrail as memories.
	const uint64_t initialBytes = initialSize * sizeof(Callable*);
	if (initialSize != 0 && initialBytes / sizeof(Callable*) != initialSize)
		throw std::runtime_error("table initial size is not representable");
	if (!reserveBytes(initialBytes, MemoryCategory::Table)) {
		reportBudgetRefusal("table initial size", MemoryCategory::Table, initialBytes);
		throw std::runtime_error(
			"table exceeds the allocation budget (RuntimeConfig::memory)");
	}

	const TableId id = nextTableId.fetch_add(1);

	try {
		// unique_ptr keeps the StoreOwnedTable at a stable address, so the
		// TableInstance* handed out below stays valid across later insertions.
		std::scoped_lock lock(storeMutex_);
		tables.emplace(id, std::make_unique<StoreOwnedTable>(initialSize, maxSize));
	} catch (...) {
		releaseBytes(initialBytes, MemoryCategory::Table);
		throw;
	}
	return id;
}

StoreOwnedTable* Store::storeOwnedTable(TableId id)
{
	std::scoped_lock lock(storeMutex_);
	auto it = tables.find(id);
	return it == tables.end() ? nullptr : it->second.get();
}

TableInstance* Store::table(TableId id)
{
	StoreOwnedTable* owned = storeOwnedTable(id);
	return owned == nullptr ? nullptr : owned->getTable();
}

bool Store::growTable(TableInstance* table, uint64_t deltaEntries)
{
	if (table == nullptr || table->hostData == nullptr)
		return false;
	if (deltaEntries == 0)
		return true;
	if (deltaEntries > (UINT64_MAX / sizeof(Callable*)))
		return false;
	const uint64_t deltaBytes = deltaEntries * sizeof(Callable*);

	if (!reserveBytes(deltaBytes, MemoryCategory::Table))
		return false;

	auto* owned = static_cast<StoreOwnedTable*>(table->hostData);
	if (!owned->grow(deltaEntries)) {
		releaseBytes(deltaBytes, MemoryCategory::Table);
		return false;
	}
	return true;
}

bool Store::releaseMemory(LinearMemory* view)
{
	if (view == nullptr)
		return false;
	std::unique_ptr<StoreOwnedMemory> owned;
	uint64_t reservation = 0;
	{
		std::scoped_lock lock(storeMutex_);
		for (auto it = memories.begin(); it != memories.end(); ++it) {
			if (it->second->getMemory() != view)
				continue;
			owned = std::move(it->second);
			const auto reserved = memoryReservations_.find(it->first);
			if (reserved != memoryReservations_.end()) {
				reservation = reserved->second;
				memoryReservations_.erase(reserved);
			}
			memories.erase(it);
			break;
		}
	}
	if (!owned)
		return false;
	const uint64_t committed = view->memorySize;
	releaseBytes(committed, MemoryCategory::LinearMemory);
	if (reservation != 0)
		creditBytes(reservedBytes_, reservation);
	return true;
}

bool Store::releaseTable(TableInstance* view)
{
	if (view == nullptr)
		return false;
	std::unique_ptr<StoreOwnedTable> owned;
	{
		std::scoped_lock lock(storeMutex_);
		for (auto it = tables.begin(); it != tables.end(); ++it) {
			if (it->second->getTable() != view)
				continue;
			owned = std::move(it->second);
			tables.erase(it);
			break;
		}
	}
	if (!owned)
		return false;
	releaseBytes(view->size * sizeof(Callable*), MemoryCategory::Table);
	return true;
}

void Store::setContinuationSettings(const ContinuationSettings& settings) noexcept
{
	continuationSettings_ = settings;
}

ContinuationBudget Store::continuationBudget() const noexcept
{
	// The *process* default first, then this store's explicit fields, so a
	// runtime that asked for nothing keeps following a default the host may
	// still change (docs/proposals/CONFIGURATION.md `CFG-4`).
	ContinuationBudget budget = WASM::continuationBudget();
	if (continuationSettings_.maxLive != 0)
		budget.maxLive = continuationSettings_.maxLive;
	if (continuationSettings_.stackBytes != 0)
		budget.stackBytes = continuationSettings_.stackBytes;
	return budget;
}

ContinuationProvider Store::continuationProvider() const noexcept
{
	if (continuationSettings_.provider != ContinuationProvider::Inherit)
		return continuationSettings_.provider;
	return WASM::continuationProvider();
}

bool Store::reserveContinuationSlot(std::uint32_t maxLive) noexcept
{
	std::uint32_t current = liveContinuations_.load(std::memory_order_relaxed);
	for (;;) {
		if (current >= maxLive)
			return false;
		if (liveContinuations_.compare_exchange_weak(current, current + 1,
													 std::memory_order_relaxed))
			return true;
	}
}

void Store::releaseContinuationSlot() noexcept
{
	std::uint32_t current = liveContinuations_.load(std::memory_order_relaxed);
	while (current != 0) {
		if (liveContinuations_.compare_exchange_weak(current, current - 1,
													 std::memory_order_relaxed))
			return;
	}
}

// ── Tags (docs/proposals/EXCEPTIONS.md EX-2) ────────────────────────────────

TagId Store::createTag(TypeId payloadType)
{
	const TagId id = nextTagId.fetch_add(1);
	StoreOwnedTag tag;
	tag.id = id;
	tag.typeId = payloadType;
	std::scoped_lock lock(storeMutex_);
	tags.emplace(id, tag);
	return id;
}

const Store::StoreOwnedTag* Store::tag(TagId id) const
{
	std::scoped_lock lock(storeMutex_);
	auto it = tags.find(id);
	return it == tags.end() ? nullptr : &it->second;
}

bool Store::ownException(std::byte* storage, uint64_t bytes)
{
	try {
		std::scoped_lock lock(storeMutex_);
		exceptions.push_back(StoreOwnedException{storage, bytes});
		return true;
	} catch (...) {
		return false;
	}
}

// ── Call tags (docs/proposals/CALL_TAGS.md CTA-2) ──────────────────────────

Store::CallTagId Store::canonicalCallTag(TypeId signature)
{
	if (signature.isNone())
		throw std::invalid_argument("canonical call tag requires a concrete TypeId");
	std::scoped_lock lock(storeMutex_);
	const auto it = canonicalCallTags.find(signature.value);
	if (it != canonicalCallTags.end())
		return it->second;
	const CallTagId id = static_cast<CallTagId>(callTags.size());
	callTags.emplace(id, StoreOwnedCallTag{id, signature, true});
	canonicalCallTags.emplace(signature.value, id);
	return id;
}

const Store::StoreOwnedCallTag* Store::callTag(CallTagId id) const
{
	std::scoped_lock lock(storeMutex_);
	const auto it = callTags.find(id);
	return it == callTags.end() ? nullptr : &it->second;
}

// ── Function references (docs/proposals/GC.md G2 / §5.3 H3) ─────────────────

void Store::registerCallableRange(const Callable* begin, size_t count)
{
	if (begin == nullptr || count == 0)
		return;
	std::scoped_lock lock(storeMutex_);
	callableRanges.push_back(CallableRange{begin, begin + count});
}

void Store::unregisterCallableRange(const Callable* begin)
{
	if (begin == nullptr)
		return;
	std::scoped_lock lock(storeMutex_);
	for (auto it = callableRanges.begin(); it != callableRanges.end(); ++it) {
		if (it->begin == begin) {
			callableRanges.erase(it);
			return;
		}
	}
}

const Callable* Store::callableAt(const void* ref) const
{
	// Ranges are few — one per instance that has callables — so a linear scan
	// beats maintaining a hash map. Integer comparison, like `GcHeap::owns`, to
	// avoid comparing unrelated pointers.
	std::scoped_lock lock(storeMutex_);
	const uintptr_t value = reinterpret_cast<uintptr_t>(ref);
	for (const CallableRange& range : callableRanges) {
		if (value >= reinterpret_cast<uintptr_t>(range.begin) &&
			value < reinterpret_cast<uintptr_t>(range.end))
			return reinterpret_cast<const Callable*>(ref);
	}
	return nullptr;
}

void Store::retainFailedInstance(std::unique_ptr<ModuleInstance> instance,
		std::shared_ptr<ModuleInstantiator> instantiator,
		std::shared_ptr<void> keepAlive)
{
	if (!instance)
		return;
	std::scoped_lock lock(storeMutex_);
	retainedInstances_.push_back(RetainedInstance{
			std::move(keepAlive), std::move(instantiator), std::move(instance) });
}



} // namespace WASM