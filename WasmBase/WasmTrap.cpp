#include "WasmTrap.hpp"
#include "WasmValue.hpp" // g_tailCallState, currentTailCallState
#include <Elvavena/Util/ElvDeferStack.hpp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace WASM {

const char* trapKindName(TrapKind kind)
{
	switch (kind) {
	case TrapKind::Unreachable:                return "unreachable";
	case TrapKind::NullReference:              return "null reference";
	case TrapKind::NullFunctionReference:      return "null function reference";
	case TrapKind::NullStructureReference:     return "null structure reference";
	case TrapKind::NullArrayReference:         return "null array reference";
	case TrapKind::NullI31Reference:           return "null i31 reference";
	case TrapKind::IntegerDivideByZero:        return "integer divide by zero";
	case TrapKind::IntegerOverflow:            return "integer overflow";
	case TrapKind::InvalidConversionToInteger: return "invalid conversion to integer";
	case TrapKind::InvalidString:              return "invalid string";
	case TrapKind::OutOfBoundsMemoryAccess:    return "out of bounds memory access";
	case TrapKind::OutOfBoundsTableAccess:     return "out of bounds table access";
	case TrapKind::OutOfBoundsArrayAccess:     return "out of bounds array access";
	case TrapKind::UnalignedAtomicAccess:      return "unaligned atomic";
	case TrapKind::UninitializedElement:       return "uninitialized element";
	case TrapKind::UndefinedElement:           return "undefined element";
	case TrapKind::IndirectCallTypeMismatch:   return "indirect call type mismatch";
	case TrapKind::CastFailure:                return "cast failure";
	case TrapKind::OutOfMemory:                return "out of memory";
	case TrapKind::CallStackExhausted:         return "call stack exhausted";
	case TrapKind::Internal:                   return "internal error";
	}
	return "internal error";
}

// ── The pending-trap slot ───────────────────────────────────────────────────
//
// Thread-local: two host threads may execute Wasm at the same time, and a trap
// on one must not make the other's functions return zeroes. Generated code
// reaches the slot through `wasm_trap_slot()` once per function.
namespace {

thread_local TrapSlot g_trapSlot{0, static_cast<int>(TrapKind::Internal), {0}};

// Depth of host→Wasm calls on this thread. Zero means no generated frame is on
// the stack, which is the only place a `Trap` exception may be thrown.
thread_local int g_wasmCallDepth = 0;

// Depth of wrapped native-import bodies that are running. Non-zero means a C++
// catch frame stands between the import and the generated caller, so
// `WASM::trap()` may throw instead of aborting.
thread_local int g_nativeImportDepth = 0;

// The deferred-cleanup stack is fixed-capacity and thread-local: it is part of
// the trap state, not of a frame, and registering a record must not allocate.
constexpr std::uint32_t kDeferCapacity = 32;
thread_local Elv::Util::DeferStack::Record g_deferRecords[kDeferCapacity];
thread_local Elv::Util::DeferStack g_deferStack(g_deferRecords, kDeferCapacity);

} // namespace

extern "C" TrapSlot* wasm_trap_slot() noexcept
{
	return &g_trapSlot;
}

void reportTrap(TrapKind kind, const char* detail) noexcept
{
	// The first trap wins. A second report can only come from code that is
	// already leaving (a cleanup thunk, a destructor, a helper that ignored the
	// pending slot); overwriting the original reason would lose the diagnosis.
	if (g_trapSlot.pending != 0)
		return;

	const char* text = (detail != nullptr) ? detail : trapKindName(kind);
	// Copy: an import may report `std::exception::what()`, which dies with the
	// exception object long before the boundary reads the trap.
	std::size_t i = 0;
	for (; text[i] != '\0' && i + 1 < kTrapMessageCapacity; ++i)
		g_trapSlot.message[i] = text[i];
	g_trapSlot.message[i] = '\0';
	g_trapSlot.kind = static_cast<int>(kind);
	if (std::getenv("WASMJIT_TRAP_TRACE") != nullptr) {
		std::fprintf(stderr, "[trap] %s: %s\n", trapKindName(kind), g_trapSlot.message);
		std::fflush(stderr);
	}
	// Publish the reason before the flag: a reader that sees the flag must see a
	// valid kind/message. Only this thread reads it.
	g_trapSlot.pending = 1;

	// The tail-call trampoline (docs/proposals/TAILCALLS.md TC-2) keeps its
	// pending target in host-thread-local state. The chain is about to unwind by
	// returning, so the driver must not re-dispatch after a trap.
	TailCallState* tail = currentTailCallState();
	tail->pending = 0;
	tail->target = nullptr;

	// Explicit cleanup for state that no object owns. RAII already covers
	// everything else, and every C++ frame is still alive here, so a thunk may
	// use pointers into the frames that registered it.
	runDeferredCleanup();
}

