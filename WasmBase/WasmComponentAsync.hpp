#ifndef WASMCOMPONENTASYNC_HPP
#define WASMCOMPONENTASYNC_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentAsync.hpp
  \brief Component Model async: futures, streams, tasks and the async built-ins.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

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

/** \brief Event categories emitted by Component Model async waitables. */
enum class ComponentEventCode : uint32_t {
	None = 0, //!< No event is pending.
	Subtask = 1, //!< A subtask completed.
	StreamRead = 2, //!< A stream became readable.
	StreamWrite = 3, //!< A stream became writable.
	FutureRead = 4, //!< A future became readable.
	FutureWrite = 5, //!< A future became writable.
	TaskCancelled = 6, //!< The task was cancelled.
};

/** \brief Outcome of a stream/future copy operation. */
enum class ComponentCopyResult : uint32_t {
	Completed = 0, //!< The copy finished.
	Dropped = 1, //!< An end was dropped before the copy finished.
	Cancelled = 2, //!< The copy was cancelled.
};

//! The Canonical ABI sentinel for "the operation blocked".
static constexpr uint32_t kBlocked = 0xFFFFFFFFu;

//! Packs the optional copy result of the async built-ins into one address-sized
//! value: all ones is `none`; otherwise the result is in the low four bits and
//! the element count/progress in the remaining high bits. The built-ins are
//! `{stream,future}.{read,write}`.
/** \brief Packs a copy outcome and progress into the Canonical ABI result word. \param result Copy outcome. \param progress Number of elements completed. \return Packed result word. */
constexpr uint32_t packCopyResult(ComponentCopyResult result, uint32_t progress)
{
	return (static_cast<uint32_t>(result) & 0xFu) | (progress << 4);
}

/** \brief Decodes a packed copy result. \param packed ABI result word. \param result Receives the decoded outcome. \param progress Receives the completed element count. \return false for kBlocked; true when output parameters were populated. */
constexpr bool unpackCopyResult(uint32_t packed, ComponentCopyResult& result,
		uint32_t& progress)
{
	if (packed == kBlocked)
		return false;
	result = static_cast<ComponentCopyResult>(packed & 0xFu);
	progress = packed >> 4;
	return true;
}

/** \brief Lifecycle states of one async component task. */
enum class ComponentTaskState : uint8_t {
	Pending, //!< Created but not yet running.
	Running, //!< The task body is executing.
	Cancelled, //!< Cancellation completed; the task will not resume.
	Resolved, //!< The task finished successfully.
};

//! The direction of one end of a stream/future.
/** \brief Read/write role of a stream or future endpoint. */
enum class ComponentEndDirection : uint8_t {
	Readable, //!< This end reads from the channel.
	Writable, //!< This end writes to the channel.
};

//! `End.State` in the Canonical ABI explainer.
/** \brief Copy/cancellation state of one async endpoint. */
enum class ComponentEndState : uint8_t {
	Idle, //!< No transfer is in progress.
	Copying, //!< A transfer is blocked on the guest buffer.
	CancellingCopy, //!< A blocked transfer is being cancelled.
	Done, //!< The transfer completed.
};

//! What a handle denotes. Resources are owned by the runtime's resource table;
//! this enum only covers the async objects a handle table can also contain.
/** \brief Kind of async object that can participate in a waitable set. */
enum class ComponentWaitableKind : uint8_t {
	Subtask, //!< Waits on a subtask.
	StreamRead, //!< Waits for a readable stream end.
	StreamWrite, //!< Waits for a writable stream end.
	FutureRead, //!< Waits for a readable future end.
	FutureWrite, //!< Waits for a writable future end.
	TaskCancelled, //!< Waits on a task cancellation.
};

/** \brief Event code and two Canonical ABI payload words produced by a waitable. */
struct ComponentAsyncEvent {
	//! Which event occurred (`None` = nothing pending).
	ComponentEventCode code = ComponentEventCode::None;
	//! First event-specific payload word (a handle in most events).
	uint32_t payload1 = 0;
	uint32_t payload2 = 0; //!< Second event-specific payload word.

	/** \brief Tests whether this is the no-event sentinel. \return true when code is None. */
	bool isNone() const { return code == ComponentEventCode::None; }
};

