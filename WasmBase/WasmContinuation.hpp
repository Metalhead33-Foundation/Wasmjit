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

//! The result of `wasm_cont_resume`. The layout is part of the generated-code
//! ABI: the backend loads the fields by offset (docs/ABI.md).
struct ContinuationResumeResult {
	//! The `ContinuationStatus` value that says which of the fields below are valid.
	std::int32_t status = static_cast<std::int32_t>(ContinuationStatus::Invalid);
	//! The tag a suspension raised, valid when `status` is Suspended.
	std::int32_t tagId = 0;
	std::int32_t value = 0; //!< first i32 result/tag payload, for the narrow ABI
	std::int32_t reserved = 0; //!< keeps `continuation` 8-byte aligned
	void*        continuation = nullptr;
	//! Wide result/tag buffers. The caller owns the storage; the runtime moves
	//! values into it and sets the matching count. `tagKinds` describes the tag
	//! payload for an unhandled suspension so the caller can drop it correctly.
	std::uint32_t resultCount = 0;
	//! Number of wide tag payload slots written into `tagValues`.
	std::uint32_t tagValueCount = 0;
	//! Caller-owned wide buffer the result values are moved into, one 16-byte slot
	//! per result; null for the narrow ABI.
	void*        resultValues = nullptr;
	//! Caller-owned wide buffer the tag payload is moved into, valid for an
	//! unhandled suspension.
	void*        tagValues = nullptr;
	//! `ContinuationValueKind` of each tag payload slot, so the caller can drop the
	//! reference values it owns.
	const std::uint8_t* tagKinds = nullptr;
};

//! One continuation value slot. The runtime never interprets the payload; the
//! backend thunk knows the Wasm type. 16-byte slots make i32/i64/f32/f64/ref
//! and v128 equally addressable. Scalar values live at offset 0.
enum class ContinuationValueKind : std::uint8_t {
	//! The slot holds a 32-bit integer.
	I32 = 0,
	//! The slot holds a 64-bit integer.
	I64 = 1,
	//! The slot holds a 32-bit float.
	F32 = 2,
	//! The slot holds a 64-bit float.
	F64 = 3,
	//! The slot holds a reference, which the runtime must retain and drop.
	Ref = 4,
	//! The slot holds a 128-bit vector, addressed through the same 16 bytes.
	V128 = 5,
};
//! Size of one `ContinuationValue` slot in bytes, so that every scalar,
//! reference and `v128` payload is equally addressable.
inline constexpr std::size_t kContinuationValueStride = 16;
//! One 16-byte continuation value slot. The runtime copies it without
//! interpreting it; the backend thunk knows the Wasm type from the kind arrays.
struct alignas(16) ContinuationValue {
	//! The raw payload. A scalar lives in `words[0]`, a `v128` uses both words.
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

//! \return the provider that `cont.new` snapshots for newly created
//! continuations
ContinuationProvider continuationProvider() noexcept;
//! \param provider the provider to use for continuations created from now on;
//! existing continuations keep the provider they were created with
void setContinuationProvider(ContinuationProvider provider) noexcept;

//! \brief Provider budget
//! `maxLive` bounds live continuation objects; `stackBytes` is the provider's
//! stack request per live continuation (A mmap size, B pthread stack size).
//! Both can be overridden with `WASMJIT_MAX_CONTINUATIONS` /
//! `WASMJIT_CONT_STACK_BYTES`; an embedder can also set them explicitly.
//! `cont.new` over budget reports `OutOfMemory` and returns null, which the
//! generated code turns into a trap.
//! Provider budget: how many continuations may be live and how large a stack
//! each one asks for. Both values can be overridden through the environment
//! (see the note above) or set explicitly through `setContinuationBudget`.
struct ContinuationBudget {
	//! Upper bound on live continuation objects; `cont.new` beyond it reports
	//! `OutOfMemory` and returns null.
	std::uint32_t maxLive = 64;
	//! Stack request per live continuation: the mmap size of a native stack, or
	//! the pthread stack size of a worker thread.
	std::uint32_t stackBytes = 512 * 1024;
};

//! \return the budget currently in force
ContinuationBudget continuationBudget() noexcept;
//! \param budget the budget to apply to continuations created from now on
void setContinuationBudget(ContinuationBudget budget) noexcept;
//! \return the number of continuation handles that are currently live
std::uint32_t liveContinuationCount() noexcept;

//! \par Reference integration
//! True when `ref` is a live continuation handle. Retain/release are the
//! reference-counting pair; the runtime's `refClone`/`refDrop` call them before
//! the GC-object path. Retain returns false when `ref` is not a continuation.
//! \param ref the reference value to classify
//! \return true when `ref` is a live continuation handle rather than a GC object
bool isContinuationRef(const void* ref) noexcept;
//! \param ref the reference whose ownership count is raised
//! \return true when `ref` was a continuation and was retained, false when it
//! is not a continuation
bool continuationRefRetain(void* ref) noexcept;
//! \param ref the reference whose ownership count is lowered
//! \return true when the count reached zero and the continuation was
//! destroyed, false for a reference that is not a continuation
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
	//! Constructs an empty handle; `valid()` is false until one is assigned.
	HostContinuation() noexcept = default;
	//! `takeOwnership` is true when the caller transfers one reference (the
	//! usual case for a value returned by Wasm); false retains a new reference.
	//! \param ref the continuation handle to wrap; null leaves the handle empty
	//! \param takeOwnership true when `ref` transfers one reference to this
	//! handle, false when the handle should retain a new one
	HostContinuation(void* ref, bool takeOwnership) noexcept;
	//! Releases the reference this handle owns.
	~HostContinuation();