extern "C" void wasm_trap_report(int kind, const char* message)
{
	reportTrap(static_cast<TrapKind>(kind), message);
}

bool trapPending() noexcept
{
	return g_trapSlot.pending != 0;
}

TrapKind pendingTrapKind() noexcept
{
	return static_cast<TrapKind>(g_trapSlot.kind);
}

const char* pendingTrapMessage() noexcept
{
	return g_trapSlot.message[0] != '\0' ? g_trapSlot.message : nullptr;
}

void clearPendingTrap() noexcept
{
	// The text stays in the slot: a caller that has just taken the trap may
	// still be reading it (the test harness does). It is overwritten by the next
	// report, and `pending` says whether it is current.
	g_trapSlot.pending = 0;
	g_trapSlot.kind = static_cast<int>(TrapKind::Internal);
}

TrapInfo takePendingTrap() noexcept
{
	TrapInfo info;
	if (g_trapSlot.pending == 0)
		return info;
	info.trapped = true;
	info.kind = static_cast<TrapKind>(g_trapSlot.kind);
	info.message = g_trapSlot.message[0] != '\0' ? g_trapSlot.message : nullptr;
	clearPendingTrap();
	return info;
}

// ── The host-boundary exception ────────────────────────────────────────────

Trap::Trap(TrapKind kind, const char* message) noexcept
	: kind_(kind), message_{}
{
	if (message == nullptr)
		message = trapKindName(kind);
	std::size_t i = 0;
	for (; message[i] != '\0' && i + 1 < kMessageCapacity; ++i)
		message_[i] = message[i];
	message_[i] = '\0';
}

// POD-only by design: nothing between the last generated frame and the throw
// needs a destructor, and the message is copied into the exception object.
[[gnu::noinline]] void checkTrap()
{
	if (g_trapSlot.pending == 0)
		return;
	// A nested call (host import → Wasm, or the component runtime calling a
	// core function) must not throw into a generated frame: leave the trap
	// pending and let the C++ caller return a benign value.
	if (g_wasmCallDepth != 0)
		return;
	const TrapInfo info = takePendingTrap();
	throw Trap(info.kind, info.message);
}

int wasmCallDepth() noexcept
{
	return g_wasmCallDepth;
}

CallDepthScope::CallDepthScope() noexcept
{
	++g_wasmCallDepth;
}

CallDepthScope::~CallDepthScope()
{
	--g_wasmCallDepth;
}

int nativeImportDepth() noexcept
{
	return g_nativeImportDepth;
}

NativeImportScope::NativeImportScope() noexcept
{
	++g_nativeImportDepth;
}

NativeImportScope::~NativeImportScope()
{
	--g_nativeImportDepth;
}

[[noreturn]] void trap(TrapKind kind, const char* detail)
{
	reportTrap(kind, detail);
	// A throw from here would have to unwind a generated frame, which has no
	// unwind table: the platform unwinder would terminate the process anyway.
	// Turn that into a deterministic, explained failure instead of undefined
	// behaviour. Helpers use `reportTrap()` + `return`; a native import wrapped
	// by `WasmNativeImport.hpp` has a catch frame (`nativeImportDepth() != 0`),
	// so there the throw is exactly what should happen.
	if (g_wasmCallDepth != 0 && g_nativeImportDepth == 0) {
		std::fprintf(stderr,
			"WasmJit: WASM::trap() called below generated code (%s); "
			"report the trap and return, or wrap the import with "
			"WASM::nativeImport()\n",
			g_trapSlot.message[0] != '\0' ? g_trapSlot.message : trapKindName(kind));
		std::fflush(stderr);
		std::abort();
	}
	const TrapInfo info = takePendingTrap();
	throw Trap(info.kind, info.message);
}

// ── Deferred cleanup ───────────────────────────────────────────────────────

TrapDeferToken defer(void (*fn)(void*) noexcept, void* arg) noexcept
{
	const Elv::Util::DeferStack::Token token = g_deferStack.defer(fn, arg);
	if (!token.valid())
		return TrapDeferToken{};
	return TrapDeferToken{token.index};
}

bool dismissDeferred(TrapDeferToken token) noexcept
{
	if (!token.valid())
		return false;
	return g_deferStack.dismiss(Elv::Util::DeferStack::Token{token.index});
}

std::uint32_t deferredCleanupCount() noexcept
{
	return g_deferStack.size();
}

void runDeferredCleanup() noexcept
{
	g_deferStack.runAll();
}

} // namespace WASM
