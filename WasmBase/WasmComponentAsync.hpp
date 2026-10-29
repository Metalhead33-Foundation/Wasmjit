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

enum class ComponentEventCode : uint32_t {
	None = 0,
	Subtask = 1,
	StreamRead = 2,
	StreamWrite = 3,
	FutureRead = 4,
	FutureWrite = 5,
	TaskCancelled = 6
};

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
constexpr uint32_t packCopyResult(ComponentCopyResult result, uint32_t progress)
{
	return (static_cast<uint32_t>(result) & 0xFu) | (progress << 4);
}

constexpr bool unpackCopyResult(uint32_t packed, ComponentCopyResult& result,
		uint32_t& progress)
{
	if (packed == kBlocked)
		return false;
	result = static_cast<ComponentCopyResult>(packed & 0xFu);
	progress = packed >> 4;
	return true;
}

enum class ComponentTaskState : uint8_t {
	Pending,
	Running,
	Cancelled,
	Resolved
};

//! The direction of one end of a stream/future.
enum class ComponentEndDirection : uint8_t {
	Readable,
	Writable
};

//! `End.State` in the Canonical ABI explainer.
enum class ComponentEndState : uint8_t {
	Idle,
	Copying,
	CancellingCopy,
	Done
};

//! What a handle denotes. Resources are owned by the runtime's resource table;
//! this enum only covers the async objects a handle table can also contain.
enum class ComponentWaitableKind : uint8_t {
	Subtask,
	StreamRead,
	StreamWrite,
	FutureRead,
	FutureWrite,
	TaskCancelled
};

struct ComponentAsyncEvent {
	ComponentEventCode code = ComponentEventCode::None;
	uint32_t payload1 = 0;
	uint32_t payload2 = 0;

	bool isNone() const { return code == ComponentEventCode::None; }
};

//! One context slot value. The Canonical ABI only asks for a value of the
//! core type named by `canon context.get/set`, so a 64-bit cell is enough.
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
class ComponentTask {
public:
	explicit ComponentTask(uint32_t id) : id_(id) {}

	uint32_t id() const { return id_; }
	ComponentTaskState state() const { return state_; }
	void setState(ComponentTaskState state) { state_ = state; }

	uint32_t backpressure() const { return backpressure_; }
	void incrementBackpressure() { ++backpressure_; }
	void decrementBackpressure()
	{
		if (backpressure_ == 0)
			throw std::runtime_error(
					"component async: backpressure.dec without a matching inc");
		--backpressure_;
	}

	void setContextSlot(uint32_t index, ComponentContextSlot value)
	{
		context_[index] = value;
	}
	ComponentContextSlot contextSlot(uint32_t index) const
	{
		const auto it = context_.find(index);
		return it == context_.end() ? ComponentContextSlot{} : it->second;
	}

	void requestCancellation() { cancellationRequested_ = true; }
	bool cancellationRequested() const { return cancellationRequested_; }

	void resolve() { state_ = ComponentTaskState::Resolved; }
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
class ComponentWaitable {
public:
	explicit ComponentWaitable(ComponentWaitableKind kind) : kind_(kind) {}
	virtual ~ComponentWaitable() = default;

	ComponentWaitableKind kind() const { return kind_; }
	bool inWaitableSet() const { return set_ != nullptr; }
	void* waitableSet() const { return set_; }
	void setWaitableSet(void* set) { set_ = set; }

	//! Returns true and fills `event` when progress is pending.
	virtual bool takeEvent(ComponentAsyncEvent& event) = 0;
private:
	ComponentWaitableKind kind_;
	void* set_ = nullptr;
};

//! A single-agent waitable set. `wait` and `poll` both return `none` when no
//! event is pending: there is no second agent that could make one appear, and
//! the documented single-agent deviation is that a caller must not spin on an
//! empty set (docs/proposals/THREADS.md TD9).
class ComponentWaitableSet {
public:
	ComponentAsyncEvent poll()
	{
		for (ComponentWaitable* waitable : waitables_) {
			ComponentAsyncEvent event;
			if (waitable->takeEvent(event))
				return event;
		}
		return ComponentAsyncEvent{};
	}

