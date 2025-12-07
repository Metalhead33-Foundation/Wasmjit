#include "WasmLibjit.hpp"
#include <vector>

jit_type_t WasmLibjit::createSignature(const std::span<const wabt::Type::Enum>& paramTypes, wabt::Type::Enum retType)
{
	std::vector<jit_type_t> fullParams;
	fullParams.push_back(jit_type_void_ptr);  // Implicit stack/memory ptr as first param
	for (auto ty : paramTypes) {
		fullParams.push_back(convertBuiltinType(ty));
	}
	return jit_type_create_signature(jit_abi_cdecl, convertBuiltinType(retType), fullParams.data(), fullParams.size(), 1);
}

jit_function_t WasmLibjit::buildUncompiledFunction(const wabt::ExprList& expr)
{
	jit_function_t retFun = nullptr;
	doWithinContext([this,&expr,&retFun](jit_context_t ctx) {
		// Hello, context!
		// Time to, uh, assemble our function, right?
	});
	return retFun;
}

jit_function_t WasmLibjit::buildUncompiledFunction(const wabt::Func* wasm_func)
{
	jit_function_t retFun = nullptr;
	doWithinContext([this,wasm_func,&retFun](jit_context_t ctx) {
		unsigned paramNum = wasm_func->GetNumParams();
		std::vector<jit_type_t> parameterTypes(paramNum);
		for(unsigned i = 0; i < paramNum; ++i)
		{
			auto typo = wasm_func->GetParamType(i);
			parameterTypes[i] = convertBuiltinType(typo);
		}
		// Hello, context!
		// Time to, uh, assemble our function, right?
	});
	return retFun;
}

