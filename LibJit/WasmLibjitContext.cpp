#include "WasmLibjitContext.hpp"

WasmLibjitContext::WasmLibjitContext() {}

jit_type_t WasmLibjitContext::convertBuiltinType(wabt::Type::Enum wabtBuiltinType) {
	switch (wabtBuiltinType) {
		case wabt::Type::I32:      return jit_type_int;      // signed 32
		case wabt::Type::I64:      return jit_type_long;     // signed 64
		case wabt::Type::F32:      return jit_type_float32;
		case wabt::Type::F64:      return jit_type_float64;
		case wabt::Type::I8:       return jit_type_sbyte;    // signed 8 (packed)
		case wabt::Type::I16:      return jit_type_short;    // signed 16 (packed)
		case wabt::Type::I8U:      return jit_type_ubyte;    // unsigned 8
		case wabt::Type::I16U:     return jit_type_ushort;   // unsigned 16
		case wabt::Type::I32U:     return jit_type_uint;     // unsigned 32
		case wabt::Type::V128:     return v128_type;         // TODO: Define as struct/array type
		case wabt::Type::FuncRef:
		case wabt::Type::ExternRef:
		case wabt::Type::ExnRef:
		case wabt::Type::Reference: return jit_type_void_ptr; // Treat refs as opaque pointers
		case wabt::Type::Void:     return jit_type_void;
		default:
			// Error: Unhandled type (e.g., Struct/Array for GC proposals)
			assert(false && "Unsupported WABT type for constant");
			return jit_type_void;
	}
}

size_t WasmLibjitContext::createConstant(const wabt::Const& constant) {
	wabt::Type::Enum wabt_type = constant.type();
	jit_type_t jit_type = convertBuiltinType(wabt_type);
	jit_constant_t jit_const = {jit_type, {}};
	const auto& data = constant;  // Alias for brevity

	switch (wabt_type) {
		case wabt::Type::I32:
			jit_const.un.int_value = static_cast<jit_int>(data.u32());  // Reinterpret bits as signed
			break;
		case wabt::Type::I64:
			jit_const.un.long_value = static_cast<jit_long>(data.u64());  // Reinterpret as signed
			break;
		case wabt::Type::F32: {
			uint32_t bits = data.f32_bits();
			memcpy(&jit_const.un.float32_value, &bits, sizeof(bits));
			break;
		}
		case wabt::Type::F64: {
			uint64_t bits = data.f64_bits();
			memcpy(&jit_const.un.float64_value, &bits, sizeof(bits));
			break;
		}
		case wabt::Type::I8: {  // Packed signed 8: sign-extend low byte to 32-bit int
			int8_t val = static_cast<int8_t>(data.u32() & 0xFF);
			jit_const.un.int_value = static_cast<jit_int>(val);
			break;
		}
		case wabt::Type::I16: {  // Packed signed 16: sign-extend low word
			int16_t val = static_cast<int16_t>(data.u32() & 0xFFFF);
			jit_const.un.int_value = static_cast<jit_int>(val);
			break;
		}
		case wabt::Type::I8U: {  // Unsigned 8: zero-extend low byte
			uint8_t val = static_cast<uint8_t>(data.u32() & 0xFF);
			jit_const.un.uint_value = static_cast<jit_uint>(val);
			break;
		}
		case wabt::Type::I16U: {  // Unsigned 16: zero-extend low word
			uint16_t val = static_cast<uint16_t>(data.u32() & 0xFFFF);
			jit_const.un.uint_value = static_cast<jit_uint>(val);
			break;
		}
		case wabt::Type::I32U:  // Unsigned 32
			jit_const.un.uint_value = static_cast<jit_uint>(data.u32());
			break;
		case wabt::Type::FuncRef:
		case wabt::Type::ExternRef:
		case wabt::Type::ExnRef:  // Refs: opaque pointer from bits
			jit_const.un.ptr_value = reinterpret_cast<void*>(data.ref_bits());
			break;
		case wabt::Type::Reference:  // Null ref
			if (data.ref_bits() == wabt::Const::kRefNullBits) {
				jit_const.un.ptr_value = nullptr;
			} else {
				jit_const.un.ptr_value = reinterpret_cast<void*>(data.ref_bits());
			}
			break;
		case wabt::Type::V128:
			v128_constants.push_back({data.v128_lane<uint64_t>(0), data.v128_lane<uint64_t>(1)});
			jit_const.un.uint_value = v128_constants.size() - 1;
			break;  // As in your original
		default:
			// Unhandled (e.g., Void/Any/Struct): skip or error
			break;
	}

	// Handle expected NaN if needed (for testing; ignore in prod JIT)
	if (data.is_expected_nan()) {
		// TODO: Set NaN payload in float union if testing assertions
	}

	tempConstants.emplace_back(jit_const);
	return tempConstants.size() - 1;
}