	ComponentAsyncEvent wait() { return poll(); }

	void add(ComponentWaitable* waitable)
	{
		if (waitable == nullptr || waitable->waitableSet() == this)
			return;
		remove(waitable);
		waitables_.push_back(waitable);
		waitable->setWaitableSet(this);
	}

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

	void removeAll()
	{
		for (ComponentWaitable* waitable : waitables_)
			waitable->setWaitableSet(nullptr);
		waitables_.clear();
	}

	bool empty() const { return waitables_.empty(); }
	std::size_t size() const { return waitables_.size(); }
	const std::vector<ComponentWaitable*>& waitables() const { return waitables_; }
private:
	std::vector<ComponentWaitable*> waitables_;
};


//! One end of a stream or future. The two ends share a ComponentAsyncChannel.
class ComponentAsyncChannel;

class ComponentAsyncEndpoint : public ComponentWaitable {
public:
	ComponentAsyncEndpoint(ComponentWaitableKind kind, uint32_t handle,
			ComponentEndDirection direction, ComponentAsyncChannel* channel)
		: ComponentWaitable(kind), handle_(handle), direction_(direction),
		  channel_(channel) {}

	uint32_t handle() const { return handle_; }
	ComponentEndDirection direction() const { return direction_; }
	ComponentEndState state() const { return state_; }
	void setState(ComponentEndState state) { state_ = state; }

	ComponentAsyncChannel* channel() const { return channel_; }
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

	bool hasPendingEvent() const { return hasPendingEvent_; }
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
	void clearPendingBuffer()
	{
		pendingMemory_ = nullptr;
		pendingAddress_ = 0;
		pendingCount_ = 0;
		pendingIsRead_ = false;
	}
	bool hasPendingBuffer() const { return pendingMemory_ != nullptr; }
	LinearMemory* pendingMemory() const { return pendingMemory_; }
	uint64_t pendingAddress() const { return pendingAddress_; }
	uint64_t pendingCount() const { return pendingCount_; }
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
class ComponentAsyncChannel {
public:
	explicit ComponentAsyncChannel(uint32_t elementSize, bool isFuture)
		: elementSize_(elementSize), isFuture_(isFuture) {}

	uint32_t elementSize() const { return elementSize_; }
	bool isFuture() const { return isFuture_; }

	bool readableAlive() const { return readableAlive_; }
	bool writableAlive() const { return writableAlive_; }
	void setReadableAlive(bool alive) { readableAlive_ = alive; }
	void setWritableAlive(bool alive) { writableAlive_ = alive; }

	//! Raw byte queue. A stream transfers whole elements; a future transfers at
	//! most one value and then stays `Done`.
	std::deque<std::vector<uint8_t>>& queue() { return queue_; }
	const std::deque<std::vector<uint8_t>>& queue() const { return queue_; }

	bool futureWritten() const { return futureWritten_; }
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
class ComponentExecutor {
public:
	virtual ~ComponentExecutor() = default;

	//! The answers of the `canon` thread built-ins. A single-agent executor
	//! always reports 0 and 1; a multi-agent executor may report per-agent
	//! values.
	virtual uint32_t threadIndex() const = 0;
	virtual uint32_t availableParallelism() const = 0;
	virtual bool singleAgent() const = 0;

	//! Schedule `work` to run on the cooperative queue. It must not run before
	//! `runUntilIdle()` is called.
	virtual void push(std::function<void()> work) = 0;
	virtual void runUntilIdle() = 0;
	virtual std::size_t pending() const = 0;
};

class SingleAgentExecutor final : public ComponentExecutor {
public:
	uint32_t threadIndex() const override { return 0; }
	uint32_t availableParallelism() const override { return 1; }
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

	std::size_t pending() const override { return queue_.size(); }
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
	using HandleAllocator = std::function<uint32_t()>;
	using HandleReleaser = std::function<void(uint32_t)>;

	ComponentAsyncState(HandleAllocator allocate, HandleReleaser release)
		: allocate_(std::move(allocate)), release_(std::move(release)) {}

	//! A stream/future with an optional element size. Returns the readable and
	//! writable handle (the low/high i32 of `{stream,future}.new`).
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
