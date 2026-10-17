#ifndef WASMTRAP_HPP
#define WASMTRAP_HPP
#include <csetjmp>
#include <cstddef>
#include <cstdint>

namespace WASM {

// ── Wasm traps ──────────────────────────────────────────────────────────────
//
// A trap is a non-local exit from the current Wasm invocation. The engine's
// own frames — generated code plus the POD native helpers it calls — carry
// no unwind tables, so a trap is a `longjmp` to a scope the embedder armed
// around the outermost call rather than a C++ exception. Jumping out of a call
// chain also strands the tail-call trampoline's process-global state, so
// trap() clears it before jumping.
//
// Usage (setjmp must be expanded in a frame that outlives the wasm call):
//
//     WASM::TrapScope scope;
//     if (setjmp(scope.buffer) == 0) {
//         result = ...call into wasm...;
//     } else {
//         std::fprintf(stderr, "trap: %s\n", scope.message());
//     }
//
// With no armed scope, trap() keeps the historical behaviour and aborts.
// See docs/proposals/BULK_MEMORY.md (BM-2) and docs/proposals/GC.md (GC-6).

class TrapScope;

// The *kind* of a trap: the spec's trap taxonomy. The kind is what an embedder or
// a test compares — "did it trap for the right reason?" — while the free-text
// message is only for the log. `trapKindName` returns the spec's wording for a
// kind, which is exactly what the spec test suite asserts on ("null reference",
// "out of bounds memory access", …), so an `assert_trap` expectation can be
// checked against the kind instead of against the log text.
//
// See docs/SAFETY.md §2.4: a trap is Wasm semantics. An engine problem (a
// compile-time limitation, a bad index that validation should have rejected) is
// *not* a trap and must not be reported as one.
enum class TrapKind : uint8_t {
	Unreachable,                 // "unreachable"
	NullReference,               // "null reference"
	NullFunctionReference,       // "null function reference"
	NullStructureReference,      // "null structure reference"
	NullArrayReference,          // "null array reference"
	NullI31Reference,            // "null i31 reference"
	IntegerDivideByZero,         // "integer divide by zero"
	IntegerOverflow,             // "integer overflow"
	InvalidConversionToInteger,  // "invalid conversion to integer"
	OutOfBoundsMemoryAccess,     // "out of bounds memory access"
	OutOfBoundsTableAccess,      // "out of bounds table access"
	OutOfBoundsArrayAccess,      // "out of bounds array access"
	UnalignedAtomicAccess,       // "unaligned atomic"
	UninitializedElement,        // "uninitialized element"
	UndefinedElement,            // "undefined element"
	IndirectCallTypeMismatch,    // "indirect call type mismatch"
	CastFailure,                 // "cast failure"
	OutOfMemory,                 // "out of memory"
	CallStackExhausted,          // "call stack exhausted"
	// Not a spec trap: a defensive check inside the engine, or a lowering that
	// cannot run. The detail text is the only description.
	Internal,
};

// The spec's text for a trap kind ("unreachable", "null reference", …). For
// TrapKind::Internal it returns "internal error"; use the message for detail.
const char* trapKindName(TrapKind kind);

struct TrapState {
	TrapScope*  active;   // innermost armed scope, or nullptr
	const char* message;  // reason for the most recent trap, or nullptr
	TrapKind    kind;     // kind of the most recent trap
};

// The thread-local trap state. One engine instance executes on one thread;
// this is thread_local so a future multi-agent build degrades safely.
TrapState& trapState();

// Raise a trap of `kind`, reporting trapKindName(kind) as the reason. Never
// returns: jumps to the innermost armed scope, or prints a diagnostic and aborts
// when no scope is armed.
[[noreturn]] void trap(TrapKind kind);

// Raise a trap of `kind` with a more precise reason than the kind's name (e.g.
// "out of bounds memory.init source" for an OutOfBoundsMemoryAccess). The kind
// is still recorded, so a comparison against trapKindName stays meaningful.
[[noreturn]] void trap(TrapKind kind, const char* detail);

// C-callable entry point for generated code: the kind travels as an int so
// the calling convention does not depend on how a scoped enum is passed.
extern "C" void wasm_trap_raise(int kind, const char* message);

// Arms a trap target for its lifetime; scopes nest LIFO. The buffer is public
// because setjmp must be expanded in the embedding function's own frame.
class TrapScope
{
public:
	std::jmp_buf buffer;
	explicit TrapScope();
	~TrapScope();
	TrapScope(const TrapScope&) = delete;
	TrapScope& operator=(const TrapScope&) = delete;

	// The reason for the trap that jumped back to `buffer`, or nullptr.
	const char* message() const;

	// The kind of the trap that jumped back to `buffer`.
	TrapKind kind() const;

private:
	TrapScope* previous;
};

} // namespace WASM

#endif // WASMTRAP_HPP
