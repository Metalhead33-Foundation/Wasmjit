#include "WasmStringOps.hpp"

#include "WasmModule.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmGcHeader.hpp"
#include "WasmMemory.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace {

const WASM::StringObject* stringObjectOrTrap(void* handle)
{
	if (handle == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullReference, "null string reference");
		return nullptr;
	}
	return static_cast<const WASM::StringObject*>(handle);
}

WASM::LinearMemory* memoryForTrap(WASM::VMContext* context, uint32_t memoryIndex)
{
	if (context == nullptr || context->memories == nullptr
			|| memoryIndex >= context->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "string instruction memory index is invalid");
		return nullptr;
	}
	WASM::LinearMemory* memory = context->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "string instruction linear memory is null");
		return nullptr;
	}
	return memory;
}

const uint8_t* checkedStringRange(WASM::LinearMemory* memory, uint64_t offset,
		uint64_t length)
{
	if (offset > memory->memorySize || length > memory->memorySize - offset) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	return memory->memoryBase + offset;
}

void* makeString(WASM::VMContext* context, std::vector<uint32_t> codePoints)
{
	if (context == nullptr || context->module == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "string allocation has no module context");
		return nullptr;
	}
	return context->module->store().strings().create(std::move(codePoints));
}

template <typename Decode>
void* newStringFromMemory(WASM::VMContext* context, uint32_t memoryIndex,
		uint32_t pointer, uint32_t length, Decode&& decode)
{
	WASM::LinearMemory* memory = memoryForTrap(context, memoryIndex);
	if (memory == nullptr)
		return nullptr;
	const uint8_t* bytes = checkedStringRange(memory, pointer, length);
	if (bytes == nullptr)
		return nullptr;
	std::vector<uint32_t> codePoints;
	if (!decode(bytes, length, codePoints))
		return nullptr;
	return makeString(context, std::move(codePoints));
}

int32_t writeEncodedString(WASM::VMContext* context, uint32_t memoryIndex,
		uint32_t pointer, const std::vector<uint8_t>& bytes)
{
	WASM::LinearMemory* memory = memoryForTrap(context, memoryIndex);
	if (memory == nullptr)
		return 0;
	uint8_t* destination = const_cast<uint8_t*>(
			checkedStringRange(memory, pointer, bytes.size()));
	if (destination == nullptr)
		return 0;
	if (!bytes.empty())
		std::memcpy(destination, bytes.data(), bytes.size());
	return static_cast<int32_t>(bytes.size());
}

template <typename Encode>
int32_t encodeStringToMemory(WASM::VMContext* context, uint32_t memoryIndex,
		void* handle, uint32_t pointer, Encode&& encode)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	std::vector<uint8_t> bytes;
	if (!encode(string->codePoints, bytes))
		return 0;
	return writeEncodedString(context, memoryIndex, pointer, bytes);
}

} // namespace

