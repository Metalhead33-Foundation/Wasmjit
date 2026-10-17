#include "WasmTrap.hpp"
#include "WasmValue.hpp" // g_tailCallState
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
	case TrapKind::OutOfBoundsMemoryAccess:    return "out of bounds memory access";
	case TrapKind::OutOfBoundsTableAccess:     return "out of bounds table access";
	case TrapKind::OutOfBoundsArrayAccess:     return "out of bounds array access";
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

TrapState& trapState()
{
	static thread_local TrapState state{nullptr, nullptr, TrapKind::Internal};
	return state;
}

TrapScope::TrapScope() : buffer{}, previous(nullptr)
{
	TrapState& state = trapState();
	previous = state.active;
	state.active = this;
}

TrapScope::~TrapScope()
{
	TrapState& state = trapState();
	state.active = previous;
}

const char* TrapScope::message() const
{
	return trapState().message;
}

TrapKind TrapScope::kind() const
{
	return trapState().kind;
}

void trap(TrapKind kind)
{
	trap(kind, trapKindName(kind));
}

void trap(TrapKind kind, const char* detail)
{
	TrapState& state = trapState();
	state.kind = kind;
	state.message = (detail != nullptr) ? detail : trapKindName(kind);

	// The tail-call trampoline (docs/TAILCALLS.md TC-2) keeps its pending flag
	// in process-global state. Longjmping past the driver would otherwise leave
	// the next invocation re-dispatching to a dead target.
	g_tailCallState.pending = 0;
	g_tailCallState.target = nullptr;

	if (state.active != nullptr)
		std::longjmp(state.active->buffer, 1);

	std::fprintf(stderr, "WasmJit: unrecovered trap: %s\n", state.message);
	std::fflush(stderr);
	std::abort();
}

extern "C" void wasm_trap_raise(int kind, const char* message)
{
	trap(static_cast<TrapKind>(kind), message);
}

} // namespace WASM
