#ifndef WASMCOMPONENTASYNC_HPP
#define WASMCOMPONENTASYNC_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentAsync.hpp
  \brief Component Model async: futures, streams, tasks and the async built-ins.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
WASMJIT_VISIBLE_BEGIN
#include "WasmMemory.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace WASM {

//! \brief Component Model concurrency, single-agent flavour
//!
//! docs/proposals/THREADS.md TD9 defers true multi-agent execution indefinitely.
//! This header is the pre-emptive interface for the Component Model's async
//! surface (docs/proposals/COMPONENT_MODEL.md CM-5/CM-8): the shape a
//! multi-agent implementation will fill in, with a working single-agent backend
//! that executes every operation synchronously.
//!
//! The interface is deliberately backend-agnostic (docs/LAYERING.md): it names
//! no code generator, only the contracts a runtime must honour:
//!
//!   * a Task owns the context slots and backpressure counter of one activation;
//!   * a Waitable is something that can produce an event and join a set;
//!   * a stream/future is a pair of endpoints over one shared element queue;
//!   * a copy either completes synchronously, blocks (`kBlocked`) or cancels;
//!   * an event is `(code, payload1, payload2)` as defined by the Canonical ABI.
//!
//! A future multi-agent backend replaces ComponentAsyncState's synchronous
//! operations with real scheduling. The public contracts stay the same.

//! The event codes the Canonical ABI passes in an event record's low word.
enum class ComponentEventCode : uint32_t {
	None = 0, //!< No event pending
	Subtask = 1, //!< A subtask produced a result or was cancelled
	StreamRead = 2, //!< A readable stream end has data or reached its end
	StreamWrite = 3, //!< A writable stream end can accept more elements
	FutureRead = 4, //!< A readable future end holds a value
	FutureWrite = 5, //!< A writable future end can accept its one value
	TaskCancelled = 6 //!< The current task's cancellation was acknowledged
};

//! The copy outcome the Canonical ABI encodes in the low four bits of a
//! `{stream,future}.{read,write}` result word.
enum class ComponentCopyResult : uint32_t {
	Completed = 0, //!< The copy completed; the high bits hold the element count
	Dropped = 1, //!< The peer end was dropped; no elements were transferred
	Cancelled = 2 //!< A pending copy was cancelled
};

//! The Canonical ABI sentinel for "the operation blocked".
static constexpr uint32_t kBlocked = 0xFFFFFFFFu;

//! Packs the optional copy result of the async built-ins into one address-sized
//! value: all ones is `none`; otherwise the result is in the low four bits and
//! the element count/progress in the remaining high bits. The built-ins are
//! `{stream,future}.{read,write}`.
//! \param [in] result The copy outcome to encode; only its low four bits are
//! used, so `kBlocked` can never be produced by this function.
//! \param [in] progress The number of elements copied, or 0/1 for a future;
//! shifted left by four and must fit the remaining 28 bits.
//! \return The packed word a built-in returns; `kBlocked` is written directly
//! by the callers instead, never through this helper.
constexpr uint32_t packCopyResult(ComponentCopyResult result, uint32_t progress)
{
	return (static_cast<uint32_t>(result) & 0xFu) | (progress << 4);
}

//! Splits a packed copy-result word back into its outcome and progress.
//! \param [in] packed The word a built-in returned, or `kBlocked`.
//! \param [out] result Receives the copy outcome; untouched when `packed` is
//! `kBlocked`.
//! \param [out] progress Receives the element count (0/1 for a future);
//! untouched when `packed` is `kBlocked`.
//! \return False when `packed` is `kBlocked` (the copy is still pending), true
//! when both outputs were written.
constexpr bool unpackCopyResult(uint32_t packed, ComponentCopyResult& result,
		uint32_t& progress)
{
	if (packed == kBlocked)
		return false;
	result = static_cast<ComponentCopyResult>(packed & 0xFu);
	progress = packed >> 4;
	return true;
}

//! Lifecycle state of one task (`Task.State` in the Canonical ABI).
enum class ComponentTaskState : uint8_t {
	Pending, //!< Created, not yet run
	Running, //!< Currently executing
	Cancelled, //!< Cancellation was acknowledged
	Resolved //!< Ran to completion
};

//! The direction of one end of a stream/future.
enum class ComponentEndDirection : uint8_t {
	Readable, //!< The end that reads (receives) elements
	Writable //!< The end that writes (supplies) elements
};

//! `End.State` in the Canonical ABI explainer.
enum class ComponentEndState : uint8_t {
	Idle, //!< No copy in flight and no value pending for this end
	Copying, //!< A copy is in flight and holds a guest buffer
	CancellingCopy, //!< A copy is being cancelled; the end is not yet idle
	Done //!< The end reached its end of stream/future (or its single value)
};

//! What a handle denotes. Resources are owned by the runtime's resource table;
//! this enum only covers the async objects a handle table can also contain.
enum class ComponentWaitableKind : uint8_t {
	Subtask, //!< A subtask (a call that may resolve later)
	StreamRead, //!< A readable stream end
	StreamWrite, //!< A writable stream end
	FutureRead, //!< A readable future end
	FutureWrite, //!< A writable future end
	TaskCancelled //!< The task's cancellation notification
};

//! One event as the Canonical ABI passes it: a code and two payload words whose
//! meaning depends on the code.
struct ComponentAsyncEvent {
	ComponentEventCode code = ComponentEventCode::None; //!< Event kind
	uint32_t payload1 = 0; //!< Handle or task id the event refers to
	//! Second payload: the packed copy result for a read/write event, 0
	//! otherwise.
	uint32_t payload2 = 0;

	//! Tests for "no event".
	//! \return True when `code` is None, i.e. this is a fresh event record that
	//! nothing has filled in yet.
	bool isNone() const { return code == ComponentEventCode::None; }
};

//! One context slot value. The Canonical ABI only asks for a value of the
//! core type named by `canon context.get/set`, so a 64-bit cell is enough.
union ComponentContextSlot {
	int64_t i64 = 0; //!< `i64` context value
	uint64_t u64; //!< `u64` context value
	double f64; //!< `f64` context value
	int32_t i32; //!< `i32` context value
	uint32_t u32; //!< `u32` context value
	float f32; //!< `f32` context value
};

