#include "WasmMemory.hpp"
#include <stdexcept>
#include <limits>
#include <algorithm>

#if defined(_WIN32) || defined(_WIN64)
#define PLATFORM_WINDOWS 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define PLATFORM_POSIX 1
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace WASM {

constexpr uint64_t MAX_SHARED_BYTES = 4ULL * 1024 * 1024 * 1024; // 4 GiB Cap for unbounded shared memory

uint64_t wasm_memory_size(const LinearMemory* memory)
{
	if (memory == nullptr)
		return 0;
	// A shared memory's metadata can be updated by another host thread. The
	// acquire load pairs with the release fence in Store::growMemory; an
	// unshared memory is read directly (it has a single owner, THREADS.md TD9).
	if (memory->isShared)
		return __atomic_load_n(&memory->memorySize, __ATOMIC_ACQUIRE);
	return memory->memorySize;
}

StoreOwnedMemory::StoreOwnedMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
		bool is64, uint64_t pageSize)
{
	// Guard against initialPages/maxPages overflow in constructor calculations.
	// `pageSize` comes from a decoded MemoryType (1 or 65536); a zero page size
	// would make every byte calculation meaningless.
	if (pageSize == 0 || initialPages > (UINT64_MAX / pageSize)) {
		throw std::invalid_argument("initialPages exceeds addressable representation limit");
	}

	const uint64_t initialBytes = initialPages * pageSize;

	if (maxPages == UINT64_MAX) {
		memory.memoryMax = UINT64_MAX;
	} else if (maxPages > (UINT64_MAX / pageSize)) {
		// A memory64 declaration can request up to 2^64 bytes, which is not
		// representable as a byte count. Treat it as unbounded; an actual grow
		// past the address space still fails at allocation.
		memory.memoryMax = UINT64_MAX;
	} else {
		memory.memoryMax = maxPages * pageSize;
	}

	// Invariant: initial size must never exceed max size
	if (initialBytes > memory.memoryMax) {
		throw std::invalid_argument("initialPages exceeds maxPages");
	}

	memory.isShared   = isShared;
	memory.is64       = is64;
	memory.pageSize   = pageSize;
	memory.memorySize = 0; // Commits set the actual size explicitly
	memory.memoryBase = nullptr;
	memory.hostData   = this;
}

const LinearMemory* StoreOwnedMemory::getMemory() const
{
	return &memory;
}

LinearMemory* StoreOwnedMemory::getMemory()
{
	return &memory;
}

// ── Private (Unshared) Linear Memory Implementation ──

PrivateLinearMemory::PrivateLinearMemory(uint64_t initialPages, uint64_t maxPages, bool is64,
		uint64_t pageSize)
	: StoreOwnedMemory(initialPages, maxPages, false, is64, pageSize)
{
	const uint64_t initialBytes = initialPages * pageSize;

	if (initialBytes > std::numeric_limits<size_t>::max()) {
		throw std::bad_alloc();
	}

	backing.resize(static_cast<size_t>(initialBytes), 0);
	memory.memoryBase = backing.data();
	memory.memorySize = initialBytes;
}

bool PrivateLinearMemory::growMemory(uint64_t deltaPages)
{
	if (deltaPages == 0) return true; // Conformance: zero growth always succeeds

	const uint64_t pageSize = memory.pageSize;
	if (pageSize == 0 || deltaPages > (UINT64_MAX / pageSize)) return false;

	const uint64_t deltaBytes = deltaPages * pageSize;
	if (UINT64_MAX - memory.memorySize < deltaBytes) return false;

	const uint64_t newSize = memory.memorySize + deltaBytes;
	if (memory.memoryMax != UINT64_MAX && newSize > memory.memoryMax) return false;
	if (newSize > std::numeric_limits<size_t>::max()) return false;

	backing.resize(static_cast<size_t>(newSize), 0);
	memory.memoryBase = backing.data();
	memory.memorySize = newSize;
	return true;
}

// ── Shared Linear Memory Implementation ──

bool SharedLinearMemory::platformReservedMemory(size_t bytes)
{
	reservedBytes = bytes;

#if defined(PLATFORM_WINDOWS)
	reservedRegion = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(PLATFORM_POSIX)
	reservedRegion = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservedRegion == MAP_FAILED) {
		reservedRegion = nullptr;
	}
#endif
	return reservedRegion != nullptr;
}

