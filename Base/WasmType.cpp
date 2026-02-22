#include "WasmType.hpp"
namespace WASM {

void ValueType::decode(WasmStream& stream) {
	int8_t tmp_opcode;
	stream >> Elv::Io::Leb(tmp_opcode);
	opcode = static_cast<ValueTypeCode>(tmp_opcode);
	// 0x6B (ref) or 0x6C (ref null)
	if (opcode == ValueTypeCode::Ref || opcode == ValueTypeCode::RefNull) {
		int32_t ht;
		stream >> Elv::Io::Leb(ht);
		this->heapType = ht; // If ht >= 0, it's a TypeIndex
	} else {
		// Standard scalar types: 0x7F (i32), 0x7B (v128), etc.
		this->heapType = -1;
	}
}

void StorageType::decode(WasmStream& stream) {
	int8_t byte;
	stream >> Elv::Io::Leb(byte);

	if (byte == static_cast<int8_t>(ValueTypeCode::I8)) { // i8
		isPacked = true;
		val.opcode = ValueTypeCode::I8;
	} else if (byte == static_cast<int8_t>(ValueTypeCode::I16) ) { // i16
		isPacked = true;
		val.opcode = ValueTypeCode::I16;
	} else {
		isPacked = false;
		// This was actually the opcode for a ValueType.
		// We need to 'put it back' or handle the decode manually.
		val.opcode = static_cast<ValueTypeCode>(byte);
		if (val.opcode == ValueTypeCode::Ref || val.opcode == ValueTypeCode::RefNull) {
			stream >> Elv::Io::Leb(val.heapType);
		} else val.heapType = -1;
	}
}

void Limits::decode(WasmStream& stream) {
	stream >> Elv::Io::Leb(flags);
	stream >> Elv::Io::Leb(initial);
	if (flags & 0x01) {
		uint64_t tmpMaximum;
		stream >> Elv::Io::Leb(tmpMaximum);
		maximum = tmpMaximum;
	} else {
		maximum.reset();
	}
}

void MemoryType::decode(WasmStream& stream)
{
	limits.decode(stream);
	if (limits.flags & 0x04)
		indexType = MemoryIndexType::I64;
	else
		indexType = MemoryIndexType::I32;
}

void PreparedFunctionStack::prepare(const Subtype& type, const FunctionBody& body) {
	if(!type.isFunction()) return; // Quick safety check
	// 1. Get the signature (assuming it's a FuncType)
	const auto& sig = std::get<FuncType>(type.composite);
	parameterCount = sig.params.size();

	// 2. Add Returns and Parameters first
	for (const auto& p : sig.results) {
		allReturns.push_back(p.val);
	}
	for (const auto& p : sig.params) {
		allLocals.push_back(p.val);
	}

	// 3. Add flattened Locals
	for (const auto& localGroup : body.locals) {
		for (uint32_t i = 0; i < localGroup.count; ++i) {
			allLocals.push_back(localGroup.type);
		}
	}
	localCount = allLocals.size() - parameterCount;
}

}