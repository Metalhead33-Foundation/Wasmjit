#ifndef WASMTRAP_HPP
#define WASMTRAP_HPP
#include <csetjmp>
#include <cstddef>

namespace WASM {

// ── Wasm traps ──────────────────────────────────────────────────────────────
//
// A trap is a non-local exit from the current Wasm invocation. The engine's
// own frames — JIT-generated code plus the POD native helpers it calls — carry
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

struct TrapState {
	TrapScope*  active;   // innermost armed scope, or nullptr
	const char* message;  // reason for the most recent trap, or nullptr
};

// The thread-local trap state. One engine instance executes on one thread;
// this is thread_local so a future multi-agent build degrades safely.
TrapState& trapState();

// Raise a trap. Never returns: jumps to the innermost armed scope, or prints a
// diagnostic and aborts when no scope is armed.
[[noreturn]] void trap(const char* message);

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

private:
	TrapScope* previous;
};

} // namespace WASM

#endif // WASMTRAP_HPP