void SharedLinearMemory::platformDealloc()
{
	if (!reservedRegion) return;

#if defined(PLATFORM_WINDOWS)
	VirtualFree(reservedRegion, 0, MEM_RELEASE);
#elif defined(PLATFORM_POSIX)
	munmap(reservedRegion, reservedBytes);
#endif

	reservedRegion = nullptr;
	reservedBytes = 0;
}

bool SharedLinearMemory::platformCommitMemory(size_t newBytes)
{
	if (!reservedRegion || newBytes > reservedBytes || newBytes <= memory.memorySize) {
		return false;
	}

	size_t delta = newBytes - static_cast<size_t>(memory.memorySize);
	uint8_t* commitStart = static_cast<uint8_t*>(reservedRegion) + memory.memorySize;

#if defined(PLATFORM_WINDOWS)
	if (VirtualAlloc(commitStart, delta, MEM_COMMIT, PAGE_READWRITE) == NULL) {
		return false;
	}
#elif defined(PLATFORM_POSIX)
	// Safely update permissions over reserved PROT_NONE region
	if (mprotect(commitStart, delta, PROT_READ | PROT_WRITE) == -1) {
		return false;
	}
#endif

	memory.memorySize = newBytes;
	return true;
}

SharedLinearMemory::SharedLinearMemory(uint64_t initialPages, uint64_t maxPages, bool is64,
		uint64_t pageSize)
	: StoreOwnedMemory(initialPages, maxPages, true, is64, pageSize), reservedRegion(nullptr), reservedBytes(0)
{
	// 1. Calculate and reconcile true maximum reserved capacity. An unbounded
	// (or unrepresentable, memory64-style) maximum is capped at MAX_SHARED_BYTES.
	uint64_t requestedMaxBytes;
	if (maxPages == UINT64_MAX || maxPages > (UINT64_MAX / pageSize)) {
		requestedMaxBytes = MAX_SHARED_BYTES;
	} else {
		requestedMaxBytes = maxPages * pageSize;
	}
	uint64_t capBytes = std::min(requestedMaxBytes, MAX_SHARED_BYTES);

	if (capBytes > std::numeric_limits<size_t>::max()) {
		throw std::bad_alloc();
	}

	// Reconcile metadata to reflect capped capacity explicitly
	memory.memoryMax = capBytes;

	// 2. Reserve Address Space
	if (!platformReservedMemory(static_cast<size_t>(capBytes))) {
		throw std::bad_alloc();
	}

	memory.memoryBase = static_cast<uint8_t*>(reservedRegion);

	// 3. Perform Initial Commit
	const uint64_t initialBytes = initialPages * pageSize;
	if (initialBytes > 0) {
		if (!platformCommitMemory(static_cast<size_t>(initialBytes))) {
			platformDealloc();
			throw std::bad_alloc();
		}
	}
}

SharedLinearMemory::~SharedLinearMemory()
{
	platformDealloc();
}

bool SharedLinearMemory::growMemory(uint64_t deltaPages)
{
	if (deltaPages == 0) return true; // WASM Spec Conformance: zero growth always succeeds

	std::scoped_lock lock(mutex);

	const uint64_t pageSize = memory.pageSize;
	if (pageSize == 0 || deltaPages > (UINT64_MAX / pageSize)) return false;

	const uint64_t deltaBytes = deltaPages * pageSize;
	if (UINT64_MAX - memory.memorySize < deltaBytes) return false;

	const uint64_t newSize = memory.memorySize + deltaBytes;
	if (newSize > reservedBytes || newSize > memory.memoryMax) return false;
	if (newSize > std::numeric_limits<size_t>::max()) return false;

	return platformCommitMemory(static_cast<size_t>(newSize));
}

bool SharedLinearMemory::discardHint(uint64_t offset, uint64_t bytes) const
{
	// The reserved-region backing can return pages to the OS while keeping the
	// address space valid: POSIX `madvise(MADV_DONTNEED)` makes subsequent reads
	// zero-filled. On other hosts the caller's memset is the specified fallback.
#if defined(PLATFORM_POSIX)
	if (!reservedRegion || bytes == 0)
		return false;
	if (offset > memory.memorySize || bytes > memory.memorySize - offset)
		return false;
	void* start = static_cast<uint8_t*>(reservedRegion) + offset;
	return ::madvise(start, static_cast<size_t>(bytes), MADV_DONTNEED) == 0;
#else
	(void)offset;
	(void)bytes;
	return false;
#endif
}

}