jit_value_t WasmLibjitContext::createPointer(void* ptr)
{
	jit_constant_t jit_const = {jit_type_void_ptr, {}};
	jit_const.un.ptr_value = ptr;
	return jit_value_create_constant(functionId, &jit_const);
}

void WasmLibjitContext::handleBlock(const wabt::BlockExpr& expr)
{
}

void WasmLibjitContext::handleBr(const wabt::BrExpr& expr)
{
}

void WasmLibjitContext::handleBrIf(const wabt::BrIfExpr& expr)
{
}

void WasmLibjitContext::handleBrTable(const wabt::BrTableExpr& expr)
{
}

void WasmLibjitContext::handleCall(const wabt::CallExpr& expr)
{
}

void WasmLibjitContext::handleCallIndirect(const wabt::CallIndirectExpr& expr)
{
}

void WasmLibjitContext::handleCallRef(const wabt::CallRefExpr& expr)
{
}

void WasmLibjitContext::handleCodeMetadata(const wabt::CodeMetadataExpr& expr)
{
}


void WasmLibjitContext::handleConst(const wabt::ConstExpr& expr)
{
	size_t constId = createConstant(expr.const_);
	offsetToConstId[expr.loc.offset] = constId;
}


void WasmLibjitContext::handleDrop(const wabt::DropExpr& expr)
{
}

void WasmLibjitContext::handleGlobalGet(const wabt::GlobalGetExpr& expr)
{
}

void WasmLibjitContext::handleGlobalSet(const wabt::GlobalSetExpr& expr)
{
}

void WasmLibjitContext::handleIf(const wabt::IfExpr& expr)
{
}

void WasmLibjitContext::handleLocalGet(const wabt::LocalGetExpr& expr)
{
}

void WasmLibjitContext::handleLocalSet(const wabt::LocalSetExpr& expr)
{
}

void WasmLibjitContext::handleLocalTee(const wabt::LocalTeeExpr& expr)
{
}

void WasmLibjitContext::handleLoop(const wabt::LoopExpr& expr)
{
}

void WasmLibjitContext::handleMemoryCopy(const wabt::MemoryCopyExpr& expr)
{
}

void WasmLibjitContext::handleDataDrop(const wabt::DataDropExpr& expr)
{
}

void WasmLibjitContext::handleMemoryFill(const wabt::MemoryFillExpr& expr)
{
}

void WasmLibjitContext::handleMemoryGrow(const wabt::MemoryGrowExpr& expr)
{
}

void WasmLibjitContext::handleMemoryInit(const wabt::MemoryInitExpr& expr)
{
}

void WasmLibjitContext::handleMemorySize(const wabt::MemorySizeExpr& expr)
{
}

void WasmLibjitContext::handleNop(const wabt::NopExpr& expr)
{
	jit_insn_nop(functionId);
}

void WasmLibjitContext::handleRefIsNull(const wabt::RefIsNullExpr& expr)
{
}

void WasmLibjitContext::handleRefFunc(const wabt::RefFuncExpr& expr)
{
}

void WasmLibjitContext::handleRefNull(const wabt::RefNullExpr& expr)
{
}

void WasmLibjitContext::handleRethrow(const wabt::RethrowExpr& expr)
{
}

void WasmLibjitContext::handleReturn(const wabt::ReturnExpr& expr)
{
	jit_insn_return(functionId, 0);
}

