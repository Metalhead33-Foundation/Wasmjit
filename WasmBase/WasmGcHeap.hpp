#ifndef WASMGCHEAP_HPP
#define WASMGCHEAP_HPP
#include <cstddef>
#include <cstdint>
#include <vector>

namespace WASM {

// ── The GC heap: a Store-owned hunk/region allocator ────────────────────────
//
// Every GC heap object (a `struct` or an `array`) is carved out of
// mmap-backed regions, so that executing JIT-compiled code never reaches the
// libc allocator (docs/proposals/GC.md §6.1/GC-2, docs/SAFETY.md §2.3). The
// memory is anonymous-mapped and therefore zero-initialised for free, which is
// the `calloc` semantics the old allocator relied on.
//
// Regions are *segregated by size class*: a small region serves exactly one
// 16-byte-quantised block size and recycles through a per-region free list, so
// reference counting can return a single object without `free()` (RC-1b). An
// allocation above kMaxInlineClass gets a dedicated one-object region that is
// released whole.
//
// Regions belong to a RegionSet, one per ModuleInstance: instance teardown
// releases every region the instance ever used, in bulk, whether or not the
// reference counts balanced (the RC leak backstop).
//
// `owns()` answers "is this pointer a GC heap block?" from the region ranges
// alone, with no side table — that is what lets the runtime drop the old
// per-object `gcObjectTypes` map and stay allocation-free per object.
class GcHeap
{
public:
	// Keep 16 bytes: i31 tagging needs >= 8 (D4) and v128 fields need 16.
	static constexpr size_t kGranularity     = 16;
	// Blocks at or below this go to a segregated class region; larger ones get a
	// dedicated region. 16..1024 => 64 classes.
	static constexpr size_t kMaxInlineClass  = 1024;
	static constexpr size_t kNumClasses      = kMaxInlineClass / kGranularity;
	static constexpr size_t kMinRegionBytes  = 64 * 1024;
	static constexpr size_t kBlocksPerRegion = 64;

	struct RegionSet;

	struct Region {
		uint8_t*   base      = nullptr;
		size_t     capacity  = 0;
		size_t     used      = 0;
		size_t     blockSize = 0;   // 0 => dedicated single-object region
		void*      freeList  = nullptr;
		uint32_t   live      = 0;
		uint32_t   classIndex = 0;
		RegionSet* owner     = nullptr;
		Region*    prevInSet = nullptr;
		Region*    nextInSet = nullptr;
	};

	// One per ModuleInstance; owns the instance's regions for bulk teardown.
	struct RegionSet {
		GcHeap*  heap = nullptr;
		Region*  list = nullptr;
		Region*  current[kNumClasses] = {};
		size_t   reservedBytes = 0;
	};

	GcHeap() = default;
	~GcHeap();
	GcHeap(const GcHeap&) = delete;
	GcHeap& operator=(const GcHeap&) = delete;

	RegionSet* createSet();
	void releaseSet(RegionSet* set);

	// Allocate one object of `size` bytes at kGranularity alignment, or nullptr
	// when the Store budget refuses the backing (the caller traps). Zeroed.
	void* allocate(RegionSet& set, size_t size);

	// Return a block obtained from allocate() to its region's free list, or
	// release a dedicated region. `block` must be a live allocation.
	void recycle(void* block);

	// True iff `p` lies inside one of this heap's regions (any instance). This
	// is the GC-object test that replaces a per-object type map.
	bool owns(const void* p) const;

	// Diagnostics (tests / leak checks).
	size_t regionCount() const { return regions.size(); }
	size_t regionBytes() const { return reservedBytes; }
	size_t totalAllocations() const { return allocationCount; }

private:
	Region* createRegion(RegionSet& set, size_t blockSize, size_t capacity, uint32_t classIndex);
	void    releaseRegion(Region* r);
	Region* regionFor(const void* p) const;

	// Sorted by base; the interval search for owns()/recycle().
	std::vector<Region*> regions;
	size_t reservedBytes  = 0;
	size_t allocationCount = 0;
};

} // namespace WASM
#endif // WASMGCHEAP_HPP
