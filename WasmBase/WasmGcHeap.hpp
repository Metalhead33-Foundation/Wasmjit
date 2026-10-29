#ifndef WASMGCHEAP_HPP
#define WASMGCHEAP_HPP

#include "WasmApi.hpp"

/*!
  \file WasmGcHeap.hpp
  \brief The mmap-backed GC heap: regions, allocation and reclamation.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace WASM {

class Store;

//! \brief The GC heap: a Store-owned hunk/region allocator
//!
//! Every GC heap object (a `struct` or an `array`) is carved out of
//! mmap-backed regions, so that executing compiled code never reaches the
//! libc allocator (docs/proposals/GC.md §6.1/GC-2, docs/SAFETY.md §2.3). The
//! memory is anonymous-mapped and therefore zero-initialised for free, which is
//! the `calloc` semantics the old allocator relied on.
//!
//! Regions are *segregated by size class*: a small region serves exactly one
//! 16-byte-quantised block size and recycles through a per-region free list, so
//! reference counting can return a single object without `free()` (RC-1b). An
//! allocation above kMaxInlineClass gets a dedicated one-object region that is
//! released whole.
//!
//! Regions belong to a RegionSet, one per ModuleInstance: instance teardown
//! releases every region the instance ever used, in bulk, whether or not the
//! reference counts balanced (the RC leak backstop).
//!
//! `owns()` answers "is this pointer a GC heap block?" from the region ranges
//! alone, with no side table — that is what lets the runtime drop the old
//! per-object `gcObjectTypes` map and stay allocation-free per object.
class WASMJIT_API GcHeap
{
	Store& owner_; //!< the Store that owns this heap and its allocation budget
public:
	//! Keep 16 bytes: i31 tagging needs >= 8 (D4) and v128 fields need 16.
	static constexpr size_t kGranularity     = 16;
	//! Blocks at or below this go to a segregated class region; larger ones get a
	//! dedicated region. 16..1024 => 64 classes.
	static constexpr size_t kMaxInlineClass  = 1024;
	static constexpr size_t kNumClasses      = kMaxInlineClass / kGranularity; //!< segregated size classes
	static constexpr size_t kMinRegionBytes  = 64 * 1024; //!< lower bound on a new region's mapping
	static constexpr size_t kBlocksPerRegion = 64; //!< target live blocks per segregated region

	struct RegionSet;

	//! One mmap-backed slab: either a segregated size class or a dedicated object.
	struct Region {
		uint8_t*   base      = nullptr; //!< mapped base address
		size_t     capacity  = 0; //!< usable bytes in the mapping
		size_t     used      = 0; //!< bump-pointer high-water mark
		size_t     blockSize = 0;   //!< 0 => dedicated single-object region
		void*      freeList  = nullptr; //!< recycled blocks of `blockSize`
		uint32_t   live      = 0; //!< allocations not yet recycled
		uint32_t   classIndex = 0; //!< size-class index, or unused for dedicated regions
		RegionSet* owner     = nullptr; //!< owning instance set
		Region*    prevInSet = nullptr; //!< doubly-linked list within the set
		Region*    nextInSet = nullptr; //!< doubly-linked list within the set
	};

	//! One per ModuleInstance; owns the instance's regions for bulk teardown.
	struct RegionSet {
		GcHeap*  heap = nullptr; //!< back-pointer to the owning heap
		Region*  list = nullptr; //!< all regions belonging to this set
		Region*  current[kNumClasses] = {}; //!< open region per size class
		size_t   reservedBytes = 0; //!< bytes charged against the Store budget
	};

	//! Binds the heap to its owning Store (budget and lifetime).
	explicit GcHeap(Store& owner) : owner_(owner) {}
	Store& owner() const { return owner_; } //!< the Store that owns this heap
	~GcHeap();
	GcHeap(const GcHeap&) = delete;
	GcHeap& operator=(const GcHeap&) = delete;

	//! Allocates an empty RegionSet for one ModuleInstance.
	RegionSet* createSet();
	//! Releases every region in `set` and frees the set itself.
	void releaseSet(RegionSet* set);

	//! Allocate one object of `size` bytes at kGranularity alignment, or nullptr
	//! when the Store budget refuses the backing (the caller traps). Zeroed.
	void* allocate(RegionSet& set, size_t size);

	//! Return a block obtained from allocate() to its region's free list, or
	//! release a dedicated region. `block` must be a live allocation.
	void recycle(void* block);

	//! True iff `p` lies inside one of this heap's regions (any instance). This
	//! is the GC-object test that replaces a per-object type map.
	bool owns(const void* p) const;

	//! Diagnostics (tests / leak checks).
	size_t regionCount() const { return regions.size(); }
	size_t regionBytes() const { return reservedBytes; } //!< total bytes reserved across regions
	size_t totalAllocations() const { return allocationCount; } //!< lifetime allocate() count
	size_t totalRecycles() const { return recycleCount; } //!< lifetime recycle() count
	//! Blocks allocated and not yet recycled: the leak metric. Reference counting
	//! does not reclaim a graph whose counts are wrong, and nothing else in the
	//! engine can see that — a count stuck above zero shows up here and nowhere
	//! else. RC-2/RC-3 are "done" when this returns to its pre-test value
	//! (docs/proposals/GC.md §7, rc_plan).
	size_t liveObjectCount() const { return allocationCount - recycleCount; }

	//! \par Debug: poison on free (docs/proposals/GC.md GC-2/RC-3)
	//! Overwrite a block with kPoisonByte before it goes back on the free list,
	//! so a use-after-recycle reads obvious garbage instead of whatever the dead
	//! object left behind. Off by default (it costs a memset per recycle).
	void setPoisonOnFree(bool on) { poisonOnFreeFlag = on; }
	bool poisonOnFree() const { return poisonOnFreeFlag; } //!< whether recycle() poisons blocks
	static constexpr uint8_t kPoisonByte = 0xDD; //!< fill byte used when poison-on-free is on

private:
	//! Host threads may allocate from/release different instances' RegionSets
	//! concurrently. Recursive because allocate/release use createRegion/
	//! releaseRegion internally. The lock is uncontended in the default
	//! single-agent build (SET-2).
	mutable std::recursive_mutex mutex_;
	//! Maps a new region into `set` and charges the Store budget.
	Region* createRegion(RegionSet& set, size_t blockSize, size_t capacity, uint32_t classIndex);
	//! Unmaps `r` and credits the Store budget.
	void    releaseRegion(Region* r);
	//! The region containing `p`, or nullptr when it is not a heap block.
	Region* regionFor(const void* p) const;

	//! Sorted by base; the interval search for owns()/recycle().
	std::vector<Region*> regions;
	size_t reservedBytes  = 0; //!< sum of live region capacities
	size_t allocationCount = 0; //!< see totalAllocations()
	size_t recycleCount    = 0; //!< see totalRecycles()
	bool   poisonOnFreeFlag = false; //!< see setPoisonOnFree()
};

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMGCHEAP_HPP
