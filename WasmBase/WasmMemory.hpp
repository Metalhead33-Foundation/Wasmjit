#ifndef WASMMEMORY_HPP
#define WASMMEMORY_HPP
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <mutex>
#include <memory>
namespace WASM {

// Must remain standard-layout. backend accesses fields via offsetof().
// NEVER add std::vector, virtual functions, or non-trivial members here.
struct LinearMemory {
	// Hot fields first — memory access is the most frequent operation.
	uint8_t*  memoryBase;   // Base of linear memory (offset 0 for cheap addressing)
	uint64_t  memorySize;   // current size in bytes
	uint64_t  memoryMax;    // max size in bytes
	bool      isShared;     // true → threads-proposal shared memory (atomics-eligible, grow is synchronized)
	void*     hostData;     // Opaque pointer for any external user data;
	// Ownership/backing storage lives OUTSIDE this struct (see below) —
	// LinearMemory itself is just the backend-visible view.
	// Appended after the hot fields so every existing offset (including
	// `hostData`) stays put: the index type of a memory64 declaration. Import
	// matching compares it against the importing module's declared type
	// (spec `match_memtype`; docs/proposals/MEMORY64.md M2).
	bool      is64;
	// Custom page sizes (docs/proposals/CUSTOM_PAGE_SIZES.md CPS-2): the byte
	// size of one page (1 or 65536). Appended after `is64` so every existing
	// field offset, including `hostData`, stays put. `memorySize`/`memoryMax`
	// remain byte counts; `memory.size`/`memory.grow` speak in pages, so the
	// generated code and the helpers divide/multiply by this field.
	uint64_t  pageSize;
};
static_assert(offsetof(LinearMemory, memoryBase) == 0,
			  "LinearMemory::memoryBase must remain first field for backend ABI compatibility");
static_assert(std::is_standard_layout_v<LinearMemory>,
			  "LinearMemory must remain standard-layout for offsetof() access from generated code");

// Sequentially consistent read of a memory's current size. Generated code uses
// this for `memory.size` instead of a plain field load, so a host thread
// growing a shared memory is observed atomically (THREADS.md TH-3/T7).
uint64_t wasm_memory_size(const LinearMemory* memory);

class StoreOwnedMemory
{
protected:
	LinearMemory memory;
public:
	explicit StoreOwnedMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
			bool is64 = false, uint64_t pageSize = 0x10000);
	virtual ~StoreOwnedMemory() = default;
	const LinearMemory* getMemory() const;
	LinearMemory* getMemory();
	// Returns false on trap-worthy failure (exceeds max). Engine translates to a Wasm trap.
	virtual bool growMemory(uint64_t deltaPages) = 0;
	// Optional host hint for `memory.discard`: return true when the range was
	// decommitted and is already zero-filled; false means the caller must
	// perform the specified zero-fill itself. The default is the portable
	// zero-fill path (docs/proposals/MEMORY_CONTROL.md MC-3).
	virtual bool discardHint(uint64_t offset, uint64_t bytes) const
	{
		(void)offset;
		(void)bytes;
		return false;
	}
};

// ── Non-shared: plain growable buffer. Realloc is fine — nobody else holds memoryBase concurrently. ──
class PrivateLinearMemory final : public StoreOwnedMemory {
private:
	std::vector<uint8_t> backing;
public:
	explicit PrivateLinearMemory(uint64_t initialPages, uint64_t maxPages, bool is64 = false,
			uint64_t pageSize = 0x10000);
	~PrivateLinearMemory() override = default;
	bool growMemory(uint64_t deltaPages) override;
};

// ── Shared: reserve maxPages up front, grow = commit only. Base pointer never moves. ──
class SharedLinearMemory final : public StoreOwnedMemory {
private:
	void*  reservedRegion;
	size_t reservedBytes;
	mutable std::mutex mutex;
	bool platformReservedMemory(size_t bytes);
	bool platformCommitMemory(size_t newBytes);
	void platformDealloc();
public:
	explicit SharedLinearMemory(uint64_t initialPages, uint64_t maxPages, bool is64 = false,
			uint64_t pageSize = 0x10000);
	~SharedLinearMemory() override;
	bool growMemory(uint64_t deltaPages) override;
	bool discardHint(uint64_t offset, uint64_t bytes) const override;
};


}

#endif // WASMMEMORY_HPP