jit_value_t WasmLibjit::emitExprs(jit_function_t func, const wabt::ExprList& exprs, std::vector<jit_value_t>& opStack)
{
	for (const auto& expr : exprs) {
		// Hello
		switch (expr.type()) {
			case wabt::ExprType::AtomicLoad: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicLoadExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicRmw: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicRmwCmpxchg: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwCmpxchgExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicStore: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicStoreExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicNotify: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicNotifyExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicFence: {
				const auto& converted_expr = dynamic_cast<const wabt::AtomicFenceExpr&>(expr);
				break;
			}
			case wabt::ExprType::AtomicWait: { const auto& converted_expr = dynamic_cast<const wabt::AtomicWaitExpr&>(expr); break; }
			case wabt::ExprType::Binary: {
				const auto& bin_expr = dynamic_cast<const wabt::BinaryExpr&>(expr);
				emitOpt(func, bin_expr.opcode, OperatorType::BINARY);
				break;
			}
			case wabt::ExprType::Block: { const auto& converted_expr = dynamic_cast<const wabt::BlockExpr&>(expr); break; }
			case wabt::ExprType::Br: { const auto& converted_expr = dynamic_cast<const wabt::BrExpr&>(expr); break; }
			case wabt::ExprType::BrIf: { const auto& converted_expr = dynamic_cast<const wabt::BrIfExpr&>(expr); break; }
			case wabt::ExprType::BrTable: { const auto& converted_expr = dynamic_cast<const wabt::BrTableExpr&>(expr); break; }
			case wabt::ExprType::Call: { const auto& converted_expr = dynamic_cast<const wabt::CallExpr&>(expr); break; }
			case wabt::ExprType::CallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::CallIndirectExpr&>(expr); break; }
			case wabt::ExprType::CallRef: { const auto& converted_expr = dynamic_cast<const wabt::CallRefExpr&>(expr); break; }
			case wabt::ExprType::CodeMetadata: { const auto& converted_expr = dynamic_cast<const wabt::CodeMetadataExpr&>(expr); break; }
			case wabt::ExprType::Compare: {
				const auto& bin_expr = dynamic_cast<const wabt::CompareExpr&>(expr);
				emitOpt(func, bin_expr.opcode, OperatorType::COMPARE);
				break;
			}
			case wabt::ExprType::Const: {
				const auto& const_expr = dynamic_cast<const wabt::ConstExpr&>(expr);
				emitConst(func, const_expr.const_);
				break;
			}
			case wabt::ExprType::Convert: {
				const auto& bin_expr = dynamic_cast<const wabt::ConvertExpr&>(expr);
				emitOpt(func, bin_expr.opcode, OperatorType::CONVERT);
				break;
			}
			case wabt::ExprType::Drop: { const auto& converted_expr = dynamic_cast<const wabt::DropExpr&>(expr); break; }
			case wabt::ExprType::GlobalGet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalGetExpr&>(expr); break; }
			case wabt::ExprType::GlobalSet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalSetExpr&>(expr); break; }
			case wabt::ExprType::If: { const auto& converted_expr = dynamic_cast<const wabt::IfExpr&>(expr); break; }
			case wabt::ExprType::Load: { const auto& converted_expr = dynamic_cast<const wabt::LoadExpr&>(expr); break; }
			case wabt::ExprType::LocalGet: { const auto& converted_expr = dynamic_cast<const wabt::LocalGetExpr&>(expr); break; }
			case wabt::ExprType::LocalSet: { const auto& converted_expr = dynamic_cast<const wabt::LocalSetExpr&>(expr); break; }
			case wabt::ExprType::LocalTee: { const auto& converted_expr = dynamic_cast<const wabt::LocalTeeExpr&>(expr); break; }
			case wabt::ExprType::Loop: { const auto& converted_expr = dynamic_cast<const wabt::LoopExpr&>(expr); break; }
			case wabt::ExprType::MemoryCopy: { const auto& converted_expr = dynamic_cast<const wabt::MemoryCopyExpr&>(expr); break; }
			case wabt::ExprType::DataDrop: { const auto& converted_expr = dynamic_cast<const wabt::DataDropExpr&>(expr); break; }
			case wabt::ExprType::MemoryFill: { const auto& converted_expr = dynamic_cast<const wabt::MemoryFillExpr&>(expr); break; }
			case wabt::ExprType::MemoryGrow: { const auto& converted_expr = dynamic_cast<const wabt::MemoryGrowExpr&>(expr); break; }
			case wabt::ExprType::MemoryInit: { const auto& converted_expr = dynamic_cast<const wabt::MemoryInitExpr&>(expr); break; }
			case wabt::ExprType::MemorySize: { const auto& converted_expr = dynamic_cast<const wabt::MemorySizeExpr&>(expr); break; }
			case wabt::ExprType::Nop: {
				jit_insn_nop(func);
				break;
			}
			case wabt::ExprType::RefIsNull: { const auto& converted_expr = dynamic_cast<const wabt::RefIsNullExpr&>(expr); break; }
			case wabt::ExprType::RefFunc: { const auto& converted_expr = dynamic_cast<const wabt::RefFuncExpr&>(expr); break; }
			case wabt::ExprType::RefNull: { const auto& converted_expr = dynamic_cast<const wabt::RefNullExpr&>(expr); break; }
			case wabt::ExprType::Rethrow: { const auto& converted_expr = dynamic_cast<const wabt::RethrowExpr&>(expr); break; }
			case wabt::ExprType::Return: { const auto& converted_expr = dynamic_cast<const wabt::ReturnExpr&>(expr); break; }
			case wabt::ExprType::ReturnCall: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallExpr&>(expr); break; }
			case wabt::ExprType::ReturnCallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallIndirectExpr&>(expr); break; }
			case wabt::ExprType::Select: { const auto& converted_expr = dynamic_cast<const wabt::SelectExpr&>(expr); break; }
			case wabt::ExprType::SimdLaneOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdLaneOpExpr&>(expr); break; }
			case wabt::ExprType::SimdLoadLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdLoadLaneExpr&>(expr); break; }
			case wabt::ExprType::SimdStoreLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdStoreLaneExpr&>(expr); break; }
			case wabt::ExprType::SimdShuffleOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdShuffleOpExpr&>(expr); break; }
			case wabt::ExprType::LoadSplat: { const auto& converted_expr = dynamic_cast<const wabt::LoadSplatExpr&>(expr); break; }
			case wabt::ExprType::LoadZero: { const auto& converted_expr = dynamic_cast<const wabt::LoadZeroExpr&>(expr); break; }
			case wabt::ExprType::Store: { const auto& converted_expr = dynamic_cast<const wabt::StoreExpr&>(expr); break; }
			case wabt::ExprType::TableCopy: { const auto& converted_expr = dynamic_cast<const wabt::TableCopyExpr&>(expr); break; }
			case wabt::ExprType::ElemDrop: { const auto& converted_expr = dynamic_cast<const wabt::ElemDropExpr&>(expr); break; }
			case wabt::ExprType::TableInit: { const auto& converted_expr = dynamic_cast<const wabt::TableInitExpr&>(expr); break; }
			case wabt::ExprType::TableGet: { const auto& converted_expr = dynamic_cast<const wabt::TableGetExpr&>(expr); break; }
			case wabt::ExprType::TableGrow: { const auto& converted_expr = dynamic_cast<const wabt::TableGrowExpr&>(expr); break; }
			case wabt::ExprType::TableSize: { const auto& converted_expr = dynamic_cast<const wabt::TableSizeExpr&>(expr); break; }
			case wabt::ExprType::TableSet: { const auto& converted_expr = dynamic_cast<const wabt::TableSetExpr&>(expr); break; }
			case wabt::ExprType::TableFill: { const auto& converted_expr = dynamic_cast<const wabt::TableFillExpr&>(expr); break; }
			case wabt::ExprType::Ternary: {
				const auto& bin_expr = dynamic_cast<const wabt::TernaryExpr&>(expr);
				emitOpt(func, bin_expr.opcode, OperatorType::TERNARY);
				break;
			}
			case wabt::ExprType::Throw: { const auto& converted_expr = dynamic_cast<const wabt::ThrowExpr&>(expr); break; }
			case wabt::ExprType::ThrowRef: { const auto& converted_expr = dynamic_cast<const wabt::ThrowRefExpr&>(expr); break; }
			case wabt::ExprType::Try: { const auto& converted_expr = dynamic_cast<const wabt::TryExpr&>(expr); break; }
			case wabt::ExprType::TryTable: { const auto& converted_expr = dynamic_cast<const wabt::TryTableExpr&>(expr); break; }
			case wabt::ExprType::Unary: {
				const auto& bin_expr = dynamic_cast<const wabt::UnaryExpr&>(expr);
				emitOpt(func, bin_expr.opcode, OperatorType::UNARY);
				break;
			}
			case wabt::ExprType::Unreachable: {
				jit_insn_nop(func);
				break;
			}
		}
	}
}