//! One context slot value. The Canonical ABI only asks for a value of the
//! core type named by `canon context.get/set`, so a 64-bit cell is enough.
/** \brief Storage for one typed Canonical ABI task context slot. */
union ComponentContextSlot {
	//! A 64-bit signed slot, the widest the ABI can name.
	int64_t i64 = 0;
	//! A 64-bit unsigned slot.
	uint64_t u64;
	//! A 64-bit float slot.
	double f64;
	//! A 32-bit signed slot.
	int32_t i32;
	//! A 32-bit unsigned slot.
	uint32_t u32;
	//! A 32-bit float slot.
	float f32;
};

//! One activation's state. The single-agent scheduler keeps exactly one of
//! these current; a future multi-agent backend keeps one per agent.
/** \brief State and context slots belonging to one component task activation. */
class ComponentTask {
public:
	/** \brief Creates a task with its runtime handle. \param id Allocated task identifier. */
	explicit ComponentTask(uint32_t id) : id_(id) {}

	/** \brief Returns the task identifier. \return Task handle. */
	uint32_t id() const { return id_; }
	/** \brief Returns the task lifecycle state. \return Current task state. */
	ComponentTaskState state() const { return state_; }
	/** \brief Sets the task lifecycle state. \param state New task state. */
	void setState(ComponentTaskState state) { state_ = state; }

	/** \brief Returns the task's current backpressure count. \return Backpressure count. */
	uint32_t backpressure() const { return backpressure_; }
	/** \brief Increments the task's backpressure count. */
	void incrementBackpressure() { ++backpressure_; }
	/** \brief Decrements backpressure; throws if no increment is outstanding. \throws std::runtime_error when the count is already zero. */
	void decrementBackpressure()
	{
		if (backpressure_ == 0)
			throw std::runtime_error(
					"component async: backpressure.dec without a matching inc");
		--backpressure_;
	}

	/** \brief Stores a typed value in a task context slot. \param index Context slot index. \param value Slot value to store. */
	void setContextSlot(uint32_t index, ComponentContextSlot value)
	{
		context_[index] = value;
	}
	/** \brief Reads a task context slot. \param index Context slot index. \return Stored value, or a zero-initialized slot if absent. */
	ComponentContextSlot contextSlot(uint32_t index) const
	{
		const auto it = context_.find(index);
		return it == context_.end() ? ComponentContextSlot{} : it->second;
	}

	/** \brief Marks cancellation as requested for this task. */
	void requestCancellation() { cancellationRequested_ = true; }
	/** \brief Tests whether cancellation was requested. \return true if cancellation is pending. */
	bool cancellationRequested() const { return cancellationRequested_; }

	/** \brief Marks this task as resolved. */
	void resolve() { state_ = ComponentTaskState::Resolved; }
	/** \brief Marks this task as cancelled. */
	void cancel() { state_ = ComponentTaskState::Cancelled; }
private:
	//! The task's runtime handle.
	uint32_t id_ = 0;
	ComponentTaskState state_ = ComponentTaskState::Pending; //!< The lifecycle state of the task.
	uint32_t backpressure_ = 0; //!< Caller-declared limit on outstanding work.
	bool cancellationRequested_ = false; //!< Set by `task.cancel`, read by the task body.
	std::unordered_map<uint32_t, ComponentContextSlot> context_; //!< Context slots this task can reach.
};

//! A waitable is anything that can produce a ComponentAsyncEvent and join one
//! waitable set. Stream/future ends and subtasks are the concrete producers.
/** \brief Base interface for objects that can emit async events and join waitable sets. */
class ComponentWaitable {
public:
	//! Creates a waitable of the given kind; joins no set yet.
	explicit ComponentWaitable(ComponentWaitableKind kind) : kind_(kind) {}
	virtual ~ComponentWaitable() = default;

	//! Which kind of producer this waitable is.
	ComponentWaitableKind kind() const { return kind_; }
	//! True while this waitable is a member of a waitable set.
	bool inWaitableSet() const { return set_ != nullptr; }
	//! The set this waitable belongs to, or null.
	void* waitableSet() const { return set_; }
	//! Records the owning set (called by `ComponentWaitableSet`).
	void setWaitableSet(void* set) { set_ = set; }

	//! Returns true and fills `event` when progress is pending.
	virtual bool takeEvent(ComponentAsyncEvent& event) = 0;
private:
	//! The waitable's kind, fixed at construction.
	ComponentWaitableKind kind_;
	void* set_ = nullptr; //!< The waitable set holding this member, or null.
};