	//! \param other the handle whose reference is moved; it becomes empty
	HostContinuation(HostContinuation&& other) noexcept;
	//! \param other the handle whose reference is moved; it becomes empty
	//! \return this handle, now owning the moved reference
	HostContinuation& operator=(HostContinuation&& other) noexcept;
	//! Deleted: a handle owns exactly one reference, so it is moved, never copied.
	HostContinuation(const HostContinuation&) = delete;
	//! Deleted: a handle owns exactly one reference, so it is moved, never copied.
	HostContinuation& operator=(const HostContinuation&) = delete;

	//! \return true when the handle owns a continuation reference
	bool valid() const noexcept { return ref_ != nullptr; }
	//! \return the underlying `void*` handle, or null when the handle is empty
	void* raw() const noexcept { return ref_; }
	//! Releases the owned reference and leaves the handle empty.
	void reset() noexcept;

	//! Consumes this handle (whether the continuation completes, suspends,
	//! traps or throws). `result` is required.
	//! \param arg the scalar i32 argument the continuation is resumed with
	//! \param result receives the status of the resume and the values it
	//! produced; must not be null
	//! \return true when the call was made, false when the handle was empty
	bool resume(std::int32_t arg, ContinuationResumeResult* result) noexcept;

	//! Host-import entry point: suspend the current continuation.
	//! \param tag the tag the suspension carries
	//! \param value the scalar i32 payload of the suspension
	//! \param resumedValue receives the scalar argument of the resume, when
	//! non-null and the suspension is resumed
	//! \return 1 when the continuation was resumed, 0 when there is no current
	//! continuation or the worker was cancelled (the trap slot says why)
	static std::int32_t suspendCurrent(std::int32_t tag, std::int32_t value,
			std::int32_t* resumedValue) noexcept;

private:
	//! The owned continuation reference, or null when the handle is empty.
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

	//! The embedder callbacks. Every hook is optional: a null pointer means the
	//! scheduler stays silent for that event.
	struct Hooks {
		//! Opaque pointer handed back to every hook unchanged.
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
		//! Wide suspension hook, invoked in place of `onSuspended` when the narrow
		//! hook is null.
		void (*onSuspendedEx)(void* userData, void* continuation,
				std::int32_t tagId, const ContinuationValue* values,
				std::uint32_t valueCount, const std::uint8_t* kinds) noexcept = nullptr;
		//! Wide completion hook, invoked in place of `onCompleted` when the narrow
		//! hook is null.
		void (*onCompletedEx)(void* userData, std::int32_t status,
				const ContinuationValue* results, std::uint32_t resultCount,
				const std::uint8_t* kinds) noexcept = nullptr;
		//! Called instead of the completion hooks when a task ends with an
		//! uncaught exception. `exception` is the engine's pending exception
		//! payload and is borrowed for the call.
		void (*onRejected)(void* userData, std::int32_t status,
				void* exception) noexcept = nullptr;
	};