void WasmLibjitContext::handleReturnCall(const wabt::ReturnCallExpr& expr)
{
}

void WasmLibjitContext::handleReturnCallIndirect(const wabt::ReturnCallIndirectExpr& expr)
{
}

void WasmLibjitContext::handleSelect(const wabt::SelectExpr& expr)
{
}

void WasmLibjitContext::handleSimdLaneOp(const wabt::SimdLaneOpExpr& expr)
{
}

void WasmLibjitContext::handleSimdLoadLane(const wabt::SimdLoadLaneExpr& expr)
{
}

void WasmLibjitContext::handleSimdStoreLane(const wabt::SimdStoreLaneExpr& expr)
{
}

void WasmLibjitContext::handleSimdShuffleOp(const wabt::SimdShuffleOpExpr& expr)
{
}

void WasmLibjitContext::handleTableCopy(const wabt::TableCopyExpr& expr)
{
}

void WasmLibjitContext::handleElemDrop(const wabt::ElemDropExpr& expr)
{
}

void WasmLibjitContext::handleTableInit(const wabt::TableInitExpr& expr)
{
}

void WasmLibjitContext::handleTableGet(const wabt::TableGetExpr& expr)
{
}

void WasmLibjitContext::handleTableGrow(const wabt::TableGrowExpr& expr)
{
}

void WasmLibjitContext::handleTableSize(const wabt::TableSizeExpr& expr)
{
}

void WasmLibjitContext::handleTableSet(const wabt::TableSetExpr& expr)
{
}

void WasmLibjitContext::handleTableFill(const wabt::TableFillExpr& expr)
{
}

void WasmLibjitContext::handleTernary(const wabt::TernaryExpr& expr)
{
}

void WasmLibjitContext::handleThrow(const wabt::ThrowExpr& expr)
{
}

void WasmLibjitContext::handleThrowRef(const wabt::ThrowRefExpr& expr)
{
}

void WasmLibjitContext::handleTry(const wabt::TryExpr& expr)
{
}

void WasmLibjitContext::handleTryTable(const wabt::TryTableExpr& expr)
{
}

void WasmLibjitContext::handleUnary(const wabt::UnaryExpr& expr)
{
	switch (expr.opcode) {
		case wabt::Opcode::I32Clz:    /* handle clz */ break;
		case wabt::Opcode::I32Ctz:    /* handle ctz */ break;
		case wabt::Opcode::I32Popcnt: /* handle popcnt */ break;
		case wabt::Opcode::I64Clz:    /* handle clz */ break;
		case wabt::Opcode::I64Ctz:    /* handle ctz */ break;
		case wabt::Opcode::I64Popcnt: /* handle popcnt */ break;

		// Core Unary Float Ops
		case wabt::Opcode::F32Abs:      /* handle abs */ break;
		case wabt::Opcode::F32Neg:      /* handle neg */ break;
		case wabt::Opcode::Invalid:
			break;
		default: break;
	}
}

void WasmLibjitContext::handleUnreachable(const wabt::UnreachableExpr& expr)
{
}

void WasmLibjitContext::betweenStages()
{
	for(auto& it : tempConstants)
	{
		if(it.type == v128_type)
		{
			auto index = it.un.uint_value;
			void* ptr = &v128_constants[index];
			it.un.ptr_value = ptr;
		}
	}
	jitConstants.reserve(tempConstants.size());
	for(const auto& it : tempConstants)
	{
		jitConstants.push_back(jit_value_create_constant(functionId, &it));
	}
	tempConstants.clear();
	// jitConstants
	// TODO
}

WasmJitContext::CompilationType WasmLibjitContext::getContextType() const
{
	return TWO_STAGE_CONST_FIRST;
}


void WasmLibjitContext::handleOperator(const wabt::Location& loc, const wabt::Opcode& opcode, OperatorType operatorType)
{
}

