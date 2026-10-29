#ifndef WASMCONTINUATION_HPP
#define WASMCONTINUATION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmContinuation.hpp
  \brief The continuation-provider contract and its status codes.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace WASM {

//! \par Continuations: the backend-neutral contract
//!
//! A continuation is an execution context that is not the current one
//! (docs/proposals/STACK_SWITCHING.md §10.8). This header is the contract a
//! backend emits calls against; it deliberately knows nothing about the
//! mechanism behind it.
//!
//! **A is the default provider:** an mmap-backed native stack with a
//! `PROT_NONE` guard page and a `ucontext`-based switch. A can be resumed or
//! dropped on another host thread when calls are serial (the engine TLS and
//! backend state travel with the stack); concurrent use is not allowed. **B is
//! the explicit fallback:** one host thread per suspended continuation, with a
//! condition-variable hand-off. Selecting B does not make the engine multi-agent;
//! both providers execute one continuation at a time and keep the single-agent
//! model.
//!
//! The wide value ABI uses fixed 16-byte slots (`ContinuationValue`) and a
//! backend-supplied invoker (`ContinuationInvokeFn`): i32/i64/f32/f64,
//! references and v128 can cross `cont.new`, `cont.bind`, `suspend`, `resume`
//! and `resume_throw*`. The scalar `wasm_cont_*` entry points remain as a
//! compatibility path for i32/void tests; the generated code uses the `_ex`
//! forms. `switch` remains rejected until its prompt/peer model is implemented
//! (STACK_SWITCHING.md §10.12 stage 4).
//!
//! A continuation reference behaves like every other reference: it is cloned
//! and dropped by the generic reference helpers. `continuationRefRetain` /
//! `continuationRefRelease` make that work without teaching the GC heap about
//! stacks. Dropping a suspended A continuation unwinds it on its current host
//! thread; a suspended stack may migrate between threads as long as it is not
//! used concurrently. Dropping a suspended B continuation cancels and joins its
//! worker.
//!
//! Lifetime: the target is a function reference owned by a module instance (or a
//! native import). As with any `Callable`, that owner must outlive every
//! continuation created from it; the provider does not clone the code or the
//! `VMContext`.
//!
//! The host-facing `HostContinuation` below is the JSPI/embedder seam. It is not
//! an agent or scheduler API; true multi-agent execution remains deferred.

//! The result of `wasm_cont_resume`. The layout is part of the generated-code
//! ABI: the backend loads the fields by offset (docs/ABI.md).
enum class ContinuationStatus : std::int32_t {
	Completed = 0, //!< the resumed context returned; `value` is its result
	Suspended = 1, //!< it suspended; `tagId`, `value` and `continuation` are valid
	Trapped = 2,   //!< a trap was transferred to the resumer's execution state
	Exception = 3, //!< an uncaught exception was transferred to the resumer
	Invalid = 4,   //!< the helper already reported a trap on the resumer
};

struct ContinuationResumeResult {
	std::int32_t status = static_cast<std::int32_t>(ContinuationStatus::Invalid);
	std::int32_t tagId = 0;
	std::int32_t value = 0; //!< first i32 result/tag payload, for the narrow ABI
	std::int32_t reserved = 0; //!< keeps `continuation` 8-byte aligned
	void*        continuation = nullptr;
	//! Wide result/tag buffers. The caller owns the storage; the runtime moves
	//! values into it and sets the matching count. `tagKinds` describes the tag
	//! payload for an unhandled suspension so the caller can drop it correctly.
	std::uint32_t resultCount = 0;
	std::uint32_t tagValueCount = 0;
	void*        resultValues = nullptr;
	void*        tagValues = nullptr;
	const std::uint8_t* tagKinds = nullptr;
};

//! One continuation value slot. The runtime never interprets the payload; the
//! backend thunk knows the Wasm type. 16-byte slots make i32/i64/f32/f64/ref
//! and v128 equally addressable. Scalar values live at offset 0.
enum class ContinuationValueKind : std::uint8_t {
	I32 = 0,
	I64 = 1,
	F32 = 2,
	F64 = 3,
	Ref = 4,
	V128 = 5,
};
inline constexpr std::size_t kContinuationValueStride = 16;
struct alignas(16) ContinuationValue {
	std::uint64_t words[2] = {0, 0};
};

//! Backend-supplied generic target invocation. `target` is a `Callable*` and
//! `context` is the creation context; the callback calls the target's C ABI
//! using the kind arrays. It is implemented by the executing backend so
//! WasmBase does not depend on a particular backend library.
using ContinuationInvokeFn = void (*)(void* target, void* context,
		const ContinuationValue* args, std::uint32_t argCount,
		ContinuationValue* results, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds);

//! \brief Provider selection
//! A (native stacks + context switch) is the product default. B (one host
//! thread per suspended continuation) is the explicit fallback/prototype. The
//! selection is snapshot at `cont.new`; existing continuations keep their
//! provider. Calls are serial: A may migrate between host threads, but no two
//! threads may use one continuation at the same time.
enum class ContinuationProvider : std::int32_t {
	NativeStacks = 0, //!< A — mmap'd stack + ucontext switch (default)
	HostThreads = 1,  //!< B — one host thread per suspended continuation
};

ContinuationProvider continuationProvider() noexcept;
void setContinuationProvider(ContinuationProvider provider) noexcept;

//! \brief Provider budget
//! `maxLive` bounds live continuation objects; `stackBytes` is the provider's
//! stack request per live continuation (A mmap size, B pthread stack size).
//! Both can be overridden with `WASMJIT_MAX_CONTINUATIONS` /
//! `WASMJIT_CONT_STACK_BYTES`; an embedder can also set them explicitly.
//! `cont.new` over budget reports `OutOfMemory` and returns null, which the
//! generated code turns into a trap.
struct ContinuationBudget {
	std::uint32_t maxLive = 64;
	std::uint32_t stackBytes = 512 * 1024;
};

ContinuationBudget continuationBudget() noexcept;
void setContinuationBudget(ContinuationBudget budget) noexcept;
std::uint32_t liveContinuationCount() noexcept;

//! \par Reference integration
//! True when `ref` is a live continuation handle. Retain/release are the
//! reference-counting pair; the runtime's `refClone`/`refDrop` call them before
//! the GC-object path. Retain returns false when `ref` is not a continuation.
bool isContinuationRef(const void* ref) noexcept;
bool continuationRefRetain(void* ref) noexcept;
bool continuationRefRelease(void* ref) noexcept;

//! \brief Host-facing handle
//! A host receives a continuation as the `void*` lowering of a `(ref cont)`
//! value (for example, the result of an exported function). `HostContinuation`
//! owns one reference and makes the same operations available to C++ that the
//! generated code uses:
//!
//!   * `resume` consumes the handle and blocks until the continuation completes
//!     or suspends; a suspension returns the replacement continuation in
//!     `result.continuation` (wrap it with `HostContinuation(ptr, true)`);
//!   * `suspendCurrent` is for a native import that wants JSPI-style
//!     "yield the current computation, resume later" behaviour. It returns 1
//!     when resumed and 0 when there is no current continuation or the worker
//!     was cancelled; the trap slot says why.
//!
//! This is deliberately *not* an agent or scheduler API. Calls are serial: one
//! host thread resumes a continuation at a time, and A continuations are bound
//! to their creating thread. True multi-agent execution remains deferred.
class HostContinuation {
public:
	HostContinuation() noexcept = default;
	//! `takeOwnership` is true when the caller transfers one reference (the
	//! usual case for a value returned by Wasm); false retains a new reference.
	HostContinuation(void* ref, bool takeOwnership) noexcept;
	~HostContinuation();

	HostContinuation(HostContinuation&& other) noexcept;
	HostContinuation& operator=(HostContinuation&& other) noexcept;
	HostContinuation(const HostContinuation&) = delete;
	HostContinuation& operator=(const HostContinuation&) = delete;

	bool valid() const noexcept { return ref_ != nullptr; }
	void* raw() const noexcept { return ref_; }
	void reset() noexcept;

	//! Consumes this handle (whether the continuation completes, suspends,
	//! traps or throws). `result` is required.
	bool resume(std::int32_t arg, ContinuationResumeResult* result) noexcept;

	//! Host-import entry point: suspend the current continuation.
	static std::int32_t suspendCurrent(std::int32_t tag, std::int32_t value,
			std::int32_t* resumedValue) noexcept;

private:
	void* ref_ = nullptr;
};

//! \brief Embedder-owned scheduling / JSPI-style promise seam
//! WasmJit deliberately has no JavaScript, Lua or Python engine. This class is
//! the opt-in contract an embedder's glue code uses to connect a language
//! runtime's promise/task queue to a suspended Wasm continuation:
//!
//!   * `submit` owns (or retains) a continuation and queues a resume value;
//!   * `step` resumes exactly one task and invokes the hooks:
//!       - `onSuspended` receives the *replacement* continuation (ownership
//!         transferred to the callback) plus the tag and payload. The embedder
//!         stores it behind its Promise/async task and later calls `submit`;
//!       - `onCompleted` receives the final status; a trap/exception has
//!         already been recorded in the engine's per-execution-state slots;
//!   * `run` drains the queue, and the embedder may interleave `step` with its
//!     own event loop.
//!
//! No hook is required. With no hooks the scheduler still resumes tasks and
//! releases replacement continuations, which makes it useful as a simple
//! trampoline. Hooks are called synchronously on the thread that calls `step`,
//! so the scheduler remains serial and is not an agent API. `submit`/`onSuspended`
//! are the legacy scalar-i32 path; `submitEx`/`submitThrow` and the `*Ex` hooks
//! carry wide `ContinuationValue` payloads and a throw/reject task. A reference
//! payload is retained by the scheduler for the task and released after `step`.
//! True multi-agent execution remains deferred.
class HostContinuationScheduler {
public:
	//! Default capacity for wide result/tag buffers. Callers can pass explicit
	//! capacities to `submitEx`; the completion result capacity is additionally
	//! clamped to the target's declared result count.
	static constexpr std::uint32_t kDefaultValueCapacity = 64;

	struct Hooks {
		void* userData = nullptr;
		//! Legacy scalar hooks. Ownership of `continuation` is transferred to
		//! the callback. The callback must release it (or keep it alive with
		//! `submit`).
		void (*onSuspended)(void* userData, void* continuation,
				std::int32_t tagId, std::int32_t value) noexcept = nullptr;
		//! `status` is a `ContinuationStatus` value. Traps/exceptions are also
		//! visible through the usual pending slots.
		void (*onCompleted)(void* userData, std::int32_t status) noexcept = nullptr;
		//! Wide hooks. `values`/`kinds` are borrowed for the duration of the
		//! call; copy them if the embedder needs them later. Ownership of
		//! `continuation` is transferred exactly as for `onSuspended`.
		void (*onSuspendedEx)(void* userData, void* continuation,
				std::int32_t tagId, const ContinuationValue* values,
				std::uint32_t valueCount, const std::uint8_t* kinds) noexcept = nullptr;
		void (*onCompletedEx)(void* userData, std::int32_t status,
				const ContinuationValue* results, std::uint32_t resultCount,
				const std::uint8_t* kinds) noexcept = nullptr;
		//! Called instead of the completion hooks when a task ends with an
		//! uncaught exception. `exception` is the engine's pending exception
		//! payload and is borrowed for the call.
		void (*onRejected)(void* userData, std::int32_t status,
				void* exception) noexcept = nullptr;
	};

	HostContinuationScheduler() noexcept = default;
	explicit HostContinuationScheduler(Hooks hooks) noexcept : hooks_(hooks) {}
	~HostContinuationScheduler();

	HostContinuationScheduler(const HostContinuationScheduler&) = delete;
	HostContinuationScheduler& operator=(const HostContinuationScheduler&) = delete;

	Hooks& hooks() noexcept { return hooks_; }
	const Hooks& hooks() const noexcept { return hooks_; }

	//! Queue a continuation. `takeOwnership` has the same meaning as
	//! `HostContinuation(ref, takeOwnership)`. An invalid reference is rejected.
	bool submit(void* continuation, std::int32_t arg,
			bool takeOwnership = true) noexcept;

	//! Wide resume task. `argKinds` is optional, but must describe reference
	//! arguments if their lifetime is to be retained. `resultCapacity`/`tagCapacity`
	//! default to the target's result count and `kDefaultValueCapacity`.
	bool submitEx(void* continuation, const ContinuationValue* args,
			std::uint32_t argCount, const std::uint8_t* argKinds,
			std::uint32_t resultCapacity = 0, std::uint32_t tagCapacity = 0,
			bool takeOwnership = true) noexcept;

	//! Throw/reject task: resumes through `wasm_cont_resume_throw_ex`. The
	//! exception is borrowed until `step` runs; ownership is not transferred.
	bool submitThrow(void* continuation, void* exception,
			bool takeOwnership = true) noexcept;

	std::size_t pending() const noexcept { return tasks_.size(); }
	void clear() noexcept;

	//! Resume one queued task. Returns false when the queue is empty.
	bool step() noexcept;
	//! Resume tasks until the queue is empty. Returns the number of tasks run.
	std::size_t run() noexcept;

private:
	struct Task {
		enum class Kind { Resume, Throw };
		Kind kind = Kind::Resume;
		void* continuation = nullptr;
		std::int32_t arg = 0;
		std::vector<ContinuationValue> args;
		std::vector<std::uint8_t> argKinds;
		void* exception = nullptr;
		void* creationContext = nullptr;
		std::uint32_t resultCapacity = 0;
		std::uint32_t tagCapacity = 0;
		bool wide = false;
	};
	Hooks hooks_{};
	std::deque<Task> tasks_{};
};

//! \par Generated-code entry points (C ABI)
//! `callable` is the target function reference; `context` is the creating
//! instance's `VMContext*` (used for a native import whose `Callable::context`
//! is null, exactly as a direct call passes the caller's context). The two flags
//! describe the target's signature (`1` for an i32 parameter/result, `0` for
//! absent). The returned handle owns one reference.
extern "C" {

void* wasm_cont_new(void* callable, void* context, std::uint32_t paramCount,
		std::int32_t takesArg, std::int32_t returnsValue);

//! \par Wide continuation ABI
//! `wasm_cont_new_ex` is the backend-facing constructor. It copies the kind
//! arrays and stores the backend invoker; the target's params/results may be
//! any supported scalar, reference or v128.
void* wasm_cont_new_ex(void* callable, void* context, ContinuationInvokeFn invoke,
		std::uint32_t paramCount, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds);

void* wasm_cont_bind_ex(void* continuation, const ContinuationValue* values,
		std::uint32_t count);

//! Resume with a wide argument buffer. `results`/`tagValues` are caller-owned
//! storage of `capacity` slots (16 bytes each); the runtime moves values in and
//! records the counts in `result`. The old `wasm_cont_resume` wrappers remain
//! for the scalar i32 ABI.
void wasm_cont_resume_ex(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationValue* tagValues,
		std::uint32_t tagCapacity, ContinuationResumeResult* result);
void wasm_cont_resume_throw_ex(void* continuation, void* exception,
		ContinuationValue* results, std::uint32_t resultCapacity,
		ContinuationValue* tagValues, std::uint32_t tagCapacity,
		ContinuationResumeResult* result);

//! Symmetric switch: capture the calling continuation, transfer to `continuation`
//! with the caller's continuation appended to the target arguments, and return
//! when the target switches back. `results` receives the caller's continuation
//! parameters. Both providers implement the peer handoff; cross-provider
//! handoff is rejected explicitly.
void wasm_cont_switch(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationResumeResult* result);

//! Suspend with a wide payload/result buffer. `argKinds` describes the tag
//! parameters; `resultKinds` describes the values the future resume supplies.
std::int32_t wasm_cont_suspend_ex(std::int32_t tagId,
		const ContinuationValue* args, std::uint32_t argCount,
		const std::uint8_t* argKinds, ContinuationValue* results,
		std::uint32_t resultCapacity, std::uint32_t* resultCount,
		const std::uint8_t* resultKinds);

//! Suspended-stack roots for a tracing collector. The refcounted engine keeps
//! these references alive on its own, but a tracing collector needs the explicit
//! list. The generated code registers pointer-typed locals/operand slots before
//! `suspend`; the provider transfers them to the collector's root set while the
//! stack is suspended and drops them when it resumes.
void wasm_cont_add_root(void* ref);

//! Drops reference values in a caller-owned wide buffer (for an unhandled
//! suspension). No-op for scalar/v128 slots.
void wasm_cont_drop_values(void* context, const std::uint8_t* kinds,
		std::uint32_t count, void* values);


//! `cont.bind`: consume the continuation identity and bind a prefix of its
//! remaining arguments. The caller still owns/releases the old handle ref.
void* wasm_cont_bind(void* continuation, const std::int32_t* values,
		std::uint32_t count);

//! Resumes `continuation` with `arg`. Blocks until it completes, suspends,
//! traps or raises an uncaught exception. Trap/exception statuses have already
//! been transferred to the *resumer's* execution state, so the generated code's
//! usual pending checks handle them.
void wasm_cont_resume(void* continuation, std::int32_t arg,
		ContinuationResumeResult* result);
void wasm_cont_resume_throw(void* continuation, void* exception,
		ContinuationResumeResult* result);

//! Suspends the current continuation on `tagId`, carrying `value`. Blocks until
//! the continuation is resumed; the resume argument is written through
//! `result` when non-null. A dropped suspended continuation reports an internal
//! trap here so the worker can unwind by returning.
std::int32_t wasm_cont_suspend(std::int32_t tagId, std::int32_t value,
		std::int32_t* result);

} // extern "C"

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMCONTINUATION_HPP
