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
	uint8_t prefix;
	stream >> prefix;
	stream.device.seek(-1, Elv::Io::SeekOrigin::SET);
	if (prefix == 0x78 || prefix == 0x77) { // i8, i16
		stream >> val.opcode;
		isPacked = true;
	} else {
		val.decode(stream);
		isPacked = false;
	}
}

void Limits::decode(WasmStream& stream) {
	stream >> Elv::Io::Leb(flags);
	stream >> Elv::Io::Leb(initial);
	if (flags & 0x01) {
		stream >> Elv::Io::Leb(maximum);
	}
}

}