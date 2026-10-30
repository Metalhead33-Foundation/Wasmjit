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
	None = 0,
	Subtask = 1,
	StreamRead = 2,
	StreamWrite = 3,
	FutureRead = 4,
	FutureWrite = 5,
	TaskCancelled = 6
};

/** \brief Outcome of a stream/future copy operation. */
enum class ComponentCopyResult : uint32_t {
	Completed = 0,
	Dropped = 1,
	Cancelled = 2
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
	Pending,
	Running,
	Cancelled,
	Resolved
};

//! The direction of one end of a stream/future.
/** \brief Read/write role of a stream or future endpoint. */
enum class ComponentEndDirection : uint8_t {
	Readable,
	Writable
};

//! `End.State` in the Canonical ABI explainer.
/** \brief Copy/cancellation state of one async endpoint. */
enum class ComponentEndState : uint8_t {
	Idle,
	Copying,
	CancellingCopy,
	Done
};

//! What a handle denotes. Resources are owned by the runtime's resource table;
//! this enum only covers the async objects a handle table can also contain.
/** \brief Kind of async object that can participate in a waitable set. */
enum class ComponentWaitableKind : uint8_t {
	Subtask,
	StreamRead,
	StreamWrite,
	FutureRead,
	FutureWrite,
	TaskCancelled
};

/** \brief Event code and two Canonical ABI payload words produced by a waitable. */
struct ComponentAsyncEvent {
	ComponentEventCode code = ComponentEventCode::None;
	uint32_t payload1 = 0;
	uint32_t payload2 = 0;

	/** \brief Tests whether this is the no-event sentinel. \return true when code is None. */
	bool isNone() const { return code == ComponentEventCode::None; }
};

//! One context slot value. The Canonical ABI only asks for a value of the
//! core type named by `canon context.get/set`, so a 64-bit cell is enough.
/** \brief Storage for one typed Canonical ABI task context slot. */
union ComponentContextSlot {
	int64_t i64 = 0;
	uint64_t u64;
	double f64;
	int32_t i32;
	uint32_t u32;
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
	uint32_t id_ = 0;
	ComponentTaskState state_ = ComponentTaskState::Pending;
	uint32_t backpressure_ = 0;
	bool cancellationRequested_ = false;
	std::unordered_map<uint32_t, ComponentContextSlot> context_;
};

//! A waitable is anything that can produce a ComponentAsyncEvent and join one
//! waitable set. Stream/future ends and subtasks are the concrete producers.
/** \brief Base interface for objects that can emit async events and join waitable sets. */
class ComponentWaitable {
public:
	explicit ComponentWaitable(ComponentWaitableKind kind) : kind_(kind) {}
	virtual ~ComponentWaitable() = default;

/**
 * \brief Performs the kind operation.
 * \return The value or status produced by this operation.
 */
	ComponentWaitableKind kind() const { return kind_; }
/**
 * \brief Performs the inWaitableSet operation.
 * \return The value or status produced by this operation.
 */
	bool inWaitableSet() const { return set_ != nullptr; }
/**
 * \brief Performs the waitableSet operation.
 */
	void* waitableSet() const { return set_; }
/**
 * \brief Performs the setWaitableSet operation.
 * \param set Input value or output destination for this operation.
 */
	void setWaitableSet(void* set) { set_ = set; }

