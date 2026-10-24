#ifndef WASMCONTINUATION_HPP
#define WASMCONTINUATION_HPP

#include <cstddef>
#include <cstdint>

namespace WASM {

// ── Continuations: the backend-neutral contract ─────────────────────────────
//
// A continuation is an execution context that is not the current one
// (docs/proposals/STACK_SWITCHING.md §10.8). This header is the contract a
// backend emits calls against; it deliberately knows nothing about the
// mechanism behind it.
//
// **A is the default provider:** an mmap-backed native stack with a
// `PROT_NONE` guard page and a `ucontext`-based switch. A can be resumed or
// dropped on another host thread when calls are serial (the engine TLS and
// backend state travel with the stack); concurrent use is not allowed. **B is
// the explicit fallback:** one host thread per suspended continuation, with a
// condition-variable hand-off. Selecting B does not make the engine multi-agent;
// both providers execute one continuation at a time and keep the single-agent
// model.
//
// The first slice is intentionally narrow and honest:
//   * one i32 parameter and one i32 result at most (or void), for the
//     continuation function, the control tag and the handler;
//   * `cont.new`, `suspend` and `resume` with at most one `(on $tag $label)`
//     handler;
//   * one-shot continuations: resuming a consumed continuation traps;
//   * a trap or an uncaught exception in the resumed context is transferred to
//     the resumer.
// `cont.bind`, `resume_throw`, `resume_throw_ref`, `switch` and multi-value
// payloads remain rejected until the provider and the semantics are extended
// (STACK_SWITCHING.md §10.12 stage 4).
//
// A continuation reference behaves like every other reference: it is cloned
// and dropped by the generic reference helpers. `continuationRefRetain` /
// `continuationRefRelease` make that work without teaching the GC heap about
// stacks. Dropping a suspended A continuation unwinds it on its current host
// thread; a suspended stack may migrate between threads as long as it is not
// used concurrently. Dropping a suspended B continuation cancels and joins its
// worker.
//
// Lifetime: the target is a function reference owned by a module instance (or a
// native import). As with any `Callable`, that owner must outlive every
// continuation created from it; the provider does not clone the code or the
// `VMContext`.
//
// The host-facing `HostContinuation` below is the JSPI/embedder seam. It is not
// an agent or scheduler API; true multi-agent execution remains deferred.

// The result of `wasm_cont_resume`. The layout is part of the generated-code
// ABI: the backend loads the fields by offset (docs/ABI.md).
enum class ContinuationStatus : std::int32_t {
	Completed = 0, // the resumed context returned; `value` is its result
	Suspended = 1, // it suspended; `tagId`, `value` and `continuation` are valid
	Trapped = 2,   // a trap was transferred to the resumer's execution state
	Exception = 3, // an uncaught exception was transferred to the resumer
	Invalid = 4,   // the helper already reported a trap on the resumer
};

struct ContinuationResumeResult {
	std::int32_t status = static_cast<std::int32_t>(ContinuationStatus::Invalid);
	std::int32_t tagId = 0;
	std::int32_t value = 0;
	std::int32_t reserved = 0; // keeps `continuation` 8-byte aligned
	void*        continuation = nullptr;
};

// ── Provider selection ─────────────────────────────────────────────────────
// A (native stacks + context switch) is the product default. B (one host
// thread per suspended continuation) is the explicit fallback/prototype. The
// selection is snapshot at `cont.new`; existing continuations keep their
// provider. Calls are serial: A may migrate between host threads, but no two
// threads may use one continuation at the same time.
enum class ContinuationProvider : std::int32_t {
	NativeStacks = 0, // A — mmap'd stack + ucontext switch (default)
	HostThreads = 1,  // B — one host thread per suspended continuation
};

ContinuationProvider continuationProvider() noexcept;
void setContinuationProvider(ContinuationProvider provider) noexcept;

// ── Provider budget ────────────────────────────────────────────────────────
// `maxLive` bounds live continuation objects; `stackBytes` is the provider's
// stack request per live continuation (A mmap size, B pthread stack size).
// Both can be overridden with `WASMJIT_MAX_CONTINUATIONS` /
// `WASMJIT_CONT_STACK_BYTES`; an embedder can also set them explicitly.
// `cont.new` over budget reports `OutOfMemory` and returns null, which the
// generated code turns into a trap.
struct ContinuationBudget {
	std::uint32_t maxLive = 64;
	std::uint32_t stackBytes = 512 * 1024;
};

ContinuationBudget continuationBudget() noexcept;
void setContinuationBudget(ContinuationBudget budget) noexcept;
std::uint32_t liveContinuationCount() noexcept;

// ── Reference integration ─────────────────────────────────────────────────
// True when `ref` is a live continuation handle. Retain/release are the
// reference-counting pair; the runtime's `refClone`/`refDrop` call them before
// the GC-object path. Retain returns false when `ref` is not a continuation.
bool isContinuationRef(const void* ref) noexcept;
bool continuationRefRetain(void* ref) noexcept;
bool continuationRefRelease(void* ref) noexcept;

// ── Host-facing handle ─────────────────────────────────────────────────────
// A host receives a continuation as the `void*` lowering of a `(ref cont)`
// value (for example, the result of an exported function). `HostContinuation`
// owns one reference and makes the same operations available to C++ that the
// generated code uses:
//
//   * `resume` consumes the handle and blocks until the continuation completes
//     or suspends; a suspension returns the replacement continuation in
//     `result.continuation` (wrap it with `HostContinuation(ptr, true)`);
//   * `suspendCurrent` is for a native import that wants JSPI-style
//     "yield the current computation, resume later" behaviour. It returns 1
//     when resumed and 0 when there is no current continuation or the worker
//     was cancelled; the trap slot says why.
//
// This is deliberately *not* an agent or scheduler API. Calls are serial: one
// host thread resumes a continuation at a time, and A continuations are bound
// to their creating thread. True multi-agent execution remains deferred.
class HostContinuation {
public:
	HostContinuation() noexcept = default;
	// `takeOwnership` is true when the caller transfers one reference (the
	// usual case for a value returned by Wasm); false retains a new reference.
	HostContinuation(void* ref, bool takeOwnership) noexcept;
	~HostContinuation();

