#include "WasmComponentRuntime.hpp"
#include "WasmRegistryImportResolver.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <utf8cpp/utf8.h>
#include <ffi.h>
#include <stdexcept>

namespace WASM {
namespace {

uint64_t nextComponentAbiInstanceId()
{
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1, std::memory_order_relaxed);
}

union AbiSlot {
	int32_t i32;
	int64_t i64;
	float f32;
	double f64;
	uint64_t raw;
};

ffi_type* ffiType(WASM::AbiCoreType type)
{
	switch (type) {
	case WASM::AbiCoreType::I32: return &ffi_type_sint32;
	case WASM::AbiCoreType::I64: return &ffi_type_sint64;
	case WASM::AbiCoreType::F32: return &ffi_type_float;
	case WASM::AbiCoreType::F64: return &ffi_type_double;
	}
	throw std::runtime_error("component runtime: unknown ABI core type");
}

WASM::ValueTypeCode valueKind(WASM::AbiCoreType type)
{
	switch (type) {
	case WASM::AbiCoreType::I32: return WASM::ValueTypeCode::I32;
	case WASM::AbiCoreType::I64: return WASM::ValueTypeCode::I64;
	case WASM::AbiCoreType::F32: return WASM::ValueTypeCode::F32;
	case WASM::AbiCoreType::F64: return WASM::ValueTypeCode::F64;
	}
	throw std::runtime_error("component runtime: unknown ABI core type");
}

WASM::AbiCoreType abiCoreTypeForValue(WASM::ValueTypeCode code)
{
	switch (code) {
	case WASM::ValueTypeCode::I32: return WASM::AbiCoreType::I32;
	case WASM::ValueTypeCode::I64: return WASM::AbiCoreType::I64;
	case WASM::ValueTypeCode::F32: return WASM::AbiCoreType::F32;
	case WASM::ValueTypeCode::F64: return WASM::AbiCoreType::F64;
	default:
		throw std::runtime_error(
				"component runtime: async context type is not numeric");
	}
}

ffi_type* ffiTypeForAddress(bool pointer64)
{
	return pointer64 ? &ffi_type_sint64 : &ffi_type_sint32;
}

uint64_t readAddress(void* argument, bool pointer64)
{
	return pointer64 ? static_cast<uint64_t>(*static_cast<int64_t*>(argument))
			: static_cast<uint64_t>(*static_cast<int32_t*>(argument));
}

void writeAddress(void* result, uint64_t value, bool pointer64)
{
	if (pointer64)
		*static_cast<int64_t*>(result) = static_cast<int64_t>(value);
	else
		*static_cast<int32_t*>(result) = static_cast<int32_t>(value);
}

WASM::CanonicalAbiOptions canonicalOptions(const WASM::Canon& canon)
{
	WASM::CanonicalAbiOptions options;
	for (const WASM::CanonOpt& opt : canon.opts) {
		switch (opt.kind) {
		case WASM::CanonOptKind::Utf8:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Utf8;
			break;
		case WASM::CanonOptKind::Utf16:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Utf16;
			break;
		case WASM::CanonOptKind::Latin1Utf16:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Latin1Utf16;
			break;
		case WASM::CanonOptKind::Memory:
			options.memory = opt.index;
			break;
		case WASM::CanonOptKind::Realloc:
			options.realloc = opt.index;
			break;
		case WASM::CanonOptKind::PostReturn:
			options.postReturn = opt.index;
			break;
		case WASM::CanonOptKind::Async:
			options.async = true;
			break;
		case WASM::CanonOptKind::Callback:
			break;
		}
	}
	if (canon.async)
		options.async = *canon.async;
	return options;
}

struct MemView {
	WASM::LinearMemory* memory = nullptr;
	WASM::Callable realloc;
	WASM::CanonicalAbiOptions options;
	ComponentRuntime* runtime = nullptr;
	uint32_t borrowScope = 0;
};

// ── Canonical ABI strings ──────────────────────────────────────────────────
// `ComponentValue::string` is always UTF-8. The guest representation depends on
// the canonopt: utf8, utf16 (LE code units), or latin1+utf16 (Latin-1 bytes when
// possible, otherwise UTF-16 with the pointer-width high bit set on the length).
struct EncodedString {
	uint64_t address = 0;
	uint64_t taggedCodeUnits = 0;
};

uint64_t allocate(const MemView& memory, uint64_t size, uint64_t alignment);
void ensureRange(const MemView& memory, uint64_t offset, uint64_t size);

std::string latin1ToUtf8(std::span<const uint8_t> bytes)
{
	std::string out;
	out.reserve(bytes.size());
	for (uint8_t byte : bytes)
		utf8::append(byte, std::back_inserter(out));
	return out;
}

std::string utf16ToUtf8(const uint8_t* data, std::size_t codeUnits)
{
	std::u16string units(codeUnits, u'\0');
	for (std::size_t i = 0; i < codeUnits; ++i)
		units[i] = static_cast<char16_t>(uint16_t(data[2 * i])
				| (uint16_t(data[2 * i + 1]) << 8));
	std::string out;
	try {
		utf8::utf16to8(units.begin(), units.end(), std::back_inserter(out));
	} catch (const utf8::exception&) {
		throw std::runtime_error(
				"component runtime: invalid UTF-16 canonical string");
	}
	return out;
}

uint64_t stringTag(const MemView& memory)
{
	return uint64_t{1} << (memory.options.pointerSize * 8 - 1);
}

void writeUtf16(const std::u16string& text, uint64_t address,
		const MemView& memory)
{
	for (std::size_t i = 0; i < text.size(); ++i) {
		memory.memory->memoryBase[address + 2 * i] = uint8_t(text[i] & 0xFF);
		memory.memory->memoryBase[address + 2 * i + 1] =
				uint8_t(text[i] >> 8);
	}
}

EncodedString storeString(const std::string& text, const MemView& memory)
{
	constexpr uint64_t kMaxStringBytes = (uint64_t{1} << 28) - 1;
	if (!utf8::is_valid(text.begin(), text.end()))
		throw std::runtime_error(
				"component runtime: host string is not valid UTF-8");
	std::u16string utf16;
	utf8::utf8to16(text.begin(), text.end(), std::back_inserter(utf16));
	const uint64_t tag = stringTag(memory);

	switch (memory.options.stringEncoding) {
	case WASM::CanonicalAbiOptions::StringEncoding::Utf8: {
		if (text.size() > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		const uint64_t address = allocate(memory, text.size(), 1);
		ensureRange(memory, address, text.size());
		if (!text.empty())
			std::memcpy(memory.memory->memoryBase + address, text.data(),
					text.size());
		return EncodedString{address, text.size()};
	}
	case WASM::CanonicalAbiOptions::StringEncoding::Utf16: {
		if (utf16.size() * 2 > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		const uint64_t address = allocate(memory, utf16.size() * 2, 2);
		ensureRange(memory, address, utf16.size() * 2);
		writeUtf16(utf16, address, memory);
		return EncodedString{address, utf16.size()};
	}
	case WASM::CanonicalAbiOptions::StringEncoding::Latin1Utf16: {
		bool latin1 = true;
		for (char16_t unit : utf16)
			if (unit >= 0x100) {
				latin1 = false;
				break;
			}
		if (latin1) {
			if (utf16.size() > kMaxStringBytes)
				throw std::runtime_error(
						"component runtime: string is too long");
			const uint64_t address = allocate(memory, utf16.size(), 2);
			ensureRange(memory, address, utf16.size());
			for (std::size_t i = 0; i < utf16.size(); ++i)
				memory.memory->memoryBase[address + i] = uint8_t(utf16[i]);
			return EncodedString{address, utf16.size()};
		}
		if (utf16.size() * 2 > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		const uint64_t address = allocate(memory, utf16.size() * 2, 2);
		ensureRange(memory, address, utf16.size() * 2);
		writeUtf16(utf16, address, memory);
		return EncodedString{address, utf16.size() | tag};
	}
	}
	throw std::runtime_error("component runtime: unknown string encoding");
}

std::string loadString(uint64_t address, uint64_t taggedCodeUnits,
		const MemView& memory)
{
	constexpr uint64_t kMaxStringBytes = (uint64_t{1} << 28) - 1;
	const uint64_t tag = stringTag(memory);
	auto decodeUtf8 = [&](uint64_t length) {
		ensureRange(memory, address, length);
		const char* data = reinterpret_cast<const char*>(
				memory.memory->memoryBase + address);
		if (!utf8::is_valid(data, data + length))
			throw std::runtime_error(
					"component runtime: invalid UTF-8 canonical string");
		return std::string(data, std::size_t(length));
	};

	switch (memory.options.stringEncoding) {
	case WASM::CanonicalAbiOptions::StringEncoding::Utf8:
		if (taggedCodeUnits > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		return decodeUtf8(taggedCodeUnits);
	case WASM::CanonicalAbiOptions::StringEncoding::Utf16:
		if (taggedCodeUnits * 2 > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		if ((address & 1) != 0)
			throw std::runtime_error(
					"component runtime: misaligned UTF-16 canonical string");
		ensureRange(memory, address, taggedCodeUnits * 2);
		return utf16ToUtf8(memory.memory->memoryBase + address,
				std::size_t(taggedCodeUnits));
	case WASM::CanonicalAbiOptions::StringEncoding::Latin1Utf16: {
		const bool isUtf16 = (taggedCodeUnits & tag) != 0;
		const uint64_t length = isUtf16 ? taggedCodeUnits ^ tag
				: taggedCodeUnits;
		if (isUtf16) {
			if (length * 2 > kMaxStringBytes)
				throw std::runtime_error(
						"component runtime: string is too long");
			if ((address & 1) != 0)
				throw std::runtime_error(
						"component runtime: misaligned UTF-16 canonical string");
			ensureRange(memory, address, length * 2);
			return utf16ToUtf8(memory.memory->memoryBase + address,
					std::size_t(length));
		}
		if (length > kMaxStringBytes)
			throw std::runtime_error("component runtime: string is too long");
		ensureRange(memory, address, length);
		return latin1ToUtf8(std::span<const uint8_t>(
				memory.memory->memoryBase + address, std::size_t(length)));
	}
	}
	throw std::runtime_error("component runtime: unknown string encoding");
}


uint64_t alignUp(uint64_t value, uint64_t alignment)
{
	return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

void ensureRange(const MemView& memory, uint64_t offset, uint64_t size)
{
	if (!memory.memory)
		throw std::runtime_error("component runtime: no canonical ABI memory");
	if (offset > memory.memory->memorySize
			|| size > memory.memory->memorySize - offset)
		throw std::runtime_error("component runtime: canonical ABI memory out of bounds");
}

uint64_t readUInt(const MemView& memory, uint64_t offset, unsigned bytes)
{
	ensureRange(memory, offset, bytes);
	uint64_t value = 0;
	for (unsigned i = 0; i < bytes; ++i)
		value |= uint64_t{memory.memory->memoryBase[offset + i]} << (i * 8);
	return value;
}

void writeUInt(const MemView& memory, uint64_t offset, uint64_t value,
		unsigned bytes)
{
	ensureRange(memory, offset, bytes);
	for (unsigned i = 0; i < bytes; ++i)
		memory.memory->memoryBase[offset + i] = uint8_t(value >> (i * 8));
}

uint64_t allocate(const MemView& memory, uint64_t size, uint64_t alignment)
{
	if (size == 0)
		return 0;
	if (!memory.realloc.fnPtr)
		throw std::runtime_error(
				"component runtime: canonical ABI realloc is not configured");
	const uint64_t address = memory.options.pointerSize == 8
			? WASM::callCallable<uint64_t>(memory.realloc, uint64_t{0}, uint64_t{0},
					uint64_t(alignment), size)
			: WASM::callCallable<uint32_t>(memory.realloc, uint32_t{0}, uint32_t{0},
					uint32_t(alignment), uint32_t(size));
	// A nested call (this runs inside a canonical-ABI closure, below generated
	// code) reports a trap through the pending slot instead of throwing. Stop
	// here: the generated caller checks the slot and returns, and a C++ throw
	// would have to unwind a generated frame.
	if (WASM::trapPending())
		return 0;
	if (address == 0)
		throw std::runtime_error("component runtime: canonical ABI realloc returned null");
	ensureRange(memory, address, size);
	return address;
}

void pushI32(std::vector<WASM::Value>& out, uint32_t value)
{
	WASM::Value value32{};
	value32.kind = WASM::ValueTypeCode::I32;
	value32.u32 = value;
	out.push_back(value32);
}

void pushI64(std::vector<WASM::Value>& out, uint64_t value)
{
	WASM::Value value64{};
	value64.kind = WASM::ValueTypeCode::I64;
	value64.ui64 = value;
	out.push_back(value64);
}

void pushPtr(std::vector<WASM::Value>& out, uint64_t value,
		const MemView& memory)
{
	if (memory.options.pointerSize == 8)
		pushI64(out, value);
	else
		pushI32(out, uint32_t(value));
}

void lowerPrimitive(const WASM::ComponentValueType& type,
		const ComponentValue& value, const MemView& memory,
		std::vector<WASM::Value>& out)
{
	using P = WASM::PrimitiveValueType;
	WASM::Value flat{};
	switch (type.primitive) {
	case P::Bool:
		flat.kind = WASM::ValueTypeCode::I32;
		flat.i32 = value.u64 != 0 ? 1 : 0;
		break;
	case P::S8: flat.kind = WASM::ValueTypeCode::I32; flat.i32 = int8_t(value.u64); break;
	case P::U8: flat.kind = WASM::ValueTypeCode::I32; flat.u32 = uint8_t(value.u64); break;
	case P::S16: flat.kind = WASM::ValueTypeCode::I32; flat.i32 = int16_t(value.u64); break;
	case P::U16: flat.kind = WASM::ValueTypeCode::I32; flat.u32 = uint16_t(value.u64); break;
	case P::S32: flat.kind = WASM::ValueTypeCode::I32; flat.i32 = int32_t(value.u64); break;
	case P::U32: flat.kind = WASM::ValueTypeCode::I32; flat.u32 = uint32_t(value.u64); break;
	case P::S64: flat.kind = WASM::ValueTypeCode::I64; flat.i64 = int64_t(value.u64); break;
	case P::U64: flat.kind = WASM::ValueTypeCode::I64; flat.ui64 = value.u64; break;
	case P::F32: flat.kind = WASM::ValueTypeCode::F32; flat.f32 = float(value.f64); break;
	case P::F64: flat.kind = WASM::ValueTypeCode::F64; flat.f64 = value.f64; break;
	case P::Char: flat.kind = WASM::ValueTypeCode::I32; flat.u32 = value.discriminant; break;
	case P::String: {
		const EncodedString encoded = storeString(value.string, memory);
		pushPtr(out, encoded.address, memory);
		pushPtr(out, encoded.taggedCodeUnits, memory);
		return;
	}
	default:
		throw std::runtime_error(
				"component runtime: unsupported canonical primitive");
	}
	out.push_back(flat);
}

void liftPrimitive(const WASM::ComponentValueType& type,
		std::span<const WASM::Value> flat, std::size_t& index,
		const MemView& memory, ComponentValue& out)
{
	using P = WASM::PrimitiveValueType;
	using K = ComponentValue::Kind;
	if (index >= flat.size())
		throw std::runtime_error("component runtime: missing flat primitive");
	const WASM::Value value = flat[index++];
	switch (type.primitive) {
	case P::Bool:
		if (value.u32 > 1)
			throw std::runtime_error("component runtime: invalid canonical bool");
		out.kind = K::Bool;
		out.u64 = value.u32 != 0 ? 1 : 0;
		break;
	case P::S8: out.kind = K::S8; out.s64 = int8_t(value.i32); break;
	case P::U8: out.kind = K::U8; out.u64 = uint8_t(value.i32); break;
	case P::S16: out.kind = K::S16; out.s64 = int16_t(value.i32); break;
	case P::U16: out.kind = K::U16; out.u64 = uint16_t(value.i32); break;
	case P::S32: out.kind = K::S32; out.s64 = value.i32; break;
	case P::U32: out.kind = K::U32; out.u64 = value.u32; break;
	case P::S64: out.kind = K::S64; out.s64 = value.i64; break;
	case P::U64: out.kind = K::U64; out.u64 = value.ui64; break;
	case P::F32: out.kind = K::F32; out.f64 = value.f32; break;
	case P::F64: out.kind = K::F64; out.f64 = value.f64; break;
	case P::Char: out.kind = K::Char; out.discriminant = value.u32; break;
	case P::String: {
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing string length");
		const WASM::Value lengthValue = flat[index++];
		const uint64_t address = memory.options.pointerSize == 8
				? value.ui64 : value.u32;
		const uint64_t taggedCodeUnits = memory.options.pointerSize == 8
				? lengthValue.ui64 : lengthValue.u32;
		out.kind = K::String;
		out.string = loadString(address, taggedCodeUnits, memory);
		break;
	}
	default:
		throw std::runtime_error(
				"component runtime: unsupported canonical primitive");
	}
}

void storeValueAt(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const ComponentValue& value,
		uint64_t address, const MemView& memory, unsigned depth);
void loadValueAt(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, uint64_t address,
		const MemView& memory, ComponentValue& out, unsigned depth);

WASM::Value zeroFlat(WASM::AbiCoreType type)
{
	WASM::Value value{};
	value.kind = type == WASM::AbiCoreType::I32 ? WASM::ValueTypeCode::I32
			: type == WASM::AbiCoreType::I64 ? WASM::ValueTypeCode::I64
			: type == WASM::AbiCoreType::F32 ? WASM::ValueTypeCode::F32
			: WASM::ValueTypeCode::F64;
	return value;
}

WASM::Value coerceLower(const WASM::Value& value, WASM::AbiCoreType from,
		WASM::AbiCoreType to)
{
	if (from == to)
		return value;
	uint64_t bits = 0;
	if (from == WASM::AbiCoreType::I32)
		bits = value.u32;
	else if (from == WASM::AbiCoreType::I64)
		bits = value.ui64;
	else if (from == WASM::AbiCoreType::F32) {
		uint32_t raw;
		float f = value.f32;
		std::memcpy(&raw, &f, sizeof(raw));
		bits = raw;
	} else {
		double d = value.f64;
		std::memcpy(&bits, &d, sizeof(bits));
	}
	if (to == WASM::AbiCoreType::I32) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::I32;
		out.u32 = uint32_t(bits);
		return out;
	}
	if (to == WASM::AbiCoreType::I64) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::I64;
		out.ui64 = bits;
		return out;
	}
	if (to == WASM::AbiCoreType::F32) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::F32;
		const uint32_t raw = uint32_t(bits);
		std::memcpy(&out.f32, &raw, sizeof(raw));
		return out;
	}
	WASM::Value out{};
	out.kind = WASM::ValueTypeCode::F64;
	std::memcpy(&out.f64, &bits, sizeof(bits));
	return out;
}

std::vector<WASM::AbiCoreType> flatTypesOf(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type)
{
	std::vector<WASM::AbiCoreType> types;
	if (!store.canonicalAbiFlatValue(type, types))
		throw std::runtime_error("component runtime: value cannot be flattened");
	return types;
}

WASM::CanonicalAbiLayout layoutOf(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const MemView& memory);

struct MapPairLayout {
	WASM::CanonicalAbiLayout key;
	WASM::CanonicalAbiLayout value;
	uint64_t valueOffset = 0;
	uint64_t size = 0;
	uint32_t align = 1;
};

MapPairLayout mapPairLayout(const WASM::TypeStore& store,
		const WASM::ComponentValueType& keyType,
		const WASM::ComponentValueType& valueType, const MemView& memory)
{
	MapPairLayout layout;
	layout.key = layoutOf(store, keyType, memory);
	layout.value = layoutOf(store, valueType, memory);
	layout.valueOffset = alignUp(layout.key.size, layout.value.align);
	layout.size = alignUp(layout.valueOffset + layout.value.size,
			std::max(layout.key.align, layout.value.align));
	layout.align = std::max(layout.key.align, layout.value.align);
	return layout;
}

void lowerValue(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const ComponentValue& value,
		const MemView& memory, std::vector<WASM::Value>& out, unsigned depth);

void appendVariantPayload(const WASM::TypeStore& store,
		const WASM::ComponentValueType& actual, const ComponentValue& value,
		std::span<const WASM::AbiCoreType> joined, const MemView& memory,
		std::vector<WASM::Value>& out, unsigned depth)
{
	std::vector<WASM::Value> payload;
	lowerValue(store, actual, value, memory, payload, depth + 1);
	const std::vector<WASM::AbiCoreType> actualTypes = flatTypesOf(store, actual);
	for (std::size_t i = 0; i < joined.size(); ++i) {
		if (i < payload.size())
			out.push_back(coerceLower(payload[i],
					i < actualTypes.size() ? actualTypes[i] : joined[i], joined[i]));
		else
			out.push_back(zeroFlat(joined[i]));
	}
}

void lowerValue(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const ComponentValue& value,
		const MemView& memory, std::vector<WASM::Value>& out, unsigned depth)
{
	if (depth > 64)
		throw std::runtime_error("component runtime: canonical value nesting too deep");
	if (type.kind == WASM::ComponentValueType::Kind::Primitive) {
		lowerPrimitive(type, value, memory, out);
		return;
	}
	if (type.kind != WASM::ComponentValueType::Kind::TypeId)
		throw std::runtime_error("component runtime: unresolved component value type");
	const WASM::ComponentTypeView view =
			store.componentType(WASM::TypeId{type.typeIndex});
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive: {
		WASM::ComponentValueType primitive;
		primitive.kind = WASM::ComponentValueType::Kind::Primitive;
		primitive.primitive = view.primitive();
		lowerPrimitive(primitive, value, memory, out);
		return;
	}
	case WASM::TypeKind::ComponentRecord:
		if (value.elements.size() != view.fields().size())
			throw std::runtime_error("component runtime: record field count mismatch");
		for (std::size_t i = 0; i < view.fields().size(); ++i)
			lowerValue(store, view.fields()[i].type, value.elements[i],
					memory, out, depth + 1);
		return;
	case WASM::TypeKind::ComponentTuple:
		if (value.elements.size() != view.tuple().size())
			throw std::runtime_error("component runtime: tuple element count mismatch");
		for (std::size_t i = 0; i < view.tuple().size(); ++i)
			lowerValue(store, view.tuple()[i], value.elements[i],
					memory, out, depth + 1);
		return;
	case WASM::TypeKind::ComponentFixedList:
		if (value.elements.size() != view.fixedLength())
			throw std::runtime_error("component runtime: fixed-list length mismatch");
		for (const ComponentValue& element : value.elements)
			lowerValue(store, view.element(), element, memory, out, depth + 1);
		return;
	case WASM::TypeKind::ComponentList: {
		WASM::CanonicalAbiLayout elementLayout;
		if (!store.canonicalAbiLayoutValue(view.element(), memory.options,
				elementLayout))
			throw std::runtime_error("component runtime: list element has no layout");
		const uint64_t address = allocate(memory,
				uint64_t{elementLayout.size} * value.elements.size(),
				elementLayout.align);
		for (std::size_t i = 0; i < value.elements.size(); ++i)
			storeValueAt(store, view.element(), value.elements[i],
					address + uint64_t(i) * elementLayout.size, memory, depth + 1);
		pushPtr(out, address, memory);
		pushPtr(out, value.elements.size(), memory);
		return;
	}
	case WASM::TypeKind::ComponentEnum:
		if (value.discriminant >= view.labels().size())
			throw std::runtime_error("component runtime: enum case out of range");
		pushI32(out, value.discriminant);
		return;
	case WASM::TypeKind::ComponentFlags: {
		if (view.labels().size() > 32)
			throw std::runtime_error("component runtime: flags wider than 32 bits");
		uint32_t bits = 0;
		for (std::size_t i = 0; i < value.flags.size() && i < 32; ++i)
			if (value.flags[i])
				bits |= uint32_t{1} << i;
		pushI32(out, bits);
		return;
	}
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow: {
		uint32_t handle = value.discriminant;
		if (memory.runtime != nullptr) {
			const WASM::TypeId resource{view.typeRef()};
			if (view.kind() == WASM::TypeKind::ComponentBorrow)
				handle = memory.runtime->useResourceHandle(handle, resource,
						true, memory.borrowScope);
			else
				memory.runtime->validateResourceHandle(handle, resource);
		}
		pushI32(out, handle);
		return;
	}
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture:
		pushI32(out, value.discriminant);
		return;
	case WASM::TypeKind::ComponentOption: {
		const std::vector<WASM::AbiCoreType> flat = flatTypesOf(store, type);
		const bool present = value.hasValue && !value.elements.empty();
		pushI32(out, present ? 1 : 0);
		const std::span<const WASM::AbiCoreType> joined(flat.data() + 1,
				flat.size() - 1);
		if (present)
			appendVariantPayload(store, view.element(), value.elements.front(),
					joined, memory, out, depth);
		else
			for (WASM::AbiCoreType item : joined)
				out.push_back(zeroFlat(item));
		return;
	}
	case WASM::TypeKind::ComponentResult: {
		const std::vector<WASM::AbiCoreType> flat = flatTypesOf(store, type);
		const std::span<const WASM::AbiCoreType> joined(flat.data() + 1,
				flat.size() - 1);
		const bool isError = value.discriminant != 0;
		pushI32(out, isError ? 1 : 0);
		const WASM::ComponentValueType* payloadType = isError
				? (view.hasResultError() ? &view.resultError() : nullptr)
				: (view.hasResultOk() ? &view.resultOk() : nullptr);
		const bool present = value.hasValue && !value.elements.empty()
				&& payloadType != nullptr;
		if (present)
			appendVariantPayload(store, *payloadType, value.elements.front(),
					joined, memory, out, depth);
		else
			for (WASM::AbiCoreType item : joined)
				out.push_back(zeroFlat(item));
		return;
	}
	case WASM::TypeKind::ComponentVariant: {
		const std::vector<WASM::AbiCoreType> flat = flatTypesOf(store, type);
		const std::span<const WASM::AbiCoreType> joined(flat.data() + 1,
				flat.size() - 1);
		if (value.discriminant >= view.cases().size())
			throw std::runtime_error("component runtime: variant case out of range");
		pushI32(out, value.discriminant);
		const WASM::ComponentCase& item = view.cases()[value.discriminant];
		if (item.hasType && value.hasValue && !value.elements.empty())
			appendVariantPayload(store, item.type, value.elements.front(),
					joined, memory, out, depth);
		else
			for (WASM::AbiCoreType type : joined)
				out.push_back(zeroFlat(type));
		return;
	}
	case WASM::TypeKind::ComponentMap: {
		const MapPairLayout pair = mapPairLayout(store, view.mapKey(),
				view.mapValue(), memory);
		const uint64_t data = allocate(memory,
				pair.size * value.elements.size(), pair.align);
		for (std::size_t i = 0; i < value.elements.size(); ++i) {
			const ComponentValue& entry = value.elements[i];
			if (entry.elements.size() != 2)
				throw std::runtime_error("component runtime: map entry is not a pair");
			const uint64_t address = data + uint64_t(i) * pair.size;
			storeValueAt(store, view.mapKey(), entry.elements[0], address,
					memory, depth + 1);
			storeValueAt(store, view.mapValue(), entry.elements[1],
					address + pair.valueOffset, memory, depth + 1);
		}
		pushPtr(out, data, memory);
		pushPtr(out, value.elements.size(), memory);
		return;
	}
	default:
		throw std::runtime_error("component runtime: type is not a value");
	}
}

WASM::Value coerceLift(const WASM::Value& value, WASM::AbiCoreType from,
		WASM::AbiCoreType to)
{
	if (from == to)
		return value;
	uint64_t bits = from == WASM::AbiCoreType::I64 ? value.ui64 : value.u32;
	if (to == WASM::AbiCoreType::I32) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::I32;
		out.u32 = uint32_t(bits);
		return out;
	}
	if (to == WASM::AbiCoreType::F32) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::F32;
		const uint32_t raw = uint32_t(bits);
		std::memcpy(&out.f32, &raw, sizeof(raw));
		return out;
	}
	if (to == WASM::AbiCoreType::F64) {
		WASM::Value out{};
		out.kind = WASM::ValueTypeCode::F64;
		std::memcpy(&out.f64, &bits, sizeof(bits));
		return out;
	}
	WASM::Value out{};
	out.kind = WASM::ValueTypeCode::I64;
	out.ui64 = bits;
	return out;
}

void liftValue(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type,
		std::span<const WASM::Value> flat, std::size_t& index,
		const MemView& memory, ComponentValue& out, unsigned depth);

void liftVariantPayload(const WASM::TypeStore& store,
		const WASM::ComponentValueType& actual,
		std::span<const WASM::AbiCoreType> joined,
		std::span<const WASM::Value> flat, std::size_t& index,
		const MemView& memory, ComponentValue& out, unsigned depth)
{
	const std::vector<WASM::AbiCoreType> expected = flatTypesOf(store, actual);
	std::vector<WASM::Value> payload;
	payload.reserve(expected.size());
	for (std::size_t i = 0; i < joined.size(); ++i) {
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing variant payload");
		const WASM::Value value = flat[index++];
		if (i < expected.size())
			payload.push_back(coerceLift(value, joined[i], expected[i]));
	}
	std::size_t payloadIndex = 0;
	liftValue(store, actual, payload, payloadIndex, memory, out, depth + 1);
}

void liftValue(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type,
		std::span<const WASM::Value> flat, std::size_t& index,
		const MemView& memory, ComponentValue& out, unsigned depth)
{
	if (depth > 64)
		throw std::runtime_error("component runtime: canonical value nesting too deep");
	if (type.kind == WASM::ComponentValueType::Kind::Primitive) {
		liftPrimitive(type, flat, index, memory, out);
		return;
	}
	if (type.kind != WASM::ComponentValueType::Kind::TypeId)
		throw std::runtime_error("component runtime: unresolved component value type");
	const WASM::ComponentTypeView view =
			store.componentType(WASM::TypeId{type.typeIndex});
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive: {
		WASM::ComponentValueType primitive;
		primitive.kind = WASM::ComponentValueType::Kind::Primitive;
		primitive.primitive = view.primitive();
		liftPrimitive(primitive, flat, index, memory, out);
		return;
	}
	case WASM::TypeKind::ComponentRecord:
		out.kind = ComponentValue::Kind::Record;
		out.elements.resize(view.fields().size());
		for (std::size_t i = 0; i < view.fields().size(); ++i)
			liftValue(store, view.fields()[i].type, flat, index, memory,
					out.elements[i], depth + 1);
		return;
	case WASM::TypeKind::ComponentTuple:
		out.kind = ComponentValue::Kind::Tuple;
		out.elements.resize(view.tuple().size());
		for (std::size_t i = 0; i < view.tuple().size(); ++i)
			liftValue(store, view.tuple()[i], flat, index, memory,
					out.elements[i], depth + 1);
		return;
	case WASM::TypeKind::ComponentFixedList:
		out.kind = ComponentValue::Kind::FixedList;
		out.elements.resize(view.fixedLength());
		for (ComponentValue& element : out.elements)
			liftValue(store, view.element(), flat, index, memory,
					element, depth + 1);
		return;
	case WASM::TypeKind::ComponentList: {
		if (index + 2 > flat.size())
			throw std::runtime_error("component runtime: missing list pointer/length");
		const uint64_t address = memory.options.pointerSize == 8
				? flat[index].ui64 : flat[index].u32;
		const uint64_t length = memory.options.pointerSize == 8
				? flat[index + 1].ui64 : flat[index + 1].u32;
		index += 2;
		WASM::CanonicalAbiLayout elementLayout;
		if (!store.canonicalAbiLayoutValue(view.element(), memory.options,
				elementLayout))
			throw std::runtime_error("component runtime: list element has no layout");
		if (length > (uint64_t{1} << 30))
			throw std::runtime_error("component runtime: list length too large");
		ensureRange(memory, address, uint64_t{elementLayout.size} * length);
		out.kind = ComponentValue::Kind::List;
		out.elements.resize(std::size_t(length));
		for (std::size_t i = 0; i < out.elements.size(); ++i)
			loadValueAt(store, view.element(),
					address + uint64_t(i) * elementLayout.size, memory,
					out.elements[i], depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentEnum:
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing enum case");
		out.kind = ComponentValue::Kind::Enum;
		out.discriminant = flat[index++].u32;
		if (out.discriminant >= view.labels().size())
			throw std::runtime_error("component runtime: enum case out of range");
		return;
	case WASM::TypeKind::ComponentFlags:
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing flags");
		out.kind = ComponentValue::Kind::Flags;
		out.discriminant = flat[index++].u32;
		out.flags.resize(view.labels().size());
		for (std::size_t i = 0; i < out.flags.size() && i < 32; ++i)
			out.flags[i] = (out.discriminant & (uint32_t{1} << i)) != 0;
		return;
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow:
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture:
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing handle");
		if ((view.kind() == WASM::TypeKind::ComponentOwn
				|| view.kind() == WASM::TypeKind::ComponentBorrow)
				&& memory.runtime != nullptr)
			memory.runtime->validateResourceHandle(flat[index].u32,
					WASM::TypeId{view.typeRef()});
		out.kind = view.kind() == WASM::TypeKind::ComponentOwn
				? ComponentValue::Kind::Own
				: view.kind() == WASM::TypeKind::ComponentBorrow
						? ComponentValue::Kind::Borrow
						: view.kind() == WASM::TypeKind::ComponentStream
								? ComponentValue::Kind::Stream
								: ComponentValue::Kind::Future;
		out.discriminant = flat[index++].u32;
		return;
	case WASM::TypeKind::ComponentOption: {
		const std::vector<WASM::AbiCoreType> flatTypes = flatTypesOf(store, type);
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing option tag");
		const uint32_t tag = flat[index++].u32;
		if (tag > 1)
			throw std::runtime_error("component runtime: invalid option tag");
		out.kind = ComponentValue::Kind::Option;
		out.discriminant = tag;
		out.hasValue = tag != 0;
		const std::span<const WASM::AbiCoreType> joined(flatTypes.data() + 1,
				flatTypes.size() - 1);
		if (out.hasValue) {
			out.elements.resize(1);
			liftVariantPayload(store, view.element(), joined, flat, index,
					memory, out.elements.front(), depth);
		} else {
			index += joined.size();
		}
		return;
	}
	case WASM::TypeKind::ComponentResult: {
		const std::vector<WASM::AbiCoreType> flatTypes = flatTypesOf(store, type);
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing result tag");
		const uint32_t tag = flat[index++].u32;
		if (tag > 1)
			throw std::runtime_error("component runtime: invalid result tag");
		out.kind = ComponentValue::Kind::Result;
		out.discriminant = tag;
		const std::span<const WASM::AbiCoreType> joined(flatTypes.data() + 1,
				flatTypes.size() - 1);
		const bool isError = tag != 0;
		const bool present = isError ? view.hasResultError() : view.hasResultOk();
		out.hasValue = present;
		if (present) {
			out.elements.resize(1);
			liftVariantPayload(store, isError ? view.resultError() : view.resultOk(),
					joined, flat, index, memory, out.elements.front(), depth);
		} else {
			index += joined.size();
		}
		return;
	}
	case WASM::TypeKind::ComponentVariant: {
		const std::vector<WASM::AbiCoreType> flatTypes = flatTypesOf(store, type);
		if (index >= flat.size())
			throw std::runtime_error("component runtime: missing variant tag");
		const uint32_t tag = flat[index++].u32;
		if (tag >= view.cases().size())
			throw std::runtime_error("component runtime: variant case out of range");
		out.kind = ComponentValue::Kind::Variant;
		out.discriminant = tag;
		const WASM::ComponentCase& item = view.cases()[tag];
		const std::span<const WASM::AbiCoreType> joined(flatTypes.data() + 1,
				flatTypes.size() - 1);
		out.hasValue = item.hasType;
		if (item.hasType) {
			out.elements.resize(1);
			liftVariantPayload(store, item.type, joined, flat, index, memory,
					out.elements.front(), depth);
		} else {
			index += joined.size();
		}
		return;
	}
	case WASM::TypeKind::ComponentMap: {
		if (index + 2 > flat.size())
			throw std::runtime_error("component runtime: missing map pointer/length");
		const uint64_t data = memory.options.pointerSize == 8
				? flat[index].ui64 : flat[index].u32;
		const uint64_t length = memory.options.pointerSize == 8
				? flat[index + 1].ui64 : flat[index + 1].u32;
		index += 2;
		if (length > (uint64_t{1} << 30))
			throw std::runtime_error("component runtime: map length too large");
		const MapPairLayout pair = mapPairLayout(store, view.mapKey(),
				view.mapValue(), memory);
		ensureRange(memory, data, pair.size * length);
		out.kind = ComponentValue::Kind::Map;
		out.elements.resize(std::size_t(length));
		for (std::size_t i = 0; i < out.elements.size(); ++i) {
			out.elements[i].kind = ComponentValue::Kind::Tuple;
			out.elements[i].elements.resize(2);
			const uint64_t address = data + uint64_t(i) * pair.size;
			loadValueAt(store, view.mapKey(), address, memory,
					out.elements[i].elements[0], depth + 1);
			loadValueAt(store, view.mapValue(), address + pair.valueOffset,
					memory, out.elements[i].elements[1], depth + 1);
		}
		return;
	}
	default:
		throw std::runtime_error("component runtime: type is not a value");
	}
}

WASM::CanonicalAbiLayout layoutOf(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const MemView& memory)
{
	WASM::CanonicalAbiLayout layout;
	if (!store.canonicalAbiLayoutValue(type, memory.options, layout))
		throw std::runtime_error("component runtime: value has no canonical layout");
	return layout;
}

void storePrimitiveAt(const WASM::ComponentValueType& type,
		const ComponentValue& value, uint64_t address, const MemView& memory)
{
	using P = WASM::PrimitiveValueType;
	const WASM::CanonicalAbiLayout layout = layoutOf(
			WASM::Store::current().types().store(), type, memory);
	switch (type.primitive) {
	case P::Bool: writeUInt(memory, address, value.u64 != 0 ? 1 : 0, 1); break;
	case P::S8:
	case P::U8: writeUInt(memory, address, value.u64, 1); break;
	case P::S16:
	case P::U16: writeUInt(memory, address, value.u64, 2); break;
	case P::S32:
	case P::U32:
	case P::Char: writeUInt(memory, address, value.u64, 4); break;
	case P::S64:
	case P::U64: writeUInt(memory, address, value.u64, 8); break;
	case P::F32: {
		if (layout.size != 4)
			throw std::runtime_error("component runtime: bad f32 layout");
		const uint32_t bits = [&] {
			float f = float(value.f64);
			uint32_t out;
			std::memcpy(&out, &f, sizeof(out));
			return out;
		}();
		writeUInt(memory, address, bits, 4);
		break;
	}
	case P::F64: {
		uint64_t bits;
		std::memcpy(&bits, &value.f64, sizeof(bits));
		writeUInt(memory, address, bits, 8);
		break;
	}
	case P::String: {
		const EncodedString encoded = storeString(value.string, memory);
		writeUInt(memory, address, encoded.address, memory.options.pointerSize);
		writeUInt(memory, address + memory.options.pointerSize,
				encoded.taggedCodeUnits, memory.options.pointerSize);
		break;
	}
	default:
		throw std::runtime_error(
				"component runtime: unsupported primitive memory store");
	}
}

void storeValueAt(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, const ComponentValue& value,
		uint64_t address, const MemView& memory, unsigned depth)
{
	if (depth > 64)
		throw std::runtime_error("component runtime: canonical value nesting too deep");
	if (type.kind == WASM::ComponentValueType::Kind::Primitive) {
		storePrimitiveAt(type, value, address, memory);
		return;
	}
	if (type.kind != WASM::ComponentValueType::Kind::TypeId)
		throw std::runtime_error("component runtime: unresolved component value type");
	const WASM::ComponentTypeView view =
			store.componentType(WASM::TypeId{type.typeIndex});
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive: {
		WASM::ComponentValueType primitive;
		primitive.kind = WASM::ComponentValueType::Kind::Primitive;
		primitive.primitive = view.primitive();
		storePrimitiveAt(primitive, value, address, memory);
		return;
	}
	case WASM::TypeKind::ComponentRecord:
	case WASM::TypeKind::ComponentTuple: {
		uint64_t offset = 0;
		const auto storeElement = [&](const WASM::ComponentValueType& element,
				const ComponentValue& item) {
			const WASM::CanonicalAbiLayout layout = layoutOf(store, element, memory);
			offset = alignUp(offset, layout.align);
			storeValueAt(store, element, item, address + offset, memory, depth + 1);
			offset += layout.size;
		};
		if (view.kind() == WASM::TypeKind::ComponentRecord) {
			if (value.elements.size() != view.fields().size())
				throw std::runtime_error("component runtime: record field count mismatch");
			for (std::size_t i = 0; i < view.fields().size(); ++i)
				storeElement(view.fields()[i].type, value.elements[i]);
		} else {
			if (value.elements.size() != view.tuple().size())
				throw std::runtime_error("component runtime: tuple element count mismatch");
			for (std::size_t i = 0; i < view.tuple().size(); ++i)
				storeElement(view.tuple()[i], value.elements[i]);
		}
		return;
	}
	case WASM::TypeKind::ComponentFixedList: {
		if (value.elements.size() != view.fixedLength())
			throw std::runtime_error("component runtime: fixed-list length mismatch");
		const WASM::CanonicalAbiLayout layout = layoutOf(store, view.element(), memory);
		for (std::size_t i = 0; i < value.elements.size(); ++i)
			storeValueAt(store, view.element(), value.elements[i],
					address + uint64_t(i) * layout.size, memory, depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentList: {
		const WASM::CanonicalAbiLayout layout = layoutOf(store, view.element(), memory);
		const uint64_t data = allocate(memory,
				uint64_t{layout.size} * value.elements.size(), layout.align);
		for (std::size_t i = 0; i < value.elements.size(); ++i)
			storeValueAt(store, view.element(), value.elements[i],
					data + uint64_t(i) * layout.size, memory, depth + 1);
		writeUInt(memory, address, data, memory.options.pointerSize);
		writeUInt(memory, address + memory.options.pointerSize,
				value.elements.size(), memory.options.pointerSize);
		return;
	}
	case WASM::TypeKind::ComponentEnum:
		if (value.discriminant >= view.labels().size())
			throw std::runtime_error("component runtime: enum case out of range");
		writeUInt(memory, address, value.discriminant,
				layoutOf(store, type, memory).size);
		return;
	case WASM::TypeKind::ComponentFlags: {
		uint32_t bits = 0;
		for (std::size_t i = 0; i < value.flags.size() && i < 32; ++i)
			if (value.flags[i])
				bits |= uint32_t{1} << i;
		writeUInt(memory, address, bits, layoutOf(store, type, memory).size);
		return;
	}
	case WASM::TypeKind::ComponentOption: {
		writeUInt(memory, address, value.hasValue ? 1 : 0, 1);
		if (value.hasValue && !value.elements.empty()) {
			const WASM::CanonicalAbiLayout payload = layoutOf(store,
					view.element(), memory);
			storeValueAt(store, view.element(), value.elements.front(),
					address + alignUp(1, payload.align), memory, depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentResult:
	case WASM::TypeKind::ComponentVariant: {
		const std::size_t caseCount = view.kind() == WASM::TypeKind::ComponentResult
				? 2 : view.cases().size();
		const std::size_t discWidth = caseCount <= 0x100 ? 1
				: caseCount <= 0x10000 ? 2 : 4;
		const bool isError = value.discriminant != 0;
		writeUInt(memory, address, value.discriminant, discWidth);
		const WASM::ComponentValueType* payloadType = nullptr;
		if (view.kind() == WASM::TypeKind::ComponentResult)
			payloadType = isError
					? (view.hasResultError() ? &view.resultError() : nullptr)
					: (view.hasResultOk() ? &view.resultOk() : nullptr);
		else if (value.discriminant < view.cases().size()
				&& view.cases()[value.discriminant].hasType)
			payloadType = &view.cases()[value.discriminant].type;
		if (payloadType != nullptr && value.hasValue && !value.elements.empty()) {
			const WASM::CanonicalAbiLayout payload = layoutOf(store, *payloadType, memory);
			storeValueAt(store, *payloadType, value.elements.front(),
					address + alignUp(discWidth, payload.align), memory, depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentMap: {
		const MapPairLayout pair = mapPairLayout(store, view.mapKey(),
				view.mapValue(), memory);
		const uint64_t data = allocate(memory,
				pair.size * value.elements.size(), pair.align);
		for (std::size_t i = 0; i < value.elements.size(); ++i) {
			const ComponentValue& entry = value.elements[i];
			if (entry.elements.size() != 2)
				throw std::runtime_error("component runtime: map entry is not a pair");
			const uint64_t entryAddress = data + uint64_t(i) * pair.size;
			storeValueAt(store, view.mapKey(), entry.elements[0], entryAddress,
					memory, depth + 1);
			storeValueAt(store, view.mapValue(), entry.elements[1],
					entryAddress + pair.valueOffset, memory, depth + 1);
		}
		writeUInt(memory, address, data, memory.options.pointerSize);
		writeUInt(memory, address + memory.options.pointerSize,
				value.elements.size(), memory.options.pointerSize);
		return;
	}
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow:
		if (memory.runtime != nullptr)
			memory.runtime->validateResourceHandle(value.discriminant,
					WASM::TypeId{view.typeRef()});
		writeUInt(memory, address, value.discriminant, 4);
		return;
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture:
		writeUInt(memory, address, value.discriminant, 4);
		return;
	default:
		throw std::runtime_error(
				"component runtime: this value cannot be stored in canonical memory");
	}
}

void loadPrimitiveAt(const WASM::ComponentValueType& type, uint64_t address,
		const MemView& memory, ComponentValue& out)
{
	using P = WASM::PrimitiveValueType;
	using K = ComponentValue::Kind;
	switch (type.primitive) {
	case P::Bool:
		out.kind = K::Bool;
		out.u64 = readUInt(memory, address, 1);
		if (out.u64 > 1)
			throw std::runtime_error("component runtime: invalid canonical bool");
		break;
	case P::S8: out.kind = K::S8; out.s64 = int8_t(readUInt(memory, address, 1)); break;
	case P::U8: out.kind = K::U8; out.u64 = readUInt(memory, address, 1); break;
	case P::S16: out.kind = K::S16; out.s64 = int16_t(readUInt(memory, address, 2)); break;
	case P::U16: out.kind = K::U16; out.u64 = readUInt(memory, address, 2); break;
	case P::S32: out.kind = K::S32; out.s64 = int32_t(readUInt(memory, address, 4)); break;
	case P::U32: out.kind = K::U32; out.u64 = readUInt(memory, address, 4); break;
	case P::S64: out.kind = K::S64; out.s64 = int64_t(readUInt(memory, address, 8)); break;
	case P::U64: out.kind = K::U64; out.u64 = readUInt(memory, address, 8); break;
	case P::F32: {
		const uint32_t bits = uint32_t(readUInt(memory, address, 4));
		float value;
		std::memcpy(&value, &bits, sizeof(value));
		out.kind = K::F32;
		out.f64 = value;
		break;
	}
	case P::F64: {
		const uint64_t bits = readUInt(memory, address, 8);
		double value;
		std::memcpy(&value, &bits, sizeof(value));
		out.kind = K::F64;
		out.f64 = value;
		break;
	}
	case P::Char: out.kind = K::Char; out.discriminant = uint32_t(readUInt(memory, address, 4)); break;
	case P::String: {
		const uint64_t data = readUInt(memory, address, memory.options.pointerSize);
		const uint64_t taggedCodeUnits = readUInt(memory,
				address + memory.options.pointerSize, memory.options.pointerSize);
		out.kind = K::String;
		out.string = loadString(data, taggedCodeUnits, memory);
		break;
	}
	default:
		throw std::runtime_error(
				"component runtime: unsupported primitive memory load");
	}
}

void loadValueAt(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, uint64_t address,
		const MemView& memory, ComponentValue& out, unsigned depth)
{
	if (depth > 64)
		throw std::runtime_error("component runtime: canonical value nesting too deep");
	if (type.kind == WASM::ComponentValueType::Kind::Primitive) {
		loadPrimitiveAt(type, address, memory, out);
		return;
	}
	if (type.kind != WASM::ComponentValueType::Kind::TypeId)
		throw std::runtime_error("component runtime: unresolved component value type");
	const WASM::ComponentTypeView view =
			store.componentType(WASM::TypeId{type.typeIndex});
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive: {
		WASM::ComponentValueType primitive;
		primitive.kind = WASM::ComponentValueType::Kind::Primitive;
		primitive.primitive = view.primitive();
		loadPrimitiveAt(primitive, address, memory, out);
		return;
	}
	case WASM::TypeKind::ComponentRecord:
	case WASM::TypeKind::ComponentTuple: {
		uint64_t offset = 0;
		const auto loadElement = [&](const WASM::ComponentValueType& element,
				ComponentValue& item) {
			const WASM::CanonicalAbiLayout layout = layoutOf(store, element, memory);
			offset = alignUp(offset, layout.align);
			loadValueAt(store, element, address + offset, memory, item, depth + 1);
			offset += layout.size;
		};
		if (view.kind() == WASM::TypeKind::ComponentRecord) {
			out.kind = ComponentValue::Kind::Record;
			out.elements.resize(view.fields().size());
			for (std::size_t i = 0; i < view.fields().size(); ++i)
				loadElement(view.fields()[i].type, out.elements[i]);
		} else {
			out.kind = ComponentValue::Kind::Tuple;
			out.elements.resize(view.tuple().size());
			for (std::size_t i = 0; i < view.tuple().size(); ++i)
				loadElement(view.tuple()[i], out.elements[i]);
		}
		return;
	}
	case WASM::TypeKind::ComponentFixedList: {
		out.kind = ComponentValue::Kind::FixedList;
		out.elements.resize(view.fixedLength());
		const WASM::CanonicalAbiLayout layout = layoutOf(store, view.element(), memory);
		for (std::size_t i = 0; i < out.elements.size(); ++i)
			loadValueAt(store, view.element(), address + uint64_t(i) * layout.size,
					memory, out.elements[i], depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentList: {
		const uint64_t data = readUInt(memory, address, memory.options.pointerSize);
		const uint64_t length = readUInt(memory,
				address + memory.options.pointerSize, memory.options.pointerSize);
		if (length > (uint64_t{1} << 30))
			throw std::runtime_error("component runtime: list length too large");
		const WASM::CanonicalAbiLayout layout = layoutOf(store, view.element(), memory);
		ensureRange(memory, data, uint64_t{layout.size} * length);
		out.kind = ComponentValue::Kind::List;
		out.elements.resize(std::size_t(length));
		for (std::size_t i = 0; i < out.elements.size(); ++i)
			loadValueAt(store, view.element(), data + uint64_t(i) * layout.size,
					memory, out.elements[i], depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentEnum:
		out.kind = ComponentValue::Kind::Enum;
		out.discriminant = uint32_t(readUInt(memory, address,
				layoutOf(store, type, memory).size));
		if (out.discriminant >= view.labels().size())
			throw std::runtime_error("component runtime: enum case out of range");
		return;
	case WASM::TypeKind::ComponentFlags:
		out.kind = ComponentValue::Kind::Flags;
		out.discriminant = uint32_t(readUInt(memory, address,
				layoutOf(store, type, memory).size));
		out.flags.resize(view.labels().size());
		for (std::size_t i = 0; i < out.flags.size() && i < 32; ++i)
			out.flags[i] = (out.discriminant & (uint32_t{1} << i)) != 0;
		return;
	case WASM::TypeKind::ComponentOption: {
		const uint32_t tag = uint32_t(readUInt(memory, address, 1));
		if (tag > 1)
			throw std::runtime_error("component runtime: invalid canonical option");
		out.kind = ComponentValue::Kind::Option;
		out.discriminant = tag;
		out.hasValue = tag != 0;
		if (out.hasValue) {
			const WASM::CanonicalAbiLayout payload = layoutOf(store,
					view.element(), memory);
			out.elements.resize(1);
			loadValueAt(store, view.element(),
					address + alignUp(1, payload.align), memory,
					out.elements.front(), depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentResult:
	case WASM::TypeKind::ComponentVariant: {
		const std::size_t caseCount = view.kind() == WASM::TypeKind::ComponentResult
				? 2 : view.cases().size();
		const std::size_t discWidth = caseCount <= 0x100 ? 1
				: caseCount <= 0x10000 ? 2 : 4;
		const uint32_t tag = uint32_t(readUInt(memory, address, discWidth));
		const bool isError = tag != 0;
		if (view.kind() == WASM::TypeKind::ComponentResult) {
			if (tag > 1)
				throw std::runtime_error("component runtime: invalid canonical result");
			out.kind = ComponentValue::Kind::Result;
			out.discriminant = tag;
			const WASM::ComponentValueType* payloadType = isError
					? (view.hasResultError() ? &view.resultError() : nullptr)
					: (view.hasResultOk() ? &view.resultOk() : nullptr);
			out.hasValue = payloadType != nullptr;
			if (payloadType != nullptr) {
				const WASM::CanonicalAbiLayout payload = layoutOf(store, *payloadType, memory);
				out.elements.resize(1);
				loadValueAt(store, *payloadType,
						address + alignUp(discWidth, payload.align), memory,
						out.elements.front(), depth + 1);
			}
			return;
		}
		if (tag >= view.cases().size())
			throw std::runtime_error("component runtime: variant case out of range");
		out.kind = ComponentValue::Kind::Variant;
		out.discriminant = tag;
		const WASM::ComponentCase& item = view.cases()[tag];
		out.hasValue = item.hasType;
		if (item.hasType) {
			const WASM::CanonicalAbiLayout payload = layoutOf(store, item.type, memory);
			out.elements.resize(1);
			loadValueAt(store, item.type,
					address + alignUp(discWidth, payload.align), memory,
					out.elements.front(), depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentMap: {
		const uint64_t data = readUInt(memory, address, memory.options.pointerSize);
		const uint64_t length = readUInt(memory,
				address + memory.options.pointerSize, memory.options.pointerSize);
		if (length > (uint64_t{1} << 30))
			throw std::runtime_error("component runtime: map length too large");
		const MapPairLayout pair = mapPairLayout(store, view.mapKey(),
				view.mapValue(), memory);
		ensureRange(memory, data, pair.size * length);
		out.kind = ComponentValue::Kind::Map;
		out.elements.resize(std::size_t(length));
		for (std::size_t i = 0; i < out.elements.size(); ++i) {
			out.elements[i].kind = ComponentValue::Kind::Tuple;
			out.elements[i].elements.resize(2);
			const uint64_t entryAddress = data + uint64_t(i) * pair.size;
			loadValueAt(store, view.mapKey(), entryAddress, memory,
					out.elements[i].elements[0], depth + 1);
			loadValueAt(store, view.mapValue(),
					entryAddress + pair.valueOffset, memory,
					out.elements[i].elements[1], depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow:
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture: {
		const uint32_t handle = uint32_t(readUInt(memory, address, 4));
		if ((view.kind() == WASM::TypeKind::ComponentOwn
				|| view.kind() == WASM::TypeKind::ComponentBorrow)
				&& memory.runtime != nullptr)
			memory.runtime->validateResourceHandle(handle,
					WASM::TypeId{view.typeRef()});
		out.kind = view.kind() == WASM::TypeKind::ComponentOwn
				? ComponentValue::Kind::Own
				: view.kind() == WASM::TypeKind::ComponentBorrow
						? ComponentValue::Kind::Borrow
						: view.kind() == WASM::TypeKind::ComponentStream
								? ComponentValue::Kind::Stream
								: ComponentValue::Kind::Future;
		out.discriminant = handle;
		return;
	}
	default:
		throw std::runtime_error(
				"component runtime: this value cannot be loaded from canonical memory");
	}
}

struct ResourceClosureData {
	std::function<void(void*, void**)> handler;
	ffi_cif* cif = nullptr;
	std::vector<ffi_type*> argumentTypes;
};

void resourceCallback(ffi_cif*, void* result, void** args, void* userData)
{
	static_cast<ResourceClosureData*>(userData)->handler(result, args);
}

// Prepares the libffi closure for a canonical builtin and publishes it as the
// next core function. The caller owns the handler and argument types.
void pushBuiltinClosure(std::vector<void*>& closures, std::vector<void*>& cifs,
		std::vector<void*>& dataSlots, std::vector<WASM::Callable>& coreFuncs,
		std::unique_ptr<ResourceClosureData> data, ffi_type* returnType)
{
	data->cif = new ffi_cif;
	if (ffi_prep_cif(data->cif, FFI_DEFAULT_ABI,
			static_cast<unsigned>(data->argumentTypes.size()), returnType,
			data->argumentTypes.data()) != FFI_OK) {
		delete data->cif;
		throw std::runtime_error("component runtime: builtin ffi_prep_cif failed");
	}
	void* code = nullptr;
	ffi_closure* closure = static_cast<ffi_closure*>(
			ffi_closure_alloc(sizeof(ffi_closure), &code));
	if (!closure || !code || ffi_prep_closure_loc(closure, data->cif,
			resourceCallback, data.get(), code) != FFI_OK) {
		if (closure)
			ffi_closure_free(closure);
		delete data->cif;
		throw std::runtime_error(
				"component runtime: builtin closure allocation failed");
	}
	WASM::Callable callable;
	callable.fnPtr = code;
	callable.context = nullptr;
	callable.localTypeIdx = 0;
	callable.typeId = WASM::TypeId{WASM::TypeId::kNone};
	coreFuncs.push_back(callable);
	closures.push_back(closure);
	cifs.push_back(data->cif);
	dataSlots.push_back(data.release());
}

struct LowerData {
	ComponentFunction function;
	MemView memory;
	ffi_cif* cif = nullptr;
	std::vector<ffi_type*> argumentTypes;
};

void lowerCallback(ffi_cif*, void* result, void** args, void* userData)
{
	auto* data = static_cast<LowerData*>(userData);
	const uint32_t borrowScope = data->memory.runtime
			? data->memory.runtime->beginBorrowScope() : 0;
	data->memory.borrowScope = borrowScope;
	struct BorrowScopeGuard {
		ComponentRuntime* runtime;
		uint32_t scope;
		~BorrowScopeGuard() { if (runtime) runtime->endBorrowScope(scope); }
	} borrowGuard{data->memory.runtime, borrowScope};
	const WASM::TypeStore& store = WASM::Store::current().types().store();
	std::vector<WASM::Value> input;
	if (!data->function.indirectParams) {
		input.resize(data->function.params.size());
		for (std::size_t i = 0; i < input.size(); ++i) {
			switch (data->function.params[i]) {
			case WASM::AbiCoreType::I32:
				input[i].kind = WASM::ValueTypeCode::I32;
				input[i].i32 = *static_cast<int32_t*>(args[i + 1]);
				break;
			case WASM::AbiCoreType::I64:
				input[i].kind = WASM::ValueTypeCode::I64;
				input[i].i64 = *static_cast<int64_t*>(args[i + 1]);
				break;
			case WASM::AbiCoreType::F32:
				input[i].kind = WASM::ValueTypeCode::F32;
				input[i].f32 = *static_cast<float*>(args[i + 1]);
				break;
			case WASM::AbiCoreType::F64:
				input[i].kind = WASM::ValueTypeCode::F64;
				input[i].f64 = *static_cast<double*>(args[i + 1]);
				break;
			}
		}
	}
	std::vector<WASM::Value> output(data->function.results.size());
	std::vector<ComponentValue> structuredOutput;
	if (data->function.callValues) {
		std::vector<ComponentValue> structuredInput(data->function.paramTypes.size());
		std::size_t index = 0;
		if (data->function.indirectParams) {
			const uint64_t paramsAddress = data->function.options.pointerSize == 8
					? *static_cast<uint64_t*>(args[1])
					: *static_cast<uint32_t*>(args[1]);
			uint64_t offset = 0;
			for (std::size_t i = 0; i < structuredInput.size(); ++i) {
				const WASM::CanonicalAbiLayout layout = layoutOf(store,
						data->function.paramTypes[i], data->memory);
				offset = alignUp(offset, layout.align);
				loadValueAt(store, data->function.paramTypes[i],
						paramsAddress + offset, data->memory,
						structuredInput[i], 0);
				offset += layout.size;
			}
		} else {
			for (std::size_t i = 0; i < structuredInput.size(); ++i)
				liftValue(store, data->function.paramTypes[i], input, index,
						data->memory, structuredInput[i], 0);
		}
		structuredOutput.resize(data->function.resultTypes.size());
		data->function.callValues(structuredInput, structuredOutput);
		output.clear();
		for (std::size_t i = 0; i < structuredOutput.size(); ++i)
			lowerValue(store, data->function.resultTypes[i], structuredOutput[i],
					data->memory, output, 0);
	} else {
		data->function.call(input, output);
	}
	if (data->function.results.empty())
		return;
	if (output.empty())
		throw std::runtime_error("component runtime: lower produced no result");
	if (data->function.indirectResults) {
		const std::size_t resultIndex = 1 + (data->function.indirectParams
				? 1 : data->function.params.size());
		const uint64_t buffer = data->function.options.pointerSize == 8
				? *static_cast<uint64_t*>(args[resultIndex])
				: *static_cast<uint32_t*>(args[resultIndex]);
		if (!structuredOutput.empty()) {
			uint64_t offset = 0;
			for (std::size_t i = 0; i < structuredOutput.size(); ++i) {
				const WASM::CanonicalAbiLayout layout = layoutOf(store,
						data->function.resultTypes[i], data->memory);
				offset = alignUp(offset, layout.align);
				storeValueAt(store, data->function.resultTypes[i],
						structuredOutput[i], buffer + offset, data->memory, 0);
				offset += layout.size;
			}
			return;
		}
		throw std::runtime_error(
				"component runtime: indirect lower result needs a structured function");
	}
	switch (data->function.results.front()) {
	case WASM::AbiCoreType::I32:
		*static_cast<int32_t*>(result) = output[0].i32;
		break;
	case WASM::AbiCoreType::I64:
		*static_cast<int64_t*>(result) = output[0].i64;
		break;
	case WASM::AbiCoreType::F32:
		*static_cast<float*>(result) = output[0].f32;
		break;
	case WASM::AbiCoreType::F64:
		*static_cast<double*>(result) = output[0].f64;
		break;
	}
}

// ── Component `value` decoding ─────────────────────────────────────────────
// The value grammar is length-delimited and type-directed (Binary.md). The
// runtime uses it for the `value` section and for `start` arguments. Nested
// lists/maps are bounded by both the input and a depth cap.
struct ValueReader {
	const uint8_t* current = nullptr;
	const uint8_t* end = nullptr;

	bool take(uint8_t& out)
	{
		if (current >= end)
			return false;
		out = *current++;
		return true;
	}
	bool takeU32(uint32_t& out)
	{
		out = 0;
		unsigned shift = 0;
		while (current < end && shift < 35) {
			const uint8_t byte = *current++;
			out |= uint32_t(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0)
				return true;
			shift += 7;
		}
		return false;
	}
	bool takeU64(uint64_t& out)
	{
		out = 0;
		unsigned shift = 0;
		while (current < end && shift < 70) {
			const uint8_t byte = *current++;
			out |= uint64_t(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0)
				return true;
			shift += 7;
		}
		return false;
	}
	bool takeS64(int64_t& out)
	{
		uint64_t raw = 0;
		unsigned shift = 0;
		uint8_t byte = 0;
		do {
			if (current >= end || shift >= 70)
				return false;
			byte = *current++;
			raw |= uint64_t(byte & 0x7F) << shift;
			shift += 7;
		} while ((byte & 0x80) != 0);
		if (shift < 64 && (byte & 0x40) != 0)
			raw |= ~uint64_t{0} << shift;
		out = static_cast<int64_t>(raw);
		return true;
	}
	bool takeBytes(uint8_t* out, std::size_t size)
	{
		if (size > std::size_t(end - current))
			return false;
		if (size != 0)
			std::memcpy(out, current, size);
		current += size;
		return true;
	}
};

uint64_t readLittleEndian(ValueReader& reader, unsigned bytes)
{
	uint8_t buffer[8]{};
	if (!reader.takeBytes(buffer, bytes))
		throw std::runtime_error(
				"component runtime: truncated component value");
	uint64_t value = 0;
	for (unsigned i = 0; i < bytes; ++i)
		value |= uint64_t(buffer[i]) << (i * 8);
	return value;
}

void decodeComponentValue(const WASM::TypeStore& store,
		const WASM::ComponentValueType& type, ValueReader& reader,
		ComponentValue& out, unsigned depth)
{
	if (depth > 64)
		throw std::runtime_error(
				"component runtime: component value nesting is too deep");
	using K = ComponentValue::Kind;
	using P = WASM::PrimitiveValueType;

	if (type.kind == WASM::ComponentValueType::Kind::Primitive) {
		switch (type.primitive) {
		case P::Bool: {
			uint8_t byte = 0;
			if (!reader.take(byte) || byte > 1)
				throw std::runtime_error("component runtime: invalid bool value");
			out.kind = K::Bool;
			out.u64 = byte;
			return;
		}
		case P::S8: { int64_t v; if (!reader.takeS64(v)) throw std::runtime_error("truncated s8"); out.kind = K::S8; out.s64 = int8_t(v); return; }
		case P::U8: { uint64_t v; if (!reader.takeU64(v)) throw std::runtime_error("truncated u8"); out.kind = K::U8; out.u64 = uint8_t(v); return; }
		case P::S16: { int64_t v; if (!reader.takeS64(v)) throw std::runtime_error("truncated s16"); out.kind = K::S16; out.s64 = int16_t(v); return; }
		case P::U16: { uint64_t v; if (!reader.takeU64(v)) throw std::runtime_error("truncated u16"); out.kind = K::U16; out.u64 = uint16_t(v); return; }
		case P::S32: { int64_t v; if (!reader.takeS64(v)) throw std::runtime_error("truncated s32"); out.kind = K::S32; out.s64 = int32_t(v); return; }
		case P::U32: { uint64_t v; if (!reader.takeU64(v)) throw std::runtime_error("truncated u32"); out.kind = K::U32; out.u64 = uint32_t(v); return; }
		case P::S64: { int64_t v; if (!reader.takeS64(v)) throw std::runtime_error("truncated s64"); out.kind = K::S64; out.s64 = v; return; }
		case P::U64: { uint64_t v; if (!reader.takeU64(v)) throw std::runtime_error("truncated u64"); out.kind = K::U64; out.u64 = v; return; }
		case P::F32: {
			const uint32_t bits = uint32_t(readLittleEndian(reader, 4));
			out.kind = K::F32;
			float value;
			std::memcpy(&value, &bits, sizeof(value));
			out.f64 = value;
			return;
		}
		case P::F64: {
			const uint64_t bits = readLittleEndian(reader, 8);
			out.kind = K::F64;
			std::memcpy(&out.f64, &bits, sizeof(bits));
			return;
		}
		case P::Char: {
			uint32_t codepoint = 0;
			if (!reader.takeU32(codepoint))
				throw std::runtime_error("component runtime: truncated char value");
			out.kind = K::Char;
			out.discriminant = codepoint;
			return;
		}
		case P::String: {
			uint32_t length = 0;
			if (!reader.takeU32(length)
					|| length > std::size_t(reader.end - reader.current))
				throw std::runtime_error(
						"component runtime: truncated string value");
			out.kind = K::String;
			out.string.assign(reinterpret_cast<const char*>(reader.current),
					length);
			reader.current += length;
			return;
		}
		default:
			throw std::runtime_error(
					"component runtime: unsupported component value primitive");
		}
	}

	if (type.kind != WASM::ComponentValueType::Kind::TypeId)
		throw std::runtime_error(
				"component runtime: value type is not canonical");
	const WASM::ComponentTypeView view =
			store.componentType(WASM::TypeId{type.typeIndex});
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive: {
		WASM::ComponentValueType primitive;
		primitive.kind = WASM::ComponentValueType::Kind::Primitive;
		primitive.primitive = view.primitive();
		decodeComponentValue(store, primitive, reader, out, depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentRecord:
		out.kind = K::Record;
		out.elements.resize(view.fields().size());
		for (std::size_t i = 0; i < view.fields().size(); ++i)
			decodeComponentValue(store, view.fields()[i].type, reader,
					out.elements[i], depth + 1);
		return;
	case WASM::TypeKind::ComponentList:
	case WASM::TypeKind::ComponentFixedList: {
		out.kind = view.kind() == WASM::TypeKind::ComponentList ? K::List
				: K::FixedList;
		uint32_t count = view.fixedLength();
		if (view.kind() == WASM::TypeKind::ComponentList) {
			if (!reader.takeU32(count) || count > (1u << 20))
				throw std::runtime_error(
						"component runtime: invalid list length");
		}
		out.elements.resize(count);
		for (uint32_t i = 0; i < count; ++i)
			decodeComponentValue(store, view.element(), reader, out.elements[i],
					depth + 1);
		return;
	}
	case WASM::TypeKind::ComponentTuple:
		out.kind = K::Tuple;
		out.elements.resize(view.tuple().size());
		for (std::size_t i = 0; i < view.tuple().size(); ++i)
			decodeComponentValue(store, view.tuple()[i], reader,
					out.elements[i], depth + 1);
		return;


	case WASM::TypeKind::ComponentVariant: {
		uint32_t tag = 0;
		if (!reader.takeU32(tag) || tag >= view.cases().size())
			throw std::runtime_error("component runtime: invalid variant tag");
		out.kind = K::Variant;
		out.discriminant = tag;
		const WASM::ComponentCase& item = view.cases()[tag];
		out.hasValue = item.hasType;
		if (item.hasType) {
			out.elements.resize(1);
			decodeComponentValue(store, item.type, reader, out.elements[0],
					depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentEnum: {
		uint32_t tag = 0;
		if (!reader.takeU32(tag) || tag >= view.labels().size())
			throw std::runtime_error("component runtime: invalid enum tag");
		out.kind = K::Enum;
		out.discriminant = tag;
		return;
	}
	case WASM::TypeKind::ComponentFlags: {
		out.kind = K::Flags;
		out.flags.assign(view.labels().size(), false);
		const std::size_t bytes = (view.labels().size() + 7) / 8;
		for (std::size_t i = 0; i < bytes; ++i) {
			uint8_t byte = 0;
			if (!reader.take(byte))
				throw std::runtime_error("component runtime: truncated flags value");
			for (unsigned bit = 0; bit < 8 && i * 8 + bit < out.flags.size(); ++bit)
				out.flags[i * 8 + bit] = (byte & (1u << bit)) != 0;
		}
		return;
	}
	case WASM::TypeKind::ComponentOption: {
		uint8_t tag = 0;
		if (!reader.take(tag) || tag > 1)
			throw std::runtime_error("component runtime: invalid option tag");
		out.kind = K::Option;
		out.hasValue = tag != 0;
		if (out.hasValue) {
			out.elements.resize(1);
			decodeComponentValue(store, view.element(), reader, out.elements[0],
					depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentResult: {
		uint8_t tag = 0;
		if (!reader.take(tag) || tag > 1)
			throw std::runtime_error("component runtime: invalid result tag");
		out.kind = K::Result;
		out.discriminant = tag;
		const bool present = tag == 0 ? view.hasResultOk()
				: view.hasResultError();
		out.hasValue = present;
		if (present) {
			out.elements.resize(1);
			decodeComponentValue(store,
					tag == 0 ? view.resultOk() : view.resultError(), reader,
					out.elements[0], depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentMap: {
		uint32_t count = 0;
		if (!reader.takeU32(count) || count > (1u << 20))
			throw std::runtime_error("component runtime: invalid map length");
		out.kind = K::Map;
		out.elements.resize(count);
		for (uint32_t i = 0; i < count; ++i) {
			out.elements[i].kind = K::Tuple;
			out.elements[i].elements.resize(2);
			decodeComponentValue(store, view.mapKey(), reader,
					out.elements[i].elements[0], depth + 1);
			decodeComponentValue(store, view.mapValue(), reader,
					out.elements[i].elements[1], depth + 1);
		}
		return;
	}
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow:
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture: {
		uint32_t handle = 0;
		if (!reader.takeU32(handle))
			throw std::runtime_error("component runtime: truncated handle value");
		out.kind = view.kind() == WASM::TypeKind::ComponentOwn ? K::Own
				: view.kind() == WASM::TypeKind::ComponentBorrow ? K::Borrow
				: view.kind() == WASM::TypeKind::ComponentStream ? K::Stream
				: K::Future;
		out.discriminant = handle;
		return;
	}
	default:
		throw std::runtime_error(
				"component runtime: unsupported component value shape");
	}
}


} // namespace

ComponentRuntime::ComponentRuntime(ModuleInstantiator& instantiator,
		const WASM::Component& component, ComponentImports imports)
	: instantiator_(instantiator), component_(component),
	  imports_(std::move(imports)),
	  async_([this] { return addHandle(); },
			  [this](uint32_t handle) { freeHandle(handle); }),
	  abi_(WASM::Store::current().types().store(),
			  nextComponentAbiInstanceId())
{
	build();
}

ComponentRuntime::~ComponentRuntime()
{
	for (void* closure : lowerClosures_)
		if (closure)
			ffi_closure_free(closure);
	for (void* cif : lowerCifs_)
		delete static_cast<ffi_cif*>(cif);
	for (void* data : lowerData_)
		delete static_cast<LowerData*>(data);
	for (void* closure : resourceClosures_)
		if (closure)
			ffi_closure_free(closure);
	for (void* cif : resourceCifs_)
		delete static_cast<ffi_cif*>(cif);
	for (void* data : resourceData_)
		delete static_cast<ResourceClosureData*>(data);
}

void ComponentRuntime::build()
{
	const WASM::TypeStore& store = WASM::Store::current().types().store();
	componentValues_.clear();
	componentValues_.reserve(component_.values.size());
	for (const WASM::ValueDecl& value : component_.values) {
		ValueReader reader{value.encoded.data(),
				value.encoded.data() + value.encoded.size()};
		ComponentValue decoded;
		decodeComponentValue(store, resolveValueType(value.type), reader,
				decoded, 0);
		if (reader.current != reader.end)
			throw std::runtime_error(
					"component runtime: component value has trailing bytes");
		componentValues_.push_back(std::move(decoded));
	}
	for (const WASM::ComponentDefinition& definition : component_.definitions) {
		switch (definition.kind) {
		case WASM::ComponentDefinitionKind::CoreInstance:
			processCoreInstance(component_.coreInstances[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Component:
			processComponent(definition.index);
			break;
		case WASM::ComponentDefinitionKind::Instance:
			processComponentInstance(component_.instances[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Alias:
			processAlias(component_.aliases[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Canon:
			processCanon(component_.canons[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Import:
			processImport(component_.imports[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Export:
			processExport(component_.exports[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Start:
			processStart(component_.starts[definition.index]);
			break;
		default:
			break;
		}
	}
}

std::optional<WASM::Callable> ComponentRuntime::CoreInstanceHandle::exportedFunction(
		std::string_view name) const
{
	if (moduleInstance)
		return moduleInstance->exportedFunction(name);
	const auto it = inlineFunctions.find(std::string(name));
	if (it == inlineFunctions.end())
		return std::nullopt;
	return it->second;
}

std::optional<WASM::LinearMemory*> ComponentRuntime::CoreInstanceHandle::exportedMemory(
		std::string_view name) const
{
	if (moduleInstance)
		return moduleInstance->exportedMemory(name);
	const auto it = inlineMemories.find(std::string(name));
	if (it == inlineMemories.end())
		return std::nullopt;
	return it->second;
}

void ComponentRuntime::CoreInstanceHandle::registerExports(
		WASM::ImportRegistrar& registrar, std::string_view moduleName) const
{
	if (moduleInstance) {
		moduleInstance->registerExports(registrar, moduleName);
		return;
	}
	for (const auto& [name, callable] : inlineFunctions)
		registrar.registerFunction(moduleName, name, callable);
	for (const auto& [name, memory] : inlineMemories)
		registrar.registerMemory(moduleName, name, memory);
}

void ComponentRuntime::processCoreInstance(const WASM::CoreInstance& instance)
{
	if (instance.kind == WASM::CoreInstance::Kind::InlineExports) {
		auto handle = std::make_unique<CoreInstanceHandle>();
		for (const WASM::CoreInlineExport& exportDecl : instance.exports) {
			if (exportDecl.sortIndex.sort == WASM::CoreSort::Func) {
				if (exportDecl.sortIndex.index >= coreFuncs_.size())
					throw std::runtime_error(
							"component runtime: inline core func export is out of bounds");
				handle->inlineFunctions.emplace(exportDecl.name,
						coreFuncs_[exportDecl.sortIndex.index]);
			} else if (exportDecl.sortIndex.sort == WASM::CoreSort::Memory) {
				if (exportDecl.sortIndex.index >= coreMemories_.size())
					throw std::runtime_error(
							"component runtime: inline core memory export is out of bounds");
				handle->inlineMemories.emplace(exportDecl.name,
						coreMemories_[exportDecl.sortIndex.index]);
			} else {
				throw std::runtime_error(
						"component runtime: inline core export sort is not supported");
			}
		}
		coreInstances_.push_back(std::move(handle));
		return;
	}
	if (instance.moduleIndex >= component_.coreModules.size())
		throw std::runtime_error(
				"component runtime: core instance module index out of bounds");
	WASM::RegistryImportResolver resolver;
	for (const WASM::CoreInstantiateArg& arg : instance.args) {
		if (arg.instanceIndex >= coreInstances_.size())
			throw std::runtime_error(
					"component runtime: core instantiate argument index out of bounds");
		coreInstances_[arg.instanceIndex]->registerExports(resolver, arg.name);
	}
	auto moduleInstance = instantiator_.instantiate(
			*component_.coreModules[instance.moduleIndex], resolver);
	if (!moduleInstance)
		throw std::runtime_error("component runtime: core instantiation failed");
	auto handle = std::make_unique<CoreInstanceHandle>();
	handle->moduleInstance = std::move(moduleInstance);
	coreInstances_.push_back(std::move(handle));
}


void ComponentRuntime::processComponent(uint32_t index)
{
	if (index >= component_.components.size())
		throw std::runtime_error(
				"component runtime: nested component index out of bounds");
	nestedComponents_.push_back(component_.components[index].get());
}

void ComponentRuntime::processComponentInstance(
		const WASM::ComponentInstance& instance)
{
	if (instance.kind != WASM::ComponentInstance::Kind::Instantiate)
		throw std::runtime_error(
				"component runtime: inline component instances are not supported");
	if (instance.componentIndex >= nestedComponents_.size())
		throw std::runtime_error(
				"component runtime: component instance target is not a nested component");
	ComponentImports childImports = imports_;
	for (const WASM::InstantiateArg& arg : instance.args) {
		if (arg.sortIndex.sort != WASM::ComponentSort::Func
				|| arg.sortIndex.index >= componentFuncs_.size())
			throw std::runtime_error(
					"component runtime: only function instantiate arguments are supported");
		childImports[arg.name] = componentFuncs_[arg.sortIndex.index];
	}
	nestedRuntimes_.push_back(std::make_unique<ComponentRuntime>(
			instantiator_, *nestedComponents_[instance.componentIndex],
			std::move(childImports)));
}

void ComponentRuntime::processAlias(const WASM::Alias& alias)
{
	if (alias.kind == WASM::Alias::Kind::Export) {
		if (alias.sort.sort != WASM::ComponentSort::Func)
			throw std::runtime_error(
					"component runtime: only component function export aliases are supported");
		if (alias.instanceIndex >= nestedRuntimes_.size())
			throw std::runtime_error(
					"component runtime: component alias instance index out of bounds");
		const auto function = nestedRuntimes_[alias.instanceIndex]
				->exportedFunction(alias.name);
		if (!function)
			throw std::runtime_error(
					"component runtime: nested component export is not a function");
		componentFuncs_.push_back(*function);
		return;
	}
	if (alias.kind != WASM::Alias::Kind::CoreExport)
		return;
	if (alias.sort.sort != WASM::ComponentSort::Core || !alias.sort.core)
		return;
	if (alias.instanceIndex >= coreInstances_.size())
		throw std::runtime_error(
				"component runtime: core alias instance index out of bounds");
	if (alias.sort.core->sort == WASM::CoreSort::Memory) {
		const auto memory = coreInstances_[alias.instanceIndex]
				->exportedMemory(alias.name);
		if (!memory)
			throw std::runtime_error(
					"component runtime: core export is not a memory");
		coreMemories_.push_back(*memory);
		return;
	}
	if (alias.sort.core->sort != WASM::CoreSort::Func)
		return;
	const std::optional<WASM::Callable> callable =
			coreInstances_[alias.instanceIndex]->exportedFunction(alias.name);
	if (!callable)
		throw std::runtime_error(
				"component runtime: core export is not a function");
	coreFuncs_.push_back(*callable);
}

void ComponentRuntime::processStreamCanon(const WASM::Canon& canon)
{
	// `stream.new`/`future.new` and the drop builtins are the endpoint-lifecycle
	// half of the async surface. Read/write/cancel/forward live in
	// processAsyncCanon because they need the canonical memory and ABI layout.
	const bool isNew = canon.kind == WASM::Canon::Kind::StreamNew
			|| canon.kind == WASM::Canon::Kind::FutureNew;
	const bool isFuture = canon.kind == WASM::Canon::Kind::FutureNew
			|| canon.kind == WASM::Canon::Kind::FutureDropReadable
			|| canon.kind == WASM::Canon::Kind::FutureDropWritable;
	const bool readable = canon.kind == WASM::Canon::Kind::StreamDropReadable
			|| canon.kind == WASM::Canon::Kind::FutureDropReadable;

	uint32_t elementSize = 0;
	if (const WASM::TypeId typeId = component_.typeId(canon.typeIndex);
			!typeId.isNone()) {
		const WASM::TypeStore& store = WASM::Store::current().types().store();
		const WASM::ComponentTypeView view = store.componentType(typeId);
		const WASM::CanonicalAbiOptions options = canonicalOptions(canon);
		if (view.hasStreamElement()) {
			WASM::CanonicalAbiLayout layout;
			if (store.canonicalAbiLayoutValue(view.streamElement(), options,
					layout))
				elementSize = layout.size;
		}
	}

	auto data = std::make_unique<ResourceClosureData>();
	ffi_type* returnType = &ffi_type_void;
	if (isNew) {
		returnType = &ffi_type_sint64;
		data->handler = [this, elementSize, isFuture](void* result, void**) {
			const auto handles = async_.newStreamFuture(isFuture, elementSize);
			*static_cast<int64_t*>(result) = int64_t(
					uint64_t(handles.first) | (uint64_t(handles.second) << 32));
		};
	} else {
		data->argumentTypes.push_back(&ffi_type_pointer);
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this, readable](void*, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			async_.drop(handle, readable ? WASM::ComponentEndDirection::Readable
					: WASM::ComponentEndDirection::Writable);
		};
	}
	pushBuiltinClosure(resourceClosures_, resourceCifs_, resourceData_,
			coreFuncs_, std::move(data), returnType);
}



void ComponentRuntime::processAsyncCanon(const WASM::Canon& canon)
{
	const WASM::CanonicalAbiOptions options = canonicalOptions(canon);
	const bool pointer64 = options.pointerSize == 8;
	ffi_type* const addressType = ffiTypeForAddress(pointer64);
	auto data = std::make_unique<ResourceClosureData>();
	data->argumentTypes.push_back(&ffi_type_pointer); // VMContext*
	ffi_type* returnType = &ffi_type_void;

	switch (canon.kind) {
	case WASM::Canon::Kind::StreamRead:
	case WASM::Canon::Kind::StreamWrite:
	case WASM::Canon::Kind::FutureRead:
	case WASM::Canon::Kind::FutureWrite: {
		const bool isFuture = canon.kind == WASM::Canon::Kind::FutureRead
				|| canon.kind == WASM::Canon::Kind::FutureWrite;
		const bool isRead = canon.kind == WASM::Canon::Kind::StreamRead
				|| canon.kind == WASM::Canon::Kind::FutureRead;
		uint32_t elementSize = 0;
		const WASM::TypeId typeId = component_.typeId(canon.typeIndex);
		if (!typeId.isNone()) {
			const WASM::TypeStore& store = WASM::Store::current().types().store();
			const WASM::ComponentTypeView view = store.componentType(typeId);
			if (view.hasStreamElement()) {
				WASM::CanonicalAbiLayout layout;
				if (store.canonicalAbiLayoutValue(view.streamElement(), options,
						layout))
					elementSize = layout.size;
			}
		}
		data->argumentTypes.push_back(&ffi_type_sint32); // end handle
		data->argumentTypes.push_back(addressType);     // buffer address
		if (!isFuture)
			data->argumentTypes.push_back(addressType); // element count
		returnType = addressType;
		data->handler = [this, isFuture, isRead, elementSize, options,
				pointer64](void* result, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			const uint64_t address = readAddress(args[2], pointer64);
			const uint64_t count = isFuture ? 1
					: readAddress(args[3], pointer64);
			const AbiMemory abi = memoryFor(options);
			if (abi.memory == nullptr)
				throw std::runtime_error(
						"component async: stream/future builtin needs a memory");
			const uint32_t packed = isRead
					? async_.read(handle, *abi.memory, address, count,
							options.async)
					: async_.write(handle, *abi.memory, address, count,
							options.async);
			(void)elementSize;
			writeAddress(result, packed, pointer64);
		};
		break;
	}
	case WASM::Canon::Kind::StreamCancelRead:
	case WASM::Canon::Kind::StreamCancelWrite:
	case WASM::Canon::Kind::FutureCancelRead:
	case WASM::Canon::Kind::FutureCancelWrite:
		data->argumentTypes.push_back(&ffi_type_sint32);
		returnType = &ffi_type_sint32;
		data->handler = [this, options](void* result, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			*static_cast<int32_t*>(result) =
					int32_t(async_.cancel(handle, options.async));
		};
		break;
	case WASM::Canon::Kind::StreamForward:
	case WASM::Canon::Kind::FutureForward:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this](void*, void** args) {
			const uint32_t readable = uint32_t(*static_cast<int32_t*>(args[1]));
			const uint32_t writable = uint32_t(*static_cast<int32_t*>(args[2]));
			async_.forward(readable, writable);
			async_.discard(readable);
			async_.discard(writable);
		};
		break;
	case WASM::Canon::Kind::TaskReturn: {
		// `task.return` takes the flattened results of the current task. The
		// single-agent runtime records that the task resolved; it does not need
		// to re-materialise them because the caller's values are already in the
		// core frame.
		const WASM::TypeStore& store = WASM::Store::current().types().store();
		for (const WASM::ComponentValueType& value : canon.results) {
			const WASM::ComponentValueType resolved = resolveValueType(value);
			std::vector<WASM::AbiCoreType> flat;
			if (!store.canonicalAbiFlatValue(resolved, flat))
				throw std::runtime_error(
						"component async: task.return result is not flattenable");
			for (WASM::AbiCoreType type : flat)
				data->argumentTypes.push_back(ffiType(type));
		}
		data->handler = [this](void*, void**) { async_.taskReturn(); };
		break;
	}
	case WASM::Canon::Kind::TaskCancel:
		data->handler = [this](void*, void**) { async_.taskCancel(); };
		break;
	case WASM::Canon::Kind::ContextGet: {
		const WASM::AbiCoreType type =
				abiCoreTypeForValue(canon.contextType.opcode);
		returnType = ffiType(type);
		data->handler = [this, type, index = canon.contextIndex](void* result,
				void**) {
			const WASM::ComponentContextSlot slot =
					async_.currentTask().contextSlot(index);
			switch (type) {
			case WASM::AbiCoreType::I32: *static_cast<int32_t*>(result) = slot.i32; break;
			case WASM::AbiCoreType::I64: *static_cast<int64_t*>(result) = slot.i64; break;
			case WASM::AbiCoreType::F32: *static_cast<float*>(result) = slot.f32; break;
			case WASM::AbiCoreType::F64: *static_cast<double*>(result) = slot.f64; break;
			}
		};
		break;
	}
	case WASM::Canon::Kind::ContextSet: {
		const WASM::AbiCoreType type =
				abiCoreTypeForValue(canon.contextType.opcode);
		data->argumentTypes.push_back(ffiType(type));
		data->handler = [this, type, index = canon.contextIndex](void*,
				void** args) {
			WASM::ComponentContextSlot slot;
			switch (type) {
			case WASM::AbiCoreType::I32: slot.i32 = *static_cast<int32_t*>(args[1]); break;
			case WASM::AbiCoreType::I64: slot.i64 = *static_cast<int64_t*>(args[1]); break;
			case WASM::AbiCoreType::F32: slot.f32 = *static_cast<float*>(args[1]); break;
			case WASM::AbiCoreType::F64: slot.f64 = *static_cast<double*>(args[1]); break;
			}
			async_.currentTask().setContextSlot(index, slot);
		};
		break;
	}
	case WASM::Canon::Kind::BackpressureInc:
	case WASM::Canon::Kind::BackpressureDec:
		data->handler = [this, increment = canon.kind
				== WASM::Canon::Kind::BackpressureInc](void*, void**) {
			async_.setBackpressure(increment);
		};
		break;
	case WASM::Canon::Kind::WaitableSetNew:
		returnType = &ffi_type_sint32;
		data->handler = [this](void* result, void**) {
			*static_cast<int32_t*>(result) =
					int32_t(async_.newWaitableSet());
		};
		break;
	case WASM::Canon::Kind::WaitableSetWait:
	case WASM::Canon::Kind::WaitableSetPoll: {
		// The memory immediate is not a canonopt for this builtin.
		WASM::CanonicalAbiOptions waitOptions = options;
		waitOptions.memory = canon.memoryIndex;
		const bool isWait = canon.kind == WASM::Canon::Kind::WaitableSetWait;
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->argumentTypes.push_back(addressType);
		returnType = &ffi_type_sint32;
		data->handler = [this, waitOptions, pointer64, isWait](void* result,
				void** args) {
			const uint32_t set = uint32_t(*static_cast<int32_t*>(args[1]));
			const uint64_t address = readAddress(args[2], pointer64);
			const WASM::ComponentAsyncEvent event = isWait
					? async_.wait(set) : async_.poll(set);
			const AbiMemory abi = memoryFor(waitOptions);
			if (abi.memory == nullptr || address + 8 > abi.memory->memorySize)
				throw std::runtime_error(
						"component async: waitable-set payload address is out of bounds");
			const auto store32 = [&](uint32_t offset, uint32_t value) {
				for (unsigned i = 0; i < 4; ++i)
					abi.memory->memoryBase[address + offset + i] =
							uint8_t(value >> (i * 8));
			};
			store32(0, event.payload1);
			store32(4, event.payload2);
			*static_cast<int32_t*>(result) =
					static_cast<int32_t>(event.code);
		};
		break;
	}
	case WASM::Canon::Kind::WaitableSetDrop:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this](void*, void** args) {
			async_.dropWaitableSet(uint32_t(*static_cast<int32_t*>(args[1])));
		};
		break;
	case WASM::Canon::Kind::WaitableJoin:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this](void*, void** args) {
			async_.join(uint32_t(*static_cast<int32_t*>(args[1])),
					uint32_t(*static_cast<int32_t*>(args[2])));
		};
		break;
	case WASM::Canon::Kind::SubtaskCancel:
		data->argumentTypes.push_back(&ffi_type_sint32);
		returnType = &ffi_type_sint32;
		data->handler = [this](void* result, void** args) {
			*static_cast<int32_t*>(result) = int32_t(async_.cancelSubtask(
					uint32_t(*static_cast<int32_t*>(args[1]))));
		};
		break;
	case WASM::Canon::Kind::SubtaskDrop:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this](void*, void** args) {
			async_.dropSubtask(uint32_t(*static_cast<int32_t*>(args[1])));
		};
		break;
	default:
		throw std::runtime_error(
				"component runtime: processAsyncCanon received a non-stream canon");
	}
	pushBuiltinClosure(resourceClosures_, resourceCifs_, resourceData_,
			coreFuncs_, std::move(data), returnType);
}


uint32_t ComponentRuntime::addHandle()
{
	if (!freeResourceSlots_.empty()) {
		const uint32_t handle = freeResourceSlots_.back();
		freeResourceSlots_.pop_back();
		resourceSlots_[handle] = ResourceSlot{0, WASM::TypeId::kNone,
				true, true, 0};
		return handle;
	}
	const uint32_t handle = static_cast<uint32_t>(resourceSlots_.size());
	resourceSlots_.push_back(ResourceSlot{0, WASM::TypeId::kNone,
			true, true, 0});
	return handle;
}

void ComponentRuntime::freeHandle(uint32_t handle)
{
	if (handle == 0 || handle >= resourceSlots_.size())
		return;
	if (!resourceSlots_[handle].live)
		return;
	resourceSlots_[handle] = ResourceSlot{};
	freeResourceSlots_.push_back(handle);
}


void ComponentRuntime::processResourceCanon(const WASM::Canon& canon)
{
	const WASM::TypeId typeId = component_.typeId(canon.typeIndex);
	if (typeId.isNone())
		throw std::runtime_error("component runtime: resource type has no identity");
	const WASM::ComponentTypeView view =
			WASM::Store::current().types().store().componentType(typeId);
	if (view.kind() != WASM::TypeKind::ComponentResource)
		throw std::runtime_error("component runtime: canon resource type is not a resource");
	const WASM::ValueTypeCode repCode = view.resource().rep.opcode;
	const WASM::AbiCoreType rep = repCode == WASM::ValueTypeCode::I32
			? WASM::AbiCoreType::I32
			: repCode == WASM::ValueTypeCode::I64 ? WASM::AbiCoreType::I64
			: repCode == WASM::ValueTypeCode::F32 ? WASM::AbiCoreType::F32
			: repCode == WASM::ValueTypeCode::F64 ? WASM::AbiCoreType::F64
			: throw std::runtime_error(
					"component runtime: resource representation is not numeric");

	auto data = std::make_unique<ResourceClosureData>();
	data->argumentTypes.push_back(&ffi_type_pointer);
	ffi_type* returnType = &ffi_type_void;
	if (canon.kind == WASM::Canon::Kind::ResourceNew) {
		data->argumentTypes.push_back(ffiType(rep));
		returnType = &ffi_type_sint32;
	} else {
		data->argumentTypes.push_back(&ffi_type_sint32);
		if (canon.kind == WASM::Canon::Kind::ResourceRep)
			returnType = ffiType(rep);
	}
	const auto readRep = [rep](void* pointer) -> uint64_t {
		switch (rep) {
		case WASM::AbiCoreType::I32: return *static_cast<uint32_t*>(pointer);
		case WASM::AbiCoreType::I64: return *static_cast<uint64_t*>(pointer);
		case WASM::AbiCoreType::F32: {
			uint32_t bits;
			std::memcpy(&bits, pointer, 4);
			return bits;
		}
		case WASM::AbiCoreType::F64: {
			uint64_t bits;
			std::memcpy(&bits, pointer, 8);
			return bits;
		}
		}
		return 0;
	};
	const auto writeRep = [rep](void* destination, uint64_t value) {
		switch (rep) {
		case WASM::AbiCoreType::I32: *static_cast<uint32_t*>(destination) = uint32_t(value); break;
		case WASM::AbiCoreType::I64: *static_cast<uint64_t*>(destination) = value; break;
		case WASM::AbiCoreType::F32: std::memcpy(destination, &value, 4); break;
		case WASM::AbiCoreType::F64: std::memcpy(destination, &value, 8); break;
		}
	};
	if (canon.kind == WASM::Canon::Kind::ResourceNew) {
		data->handler = [this, readRep, typeId](void* result, void** args) {
			const uint64_t representation = readRep(args[1]);
			uint32_t handle;
			if (!freeResourceSlots_.empty()) {
				handle = freeResourceSlots_.back();
				freeResourceSlots_.pop_back();
				resourceSlots_[handle] = ResourceSlot{representation, typeId,
						true, true, 0};
			} else {
				handle = static_cast<uint32_t>(resourceSlots_.size());
				resourceSlots_.push_back(ResourceSlot{representation, typeId,
						true, true, 0});
			}
			*static_cast<int32_t*>(result) = int32_t(handle);
		};
	} else if (canon.kind == WASM::Canon::Kind::ResourceRep) {
		data->handler = [this, writeRep, typeId](void* result, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			if (handle >= resourceSlots_.size() || !resourceSlots_[handle].live
					|| resourceSlots_[handle].typeId != typeId)
				throw std::runtime_error("component runtime: invalid resource handle");
			writeRep(result, resourceSlots_[handle].representation);
		};
	} else {
		const bool hasDestructor = view.resource().hasDestructor;
		const uint32_t destructor = view.resource().destructor;
		data->handler = [this, rep, hasDestructor, destructor, typeId](void*, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			if (handle == 0 || handle >= resourceSlots_.size()
					|| !resourceSlots_[handle].live
					|| resourceSlots_[handle].typeId != typeId)
				throw std::runtime_error("component runtime: invalid resource handle");
			if (!resourceSlots_[handle].own)
				throw std::runtime_error("component runtime: cannot drop a borrowed handle");
			const uint64_t representation = resourceSlots_[handle].representation;
			if (hasDestructor && destructor < coreFuncs_.size()) {
				ComponentFunction drop;
				drop.params = {rep};
				WASM::Value value{};
				value.kind = rep == WASM::AbiCoreType::I32
						? WASM::ValueTypeCode::I32
						: rep == WASM::AbiCoreType::I64
								? WASM::ValueTypeCode::I64
								: rep == WASM::AbiCoreType::F32
										? WASM::ValueTypeCode::F32
										: WASM::ValueTypeCode::F64;
				if (rep == WASM::AbiCoreType::I32)
					value.u32 = uint32_t(representation);
				else if (rep == WASM::AbiCoreType::I64)
					value.ui64 = representation;
				else if (rep == WASM::AbiCoreType::F32) {
					const uint32_t bits = uint32_t(representation);
					std::memcpy(&value.f32, &bits, 4);
				} else {
					std::memcpy(&value.f64, &representation, 8);
				}
				invokeCore(coreFuncs_[destructor], drop,
						std::span<const WASM::Value>(&value, 1), {});
			}
			resourceSlots_[handle] = ResourceSlot{};
			freeResourceSlots_.push_back(handle);
		};
	}
	data->cif = new ffi_cif;
	if (ffi_prep_cif(data->cif, FFI_DEFAULT_ABI,
			static_cast<unsigned>(data->argumentTypes.size()), returnType,
			data->argumentTypes.data()) != FFI_OK)
		throw std::runtime_error("component runtime: resource ffi_prep_cif failed");
	void* code = nullptr;
	ffi_closure* closure = static_cast<ffi_closure*>(
			ffi_closure_alloc(sizeof(ffi_closure), &code));
	if (!closure || !code || ffi_prep_closure_loc(closure, data->cif,
			resourceCallback, data.get(), code) != FFI_OK) {
		if (closure)
			ffi_closure_free(closure);
		delete data->cif;
		data->cif = nullptr;
		throw std::runtime_error("component runtime: resource closure allocation failed");
	}
	WASM::Callable callable;
	callable.fnPtr = code;
	callable.context = nullptr;
	callable.localTypeIdx = 0;
	callable.typeId = WASM::TypeId{WASM::TypeId::kNone};
	coreFuncs_.push_back(callable);
	resourceClosures_.push_back(closure);
	resourceCifs_.push_back(data->cif);
	resourceData_.push_back(data.release());
}

void ComponentRuntime::processCanon(const WASM::Canon& canon)
{
	if (canon.kind == WASM::Canon::Kind::ResourceNew
			|| canon.kind == WASM::Canon::Kind::ResourceDrop
			|| canon.kind == WASM::Canon::Kind::ResourceRep) {
		processResourceCanon(canon);
		return;
	}
	if (canon.kind == WASM::Canon::Kind::StreamNew
			|| canon.kind == WASM::Canon::Kind::FutureNew
			|| canon.kind == WASM::Canon::Kind::StreamDropReadable
			|| canon.kind == WASM::Canon::Kind::StreamDropWritable
			|| canon.kind == WASM::Canon::Kind::FutureDropReadable
			|| canon.kind == WASM::Canon::Kind::FutureDropWritable) {
		processStreamCanon(canon);
		return;
	}
	switch (canon.kind) {
	case WASM::Canon::Kind::StreamRead:
	case WASM::Canon::Kind::StreamWrite:
	case WASM::Canon::Kind::StreamCancelRead:
	case WASM::Canon::Kind::StreamCancelWrite:
	case WASM::Canon::Kind::StreamForward:
	case WASM::Canon::Kind::FutureRead:
	case WASM::Canon::Kind::FutureWrite:
	case WASM::Canon::Kind::FutureCancelRead:
	case WASM::Canon::Kind::FutureCancelWrite:
	case WASM::Canon::Kind::FutureForward:
	case WASM::Canon::Kind::TaskReturn:
	case WASM::Canon::Kind::TaskCancel:
	case WASM::Canon::Kind::ContextGet:
	case WASM::Canon::Kind::ContextSet:
	case WASM::Canon::Kind::BackpressureInc:
	case WASM::Canon::Kind::BackpressureDec:
	case WASM::Canon::Kind::WaitableSetNew:
	case WASM::Canon::Kind::WaitableSetWait:
	case WASM::Canon::Kind::WaitableSetPoll:
	case WASM::Canon::Kind::WaitableSetDrop:
	case WASM::Canon::Kind::WaitableJoin:
	case WASM::Canon::Kind::SubtaskCancel:
	case WASM::Canon::Kind::SubtaskDrop:
		processAsyncCanon(canon);
		return;
	case WASM::Canon::Kind::ErrorContextNew:
	case WASM::Canon::Kind::ErrorContextDebugMessage:
	case WASM::Canon::Kind::ErrorContextDrop:
		processErrorContextCanon(canon);
		return;
	case WASM::Canon::Kind::ThreadIndex:
	case WASM::Canon::Kind::ThreadNewIndirect:
	case WASM::Canon::Kind::ThreadResumeLater:
	case WASM::Canon::Kind::ThreadSuspend:
	case WASM::Canon::Kind::ThreadYield:
	case WASM::Canon::Kind::ThreadSuspendThenResume:
	case WASM::Canon::Kind::ThreadYieldThenResume:
	case WASM::Canon::Kind::ThreadSuspendThenPromote:
	case WASM::Canon::Kind::ThreadYieldThenPromote:
	case WASM::Canon::Kind::ThreadSpawnRef:
	case WASM::Canon::Kind::ThreadSpawnIndirect:
	case WASM::Canon::Kind::ThreadAvailableParallelism:
		processThreadCanon(canon);
		return;
	default:
		break;
	}
	if (canon.kind == WASM::Canon::Kind::Lower) {
		if (canon.funcIndex >= componentFuncs_.size())
			throw std::runtime_error(
					"component runtime: canon lower function index out of bounds");
		auto data = std::make_unique<LowerData>();
		data->function = componentFuncs_[canon.funcIndex];
		data->function.options = canonicalOptions(canon);
		const AbiMemory abiMemory = memoryFor(data->function.options);
		data->memory = MemView{abiMemory.memory, abiMemory.realloc,
				data->function.options, this, 0};
		const bool indirectParams = data->function.indirectParams;
		const bool indirectResults = data->function.indirectResults;
		data->argumentTypes.push_back(&ffi_type_pointer);
		if (indirectParams) {
			data->argumentTypes.push_back(data->function.options.pointerSize == 8
					? &ffi_type_sint64 : &ffi_type_sint32);
		} else {
			for (WASM::AbiCoreType type : data->function.params)
				data->argumentTypes.push_back(ffiType(type));
		}
		if (indirectResults)
			data->argumentTypes.push_back(&ffi_type_pointer);
		data->cif = new ffi_cif;
		ffi_type* returnType = data->function.results.empty() || indirectResults
				? &ffi_type_void : ffiType(data->function.results.front());
		if (ffi_prep_cif(data->cif, FFI_DEFAULT_ABI,
				static_cast<unsigned>(data->argumentTypes.size()), returnType,
				data->argumentTypes.data()) != FFI_OK)
			throw std::runtime_error("component runtime: ffi_prep_cif failed");
		void* code = nullptr;
		ffi_closure* closure = static_cast<ffi_closure*>(
				ffi_closure_alloc(sizeof(ffi_closure), &code));
		if (!closure || !code
				|| ffi_prep_closure_loc(closure, data->cif, lowerCallback,
						data.get(), code) != FFI_OK) {
			if (closure)
				ffi_closure_free(closure);
			delete data->cif;
			data->cif = nullptr;
			throw std::runtime_error(
					"component runtime: ffi closure allocation failed");
		}
		WASM::Callable callable;
		callable.fnPtr = code;
		callable.context = nullptr;
		callable.localTypeIdx = 0;
		callable.typeId = WASM::TypeId{WASM::TypeId::kNone};
		coreFuncs_.push_back(callable);
		lowerClosures_.push_back(closure);
		lowerCifs_.push_back(data->cif);
		lowerData_.push_back(data.release());
		return;
	}
	if (canon.kind != WASM::Canon::Kind::Lift)
		throw std::runtime_error(
				"component runtime: this canonical builtin is not supported");
	if (canon.coreFuncIndex >= coreFuncs_.size())
		throw std::runtime_error(
				"component runtime: canon lift core function index out of bounds");
	const WASM::TypeId typeId = component_.typeId(canon.typeIndex);
	if (typeId.isNone())
		throw std::runtime_error(
				"component runtime: canon lift type has no closed identity");
	const WASM::CanonicalAbiOptions options = canonicalOptions(canon);
	const WASM::CanonicalAbiSignature* abi =
			abi_.resolve(typeId, options);
	if (!abi)
		throw std::runtime_error(
				"component runtime: canon lift type is outside the flat ABI subset");

	const WASM::ComponentTypeView functionType =
			WASM::Store::current().types().store().componentType(typeId);
	ComponentFunction function;
	function.typeId = typeId;
	function.params = abi->flatParams;
	function.results = abi->flatResults;
	function.options = options;
	function.indirectParams = abi->indirectParams;
	function.indirectResults = abi->indirectResults;
	for (const WASM::ComponentFuncParam& param : functionType.params())
		function.paramTypes.push_back(param.type);
	for (const WASM::ComponentValueType& result : functionType.results())
		function.resultTypes.push_back(result);
	const WASM::Callable callable = coreFuncs_[canon.coreFuncIndex];
	function.call = [this, callable, function](std::span<const WASM::Value> args,
			std::span<WASM::Value> results) {
		invokeCore(callable, function, args, results);
	};
	function.callValues = [this, callable, function](
			std::span<const ComponentValue> args,
			std::span<ComponentValue> results) {
		const AbiMemory memory = memoryFor(function.options);
		const WASM::TypeStore& store = WASM::Store::current().types().store();
		const uint32_t borrowScope = beginBorrowScope();
		const MemView view{memory.memory, memory.realloc, function.options, this,
				borrowScope};
		ComponentFunction core = function;
		std::vector<WASM::Value> flatArgs;
		if (function.indirectParams) {
			uint64_t offset = 0;
			uint32_t alignment = 1;
			std::vector<uint64_t> offsets(function.paramTypes.size());
			for (std::size_t i = 0; i < function.paramTypes.size(); ++i) {
				const WASM::CanonicalAbiLayout layout = layoutOf(
						store, function.paramTypes[i], view);
				alignment = std::max(alignment, layout.align);
				offset = alignUp(offset, layout.align);
				offsets[i] = offset;
				offset += layout.size;
			}
			const uint64_t paramsAddress = allocate(view, offset, alignment);
			for (std::size_t i = 0; i < args.size(); ++i)
				storeValueAt(store, function.paramTypes[i], args[i],
						paramsAddress + offsets[i], view, 0);
			pushPtr(flatArgs, paramsAddress, view);
			core.params = {function.options.pointerSize == 8
					? WASM::AbiCoreType::I64 : WASM::AbiCoreType::I32};
		} else {
			lowerValues(function, args, memory, flatArgs);
		}
		core.results = function.indirectResults
				? std::vector<WASM::AbiCoreType>{function.options.pointerSize == 8
						? WASM::AbiCoreType::I64 : WASM::AbiCoreType::I32}
				: function.results;
		std::vector<WASM::Value> flatResults(core.results.size());
		invokeCore(callable, core, flatArgs, flatResults);
		if (function.indirectResults) {
			const uint64_t address = function.options.pointerSize == 8
					? flatResults[0].ui64 : flatResults[0].u32;
			uint64_t offset = 0;
			uint32_t alignment = 1;
			for (std::size_t i = 0; i < function.resultTypes.size(); ++i) {
				const WASM::CanonicalAbiLayout layout = layoutOf(
						store, function.resultTypes[i], view);
				alignment = std::max(alignment, layout.align);
				offset = alignUp(offset, layout.align);
				loadValueAt(store, function.resultTypes[i], address + offset,
						view, results[i], 0);
				offset += layout.size;
			}
		} else {
			liftValues(function, flatResults, memory, results);
		}
		if (memory.postReturn.fnPtr) {
			ComponentFunction post;
			post.params = core.results;
			invokeCore(memory.postReturn, post, flatResults, {});
		}
		endBorrowScope(borrowScope);
	};
	componentFuncs_.push_back(std::move(function));
}

void ComponentRuntime::processErrorContextCanon(const WASM::Canon& canon)
{
	const WASM::CanonicalAbiOptions options = canonicalOptions(canon);
	const bool pointer64 = options.pointerSize == 8;
	ffi_type* const addressType = ffiTypeForAddress(pointer64);
	auto data = std::make_unique<ResourceClosureData>();
	data->argumentTypes.push_back(&ffi_type_pointer);
	ffi_type* returnType = &ffi_type_void;

	switch (canon.kind) {
	case WASM::Canon::Kind::ErrorContextNew:
		data->argumentTypes.push_back(addressType);
		data->argumentTypes.push_back(addressType);
		returnType = &ffi_type_sint32;
		data->handler = [this, options, pointer64](void* result, void** args) {
			const uint64_t address = readAddress(args[1], pointer64);
			const uint64_t length = readAddress(args[2], pointer64);
			const AbiMemory abi = memoryFor(options);
			if (abi.memory == nullptr)
				throw std::runtime_error(
						"component async: error-context buffer needs a memory");
			const MemView view{abi.memory, abi.realloc, options, this, 0};
			std::string message = loadString(address, length, view);
			*static_cast<int32_t*>(result) =
					int32_t(async_.newErrorContext(std::move(message)));
		};
		break;
	case WASM::Canon::Kind::ErrorContextDebugMessage:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->argumentTypes.push_back(addressType);
		data->handler = [this, options, pointer64](void*, void** args) {
			const uint32_t handle = uint32_t(*static_cast<int32_t*>(args[1]));
			const uint64_t address = readAddress(args[2], pointer64);
			const std::string message = async_.errorContext(handle);
			const AbiMemory abi = memoryFor(options);
			if (abi.memory == nullptr)
				throw std::runtime_error(
						"component async: error-context output needs a memory");
			const MemView view{abi.memory, abi.realloc, options, this, 0};
			const EncodedString encoded = storeString(message, view);
			writeUInt(view, address, encoded.address, options.pointerSize);
			writeUInt(view, address + options.pointerSize,
					encoded.taggedCodeUnits, options.pointerSize);
		};
		break;
	case WASM::Canon::Kind::ErrorContextDrop:
		data->argumentTypes.push_back(&ffi_type_sint32);
		data->handler = [this](void*, void** args) {
			async_.dropErrorContext(uint32_t(*static_cast<int32_t*>(args[1])));
		};
		break;
	default:
		throw std::runtime_error(
				"component runtime: processErrorContextCanon received a non-error canon");
	}
	pushBuiltinClosure(resourceClosures_, resourceCifs_, resourceData_,
			coreFuncs_, std::move(data), returnType);
}

void ComponentRuntime::processThreadCanon(const WASM::Canon& canon)
{
	auto data = std::make_unique<ResourceClosureData>();
	data->argumentTypes.push_back(&ffi_type_pointer);
	ffi_type* returnType = &ffi_type_void;
	switch (canon.kind) {
	case WASM::Canon::Kind::ThreadIndex:
		returnType = &ffi_type_sint32;
		data->handler = [this](void* result, void**) {
			*static_cast<int32_t*>(result) =
					int32_t(async_.executor().threadIndex());
		};
		break;
	case WASM::Canon::Kind::ThreadAvailableParallelism:
		returnType = &ffi_type_sint32;
		data->handler = [this](void* result, void**) {
			*static_cast<int32_t*>(result) =
					int32_t(async_.executor().availableParallelism());
		};
		break;
	case WASM::Canon::Kind::ThreadYield:
		data->handler = [](void*, void**) {
			// A single-agent executor has nothing else to run; yield is a no-op.
		};
		break;
	default:
		// Every remaining thread builtin creates or manipulates a real
		// concurrent agent. The interface exists, but the single-agent backend
		// refuses the operation rather than pretending to schedule it.
		data->handler = [kind = canon.kind](void*, void**) {
			throw std::runtime_error(
					"component runtime: thread builtin " +
					std::to_string(static_cast<unsigned>(kind)) +
					" requires a multi-agent backend (docs/proposals/THREADS.md TD9)");
		};
		break;
	}
	pushBuiltinClosure(resourceClosures_, resourceCifs_, resourceData_,
			coreFuncs_, std::move(data), returnType);
}


void ComponentRuntime::processImport(const WASM::ImportDecl& importDecl)
{
	if (importDecl.type.kind != WASM::ExternType::Kind::Func)
		throw std::runtime_error(
				"component runtime: only function component imports are supported");
	const auto found = imports_.find(importDecl.name.name);
	if (found == imports_.end())
		throw std::runtime_error(
				"component runtime: missing host import `" + importDecl.name.name + "`");
	const WASM::TypeId typeId = component_.typeId(importDecl.type.typeIndex);
	if (typeId.isNone())
		throw std::runtime_error(
				"component runtime: imported function type has no closed identity");
	const WASM::CanonicalAbiSignature* abi =
			abi_.resolve(typeId, {});
	if (!abi)
		throw std::runtime_error(
				"component runtime: imported function type is outside the flat ABI subset");
	if (found->second.params != abi->flatParams
			|| found->second.results != abi->flatResults)
		throw std::runtime_error(
				"component runtime: host function signature does not match import");
	ComponentFunction function = found->second;
	function.typeId = typeId;
	function.indirectParams = abi->indirectParams;
	function.indirectResults = abi->indirectResults;
	const WASM::ComponentTypeView functionType =
			WASM::Store::current().types().store().componentType(typeId);
	function.paramTypes.clear();
	function.resultTypes.clear();
	for (const WASM::ComponentFuncParam& param : functionType.params())
		function.paramTypes.push_back(param.type);
	for (const WASM::ComponentValueType& result : functionType.results())
		function.resultTypes.push_back(result);
	componentFuncs_.push_back(std::move(function));
}

void ComponentRuntime::processExport(const WASM::ComponentExport& exportDecl)
{
	if (exportDecl.sortIndex.sort != WASM::ComponentSort::Func)
		return;
	if (exportDecl.sortIndex.index >= componentFuncs_.size())
		throw std::runtime_error(
				"component runtime: function export index out of bounds");
	exportedFuncs_[exportDecl.name.name] = exportDecl.sortIndex.index;
}

WASM::ComponentValueType ComponentRuntime::resolveValueType(
		const WASM::ComponentValueType& type) const
{
	if (type.kind == WASM::ComponentValueType::Kind::Primitive
			|| type.kind == WASM::ComponentValueType::Kind::TypeId)
		return type;
	const WASM::TypeId id = component_.typeId(type.typeIndex);
	if (id.isNone())
		throw std::runtime_error(
				"component runtime: value type has no closed identity");
	WASM::ComponentValueType resolved;
	resolved.kind = WASM::ComponentValueType::Kind::TypeId;
	resolved.typeIndex = id.value;
	return resolved;
}

void ComponentRuntime::processStart(const WASM::Start& start)
{
	if (start.funcIndex >= componentFuncs_.size())
		throw std::runtime_error(
				"component runtime: start function index out of bounds");
	if (start.args.size() > componentValues_.size())
		throw std::runtime_error(
				"component runtime: start argument index out of bounds");
	std::vector<ComponentValue> args;
	args.reserve(start.args.size());
	for (uint32_t index : start.args)
		args.push_back(componentValues_[index]);
	std::vector<ComponentValue> results(start.resultCount);
	const ComponentFunction& function = componentFuncs_[start.funcIndex];
	if (!function.callValues)
		throw std::runtime_error(
				"component runtime: start function has no structured entry point");
	function.callValues(args, results);
}


std::optional<ComponentFunction> ComponentRuntime::exportedFunction(
		std::string_view name) const
{
	const auto found = exportedFuncs_.find(std::string(name));
	if (found == exportedFuncs_.end())
		return std::nullopt;
	return componentFuncs_[found->second];
}

WASM::Callable ComponentRuntime::coreFunction(std::size_t index) const
{
	if (index >= coreFuncs_.size())
		throw std::out_of_range("component runtime: core function index out of bounds");
	return coreFuncs_[index];
}

WASM::LinearMemory* ComponentRuntime::coreMemory(std::size_t index) const
{
	if (index >= coreMemories_.size())
		throw std::out_of_range("component runtime: core memory index out of bounds");
	return coreMemories_[index];
}

uint32_t ComponentRuntime::beginBorrowScope()
{
	currentBorrowScope_ = borrowScopeSerial_++;
	return currentBorrowScope_;
}

void ComponentRuntime::endBorrowScope(uint32_t scope)
{
	for (std::size_t i = 1; i < resourceSlots_.size(); ++i) {
		ResourceSlot& slot = resourceSlots_[i];
		if (slot.live && !slot.own && slot.borrowScope == scope) {
			slot = ResourceSlot{};
			freeResourceSlots_.push_back(static_cast<uint32_t>(i));
		}
	}
	currentBorrowScope_ = 0;
}

void ComponentRuntime::validateResourceHandle(uint32_t handle,
		WASM::TypeId expected) const
{
	if (handle == 0 || handle >= resourceSlots_.size()
			|| !resourceSlots_[handle].live)
		throw std::runtime_error("component runtime: invalid resource handle");
	if (expected != WASM::TypeId{WASM::TypeId::kNone}
			&& resourceSlots_[handle].typeId != expected)
		throw std::runtime_error("component runtime: resource handle type mismatch");
}

uint32_t ComponentRuntime::useResourceHandle(uint32_t handle,
		WASM::TypeId expected, bool borrow, uint32_t scope)
{
	validateResourceHandle(handle, expected);
	// Copy: allocating the borrowed slot below may reallocate `resourceSlots_`,
	// which would dangle a reference into it.
	const ResourceSlot original = resourceSlots_[handle];
	if (!borrow)
		return handle;
	if (!original.own)
		throw std::runtime_error("component runtime: cannot lend a borrowed handle");
	uint32_t borrowed;
	if (!freeResourceSlots_.empty()) {
		borrowed = freeResourceSlots_.back();
		freeResourceSlots_.pop_back();
		resourceSlots_[borrowed] = ResourceSlot{};
	} else {
		borrowed = static_cast<uint32_t>(resourceSlots_.size());
		resourceSlots_.push_back(ResourceSlot{});
	}
	ResourceSlot& slot = resourceSlots_[borrowed];
	slot.representation = original.representation;
	slot.typeId = original.typeId;
	slot.live = true;
	slot.own = false;
	slot.borrowScope = scope;
	return borrowed;
}

ComponentRuntime::AbiMemory ComponentRuntime::memoryFor(
		const WASM::CanonicalAbiOptions& options) const
{
	AbiMemory memory{};
	memory.pointerSize = options.pointerSize;
	if (options.memory != WASM::CanonicalAbiOptions::kNoIndex) {
		if (options.memory >= coreMemories_.size())
			throw std::runtime_error(
					"component runtime: canonical memory index out of bounds");
		memory.memory = coreMemories_[options.memory];
	}
	if (options.realloc != WASM::CanonicalAbiOptions::kNoIndex) {
		if (options.realloc >= coreFuncs_.size())
			throw std::runtime_error(
					"component runtime: canonical realloc index out of bounds");
		memory.realloc = coreFuncs_[options.realloc];
	}
	if (options.postReturn != WASM::CanonicalAbiOptions::kNoIndex) {
		if (options.postReturn >= coreFuncs_.size())
			throw std::runtime_error(
					"component runtime: canonical post-return index out of bounds");
		memory.postReturn = coreFuncs_[options.postReturn];
	}
	return memory;
}

void ComponentRuntime::lowerValues(const ComponentFunction& function,
		std::span<const ComponentValue> values, const AbiMemory& memory,
		std::vector<WASM::Value>& flat)
{
	if (values.size() != function.paramTypes.size())
		throw std::runtime_error("component runtime: argument count mismatch");
	const WASM::TypeStore& store = WASM::Store::current().types().store();
	const MemView view{memory.memory, memory.realloc, function.options, this,
			currentBorrowScope_};
	for (std::size_t i = 0; i < values.size(); ++i)
		lowerValue(store, function.paramTypes[i], values[i], view, flat, 0);
}

void ComponentRuntime::liftValues(const ComponentFunction& function,
		std::span<const WASM::Value> flat, const AbiMemory& memory,
		std::span<ComponentValue> values)
{
	if (values.size() != function.resultTypes.size())
		throw std::runtime_error("component runtime: result count mismatch");
	const WASM::TypeStore& store = WASM::Store::current().types().store();
	const MemView view{memory.memory, memory.realloc, function.options, this,
			currentBorrowScope_};
	std::size_t index = 0;
	for (std::size_t i = 0; i < values.size(); ++i)
		liftValue(store, function.resultTypes[i], flat, index, view, values[i], 0);
}

void ComponentRuntime::invokeCore(const WASM::Callable& callable,
		const ComponentFunction& function,
		std::span<const WASM::Value> args,
		std::span<WASM::Value> results) const
{
	if (args.size() != function.params.size())
		throw std::runtime_error("component runtime: argument count mismatch");
	if (results.size() != function.results.size())
		throw std::runtime_error("component runtime: result count mismatch");
	if (!callable.fnPtr)
		throw std::runtime_error("component runtime: null core function pointer");

	std::vector<ffi_type*> argumentTypes;
	argumentTypes.reserve(function.params.size() + 2);
	argumentTypes.push_back(&ffi_type_pointer);
	std::vector<AbiSlot> slots(function.params.size());
	std::vector<void*> argumentValues;
	argumentValues.reserve(function.params.size() + 2);
	WASM::VMContext* context = callable.context;
	argumentValues.push_back(&context);
	for (std::size_t i = 0; i < function.params.size(); ++i) {
		argumentTypes.push_back(ffiType(function.params[i]));
		switch (function.params[i]) {
		case WASM::AbiCoreType::I32: slots[i].i32 = args[i].i32; break;
		case WASM::AbiCoreType::I64: slots[i].i64 = args[i].i64; break;
		case WASM::AbiCoreType::F32: slots[i].f32 = args[i].f32; break;
		case WASM::AbiCoreType::F64: slots[i].f64 = args[i].f64; break;
		}
		argumentValues.push_back(&slots[i]);
	}

	ffi_cif cif;
	const bool indirectResults = function.results.size() > 1;
	alignas(16) std::array<unsigned char, 256> resultBuffer{};
	void* resultPointer = resultBuffer.data();
	if (indirectResults) {
		argumentTypes.push_back(&ffi_type_pointer);
		argumentValues.push_back(&resultPointer);
	}
	ffi_type* returnType = function.results.empty() || indirectResults
			? &ffi_type_void : ffiType(function.results.front());
	if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
			static_cast<unsigned>(argumentValues.size()), returnType,
			argumentTypes.data()) != FFI_OK)
		throw std::runtime_error("component runtime: ffi_prep_cif failed");
	AbiSlot result{};
	ffi_call(&cif, FFI_FN(callable.fnPtr), function.results.empty()
			|| indirectResults ? nullptr : &result, argumentValues.data());
	std::vector<std::size_t> offsets(function.results.size());
	std::size_t nextOffset = 0;
	for (std::size_t i = 0; i < function.results.size(); ++i) {
		const std::size_t size = function.results[i] == WASM::AbiCoreType::I64
				|| function.results[i] == WASM::AbiCoreType::F64 ? 8 : 4;
		nextOffset = alignUp(nextOffset, size);
		offsets[i] = nextOffset;
		nextOffset += size;
	}
	for (std::size_t i = 0; i < function.results.size(); ++i) {
		results[i].kind = valueKind(function.results[i]);
		const auto read = [&](auto tag) {
			using T = decltype(tag);
			T value;
			std::memcpy(&value, resultBuffer.data() + offsets[i], sizeof(T));
			return value;
		};
		switch (function.results[i]) {
		case WASM::AbiCoreType::I32:
			results[i].i32 = indirectResults ? read(int32_t{}) : result.i32;
			break;
		case WASM::AbiCoreType::I64:
			results[i].i64 = indirectResults ? read(int64_t{}) : result.i64;
			break;
		case WASM::AbiCoreType::F32:
			results[i].f32 = indirectResults ? read(float{}) : result.f32;
			break;
		case WASM::AbiCoreType::F64:
			results[i].f64 = indirectResults ? read(double{}) : result.f64;
			break;
		}
	}
}

} // namespace WASM
