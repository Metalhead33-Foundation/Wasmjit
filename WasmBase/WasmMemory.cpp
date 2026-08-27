#include "WasmMemory.hpp"
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

StoreOwnedMemory::StoreOwnedMemory(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	memory.memoryMax = maxPages == UINT64_MAX ? UINT64_MAX : maxPages * 0x10000;
	memory.isShared  = isShared;
	memory.memorySize = initialPages * 0x10000;
	memory.memoryBase = nullptr;
	memory.hostData = this;
}

const LinearMemory* StoreOwnedMemory::getMemory() const
{
	return &memory;
}

PrivateLinearMemory::PrivateLinearMemory(uint64_t initialPages, uint64_t maxPages)
	: StoreOwnedMemory(initialPages, maxPages, false), backing(initialPages * 0x10000, 0)
{
	memory.memoryBase = backing.data();
}

bool PrivateLinearMemory::growMemory(uint64_t deltaPages)
{
	const size_t delta = static_cast<size_t>(deltaPages) * 0x10000;
	const size_t newSize = backing.size() + delta;
	if (memory.memoryMax != UINT64_MAX && newSize > memory.memoryMax)
		return false; // Caller should turn this into a Wasm trap
	backing.resize(newSize, 0); // Wasm requires new pages to be zero-initialized
	memory.memoryBase = backing.data();
	memory.memorySize = static_cast<uint64_t>(newSize);
	return true;
}

/*
	void*  reservedRegion = nullptr;
	size_t reservedBytes  = 0;
*/

bool SharedLinearMemory::platformReservedMemory(size_t bytes)
{
	reservedBytes = bytes;

#if defined(PLATFORM_WINDOWS)
	// Reserve virtual address space without committing physical storage/page-file space
	reservedRegion = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);

#elif defined(PLATFORM_POSIX)
	// Map address space with PROT_NONE and MAP_ANONYMOUS so no actual memory/swap is backed yet
	reservedRegion = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservedRegion == MAP_FAILED) {
		reservedRegion = nullptr;
	}
#endif
	if(reservedRegion) return true;
	else return false;
}

void SharedLinearMemory::platformDealloc()
{
	if (!reservedRegion) return;

#if defined(PLATFORM_WINDOWS)
	// Release the entire virtual address range back to the OS.
	// Note: dwSize must be 0 when using MEM_RELEASE in VirtualFree.
	VirtualFree(reservedRegion, 0, MEM_RELEASE);

#elif defined(PLATFORM_POSIX)
	// Unmap the address space back to the OS
	munmap(reservedRegion, reservedBytes);
#endif

	reservedRegion = nullptr;
	reservedBytes = 0;
}

bool SharedLinearMemory::platformCommitMemory(size_t newBytes)
{
	// Validations:
	// 1. Must have a reserved region.
	// 2. Cannot commit beyond the total reserved capacity (memoryMax).
	// 3. newBytes must be greater than current committed size (cannot shrink).
	if (!reservedRegion || newBytes > memory.memoryMax || newBytes <= memory.memorySize) {
		return false;
	}

	// Delta is the number of NEW bytes to commit, starting at the current committed offset
	size_t delta = newBytes - memory.memorySize;
	uint8_t* commitStart = static_cast<uint8_t*>(reservedRegion) + memory.memorySize;

#if defined(PLATFORM_WINDOWS)
	if (VirtualAlloc(commitStart, delta, MEM_COMMIT, PAGE_READWRITE) == NULL) {
		return false;
	}
#elif defined(PLATFORM_POSIX)
	// On POSIX, mprotect changes permissions for the newly committed page range
	if (mprotect(commitStart, delta, PROT_READ | PROT_WRITE) == -1) {
		return false;
	}
#endif

	// Update the JIT-visible committed size
	memory.memorySize = newBytes;
	return true;
}

SharedLinearMemory::SharedLinearMemory(uint64_t initialPages, uint64_t maxPages)
	: StoreOwnedMemory(initialPages, maxPages, true), reservedRegion(nullptr), reservedBytes(0)
{
	reservedBytes = maxPages * 0x10000;
	platformReservedMemory(reservedBytes); // mmap PROT_NONE / VirtualAlloc MEM_RESERVE
	memory.memoryBase = static_cast<uint8_t*>(reservedRegion); // fixed for life
	platformCommitMemory(initialPages * 0x10000); // commit initial pages
	memory.memorySize = initialPages * 0x10000;
}

SharedLinearMemory::~SharedLinearMemory()
{
	platformDealloc();
}

bool SharedLinearMemory::growMemory(uint64_t deltaPages)
{
	std::scoped_lock lock(mutex);
	// 1. Prevent integer overflow on multiplication
	if (deltaPages > (UINT64_MAX / 0x10000)) return false;

	const uint64_t delta = deltaPages * 0x10000;

	// 2. Prevent integer overflow on addition
	if (UINT64_MAX - memory.memorySize < delta) return false;

	const uint64_t newSize = memory.memorySize + delta;

	// 3. Bound check against maximum pre-reserved region
	if (newSize > reservedBytes) return false;

	// 4. Commit and handle OS failure gracefully
	if (!platformCommitMemory(newSize)) {
		return false; // OS failed to commit memory (e.g. out of memory/swap)
	}

	memory.memorySize = newSize; // Pointer remains completely stationary
	return true;
}

}