	//! Constructs a scheduler with no hooks and an empty queue.
	HostContinuationScheduler() noexcept = default;
	//! \param hooks the callbacks the scheduler reports through
	explicit HostContinuationScheduler(Hooks hooks) noexcept : hooks_(hooks) {}
	//! Drops the queued tasks, releasing the continuations they own.
	~HostContinuationScheduler();

	//! Deleted: the scheduler owns its queue and hooks, so it cannot be copied.
	HostContinuationScheduler(const HostContinuationScheduler&) = delete;
	//! Deleted: the scheduler owns its queue and hooks, so it cannot be copied.
	HostContinuationScheduler& operator=(const HostContinuationScheduler&) = delete;

	//! \return the hooks the scheduler was constructed with, for in-place editing
	Hooks& hooks() noexcept { return hooks_; }
	//! \return the hooks the scheduler was constructed with
	const Hooks& hooks() const noexcept { return hooks_; }

	//! Queue a continuation. `takeOwnership` has the same meaning as
	//! `HostContinuation(ref, takeOwnership)`. An invalid reference is rejected.
	//! \param continuation the continuation the task resumes
	//! \param arg the scalar i32 argument the resume passes
	//! \param takeOwnership true when `continuation` transfers one reference to
	//! the queue, false when the queue retains its own
	//! \return true when the task was queued, false for an invalid reference
	bool submit(void* continuation, std::int32_t arg,
			bool takeOwnership = true) noexcept;

	//! Wide resume task. `argKinds` is optional, but must describe reference
	//! arguments if their lifetime is to be retained. `resultCapacity`/`tagCapacity`
	//! default to the target's result count and `kDefaultValueCapacity`.
	//! \param continuation the continuation the task resumes
	//! \param args the wide argument values, `argCount` of them
	//! \param argCount the number of argument slots in `args`
	//! \param argKinds the `ContinuationValueKind` of each argument, or null
	//! when no reference argument needs to be retained
	//! \param resultCapacity the number of result slots to reserve, zero for the
	//! target’s own result count
	//! \param tagCapacity the number of tag payload slots to reserve, zero for
	//! the default capacity
	//! \param takeOwnership true when `continuation` transfers one reference to
	//! the queue
	//! \return true when the task was queued, false for an invalid reference
	bool submitEx(void* continuation, const ContinuationValue* args,
			std::uint32_t argCount, const std::uint8_t* argKinds,
			std::uint32_t resultCapacity = 0, std::uint32_t tagCapacity = 0,
			bool takeOwnership = true) noexcept;

	//! Throw/reject task: resumes through `wasm_cont_resume_throw_ex`. The
	//! exception is borrowed until `step` runs; ownership is not transferred.
	//! \param continuation the continuation the exception is thrown into
	//! \param exception the borrowed exception object; ownership is not
	//! transferred
	//! \param takeOwnership true when `continuation` transfers one reference to
	//! the queue
	//! \return true when the task was queued, false for an invalid reference
	bool submitThrow(void* continuation, void* exception,
			bool takeOwnership = true) noexcept;

	//! \return the number of tasks waiting to be resumed
	std::size_t pending() const noexcept { return tasks_.size(); }
	//! Drops every queued task, releasing the continuations they own.
	void clear() noexcept;