//! A single-agent waitable set. `wait` and `poll` both return `none` when no
//! event is pending: there is no second agent that could make one appear, and
//! the documented single-agent deviation is that a caller must not spin on an
//! empty set (docs/proposals/THREADS.md TD9).
/** \brief Single-agent collection of waitables that can be polled for events. */
class ComponentWaitableSet {
public:
	//! Returns the first pending event, or `none` when the set is quiet.
	ComponentAsyncEvent poll()
	{
		for (ComponentWaitable* waitable : waitables_) {
			ComponentAsyncEvent event;
			if (waitable->takeEvent(event))
				return event;
		}
		return ComponentAsyncEvent{};
	}

	//! Single-agent `wait`: identical to `poll` (no other agent can make an event).
	ComponentAsyncEvent wait() { return poll(); }

	//! Adds `waitable` (re-parenting it from any previous set).
	void add(ComponentWaitable* waitable)
	{
		if (waitable == nullptr || waitable->waitableSet() == this)
			return;
		remove(waitable);
		waitables_.push_back(waitable);
		waitable->setWaitableSet(this);
	}

	//! Removes `waitable` from this set, clearing its owner pointer.
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

	//! Empties the set, clearing every member's owner pointer.
	void removeAll()
	{
		for (ComponentWaitable* waitable : waitables_)
			waitable->setWaitableSet(nullptr);
		waitables_.clear();
	}

	//! True when the set holds no waitable.
	bool empty() const { return waitables_.empty(); }
	//! Number of members.
	std::size_t size() const { return waitables_.size(); }
	//! The members, in insertion order.
	const std::vector<ComponentWaitable*>& waitables() const { return waitables_; }
private:
	//! The set's members, in insertion order.
	std::vector<ComponentWaitable*> waitables_; //!< The members, in insertion order.
};


//! One end of a stream or future. The two ends share a ComponentAsyncChannel.
class ComponentAsyncChannel;

/** \brief Readable or writable endpoint for a shared stream/future channel. */
class ComponentAsyncEndpoint : public ComponentWaitable {
public:
	//! Creates an endpoint for `handle` on `channel` in `direction`.
	ComponentAsyncEndpoint(ComponentWaitableKind kind, uint32_t handle,
			ComponentEndDirection direction, ComponentAsyncChannel* channel)
		: ComponentWaitable(kind), handle_(handle), direction_(direction),
		  channel_(channel) {}

	//! The endpoint's Canonical ABI handle.
	uint32_t handle() const { return handle_; }
	//! Whether this end reads or writes the channel.
	ComponentEndDirection direction() const { return direction_; }
	//! The endpoint's current copy state.
	ComponentEndState state() const { return state_; }
	//! Sets the endpoint's copy state.
	void setState(ComponentEndState state) { state_ = state; }

	//! The channel both ends share.
	ComponentAsyncChannel* channel() const { return channel_; }
	//! Re-points the endpoint at another channel.
	void setChannel(ComponentAsyncChannel* channel) { channel_ = channel; }

	bool takeEvent(ComponentAsyncEvent& event) override
	{
		if (!hasPendingEvent_)
			return false;
		hasPendingEvent_ = false;
		event = pendingEvent_;
		pendingEvent_ = ComponentAsyncEvent{};
		return true;
	}

	//! True when an event is waiting to be taken.
	bool hasPendingEvent() const { return hasPendingEvent_; }
	//! Queues `event` for the next `takeEvent`.
	void setPending(ComponentAsyncEvent event)
	{
		pendingEvent_ = event;
		hasPendingEvent_ = true;
	}

