#ifndef WASMMEMORY_HPP
#define WASMMEMORY_HPP
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <mutex>
#include <memory>
namespace WASM {

// Must remain standard-layout. JIT accesses fields via offsetof().
// NEVER add std::vector, virtual functions, or non-trivial members here.
struct LinearMemory {
	// Hot fields first — memory access is the most frequent operation.
	uint8_t*  memoryBase;   // Base of linear memory (offset 0 for cheap addressing)
	uint64_t  memorySize;   // current size in bytes
	uint64_t  memoryMax;    // max size in bytes
	bool      isShared;     // true → threads-proposal shared memory (atomics-eligible, grow is synchronized)
	void*     hostData;     // Opaque pointer for any external user data;
	// Ownership/backing storage lives OUTSIDE this struct (see below) —
	// LinearMemory itself is just the JIT-visible view.
};
static_assert(offsetof(LinearMemory, memoryBase) == 0,
			  "LinearMemory::memoryBase must remain first field for JIT ABI compatibility");
static_assert(std::is_standard_layout_v<LinearMemory>,
			  "LinearMemory must remain standard-layout for offsetof() access from the JIT");

class StoreOwnedMemory
{
protected:
	LinearMemory memory;
public:
	explicit StoreOwnedMemory(uint64_t initialPages, uint64_t maxPages, bool isShared);
	virtual ~StoreOwnedMemory() = default;
	const LinearMemory* getMemory() const;
	// Returns false on trap-worthy failure (exceeds max). Engine translates to a Wasm trap.
	virtual bool growMemory(uint64_t deltaPages) = 0;
};

// ── Non-shared: plain growable buffer. Realloc is fine — nobody else holds memoryBase concurrently. ──
class PrivateLinearMemory final : public StoreOwnedMemory {
private:
	std::vector<uint8_t> backing;
public:
	explicit PrivateLinearMemory(uint64_t initialPages, uint64_t maxPages);
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
	explicit SharedLinearMemory(uint64_t initialPages, uint64_t maxPages);
	~SharedLinearMemory() override;
	bool growMemory(uint64_t deltaPages) override;
};


}

#endif // WASMMEMORY_HPP