	//! Resume one queued task. Returns false when the queue is empty.
	//! \return true when a task was resumed, false when the queue was empty
	bool step() noexcept;
	//! Resume tasks until the queue is empty. Returns the number of tasks run.
	//! \return the number of tasks that were resumed
	std::size_t run() noexcept;

private:
	//! One queued resume or throw, with the values it carries.
	struct Task {
		//! Which entry point runs the task: a resume with arguments, or a resume that
		//! throws into the continuation.
		enum class Kind { Resume, Throw };
		//! Whether the task resumes with values or throws.
		Kind kind = Kind::Resume;
		//! The continuation the task owns; released after it is stepped.
		void* continuation = nullptr;
		//! The scalar i32 argument of a narrow resume.
		std::int32_t arg = 0;
		//! The wide argument values of a resume task.
		std::vector<ContinuationValue> args;
		//! The `ContinuationValueKind` of each entry of `args`; empty for the narrow
		//! form.
		std::vector<std::uint8_t> argKinds;
		//! The borrowed exception object of a throw task.
		void* exception = nullptr;
		//! The `VMContext*` of the instance that created the continuation.
		void* creationContext = nullptr;
		//! Number of result slots reserved for the resumed continuation.
		std::uint32_t resultCapacity = 0;
		//! Number of tag payload slots reserved for a suspension.
		std::uint32_t tagCapacity = 0;
		//! True when the task uses the wide (kind-array) ABI.
		bool wide = false;
	};
	//! The callbacks invoked for every resumed task.
	Hooks hooks_{};
	//! The queue of resume and throw tasks, oldest first.
	std::deque<Task> tasks_{};
};

