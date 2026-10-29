#include "WasmTrap.hpp"
#include "WasmAgent.hpp"
#include "WasmDiagnostics.hpp"
#include "WasmStack.hpp"
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
// The slot lives in the current execution state (WasmAgent.hpp). Generated code
// reaches it through `wasm_trap_slot()` once per function.


extern "C" TrapSlot* wasm_trap_slot() noexcept
{
	return &currentExecutionState().trapSlot;
}

void reportTrap(TrapKind kind, const char* detail) noexcept
{
	reportTrapAt(kind, detail, kNoTrapIndex, kNoTrapIndex, nullptr);
}

void reportTrapAt(TrapKind kind, const char* detail,
		std::uint32_t functionIndex, std::uint32_t instructionOffset,
		const char* functionName) noexcept
{
	ExecutionState& state = currentExecutionState();
	TrapSlot& slot = state.trapSlot;

	// The first trap wins. A second report can only come from code that is
	// already leaving (a cleanup thunk, a destructor, a helper that ignored the
	// pending slot); overwriting the original reason would lose the diagnosis.
	if (slot.pending != 0)
		return;

	// The compile-time site belongs to the winning report.
	if (functionIndex != kNoTrapIndex || instructionOffset != kNoTrapIndex
			|| functionName != nullptr) {
		slot.functionIndex = functionIndex;
		slot.instructionOffset = instructionOffset;
		slot.functionName = functionName;
	}

	const char* text = (detail != nullptr) ? detail : trapKindName(kind);
	// Copy: an import may report `std::exception::what()`, which dies with the
	// exception object long before the boundary reads the trap.
	std::size_t i = 0;
	for (; text[i] != '\0' && i + 1 < kTrapMessageCapacity; ++i)
		slot.message[i] = text[i];
	slot.message[i] = '\0';
	slot.kind = static_cast<int>(kind);
	if (std::getenv("WASMJIT_TRAP_TRACE") != nullptr) {
		std::fprintf(stderr, "[trap] %s: %s\n", trapKindName(kind), slot.message);
		std::fflush(stderr);
	}
	// Publish the reason before the flag: a reader that sees the flag must see a
	// valid kind/message. Only code running in this execution state reads it.
	slot.pending = 1;

	// The tail-call trampoline (docs/proposals/TAILCALLS.md TC-2) keeps its
	// pending target in the execution state. The chain is about to unwind by
	// returning, so the driver must not re-dispatch after a trap.
	state.tailCallState.pending = 0;
	state.tailCallState.target = nullptr;

	// Explicit cleanup for state that no object owns. RAII already covers
	// everything else, and every C++ frame is still alive here, so a thunk may
	// use pointers into the frames that registered it.
	state.deferStack.runAll();
}

extern "C" void wasm_trap_report(int kind, const char* message)
{
	reportTrap(static_cast<TrapKind>(kind), message);
}

extern "C" void wasm_trap_report_at(int kind, const char* message,
		std::uint32_t functionIndex, std::uint32_t instructionOffset,
		const char* functionName)
{
	reportTrapAt(static_cast<TrapKind>(kind), message, functionIndex,
			instructionOffset, functionName);
}

bool trapPending() noexcept
{
	return currentExecutionState().trapSlot.pending != 0;
}

TrapKind pendingTrapKind() noexcept
{
	return static_cast<TrapKind>(currentExecutionState().trapSlot.kind);
}

const char* pendingTrapMessage() noexcept
{
	const TrapSlot& slot = currentExecutionState().trapSlot;
	return slot.message[0] != '\0' ? slot.message : nullptr;
}

void clearPendingTrap() noexcept
{
	// The text stays in the slot: a caller that has just taken the trap may
	// still be reading it (the test harness does). It is overwritten by the next
	// report, and `pending` says whether it is current.
	TrapSlot& slot = currentExecutionState().trapSlot;
	slot.pending = 0;
	slot.kind = static_cast<int>(TrapKind::Internal);
	slot.functionIndex = kNoTrapIndex;
	slot.instructionOffset = kNoTrapIndex;
	slot.functionName = nullptr;
}