//! One activation's state. The single-agent scheduler keeps exactly one of
//! these current; a future multi-agent backend keeps one per agent.
class ComponentTask {
public:
	//! Creates the task record for one activation.
	//! \param [in] id Host-assigned task id, as the `task.*` built-ins report
	//! it; the class only stores it.
	explicit ComponentTask(uint32_t id) : id_(id) {}

	//! The task id.
	//! \return The id passed to the constructor.
	uint32_t id() const { return id_; }
	//! The task's lifecycle state.
	//! \return `Pending` until resolve()/cancel() is called.
	ComponentTaskState state() const { return state_; }
	//! Sets the lifecycle state.
	//! \param [in] state The new state; no transition is validated here.
	void setState(ComponentTaskState state) { state_ = state; }

	//! The outstanding backpressure count.
	//! \return How many `backpressure.inc` calls have no matching `dec` yet.
	uint32_t backpressure() const { return backpressure_; }
	//! Records one `backpressure.inc`.
	void incrementBackpressure() { ++backpressure_; }
	//! Records one `backpressure.dec`.
	//! \throws std::runtime_error when the counter is already 0, because the
	//! component would then have decremented without incrementing.
	void decrementBackpressure()
	{
		if (backpressure_ == 0)
			throw std::runtime_error(
					"component async: backpressure.dec without a matching inc");
		--backpressure_;
	}

	//! Writes one context slot.
	//! \param [in] index The slot index, as `canon context.set` names it; it is
	//! not a typed index space entry.
	//! \param [in] value The value to store, copied bit-for-bit.
	void setContextSlot(uint32_t index, ComponentContextSlot value)
	{
		context_[index] = value;
	}
	//! Reads one context slot.
	//! \param [in] index The slot index.
	//! \return The stored value, or a zeroed slot when `index` was never
	//! written.
	ComponentContextSlot contextSlot(uint32_t index) const
	{
		const auto it = context_.find(index);
		return it == context_.end() ? ComponentContextSlot{} : it->second;
	}

	//! Marks that the task has been asked to cancel; `task.cancel` may now run.
	void requestCancellation() { cancellationRequested_ = true; }
	//! Whether a cancellation was requested.
	//! \return True after requestCancellation() and before the task stops.
	bool cancellationRequested() const { return cancellationRequested_; }

	//! Marks the task resolved (`task.return`).
	void resolve() { state_ = ComponentTaskState::Resolved; }
	//! Marks the task cancelled (`task.cancel`).
	void cancel() { state_ = ComponentTaskState::Cancelled; }
private:
	uint32_t id_ = 0; //!< Host-assigned task id
	ComponentTaskState state_ = ComponentTaskState::Pending; //!< Lifecycle state
	uint32_t backpressure_ = 0; //!< Outstanding backpressure count
	bool cancellationRequested_ = false; //!< A cancellation was requested
	//! Context slots by `context.get/set` index.
	std::unordered_map<uint32_t, ComponentContextSlot> context_;
};

//! A waitable is anything that can produce a ComponentAsyncEvent and join one
//! waitable set. Stream/future ends and subtasks are the concrete producers.
class ComponentWaitable {
public:
	//! Creates a waitable of one kind.
	//! \param [in] kind What the handle denotes; it decides the event code the
	//! waitable reports when it fires.
	explicit ComponentWaitable(ComponentWaitableKind kind) : kind_(kind) {}
	//! Virtual destructor: a waitable may be joined to a set, which only holds
	//! non-owning pointers.
	virtual ~ComponentWaitable() = default;

	//! What this waitable denotes.
	//! \return The kind passed to the constructor.
	ComponentWaitableKind kind() const { return kind_; }
	//! Whether the waitable currently belongs to a set.
	//! \return True when a set holds it, false when it is unjoined.
	bool inWaitableSet() const { return set_ != nullptr; }
	//! The set the waitable was joined to, as an untyped pointer.
	//! \return The owning ComponentWaitableSet, or nullptr when unjoined.
	void* waitableSet() const { return set_; }
	//! Records the set the waitable was joined to or removed from.
	//! \param [in] set The set, or nullptr when the waitable leaves its set; the
	//! pointer is stored untyped and not owned.
	void setWaitableSet(void* set) { set_ = set; }

	//! Returns true and fills `event` when progress is pending.
	//! \param [out] event Receives the pending event; untouched when there is
	//! nothing to report.
	//! \return True when an event was written, false when the waitable has no
	//! pending progress (the caller then leaves `event` alone).
	virtual bool takeEvent(ComponentAsyncEvent& event) = 0;
private:
	ComponentWaitableKind kind_; //!< What this waitable denotes
	void* set_ = nullptr; //!< Joined set, or nullptr when unjoined
};

//! A single-agent waitable set. `wait` and `poll` both return `none` when no
//! event is pending: there is no second agent that could make one appear, and
//! the documented single-agent deviation is that a caller must not spin on an
//! empty set (docs/proposals/THREADS.md TD9).
class ComponentWaitableSet {
public:
	//! Polls every joined waitable once, in join order.
	//! \return The first event a waitable produced, taken out of that waitable
	//! so it is not reported twice; an event whose isNone() is true when the
	//! set is empty or nothing is pending.
	ComponentAsyncEvent poll()
	{
		for (ComponentWaitable* waitable : waitables_) {
			ComponentAsyncEvent event;
			if (waitable->takeEvent(event))
				return event;
		}
		return ComponentAsyncEvent{};
	}

	//! Waits for an event. In this single-agent runtime it is exactly poll():
	//! no other agent can produce an event while the caller waits.
	//! \return The first pending event, or a `none` event when nothing is
	//! pending.
	ComponentAsyncEvent wait() { return poll(); }

	//! Joins one waitable to this set, moving it out of any other set.
	//! \param [in] waitable The waitable to join; null and an already-joined
	//! waitable are ignored, and the set does not take ownership.
	void add(ComponentWaitable* waitable)
	{
		if (waitable == nullptr || waitable->waitableSet() == this)
			return;
		remove(waitable);
		waitables_.push_back(waitable);
		waitable->setWaitableSet(this);
	}

