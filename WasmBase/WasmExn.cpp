#include "WasmExn.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"

#include <cstddef>

namespace {

// The payload starts after this header and is therefore 16-byte aligned (the
// allocation itself is at least `alignof(std::max_align_t)`-aligned). The
// payload's field offsets come from `translateStruct(tag params)`, whose
// alignment never exceeds 16 (v128).
struct alignas(16) ExnHeader {
	int32_t  tagId;
	uint32_t payloadBytes;
	uint64_t reserved;
};
static_assert(sizeof(ExnHeader) == 16, "an exnref payload must stay 16-byte aligned");

constexpr std::size_t kHeaderSize = sizeof(ExnHeader);

} // namespace

namespace {

// The calling thread's pending exception. Thread-local for the same reason the
// trap slot is: one call chain per host thread, and no thread may observe
// another's uncaught exception.
thread_local void* g_pendingException = nullptr;

} // namespace

extern "C" void** wasm_exn_slot() noexcept
{
return &g_pendingException;
}

extern "C" {

void* wasm_exn_create(int32_t tagId, uint32_t payloadBytes)
{
	const std::size_t total = kHeaderSize + static_cast<std::size_t>(payloadBytes);
	WASM::Store& store = WASM::Store::global();
	if (!store.reserveBytes(total)) {
		WASM::reportTrap(WASM::TrapKind::OutOfMemory);
		return nullptr;
	}

	std::byte* storage = nullptr;
	try {
		storage = new std::byte[total];
	} catch (...) {
		// A failed allocation is a host limitation, reported as a recoverable
		// trap. Reporting returns; the generated caller checks the pending slot
		// before it uses the (null) exception pointer.
		store.releaseBytes(total);
		WASM::reportTrap(WASM::TrapKind::OutOfMemory);
		return nullptr;
	}

	if (!store.ownException(storage, total)) {
		delete[] storage;
		store.releaseBytes(total);
		WASM::reportTrap(WASM::TrapKind::OutOfMemory);
		return nullptr;
	}

	auto* header = reinterpret_cast<ExnHeader*>(storage);
	header->tagId = tagId;
	header->payloadBytes = payloadBytes;
	header->reserved = 0;
	return storage + kHeaderSize;
}

int32_t wasm_exn_tag_id(const void* payload)
{
	if (payload == nullptr)
		return -1;
	const auto* base = static_cast<const std::byte*>(payload) - kHeaderSize;
	return reinterpret_cast<const ExnHeader*>(base)->tagId;
}

void wasm_exn_set_pending(void* payload) { g_pendingException = payload; }
void* wasm_exn_pending() { return g_pendingException; }
void* wasm_exn_take_pending()
{
	void* pending = g_pendingException;
	g_pendingException = nullptr;
	return pending;
}
void wasm_exn_clear_pending() { g_pendingException = nullptr; }

} // extern "C"