	HostContinuation(HostContinuation&& other) noexcept;
	HostContinuation& operator=(HostContinuation&& other) noexcept;
	HostContinuation(const HostContinuation&) = delete;
	HostContinuation& operator=(const HostContinuation&) = delete;

	bool valid() const noexcept { return ref_ != nullptr; }
	void* raw() const noexcept { return ref_; }
	void reset() noexcept;

	// Consumes this handle (whether the continuation completes, suspends,
	// traps or throws). `result` is required.
	bool resume(std::int32_t arg, ContinuationResumeResult* result) noexcept;

	// Host-import entry point: suspend the current continuation.
	static std::int32_t suspendCurrent(std::int32_t tag, std::int32_t value,
			std::int32_t* resumedValue) noexcept;

private:
	void* ref_ = nullptr;
};

// ── Generated-code entry points (C ABI) ───────────────────────────────────
// `callable` is the target function reference; `context` is the creating
// instance's `VMContext*` (used for a native import whose `Callable::context`
// is null, exactly as a direct call passes the caller's context). The two flags
// describe the target's signature (`1` for an i32 parameter/result, `0` for
// absent). The returned handle owns one reference.
extern "C" {

void* wasm_cont_new(void* callable, void* context, std::uint32_t paramCount,
		std::int32_t takesArg, std::int32_t returnsValue);

// `cont.bind`: consume the continuation identity and bind a prefix of its
// remaining arguments. The caller still owns/releases the old handle ref.
void* wasm_cont_bind(void* continuation, const std::int32_t* values,
		std::uint32_t count);

// Resumes `continuation` with `arg`. Blocks until it completes, suspends,
// traps or raises an uncaught exception. Trap/exception statuses have already
// been transferred to the *resumer's* execution state, so the generated code's
// usual pending checks handle them.
void wasm_cont_resume(void* continuation, std::int32_t arg,
		ContinuationResumeResult* result);
void wasm_cont_resume_throw(void* continuation, void* exception,
		ContinuationResumeResult* result);

// Suspends the current continuation on `tagId`, carrying `value`. Blocks until
// the continuation is resumed; the resume argument is written through
// `result` when non-null. A dropped suspended continuation reports an internal
// trap here so the worker can unwind by returning.
std::int32_t wasm_cont_suspend(std::int32_t tagId, std::int32_t value,
		std::int32_t* result);

} // extern "C"

} // namespace WASM

#endif // WASMCONTINUATION_HPP