	//! Removes one waitable from this set.
	//! \param [in] waitable The waitable to remove; null and a waitable that is
	//! not in this set are ignored.
	void remove(ComponentWaitable* waitable)
	{
		if (waitable == nullptr)
			return;
		for (auto it = waitables_.begin(); it != waitables_.end(); ++it) {
			if (*it == waitable) {
				waitables_.erase(it);
				waitable->setWaitableSet(nullptr);
				return;
			}
		}
	}

	//! Detaches every waitable from this set, leaving each unjoined.
	void removeAll()
	{
		for (ComponentWaitable* waitable : waitables_)
			waitable->setWaitableSet(nullptr);
		waitables_.clear();
	}

	//! Whether the set holds nothing.
	//! \return True when no waitable is joined.
	bool empty() const { return waitables_.empty(); }
	//! How many waitables are joined.
	//! \return The number of waitables in the set.
	std::size_t size() const { return waitables_.size(); }
	//! The joined waitables, in join order.
	//! \return A borrow of the set's own vector; the pointers are not owned and
	//! are invalidated by the next add()/remove().
	const std::vector<ComponentWaitable*>& waitables() const { return waitables_; }
	//! Private state: the joined waitables, in join order.
private:
	//! The joined waitables, in join order; the pointers are borrowed.
	std::vector<ComponentWaitable*> waitables_;
};


//! One end of a stream or future. The two ends share a ComponentAsyncChannel.
class ComponentAsyncChannel;

//! One end of a stream or future: a waitable that names its channel, its
//! direction and the state of its copy. Events are produced here and taken by
//! the owning waitable set.
class ComponentAsyncEndpoint : public ComponentWaitable {
public:
	//! Creates one end of a stream/future pair.
	//! \param [in] kind Whether this is the read or the write end of a stream
	//! or a future; it decides the event code this end reports.
	//! \param [in] handle Handle allocated for this end; it is what the
	//! built-ins and the peer use to name it.
	//! \param [in] direction Readable or writable end.
	//! \param [in] channel The channel shared with the peer end; not owned, and
	//! it must outlive this endpoint.
	ComponentAsyncEndpoint(ComponentWaitableKind kind, uint32_t handle,
			ComponentEndDirection direction, ComponentAsyncChannel* channel)
		: ComponentWaitable(kind), handle_(handle), direction_(direction),
		  channel_(channel) {}

	//! The handle allocated for this end.
	//! \return The handle the guest uses to name this end.
	uint32_t handle() const { return handle_; }
	//! Which end this is.
	//! \return `Readable` for the receiving end, `Writable` for the sending one.
	ComponentEndDirection direction() const { return direction_; }
	//! The state of this end's copy.
	//! \return `Idle` when no copy is in flight and nothing is pending.
	ComponentEndState state() const { return state_; }
	//! Sets the state of this end's copy.
	//! \param [in] state The new state; callers move between Idle, Copying and
	//! Done, and set CancellingCopy while a cancel is in progress.
	void setState(ComponentEndState state) { state_ = state; }

	//! The channel this end is attached to.
	//! \return The shared queue holder; `forward` repoints it, so it is not
	//! necessarily the channel the end was created with.
	ComponentAsyncChannel* channel() const { return channel_; }
	//! Repoints this end at another channel (used by `stream.forward`).
	//! \param [in] channel The channel to attach to; not owned.
	void setChannel(ComponentAsyncChannel* channel) { channel_ = channel; }

	//! Takes the pending event of this end, if any.
	//! \param [out] event Receives the pending event; set to a `none` event when
	//! there was none.
	//! \return True when an event was pending and has been handed over, false
	//! when this end had nothing to report.
	bool takeEvent(ComponentAsyncEvent& event) override
	{
		if (!hasPendingEvent_)
			return false;
		hasPendingEvent_ = false;
		event = pendingEvent_;
		pendingEvent_ = ComponentAsyncEvent{};
		return true;
	}

	//! Whether an event is waiting to be taken.
	//! \return True between setPending() and the next takeEvent().
	bool hasPendingEvent() const { return hasPendingEvent_; }
	//! Stores one event until the owning set takes it.
	//! \param [in] event The event to publish; only one is kept, so a second
	//! call before the first is taken replaces it.
	void setPending(ComponentAsyncEvent event)
	{
		pendingEvent_ = event;
		hasPendingEvent_ = true;
	}

	//! A blocked async copy keeps the guest buffer here so the other end can
	//! complete it when data arrives. The pointer is into linear memory, which
	//! does not move while the memory is not grown.
	//! \param [in] memory The linear memory the buffer lives in; borrowed, and
	//! it must not be grown until the copy completes.
	//! \param [in] address Byte offset of the guest buffer inside `memory`.
	//! \param [in] count Number of elements to move; for a future this is 1.
	//! \param [in] isRead True when this end is the reader (the copy writes into
	//! the buffer), false when it is the writer (the copy reads it).
	void setPendingBuffer(LinearMemory* memory, uint64_t address, uint64_t count,
			bool isRead)
	{
		pendingMemory_ = memory;
		pendingAddress_ = address;
		pendingCount_ = count;
		pendingIsRead_ = isRead;
	}
	//! Forgets the blocked buffer, leaving the end with no copy to complete.
	void clearPendingBuffer()
	{
		pendingMemory_ = nullptr;
		pendingAddress_ = 0;
		pendingCount_ = 0;
		pendingIsRead_ = false;
	}
	//! Whether a blocked copy is holding a guest buffer.
	//! \return True while a buffer is recorded, i.e. until the copy completes
	//! or is cancelled.
	bool hasPendingBuffer() const { return pendingMemory_ != nullptr; }
	//! The memory of the blocked copy.
	//! \return The borrowed memory, or nullptr when no copy is blocked.
	LinearMemory* pendingMemory() const { return pendingMemory_; }
	//! The buffer offset of the blocked copy.
	//! \return The byte address, or 0 when no copy is blocked.
	uint64_t pendingAddress() const { return pendingAddress_; }
	//! The element count of the blocked copy.
	//! \return How many elements the blocked copy may still move; 0 when none.
	uint64_t pendingCount() const { return pendingCount_; }
	//! Whether the blocked copy is a read.
	//! \return True when this end is the reader and the copy writes into guest
	//! memory, false for a blocked write.
	bool pendingIsRead() const { return pendingIsRead_; }
	//! Private state: identity, copy state and the blocked-copy buffer.
private:
	uint32_t handle_ = 0; //!< Handle allocated for this end
	ComponentEndDirection direction_; //!< Readable or writable end
	ComponentAsyncChannel* channel_ = nullptr; //!< Shared channel, not owned
	ComponentEndState state_ = ComponentEndState::Idle; //!< Copy state
	bool hasPendingEvent_ = false; //!< An event waits to be taken
	ComponentAsyncEvent pendingEvent_{}; //!< The waiting event
	LinearMemory* pendingMemory_ = nullptr; //!< Memory of a blocked copy
	uint64_t pendingAddress_ = 0; //!< Buffer address of a blocked copy
	uint64_t pendingCount_ = 0; //!< Elements a blocked copy may still move
	bool pendingIsRead_ = false; //!< True when the blocked copy is a read
};

