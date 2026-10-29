#ifndef WASMMEMORY_HPP
#define WASMMEMORY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmMemory.hpp
  \brief Linear memory: the backend-visible view, growth and the shared memory ABI.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <mutex>
#include <memory>
namespace WASM {

//! Must remain standard-layout. backend accesses fields via offsetof().
//! NEVER add std::vector, virtual functions, or non-trivial members here.
struct LinearMemory {
	//! Hot fields first — memory access is the most frequent operation.
	uint8_t*  memoryBase;   //!< Base of linear memory (offset 0 for cheap addressing)
	uint64_t  memorySize;   //!< current size in bytes
	uint64_t  memoryMax;    //!< max size in bytes
	bool      isShared;     //!< true → threads-proposal shared memory (atomics-eligible, grow is synchronized)
	void*     hostData;     //!< Opaque pointer for any external user data;
	                        //!< Ownership/backing storage lives OUTSIDE this struct (see below) —
	                        //!< LinearMemory itself is just the backend-visible view.
	//! Index type of the memory64 declaration, appended after the hot fields so
	//! every existing offset (including `hostData`) stays put. Import matching
	//! compares it against the importing module's declared type (spec
	//! `match_memtype`; docs/proposals/MEMORY64.md M2).
	bool      is64;
	//! Custom page sizes (docs/proposals/CUSTOM_PAGE_SIZES.md CPS-2): the byte
	//! size of one page (1 or 65536). Appended after `is64` so every existing
	//! field offset, including `hostData`, stays put. `memorySize`/`memoryMax`
	//! remain byte counts; `memory.size`/`memory.grow` speak in pages, so the
	//! generated code and the helpers divide/multiply by this field.
	uint64_t  pageSize;
};
static_assert(offsetof(LinearMemory, memoryBase) == 0,
			  "LinearMemory::memoryBase must remain first field for backend ABI compatibility");
static_assert(std::is_standard_layout_v<LinearMemory>,
			  "LinearMemory must remain standard-layout for offsetof() access from generated code");

//! Sequentially consistent read of a memory's current size. Generated code uses
//! this for `memory.size` instead of a plain field load, so a host thread
//! growing a shared memory is observed atomically (THREADS.md TH-3/T7).
/** \\brief Reads the current memory size with sequential consistency. \\param memory Backend-visible linear-memory descriptor. \\return Current memory size in bytes. */
uint64_t wasm_memory_size(const LinearMemory* memory);

/** \\brief Store-owned backing allocation for a backend-visible LinearMemory view. */
class StoreOwnedMemory
{
protected:
	LinearMemory memory;
public:
	/** \\brief Creates backing storage with the declared limits and page size. \\param initialPages Initial page count. \\param maxPages Maximum page count. \\param isShared Whether the memory is shared. \\param is64 Whether the declaration uses memory64 indices. \\param pageSize Page size in bytes (defaults to 64 KiB). */
	explicit StoreOwnedMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
			bool is64 = false, uint64_t pageSize = 0x10000);
	virtual ~StoreOwnedMemory() = default;
	/** \\brief Returns read-only access to the backend-visible memory descriptor. \\return Pointer to the owned descriptor. */
	const LinearMemory* getMemory() const;
	/** \\brief Returns mutable access to the backend-visible memory descriptor. \\return Pointer to the owned descriptor. */
	LinearMemory* getMemory();
	//! Returns false on trap-worthy failure (exceeds max). Engine translates to a Wasm trap.
	/** \\brief Grows memory by a page count. \\param deltaPages Number of pages to add. \\return false when growth exceeds the declared maximum or cannot be committed. */
	virtual bool growMemory(uint64_t deltaPages) = 0;
	//! Optional host hint for `memory.discard`: return true when the range was
	//! decommitted and is already zero-filled; false means the caller must
	//! perform the specified zero-fill itself. The default is the portable
	//! zero-fill path (docs/proposals/MEMORY_CONTROL.md MC-3).
	/** \\brief Offers the host an opportunity to decommit a memory range. \\param offset Starting byte offset. \\param bytes Range length in bytes. \\return true if the range was decommitted and is already zero-filled; false requests explicit zero-fill. */
	virtual bool discardHint(uint64_t offset, uint64_t bytes) const
	{
		(void)offset;
		(void)bytes;
		return false;
	}
};

//! \brief Non-shared: plain growable buffer. Realloc is fine — nobody else holds memoryBase concurrently.
/** \\brief Non-shared growable memory backed by a relocatable byte vector. */
class PrivateLinearMemory final : public StoreOwnedMemory {
private:
	std::vector<uint8_t> backing;
public:
	explicit PrivateLinearMemory(uint64_t initialPages, uint64_t maxPages, bool is64 = false,
			uint64_t pageSize = 0x10000);
	/** \\brief Releases the private memory backing storage. */
	~PrivateLinearMemory() override = default;
	/** \\brief Grows the private buffer by the requested page count. \\param deltaPages Number of pages to add. \\return false if growth exceeds the declared maximum or allocation fails. */
	/** \\brief Commits additional pages without moving the reserved base pointer. \\param deltaPages Number of pages to add. \\return false if growth exceeds the maximum or commit fails. */
	bool growMemory(uint64_t deltaPages) override;
};

//! \brief Shared: reserve maxPages up front, grow = commit only. Base pointer never moves.
/** \\brief Shared memory with a reserved address range and stable base pointer. */
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
	/** \\brief Releases the reserved shared-memory address range. */
	~SharedLinearMemory() override;
	bool growMemory(uint64_t deltaPages) override;
	/** \\brief Decommits a shared-memory range when the platform supports it. \\param offset Starting byte offset. \\param bytes Range length. \\return true if decommitted and zero-filled; false if caller must zero-fill. */
	bool discardHint(uint64_t offset, uint64_t bytes) const override;
};


}

WASMJIT_VISIBLE_END

#endif // WASMMEMORY_HPP
