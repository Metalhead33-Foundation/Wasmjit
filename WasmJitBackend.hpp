#ifndef WASMJITBACKEND_H
#define WASMJITBACKEND_H
#include <wabt/binary-reader-ir.h>  // WABT for parsing
#include <wabt/ir.h>

// Abstract JIT backend interface
class WasmJitBackend {
public:
	virtual ~WasmJitBackend() = default;
	virtual void* compile(const wabt::ExprList& expr) = 0;  // Compile WASM expr to native fn
	virtual void execute(void* code, void* stack) = 0;
};

#endif // WASMJITBACKEND_H