	//! A blocked async copy keeps the guest buffer here so the other end can
	//! complete it when data arrives. The pointer is into linear memory, which
	//! does not move while the memory is not grown.
	void setPendingBuffer(LinearMemory* memory, uint64_t address, uint64_t count,
			bool isRead)
	{
		pendingMemory_ = memory;
		pendingAddress_ = address;
		pendingCount_ = count;
		pendingIsRead_ = isRead;
	}
	//! Clears the blocked-copy buffer (the copy completed or was cancelled).
	void clearPendingBuffer()
	{
		pendingMemory_ = nullptr;
		pendingAddress_ = 0;
		pendingCount_ = 0;
		pendingIsRead_ = false;
	}
	//! True when a blocked copy owns a guest buffer.
	bool hasPendingBuffer() const { return pendingMemory_ != nullptr; }
	//! The memory the blocked copy's buffer lives in.
	LinearMemory* pendingMemory() const { return pendingMemory_; }
	//! The byte address of the blocked copy's buffer.
	uint64_t pendingAddress() const { return pendingAddress_; }
	//! How many bytes the blocked copy still expects.
	uint64_t pendingCount() const { return pendingCount_; }
	//! True when the blocked copy is a read (guest buffer is a destination).
	bool pendingIsRead() const { return pendingIsRead_; }
private:
	uint32_t handle_ = 0; //!< The Canonical ABI handle of the endpoint.
	ComponentEndDirection direction_; //!< Whether this endpoint reads or writes.
	ComponentAsyncChannel* channel_ = nullptr; //!< The channel this endpoint belongs to.
	ComponentEndState state_ = ComponentEndState::Idle; //!< The lifecycle state of the endpoint.
	bool hasPendingEvent_ = false; //!< True while `pendingEvent_` is deliverable.
	ComponentAsyncEvent pendingEvent_{}; //!< The event waiting to be taken.
	LinearMemory* pendingMemory_ = nullptr; //!< Guest memory of a blocked copy.
	uint64_t pendingAddress_ = 0; //!< Guest address of a blocked copy.
	uint64_t pendingCount_ = 0; //!< Bytes the blocked copy still expects.
	bool pendingIsRead_ = false; //!< True when the blocked copy reads from memory.
};

//! The shared queue behind a readable/writable pair. `elementSize == 0` models
//! the element-less `stream`/`future` forms, for which the buffer is ignored.
/** \brief Shared element queue and lifecycle state behind paired endpoints. */
class ComponentAsyncChannel {
public:
	//! Creates the channel for `elementSize`-byte elements (0 = element-less).
	explicit ComponentAsyncChannel(uint32_t elementSize, bool isFuture)
		: elementSize_(elementSize), isFuture_(isFuture) {}

	//! The element size in bytes (0 for the element-less forms).
	uint32_t elementSize() const { return elementSize_; }
	//! True for a `future` channel; false for a `stream`.
	bool isFuture() const { return isFuture_; }

	//! True while the readable end is open.
	bool readableAlive() const { return readableAlive_; }
	//! True while the writable end is open.
	bool writableAlive() const { return writableAlive_; }
	//! Records that the readable end was dropped (or not).
	void setReadableAlive(bool alive) { readableAlive_ = alive; }
	//! Records that the writable end was dropped (or not).
	void setWritableAlive(bool alive) { writableAlive_ = alive; }

	//! Raw byte queue. A stream transfers whole elements; a future transfers at
	//! most one value and then stays `Done`.
	//! The queue, mutable form (the copy helpers append and consume elements).
	std::deque<std::vector<uint8_t>>& queue() { return queue_; }
	//! The queue, read-only form.
	const std::deque<std::vector<uint8_t>>& queue() const { return queue_; }

	//! True once a future's single value has been written.
	bool futureWritten() const { return futureWritten_; }
	//! Records that a future's value was written.
	void setFutureWritten(bool written) { futureWritten_ = written; }
private:
	uint32_t elementSize_ = 0; //!< Byte size of one channel element.
	bool isFuture_ = false; //!< True for a future, false for a stream.
	bool readableAlive_ = true; //!< False once the readable end is dropped.
	bool writableAlive_ = true; //!< False once the writable end is dropped.
	bool futureWritten_ = false; //!< True once the single future value was written.
	std::deque<std::vector<uint8_t>> queue_; //!< The buffered elements, oldest first.
};


//! \brief The single-agent execution interface
//!
//! This is the seam a future multi-agent backend implements. The default
//! implementation is intentionally synchronous, single-threaded and reentrant:
//! `runUntilIdle` drains the queue on the calling thread. There is no OS thread
//! and no global state beyond the ComponentAsyncState instance.
/** \brief Scheduling interface used by component async operations. */
class ComponentExecutor {
public:
	virtual ~ComponentExecutor() = default;