//! The shared queue behind a readable/writable pair. `elementSize == 0` models
//! the element-less `stream`/`future` forms, for which the buffer is ignored.
class ComponentAsyncChannel {
public:
	//! Creates the shared channel of one stream/future pair.
	//! \param [in] elementSize Size of one element in bytes; 0 models the
	//! element-less `stream`/`future` forms, whose buffers are ignored.
	//! \param [in] isFuture True for a future (at most one element, then
	//! `Done`), false for a stream.
	explicit ComponentAsyncChannel(uint32_t elementSize, bool isFuture)
		: elementSize_(elementSize), isFuture_(isFuture) {}

	//! The element size of the channel.
	//! \return Bytes per element, or 0 for the element-less forms.
	uint32_t elementSize() const { return elementSize_; }
	//! Whether the channel carries a future rather than a stream.
	//! \return True for a future channel, false for a stream.
	bool isFuture() const { return isFuture_; }

	//! Whether the readable end is still open.
	//! \return False once the reader dropped its end; writers then see
	//! `Dropped`.
	bool readableAlive() const { return readableAlive_; }
	//! Whether the writable end is still open.
	//! \return False once the writer dropped its end; readers then see
	//! `Dropped`.
	bool writableAlive() const { return writableAlive_; }
	//! Marks the readable end open or closed.
	//! \param [in] alive False when the readable end was dropped.
	void setReadableAlive(bool alive) { readableAlive_ = alive; }
	//! Marks the writable end open or closed.
	//! \param [in] alive False when the writable end was dropped.
	void setWritableAlive(bool alive) { writableAlive_ = alive; }

	//! Raw byte queue. A stream transfers whole elements; a future transfers at
	//! most one value and then stays `Done`.
	//! \return The queue of encoded elements, oldest first; mutable so the
	//! runtime can complete a blocked copy in place.
	std::deque<std::vector<uint8_t>>& queue() { return queue_; }
	//! Raw byte queue, read-only view.
	//! \return The queue of encoded elements, oldest first.
	const std::deque<std::vector<uint8_t>>& queue() const { return queue_; }

	//! Whether a future's single value has already been written.
	//! \return True once `future.write` stored its value; a second write is then
	//! the write-twice case the built-in rejects.
	bool futureWritten() const { return futureWritten_; }
	//! Records that a future's value was written.
	//! \param [in] written True after the single `future.write` succeeded.
	void setFutureWritten(bool written) { futureWritten_ = written; }
	//! Private state: element size, open ends and the element queue.
private:
	uint32_t elementSize_ = 0; //!< Bytes per element, 0 when element-less
	bool isFuture_ = false; //!< True for a future channel
	bool readableAlive_ = true; //!< The readable end was not dropped
	bool writableAlive_ = true; //!< The writable end was not dropped
	bool futureWritten_ = false; //!< A future's value was written
	std::deque<std::vector<uint8_t>> queue_; //!< Encoded elements, oldest first
};


//! \brief The single-agent execution interface
//!
//! This is the seam a future multi-agent backend implements. The default
//! implementation is intentionally synchronous, single-threaded and reentrant:
//! `runUntilIdle` drains the queue on the calling thread. There is no OS thread
//! and no global state beyond the ComponentAsyncState instance.
class ComponentExecutor {
public:
	//! Virtual destructor: the runtime owns its executor through this
	//! interface.
	virtual ~ComponentExecutor() = default;

	//! The answers of the `canon` thread built-ins. A single-agent executor
	//! always reports 0 and 1; a multi-agent executor may report per-agent
	//! values.
	//! \return The index of the agent this call runs on; 0 in the single-agent
	//! runtime.
	virtual uint32_t threadIndex() const = 0;
	//! The answer of `thread.available-parallelism`.
	//! \return How many agents may run concurrently; 1 in the single-agent
	//! runtime.
	virtual uint32_t availableParallelism() const = 0;
	//! Whether this executor runs exactly one agent.
	//! \return True for the single-agent runtime, false for a scheduler that can
	//! interleave agents.
	virtual bool singleAgent() const = 0;

	//! Schedule `work` to run on the cooperative queue. It must not run before
	//! `runUntilIdle()` is called.
	//! \param [in] work The closure to run later; an empty closure is dropped
	//! rather than queued.
	virtual void push(std::function<void()> work) = 0;
	//! Drains the queue on the calling thread, running work that enqueues more
	//! work as well; returns when the queue is empty.
	virtual void runUntilIdle() = 0;
	//! How much work is queued.
	//! \return The number of closures pushed and not yet run.
	virtual std::size_t pending() const = 0;
};

//! The synchronous executor the single-agent runtime uses: work runs on the
//! calling thread, in push order, and there is no OS thread.
class SingleAgentExecutor final : public ComponentExecutor {
public:
	//! \return Always 0: there is one agent.
	uint32_t threadIndex() const override { return 0; }
	//! \return Always 1: work is serialised on the calling thread.
	uint32_t availableParallelism() const override { return 1; }
	//! \return Always true, which is what makes `wait`/`poll` non-blocking.
	bool singleAgent() const override { return true; }

	//! Queues one closure.
	//! \param [in] work The closure to run at the next runUntilIdle(); an empty
	//! closure is ignored.
	void push(std::function<void()> work) override
	{
		if (work)
			queue_.push_back(std::move(work));
	}