TrapInfo takePendingTrap() noexcept
{
	TrapSlot& slot = currentExecutionState().trapSlot;
	TrapInfo info;
	if (slot.pending == 0)
		return info;
	info.trapped = true;
	info.kind = static_cast<TrapKind>(slot.kind);
	info.message = slot.message[0] != '\0' ? slot.message : nullptr;
	info.functionIndex = slot.functionIndex;
	info.instructionOffset = slot.instructionOffset;
	info.functionName = slot.functionName;
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
	ExecutionState& state = currentExecutionState();
	if (state.trapSlot.pending == 0)
		return;
	// A nested call (host import → Wasm, or the component runtime calling a
	// core function) must not throw into a generated frame: leave the trap
	// pending and let the C++ caller return a benign value.
	if (state.wasmCallDepth != 0)
		return;
	const TrapInfo info = takePendingTrap();
	// This is the point where a pending status becomes a host exception, so it
	// is the one place a runtime trap is recorded (DG-2). The slot was cleared
	// by `takePendingTrap()`, but `info.message` still points into its buffer.
	emitTrapDiagnostic(info.kind, info.message, info.functionIndex,
			info.instructionOffset, info.functionName);
	throw Trap(info.kind, info.message);
}

int wasmCallDepth() noexcept
{
	return currentExecutionState().wasmCallDepth;
}

CallDepthScope::CallDepthScope() noexcept
{
	ExecutionState& state = currentExecutionState();
	// Outermost entry: make sure this thread's stack guard is installed. An
	// instance may be created on one thread and called from another, and the
	// limit captured at creation describes the creating thread's stack
	// (docs/SAFETY.md §2.8, WasmStack.hpp).
	if (state.wasmCallDepth == 0)
		ensureCurrentThreadStackLimit(state);
	++state.wasmCallDepth;
}

CallDepthScope::~CallDepthScope()
{
	--currentExecutionState().wasmCallDepth;
}

int nativeImportDepth() noexcept
{
	return currentExecutionState().nativeImportDepth;
}

NativeImportScope::NativeImportScope() noexcept
{
	++currentExecutionState().nativeImportDepth;
}

NativeImportScope::~NativeImportScope()
{
	--currentExecutionState().nativeImportDepth;
}

[[noreturn]] void trap(TrapKind kind, const char* detail)
{
	reportTrap(kind, detail);
	ExecutionState& state = currentExecutionState();
	// A throw from here would have to unwind a generated frame, which has no
	// unwind table: the platform unwinder would terminate the process anyway.
	// Turn that into a deterministic, explained failure instead of undefined
	// behaviour. Helpers use `reportTrap()` + `return`; a native import wrapped
	// by `WasmNativeImport.hpp` has a catch frame (`nativeImportDepth() != 0`),
	// so there the throw is exactly what should happen.
	if (state.wasmCallDepth != 0 && state.nativeImportDepth == 0) {
		const TrapSlot& slot = state.trapSlot;
		// Recorded before the abort so a sink can still explain the failure. The
		// id follows the slot's kind, which is the first (winning) report.
		emitTrapDiagnostic(static_cast<TrapKind>(slot.kind),
				slot.message[0] != '\0' ? slot.message : trapKindName(kind),
				slot.functionIndex, slot.instructionOffset, slot.functionName);
		std::fprintf(stderr,
			"WasmJit: WASM::trap() called below generated code (%s); "
			"report the trap and return, or wrap the import with "
			"WASM::nativeImport()\n",
			slot.message[0] != '\0' ? slot.message : trapKindName(kind));
		std::fflush(stderr);
		std::abort();
	}
	const TrapInfo info = takePendingTrap();
	// Inside a wrapped native import the trampoline re-publishes this trap and
	// `checkTrap` records it at the actual host boundary; recording it here as
	// well would duplicate the record.
	if (state.wasmCallDepth == 0)
		emitTrapDiagnostic(info.kind, info.message, info.functionIndex,
				info.instructionOffset, info.functionName);
	throw Trap(info.kind, info.message);
}

// ── Deferred cleanup ───────────────────────────────────────────────────────

TrapDeferToken defer(void (*fn)(void*) noexcept, void* arg) noexcept
{
	const Elv::Util::DeferStack::Token token = currentExecutionState().deferStack.defer(fn, arg);
	if (!token.valid())
		return TrapDeferToken{};
	return TrapDeferToken{token.index};
}

bool dismissDeferred(TrapDeferToken token) noexcept
{
	if (!token.valid())
		return false;
	return currentExecutionState().deferStack.dismiss(Elv::Util::DeferStack::Token{token.index});
}

std::uint32_t deferredCleanupCount() noexcept
{
	return currentExecutionState().deferStack.size();
}

void runDeferredCleanup() noexcept
{
	currentExecutionState().deferStack.runAll();
}

} // namespace WASM