	//! The answers of the `canon` thread built-ins. A single-agent executor
	//! always reports 0 and 1; a multi-agent executor may report per-agent
	//! values.
	virtual uint32_t threadIndex() const = 0;
	//! How many agents may execute concurrently (1 for a single-agent executor).
	virtual uint32_t availableParallelism() const = 0;
	//! True when this executor admits one agent only.
	virtual bool singleAgent() const = 0;

	//! Schedule `work` to run on the cooperative queue. It must not run before
	//! `runUntilIdle()` is called.
	virtual void push(std::function<void()> work) = 0;
	//! Runs every queued item, including items they enqueue.
	virtual void runUntilIdle() = 0;
	//! How many items are queued.
	virtual std::size_t pending() const = 0;
};

/** \brief Synchronous executor implementing the documented single-agent mode. */
class SingleAgentExecutor final : public ComponentExecutor {
public:
	uint32_t threadIndex() const override { return 0; }
	//! Always 1: the single-agent executor runs one task at a time.
	uint32_t availableParallelism() const override { return 1; }
	//! Always true: this is the documented single-agent mode.
	bool singleAgent() const override { return true; }

	void push(std::function<void()> work) override
	{
		if (work)
			queue_.push_back(std::move(work));
	}

	void runUntilIdle() override
	{
		//! Take a snapshot so work that enqueues more work is still drained.
		while (!queue_.empty()) {
			std::function<void()> work = std::move(queue_.front());
			queue_.pop_front();
			work();
		}
	}

	//! Number of queued work items.
	std::size_t pending() const override { return queue_.size(); }
private:
	//! The cooperative work queue, drained by `runUntilIdle`.
	std::deque<std::function<void()>> queue_;
};


//! \brief Stream/future and waitable bookkeeping
//!
//! ComponentAsyncState owns the handles for async objects. The host runtime
//! supplies the allocator/releaser so async handles share the component's one
//! handle index space with resources.
/**
 * \brief Owns async handles, channels, tasks, waitable sets, and error contexts.
 *
 * The current implementation is synchronous and single-agent. Allocated handles
 * share the component instance's handle table through the supplied callbacks.
 */
class ComponentAsyncState {
public:
//! Allocates a handle in the component's shared handle space.
	using HandleAllocator = std::function<uint32_t()>;
//! Releases a handle previously obtained from `HandleAllocator`.
	using HandleReleaser = std::function<void(uint32_t)>;

	/** \brief Creates async state using the component's shared handle allocator. \param allocate Allocates a handle in the component handle space. \param release Releases a previously allocated handle. */
	ComponentAsyncState(HandleAllocator allocate, HandleReleaser release)
		: allocate_(std::move(allocate)), release_(std::move(release)) {}

