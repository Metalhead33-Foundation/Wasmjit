#ifndef WASMLIBJIT_HPP
#define WASMLIBJIT_HPP
#include "../WasmJitBackend.hpp"
#include <jit/jit.h>
#include <functional>
#include <span>

/*
using BinaryExpr = OpcodeExpr<ExprType::Binary>;
using CompareExpr = OpcodeExpr<ExprType::Compare>;
using ConvertExpr = OpcodeExpr<ExprType::Convert>;
using UnaryExpr = OpcodeExpr<ExprType::Unary>;
using TernaryExpr = OpcodeExpr<ExprType::Ternary>;
*/

class WasmLibjit : public WasmJitBackend
{
public:
	enum OperatorType : uint8_t {
		BINARY,
		COMPARE,
		CONVERT,
		UNARY,
		TERNARY
	};
	typedef std::function<void(jit_context_t)> LockedContextFunction;
private:
	WasmLibjit(const WasmLibjit& cpy) = delete;
	WasmLibjit& operator=(const WasmLibjit& cpy) = delete;
	// Variables
	jit_context_t context;
	jit_type_t v128_type;

	// Private functions
	jit_type_t createSignature(const std::span<const wabt::Type::Enum>& paramTypes, wabt::Type::Enum retType);
	jit_function_t buildUncompiledFunction(const wabt::ExprList& expr);
	jit_function_t buildUncompiledFunction(const wabt::Func* wasm_func);
	jit_value_t emitExprs(jit_function_t func, const wabt::ExprList& exprs, std::vector<jit_value_t>& opStack);
	void emitConst(jit_function_t func, const wabt::Const& constant);
	void emitOpt(jit_function_t func, const wabt::Opcode& opcode, OperatorType optype);
public:
	WasmLibjit();
	~WasmLibjit() override;
	jit_type_t convertBuiltinType(wabt::Type::Enum wabtBuiltinType);
	void doWithinContext(const LockedContextFunction& toDo);
	void* compile(const wabt::ExprList& expr) override;
	void execute(void* code, void* stack) override;
};

#endif // WASMLIBJIT_HPP