//! \par Generated-code entry points (C ABI)
//! `callable` is the target function reference; `context` is the creating
//! instance's `VMContext*` (used for a native import whose `Callable::context`
//! is null, exactly as a direct call passes the caller's context). The two flags
//! describe the target's signature (`1` for an i32 parameter/result, `0` for
//! absent). The returned handle owns one reference.
extern "C" {

//! Creates a continuation over the target’s narrow i32 signature.
//! \param callable the target function reference
//! \param context the `VMContext*` of the creating instance
//! \param paramCount the number of parameters the target still expects
//! \param takesArg non-zero when the target takes an i32 parameter
//! \param returnsValue non-zero when the target returns an i32 result
//! \return a handle that owns one reference, or null when the budget is
//! exhausted
void* wasm_cont_new(void* callable, void* context, std::uint32_t paramCount,
		std::int32_t takesArg, std::int32_t returnsValue);

//! \par Wide continuation ABI
//! `wasm_cont_new_ex` is the backend-facing constructor. It copies the kind
//! arrays and stores the backend invoker; the target's params/results may be
//! any supported scalar, reference or v128.
//! \param callable the target function reference
//! \param context the `VMContext*` of the creating instance
//! \param invoke the backend invoker that calls the target
//! \param paramCount the number of parameters the target still expects
//! \param resultCount the number of results the target produces
//! \param paramKinds the `ContinuationValueKind` of each parameter
//! \param resultKinds the `ContinuationValueKind` of each result
//! \return a handle that owns one reference, or null when the budget is
//! exhausted
void* wasm_cont_new_ex(void* callable, void* context, ContinuationInvokeFn invoke,
		std::uint32_t paramCount, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds);

//! Binds a prefix of a continuation’s remaining arguments (`cont.bind`).
//! \param continuation the continuation whose arguments are bound
//! \param values the wide argument values that are bound
//! \param count the number of argument slots in `values`
//! \return the new continuation, which owns one reference, or null when the
//! continuation is empty or the budget is exhausted
void* wasm_cont_bind_ex(void* continuation, const ContinuationValue* values,
		std::uint32_t count);

//! Resume with a wide argument buffer. `results`/`tagValues` are caller-owned
//! storage of `capacity` slots (16 bytes each); the runtime moves values in and
//! records the counts in `result`. The old `wasm_cont_resume` wrappers remain
//! for the scalar i32 ABI.
//! \param continuation the continuation that is resumed
//! \param args the wide argument values, `argCount` of them
//! \param argCount the number of argument slots in `args`
//! \param results caller-owned storage the result values are moved into
//! \param resultCapacity the number of slots in `results`
//! \param tagValues caller-owned storage the tag payload is moved into
//! \param tagCapacity the number of slots in `tagValues`
//! \param result receives the status and the counts of the resume
void wasm_cont_resume_ex(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationValue* tagValues,
		std::uint32_t tagCapacity, ContinuationResumeResult* result);
//! Resumes a continuation by throwing `exception` into it.
//! \param continuation the continuation that is resumed
//! \param exception the borrowed exception object that is thrown
//! \param results caller-owned storage the result values are moved into
//! \param resultCapacity the number of slots in `results`
//! \param tagValues caller-owned storage the tag payload is moved into
//! \param tagCapacity the number of slots in `tagValues`
//! \param result receives the status and the counts of the resume
void wasm_cont_resume_throw_ex(void* continuation, void* exception,
		ContinuationValue* results, std::uint32_t resultCapacity,
		ContinuationValue* tagValues, std::uint32_t tagCapacity,
		ContinuationResumeResult* result);

//! Symmetric switch: capture the calling continuation, transfer to `continuation`
//! with the caller's continuation appended to the target arguments, and return
//! when the target switches back. `results` receives the caller's continuation
//! parameters. Both providers implement the peer handoff; cross-provider
//! handoff is rejected explicitly.
//! \param continuation the continuation that is switched to
//! \param args the wide argument values passed to it
//! \param argCount the number of argument slots in `args`
//! \param results caller-owned storage the caller’s own continuation
//! parameters are moved into when the target switches back
//! \param resultCapacity the number of slots in `results`
//! \param result receives the status and the counts of the handoff
void wasm_cont_switch(void* continuation, const ContinuationValue* args,
		std::uint32_t argCount, ContinuationValue* results,
		std::uint32_t resultCapacity, ContinuationResumeResult* result);

//! Suspend with a wide payload/result buffer. `argKinds` describes the tag
//! parameters; `resultKinds` describes the values the future resume supplies.
//! \param tagId the tag the suspension carries
//! \param args the wide tag payload values
//! \param argCount the number of payload slots in `args`
//! \param argKinds the `ContinuationValueKind` of each payload slot
//! \param results caller-owned storage the resume values are moved into
//! \param resultCapacity the number of slots in `results`
//! \param resultCount receives the number of resume values written
//! \param resultKinds the `ContinuationValueKind` of each resume value the
//! future resume supplies
//! \return 1 when the continuation was resumed, 0 when there is no current
//! continuation or it was dropped while suspended
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
//! \param ref the reference slot that must stay alive while the stack is
//! suspended
void wasm_cont_add_root(void* ref);

//! Drops reference values in a caller-owned wide buffer (for an unhandled
//! suspension). No-op for scalar/v128 slots.
//! \param context the `VMContext*` the values belong to
//! \param kinds the `ContinuationValueKind` of each slot
//! \param count the number of slots in `values`
//! \param values the wide buffer whose references are released
void wasm_cont_drop_values(void* context, const std::uint8_t* kinds,
		std::uint32_t count, void* values);


//! `cont.bind`: consume the continuation identity and bind a prefix of its
//! remaining arguments. The caller still owns/releases the old handle ref.
//! \param continuation the continuation whose scalar arguments are bound
//! \param values the scalar i32 values that are bound
//! \param count the number of values in `values`
//! \return the new continuation, which owns one reference, or null when the
//! continuation is empty or the budget is exhausted
void* wasm_cont_bind(void* continuation, const std::int32_t* values,
		std::uint32_t count);

//! Resumes `continuation` with `arg`. Blocks until it completes, suspends,
//! traps or raises an uncaught exception. Trap/exception statuses have already
//! been transferred to the *resumer's* execution state, so the generated code's
//! usual pending checks handle them.
//! \param continuation the continuation that is resumed
//! \param arg the scalar i32 argument of the resume
//! \param result receives the status and the values of the resume
void wasm_cont_resume(void* continuation, std::int32_t arg,
		ContinuationResumeResult* result);
//! Resumes a continuation by throwing `exception` into it (narrow ABI).
//! \param continuation the continuation that is resumed
//! \param exception the borrowed exception object that is thrown
//! \param result receives the status and the values of the resume
void wasm_cont_resume_throw(void* continuation, void* exception,
		ContinuationResumeResult* result);

//! Suspends the current continuation on `tagId`, carrying `value`. Blocks until
//! the continuation is resumed; the resume argument is written through
//! `result` when non-null. A dropped suspended continuation reports an internal
//! trap here so the worker can unwind by returning.
//! \param tagId the tag the suspension carries
//! \param value the scalar i32 payload of the suspension
//! \param result receives the scalar resume argument when it is not null
//! \return 1 when the continuation was resumed, 0 when there is no current
//! continuation or it was dropped while suspended
std::int32_t wasm_cont_suspend(std::int32_t tagId, std::int32_t value,
		std::int32_t* result);

} // extern "C"

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMCONTINUATION_HPP