	//! Returns true and fills `event` when progress is pending.
/**
 * \brief Performs the takeEvent operation.
 * \param event Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	virtual bool takeEvent(ComponentAsyncEvent& event) = 0;
private:
	ComponentWaitableKind kind_;
	void* set_ = nullptr;
};

//! A single-agent waitable set. `wait` and `poll` both return `none` when no
//! event is pending: there is no second agent that could make one appear, and
//! the documented single-agent deviation is that a caller must not spin on an
//! empty set (docs/proposals/THREADS.md TD9).
/** \brief Single-agent collection of waitables that can be polled for events. */
class ComponentWaitableSet {
public:
/**
 * \brief Performs the poll operation.
 * \return The value or status produced by this operation.
 */
	ComponentAsyncEvent poll()
	{
		for (ComponentWaitable* waitable : waitables_) {
			ComponentAsyncEvent event;
			if (waitable->takeEvent(event))
				return event;
		}
		return ComponentAsyncEvent{};
	}

/**
 * \brief Performs the wait operation.
 * \return The value or status produced by this operation.
 */
	ComponentAsyncEvent wait() { return poll(); }

/**
 * \brief Performs the add operation.
 * \param waitable Input value or output destination for this operation.
 */
	void add(ComponentWaitable* waitable)
	{
		if (waitable == nullptr || waitable->waitableSet() == this)
			return;
		remove(waitable);
/**
 * \brief Performs the push_back operation.
 * \param waitable Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		waitables_.push_back(waitable);
/**
 * \brief Performs the setWaitableSet operation.
 * \param this Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		waitable->setWaitableSet(this);
	}

/**
 * \brief Performs the remove operation.
 * \param waitable Input value or output destination for this operation.
 */
	void remove(ComponentWaitable* waitable)
	{
		if (waitable == nullptr)
			return;
		for (auto it = waitables_.begin(); it != waitables_.end(); ++it) {
			if (*it == waitable) {
/**
 * \brief Performs the erase operation.
 * \param it Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
				waitables_.erase(it);
/**
 * \brief Performs the setWaitableSet operation.
 * \param nullptr Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
				waitable->setWaitableSet(nullptr);
				return;
			}
		}
	}

/**
 * \brief Performs the removeAll operation.
 */
	void removeAll()
	{
		for (ComponentWaitable* waitable : waitables_)
/**
 * \brief Performs the setWaitableSet operation.
 * \param nullptr Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			waitable->setWaitableSet(nullptr);
/**
 * \brief Performs the clear operation.
 * \return The value or status produced by this operation.
 */
		waitables_.clear();
	}

/**
 * \brief Performs the empty operation.
 * \return The value or status produced by this operation.
 */
	bool empty() const { return waitables_.empty(); }
/**
 * \brief Performs the size operation.
 * \return The value or status produced by this operation.
 */
	std::size_t size() const { return waitables_.size(); }
/**
 * \brief Performs the waitables operation.
 * \return The value or status produced by this operation.
 */
	const std::vector<ComponentWaitable*>& waitables() const { return waitables_; }
private:
	std::vector<ComponentWaitable*> waitables_;
};


//! One end of a stream or future. The two ends share a ComponentAsyncChannel.
class ComponentAsyncChannel;

/** \brief Readable or writable endpoint for a shared stream/future channel. */
class ComponentAsyncEndpoint : public ComponentWaitable {
public:
	ComponentAsyncEndpoint(ComponentWaitableKind kind, uint32_t handle,
			ComponentEndDirection direction, ComponentAsyncChannel* channel)
/**
 * \brief Performs the ComponentWaitable operation.
 * \param channel Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		: ComponentWaitable(kind), handle_(handle), direction_(direction),
		  channel_(channel) {}

/**
 * \brief Performs the handle operation.
 * \return The value or status produced by this operation.
 */
	uint32_t handle() const { return handle_; }
/**
 * \brief Performs the direction operation.
 * \return The value or status produced by this operation.
 */
	ComponentEndDirection direction() const { return direction_; }
/**
 * \brief Performs the state operation.
 * \return The value or status produced by this operation.
 */
	ComponentEndState state() const { return state_; }
/**
 * \brief Performs the setState operation.
 * \param state Input value or output destination for this operation.
 */
	void setState(ComponentEndState state) { state_ = state; }

/**
 * \brief Performs the channel operation.
 * \return The value or status produced by this operation.
 */
	ComponentAsyncChannel* channel() const { return channel_; }
/**
 * \brief Performs the setChannel operation.
 * \param channel Input value or output destination for this operation.
 */
	void setChannel(ComponentAsyncChannel* channel) { channel_ = channel; }

/**
 * \brief Performs the takeEvent operation.
 * \param event Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	bool takeEvent(ComponentAsyncEvent& event) override
	{
		if (!hasPendingEvent_)
			return false;
		hasPendingEvent_ = false;
		event = pendingEvent_;
		pendingEvent_ = ComponentAsyncEvent{};
		return true;
	}

/**
 * \brief Performs the hasPendingEvent operation.
 * \return The value or status produced by this operation.
 */
	bool hasPendingEvent() const { return hasPendingEvent_; }
/**
 * \brief Performs the setPending operation.
 * \param event Input value or output destination for this operation.
 */
	void setPending(ComponentAsyncEvent event)
	{
		pendingEvent_ = event;
		hasPendingEvent_ = true;
	}

	//! A blocked async copy keeps the guest buffer here so the other end can
	//! complete it when data arrives. The pointer is into linear memory, which
	//! does not move while the memory is not grown.
/**
 * \brief Performs the setPendingBuffer operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param count Input value or output destination for this operation.
 * \param isRead Input value or output destination for this operation.
 */
	void setPendingBuffer(LinearMemory* memory, uint64_t address, uint64_t count,
			bool isRead)
	{
		pendingMemory_ = memory;
		pendingAddress_ = address;
		pendingCount_ = count;
		pendingIsRead_ = isRead;
	}
/**
 * \brief Performs the clearPendingBuffer operation.
 */
	void clearPendingBuffer()
	{
		pendingMemory_ = nullptr;
		pendingAddress_ = 0;
		pendingCount_ = 0;
		pendingIsRead_ = false;
	}
/**
 * \brief Performs the hasPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
	bool hasPendingBuffer() const { return pendingMemory_ != nullptr; }
/**
 * \brief Performs the pendingMemory operation.
 * \return The value or status produced by this operation.
 */
	LinearMemory* pendingMemory() const { return pendingMemory_; }
/**
 * \brief Performs the pendingAddress operation.
 * \return The value or status produced by this operation.
 */
	uint64_t pendingAddress() const { return pendingAddress_; }
/**
 * \brief Performs the pendingCount operation.
 * \return The value or status produced by this operation.
 */
	uint64_t pendingCount() const { return pendingCount_; }
/**
 * \brief Performs the pendingIsRead operation.
 * \return The value or status produced by this operation.
 */
	bool pendingIsRead() const { return pendingIsRead_; }
private:
	uint32_t handle_ = 0;
	ComponentEndDirection direction_;
	ComponentAsyncChannel* channel_ = nullptr;
	ComponentEndState state_ = ComponentEndState::Idle;
	bool hasPendingEvent_ = false;
	ComponentAsyncEvent pendingEvent_{};
	LinearMemory* pendingMemory_ = nullptr;
	uint64_t pendingAddress_ = 0;
	uint64_t pendingCount_ = 0;
	bool pendingIsRead_ = false;
};

//! The shared queue behind a readable/writable pair. `elementSize == 0` models
//! the element-less `stream`/`future` forms, for which the buffer is ignored.
/** \brief Shared element queue and lifecycle state behind paired endpoints. */
class ComponentAsyncChannel {
public:
/**
 * \brief Performs the ComponentAsyncChannel operation.
 * \param elementSize Input value or output destination for this operation.
 * \param isFuture Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	explicit ComponentAsyncChannel(uint32_t elementSize, bool isFuture)
/**
 * \brief Performs the elementSize_ operation.
 * \param isFuture Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		: elementSize_(elementSize), isFuture_(isFuture) {}

/**
 * \brief Performs the elementSize operation.
 * \return The value or status produced by this operation.
 */
	uint32_t elementSize() const { return elementSize_; }
/**
 * \brief Performs the isFuture operation.
 * \return The value or status produced by this operation.
 */
	bool isFuture() const { return isFuture_; }

/**
 * \brief Performs the readableAlive operation.
 * \return The value or status produced by this operation.
 */
	bool readableAlive() const { return readableAlive_; }
/**
 * \brief Performs the writableAlive operation.
 * \return The value or status produced by this operation.
 */
	bool writableAlive() const { return writableAlive_; }
/**
 * \brief Performs the setReadableAlive operation.
 * \param alive Input value or output destination for this operation.
 */
	void setReadableAlive(bool alive) { readableAlive_ = alive; }
/**
 * \brief Performs the setWritableAlive operation.
 * \param alive Input value or output destination for this operation.
 */
	void setWritableAlive(bool alive) { writableAlive_ = alive; }

	//! Raw byte queue. A stream transfers whole elements; a future transfers at
	//! most one value and then stays `Done`.
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
	std::deque<std::vector<uint8_t>>& queue() { return queue_; }
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
	const std::deque<std::vector<uint8_t>>& queue() const { return queue_; }

/**
 * \brief Performs the futureWritten operation.
 * \return The value or status produced by this operation.
 */
	bool futureWritten() const { return futureWritten_; }
/**
 * \brief Performs the setFutureWritten operation.
 * \param written Input value or output destination for this operation.
 */
	void setFutureWritten(bool written) { futureWritten_ = written; }
private:
	uint32_t elementSize_ = 0;
	bool isFuture_ = false;
	bool readableAlive_ = true;
	bool writableAlive_ = true;
	bool futureWritten_ = false;
	std::deque<std::vector<uint8_t>> queue_;
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
/**
 * \brief Performs the threadIndex operation.
 * \return The value or status produced by this operation.
 */
	virtual uint32_t threadIndex() const = 0;
/**
 * \brief Performs the availableParallelism operation.
 * \return The value or status produced by this operation.
 */
	virtual uint32_t availableParallelism() const = 0;
/**
 * \brief Performs the singleAgent operation.
 * \return The value or status produced by this operation.
 */
	virtual bool singleAgent() const = 0;

	//! Schedule `work` to run on the cooperative queue. It must not run before
	//! `runUntilIdle()` is called.
/**
 * \brief Performs the push operation.
 * \param work Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	virtual void push(std::function<void()> work) = 0;
/**
 * \brief Performs the runUntilIdle operation.
 * \return The value or status produced by this operation.
 */
	virtual void runUntilIdle() = 0;
/**
 * \brief Performs the pending operation.
 * \return The value or status produced by this operation.
 */
	virtual std::size_t pending() const = 0;
};

/** \brief Synchronous executor implementing the documented single-agent mode. */
class SingleAgentExecutor final : public ComponentExecutor {
public:
/**
 * \brief Performs the threadIndex operation.
 * \return The value or status produced by this operation.
 */
	uint32_t threadIndex() const override { return 0; }
/**
 * \brief Performs the availableParallelism operation.
 * \return The value or status produced by this operation.
 */
	uint32_t availableParallelism() const override { return 1; }
/**
 * \brief Performs the singleAgent operation.
 * \return The value or status produced by this operation.
 */
	bool singleAgent() const override { return true; }

/**
 * \brief Performs the push operation.
 * \param work Input value or output destination for this operation.
 */
	void push(std::function<void()> work) override
	{
		if (work)
/**
 * \brief Performs the push_back operation.
 * \return The value or status produced by this operation.
 */
			queue_.push_back(std::move(work));
	}

/**
 * \brief Performs the runUntilIdle operation.
 */
	void runUntilIdle() override
	{
		//! Take a snapshot so work that enqueues more work is still drained.
		while (!queue_.empty()) {
/**
 * \brief Performs the void operation.
 * \param work Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			std::function<void()> work = std::move(queue_.front());
/**
 * \brief Performs the pop_front operation.
 * \return The value or status produced by this operation.
 */
			queue_.pop_front();
			work();
		}
	}

/**
 * \brief Performs the pending operation.
 * \return The value or status produced by this operation.
 */
	std::size_t pending() const override { return queue_.size(); }
private:
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
	using HandleAllocator = std::function<uint32_t()>;
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
/**
 * \brief Performs the setState operation.
 * \param Copying Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
					end.setState(ComponentEndState::Copying);
/**
 * \brief Performs the setPendingBuffer operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param true Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
					end.setPendingBuffer(&memory, address, 1, true);
					return kBlocked;
				}
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
				throw std::runtime_error(
						"component async: a synchronous future.read would block "
						"(single-agent runtime)");
			}
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			const std::vector<uint8_t>& value = channel.queue().front();
			if (!value.empty())
				copyToMemory(memory, address, value.data(), value.size());
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			channel.queue().pop_front();
/**
 * \brief Performs the setState operation.
 * \param Done Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			end.setState(ComponentEndState::Done);
/**
 * \brief Performs the clearPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
			end.clearPendingBuffer();
			return packCopyResult(ComponentCopyResult::Completed, 1);
		}

/**
 * \brief Performs the elementSize operation.
 * \return The value or status produced by this operation.
 */
		const std::size_t elementSize = channel.elementSize();
		uint64_t copied = 0;
		while (copied < count && !channel.queue().empty()) {
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			const std::vector<uint8_t>& value = channel.queue().front();
			if (elementSize != 0 && !value.empty())
				copyToMemory(memory, address + copied * elementSize,
/**
 * \brief Performs the data operation.
 * \return The value or status produced by this operation.
 */
						value.data(), value.size());
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			channel.queue().pop_front();
			++copied;
		}
		if (copied != 0 || count == 0) {
/**
 * \brief Performs the clearPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
			end.clearPendingBuffer();
			return packCopyResult(ComponentCopyResult::Completed,
					static_cast<uint32_t>(copied));
		}
		if (async) {
/**
 * \brief Performs the setState operation.
 * \param Copying Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			end.setState(ComponentEndState::Copying);
/**
 * \brief Performs the setPendingBuffer operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param count Input value or output destination for this operation.
 * \param true Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			end.setPendingBuffer(&memory, address, count, true);
			return kBlocked;
		}
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
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
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
				throw std::runtime_error(
						"component async: future.write called twice");
			}
/**
 * \brief Performs the value operation.
 * \return The value or status produced by this operation.
 */
			std::vector<uint8_t> value(channel.elementSize());
			if (!value.empty())
				copyFromMemory(value.data(), memory, address, value.size());
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			channel.queue().push_back(std::move(value));
/**
 * \brief Performs the setFutureWritten operation.
 * \param true Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			channel.setFutureWritten(true);
/**
 * \brief Performs the setState operation.
 * \param Done Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			end.setState(ComponentEndState::Done);
			//! If the reader already blocked, complete it now.
			if (ComponentAsyncEndpoint* peer = peerOf(handle);
/**
 * \brief Performs the hasPendingBuffer operation.
 * \param peer Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
					peer != nullptr && peer->hasPendingBuffer() && peer->pendingIsRead())
				completePendingRead(*peer);
			return packCopyResult(ComponentCopyResult::Completed, 1);
		}

/**
 * \brief Performs the elementSize operation.
 * \return The value or status produced by this operation.
 */
		const std::size_t elementSize = channel.elementSize();
		for (uint64_t i = 0; i < count; ++i) {
/**
 * \brief Performs the value operation.
 * \param elementSize Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			std::vector<uint8_t> value(elementSize);
			if (elementSize != 0)
				copyFromMemory(value.data(), memory, address + i * elementSize,
						elementSize);
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			channel.queue().push_back(std::move(value));
		}
		if (ComponentAsyncEndpoint* peer = peerOf(handle);
/**
 * \brief Performs the hasPendingBuffer operation.
 * \param peer Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
				peer != nullptr && peer->hasPendingBuffer() && peer->pendingIsRead())
			completePendingRead(*peer);
		return packCopyResult(ComponentCopyResult::Completed,
				static_cast<uint32_t>(count));
	}

	//! `{stream,future}.cancel-{read,write}`. The single-agent runtime has no
	//! buffer-owning background copy, so cancellation only succeeds for an end
	//! whose copy was already blocked (state == Copying).
/**
 * \brief Performs the argument operation.
 * \param handle Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	/** \brief Cancels a copy already blocked on an endpoint. \param handle Endpoint handle whose pending copy is cancelled. \param async Reserved async-mode argument (currently unused). \return Packed Cancelled result. \throws std::runtime_error if the handle is invalid or no copy is pending. */
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

/**
 * \brief Performs the channel operation.
 * \return The value or status produced by this operation.
 */
		ComponentAsyncChannel* channel = end.channel();
/**
 * \brief Performs the clearPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
		end.clearPendingBuffer();
		if (direction == ComponentEndDirection::Readable)
/**
 * \brief Performs the setReadableAlive operation.
 * \param false Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			channel->setReadableAlive(false);
		else
/**
 * \brief Performs the setWritableAlive operation.
 * \param false Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			channel->setWritableAlive(false);

		if (ComponentAsyncEndpoint* peer = peerOf(handle); peer != nullptr) {
/**
 * \brief Performs the setChannel operation.
 * \param channel Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			peer->setChannel(channel);
/**
 * \brief Performs the setPending operation.
 * \return The value or status produced by this operation.
 */
			peer->setPending(ComponentAsyncEvent{eventCode(peer->kind()),
/**
 * \brief Performs the handle operation.
 * \param Dropped Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
					peer->handle(),
					packCopyResult(ComponentCopyResult::Dropped, 0)});
		}
		if (end.waitableSet() != nullptr)
/**
 * \brief Performs the waitableSet operation.
 * \param end Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			static_cast<ComponentWaitableSet*>(end.waitableSet())->remove(&end);
		release_(handle);
/**
 * \brief Performs the erase operation.
 * \param it Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		endpoints_.erase(it);
		if (channels_.count(handle) != 0)
/**
 * \brief Performs the erase operation.
 * \param handle Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
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
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			destination->queue().push_back(std::move(source->queue().front()));
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			source->queue().pop_front();
		}
		if (ComponentAsyncEndpoint* sourceWriter = peerOf(readableHandle);
				sourceWriter != nullptr)
/**
 * \brief Performs the setChannel operation.
 * \param destination Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			sourceWriter->setChannel(destination);
/**
 * \brief Performs the setChannel operation.
 * \param source Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
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

/**
 * \brief Performs the endpoint operation.
 * \param handle Input value or output destination for this operation.
 * \param direction Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	ComponentAsyncEndpoint& endpoint(uint32_t handle,
			ComponentEndDirection direction)
	{
/**
 * \brief Performs the find operation.
 * \param handle Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		auto it = endpoints_.find(handle);
		if (it == endpoints_.end())
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error("component async: invalid stream/future handle");
		if (it->second->direction() != direction)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error(
					"component async: wrong end of a stream/future");
		return *it->second;
	}

/**
 * \brief Performs the waitableFor operation.
 * \param handle Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	ComponentWaitable* waitableFor(uint32_t handle)
	{
		if (ComponentAsyncEndpoint* end = endpointOrNull(handle); end != nullptr)
			return end;
		return nullptr;
	}

/**
 * \brief Performs the requireIdle operation.
 * \param end Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void requireIdle(ComponentAsyncEndpoint& end)
	{
		if (end.state() != ComponentEndState::Idle)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error(
					"component async: stream/future end is not idle");
	}

	//! A previously blocked read: move up to its buffer's capacity from the
	//! channel into the guest memory and publish a progress event.
/**
 * \brief Performs the completePendingRead operation.
 * \param reader Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void completePendingRead(ComponentAsyncEndpoint& reader)
	{
/**
 * \brief Performs the channel operation.
 * \return The value or status produced by this operation.
 */
		ComponentAsyncChannel& channel = *reader.channel();
/**
 * \brief Performs the pendingMemory operation.
 * \return The value or status produced by this operation.
 */
		LinearMemory* memory = reader.pendingMemory();
		if (memory == nullptr)
			return;
/**
 * \brief Performs the pendingAddress operation.
 * \return The value or status produced by this operation.
 */
		const uint64_t address = reader.pendingAddress();
/**
 * \brief Performs the pendingCount operation.
 * \return The value or status produced by this operation.
 */
		const uint64_t count = reader.pendingCount();
/**
 * \brief Performs the elementSize operation.
 * \return The value or status produced by this operation.
 */
		const std::size_t elementSize = channel.elementSize();

		uint64_t copied = 0;
		while (copied < count && !channel.queue().empty()) {
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			const std::vector<uint8_t>& value = channel.queue().front();
			if (elementSize != 0 && !value.empty())
				copyToMemory(*memory, address + copied * elementSize, value.data(),
/**
 * \brief Performs the size operation.
 * \return The value or status produced by this operation.
 */
						value.size());
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
			channel.queue().pop_front();
			++copied;
		}
/**
 * \brief Performs the clearPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
		reader.clearPendingBuffer();
/**
 * \brief Performs the setState operation.
 * \param Idle Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
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
/**
 * \brief Performs the completePendingWrite operation.
 * \param writer Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void completePendingWrite(ComponentAsyncEndpoint& writer)
	{
/**
 * \brief Performs the channel operation.
 * \return The value or status produced by this operation.
 */
		ComponentAsyncChannel& channel = *writer.channel();
/**
 * \brief Performs the pendingMemory operation.
 * \return The value or status produced by this operation.
 */
		LinearMemory* memory = writer.pendingMemory();
		if (memory == nullptr)
			return;
/**
 * \brief Performs the pendingAddress operation.
 * \return The value or status produced by this operation.
 */
		const uint64_t address = writer.pendingAddress();
/**
 * \brief Performs the elementSize operation.
 * \return The value or status produced by this operation.
 */
		const std::size_t elementSize = channel.elementSize();
/**
 * \brief Performs the value operation.
 * \param elementSize Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		std::vector<uint8_t> value(elementSize);
		if (elementSize != 0)
			copyFromMemory(value.data(), *memory, address, elementSize);
/**
 * \brief Performs the queue operation.
 * \return The value or status produced by this operation.
 */
		channel.queue().push_back(std::move(value));
/**
 * \brief Performs the clearPendingBuffer operation.
 * \return The value or status produced by this operation.
 */
		writer.clearPendingBuffer();
/**
 * \brief Performs the setState operation.
 * \param Done Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		writer.setState(ComponentEndState::Done);
/**
 * \brief Performs the setPending operation.
 * \return The value or status produced by this operation.
 */
		writer.setPending(ComponentAsyncEvent{eventCode(writer.kind()),
/**
 * \brief Performs the handle operation.
 * \param Completed Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
				writer.handle(),
				packCopyResult(ComponentCopyResult::Completed, 1)});
	}



/**
 * \brief Performs the checkBuffer operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param size Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void checkBuffer(LinearMemory& memory, uint64_t address, uint64_t size)
	{
		if (address > memory.memorySize || size > memory.memorySize - address)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error(
					"component async: stream/future buffer is out of bounds");
	}

/**
 * \brief Performs the copyToMemory operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param data Input value or output destination for this operation.
 * \param size Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void copyToMemory(LinearMemory& memory, uint64_t address,
			const uint8_t* data, std::size_t size)
	{
		if (size != 0)
/**
 * \brief Performs the memcpy operation.
 * \param address Input value or output destination for this operation.
 * \param data Input value or output destination for this operation.
 * \param size Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			std::memcpy(memory.memoryBase + address, data, size);
	}

/**
 * \brief Performs the copyFromMemory operation.
 * \param out Input value or output destination for this operation.
 * \param memory Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param size Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	static void copyFromMemory(uint8_t* out, LinearMemory& memory,
			uint64_t address, std::size_t size)
	{
		if (size != 0)
/**
 * \brief Performs the memcpy operation.
 * \param out Input value or output destination for this operation.
 * \param address Input value or output destination for this operation.
 * \param size Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
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