	//! A stream/future with an optional element size. Returns the readable and
	//! writable handle (the low/high i32 of `{stream,future}.new`).
	/** \brief Creates paired readable/writable stream or future handles. \param future true for a single-value future; false for a stream. \param elementSize Size in bytes of each element (zero for element-less forms). \return Pair of readable and writable handles, in that order. */
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
		return {readable, writable};
	}

	//! `{stream,future}.read`: copy up to `count` elements from the channel into
	//! `memory[address]`. Returns `kBlocked` when `async` and no data is ready.
	/** \brief Reads queued elements into guest linear memory. \param handle Readable endpoint handle. \param memory Guest memory receiving copied bytes. \param address Destination byte address. \param count Maximum number of elements to read. \param async Whether a missing value may leave the copy blocked. \return Packed copy result, or kBlocked for a pending asynchronous read. \throws std::runtime_error for invalid state, out-of-bounds memory, or synchronous blocking. */
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
	/** \brief Copies elements from guest linear memory into the channel. \param handle Writable endpoint handle. \param memory Guest memory containing source bytes. \param address Source byte address. \param count Number of elements to write. \param async Whether a missing reader may leave the copy blocked. \return Packed copy result, or kBlocked for a pending asynchronous write. \throws std::runtime_error for invalid state, out-of-bounds memory, or invalid future writes. */
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
	/** \brief Cancels a copy already blocked on an endpoint. \param handle Endpoint handle whose pending copy is cancelled. \return Packed Cancelled result. \throws std::runtime_error if the handle is invalid or no copy is pending. */
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

	/** \brief Drops one endpoint and notifies its peer. \param handle Endpoint handle to release. \param direction Expected endpoint direction. \throws std::runtime_error for invalid handles, direction, or lifecycle state. */
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
	/** \brief Transfers queued data and endpoint linkage from one channel to another. \param readableHandle Source readable endpoint. \param writableHandle Destination writable endpoint. \throws std::runtime_error if endpoint types/sizes differ or either end is busy. */
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
	/** \brief Removes an endpoint handle without notifying its peer. \param handle Endpoint handle to discard; absent handles are ignored. */
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
	/** \brief Allocates an empty waitable set. \return Handle of the new set. */
	uint32_t newWaitableSet()
	{
		const uint32_t handle = allocate_();
		waitableSets_.emplace(handle, ComponentWaitableSet{});
		return handle;
	}

	/** \brief Resolves a waitable-set handle. \param handle Waitable-set handle. \return Mutable reference to the set. \throws std::runtime_error if the handle is invalid. */
	ComponentWaitableSet& waitableSet(uint32_t handle)
	{
		const auto it = waitableSets_.find(handle);
		if (it == waitableSets_.end())
			throw std::runtime_error("component async: invalid waitable-set handle");
		return it->second;
	}

	/** \brief Waits/polls the single-agent set for its next pending event. \param setHandle Waitable-set handle. \return Next event, or the None sentinel when no event is pending. */
	ComponentAsyncEvent wait(uint32_t setHandle)
	{
		return waitableSet(setHandle).wait();
	}

	/** \brief Polls a waitable set without blocking. \param setHandle Waitable-set handle. \return Next event, or the None sentinel when no event is pending. */
	ComponentAsyncEvent poll(uint32_t setHandle)
	{
		return waitableSet(setHandle).poll();
	}

	/** \brief Releases an empty waitable set. \param handle Waitable-set handle. \throws std::runtime_error for an invalid or non-empty set. */
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

	/** \brief Adds a waitable to a set, or detaches it when setHandle is zero. \param waitableHandle Handle of the waitable object. \param setHandle Target set handle, or zero to detach. \throws std::runtime_error if the waitable or set handle is invalid. */
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
	/** \brief Returns the current task activation. \return Mutable reference to the current task. */
	ComponentTask& currentTask() { return *currentTask_; }
	/** \brief Resolves the current task. \return Canonical ABI success value (zero). */
	uint32_t taskReturn() { currentTask_->resolve(); return 0; }
	/** \brief Cancels the current task after a cancellation request. \return Canonical ABI success value (zero). \throws std::runtime_error if cancellation was not requested. */
	uint32_t taskCancel()
	{
		if (!currentTask_->cancellationRequested())
			throw std::runtime_error(
					"component async: task.cancel without a cancellation request");
		currentTask_->cancel();
		return 0;
	}

	/** \brief Increments or decrements current-task backpressure. \param increment true to increment; false to decrement. \throws std::runtime_error if decrementing a zero count. */
	void setBackpressure(bool increment)
	{
		if (increment)
			currentTask_->incrementBackpressure();
		else
			currentTask_->decrementBackpressure();
	}

	/** \brief Returns current-task backpressure. \return Current backpressure count. */
	uint32_t backpressure() const { return currentTask_->backpressure(); }

	//! Error contexts. The single-agent model stores the diagnostic string; a
	//! multi-agent backend would use the same handle table.
	/** \brief Creates an error-context handle containing a diagnostic message. \param message Message to associate with the new context. \return Allocated error-context handle. */
	uint32_t newErrorContext(std::string message)
	{
		const uint32_t handle = allocate_();
		errorContexts_.emplace(handle, std::move(message));
		return handle;
	}

	/** \brief Retrieves an error-context message. \param handle Error-context handle. \return Const reference to the stored message. \throws std::runtime_error if the handle is invalid. */
	const std::string& errorContext(uint32_t handle) const
	{
		const auto it = errorContexts_.find(handle);
		if (it == errorContexts_.end())
			throw std::runtime_error("component async: invalid error-context handle");
		return it->second;
	}

	/** \brief Releases an error-context handle. \param handle Error-context handle to release. \throws std::runtime_error if the handle is invalid. */
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
	/** \brief Creates a subtask handle in the synchronous executor. \return Newly allocated subtask handle. */
	uint32_t newSubtask()
	{
		const uint32_t handle = allocate_();
		subtasks_.insert(handle);
		return handle;
	}

	/** \brief Cancels and releases a subtask handle. \param handle Subtask handle. \return ComponentTaskState::Cancelled. \throws std::runtime_error if the handle is invalid. */
	uint32_t cancelSubtask(uint32_t handle)
	{
		const auto it = subtasks_.find(handle);
		if (it == subtasks_.end())
			throw std::runtime_error("component async: invalid subtask handle");
		subtasks_.erase(it);
		release_(handle);
		return static_cast<uint32_t>(ComponentTaskState::Cancelled);
	}

	/** \brief Releases a subtask handle. \param handle Subtask handle. \throws std::runtime_error if the handle is invalid. */
	void dropSubtask(uint32_t handle)
	{
		const auto it = subtasks_.find(handle);
		if (it == subtasks_.end())
			throw std::runtime_error("component async: invalid subtask handle");
		subtasks_.erase(it);
		release_(handle);
	}

	/** \brief Returns the executor used by this async state. \return Mutable reference to the executor. */
	ComponentExecutor& executor() { return executor_; }

	/** \brief Looks up an endpoint without throwing. \param handle Endpoint handle. \return Endpoint pointer, or nullptr when absent. */
	ComponentAsyncEndpoint* endpointOrNull(uint32_t handle)
	{
		const auto it = endpoints_.find(handle);
		return it == endpoints_.end() ? nullptr : it->second.get();
	}

	/** \brief Finds the paired endpoint for a stream/future handle. \param handle Endpoint handle. \return Peer endpoint, or nullptr if unavailable. */
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
	//! The event code a waitable kind produces.
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

	//! The endpoint for `handle`, checking that `direction` matches the end.
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

	//! The waitable behind `handle`, or null when it is not an endpoint.
	ComponentWaitable* waitableFor(uint32_t handle)
	{
		if (ComponentAsyncEndpoint* end = endpointOrNull(handle); end != nullptr)
			return end;
		return nullptr;
	}

	//! Throws unless the endpoint is idle (no copy in flight).
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



	//! Throws unless `[address, address + size)` lies inside `memory`.
	static void checkBuffer(LinearMemory& memory, uint64_t address, uint64_t size)
	{
		if (address > memory.memorySize || size > memory.memorySize - address)
			throw std::runtime_error(
					"component async: stream/future buffer is out of bounds");
	}

	//! Copies `size` bytes into linear memory (the caller checked the range).
	static void copyToMemory(LinearMemory& memory, uint64_t address,
			const uint8_t* data, std::size_t size)
	{
		if (size != 0)
			std::memcpy(memory.memoryBase + address, data, size);
	}

	//! Copies `size` bytes out of linear memory (the caller checked the range).
	static void copyFromMemory(uint8_t* out, LinearMemory& memory,
			uint64_t address, std::size_t size)
	{
		if (size != 0)
			std::memcpy(out, memory.memoryBase + address, size);
	}

	HandleAllocator allocate_; //!< Allocates a handle in the shared resource index space.
	HandleReleaser release_; //!< Releases a handle from that index space.
	SingleAgentExecutor executor_; //!< Runs task bodies on the calling agent.
	//! The task activation currently running on the calling agent.
	std::unique_ptr<ComponentTask> currentTask_ = std::make_unique<ComponentTask>(0);
	std::unordered_map<uint32_t, std::unique_ptr<ComponentAsyncEndpoint>> endpoints_; //!< Endpoints, keyed by handle.
	std::vector<std::unique_ptr<ComponentAsyncChannel>> channelStorage_; //!< Owns the channels referenced by `channels_`.
	std::unordered_map<uint32_t, ComponentAsyncChannel*> channels_; //!< Channels, keyed by their handle.
	std::unordered_map<uint32_t, uint32_t> readableOf_; //!< writable -> readable
	std::unordered_map<uint32_t, uint32_t> writableOf_; //!< readable -> writable
	//! Waitable sets, keyed by their Canonical ABI handle.
	std::unordered_map<uint32_t, ComponentWaitableSet> waitableSets_;
	//! Error-context strings, keyed by their Canonical ABI handle.
	std::unordered_map<uint32_t, std::string> errorContexts_;
	//! Live subtask handles.
	std::unordered_set<uint32_t> subtasks_;
};


} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTASYNC_HPP