	//! Runs queued closures until the queue is empty, including closures that
	//! were queued while draining.
	void runUntilIdle() override
	{
		//! Take a snapshot so work that enqueues more work is still drained.
		while (!queue_.empty()) {
			std::function<void()> work = std::move(queue_.front());
			queue_.pop_front();
			work();
		}
	}

	//! \return The number of closures waiting to run.
	std::size_t pending() const override { return queue_.size(); }
	//! Private state: the pending closures, in push order.
private:
	std::deque<std::function<void()>> queue_;
};


//! \brief Stream/future and waitable bookkeeping
//!
//! ComponentAsyncState owns the handles for async objects. The host runtime
//! supplies the allocator/releaser so async handles share the component's one
//! handle index space with resources.
class ComponentAsyncState {
public:
	//! Allocates one async handle; supplied by the host runtime so async
	//! objects share the component's handle index space with resources.
	using HandleAllocator = std::function<uint32_t()>;
	//! Releases one async handle to the same shared handle index space.
	using HandleReleaser = std::function<void(uint32_t)>;

	//! Creates the async state of one component instance.
	//! \param [in] allocate Called for every handle this state hands out; it
	//! must never return 0, the invalid handle.
	//! \param [in] release Called when a handle is dropped, so the shared
	//! handle space can reuse the slot.
	ComponentAsyncState(HandleAllocator allocate, HandleReleaser release)
		: allocate_(std::move(allocate)), release_(std::move(release)) {}

	//! A stream/future with an optional element size. Returns the readable and
	//! writable handle (the low/high i32 of `{stream,future}.new`).
	//! \param [in] future True to create a future (one value, then `Done`),
	//! false to create a stream.
	//! \param [in] elementSize Size of one element in bytes; 0 for the
	//! element-less `stream`/`future` forms.
	//! \return The readable handle followed by the writable handle, both
	//! allocated through the handle allocator and usable until they are dropped.
	std::pair<uint32_t, uint32_t> newStreamFuture(bool future, uint32_t elementSize)
	{
		const uint32_t readable = allocate_();
		const uint32_t writable = allocate_();
		auto channel = std::make_unique<ComponentAsyncChannel>(elementSize, future);
		ComponentAsyncChannel* raw = channel.get();
		channelStorage_.push_back(std::move(channel));
		channels_[readable] = raw;
		channels_[writable] = raw;

		auto readEnd = std::make_unique<ComponentAsyncEndpoint>(
				future ? ComponentWaitableKind::FutureRead
						: ComponentWaitableKind::StreamRead,
				readable, ComponentEndDirection::Readable, raw);
		auto writeEnd = std::make_unique<ComponentAsyncEndpoint>(
				future ? ComponentWaitableKind::FutureWrite
						: ComponentWaitableKind::StreamWrite,
				writable, ComponentEndDirection::Writable, raw);
		endpoints_.emplace(readable, std::move(readEnd));
		endpoints_.emplace(writable, std::move(writeEnd));
		readableOf_[writable] = readable;
		writableOf_[readable] = writable;
		//! The pair, in the order `{stream,future}.new` returns it.
		return {readable, writable};
	}

	//! `{stream,future}.read`: copy up to `count` elements from the channel into
	//! `memory[address]`. Returns `kBlocked` when `async` and no data is ready.
	//! \param [in] handle The readable end's handle; a writable handle or an
	//! unknown one is rejected.
	//! \param [in] memory The linear memory the guest buffer lives in; it is
	//! only read from for a blocked copy, and must not be grown until that copy
	//! completes.
	//! \param [in] address Byte offset of the guest buffer in `memory`.
	//! \param [in] count Number of elements to move; for a future this is
	//! ignored and at most one value is read.
	//! \param [in] async True to block instead of failing when no element is
	//! ready, false to require immediate progress.
	//! \return The packed result word: `Completed` with the element count,
	//! `Dropped` when the writer already dropped its end, or `kBlocked` when the
	//! copy parked the guest buffer for the writer to complete.
	//! \throws std::runtime_error when the handle is not a readable end, when
	//! the end is not idle, when the buffer is out of bounds, or when a
	//! synchronous read would have to block.
	uint32_t read(uint32_t handle, LinearMemory& memory, uint64_t address,
			uint64_t count, bool async)
	{
		ComponentAsyncEndpoint& end = endpoint(handle,
				ComponentEndDirection::Readable);
		ComponentAsyncChannel& channel = *end.channel();
		requireIdle(end);
		checkBuffer(memory, address, count * channel.elementSize());
		if (count == 0 && !channel.isFuture())
			return packCopyResult(ComponentCopyResult::Completed, 0);

		//! A writer may have already blocked on this read; feed its data in now.
		if (ComponentAsyncEndpoint* peer = peerOf(handle);
				peer != nullptr && peer->hasPendingBuffer() && !peer->pendingIsRead())
			completePendingWrite(*peer);

		if (!channel.writableAlive()) {
			end.setState(ComponentEndState::Done);
			return packCopyResult(ComponentCopyResult::Dropped, 0);
		}
		if (channel.isFuture()) {
			if (channel.queue().empty()) {
				if (async) {
					end.setState(ComponentEndState::Copying);
					end.setPendingBuffer(&memory, address, 1, true);
					return kBlocked;
				}
				throw std::runtime_error(
						"component async: a synchronous future.read would block "
						"(single-agent runtime)");
			}
			const std::vector<uint8_t>& value = channel.queue().front();
			if (!value.empty())
				copyToMemory(memory, address, value.data(), value.size());
			channel.queue().pop_front();
			end.setState(ComponentEndState::Done);
			end.clearPendingBuffer();
			return packCopyResult(ComponentCopyResult::Completed, 1);
		}

		const std::size_t elementSize = channel.elementSize();
		uint64_t copied = 0;
		while (copied < count && !channel.queue().empty()) {
			const std::vector<uint8_t>& value = channel.queue().front();
			if (elementSize != 0 && !value.empty())
				copyToMemory(memory, address + copied * elementSize,
						value.data(), value.size());
			channel.queue().pop_front();
			++copied;
		}
		if (copied != 0 || count == 0) {
			end.clearPendingBuffer();
			return packCopyResult(ComponentCopyResult::Completed,
					static_cast<uint32_t>(copied));
		}
		if (async) {
			end.setState(ComponentEndState::Copying);
			end.setPendingBuffer(&memory, address, count, true);
			return kBlocked;
		}
		throw std::runtime_error(
				"component async: a synchronous stream.read would block "
				"(single-agent runtime)");
	}


