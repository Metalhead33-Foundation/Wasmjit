#include "WasmExn.hpp"
#include "WasmAgent.hpp"
#include "WasmDiagnostics.hpp"
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

// The pending exception is part of the current execution state (WasmAgent.hpp):
// a trap or an uncaught exception must be invisible to another agent sharing
// the host thread.

} // namespace

extern "C" void** wasm_exn_slot() noexcept
{
return &WASM::currentExecutionState().pendingException;
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

void wasm_exn_set_pending(void* payload) { WASM::currentExecutionState().pendingException = payload; }
void* wasm_exn_pending() { return WASM::currentExecutionState().pendingException; }
void* wasm_exn_take_pending()
{
	void* pending = WASM::currentExecutionState().pendingException;
	WASM::currentExecutionState().pendingException = nullptr;
	if (pending != nullptr) {
		// The host has just observed an uncaught exception: that is the
		// conversion boundary DG-2 asks for. The header is the Store-owned
		// `{tagId, payloadBytes, reserved}` layout above.
		const auto* base = static_cast<const std::byte*>(pending) - kHeaderSize;
		const auto* header = reinterpret_cast<const ExnHeader*>(base);
		WASM::emitExceptionDiagnostic(header->tagId, header->payloadBytes);
	}
	return pending;
}
void wasm_exn_clear_pending() { WASM::currentExecutionState().pendingException = nullptr; }

} // extern "C"