extern "C" int32_t wasm_string_measure_utf8(void* handle) noexcept
{
	try {
		const WASM::StringObject* string = stringObjectOrTrap(handle);
		if (string == nullptr)
			return 0;
		uint32_t length = 0;
		return WASM::measureUtf8(string->codePoints, length)
				? static_cast<int32_t>(length) : -1;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_measure_wtf8(void* handle) noexcept
{
	try {
		const WASM::StringObject* string = stringObjectOrTrap(handle);
		if (string == nullptr)
			return 0;
		uint32_t length = 0;
		return WASM::measureWtf8(string->codePoints, length)
				? static_cast<int32_t>(length) : -1;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_measure_wtf16(void* handle) noexcept
{
	try {
		const WASM::StringObject* string = stringObjectOrTrap(handle);
		if (string == nullptr)
			return 0;
		uint32_t length = 0;
		return WASM::measureWtf16(string->codePoints, length)
				? static_cast<int32_t>(length) : -1;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_eq(void* leftHandle, void* rightHandle) noexcept
{
	try {
		// `string.eq` is the one string instruction where null is not a trap:
		// null == null, and a null is unequal to any non-null string.
		if (leftHandle == nullptr || rightHandle == nullptr)
			return leftHandle == rightHandle ? 1 : 0;
		const auto* left = static_cast<const WASM::StringObject*>(leftHandle);
		const auto* right = static_cast<const WASM::StringObject*>(rightHandle);
		return left->codePoints == right->codePoints ? 1 : 0;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" void* wasm_string_concat(WASM::VMContext* context, void* leftHandle,
		void* rightHandle) noexcept
{
	try {
		if (context == nullptr || context->module == nullptr) {
			WASM::reportTrap(WASM::TrapKind::Internal, "string.concat has no module context");
			return nullptr;
		}
		const WASM::StringObject* left = stringObjectOrTrap(leftHandle);
		if (left == nullptr)
			return nullptr;
		const WASM::StringObject* right = stringObjectOrTrap(rightHandle);
		if (right == nullptr)
			return nullptr;
		return context->module->store().strings().create(
				WASM::concatCodePoints(left->codePoints, right->codePoints));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" int32_t wasm_string_is_usv_sequence(void* handle) noexcept
{
	try {
		const WASM::StringObject* string = stringObjectOrTrap(handle);
		if (string == nullptr)
			return 0;
		return WASM::isUsvSequence(string->codePoints) ? 1 : 0;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" void* wasm_string_new_utf8(WASM::VMContext* context,
		uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept
{
	try {
		if (length > WASM::kMaxStringBytes) {
			WASM::reportTrap(WASM::TrapKind::OutOfMemory,
					"string exceeds the implementation limit");
			return nullptr;
		}
		return newStringFromMemory(context, memoryIndex, pointer, length,
				[](const uint8_t* bytes, uint32_t size, std::vector<uint32_t>& out) {
					if (WASM::decodeUtf8(bytes, size, out))
						return true;
					WASM::reportTrap(WASM::TrapKind::InvalidString,
							"string.new_utf8: invalid UTF-8");
					return false;
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" void* wasm_string_new_lossy_utf8(WASM::VMContext* context,
		uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept
{
	try {
		if (length > WASM::kMaxStringBytes) {
			WASM::reportTrap(WASM::TrapKind::OutOfMemory,
					"string exceeds the implementation limit");
			return nullptr;
		}
		return newStringFromMemory(context, memoryIndex, pointer, length,
				[](const uint8_t* bytes, uint32_t size, std::vector<uint32_t>& out) {
					WASM::decodeUtf8Lossy(bytes, size, out);
					return true;
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" void* wasm_string_new_wtf8(WASM::VMContext* context,
		uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept
{
	try {
		if (length > WASM::kMaxStringBytes) {
			WASM::reportTrap(WASM::TrapKind::OutOfMemory,
					"string exceeds the implementation limit");
			return nullptr;
		}
		return newStringFromMemory(context, memoryIndex, pointer, length,
				[](const uint8_t* bytes, uint32_t size, std::vector<uint32_t>& out) {
					if (WASM::decodeWtf8(bytes, size, out))
						return true;
					WASM::reportTrap(WASM::TrapKind::InvalidString,
							"string.new_wtf8: invalid WTF-8");
					return false;
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" void* wasm_string_new_wtf16(WASM::VMContext* context,
		uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept
{
	try {
		if (length > WASM::kMaxStringCodeUnits) {
			WASM::reportTrap(WASM::TrapKind::OutOfMemory,
					"string exceeds the implementation limit");
			return nullptr;
		}
		WASM::LinearMemory* memory = memoryForTrap(context, memoryIndex);
		if (memory == nullptr)
			return nullptr;
		const uint64_t bytes = static_cast<uint64_t>(length) * 2u;
		const uint8_t* data = checkedStringRange(memory, pointer, bytes);
		if (data == nullptr)
			return nullptr;
		std::vector<uint32_t> codePoints;
		WASM::decodeWtf16(data, length, codePoints);
		return makeString(context, std::move(codePoints));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" int32_t wasm_string_encode_utf8(WASM::VMContext* context,
		uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept
{
	try {
		return encodeStringToMemory(context, memoryIndex, handle, pointer,
				[](const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) {
					if (WASM::encodeUtf8(codePoints, out, false))
						return true;
					WASM::reportTrap(WASM::TrapKind::InvalidString,
							"string.encode_utf8: isolated surrogate");
					return false;
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_encode_lossy_utf8(WASM::VMContext* context,
		uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept
{
	try {
		return encodeStringToMemory(context, memoryIndex, handle, pointer,
				[](const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) {
					return WASM::encodeUtf8(codePoints, out, true);
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_encode_wtf8(WASM::VMContext* context,
		uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept
{
	try {
		return encodeStringToMemory(context, memoryIndex, handle, pointer,
				[](const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) {
					return WASM::encodeWtf8(codePoints, out);
				});
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_encode_wtf16(WASM::VMContext* context,
		uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept
{
	try {
		const int32_t bytes = encodeStringToMemory(context, memoryIndex, handle,
				pointer,
				[](const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out) {
					return WASM::encodeWtf16(codePoints, out);
				});
		// The result is a count of 16-bit code units, not of bytes.
		return bytes / 2;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

namespace {

bool stringArrayLength(void* handle, uint32_t& length)
{
	if (handle == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullArrayReference, "null array reference");
		return false;
	}
	length = *reinterpret_cast<const uint32_t*>(
			static_cast<const uint8_t*>(handle) + WASM::GcHeader::kArrayLengthOffset);
	return true;
}

} // namespace

extern "C" void* wasm_string_new_array(WASM::VMContext* context, void* arrayHandle,
		int32_t start, int32_t end, uint32_t dataOffset, uint32_t elementSize,
		int32_t mode) noexcept
{
	try {
		const bool wtf16 = mode == 3;
		if ((wtf16 && elementSize != 2) || (!wtf16 && elementSize != 1)) {
			WASM::reportTrap(WASM::TrapKind::Internal, "string array element size mismatch");
			return nullptr;
		}
		uint32_t length = 0;
		if (!stringArrayLength(arrayHandle, length))
			return nullptr;
		if (start < 0 || end < 0) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return nullptr;
		}
		const uint64_t first = static_cast<uint32_t>(start);
		const uint64_t last = static_cast<uint32_t>(end);
		if (first > length || last > length || last < first) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return nullptr;
		}
		const uint64_t count = last - first;
		const uint64_t limit = wtf16 ? WASM::kMaxStringCodeUnits : WASM::kMaxStringBytes;
		if (count > limit) {
			WASM::reportTrap(WASM::TrapKind::OutOfMemory,
					"string exceeds the implementation limit");
			return nullptr;
		}

		const uint8_t* data = static_cast<const uint8_t*>(arrayHandle) + dataOffset
				+ static_cast<std::size_t>(first) * elementSize;
		std::vector<uint32_t> codePoints;
		switch (mode) {
		case 0:
			if (!WASM::decodeUtf8(data, static_cast<std::size_t>(count), codePoints)) {
				WASM::reportTrap(WASM::TrapKind::InvalidString,
						"string.new_utf8_array: invalid UTF-8");
				return nullptr;
			}
			break;
		case 1:
			WASM::decodeUtf8Lossy(data, static_cast<std::size_t>(count), codePoints);
			break;
		case 2:
			if (!WASM::decodeWtf8(data, static_cast<std::size_t>(count), codePoints)) {
				WASM::reportTrap(WASM::TrapKind::InvalidString,
						"string.new_wtf8_array: invalid WTF-8");
				return nullptr;
			}
			break;
		case 3:
			WASM::decodeWtf16(data, static_cast<std::size_t>(count), codePoints);
			break;
		default:
			WASM::reportTrap(WASM::TrapKind::Internal, "unknown string array mode");
			return nullptr;
		}
		return makeString(context, std::move(codePoints));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

extern "C" int32_t wasm_string_encode_array(void* stringHandle, void* arrayHandle,
		int32_t start, uint32_t dataOffset, uint32_t elementSize,
		int32_t mode) noexcept
{
	try {
		const bool wtf16 = mode == 3;
		if ((wtf16 && elementSize != 2) || (!wtf16 && elementSize != 1)) {
			WASM::reportTrap(WASM::TrapKind::Internal, "string array element size mismatch");
			return 0;
		}
		const WASM::StringObject* string = stringObjectOrTrap(stringHandle);
		if (string == nullptr)
			return 0;
		uint32_t length = 0;
		if (!stringArrayLength(arrayHandle, length))
			return 0;
		if (start < 0) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return 0;
		}
		const uint64_t first = static_cast<uint32_t>(start);
		if (first > length) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return 0;
		}
		// Every code point needs at least one element; this cheap check avoids
		// encoding a large string only to discover that the array is too small.
		if (string->codePoints.size() > length - first) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return 0;
		}

		std::vector<uint8_t> encoded;
		switch (mode) {
		case 0:
			if (!WASM::encodeUtf8(string->codePoints, encoded, false)) {
				WASM::reportTrap(WASM::TrapKind::InvalidString,
						"string.encode_utf8_array: isolated surrogate");
				return 0;
			}
			break;
		case 1:
			WASM::encodeUtf8(string->codePoints, encoded, true);
			break;
		case 2:
			WASM::encodeWtf8(string->codePoints, encoded);
			break;
		case 3:
			WASM::encodeWtf16(string->codePoints, encoded);
			break;
		default:
			WASM::reportTrap(WASM::TrapKind::Internal, "unknown string array mode");
			return 0;
		}
		const uint64_t count = wtf16 ? encoded.size() / 2u : encoded.size();
		if (first + count > length) {
			WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
			return 0;
		}
		uint8_t* destination = static_cast<uint8_t*>(arrayHandle) + dataOffset
				+ static_cast<std::size_t>(first) * elementSize;
		if (!encoded.empty())
			std::memcpy(destination, encoded.data(), encoded.size());
		return static_cast<int32_t>(count);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return 0;
	}
}

extern "C" int32_t wasm_string_encode_array_packed(void* stringHandle,
		void* arrayHandle, uint32_t start, uint64_t packedLayout) noexcept
{
	return wasm_string_encode_array(stringHandle, arrayHandle,
			static_cast<int32_t>(start),
			static_cast<uint32_t>(packedLayout & 0xFFFFFFFFu),
			static_cast<uint32_t>((packedLayout >> 32) & 0xFFu),
			static_cast<int32_t>((packedLayout >> 40) & 0xFFu));
}

extern "C" void* wasm_string_new_array_packed(WASM::VMContext* context,
		void* arrayHandle, uint64_t packedRange, uint64_t packedLayout) noexcept
{
	return wasm_string_new_array(context, arrayHandle,
			static_cast<int32_t>(packedRange),
			static_cast<int32_t>(packedRange >> 32),
			static_cast<uint32_t>(packedLayout & 0xFFFFFFFFu),
			static_cast<uint32_t>((packedLayout >> 32) & 0xFFu),
			static_cast<int32_t>((packedLayout >> 40) & 0xFFu));
}