	//! `{stream,future}.write`: copy `count` elements from memory into the
	//! channel. Returns `kBlocked` when `async` and there is no reader yet.
	//! \param [in] handle The writable end's handle; a readable handle or an
	//! unknown one is rejected.
	//! \param [in] memory The linear memory the guest buffer lives in; its bytes
	//! are copied out, so it may be grown once this call returns.
	//! \param [in] address Byte offset of the guest buffer in `memory`.
	//! \param [in] count Number of elements to copy; for a future this is
	//! ignored and exactly one value is written.
	//! \param [in] async True to park the guest buffer when a future's first
	//! value has not been consumed yet, false to reject that case.
	//! \return The packed result word: `Completed` with the element count,
	//! `Dropped` when the reader already dropped its end, or `kBlocked` when a
	//! second future write parked the guest buffer until the first value is
	//! consumed.
	//! \throws std::runtime_error when the handle is not a writable end, when
	//! the end is not idle, when the buffer is out of bounds, or when a
	//! synchronous future write is the second one.
	uint32_t write(uint32_t handle, LinearMemory& memory, uint64_t address,
			uint64_t count, bool async)
	{
		ComponentAsyncEndpoint& end = endpoint(handle,
				ComponentEndDirection::Writable);
		ComponentAsyncChannel& channel = *end.channel();
		requireIdle(end);
		checkBuffer(memory, address, count * channel.elementSize());

		if (!channel.readableAlive()) {
			end.setState(ComponentEndState::Done);
			return packCopyResult(ComponentCopyResult::Dropped, 0);
		}
		if (channel.isFuture()) {
			if (channel.futureWritten()) {
				if (async) {
					end.setState(ComponentEndState::Copying);
					end.setPendingBuffer(&memory, address, 1, false);
					return kBlocked;
				}
				throw std::runtime_error(
						"component async: future.write called twice");
			}
			//! The future's single value, sized by the channel's element size.
			//! \param [in] elementSize The channel's element size in bytes, used
			//! as the buffer's initial size.
			//! \return There is nothing to return: this record is a local buffer,
			//! not a function.
			std::vector<uint8_t> value(channel.elementSize());
			if (!value.empty())
				copyFromMemory(value.data(), memory, address, value.size());
			channel.queue().push_back(std::move(value));
			channel.setFutureWritten(true);
			end.setState(ComponentEndState::Done);
			//! If the reader already blocked, complete it now.
			if (ComponentAsyncEndpoint* peer = peerOf(handle);
					peer != nullptr && peer->hasPendingBuffer() && peer->pendingIsRead())
				completePendingRead(*peer);
			return packCopyResult(ComponentCopyResult::Completed, 1);
		}

		const std::size_t elementSize = channel.elementSize();
		for (uint64_t i = 0; i < count; ++i) {
			std::vector<uint8_t> value(elementSize);
			if (elementSize != 0)
				copyFromMemory(value.data(), memory, address + i * elementSize,
						elementSize);
			channel.queue().push_back(std::move(value));
		}
		if (ComponentAsyncEndpoint* peer = peerOf(handle);
				peer != nullptr && peer->hasPendingBuffer() && peer->pendingIsRead())
			completePendingRead(*peer);
		return packCopyResult(ComponentCopyResult::Completed,
				static_cast<uint32_t>(count));
	}

	//! `{stream,future}.cancel-{read,write}`. The single-agent runtime has no
	//! buffer-owning background copy, so cancellation only succeeds for an end
	//! whose copy was already blocked (state == Copying).
	//! \param [in] handle The end whose blocked copy should be cancelled; a
	//! handle the state does not know is rejected.
	//! \param [in] async The built-in's async flag. The single-agent runtime has
	//! no separate cancel-request state, so the value is ignored.
	//! \return The packed `Cancelled` result with progress 0; the guest buffer of
	//! the cancelled copy is forgotten, so nothing is transferred.
	//! \throws std::runtime_error when the handle is unknown, or when the end
	//! has no blocked copy to cancel.
	uint32_t cancel(uint32_t handle, bool /*async*/)
	{
		auto it = endpoints_.find(handle);
		if (it == endpoints_.end())
			throw std::runtime_error("component async: invalid handle");
		ComponentAsyncEndpoint& end = *it->second;
		if (end.state() != ComponentEndState::Copying)
			throw std::runtime_error(
					"component async: cancel without a pending copy");
		end.setState(ComponentEndState::Idle);
		end.clearPendingBuffer();
		return packCopyResult(ComponentCopyResult::Cancelled, 0);
	}

	//! `{stream,future}.drop-{readable,writable}`: closes one end and tells the
	//! peer it saw `Dropped`.
	//! \param [in] handle The handle of the end being dropped; an unknown handle
	//! is rejected.
	//! \param [in] direction Which end `handle` must be; dropping the other end
	//! is an error rather than a silent no-op.
	//! \throws std::runtime_error when the handle is unknown, when it is the
	//! wrong end, when a copy is still in flight, or when a future's writable end
	//! is dropped before its single value was written.
	void drop(uint32_t handle, ComponentEndDirection direction)
	{
		auto it = endpoints_.find(handle);
		if (it == endpoints_.end())
			throw std::runtime_error("component async: invalid handle");
		ComponentAsyncEndpoint& end = *it->second;
		if (end.direction() != direction)
			throw std::runtime_error(
					"component async: dropping the wrong end of a stream/future");
		if (end.state() == ComponentEndState::Copying
				|| end.state() == ComponentEndState::CancellingCopy)
			throw std::runtime_error(
					"component async: cannot drop an end during a copy");
		if (direction == ComponentEndDirection::Writable
				&& end.channel()->isFuture()
				&& !end.channel()->futureWritten()
				&& end.channel()->readableAlive())
			throw std::runtime_error(
					"component async: dropping a future writable end before a "
					"value was written");

		ComponentAsyncChannel* channel = end.channel();
		end.clearPendingBuffer();
		if (direction == ComponentEndDirection::Readable)
			channel->setReadableAlive(false);
		else
			channel->setWritableAlive(false);

		if (ComponentAsyncEndpoint* peer = peerOf(handle); peer != nullptr) {
			peer->setChannel(channel);
			peer->setPending(ComponentAsyncEvent{eventCode(peer->kind()),
					peer->handle(),
					packCopyResult(ComponentCopyResult::Dropped, 0)});
		}
		if (end.waitableSet() != nullptr)
			static_cast<ComponentWaitableSet*>(end.waitableSet())->remove(&end);
		release_(handle);
		endpoints_.erase(it);
		if (channels_.count(handle) != 0)
			channels_.erase(handle);
	}