void WasmLibjitContext::handleLoadStore(const wabt::Location& loc, const wabt::Var& memidx, const wabt::Opcode& opcode, const wabt::Address& align, const wabt::Address& offset, LoadStoreType operatorType)
{
	void* linMemOffset = &linMemAddr[offset];
	auto memAddrConstant = createPointer(linMemOffset);
	switch (opcode) {
		case wabt::Opcode::I32Load:
		{
			stack.push(jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_int));
			break;
		}
		case wabt::Opcode::I64Load:
		{
			stack.push(jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_long));
			break;
		}
		case wabt::Opcode::F32Load:
		{
			stack.push(jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_float32));
			break;
		}
		case wabt::Opcode::F64Load:
		{
			stack.push(jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_float64));
			break;
		}
		case wabt::Opcode::I32Load8S:
		{
			auto tmpVar = jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_sbyte);
			stack.push(jit_insn_convert(functionId, tmpVar, jit_type_int, 0));
			break;
		}
		case wabt::Opcode::I32Load8U:
		{
			auto tmpVar = jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_ubyte);
			stack.push(jit_insn_convert(functionId, tmpVar, jit_type_int, 0));
			break;
		}
		case wabt::Opcode::I32Load16S:
		{
			auto tmpVar = jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_short);
			stack.push(jit_insn_convert(functionId, tmpVar, jit_type_int, 0));
			break;
		}
		case wabt::Opcode::I32Load16U:
		{
			auto tmpVar = jit_insn_load_elem_address(functionId, memAddrConstant, 0, jit_type_ushort);
			stack.push(jit_insn_convert(functionId, tmpVar, jit_type_int, 0));
			break;
		}
		case wabt::Opcode::I64Load8S:
		case wabt::Opcode::I64Load8U:
		case wabt::Opcode::I64Load16S:
		case wabt::Opcode::I64Load16U:
		case wabt::Opcode::I64Load32S:
		case wabt::Opcode::I64Load32U:
		case wabt::Opcode::I32Store:
		case wabt::Opcode::I64Store:
		case wabt::Opcode::F32Store:
		case wabt::Opcode::F64Store:
		case wabt::Opcode::I32Store8:
		case wabt::Opcode::I32Store16:
		case wabt::Opcode::I64Store8:
		case wabt::Opcode::I64Store16:
		case wabt::Opcode::I64Store32:
		case wabt::Opcode::V128Load:
		case wabt::Opcode::V128Load8X8S:
		case wabt::Opcode::V128Load8X8U:
		case wabt::Opcode::V128Load16X4S:
		case wabt::Opcode::V128Load16X4U:
		case wabt::Opcode::V128Load32X2S:
		case wabt::Opcode::V128Load32X2U:
		case wabt::Opcode::V128Load8Splat:
		case wabt::Opcode::V128Load16Splat:
		case wabt::Opcode::V128Load32Splat:
		case wabt::Opcode::V128Load64Splat:
		case wabt::Opcode::V128Store:
		case wabt::Opcode::V128Load8Lane:
		case wabt::Opcode::V128Load16Lane:
		case wabt::Opcode::V128Load32Lane:
		case wabt::Opcode::V128Load64Lane:
		case wabt::Opcode::V128Store8Lane:
		case wabt::Opcode::V128Store16Lane:
		case wabt::Opcode::V128Store32Lane:
		case wabt::Opcode::V128Store64Lane:
		case wabt::Opcode::V128Load32Zero:
		case wabt::Opcode::V128Load64Zero:
		case wabt::Opcode::I32AtomicLoad:
		case wabt::Opcode::I64AtomicLoad:
		case wabt::Opcode::I32AtomicLoad8U:
		case wabt::Opcode::I32AtomicLoad16U:
		case wabt::Opcode::I64AtomicLoad8U:
		case wabt::Opcode::I64AtomicLoad16U:
		case wabt::Opcode::I64AtomicLoad32U:
		case wabt::Opcode::I32AtomicStore:
		case wabt::Opcode::I64AtomicStore:
		case wabt::Opcode::I32AtomicStore8:
		case wabt::Opcode::I32AtomicStore16:
		case wabt::Opcode::I64AtomicStore8:
		case wabt::Opcode::I64AtomicStore16:
		case wabt::Opcode::I64AtomicStore32:
		case wabt::Opcode::Invalid:
			break;
		default: break;
	}
}
