#include "WasmStore.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace WASM {

namespace {

// Reads WASMJIT_MAX_TOTAL_BYTES once. Bytes; "0"/"unlimited" means no cap.
uint64_t parseAllocationBudget()
{
	const char* raw = std::getenv("WASMJIT_MAX_TOTAL_BYTES");
	if (raw == nullptr || *raw == '\0')
		return uint64_t{2} << 30; // 2 GiB
	if (std::strcmp(raw, "unlimited") == 0 || std::strcmp(raw, "0") == 0)
		return UINT64_MAX;
	char* end = nullptr;
	const unsigned long long parsed = std::strtoull(raw, &end, 0);
	if (end == raw || parsed == 0)
		return uint64_t{2} << 30;
	return static_cast<uint64_t>(parsed);
}

} // namespace

uint64_t Store::allocationBudgetBytes()
{
	static const uint64_t budget = parseAllocationBudget();
	return budget;
}

bool Store::reserveBytes(uint64_t bytes)
{
	if (bytes == 0)
		return true;

	const uint64_t budget = allocationBudgetBytes();
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

void Store::releaseBytes(uint64_t bytes)
{
	allocatedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
}

Store& Store::global()
{
	static Store instance;
	return instance;
}

Store::Store() : nextMemoryId(0), nextTableId(0), nextTagId(0)
{
}

Store::~Store()
{
	for (const StoreOwnedException& exception : exceptions)
		delete[] exception.storage;
}

Store::MemoryId Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared, bool is64)
{
	// Charge the initial commit against the process-wide budget (guardrail: an
	// absurd initial size must fail loudly, not allocate).
	if (initialPages > (UINT64_MAX / 0x10000ull))
		throw std::runtime_error("linear memory initial size is not representable");
	const uint64_t initialBytes = initialPages * 0x10000ull;
	if (!reserveBytes(initialBytes))
		throw std::runtime_error(
			"linear memory exceeds the allocation budget (see WASMJIT_MAX_TOTAL_BYTES)");

	const MemoryId id = nextMemoryId.fetch_add(1);

	try {
		std::unique_ptr<StoreOwnedMemory> memory = isShared
			? std::unique_ptr<StoreOwnedMemory>(std::make_unique<SharedLinearMemory>(initialPages, maxPages, is64))
			: std::unique_ptr<StoreOwnedMemory>(std::make_unique<PrivateLinearMemory>(initialPages, maxPages, is64));

		// The unique_ptr keeps the StoreOwnedMemory at a stable address, so the
		// LinearMemory* handed out below stays valid across later insertions.
		std::scoped_lock lock(storeMutex_);
		memories.emplace(id, std::move(memory));
	} catch (...) {
		releaseBytes(initialBytes);
		throw;
	}
	return id;
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
	if (deltaPages > (UINT64_MAX / 0x10000ull))
		return false;
	const uint64_t deltaBytes = deltaPages * 0x10000ull;

	// Reserve *before* the allocation attempt, so a grow that would exceed the
	// budget fails as a failed grow (-1) rather than ballooning the host.
	Store& store = global();
	if (!store.reserveBytes(deltaBytes))
		return false;

	auto* owned = static_cast<StoreOwnedMemory*>(memory->hostData);
	if (!owned->growMemory(deltaPages)) {
		store.releaseBytes(deltaBytes);
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
	if (!reserveBytes(initialBytes))
		throw std::runtime_error(
			"table exceeds the allocation budget (see WASMJIT_MAX_TOTAL_BYTES)");

	const TableId id = nextTableId.fetch_add(1);

	try {
		// unique_ptr keeps the StoreOwnedTable at a stable address, so the
		// TableInstance* handed out below stays valid across later insertions.
		std::scoped_lock lock(storeMutex_);
		tables.emplace(id, std::make_unique<StoreOwnedTable>(initialSize, maxSize));
	} catch (...) {
		releaseBytes(initialBytes);
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

	Store& store = global();
	if (!store.reserveBytes(deltaBytes))
		return false;

	auto* owned = static_cast<StoreOwnedTable*>(table->hostData);
	if (!owned->grow(deltaEntries)) {
		store.releaseBytes(deltaBytes);
		return false;
	}
	return true;
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



} // namespace WASM