	//! `{stream,future}.forward`: connect the readable end to the writable end
	//! and merge any queued elements. Both handles are removed by the caller.
	//! \param [in] readableHandle The handle of the readable end whose elements
	//! are forwarded.
	//! \param [in] writableHandle The handle of the writable end that receives
	//! them.
	//! \throws std::runtime_error when either handle names the wrong kind of
	//! end, when one channel is a future and the other a stream, when the element
	//! sizes differ, when an end is not idle, or when an end is joined to a
	//! waitable set.
	void forward(uint32_t readableHandle, uint32_t writableHandle)
	{
		ComponentAsyncEndpoint& readable = endpoint(readableHandle,
				ComponentEndDirection::Readable);
		ComponentAsyncEndpoint& writable = endpoint(writableHandle,
				ComponentEndDirection::Writable);
		if (readable.channel()->isFuture() != writable.channel()->isFuture())
			throw std::runtime_error(
					"component async: cannot forward a stream into a future");
		if (readable.channel()->elementSize() != writable.channel()->elementSize())
			throw std::runtime_error(
					"component async: forward element types do not match");
		requireIdle(readable);
		requireIdle(writable);
		if (readable.inWaitableSet() || writable.inWaitableSet())
			throw std::runtime_error(
					"component async: cannot forward an end in a waitable set");

		ComponentAsyncChannel* source = readable.channel();
		ComponentAsyncChannel* destination = writable.channel();
		while (!source->queue().empty()) {
			destination->queue().push_back(std::move(source->queue().front()));
			source->queue().pop_front();
		}
		if (ComponentAsyncEndpoint* sourceWriter = peerOf(readableHandle);
				sourceWriter != nullptr)
			sourceWriter->setChannel(destination);
		writable.setChannel(source);
	}

	//! Removes a handle without notifying the peer. Used by `forward`, where the
	//! endpoints are transferred rather than dropped.
	//! \param [in] handle The handle to remove; an unknown handle is ignored, so
	//! the call is idempotent.
	void discard(uint32_t handle)
	{
		const auto it = endpoints_.find(handle);
		if (it == endpoints_.end())
			return;
		if (it->second->waitableSet() != nullptr)
			static_cast<ComponentWaitableSet*>(it->second->waitableSet())
					->remove(it->second.get());
		endpoints_.erase(it);
		channels_.erase(handle);
		release_(handle);
	}



	//! \par Waitable sets
	uint32_t newWaitableSet()
	{
		const uint32_t handle = allocate_();
		waitableSets_.emplace(handle, ComponentWaitableSet{});
		return handle;
	}

	ComponentWaitableSet& waitableSet(uint32_t handle)
	{
		const auto it = waitableSets_.find(handle);
		if (it == waitableSets_.end())
			throw std::runtime_error("component async: invalid waitable-set handle");
		return it->second;
	}

	ComponentAsyncEvent wait(uint32_t setHandle)
	{
		return waitableSet(setHandle).wait();
	}

	ComponentAsyncEvent poll(uint32_t setHandle)
	{
		return waitableSet(setHandle).poll();
	}

	void dropWaitableSet(uint32_t handle)
	{
		const auto it = waitableSets_.find(handle);
		if (it == waitableSets_.end())
			throw std::runtime_error("component async: invalid waitable-set handle");
		if (!it->second.empty())
			throw std::runtime_error(
					"component async: dropping a non-empty waitable set");
		it->second.removeAll();
		waitableSets_.erase(it);
		release_(handle);
	}

	void join(uint32_t waitableHandle, uint32_t setHandle)
	{
		ComponentWaitable* waitable = waitableFor(waitableHandle);
		if (waitable == nullptr)
			throw std::runtime_error("component async: invalid waitable handle");
		if (setHandle == 0) {
			if (waitable->waitableSet() != nullptr)
				static_cast<ComponentWaitableSet*>(waitable->waitableSet())
						->remove(waitable);
			return;
		}
		waitableSet(setHandle).add(waitable);
	}

	//! \par Tasks, context and backpressure
	ComponentTask& currentTask() { return *currentTask_; }
	uint32_t taskReturn() { currentTask_->resolve(); return 0; }
	uint32_t taskCancel()
	{
		if (!currentTask_->cancellationRequested())
			throw std::runtime_error(
					"component async: task.cancel without a cancellation request");
		currentTask_->cancel();
		return 0;
	}

	void setBackpressure(bool increment)
	{
		if (increment)
			currentTask_->incrementBackpressure();
		else
			currentTask_->decrementBackpressure();
	}

	uint32_t backpressure() const { return currentTask_->backpressure(); }

	//! Error contexts. The single-agent model stores the diagnostic string; a
	//! multi-agent backend would use the same handle table.
	uint32_t newErrorContext(std::string message)
	{
		const uint32_t handle = allocate_();
		errorContexts_.emplace(handle, std::move(message));
		return handle;
	}

	const std::string& errorContext(uint32_t handle) const
	{
		const auto it = errorContexts_.find(handle);
		if (it == errorContexts_.end())
			throw std::runtime_error("component async: invalid error-context handle");
		return it->second;
	}

	void dropErrorContext(uint32_t handle)
	{
		const auto it = errorContexts_.find(handle);
		if (it == errorContexts_.end())
			throw std::runtime_error("component async: invalid error-context handle");
		errorContexts_.erase(it);
		release_(handle);
	}

	//! Subtasks: in a single-agent executor a synchronous call is already
	//! resolved when it returns. The handle exists so the built-ins validate.
	uint32_t newSubtask()
	{
		const uint32_t handle = allocate_();
		subtasks_.insert(handle);
		return handle;
	}