void WasmLibjit::emitConst(jit_function_t func, const wabt::Const& constant)
{
	auto type = convertBuiltinType(constant.type());
}

void WasmLibjit::emitOpt(jit_function_t func, const wabt::Opcode& opcode, WasmLibjit::OperatorType optype)
{

}

WasmLibjit::WasmLibjit()
{
	context = jit_context_create();
	jit_type_t float32_types[] = {jit_type_float32,jit_type_float32,jit_type_float32,jit_type_float32};
	v128_type = jit_type_create_struct(float32_types, 4, 1);
}

WasmLibjit::~WasmLibjit()
{
	jit_context_destroy(context);
}

jit_type_t WasmLibjit::convertBuiltinType(wabt::Type::Enum wabtBuiltinType)
{
	// I guess I will need to setup
	switch (wabtBuiltinType) {
		case wabt::Type::I32: return jit_type_int;
		case wabt::Type::I64: return jit_type_long;
		case wabt::Type::F32: return jit_type_float32;
		case wabt::Type::F64: return jit_type_float64;
		case wabt::Type::V128: return v128_type;
		case wabt::Type::I8: return jit_type_sbyte;
		case wabt::Type::I16: return jit_type_short;
		case wabt::Type::ExnRef: return jit_type_void_ptr;
		case wabt::Type::FuncRef: return jit_type_void_ptr;
		case wabt::Type::ExternRef: return jit_type_void_ptr;
		case wabt::Type::Reference: return jit_type_void_ptr;
		case wabt::Type::Func: return jit_type_void_ptr;
		case wabt::Type::Struct: return jit_type_void_ptr;
		case wabt::Type::Array: return jit_type_void_ptr;
		case wabt::Type::Void: return jit_type_void;
		case wabt::Type::Any: return jit_type_void_ptr;
		case wabt::Type::I8U: return jit_type_ubyte;
		case wabt::Type::I16U:return jit_type_ushort;
		case wabt::Type::I32U: return jit_type_uint;
			break;
	}
}

void WasmLibjit::doWithinContext(const LockedContextFunction& toDo)
{
	jit_context_build_start(context);
	toDo(context);
	jit_context_build_end(context);
}


void* WasmLibjit::compile(const wabt::ExprList& expr)
{
	auto func = buildUncompiledFunction(expr);
	if(func) {
		// Yello
		return jit_function_to_closure(func);
	}
	else return nullptr;
}

void WasmLibjit::execute(void* code, void* stack)
{
}
