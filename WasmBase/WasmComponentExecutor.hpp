#ifndef WASMCOMPONENTEXECUTOR_HPP
#define WASMCOMPONENTEXECUTOR_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentExecutor.hpp
  \brief The embedder-owned scheduling seam for Component Model async work.

  This header is part of the embedder surface. The engine ships a synchronous
  single-agent executor; a multi-agent embedder supplies its own
  `ComponentExecutor` and passes it to `ComponentInstance::setExecutor`. The
  engine never spawns threads itself (docs/proposals/THREADS.md TH-5).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace WASM {

/** \brief Scheduling interface used by component async operations. */
class ComponentExecutor {
public:
	virtual ~ComponentExecutor() = default;

	/** \brief The answer of `canon thread.index`. \return The current agent index. */
	virtual uint32_t threadIndex() const = 0;
	/** \brief The answer of `canon thread.available-parallelism`. \return Concurrent agent capacity. */
	virtual uint32_t availableParallelism() const = 0;
	/** \brief True when this executor admits one agent only. \return true for the single-agent mode. */
	virtual bool singleAgent() const = 0;
	/** \brief Schedules work on the cooperative queue. \param work Item to run later. */
	virtual void push(std::function<void()> work) = 0;
	/** \brief Drains every queued item, including items they enqueue. */
	virtual void runUntilIdle() = 0;
	/** \brief Returns the number of queued items. \return Pending work count. */
	virtual std::size_t pending() const = 0;

	/** \brief Cooperative yield: lets every queued item run. */
	void yield() { runUntilIdle(); }
	/** \brief Enqueues one agent entry point; the executor decides which OS thread runs it. \param work Agent entry point. */
	void spawnAgent(std::function<void()> work) { push(std::move(work)); }
};

/** \brief Synchronous executor implementing the documented single-agent mode. */
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
		while (!queue_.empty()) {
			std::function<void()> work = std::move(queue_.front());
			queue_.pop_front();
			work();
		}
	}

	std::size_t pending() const override { return queue_.size(); }

private:
	std::deque<std::function<void()>> queue_; //!< Cooperative work queue.
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTEXECUTOR_HPP
