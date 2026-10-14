#include "WasmGcHeap.hpp"
#include "WasmStore.hpp"
#include <algorithm>
#include <new>

#if defined(_WIN32) || defined(_WIN64)
#define GCHEAP_PLATFORM_WINDOWS 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define GCHEAP_PLATFORM_POSIX 1
#include <sys/mman.h>
#endif

namespace WASM {

namespace {

// Backing store for one region. Anonymous mmap is deliberately used instead of
// malloc/calloc: it is zero-initialised, page-aligned (so every 16-byte block in
// a region is 16-aligned), and keeps the libc allocator out of the wasm
// allocation path entirely (docs/SAFETY.md §2.3, docs/proposals/GC.md GC-2).
void* platformMap(size_t bytes)
{
#if defined(GCHEAP_PLATFORM_WINDOWS)
	return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
#endif
}

void platformUnmap(void* p, size_t bytes)
{
#if defined(GCHEAP_PLATFORM_WINDOWS)
	(void)bytes;
	VirtualFree(p, 0, MEM_RELEASE);
#else
	munmap(p, bytes);
#endif
}

} // namespace

GcHeap::~GcHeap()
{
	for (Region* r : regions) {
		if (r->base != nullptr)
			platformUnmap(r->base, r->capacity);
		delete r;
	}
	regions.clear();
}

GcHeap::RegionSet* GcHeap::createSet()
{
	RegionSet* set = new RegionSet();
	set->heap = this;
	return set;
}

void GcHeap::releaseSet(RegionSet* set)
{
	if (set == nullptr)
		return;
	Region* r = set->list;
	while (r != nullptr) {
		Region* next = r->nextInSet;
		releaseRegion(r);
		r = next;
	}
	delete set;
}

GcHeap::Region* GcHeap::createRegion(RegionSet& set, size_t blockSize, size_t capacity, uint32_t classIndex)
{
	// Charge the whole region before backing it, exactly like the memory/table
	// guardrail: an allocation that would exceed the budget fails without
	// touching the host (docs/SAFETY.md §2.3).
	if (!Store::global().reserveBytes(capacity))
		return nullptr;

	void* mem = platformMap(capacity);
	if (mem == nullptr) {
		Store::global().releaseBytes(capacity);
		return nullptr;
	}

	Region* r = new Region();
	r->base       = static_cast<uint8_t*>(mem);
	r->capacity   = capacity;
	r->used       = 0;
	r->blockSize  = blockSize;
	r->classIndex = classIndex;
	r->owner      = &set;
	r->prevInSet  = nullptr;
	r->nextInSet  = set.list;
	if (set.list != nullptr)
		set.list->prevInSet = r;
	set.list = r;
	set.reservedBytes += capacity;

	// Keep the global list sorted by base for owns()/recycle()'s interval search.
	auto it = std::lower_bound(regions.begin(), regions.end(), r->base,
			[](const Region* region, const uint8_t* base) { return region->base < base; });
	regions.insert(it, r);

	reservedBytes += capacity;
	return r;
}

void GcHeap::releaseRegion(Region* r)
{
	if (r == nullptr)
		return;

	if (r->owner != nullptr) {
		if (r->prevInSet != nullptr)
			r->prevInSet->nextInSet = r->nextInSet;
		else
			r->owner->list = r->nextInSet;
		if (r->nextInSet != nullptr)
			r->nextInSet->prevInSet = r->prevInSet;
		r->owner->reservedBytes -= r->capacity;
		// The class's current region may be this one.
		if (r->blockSize != 0 && r->classIndex < kNumClasses
				&& r->owner->current[r->classIndex] == r)
			r->owner->current[r->classIndex] = nullptr;
	}

	auto it = std::lower_bound(regions.begin(), regions.end(), r->base,
			[](const Region* region, const uint8_t* base) { return region->base < base; });
	if (it != regions.end() && *it == r)
		regions.erase(it);

	reservedBytes -= r->capacity;
	if (r->base != nullptr)
		platformUnmap(r->base, r->capacity);
	delete r;
}

GcHeap::Region* GcHeap::regionFor(const void* p) const
{
	if (p == nullptr || regions.empty())
		return nullptr;
	const uint8_t* bytes = static_cast<const uint8_t*>(p);
	auto it = std::upper_bound(regions.begin(), regions.end(), bytes,
			[](const uint8_t* value, const Region* region) { return value < region->base; });
	if (it == regions.begin())
		return nullptr;
	Region* r = *(it - 1);
	if (bytes >= r->base && bytes < r->base + r->capacity)
		return r;
	return nullptr;
}

bool GcHeap::owns(const void* p) const
{
	return regionFor(p) != nullptr;
}

void* GcHeap::allocate(RegionSet& set, size_t size)
{
	if (size == 0)
		size = 1;
	const size_t rounded = (size + kGranularity - 1) / kGranularity * kGranularity;

	// Large: a dedicated region holding exactly this object.
	if (rounded > kMaxInlineClass) {
		Region* r = createRegion(set, /*blockSize*/ 0, rounded, /*classIndex*/ 0);
		if (r == nullptr)
			return nullptr;
		r->live = 1;
		++allocationCount;
		return r->base;
	}

	const uint32_t cls = static_cast<uint32_t>(rounded / kGranularity - 1);
	Region* r = set.current[cls];
	if (r != nullptr) {
		if (r->freeList != nullptr) {
			void* block = r->freeList;
			r->freeList = *reinterpret_cast<void**>(block);
			++r->live;
			++allocationCount;
			return block;
		}
		if (r->used + r->blockSize <= r->capacity) {
			void* block = r->base + r->used;
			r->used += r->blockSize;
			++r->live;
			++allocationCount;
			return block;
		}
	}

	const size_t capacity = std::max(kMinRegionBytes, rounded * kBlocksPerRegion);
	r = createRegion(set, rounded, capacity, cls);
	if (r == nullptr)
		return nullptr;
	set.current[cls] = r;
	void* block = r->base;
	r->used = rounded;
	r->live = 1;
	++allocationCount;
	return block;
}

void GcHeap::recycle(void* block)
{
	Region* r = regionFor(block);
	if (r == nullptr)
		return;
	// A dedicated region *is* the object: release it whole and credit the budget.
	if (r->blockSize == 0) {
		if (r->live > 0)
			--r->live;
		releaseRegion(r);
		return;
	}
	*reinterpret_cast<void**>(block) = r->freeList;
	r->freeList = block;
	if (r->live > 0)
		--r->live;
}

} // namespace WASM
