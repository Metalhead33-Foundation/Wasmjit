// Wasm tracing hook (docs/proposals/INSTRUMENT_TRACING.md).
//
// The proposal assigns no binary encoding yet. The engine uses a documented
// provisional encoding (`0xFC 0x100`) and exposes the immediate to an optional
// compile-time callback. The instruction itself lowers to nothing at run time,
// so enabling or disabling the callback cannot change a result.
#ifndef WASMTRACING_HPP
#define WASMTRACING_HPP
#include <cstdint>
namespace WASM {

using TraceCallback = void (*)(uint32_t immediate);

inline TraceCallback& traceCallbackSlot()
{
	static TraceCallback callback = nullptr;
	return callback;
}

inline void setTraceCallback(TraceCallback callback)
{
	traceCallbackSlot() = callback;
}

inline void traceImmediate(uint32_t immediate)
{
	if (TraceCallback callback = traceCallbackSlot())
		callback(immediate);
}

} // namespace WASM
#endif // WASMTRACING_HPP