	uint32_t cancelSubtask(uint32_t handle)
	{
		const auto it = subtasks_.find(handle);
		if (it == subtasks_.end())
			throw std::runtime_error("component async: invalid subtask handle");
		subtasks_.erase(it);
		release_(handle);
		return static_cast<uint32_t>(ComponentTaskState::Cancelled);
	}

	void dropSubtask(uint32_t handle)
	{
		const auto it = subtasks_.find(handle);
		if (it == subtasks_.end())
			throw std::runtime_error("component async: invalid subtask handle");
		subtasks_.erase(it);
		release_(handle);
	}

	ComponentExecutor& executor() { return executor_; }

	ComponentAsyncEndpoint* endpointOrNull(uint32_t handle)
	{
		const auto it = endpoints_.find(handle);
		return it == endpoints_.end() ? nullptr : it->second.get();
	}

	ComponentAsyncEndpoint* peerOf(uint32_t handle)
	{
		auto it = writableOf_.find(handle);
		if (it != writableOf_.end())
			return endpointOrNull(it->second);
		it = readableOf_.find(handle);
		if (it != readableOf_.end())
			return endpointOrNull(it->second);
		return nullptr;
	}


private:
	static ComponentEventCode eventCode(ComponentWaitableKind kind)
	{
		switch (kind) {
		case ComponentWaitableKind::StreamRead: return ComponentEventCode::StreamRead;
		case ComponentWaitableKind::StreamWrite: return ComponentEventCode::StreamWrite;
		case ComponentWaitableKind::FutureRead: return ComponentEventCode::FutureRead;
		case ComponentWaitableKind::FutureWrite: return ComponentEventCode::FutureWrite;
		case ComponentWaitableKind::Subtask: return ComponentEventCode::Subtask;
		case ComponentWaitableKind::TaskCancelled: return ComponentEventCode::TaskCancelled;
		}
		return ComponentEventCode::None;
	}

	ComponentAsyncEndpoint& endpoint(uint32_t handle,
			ComponentEndDirection direction)
	{
		auto it = endpoints_.find(handle);
		if (it == endpoints_.end())
			throw std::runtime_error("component async: invalid stream/future handle");
		if (it->second->direction() != direction)
			throw std::runtime_error(
					"component async: wrong end of a stream/future");
		return *it->second;
	}

	ComponentWaitable* waitableFor(uint32_t handle)
	{
		if (ComponentAsyncEndpoint* end = endpointOrNull(handle); end != nullptr)
			return end;
		return nullptr;
	}

	static void requireIdle(ComponentAsyncEndpoint& end)
	{
		if (end.state() != ComponentEndState::Idle)
			throw std::runtime_error(
					"component async: stream/future end is not idle");
	}

	//! A previously blocked read: move up to its buffer's capacity from the
	//! channel into the guest memory and publish a progress event.
	static void completePendingRead(ComponentAsyncEndpoint& reader)
	{
		ComponentAsyncChannel& channel = *reader.channel();
		LinearMemory* memory = reader.pendingMemory();
		if (memory == nullptr)
			return;
		const uint64_t address = reader.pendingAddress();
		const uint64_t count = reader.pendingCount();
		const std::size_t elementSize = channel.elementSize();

		uint64_t copied = 0;
		while (copied < count && !channel.queue().empty()) {
			const std::vector<uint8_t>& value = channel.queue().front();
			if (elementSize != 0 && !value.empty())
				copyToMemory(*memory, address + copied * elementSize, value.data(),
						value.size());
			channel.queue().pop_front();
			++copied;
		}
		reader.clearPendingBuffer();
		reader.setState(channel.isFuture() ? ComponentEndState::Done
				: ComponentEndState::Idle);
		reader.setPending(ComponentAsyncEvent{
				eventCode(reader.kind()), reader.handle(),
				packCopyResult(ComponentCopyResult::Completed,
						static_cast<uint32_t>(copied))});
	}

	//! A previously blocked write (a future written twice): once the first value
	//! has been consumed, move the stored value into the channel and publish an
	//! event for the writer.
	static void completePendingWrite(ComponentAsyncEndpoint& writer)
	{
		ComponentAsyncChannel& channel = *writer.channel();
		LinearMemory* memory = writer.pendingMemory();
		if (memory == nullptr)
			return;
		const uint64_t address = writer.pendingAddress();
		const std::size_t elementSize = channel.elementSize();
		std::vector<uint8_t> value(elementSize);
		if (elementSize != 0)
			copyFromMemory(value.data(), *memory, address, elementSize);
		channel.queue().push_back(std::move(value));
		writer.clearPendingBuffer();
		writer.setState(ComponentEndState::Done);
		writer.setPending(ComponentAsyncEvent{eventCode(writer.kind()),
				writer.handle(),
				packCopyResult(ComponentCopyResult::Completed, 1)});
	}



	static void checkBuffer(LinearMemory& memory, uint64_t address, uint64_t size)
	{
		if (address > memory.memorySize || size > memory.memorySize - address)
			throw std::runtime_error(
					"component async: stream/future buffer is out of bounds");
	}

	static void copyToMemory(LinearMemory& memory, uint64_t address,
			const uint8_t* data, std::size_t size)
	{
		if (size != 0)
			std::memcpy(memory.memoryBase + address, data, size);
	}

	static void copyFromMemory(uint8_t* out, LinearMemory& memory,
			uint64_t address, std::size_t size)
	{
		if (size != 0)
			std::memcpy(out, memory.memoryBase + address, size);
	}

	HandleAllocator allocate_;
	HandleReleaser release_;
	SingleAgentExecutor executor_;
	std::unique_ptr<ComponentTask> currentTask_ = std::make_unique<ComponentTask>(0);
	std::unordered_map<uint32_t, std::unique_ptr<ComponentAsyncEndpoint>> endpoints_;
	std::vector<std::unique_ptr<ComponentAsyncChannel>> channelStorage_;
	std::unordered_map<uint32_t, ComponentAsyncChannel*> channels_;
	std::unordered_map<uint32_t, uint32_t> readableOf_; //!< writable -> readable
	std::unordered_map<uint32_t, uint32_t> writableOf_; //!< readable -> writable
	std::unordered_map<uint32_t, ComponentWaitableSet> waitableSets_;
	std::unordered_map<uint32_t, std::string> errorContexts_;
	std::unordered_set<uint32_t> subtasks_;
};


} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTASYNC_HPP
