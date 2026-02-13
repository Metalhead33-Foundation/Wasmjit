#include "WasmType.hpp"
namespace WASM {

void ValueType::decode(WasmStream& stream) {
	stream >> opcode;
	// 0x6B (ref) or 0x6C (ref null)
	if (opcode == 0x6B || opcode == 0x6C) {
		int32_t ht;
		stream >> Elv::Io::Leb(ht);
		this->heapType = ht; // If ht >= 0, it's a TypeIndex
	} else {
		// Standard scalar types: 0x7F (i32), 0x7B (v128), etc.
		this->heapType = -1;
	}
}

void StorageType::decode(WasmStream& stream) {
	uint8_t byte;
	stream >> byte;

	if (byte == 0x78) { // i8
		isPacked = true;
		val.opcode = 0x78;
	} else if (byte == 0x77) { // i16
		isPacked = true;
		val.opcode = 0x77;
	} else {
		isPacked = false;
		// This was actually the opcode for a ValueType.
		// We need to 'put it back' or handle the decode manually.
		val.opcode = byte;
		if (byte == 0x6B || byte == 0x6C) {
			stream >> Elv::Io::Leb(val.heapType);
		}
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

}