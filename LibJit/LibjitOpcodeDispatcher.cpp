#include "LibjitOpcodeDispatcher.hpp"
#include "../WasmBase/WasmDeterminism.hpp"
#include "../WasmBase/WasmContinuation.hpp"
#include "../WasmBase/WasmSafety.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/WasmExn.hpp"
#include "../WasmBase/WasmGcHeader.hpp"
#include "../WasmBase/WasmString.hpp"
#include "../WasmBase/WasmTracing.hpp"
#include "../WasmBase/wasm_atomic.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace LibJIT {

namespace {

// ── Wide continuation invoker ──────────────────────────────────────────────
// `WasmBase/WasmContinuation.hpp` is backend-neutral; this callback is the
// LibJIT side of the wide target invocation. It builds the target's typed C
// signature from the kind arrays, then uses `jit_apply` so all ABI classes
// (integer, SSE, pointer) are handled by the JIT library rather than guessed
// here. Values are 16-byte raw slots; a multi-result target writes its usual
// caller-provided tuple, which is copied back into the slots.
static jit_type_t continuationJitType(std::uint8_t kind)
{
	switch (static_cast<WASM::ContinuationValueKind>(kind)) {
	case WASM::ContinuationValueKind::I32: return jit_type_int;
	case WASM::ContinuationValueKind::I64: return jit_type_long;
	case WASM::ContinuationValueKind::F32: return jit_type_float32;
	case WASM::ContinuationValueKind::F64: return jit_type_float64;
	case WASM::ContinuationValueKind::Ref: return jit_type_void_ptr;
	case WASM::ContinuationValueKind::V128: {
		jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
		jit_type_t type = jit_type_create_struct(fields, 4, 0);
		jit_type_set_size_and_alignment(type, 16, 16);
		return type;
	}
	default:
		return jit_type_void_ptr;
	}
}

static const std::uint8_t* persistContinuationKinds(
		const std::vector<std::uint8_t>& kinds)
{
	// The generated code embeds these pointers as constants, so they must
	// outlive the compilation. A deque keeps element addresses stable.
	static std::mutex mutex;
	static std::deque<std::vector<std::uint8_t>> storage;
	std::lock_guard<std::mutex> lock(mutex);
	storage.push_back(kinds);
	return storage.back().data();
}

static void invokeContinuation(void* target, void* context,
		const WASM::ContinuationValue* args, std::uint32_t argCount,
		WASM::ContinuationValue* results, std::uint32_t resultCount,
		const std::uint8_t* paramKinds, const std::uint8_t* resultKinds){

	auto* callable = static_cast<WASM::Callable*>(target);
	if (callable == nullptr || callable->fnPtr == nullptr)
		throw std::runtime_error("continuation invoker has no target");
	void* calleeContext = callable->context != nullptr ? callable->context : context;

	std::vector<jit_type_t> params;
	params.push_back(jit_type_void_ptr);
	for (std::uint32_t i = 0; i < argCount; ++i)
		params.push_back(continuationJitType(paramKinds[i]));

	std::vector<jit_type_t> resultTypes;
	for (std::uint32_t i = 0; i < resultCount; ++i)
		resultTypes.push_back(continuationJitType(resultKinds[i]));

	const bool multiResult = resultCount > 1;
	jit_type_t returnType = resultCount == 0 ? jit_type_void
			: resultCount == 1 ? resultTypes[0] : jit_type_void;
	if (multiResult)
		params.push_back(jit_type_void_ptr);
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, returnType,
			params.data(), static_cast<unsigned int>(params.size()), 1);

	std::vector<void*> callArgs;
	callArgs.push_back(&calleeContext);
	for (std::uint32_t i = 0; i < argCount; ++i)
		callArgs.push_back(const_cast<WASM::ContinuationValue*>(&args[i]));

	std::vector<std::uint8_t> tuple;
	void* tuplePtr = nullptr;
	if (multiResult) {
		jit_type_t tupleType = jit_type_create_struct(resultTypes.data(),
				static_cast<unsigned int>(resultTypes.size()), 1);
		tuple.resize(jit_type_get_size(tupleType));
		tuplePtr = tuple.data();
		callArgs.push_back(&tuplePtr);
		jit_apply(signature, callable->fnPtr, callArgs.data(),
				static_cast<unsigned int>(callArgs.size()), nullptr);
		for (std::uint32_t i = 0; i < resultCount; ++i) {
			const jit_nuint offset = jit_type_get_offset(tupleType,
					static_cast<unsigned int>(i));
			const jit_nuint size = jit_type_get_size(resultTypes[i]);
			std::memset(&results[i], 0, sizeof(WASM::ContinuationValue));
			std::memcpy(&results[i], tuple.data() + offset,
					std::min<std::size_t>(size, sizeof(WASM::ContinuationValue)));
		}
		jit_type_free(tupleType);
	} else if (resultCount == 1) {
		jit_apply(signature, callable->fnPtr, callArgs.data(),
				static_cast<unsigned int>(callArgs.size()), &results[0]);
	} else {
		jit_apply(signature, callable->fnPtr, callArgs.data(),
				static_cast<unsigned int>(callArgs.size()), nullptr);
	}
	jit_type_free(signature);
}

// A Wasm trap (docs/SAFETY.md §2.4) is *reported*, not thrown or jumped:
// `wasm_trap_report` records the kind/message in the engine's pending slot and
// returns, and the dispatcher branches to the function's error epilogue, which
// returns zeroes. The caller then checks the same slot after its call
// (`emitPendingTrapCheck`). Nothing here needs an unwind table, and a helper may
// hold a `std::string` across the trap point: it returns normally, so the
// destructor runs (docs/TRAP_CLEANUP.md §8.5/§9.7).
//
// The *kind* travels as an int and the message as a constant string, because a
// scoped enum cannot be passed through a jit_type signature; wasm_trap_report
// re-assembles them. The kind is what makes the trap checkable
// (docs/SAFETY.md §2.4), the message is only for the log.
void emitTrapReportCall(jit_function_t fn, WASM::TrapKind kind, const char* detail,
		uint32_t functionIndex, uint32_t instructionOffset, const char* functionName)
{
	// DG-5: the backend knows the function/instruction at compile time, so the
	// trap site travels as constants. Report sites without one use the 2-arg
	// `wasm_trap_report` instead.
	jit_type_t params[5] = {jit_type_int, jit_type_void_ptr, jit_type_uint,
			jit_type_uint, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[5] = {
		jit_value_create_nint_constant(fn, jit_type_int,
			static_cast<jit_nint>(static_cast<int>(kind))),
		jit_value_create_nint_constant(fn, jit_type_void_ptr,
			reinterpret_cast<jit_nint>(const_cast<char*>(detail))),
		jit_value_create_nint_constant(fn, jit_type_uint,
			static_cast<jit_nint>(functionIndex)),
		jit_value_create_nint_constant(fn, jit_type_uint,
			static_cast<jit_nint>(instructionOffset)),
		jit_value_create_nint_constant(fn, jit_type_void_ptr,
			reinterpret_cast<jit_nint>(const_cast<char*>(functionName)))
	};
	jit_insn_call_native(fn, "wasm_trap_report_at",
		reinterpret_cast<void*>(&WASM::wasm_trap_report_at), sig, args, 5, 0);
}

// ── Stringref runtime helpers (docs/proposals/STRINGREF.md STR-3) ───────────
// These are the fallible operations the generated code calls. Strings are
// host-owned Store objects; the pointer in a `stringref` is a StringObject*.
//
// A failure is reported (`reportTrap`) and the helper returns a null/default
// value; the generated caller checks the pending slot immediately after the
// call and returns before it uses the value. The helpers hold `std::vector`
// locals across their failure points on purpose: with status-return traps those
// destructors run normally (docs/TRAP_CLEANUP.md §8.5).

static const uint8_t* checkedStringRange(WASM::LinearMemory* memory,
										 uint64_t offset, uint64_t length)
{
	if (memory == nullptr) {
		reportTrap(WASM::TrapKind::Internal, "null linear memory");
		return nullptr;
	}
	const uint64_t size = memory->memorySize;
	if (offset > size || length > size - offset) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	return memory->memoryBase + offset;
}

static const WASM::StringObject* stringObjectOrTrap(void* handle)
{
	if (handle == nullptr) {
		reportTrap(WASM::TrapKind::NullReference, "null string reference");
		return nullptr;
	}
	return static_cast<const WASM::StringObject*>(handle);
}

static void* makeString(std::vector<uint32_t> codePoints)
{
	return WASM::Store::current().strings().create(std::move(codePoints));
}

static void* wasm_string_new_utf8(WASM::LinearMemory* memory, uint32_t ptr, uint32_t len)
{
	if (len > WASM::kMaxStringBytes) {
		reportTrap(WASM::TrapKind::OutOfMemory, "string exceeds the implementation limit");
		return nullptr;
	}
	const uint8_t* bytes = checkedStringRange(memory, ptr, len);
	if (bytes == nullptr)
		return nullptr;
	std::vector<uint32_t> codePoints;
	if (!WASM::decodeUtf8(bytes, len, codePoints)) {
		reportTrap(WASM::TrapKind::InvalidString, "string.new_utf8: invalid UTF-8");
		return nullptr;
	}
	return makeString(std::move(codePoints));
}

static void* wasm_string_new_lossy_utf8(WASM::LinearMemory* memory, uint32_t ptr, uint32_t len)
{
	if (len > WASM::kMaxStringBytes) {
		reportTrap(WASM::TrapKind::OutOfMemory, "string exceeds the implementation limit");
		return nullptr;
	}
	const uint8_t* bytes = checkedStringRange(memory, ptr, len);
	if (bytes == nullptr)
		return nullptr;
	std::vector<uint32_t> codePoints;
	WASM::decodeUtf8Lossy(bytes, len, codePoints);
	return makeString(std::move(codePoints));
}

static void* wasm_string_new_wtf8(WASM::LinearMemory* memory, uint32_t ptr, uint32_t len)
{
	if (len > WASM::kMaxStringBytes) {
		reportTrap(WASM::TrapKind::OutOfMemory, "string exceeds the implementation limit");
		return nullptr;
	}
	const uint8_t* bytes = checkedStringRange(memory, ptr, len);
	if (bytes == nullptr)
		return nullptr;
	std::vector<uint32_t> codePoints;
	if (!WASM::decodeWtf8(bytes, len, codePoints)) {
		reportTrap(WASM::TrapKind::InvalidString, "string.new_wtf8: invalid WTF-8");
		return nullptr;
	}
	return makeString(std::move(codePoints));
}

static void* wasm_string_new_wtf16(WASM::LinearMemory* memory, uint32_t ptr, uint32_t codeUnits)
{
	if (codeUnits > WASM::kMaxStringCodeUnits) {
		reportTrap(WASM::TrapKind::OutOfMemory, "string exceeds the implementation limit");
		return nullptr;
	}
	// `codeUnits` is a count of 16-bit units, so the byte range is twice that.
	const uint64_t bytes = static_cast<uint64_t>(codeUnits) * 2u;
	const uint8_t* data = checkedStringRange(memory, ptr, bytes);
	if (data == nullptr)
		return nullptr;
	std::vector<uint32_t> codePoints;
	WASM::decodeWtf16(data, codeUnits, codePoints);
	return makeString(std::move(codePoints));
}

static int32_t wasm_string_measure_utf8(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	uint32_t length = 0;
	return WASM::measureUtf8(string->codePoints, length)
			? static_cast<int32_t>(length) : -1;
}

static int32_t wasm_string_measure_wtf8(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	uint32_t length = 0;
	return WASM::measureWtf8(string->codePoints, length)
			? static_cast<int32_t>(length) : -1;
}

static int32_t wasm_string_measure_wtf16(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	uint32_t length = 0;
	return WASM::measureWtf16(string->codePoints, length)
			? static_cast<int32_t>(length) : -1;
}

static int32_t writeEncodedString(WASM::LinearMemory* memory, uint32_t ptr,
								  const std::vector<uint8_t>& bytes)
{
	uint8_t* destination = const_cast<uint8_t*>(
			checkedStringRange(memory, ptr, bytes.size()));
	if (destination == nullptr)
		return 0;
	if (!bytes.empty())
		std::memcpy(destination, bytes.data(), bytes.size());
	return static_cast<int32_t>(bytes.size());
}

static int32_t wasm_string_encode_utf8(WASM::LinearMemory* memory, void* handle, uint32_t ptr)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	std::vector<uint8_t> bytes;
	if (!WASM::encodeUtf8(string->codePoints, bytes, false)) {
		reportTrap(WASM::TrapKind::InvalidString, "string.encode_utf8: isolated surrogate");
		return 0;
	}
	return writeEncodedString(memory, ptr, bytes);
}

static int32_t wasm_string_encode_lossy_utf8(WASM::LinearMemory* memory, void* handle, uint32_t ptr)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	std::vector<uint8_t> bytes;
	WASM::encodeUtf8(string->codePoints, bytes, true);
	return writeEncodedString(memory, ptr, bytes);
}

static int32_t wasm_string_encode_wtf8(WASM::LinearMemory* memory, void* handle, uint32_t ptr)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	std::vector<uint8_t> bytes;
	WASM::encodeWtf8(string->codePoints, bytes);
	return writeEncodedString(memory, ptr, bytes);
}

static int32_t wasm_string_encode_wtf16(WASM::LinearMemory* memory, void* handle, uint32_t ptr)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	std::vector<uint8_t> bytes;
	WASM::encodeWtf16(string->codePoints, bytes);
	writeEncodedString(memory, ptr, bytes);
	// The result is a count of 16-bit code units, not of bytes.
	return static_cast<int32_t>(bytes.size() / 2u);
}

static void* wasm_string_concat(void* leftHandle, void* rightHandle)
{
	const WASM::StringObject* left = stringObjectOrTrap(leftHandle);
	if (left == nullptr)
		return nullptr;
	const WASM::StringObject* right = stringObjectOrTrap(rightHandle);
	if (right == nullptr)
		return nullptr;
	return makeString(WASM::concatCodePoints(left->codePoints, right->codePoints));
}

static int32_t wasm_string_eq(void* leftHandle, void* rightHandle)
{
	// `string.eq` is the one string instruction where null is not a trap:
	// null == null, and a null is unequal to any non-null string.
	if (leftHandle == nullptr || rightHandle == nullptr)
		return leftHandle == rightHandle ? 1 : 0;
	const auto* left = static_cast<const WASM::StringObject*>(leftHandle);
	const auto* right = static_cast<const WASM::StringObject*>(rightHandle);
	return left->codePoints == right->codePoints ? 1 : 0;
}

static int32_t wasm_string_is_usv_sequence(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return 0;
	return WASM::isUsvSequence(string->codePoints) ? 1 : 0;
}

// ── stringview helpers (docs/proposals/STRINGREF.md STR-4) ──────────────────

static const WASM::StringViewObject* viewObjectOrTrap(void* handle)
{
	if (handle == nullptr) {
		reportTrap(WASM::TrapKind::NullReference, "null string view");
		return nullptr;
	}
	return static_cast<const WASM::StringViewObject*>(handle);
}

static WASM::StringIteratorObject* iteratorObjectOrTrap(void* handle)
{
	if (handle == nullptr) {
		reportTrap(WASM::TrapKind::NullReference, "null string iterator");
		return nullptr;
	}
	return static_cast<WASM::StringIteratorObject*>(handle);
}

static void* wasm_string_as_wtf8(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return nullptr;
	return WASM::Store::current().strings().createView(string);
}

static void* wasm_string_as_wtf16(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return nullptr;
	return WASM::Store::current().strings().createView(string);
}

static void* wasm_string_as_iter(void* handle)
{
	const WASM::StringObject* string = stringObjectOrTrap(handle);
	if (string == nullptr)
		return nullptr;
	return WASM::Store::current().strings().createIterator(string);
}

static int32_t wasm_stringview_wtf8_advance(void* viewHandle, int32_t pos, int32_t bytes)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return 0;
	uint32_t next = 0;
	if (!WASM::wtf8Advance(view->string->codePoints, pos, bytes, next)) {
		reportTrap(WASM::TrapKind::InvalidString,
			 "stringview_wtf8.advance: position exceeds 2^31-1");
		return 0;
	}
	return static_cast<int32_t>(next);
}

static uint64_t wasm_stringview_wtf8_encode(WASM::LinearMemory* memory, void* viewHandle,
											uint32_t ptr, int32_t pos, int32_t bytes,
											int encoding)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return 0;
	WASM::Wtf8Encoding mode = WASM::Wtf8Encoding::Wtf8;
	if (encoding == 0)
		mode = WASM::Wtf8Encoding::Utf8;
	else if (encoding == 1)
		mode = WASM::Wtf8Encoding::LossyUtf8;
	std::vector<uint8_t> encoded;
	uint32_t next = 0;
	if (!WASM::wtf8Encode(view->string->codePoints, pos, bytes, mode, next, encoded)) {
		reportTrap(WASM::TrapKind::InvalidString,
			 "stringview_wtf8.encode: isolated surrogate or position overflow");
		return 0;
	}
	writeEncodedString(memory, ptr, encoded);
	return (static_cast<uint64_t>(next) << 32) | static_cast<uint32_t>(encoded.size());
}

static void* wasm_stringview_wtf8_slice(void* viewHandle, int32_t start, int32_t end)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return nullptr;
	return makeString(WASM::sliceWtf8(view->string->codePoints, start, end));
}

static int32_t wasm_stringview_wtf16_length(void* viewHandle)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return 0;
	uint32_t length = 0;
	if (!WASM::measureWtf16(view->string->codePoints, length))
		return -1;
	return static_cast<int32_t>(length);
}

static int32_t wasm_stringview_wtf16_get_codeunit(void* viewHandle, int32_t pos)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return 0;
	uint32_t unit = 0;
	if (!WASM::wtf16CodeUnitAt(view->string->codePoints, pos, unit)) {
		reportTrap(WASM::TrapKind::InvalidString,
			 "stringview_wtf16.get_codeunit: position out of range");
		return 0;
	}
	return static_cast<int32_t>(unit);
}

static int32_t wasm_stringview_wtf16_encode(WASM::LinearMemory* memory, void* viewHandle,
											uint32_t ptr, int32_t pos, int32_t units)
{
	if ((ptr & 1u) != 0) {
		reportTrap(WASM::TrapKind::InvalidString,
			 "stringview_wtf16.encode: pointer is not two-byte aligned");
		return 0;
	}
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return 0;
	std::vector<uint8_t> encoded;
	const uint32_t written = WASM::wtf16Encode(view->string->codePoints, pos, units, encoded);
	writeEncodedString(memory, ptr, encoded);
	return static_cast<int32_t>(written);
}

static void* wasm_stringview_wtf16_slice(void* viewHandle, int32_t start, int32_t end)
{
	const WASM::StringViewObject* view = viewObjectOrTrap(viewHandle);
	if (view == nullptr)
		return nullptr;
	return makeString(WASM::sliceWtf16(view->string->codePoints, start, end));
}

static int32_t wasm_stringview_iter_next(void* iteratorHandle)
{
	WASM::StringIteratorObject* iterator = iteratorObjectOrTrap(iteratorHandle);
	if (iterator->position >= iterator->string->codePoints.size())
		return -1;
	return static_cast<int32_t>(iterator->string->codePoints[iterator->position++]);
}

static int32_t wasm_stringview_iter_advance(void* iteratorHandle, int32_t count)
{
	WASM::StringIteratorObject* iterator = iteratorObjectOrTrap(iteratorHandle);
	const uint32_t size = static_cast<uint32_t>(iterator->string->codePoints.size());
	const uint32_t remaining = size - iterator->position;
	const uint32_t take = count < 0
			? remaining
			: std::min(static_cast<uint32_t>(count), remaining);
	iterator->position += take;
	return static_cast<int32_t>(take);
}

static int32_t wasm_stringview_iter_rewind(void* iteratorHandle, int32_t count)
{
	WASM::StringIteratorObject* iterator = iteratorObjectOrTrap(iteratorHandle);
	const uint32_t take = count < 0
			? iterator->position
			: std::min(static_cast<uint32_t>(count), iterator->position);
	iterator->position -= take;
	return static_cast<int32_t>(take);
}

static void* wasm_stringview_iter_slice(void* iteratorHandle, int32_t count)
{
	WASM::StringIteratorObject* iterator = iteratorObjectOrTrap(iteratorHandle);
	return makeString(WASM::sliceCodePoints(iterator->string->codePoints,
										   static_cast<int32_t>(iterator->position),
										   count));
}

// ── GC-array string bridge (docs/proposals/STRINGREF.md STR-6) ──────────────
// The array type is validated at compile time; the JIT passes the inline data
// offset and the packed element size (1 for i8, 2 for i16). The array length is
// the GC header word shared with every other array access.

enum class StringArrayMode { Utf8 = 0, LossyUtf8 = 1, Wtf8 = 2, Wtf16 = 3 };

static bool stringArrayLength(void* handle, uint32_t& length)
{
	if (handle == nullptr) {
		reportTrap(WASM::TrapKind::NullArrayReference, "null array reference");
		return false;
	}
	length = *reinterpret_cast<const uint32_t*>(
			static_cast<const uint8_t*>(handle) + WASM::GcHeader::kArrayLengthOffset);
	return true;
}

static void* wasm_string_new_array(void* arrayHandle, int32_t start, int32_t end,
   uint32_t dataOffset, uint32_t elementSize, int mode)
{
if ((mode == static_cast<int>(StringArrayMode::Wtf16) && elementSize != 2)
|| (mode != static_cast<int>(StringArrayMode::Wtf16) && elementSize != 1)) {
reportTrap(WASM::TrapKind::Internal, "string array element size mismatch");
return nullptr;
}
uint32_t length = 0;
if (!stringArrayLength(arrayHandle, length))
return nullptr;
if (start < 0 || end < 0) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return nullptr;
}
const uint64_t first = static_cast<uint32_t>(start);
const uint64_t last = static_cast<uint32_t>(end);
if (first > length || last > length || last < first) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return nullptr;
}
const uint64_t count = last - first;
const uint64_t limit = mode == static_cast<int>(StringArrayMode::Wtf16)
? WASM::kMaxStringCodeUnits : WASM::kMaxStringBytes;
if (count > limit) {
reportTrap(WASM::TrapKind::OutOfMemory, "string exceeds the implementation limit");
return nullptr;
}

const uint8_t* data = static_cast<const uint8_t*>(arrayHandle) + dataOffset
+ static_cast<std::size_t>(first) * elementSize;
std::vector<uint32_t> codePoints;
switch (static_cast<StringArrayMode>(mode)) {
case StringArrayMode::Utf8:
if (!WASM::decodeUtf8(data, static_cast<std::size_t>(count), codePoints)) {
reportTrap(WASM::TrapKind::InvalidString, "string.new_utf8_array: invalid UTF-8");
return nullptr;
}
break;
case StringArrayMode::LossyUtf8:
WASM::decodeUtf8Lossy(data, static_cast<std::size_t>(count), codePoints);
break;
case StringArrayMode::Wtf8:
if (!WASM::decodeWtf8(data, static_cast<std::size_t>(count), codePoints)) {
reportTrap(WASM::TrapKind::InvalidString, "string.new_wtf8_array: invalid WTF-8");
return nullptr;
}
break;
case StringArrayMode::Wtf16:
WASM::decodeWtf16(data, static_cast<std::size_t>(count), codePoints);
break;
}
return makeString(std::move(codePoints));
}

static int32_t wasm_string_encode_array(void* stringHandle, void* arrayHandle, int32_t start,
uint32_t dataOffset, uint32_t elementSize, int mode)
{
if ((mode == static_cast<int>(StringArrayMode::Wtf16) && elementSize != 2)
|| (mode != static_cast<int>(StringArrayMode::Wtf16) && elementSize != 1)) {
reportTrap(WASM::TrapKind::Internal, "string array element size mismatch");
return 0;
}
const WASM::StringObject* string = stringObjectOrTrap(stringHandle);
if (string == nullptr)
return 0;
uint32_t length = 0;
if (!stringArrayLength(arrayHandle, length))
return 0;
if (start < 0) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return 0;
}
const uint64_t first = static_cast<uint32_t>(start);
if (first > length) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return 0;
}
// Every code point needs at least one code unit; this cheap check avoids
// encoding a large string only to discover that the array is too small.
if (string->codePoints.size() > length - first) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return 0;
}

std::vector<uint8_t> encoded;
switch (static_cast<StringArrayMode>(mode)) {
case StringArrayMode::Utf8:
if (!WASM::encodeUtf8(string->codePoints, encoded, false)) {
reportTrap(WASM::TrapKind::InvalidString, "string.encode_utf8_array: isolated surrogate");
return 0;
}
break;
case StringArrayMode::LossyUtf8:
WASM::encodeUtf8(string->codePoints, encoded, true);
break;
case StringArrayMode::Wtf8:
WASM::encodeWtf8(string->codePoints, encoded);
break;
case StringArrayMode::Wtf16:
WASM::encodeWtf16(string->codePoints, encoded);
break;
}
const uint64_t units = encoded.size() / elementSize;
if (first + units > length) {
reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
return 0;
}
if (!encoded.empty())
std::memcpy(static_cast<uint8_t*>(arrayHandle) + dataOffset
+ static_cast<std::size_t>(first) * elementSize,
encoded.data(), encoded.size());
return static_cast<int32_t>(units);
}


extern "C" {
static uint32_t wasm_i32_clz(uint32_t x)
{
	return x == 0 ? 32u : static_cast<uint32_t>(__builtin_clz(x));
}
static uint32_t wasm_i32_ctz(uint32_t x)
{
	return x == 0 ? 32u : static_cast<uint32_t>(__builtin_ctz(x));
}
static uint32_t wasm_i32_popcnt(uint32_t x)
{
	return static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned>(x)));
}
static uint64_t wasm_i64_clz(uint64_t x)
{
	return x == 0 ? 64ull : static_cast<uint64_t>(__builtin_clzll(x));
}
static uint64_t wasm_i64_ctz(uint64_t x)
{
	return x == 0 ? 64ull : static_cast<uint64_t>(__builtin_ctzll(x));
}
static uint64_t wasm_i64_popcnt(uint64_t x)
{
	return static_cast<uint64_t>(__builtin_popcountll(static_cast<unsigned long long>(x)));
}
static float wasm_copysign_f32(float x, float y)
{
	return std::copysign(x, y);
}
static double wasm_copysign_f64(double x, double y)
{
	return std::copysign(x, y);
}

static int32_t wasm_i32_reinterpret_f32(float x)
{
	union {
		float f32;
		int32_t i32;
	} bits{.f32 = x};
	return bits.i32;
}

static int64_t wasm_i64_reinterpret_f64(double x)
{
	union {
		double f64;
		int64_t i64;
	} bits{.f64 = x};
	return bits.i64;
}

static float wasm_f32_reinterpret_i32(int32_t x)
{
	union {
		int32_t i32;
		float f32;
	} bits{.i32 = x};
	return bits.f32;
}

static double wasm_f64_reinterpret_i64(int64_t x)
{
	union {
		int64_t i64;
		double f64;
	} bits{.i64 = x};
	return bits.f64;
}

// 64-bit throughout (docs/proposals/MEMORY64.md M64-1): a memory64 grows by an
// i64 page delta and reports an i64 page count. The sentinel for "growth
// failed" is all-ones, which reads as -1 in either index width.
static uint64_t wasm_memory_grow_impl(WASM::VMContext* vm, uint32_t memIdx, uint64_t deltaPages)
{
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	const int64_t previous = m->growMemory(memIdx, deltaPages);
	return previous < 0 ? ~uint64_t{0} : static_cast<uint64_t>(previous);
}

// memory.copy may move bytes between two different memories (multi-memory),
// so both indices are carried through to the host helper.
static void wasm_memory_copy_impl(WASM::VMContext* vm, uint32_t dstMemIdx, uint32_t srcMemIdx,
								  int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	if (dstMemIdx >= vm->memoryCount || srcMemIdx >= vm->memoryCount) {
		reportTrap(WASM::TrapKind::Internal, "memory index out of range");
		return;
	}
	const WASM::LinearMemory* dstMem = vm->memories[dstMemIdx];
	const WASM::LinearMemory* srcMem = vm->memories[srcMemIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > dstMem->memorySize || usrc + ulen > srcMem->memorySize) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	std::memmove(dstMem->memoryBase + udst, srcMem->memoryBase + usrc, static_cast<size_t>(ulen));
}

static void wasm_memory_fill_impl(WASM::VMContext* vm, uint32_t memIdx, int32_t dst, int32_t value, int32_t len)
{
	if (dst < 0 || len < 0) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	if (memIdx >= vm->memoryCount) {
		reportTrap(WASM::TrapKind::Internal, "memory index out of range");
		return;
	}
	const WASM::LinearMemory* memory = vm->memories[memIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > memory->memorySize) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	std::memset(memory->memoryBase + udst, value & 0xFF, static_cast<size_t>(ulen));
}

// memory.discard: zero-fill a byte range, aligning the start down and the end
// up to the engine's 64 KiB page size (docs/proposals/MEMORY_CONTROL.md MC-2).
// The bounds check happens before any page is touched, and a zero-length range
// is a no-op so a valid empty range at the end of memory cannot trap.
static void wasm_memory_discard_impl(WASM::VMContext* vm, uint32_t memIdx,
									 uint64_t dst, uint64_t len)
{
	if (memIdx >= vm->memoryCount) {
		reportTrap(WASM::TrapKind::Internal, "memory index out of range");
		return;
	}
	const WASM::LinearMemory* memory = vm->memories[memIdx];
	const uint64_t size = memory->memorySize;
	if (dst > size || len > size - dst) {
		reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	if (len == 0)
		return;

	constexpr uint64_t page = 0x10000;
	const uint64_t start = dst & ~(page - 1);
	const uint64_t end = dst + len;
	const uint64_t rounded = end + (page - 1);
	uint64_t roundedEnd = (rounded < end) ? size : (rounded & ~(page - 1));
	if (roundedEnd > size)
		roundedEnd = size;
	if (roundedEnd > start) {
		// Shared memories can decommit through the StoreOwnedMemory hint; the
		// portable fallback (and private memories) zero-fill here.
		const auto* owner = static_cast<const WASM::StoreOwnedMemory*>(memory->hostData);
		if (owner != nullptr && owner->discardHint(start, roundedEnd - start))
			return;
		std::memset(memory->memoryBase + start, 0,
					static_cast<size_t>(roundedEnd - start));
	}
}

// 64x64 -> 128 multiplication in portable 32-bit limbs. Kept backend-local so
// the lowering does not depend on `__int128`
// (docs/proposals/NUMERIC_PROPOSALS.md NUM-1).
static void mulWideU64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi)
{
	const uint64_t a0 = a & 0xFFFFFFFFu;
	const uint64_t a1 = a >> 32;
	const uint64_t b0 = b & 0xFFFFFFFFu;
	const uint64_t b1 = b >> 32;
	const uint64_t p00 = a0 * b0;
	const uint64_t p01 = a0 * b1;
	const uint64_t p10 = a1 * b0;
	const uint64_t p11 = a1 * b1;
	const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
	lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
	hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static void mulWideS64(int64_t a, int64_t b, uint64_t& lo, uint64_t& hi)
{
	mulWideU64(static_cast<uint64_t>(a), static_cast<uint64_t>(b), lo, hi);
	// Correct the unsigned high half for the two's-complement sign bits.
	if (a < 0)
		hi -= static_cast<uint64_t>(b);
	if (b < 0)
		hi -= static_cast<uint64_t>(a);
}

static uint64_t wasm_i64_mul_wide_u_lo(uint64_t a, uint64_t b)
{
	uint64_t lo, hi;
	mulWideU64(a, b, lo, hi);
	(void)hi;
	return lo;
}

static uint64_t wasm_i64_mul_wide_u_hi(uint64_t a, uint64_t b)
{
	uint64_t lo, hi;
	mulWideU64(a, b, lo, hi);
	(void)lo;
	return hi;
}

static uint64_t wasm_i64_mul_wide_s_lo(int64_t a, int64_t b)
{
	uint64_t lo, hi;
	mulWideS64(a, b, lo, hi);
	(void)hi;
	return lo;
}

static uint64_t wasm_i64_mul_wide_s_hi(int64_t a, int64_t b)
{
	uint64_t lo, hi;
	mulWideS64(a, b, lo, hi);
	(void)lo;
	return hi;
}

static uint64_t wasm_table_grow_impl(WASM::VMContext* vm, uint32_t tableIdx, void* initRef, uint64_t deltaEntries)
{
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	if (vm->tables == nullptr || tableIdx >= vm->tableCount)
		return ~uint64_t{0};
	WASM::TableInstance* table = vm->tables[tableIdx];
	const uint64_t oldSize = table->size;
	if (!m->growTable(tableIdx, deltaEntries))
		return ~uint64_t{0};
	for (uint64_t i = oldSize; i < table->size; ++i)
		table->base[i] = static_cast<WASM::Callable*>(initRef);
	return oldSize;
}

static void wasm_table_fill_impl(WASM::VMContext* vm, uint32_t tableIdx, int32_t start, void* ref, int32_t len)
{
	if (start < 0 || len < 0) {
		reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	if (vm->tables == nullptr || tableIdx >= vm->tableCount) {
		reportTrap(WASM::TrapKind::Internal, "table index out of range");
		return;
	}
	WASM::TableInstance* table = vm->tables[tableIdx];
	const uint64_t ustart = static_cast<uint64_t>(start);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (ustart + ulen > table->size) {
		reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	for (uint64_t i = 0; i < ulen; ++i)
		table->base[ustart + i] = static_cast<WASM::Callable*>(ref);
}

static void wasm_table_copy_impl(WASM::VMContext* vm, uint32_t dstTableIdx, uint32_t srcTableIdx,
								 int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0) {
		reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	if (vm->tables == nullptr || dstTableIdx >= vm->tableCount || srcTableIdx >= vm->tableCount) {
		reportTrap(WASM::TrapKind::Internal, "table index out of range");
		return;
	}
	WASM::TableInstance* dstTable = vm->tables[dstTableIdx];
	WASM::TableInstance* srcTable = vm->tables[srcTableIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > dstTable->size || usrc + ulen > srcTable->size) {
		reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	std::memmove(dstTable->base + udst, srcTable->base + usrc,
				 static_cast<size_t>(ulen) * sizeof(WASM::Callable*));
}

static int32_t wasm_i32_trunc_sat_f32_s(float x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<float>(std::numeric_limits<int32_t>::min()))
		return std::numeric_limits<int32_t>::min();
	if (x >= static_cast<float>(std::numeric_limits<int32_t>::max()))
		return std::numeric_limits<int32_t>::max();
	return static_cast<int32_t>(x);
}

static uint32_t wasm_i32_trunc_sat_f32_u(float x)
{
	if (std::isnan(x) || x <= 0.0f)
		return 0;
	if (x >= static_cast<float>(std::numeric_limits<uint32_t>::max()))
		return std::numeric_limits<uint32_t>::max();
	return static_cast<uint32_t>(x);
}

static int32_t wasm_i32_trunc_sat_f64_s(double x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<double>(std::numeric_limits<int32_t>::min()))
		return std::numeric_limits<int32_t>::min();
	if (x >= static_cast<double>(std::numeric_limits<int32_t>::max()))
		return std::numeric_limits<int32_t>::max();
	return static_cast<int32_t>(x);
}

static uint32_t wasm_i32_trunc_sat_f64_u(double x)
{
	if (std::isnan(x) || x <= 0.0)
		return 0;
	if (x >= static_cast<double>(std::numeric_limits<uint32_t>::max()))
		return std::numeric_limits<uint32_t>::max();
	return static_cast<uint32_t>(x);
}

static int64_t wasm_i64_trunc_sat_f32_s(float x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<float>(std::numeric_limits<int64_t>::min()))
		return std::numeric_limits<int64_t>::min();
	if (x >= static_cast<float>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(x);
}

static uint64_t wasm_i64_trunc_sat_f32_u(float x)
{
	if (std::isnan(x) || x <= 0.0f)
		return 0;
	if (x >= static_cast<float>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(x);
}

static int64_t wasm_i64_trunc_sat_f64_s(double x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<double>(std::numeric_limits<int64_t>::min()))
		return std::numeric_limits<int64_t>::min();
	if (x >= static_cast<double>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(x);
}

static uint64_t wasm_i64_trunc_sat_f64_u(double x)
{
	if (std::isnan(x) || x <= 0.0)
		return 0;
	if (x >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(x);
}

// ── Trapping arithmetic ─────────────────────────────────────────────────────
// The integer divide/remainder and the *trapping* float→int conversions must
// trap on failure (docs/proposals/SMALL_PROPOSALS.md §3 keeps them distinct
// from trunc_sat_*). LibJIT's own div/convert do not check, so the check lives
// here and raises a real Wasm trap via WASM::trap (WasmTrap.hpp).
static int32_t wasm_i32_div_s(int32_t a, int32_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	if (a == std::numeric_limits<int32_t>::min() && b == -1) { reportTrap(WASM::TrapKind::IntegerOverflow); return 0; }
	return a / b;
}
static uint32_t wasm_i32_div_u(uint32_t a, uint32_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return a / b;
}
static int32_t wasm_i32_rem_s(int32_t a, int32_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return (b == -1) ? 0 : (a % b); // INT_MIN % -1 is UB in C++
}
static uint32_t wasm_i32_rem_u(uint32_t a, uint32_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return a % b;
}
static int64_t wasm_i64_div_s(int64_t a, int64_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	if (a == std::numeric_limits<int64_t>::min() && b == -1) { reportTrap(WASM::TrapKind::IntegerOverflow); return 0; }
	return a / b;
}
static uint64_t wasm_i64_div_u(uint64_t a, uint64_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return a / b;
}
static int64_t wasm_i64_rem_s(int64_t a, int64_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return (b == -1) ? 0 : (a % b);
}
static uint64_t wasm_i64_rem_u(uint64_t a, uint64_t b)
{
	if (b == 0) { reportTrap(WASM::TrapKind::IntegerDivideByZero); return 0; }
	return a % b;
}

static void trapInvalidConversion()
{
	reportTrap(WASM::TrapKind::InvalidConversionToInteger);
}
static void trapIntegerOverflow()
{
	reportTrap(WASM::TrapKind::IntegerOverflow);
}
static int32_t wasm_i32_trunc_f32_s(float x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	// The truncated result, not the input, must fit: allow (-2^31-1, 2^31).
	if (!(d > -2147483649.0 && d < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}
static uint32_t wasm_i32_trunc_f32_u(float x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}
static int32_t wasm_i32_trunc_f64_s(double x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -2147483649.0 && x < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}
static uint32_t wasm_i32_trunc_f64_u(double x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}
static int64_t wasm_i64_trunc_f32_s(float x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}
static uint64_t wasm_i64_trunc_f32_u(float x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}
static int64_t wasm_i64_trunc_f64_s(double x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}
static uint64_t wasm_i64_trunc_f64_u(double x)
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}

// WebAssembly min/max: a NaN operand yields an arithmetic (quiet) NaN, and the
// zero comparison is sign-aware (min(+0,-0) = -0, max(+0,-0) = +0).
static float wasm_f32_min(float a, float b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) || std::signbit(b)) ? -0.0f : 0.0f;
	return a < b ? a : b;
}
static float wasm_f32_max(float a, float b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) && std::signbit(b)) ? -0.0f : 0.0f;
	return a > b ? a : b;
}
static double wasm_f64_min(double a, double b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) || std::signbit(b)) ? -0.0 : 0.0;
	return a < b ? a : b;
}
static double wasm_f64_max(double a, double b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) && std::signbit(b)) ? -0.0 : 0.0;
	return a > b ? a : b;
}

// LibJIT's float rounding primitives pass a signalling NaN through unchanged;
// WebAssembly requires an arithmetic (quiet) NaN.
static float wasm_f32_trunc_impl(float x)
{
	if (std::isnan(x)) return std::numeric_limits<float>::quiet_NaN();
	return std::trunc(x);
}
static float wasm_f32_nearest_impl(float x)
{
	if (std::isnan(x)) return std::numeric_limits<float>::quiet_NaN();
	return std::nearbyint(x);
}
static double wasm_f64_trunc_impl(double x)
{
	if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
	return std::trunc(x);
}
static double wasm_f64_nearest_impl(double x)
{
	if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
	return std::nearbyint(x);
}

// ── Rounding-mode-control (docs/proposals/NUMERIC_PROPOSALS.md NUM-2) ──────
// The 0xFC 0x80–0xBB block repeats one 20-entry group three times: ceil,
// floor and trunc. The first five entries of a group are f32 arithmetic, the
// next five f64 arithmetic, then five f32 conversions and five f64
// conversions. The helpers take the raw sub-opcode and do the rounding on the
// exact (long double) intermediate rather than through the host FP mode.
static uint32_t roundingMode(uint32_t sub) { return (sub - 0x80u) / 0x14u; }

// Round an exact real result to the nearest representable f32/f64 in the
// requested direction. `std::ceil` followed by a cast would double-round a
// value such as 16777217 to 16777216.
static float roundToFloat(long double v, uint32_t mode)
{
	if (std::isnan(static_cast<double>(v)))
		return std::numeric_limits<float>::quiet_NaN();
	float r = static_cast<float>(v);
	if (std::isinf(r))
		return r;
	const long double back = static_cast<long double>(r);
	switch (mode) {
	case 0: if (back < v) r = std::nextafter(r, std::numeric_limits<float>::infinity()); break;
	case 1: if (back > v) r = std::nextafter(r, -std::numeric_limits<float>::infinity()); break;
	default:
		if (v >= 0.0L) { if (back > v) r = std::nextafter(r, -std::numeric_limits<float>::infinity()); }
		else { if (back < v) r = std::nextafter(r, std::numeric_limits<float>::infinity()); }
		break;
	}
	return r;
}

static double roundToDouble(long double v, uint32_t mode)
{
	if (std::isnan(static_cast<double>(v)))
		return std::numeric_limits<double>::quiet_NaN();
	double r = static_cast<double>(v);
	if (std::isinf(r))
		return r;
	const long double back = static_cast<long double>(r);
	switch (mode) {
	case 0: if (back < v) r = std::nextafter(r, std::numeric_limits<double>::infinity()); break;
	case 1: if (back > v) r = std::nextafter(r, -std::numeric_limits<double>::infinity()); break;
	default:
		if (v >= 0.0L) { if (back > v) r = std::nextafter(r, -std::numeric_limits<double>::infinity()); }
		else { if (back < v) r = std::nextafter(r, std::numeric_limits<double>::infinity()); }
		break;
	}
	return r;
}

static float wasm_rounding_f32_unary(uint32_t sub, float a)
{
	const uint32_t rel = (sub - 0x80u) % 0x14u;
	long double v = (rel == 0) ? std::sqrt(static_cast<long double>(a))
							   : static_cast<long double>(a);
	return roundToFloat(v, roundingMode(sub));
}

static float wasm_rounding_f32_binary(uint32_t sub, float a, float b)
{
	const uint32_t rel = (sub - 0x80u) % 0x14u;
	long double x = static_cast<long double>(a);
	long double y = static_cast<long double>(b);
	long double v = x;
	switch (rel) {
	case 1: v = x + y; break;
	case 2: v = x - y; break;
	case 3: v = x * y; break;
	case 4: v = x / y; break;
	default: v = x; break;
	}
	return roundToFloat(v, roundingMode(sub));
}

static double wasm_rounding_f64_unary(uint32_t sub, double a)
{
	const uint32_t rel = (sub - 0x80u) % 0x14u;
	long double v = (rel == 5) ? std::sqrt(static_cast<long double>(a))
							   : static_cast<long double>(a);
	return roundToDouble(v, roundingMode(sub));
}

static double wasm_rounding_f64_binary(uint32_t sub, double a, double b)
{
	const uint32_t rel = (sub - 0x80u) % 0x14u;
	long double x = static_cast<long double>(a);
	long double y = static_cast<long double>(b);
	long double v = x;
	switch (rel) {
	case 6: v = x + y; break;
	case 7: v = x - y; break;
	case 8: v = x * y; break;
	case 9: v = x / y; break;
	default: v = x; break;
	}
	return roundToDouble(v, roundingMode(sub));
}

static float wasm_rounding_i32_s_to_f32(uint32_t sub, int32_t v)
{
	return roundToFloat(static_cast<long double>(v), roundingMode(sub));
}
static float wasm_rounding_i32_u_to_f32(uint32_t sub, uint32_t v)
{
	return roundToFloat(static_cast<long double>(v), roundingMode(sub));
}
static float wasm_rounding_i64_s_to_f32(uint32_t sub, int64_t v)
{
	return roundToFloat(static_cast<long double>(v), roundingMode(sub));
}
static float wasm_rounding_i64_u_to_f32(uint32_t sub, uint64_t v)
{
	return roundToFloat(static_cast<long double>(v), roundingMode(sub));
}
static float wasm_rounding_f64_to_f32(uint32_t sub, double v)
{
	return roundToFloat(static_cast<long double>(v), roundingMode(sub));
}
static double wasm_rounding_i32_s_to_f64(uint32_t sub, int32_t v)
{
	return roundToDouble(static_cast<long double>(v), roundingMode(sub));
}
static double wasm_rounding_i32_u_to_f64(uint32_t sub, uint32_t v)
{
	return roundToDouble(static_cast<long double>(v), roundingMode(sub));
}
static double wasm_rounding_i64_s_to_f64(uint32_t sub, int64_t v)
{
	return roundToDouble(static_cast<long double>(v), roundingMode(sub));
}
static double wasm_rounding_i64_u_to_f64(uint32_t sub, uint64_t v)
{
	return roundToDouble(static_cast<long double>(v), roundingMode(sub));
}
static double wasm_rounding_f32_to_f64(uint32_t sub, float v)
{
	return roundToDouble(static_cast<long double>(v), roundingMode(sub));
}

// LibJIT's ulong -> float conversion mishandles values >= 2^63; the C++
// conversion is correctly rounded for the whole unsigned range.
static float wasm_f32_convert_i64_u_impl(uint64_t v) { return static_cast<float>(v); }
static double wasm_f64_convert_i64_u_impl(uint64_t v) { return static_cast<double>(v); }

static void* wasm_struct_new_default_impl(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
}

// Custom Descriptors: the descriptor operand is retained by the new object.
static void* wasm_struct_new_desc_impl(WASM::VMContext* vm, uint32_t size,
		uint32_t typeIndex, void* descriptor)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(
			size, typeIndex, descriptor);
}

static void* wasm_ref_get_desc_impl(WASM::VMContext* vm, void* ref)
{
	WASM::ModuleInstance* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	if (ref == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullStructureReference,
				"ref.get_desc: null structure reference");
		return nullptr;
	}
	void* descriptor = instance->structDescriptor(ref);
	if (descriptor == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullReference,
				"ref.get_desc: null custom descriptor");
		return nullptr;
	}
	return descriptor;
}

static int32_t wasm_desc_eq_impl(WASM::VMContext* vm, void* ref, void* descriptor,
		int32_t allowNull)
{
	WASM::ModuleInstance* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	if (descriptor == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullReference,
				"cast_desc_eq: null custom descriptor");
		return 0;
	}
	if (ref == nullptr) {
		if (allowNull)
			return 1;
		WASM::reportTrap(WASM::TrapKind::NullStructureReference,
				"cast_desc_eq: null structure reference");
		return 0;
	}
	return instance->structDescriptor(ref) == descriptor ? 1 : 0;
}

static void* wasm_array_new_default_impl(WASM::VMContext* vm, uint32_t dataOffset, uint32_t elementSize, uint32_t length, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateArrayObject(dataOffset, elementSize, length, typeIndex);
}

static void* wasm_i31_new_impl(int32_t value)
{
	uintptr_t raw = (static_cast<uint32_t>(value) << 1u) | 1u;
	return reinterpret_cast<void*>(raw);
}

static int32_t wasm_i31_get_s_impl(void* ref)
{
	uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ref));
	return static_cast<int32_t>(raw) >> 1;
}

static int32_t wasm_i31_get_u_impl(void* ref)
{
	uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ref));
	return static_cast<int32_t>(raw >> 1);
}

static void wasm_memory_init_impl(WASM::VMContext* vm, uint32_t memIdx, uint32_t dataIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit(memIdx, dataIdx, dst, src, len);
}

static void wasm_data_drop_impl(WASM::VMContext* vm, uint32_t dataIdx)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->dataDrop(dataIdx);
}

static void wasm_table_init_impl(WASM::VMContext* vm, uint32_t elemIdx, uint32_t tableIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->tableInit(elemIdx, tableIdx, dst, src, len);
}

static void wasm_elem_drop_impl(WASM::VMContext* vm, uint32_t elemIdx)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->elemDrop(elemIdx);
}

static void wasm_buffer_init_from_data_impl(WASM::VMContext* vm, uint32_t dataIdx, void* dst, uint32_t src, uint32_t lenBytes)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->bufferInitFromData(dataIdx, dst, src, lenBytes);
}

static void wasm_buffer_init_from_elems_impl(WASM::VMContext* vm, uint32_t elemIdx, void* dst, uint32_t src, uint32_t lenElems)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->bufferInitFromElems(elemIdx, dst, src, lenElems);
}

// More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2). Each runtime
// method performs the GC-5 range check before allocating and reports a trap
// (returning null) on failure, so the caller checks the pending slot.
static void* wasm_array_new_array_impl(WASM::VMContext* vm, uint32_t dstTypeIdx,
		uint32_t srcTypeIdx, void* src, uint32_t start, uint32_t length)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewArray(
			dstTypeIdx, srcTypeIdx, src, start, length);
}

static void* wasm_array_new_memory_impl(WASM::VMContext* vm, uint32_t dstTypeIdx,
		uint32_t memIdx, uint32_t start, uint32_t length)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewMemory(
			dstTypeIdx, memIdx, start, length);
}

static void* wasm_array_new_table_impl(WASM::VMContext* vm, uint32_t dstTypeIdx,
		uint32_t tableIdx, uint32_t start, uint32_t length)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewTable(
			dstTypeIdx, tableIdx, start, length);
}

static int32_t wasm_ref_matches_heap_type_impl(WASM::VMContext* vm, void* ref, int32_t heapType, int32_t isTypeIndex, int32_t nullable)
{
	WASM::HeapType ht;
	ht.isTypeIndex = (isTypeIndex != 0);
	if (ht.isTypeIndex)
		ht.typeIndex = static_cast<uint32_t>(heapType);
	else
		ht.abstract = static_cast<WASM::AbstractHeapType>(heapType);
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->refMatchesHeapType(ref, ht, nullable != 0) ? 1 : 0;
}

// The extern hierarchy bridge (docs/proposals/GC.md GC-8 / RT-1). The runtime
// method owns the box allocation and the inner-reference count; these are the
// plain C entry points generated code calls.
static void* wasm_extern_box_impl(WASM::VMContext* vm, void* ref)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->externBox(ref);
}

static void* wasm_extern_unbox_impl(WASM::VMContext* vm, void* ref)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->externUnbox(ref);
}

// Copies `lenElems` elements between two inline buffers. When the element type
// is a reference, every destination element must also be *retained*: the copy
// creates a new owning slot for the object (docs/proposals/GC.md RC-5), so a
// later overwrite releases a counted value instead of underflowing.
static void wasm_buffer_copy_impl(WASM::VMContext* vm, void* dst, uint32_t dstIndex, void* src, uint32_t srcIndex, uint32_t lenElems, uint32_t elemSize, int32_t isRefElement)
{
	uint8_t* d = static_cast<uint8_t*>(dst) + static_cast<size_t>(dstIndex) * elemSize;
	const uint8_t* s = static_cast<const uint8_t*>(src) + static_cast<size_t>(srcIndex) * elemSize;
	std::memmove(d, s, static_cast<size_t>(lenElems) * elemSize);
	if (isRefElement == 0)
		return;
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	for (uint32_t i = 0; i < lenElems; ++i) {
		void* ref = nullptr;
		std::memcpy(&ref, d + static_cast<size_t>(i) * elemSize, sizeof(void*));
		instance->refClone(ref);
	}
}

// ── Reference-counting helpers (docs/proposals/GC.md D15/RC-1) ──────────────
// `refClone`/`refDrop` are the retain/release pair; `refStore` is the write
// barrier every reference-typed slot goes through. They classify the reference
// in the runtime (a GC object vs an i31 immediate / externref / funcref), so
// the JIT does not need to know the counts.
[[maybe_unused]] static void* wasm_ref_clone_impl(WASM::VMContext* vm, void* ref)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->refClone(ref);
}

[[maybe_unused]] static void wasm_ref_drop_impl(WASM::VMContext* vm, void* ref)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->refDrop(ref);
}

static void wasm_ref_store_impl(WASM::VMContext* vm, void* slot, void* newRef)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->refStore(reinterpret_cast<void**>(slot), newRef);
}
}

jit_value_t callNativeUnary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT, jit_value_t a)
{
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, &argT, 1, 1);
	jit_value_t args[] = {a};
	return jit_insn_call_native(fn, "wasm.unary", fptr, sig, args, 1, 0);
}

// ── Operand-stack ownership (docs/proposals/GC.md RC-5) ─────────────────────
// When true, every reference pushed on the operand stack is cloned (the stack
// owns a count) and every popped reference is released. This makes counts exact,
// so reclamation reaches objects held only transiently.
//
// ENABLED. It used to crash on a module that pushed a null `jit_value_t` (an
// unmodelled value type such as `exnref` lowered to `jit_type_void`); such a
// module is now rejected at load — Module::validateValueTypes and the
// jitTypeForValueType guard — so no null value can reach the stack. See
// docs/UNMODELLED_VALUE_TYPES.md.
constexpr bool kStackOwnsRefs = true;

// True when a JIT value is a reference (a pointer-typed value). A null JIT type
// means an unsupported value type — e.g. `exnref`, which this engine does not
// represent — and is treated as a non-reference.
static bool isRefJitValue(jit_value_t v)
{
	jit_type_t t = jit_value_get_type(v);
	// Exclude tagged types (`i31`): LibJIT reports them as pointers, but they are
	// not GC objects, and a void_ptr round-trip through a tagged type breaks.
	return t != nullptr && jit_type_is_pointer(t) && !jit_type_is_tagged(t);
}

jit_value_t callNativeBinary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT1, jit_type_t argT2,
							 jit_value_t a, jit_value_t b)
{
	jit_type_t params[] = {argT1, argT2};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, params, 2, 1);
	jit_value_t args[] = {a, b};
	return jit_insn_call_native(fn, "wasm.binary", fptr, sig, args, 2, 0);
}

jit_value_t callNativeTernary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT1, jit_type_t argT2,
							  jit_type_t argT3, jit_value_t a, jit_value_t b, jit_value_t c)
{
	jit_type_t params[] = {argT1, argT2, argT3};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, params, 3, 1);
	jit_value_t args[] = {a, b, c};
	return jit_insn_call_native(fn, "wasm.ternary", fptr, sig, args, 3, 0);
}

jit_value_t i32Rotl(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c31 = jit_value_create_nint_constant(fn, jit_type_int, 31);
	jit_value_t c32 = jit_value_create_nint_constant(fn, jit_type_int, 32);
	jit_value_t k = jit_insn_and(fn, r, c31);
	jit_value_t inv = jit_insn_sub(fn, c32, k);
	jit_value_t km = jit_insn_and(fn, inv, c31);
	jit_value_t hi = jit_insn_shl(fn, n, k);
	jit_value_t lo = jit_insn_ushr(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i32Rotr(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c31 = jit_value_create_nint_constant(fn, jit_type_int, 31);
	jit_value_t c32 = jit_value_create_nint_constant(fn, jit_type_int, 32);
	jit_value_t k = jit_insn_and(fn, r, c31);
	jit_value_t inv = jit_insn_sub(fn, c32, k);
	jit_value_t km = jit_insn_and(fn, inv, c31);
	jit_value_t lo = jit_insn_ushr(fn, n, k);
	jit_value_t hi = jit_insn_shl(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i64Rotl(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c63 = jit_value_create_long_constant(fn, jit_type_long, 63);
	jit_value_t c64 = jit_value_create_long_constant(fn, jit_type_long, 64);
	jit_value_t k = jit_insn_and(fn, r, c63);
	jit_value_t inv = jit_insn_sub(fn, c64, k);
	jit_value_t km = jit_insn_and(fn, inv, c63);
	jit_value_t hi = jit_insn_shl(fn, n, k);
	jit_value_t lo = jit_insn_ushr(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i64Rotr(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c63 = jit_value_create_long_constant(fn, jit_type_long, 63);
	jit_value_t c64 = jit_value_create_long_constant(fn, jit_type_long, 64);
	jit_value_t k = jit_insn_and(fn, r, c63);
	jit_value_t inv = jit_insn_sub(fn, c64, k);
	jit_value_t km = jit_insn_and(fn, inv, c63);
	jit_value_t lo = jit_insn_ushr(fn, n, k);
	jit_value_t hi = jit_insn_shl(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

} // namespace

void OpcodeDispatcher::pushValue(jit_value_t v)
{
	// In unreachable code the operand stack is polymorphic and the emitted
	// values are dead; don't let them accumulate in the compile-time model.
	if (unreachableCode)
		return;
	// A reference on the operand stack *owns* one count (docs/proposals/GC.md
	// RC-5): clone it, so the stack holds its own reference and every pop can
	// release it. Producers that already hold an owned value (a call result, or
	// a pass-through such as `ref.cast`) use pushOwnedValue instead.
	if (kStackOwnsRefs && isRefJitValue(v))
		v = castRefValue(emitRefClone(v), jit_value_get_type(v));
	valueStack.push_back(v);
}

void OpcodeDispatcher::pushOwnedValue(jit_value_t v)
{
	// Push a reference whose ownership the stack takes over: no clone. Used by
	// owned producers (call results) and by pass-through instructions where the
	// popped value's ownership simply moves.
	if (unreachableCode)
		return;
	valueStack.push_back(v);
}

void OpcodeDispatcher::emitDropValue(jit_value_t v)
{
	// Release the operand stack's ownership of a popped reference.
	if (kStackOwnsRefs && isRefJitValue(v))
		emitRefDrop(v);
}

void OpcodeDispatcher::emitDropRefLocals()
{
	// At function exit, every reference-typed *local* (params are borrows, so
	// they are skipped) releases the count it owns (docs/proposals/GC.md RC-5).
	if (!kStackOwnsRefs)
		return;
	for (size_t i = currentFunc.params.size(); i < locals.size(); ++i) {
		jit_value_t lv = jit_insn_load(function, locals[i]);
		if (isRefJitValue(lv))
			emitRefDrop(lv);
	}
}

jit_value_t OpcodeDispatcher::popValue()
{
	if (unreachableCode) {
		// Operand of an instruction in unreachable code: the Wasm operand stack
		// is polymorphic there, so synthesize a placeholder and leave the
		// compile-time stack untouched.
		return jit_value_create_nint_constant(function, jit_type_int, 0);
	}
	assert(!valueStack.empty());
	jit_value_t v = valueStack.back();
	valueStack.pop_back();
	return v;
}

void OpcodeDispatcher::markUnreachable()
{
	unreachableCode = true;
}

jit_value_t OpcodeDispatcher::zeroConstantForType(jit_type_t t)
{
	if (jit_type_is_struct(t)) {
		// A struct-typed value is written through its *address*: in this LibJIT
		// build `jit_insn_store_relative` does not write a `jit_value_create`d
		// aggregate (verified — see the note in the header), so the old
		// field-by-field loop would hand back an uninitialised slot. `v128` is
		// the only struct type that reaches here; the SIMD helper zeroes its 16
		// bytes, which is also what a `v128` local/block-parameter needs.
		jit_value_t aggregate = jit_value_create(function, t);
		if (jit_type_get_size(t) == 16) {
			jit_type_t params[1] = {jit_type_void_ptr};
			jit_value_t args[1] = {jit_insn_address_of(function, aggregate)};
			callSimdHelper(reinterpret_cast<void*>(wasm_v128_zero), jit_type_void, params, args);
			return aggregate;
		}
		const unsigned int fieldCount = jit_type_num_fields(t);
		for (unsigned int i = 0; i < fieldCount; ++i) {
			jit_type_t fieldType = jit_type_get_field(t, i);
			jit_nint offset = static_cast<jit_nint>(jit_type_get_offset(t, i));
			jit_insn_store_relative(function, aggregate, offset, zeroConstantForType(fieldType));
		}
		return jit_insn_load(function, aggregate);
	}
	if (t == jit_type_int || t == jit_type_uint)
		return jit_value_create_nint_constant(function, t, 0);
	if (t == jit_type_long || t == jit_type_ulong)
		return jit_value_create_long_constant(function, t, 0);
	if (t == jit_type_float32)
		return jit_value_create_float32_constant(function, t, 0.f);
	if (t == jit_type_float64)
		return jit_value_create_float64_constant(function, t, 0.0);
	if (jit_type_is_pointer(t) || t == jit_type_void_ptr || jit_type_is_tagged(t))
		return jit_value_create_nint_constant(function, t, 0);
	// v128 / tagged: best-effort zero — expand when those opcodes are implemented
	return jit_value_create_nint_constant(function, t, 0);
}

// ── SIMD lowering (docs/SIMD_LIBJIT.md §6) ──────────────────────────────────

jit_type_t OpcodeDispatcher::v128Type()
{
	// The translator's lowered `{i32,i32,i32,i32}` struct: 16 bytes, 16-byte
	// aligned (forced in LibJitTypeTranslator's constructor). Memoised per
	// function — the translator's cache is keyed by value type, so this is just
	// an extra indirection saved.
	if (simdCachedType == nullptr) {
		WASM::ValueType vt;
		vt.opcode = WASM::ValueTypeCode::V128;
		vt.heapType = -1;
		vt.isExact = false;
		vt.isShared = false;
		simdCachedType = jitTypeForValueType(vt);
	}
	return simdCachedType;
}

jit_value_t OpcodeDispatcher::newV128Slot()
{
	// A struct-typed LibJIT local is the storage: helpers read and write its 16
	// bytes through `jit_insn_address_of` (§6.2/§6.3).
	return jit_value_create(function, v128Type());
}

jit_value_t OpcodeDispatcher::callSimdHelper(void* fptr, jit_type_t ret,
		std::span<const jit_type_t> params, std::span<const jit_value_t> args)
{
	// Memoise by (return type, parameter types): the SIMD ops share a handful of
	// shapes, so a function with hundreds of SIMD instructions still creates a
	// handful of signatures instead of one per instruction (§6.4).
	std::vector<jit_type_t> key;
	key.reserve(params.size() + 1);
	key.push_back(ret);
	key.insert(key.end(), params.begin(), params.end());

	jit_type_t sig = nullptr;
	for (const auto& entry : simdSignatureCache) {
		if (entry.first == key) {
			sig = entry.second;
			break;
		}
	}
	if (sig == nullptr) {
		std::vector<jit_type_t> mutableParams(params.begin(), params.end());
		sig = jit_type_create_signature(jit_abi_cdecl, ret, mutableParams.data(),
				static_cast<unsigned>(mutableParams.size()), 1);
		simdSignatureCache.emplace_back(std::move(key), sig);
	}

	jit_value_t result = jit_insn_call_native(function, "wasm.simd", fptr, sig,
			const_cast<jit_value_t*>(args.data()), static_cast<unsigned>(args.size()), 0);
	// A SIMD helper is allowed to report (it is engine code like any other); the
	// check is emitted whether or not today's helpers can trap, so a later one
	// cannot forget it.
	emitPendingTrapCheck();
	return result;
}

jit_value_t OpcodeDispatcher::callSimdOutHelper(void* fptr, jit_value_t out,
		std::span<const jit_type_t> leadingTypes, std::span<const jit_value_t> leadingArgs)
{
	// The out-slot shape of §6.3: leading operands, then the result's address.
	std::vector<jit_type_t> params(leadingTypes.begin(), leadingTypes.end());
	params.push_back(jit_type_void_ptr);
	std::vector<jit_value_t> args(leadingArgs.begin(), leadingArgs.end());
	args.push_back(jit_insn_address_of(function, out));
	return callSimdHelper(fptr, jit_type_void, params, args);
}

// The four `v128.load*` / `v128.store*` shapes (docs/SIMD_LIBJIT.md §6.3).

void OpcodeDispatcher::emitSimdLoad(const WASM::MemArg& ma, jit_nint width, void* helper)
{
	// In unreachable code the operand stack is polymorphic, so the address would
	// be a placeholder and the bounds check would be emitted for nothing.
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t src = effectiveMemoryAddress(ma, width);
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {src};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdLoadLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t a = popValue();   // wasm's `[i32 v128] -> [v128]`: vector on top
	jit_value_t src = effectiveMemoryAddress(ma, width);
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint};
	jit_value_t args[3] = {src, jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, lane)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdStore(const WASM::MemArg& ma, jit_nint width, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t value = popValue();   // wasm's `[i32 v128] -> []`: value on top
	jit_value_t dst = effectiveMemoryAddress(ma, width);
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {dst, jit_insn_address_of(function, value)};
	callSimdHelper(helper, jit_type_void, types, args);
}

void OpcodeDispatcher::emitSimdStoreLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t a = popValue();
	jit_value_t dst = effectiveMemoryAddress(ma, width);
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint};
	jit_value_t args[3] = {dst, jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, lane)};
	callSimdHelper(helper, jit_type_void, types, args);
}

// The per-lane shapes. Both take vector operands by address and write the result
// into a fresh slot, so the arithmetic and the mask-producing comparisons share
// one path — the difference is only which helper is called.

void OpcodeDispatcher::emitSimdBinaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t b = popValue();   // wasm's `[v128 v128] -> [v128]`: second on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {jit_insn_address_of(function, a), jit_insn_address_of(function, b)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdUnaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {jit_insn_address_of(function, a)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdTernaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		popValue();
		return;
	}
	jit_value_t c = popValue();   // the selector is on top
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_insn_address_of(function, b), jit_insn_address_of(function, c)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdShiftHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t count = popValue();   // `[v128 i32] -> [v128]`: the count is on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_int};
	jit_value_t args[2] = {jit_insn_address_of(function, a), count};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdReduceI32Helper(void* helper)
{
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t a = popValue();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {jit_insn_address_of(function, a)};
	// No out slot: the helper's result *is* the wasm i32 the op produces.
	pushValue(callSimdHelper(helper, jit_type_int, types, args));
}

jit_value_t OpcodeDispatcher::castRefValue(jit_value_t value, jit_type_t targetType)
{
	jit_type_t sourceType = jit_value_get_type(value);
	if (sourceType == targetType)
		return value;
	return jit_insn_convert(function, value, targetType, 0);
}

jit_value_t OpcodeDispatcher::refAsVoidPtr(jit_value_t value)
{
	return castRefValue(value, jit_type_void_ptr);
}

jit_value_t OpcodeDispatcher::typedNullRef(jit_type_t refType)
{
	return zeroConstantForType(refType);
}

jit_value_t OpcodeDispatcher::emitRefTypeTest(jit_value_t ref, const WASM::HeapType& heapType, bool nullable)
{
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		refAsVoidPtr(ref),
		jit_value_create_nint_constant(function, jit_type_int,
			heapType.isTypeIndex ? static_cast<jit_nint>(heapType.typeIndex) : static_cast<jit_nint>(heapType.abstract)),
		jit_value_create_nint_constant(function, jit_type_int, heapType.isTypeIndex ? 1 : 0),
		jit_value_create_nint_constant(function, jit_type_int, nullable ? 1 : 0)
	};
	return jit_insn_call_native(function, "wasm_ref_matches_heap_type_impl",
								reinterpret_cast<void*>(wasm_ref_matches_heap_type_impl),
								sig, args, 5, 0);
}

void OpcodeDispatcher::emitRefStore(jit_value_t slotAddr, jit_value_t newRef)
{
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 3, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(slotAddr), refAsVoidPtr(newRef)};
	jit_insn_call_native(function, "wasm_ref_store_impl",
			reinterpret_cast<void*>(wasm_ref_store_impl), sig, args, 3, 0);
	emitPendingTrapCheck();
}

jit_value_t OpcodeDispatcher::emitRefClone(jit_value_t ref)
{
	// `wasm_ref_clone(ref)` -> the same reference, count +1.
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_value_t cloned = jit_insn_call_native(function, "wasm_ref_clone_impl",
			reinterpret_cast<void*>(wasm_ref_clone_impl), sig, args, 2, 0);
	emitPendingTrapCheck();
	return cloned;
}

void OpcodeDispatcher::emitRefDrop(jit_value_t ref)
{
	// `wasm_ref_drop(ref)` -> count -1 (reports on underflow).
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_insn_call_native(function, "wasm_ref_drop_impl",
			reinterpret_cast<void*>(wasm_ref_drop_impl), sig, args, 2, 0);
	emitPendingTrapCheck();
}

jit_type_t OpcodeDispatcher::tableElementJitType(WASM::TableIdx arg) const
{
	if (arg < module.importTables.size())
		return typeTranslator.translateType(module.importTables[arg].table.elementType);
	const size_t internalIdx = arg - module.importTables.size();
	assert(internalIdx < module.tables.size());
	return typeTranslator.translateType(module.tables[internalIdx].elementType);
}

jit_type_t OpcodeDispatcher::jitRefTypeForHeapType(const WASM::HeapType& ht, bool nullable)
{
	WASM::ValueType vt;
	vt.opcode = nullable ? WASM::ValueTypeCode::RefNull : WASM::ValueTypeCode::Ref;
	vt.heapType = ht.isTypeIndex ? ht.typeIndex : static_cast<int32_t>(ht.abstract);
	vt.isExact = ht.isExact;
	vt.isShared = ht.isShared;
	return jitTypeForValueType(vt);
}

jit_type_t OpcodeDispatcher::structRefType(WASM::TypeIdx arg, bool nullable) const
{
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = arg;
	return const_cast<OpcodeDispatcher*>(this)->jitRefTypeForHeapType(ht, nullable);
}

jit_type_t OpcodeDispatcher::arrayRefType(WASM::TypeIdx arg, bool nullable) const
{
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = arg;
	return const_cast<OpcodeDispatcher*>(this)->jitRefTypeForHeapType(ht, nullable);
}

WASM::CoreTypeView OpcodeDispatcher::structTypeForIndex(WASM::TypeIdx arg) const
{
	assert(arg < module.typeCount());
	const WASM::CoreTypeView view = module.typeView(arg);
	assert(view.kind() == WASM::CanonTypeKind::Struct);
	return view;
}

WASM::CoreTypeView OpcodeDispatcher::arrayTypeForIndex(WASM::TypeIdx arg) const
{
	assert(arg < module.typeCount());
	const WASM::CoreTypeView view = module.typeView(arg);
	assert(view.kind() == WASM::CanonTypeKind::Array);
	return view;
}

jit_type_t OpcodeDispatcher::arrayElementJitType(WASM::TypeIdx arg) const
{
	return typeTranslator.translateCanonStorage(arrayTypeForIndex(arg).arrayElement().storage);
}

jit_nint OpcodeDispatcher::structFieldOffset(WASM::TypeIdx arg, uint32_t fieldIndex) const
{
	jit_type_t structType = jitTypeForTypeIdx(arg);
	return static_cast<jit_nint>(jit_type_get_offset(structType, fieldIndex + WASM::GcHeader::kHeaderFields));
}

jit_nint OpcodeDispatcher::arrayLengthOffset(WASM::TypeIdx arg) const
{
	// [TypeId][refcount][length] — fixed for every array.
	(void)arg;
	return static_cast<jit_nint>(WASM::GcHeader::kArrayLengthOffset);
}

jit_nint OpcodeDispatcher::arrayDataOffset(WASM::TypeIdx arg) const
{
	// Elements are inline; the offset depends only on the element's alignment,
	// and must match the runtime's allocateArrayObject / gcArrayDataOffset (H1).
	return static_cast<jit_nint>(
			WASM::gcArrayDataOffset(arrayTypeForIndex(arg).arrayElement().storage));
}

jit_value_t OpcodeDispatcher::arrayElementBase(jit_value_t arrayRef, WASM::TypeIdx arg)
{
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t addr = jit_insn_add(function, refAsVoidPtr(arrayRef),
			jit_value_create_nint_constant(function, jit_type_nint, arrayDataOffset(arg)));
	return castRefValue(addr, dataPtrType);
}

jit_type_t OpcodeDispatcher::resultTupleType(const WASM::FuncType& wasm_func)
{
	// The result buffer of a multi-result function. A fresh struct type per
	// site is deliberate: only its field offsets and field types are used, and
	// LibJIT keeps every type alive for the context's lifetime. Deriving it
	// from the FuncType on both sides keeps caller and callee agreeing.
	return typeTranslator.translateStruct(wasm_func.results);
}

jit_value_t OpcodeDispatcher::newResultBuffer(const WASM::FuncType& calleeSig)
{
	if (calleeSig.results.size() <= 1)
		return nullptr;
	jit_value_t slot = jit_value_create(function, resultTupleType(calleeSig));
	// A struct-typed `jit_value_create` is a real frame slot; pass its address
	// rather than the aggregate (docs/SIMD_LIBJIT.md §6.1).
	return jit_insn_address_of(function, slot);
}

void OpcodeDispatcher::packReturnValues(jit_type_t tupleType, jit_value_t out, size_t resultCount)
{
	assert(jit_type_is_struct(tupleType));
	assert(jit_type_num_fields(tupleType) == resultCount);
	assert(out != nullptr);

	// Pop in reverse so field 0 is the bottom-most result, matching the tuple
	// layout that pushCallResults reads back.
	std::vector<jit_value_t> values(resultCount);
	for (size_t i = resultCount; i-- > 0; )
		values[i] = popValue();
	for (size_t i = 0; i < resultCount; ++i) {
		jit_value_t value = values[i];
		// Canonicalise only float fields; integer/vector results are untouched.
		if (i < currentFunc.results.size()) {
			const WASM::ValueTypeCode code = currentFunc.results[i].val.opcode;
			if (code == WASM::ValueTypeCode::F32)
				value = canonicalizeFloat(value, false);
			else if (code == WASM::ValueTypeCode::F64)
				value = canonicalizeFloat(value, true);
		}
		jit_nuint fieldOffset = jit_type_get_offset(tupleType, static_cast<unsigned int>(i));
		jit_insn_store_relative(function, out, static_cast<jit_nint>(fieldOffset), value);
	}
}

void OpcodeDispatcher::pushCallResults(const WASM::FuncType& calleeSig, jit_value_t ret, jit_value_t outBuffer)
{
	if (calleeSig.results.empty()) {
		(void)ret;
		(void)outBuffer;
		return;
	}
	if (calleeSig.results.size() == 1) {
		// A call result is already owned by the caller (the callee transferred
		// it), so it is pushed without an extra clone.
		(void)outBuffer;
		pushOwnedValue(ret);
		return;
	}

	// The callee wrote its results into the caller's tuple; read them back.
	assert(outBuffer != nullptr);
	jit_type_t tupleType = resultTupleType(calleeSig);
	for (size_t i = 0; i < calleeSig.results.size(); ++i) {
		jit_type_t fieldType = jit_type_get_field(tupleType, static_cast<unsigned int>(i));
		jit_nuint fieldOffset = jit_type_get_offset(tupleType, static_cast<unsigned int>(i));
		pushOwnedValue(jit_insn_load_relative(function, outBuffer, static_cast<jit_nint>(fieldOffset), fieldType));
	}
}

// ── Exceptions (docs/proposals/EXCEPTIONS.md EX-3/EX-4) ─────────────────────

WASM::FuncType OpcodeDispatcher::tagPayloadType(WASM::TagIdx tagIdx) const
{
	const size_t imported = module.importTags.size();
	uint32_t typeIdx = 0;
	if (tagIdx < imported) {
		typeIdx = module.importTags[tagIdx].typeIdx;
	} else {
		const size_t local = tagIdx - imported;
		if (local >= module.tags.size())
			throw std::runtime_error("tag index out of range for this module");
		typeIdx = module.tags[local].typeIdx;
	}
	if (typeIdx >= module.typeCount()
			|| module.typeView(typeIdx).kind() != WASM::CanonTypeKind::Func)
		throw std::runtime_error("tag does not name a function type");
	return module.funcTypeAt(typeIdx);
}

WASM::TagId OpcodeDispatcher::tagIdForClause(const WASM::CatchClause& clause) const
{
	if (clause.tagIdx >= internals.tagIds.size())
		throw std::runtime_error("try_table catch names an out-of-range tag");
	return internals.tagIds[clause.tagIdx];
}

jit_value_t OpcodeDispatcher::createException(WASM::TagId tagId, const WASM::FuncType& payload)
{
	jit_nint payloadBytes = 0;
	if (!payload.params.empty())
		payloadBytes = static_cast<jit_nint>(
				jit_type_get_size(typeTranslator.translateStruct(payload.params)));
	return callNativeHelper(reinterpret_cast<void*>(wasm_exn_create), jit_type_void_ptr,
			{jit_type_void_ptr, jit_type_int, jit_type_uint},
			{jit_value_get_param(function, 0),
			 jit_value_create_nint_constant(function, jit_type_int, static_cast<jit_nint>(tagId)),
			 atomicUintConstant(static_cast<uint32_t>(payloadBytes))});
}

void OpcodeDispatcher::storeExceptionPayload(jit_value_t exnPayload, const WASM::FuncType& payload,
											 const std::vector<jit_value_t>& values)
{
	if (payload.params.empty())
		return;
	if (values.size() != payload.params.size())
		throw std::runtime_error("exception payload arity mismatch");
	jit_type_t tuple = typeTranslator.translateStruct(payload.params);
	for (size_t i = 0; i < values.size(); ++i) {
		const jit_nint offset = static_cast<jit_nint>(
				jit_type_get_offset(tuple, static_cast<unsigned int>(i)));
		if (WASM::isReferenceType(payload.params[i].val.opcode)) {
			// The exception object owns its payload references (same barrier as
			// struct.new). Exception objects are Store-owned and live until
			// teardown, so this is a bounded lifetime cost, documented in the
			// exceptions note; GC objects themselves are reclaimed normally.
			jit_value_t slot = jit_insn_add(function, exnPayload,
					jit_value_create_nint_constant(function, jit_type_nint, offset));
			emitRefStore(slot, values[i]);
			continue;
		}
		jit_insn_store_relative(function, exnPayload, offset, values[i]);
	}
}

std::vector<jit_value_t> OpcodeDispatcher::loadExceptionPayload(jit_value_t exnPayload,
																const WASM::FuncType& payload)
{
	std::vector<jit_value_t> values;
	if (payload.params.empty())
		return values;
	jit_type_t tuple = typeTranslator.translateStruct(payload.params);
	values.reserve(payload.params.size());
	for (size_t i = 0; i < payload.params.size(); ++i) {
		jit_type_t fieldType = jit_type_get_field(tuple, static_cast<unsigned int>(i));
		const jit_nint offset = static_cast<jit_nint>(
				jit_type_get_offset(tuple, static_cast<unsigned int>(i)));
		values.push_back(jit_insn_load_relative(function, exnPayload, offset, fieldType));
	}
	return values;
}

void OpcodeDispatcher::emitThrowToTarget(jit_value_t exn, std::vector<jit_value_t> values,
										 ControlBlock& target, bool carryExn)
{
	const auto& slots = branchSlotsForTarget(target);
	if (slots.size() != values.size() + (carryExn ? 1u : 0u))
		throw std::runtime_error("try_table catch target arity does not match the tag payload");
	// A caught exception is consumed: clear the pending slot before branching,
	// or the caller (and the embedder) would still observe it as uncaught.
	callNativeHelper(reinterpret_cast<void*>(wasm_exn_clear_pending), jit_type_void, {}, {});
	for (size_t i = 0; i < values.size(); ++i)
		jit_insn_store(function, slots[i], values[i]);
	if (carryExn)
		jit_insn_store(function, slots.back(),
				castRefValue(exn, jit_value_get_type(slots.back())));
	jit_insn_branch(function, &target.label);
}

jit_value_t OpcodeDispatcher::emitPendingException()
{
	// The slot is thread-local and resolved once per function (`exceptionSlot`
	// in the entry block); reading it inline costs one load.
	return jit_insn_load_relative(function, exceptionSlot, 0, jit_type_void_ptr);
}

void OpcodeDispatcher::emitReturnZeroes()
{
	// An exception leaves the function, but the generated signature still has
	// to return well-formed values. Zeroes are never observed by the caller:
	// it sees the pending slot first.
	const size_t n = currentFunc.results.size();
	for (size_t i = 0; i < n; ++i)
		pushOwnedValue(zeroConstantForType(jitTypeForValueType(currentFunc.results[i].val)));
	emitFunctionReturn(true);
}

void OpcodeDispatcher::emitFunctionEntry()
{
	// ── Guest call-stack guard ──────────────────────────────────────────────
	// Checked before the body runs. The prologue (and therefore the frame
	// allocation) has already happened at this point, which is why a function's
	// frame is bounded by `kMaxFunctionLocalBytes`. The helper reports
	// `CallStackExhausted`; the check below turns that into an early return, so
	// the whole chain unwinds by returning (WasmBase/WasmStack.hpp,
	// docs/SAFETY.md §2.8).
	jit_type_t params[1] = {jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 1, 1);
	jit_value_t args[1] = {jit_value_get_param(function, 0)};
	jit_insn_call_native(function, "wasm_stack_check",
		reinterpret_cast<void*>(&WASM::wasm_stack_check), signature, args, 1, 0);

	// The trap slot is thread-local; resolve it once and keep the address in a
	// function local. Every later check loads `pending` through it, so the hot
	// path is a load and a branch rather than a call.
	jit_type_t slotSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, nullptr, 0, 1);
	jit_value_t slot = jit_insn_call_native(function, "wasm_trap_slot",
			reinterpret_cast<void*>(&WASM::wasm_trap_slot), slotSig, nullptr, 0, 0);
	trapSlot = jit_value_create(function, jit_type_void_ptr);
	jit_insn_store(function, trapSlot, slot);

	// Same for the pending Wasm exception (a pointer slot the `throw` path
	// writes and every call site checks).
	jit_type_t exnSlotSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, nullptr, 0, 1);
	jit_value_t exnSlot = jit_insn_call_native(function, "wasm_exn_slot",
			reinterpret_cast<void*>(&wasm_exn_slot), exnSlotSig, nullptr, 0, 0);
	exceptionSlot = jit_value_create(function, jit_type_void_ptr);
	jit_insn_store(function, exceptionSlot, exnSlot);

	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitTrapEpilogue()
{
	if (!trapEpilogueUsed)
		return;

	// The epilogue is reached only by branches emitted from trap checks. It
	// returns the zero of each result type: the caller sees the pending slot
	// before it looks at any value, so a zeroed result is never observed.
	const bool savedUnreachable = unreachableCode;
	suppressTrapChecks = true;
	unreachableCode = false;
	jit_insn_label(function, &trapEpilogueLabel);
	emitReturnZeroes();
	suppressTrapChecks = false;
	unreachableCode = savedUnreachable;
}

void OpcodeDispatcher::emitExceptionPropagation(jit_value_t exn)
{
	// Walk the enclosing try_table frames innermost-first; each frame's
	// clauses are tried in source order. Every emitted path leaves the block.
	for (size_t frameIndex = controlStack.size(); frameIndex-- > 0; ) {
		ControlBlock& frame = controlStack[frameIndex];
		if (frame.kind != ControlBlock::TryTable)
			continue;
		for (const WASM::CatchClause& clause : frame.catchClauses) {
			if (clause.labelIdx >= frameIndex)
				throw std::runtime_error("try_table catch label out of range");
			ControlBlock& target = controlStack[frameIndex - 1 - clause.labelIdx];
			const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			if (!catchAll) {
				const WASM::TagId clauseTagId = tagIdForClause(clause);
				jit_value_t thrownTag = callNativeHelper(
						reinterpret_cast<void*>(wasm_exn_tag_id), jit_type_int,
						{jit_type_void_ptr}, {exn});
				jit_value_t wantTag = jit_value_create_nint_constant(function, jit_type_int,
						static_cast<jit_nint>(clauseTagId));
				jit_value_t matches = jit_insn_eq(function, thrownTag, wantTag);
				jit_label_t next = jit_label_undefined;
				jit_insn_branch_if_not(function, matches, &next);
				const WASM::FuncType& payload = tagPayloadType(clause.tagIdx);
				emitThrowToTarget(exn, loadExceptionPayload(exn, payload), target, carryExn);
				jit_insn_label(function, &next);
				continue;
			}
			// `catch_all` / `catch_all_ref` matches unconditionally and is last
			// in a valid module, so it ends the chain.
			emitThrowToTarget(exn, {}, target, carryExn);
			return;
		}
	}

	// No handler anywhere in this function: keep it pending for the caller.
	callNativeHelper(reinterpret_cast<void*>(wasm_exn_set_pending), jit_type_void,
			{jit_type_void_ptr}, {exn});
	emitReturnZeroes();
}

void OpcodeDispatcher::emitPendingExceptionCheck()
{
	// A trap wins over a pending Wasm exception, and must never be routed into a
	// `try_table` handler: a trap is not catchable by Wasm code.
	emitPendingTrapCheck();

	jit_value_t pending = emitPendingException();
	jit_value_t word = jit_insn_convert(function, pending, jit_type_ulong, 0);
	jit_value_t none = jit_insn_eq(function, word,
			jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t continuation = jit_label_undefined;
	jit_insn_branch_if(function, none, &continuation);
	emitExceptionPropagation(pending);
	jit_insn_label(function, &continuation);
}

WASM::TypeIdx OpcodeDispatcher::functionTypeIndexForFunc(WASM::FuncIdx funcIdx) const
{
	if (funcIdx < importedFuncCount)
		return module.importFunctions[funcIdx].typeIdx;

	const uint32_t internalIdx = funcIdx - importedFuncCount;
	assert(internalIdx < module.internalFunctionTypeIndices.size());
	return module.internalFunctionTypeIndices[internalIdx];
}

WASM::FuncType OpcodeDispatcher::functionSignatureForType(WASM::TypeIdx typeIdx) const
{
	assert(module.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func);
	return module.funcTypeAt(typeIdx);
}

WASM::TypeIdx OpcodeDispatcher::continuationFuncIndex(WASM::TypeIdx contTypeIdx) const
{
	if (contTypeIdx >= module.typeCount())
		throw std::runtime_error("continuation type index is out of range");
	const WASM::CoreTypeView view = module.typeView(contTypeIdx);
	if (view.kind() != WASM::CanonTypeKind::Cont)
		throw std::runtime_error("continuation instruction does not name a cont type");
	const WASM::CanonHeapType func = view.contFunc();
	if (func.kind != WASM::CanonHeapType::Kind::Global)
		throw std::runtime_error("continuation function type is not module-local");
	for (uint32_t i = 0; i < module.typeCount(); ++i) {
		if (module.typeIds[i] == func.id)
			return i;
	}
	throw std::runtime_error("continuation function type is not in this module");
}

WASM::FuncType OpcodeDispatcher::continuationSignature(WASM::TypeIdx contTypeIdx) const
{
	return module.funcTypeAt(continuationFuncIndex(contTypeIdx));
}

jit_value_t OpcodeDispatcher::callablePointerForFuncIndex(WASM::FuncIdx funcIdx)
{
	WASM::Callable* callable = nullptr;
	if (funcIdx < importedFuncCount) {
		assert(funcIdx < internals.importStorage.size());
		callable = &internals.importStorage[funcIdx];
	} else {
		const uint32_t internalIdx = funcIdx - importedFuncCount;
		assert(internalIdx < internals.internalCallables.size());
		callable = &internals.internalCallables[internalIdx];
	}
	return jit_value_create_nint_constant(
		function, jit_type_void_ptr, reinterpret_cast<jit_nint>(callable));
}

// Runtime fallback for callable type checks. Exact canonical-id equality is
// handled inline by the caller; this consults the registry for subtyping.
// Native imports with no canonical type (kNone) are accepted.
static int32_t wasm_callable_type_matches(uint32_t actual, uint32_t expected)
{
	if (actual == WASM::TypeId::kNone || actual == expected)
		return 1;
	return WASM::Store::current().types().matches(WASM::TypeId{actual}, WASM::TypeId{expected}) ? 1 : 0;
}

void OpcodeDispatcher::emitCallableTypeCheck(jit_value_t callablePtr, WASM::TypeIdx typeIdx)
{
	const WASM::TypeId want = module.typeId(WASM::LocalTypeIdx{typeIdx});
	jit_value_t gotType = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, typeId), jit_type_uint);
	jit_value_t wantType = jit_value_create_nint_constant(
		function, jit_type_uint, static_cast<jit_nint>(want.value));

	// Fast path: canonical-id equality.
	jit_label_t lbCont = jit_label_undefined;
	jit_value_t typeOk = jit_insn_eq(function, gotType, wantType);
	jit_insn_branch_if(function, typeOk, &lbCont);

	// Slow path: registry subtyping lookup.
	jit_type_t params[] = {jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 2, 1);
	jit_value_t args[] = {gotType, wantType};
	jit_value_t res = jit_insn_call_native(function, "wasm_callable_type_matches",
		reinterpret_cast<void*>(wasm_callable_type_matches), sig, args, 2, 0);
	jit_value_t zero = jit_value_create_nint_constant(function, jit_type_int, 0);
	jit_value_t ok = jit_insn_ne(function, res, zero);
	jit_insn_branch_if(function, ok, &lbCont);
	emitTrap(WASM::TrapKind::IndirectCallTypeMismatch);
	jit_insn_label(function, &lbCont);
}

void OpcodeDispatcher::dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx)
{
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	emitTrapIfNull(WASM::TrapKind::NullFunctionReference, callablePtr);

	emitCallableTypeCheck(callablePtr, typeIdx);

	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	jit_value_t outBuffer = newResultBuffer(calleeSig);
	std::vector<jit_value_t> args(1 + calleeSig.params.size() + (outBuffer != nullptr ? 1 : 0));
	args[0] = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, context), jit_type_void_ptr);
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	if (outBuffer != nullptr)
		args.back() = outBuffer;

	jit_value_t fnPtr = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	jit_value_t ret = jit_insn_call_indirect(
		function, fnPtr, calleeJitSig, args.data(), static_cast<unsigned>(args.size()), 0);
	emitPendingExceptionCheck();
	pushCallResults(calleeSig, ret, outBuffer);
}

void OpcodeDispatcher::emitTrap(WASM::TrapKind kind)
{
	emitTrap(kind, nullptr);
}

void OpcodeDispatcher::emitReportTrap(WASM::TrapKind kind, const char* detail)
{
	// Record the trap, then leave through the shared error epilogue. The path
	// continues (the report returns), so the branch — not a `noreturn` call — is
	// what keeps the generated signature well-formed.
	requestTrapEpilogue();
	emitTrapReportCall(function, kind, detail, functionIndex,
			static_cast<uint32_t>(instructionOffset), functionName);
	jit_insn_branch(function, &trapEpilogueLabel);
}

void OpcodeDispatcher::requestTrapEpilogue()
{
	trapEpilogueUsed = true;
}

void OpcodeDispatcher::emitPendingTrapCheck()
{
	// A helper or callee reported a trap: return the zero of each result type
	// instead of continuing. Skipped in the epilogue itself (a failure there must
	// not branch back into the epilogue) and in dead code.
	if (suppressTrapChecks || unreachableCode)
		return;

	jit_value_t pending = jit_insn_load_relative(function, trapSlot,
			static_cast<jit_nint>(offsetof(WASM::TrapSlot, pending)), jit_type_int);
	jit_value_t none = jit_insn_eq(function, pending,
			jit_value_create_nint_constant(function, jit_type_int, 0));
	jit_label_t continuation = jit_label_undefined;
	jit_insn_branch_if(function, none, &continuation);
	requestTrapEpilogue();
	jit_insn_branch(function, &trapEpilogueLabel);
	jit_insn_label(function, &continuation);
}

void OpcodeDispatcher::emitTrap(WASM::TrapKind kind, const char* detail)
{
	const char* reason = (detail != nullptr) ? detail : WASM::trapKindName(kind);
	// Compose the message once, at compile time, and intern it in the instance:
	// the generated code embeds the pointer and `reportTrap` does not copy it
	// (WasmTrap.hpp, docs/SAFETY.md). The numeric kind still drives `assert_trap`
	// comparisons; this only improves the log/embedder message
	// (docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	emitReportTrap(kind,
		internals.internTrapMessage(std::string(reason) + " in " + functionLabel));
}

void OpcodeDispatcher::notImplemented(const char* name)
{
	// The backend cannot lower `name` — in practice only a value type the engine
	// does not model reaches this now that SIMD and the atomics have handlers
	// (docs/UNMODELLED_VALUE_TYPES.md). It is a compile-time limitation rather
	// than a Wasm trap, so it is reported as a clean instantiation failure. The
	// LibJIT context's build region is guarded by RAII in compileFunctions(), so
	// unwinding from here cannot wedge it (docs/SAFETY.md).
	throw std::runtime_error(std::string("LibJIT backend: opcode is not lowered yet: ") + name);
}

void OpcodeDispatcher::emitTrapIfNull(WASM::TrapKind kind, jit_value_t pointer, const char* detail)
{
	jit_value_t word = jit_insn_convert(function, pointer, jit_type_ulong, 0);
	jit_value_t nonNull = jit_insn_ne(function, word,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, nonNull, &ok);
	emitTrap(kind, detail);
	jit_insn_label(function, &ok);
}

std::vector<WASM::StorageType> OpcodeDispatcher::storageTypesForBlockType(const WASM::BlockType& bt) const
{
	if (bt.isVoid())
		return {};
	if (!bt.isTypeIndex) {
		WASM::StorageType st;
		st.isPacked = false;
		// `heapType` is the immediate of a typed-reference block type
		// (`(block (result (ref $t)))`); -1 for the shorthand value types.
		st.val = WASM::ValueType{bt.valType, bt.heapType};
		return {st};
	}
	assert(static_cast<uint32_t>(bt.typeIndex) < module.typeCount());
	assert(module.typeView(bt.typeIndex).kind() == WASM::CanonTypeKind::Func);
	const WASM::FuncType ft = module.funcTypeAt(bt.typeIndex);
	return ft.results;
}

std::vector<WASM::StorageType> OpcodeDispatcher::parameterTypesForBlockType(const WASM::BlockType& bt) const
{
	if (!bt.isTypeIndex)
		return {};
	assert(static_cast<uint32_t>(bt.typeIndex) < module.typeCount());
	assert(module.typeView(bt.typeIndex).kind() == WASM::CanonTypeKind::Func);
	const WASM::FuncType ft = module.funcTypeAt(bt.typeIndex);
	return ft.params;
}

std::vector<jit_value_t> OpcodeDispatcher::createSlotsForTypes(const std::vector<WASM::StorageType>& types)
{
	std::vector<jit_value_t> slots;
	slots.reserve(types.size());
	for (const auto& type : types) {
		jit_type_t jt = jitTypeForValueType(type.val);
		jit_value_t slot = jit_value_create(function, jt);
		jit_insn_store(function, slot, zeroConstantForType(jt));
		slots.push_back(slot);
	}
	return slots;
}

void OpcodeDispatcher::storeStackTopToSlots(const std::vector<jit_value_t>& slots)
{
	// Dead code: the compile-time stack is polymorphic, so there is nothing
	// meaningful to store.
	if (unreachableCode || slots.empty())
		return;
	// Align the slot window with the top of the model stack. If the model stack
	// is short (drift in hard-to-model control flow, e.g. dead branches), the
	// missing values keep their previous (zero) contents instead of crashing.
	const size_t available = std::min(slots.size(), valueStack.size());
	const size_t base = valueStack.size() - available;
	const size_t slotBase = slots.size() - available;
	for (size_t i = 0; i < available; ++i)
		jit_insn_store(function, slots[slotBase + i], valueStack[base + i]);
}

void OpcodeDispatcher::restoreValuesFromSlots(const std::vector<jit_value_t>& slots)
{
	for (jit_value_t slot : slots)
		pushValue(jit_insn_load(function, slot));
}

void OpcodeDispatcher::resizeValueStack(size_t newSize)
{
	// Never grow the model stack: in unreachable code it may already be
	// shorter than the block's entry depth (pushes are ignored there).
	if (newSize < valueStack.size())
		valueStack.resize(newSize);
}

ControlBlock& OpcodeDispatcher::branchTarget(WASM::LabelIdx arg)
{
	assert(arg < controlStack.size());
	return controlStack[controlStack.size() - 1 - arg];
}

const std::vector<WASM::StorageType>& OpcodeDispatcher::branchTypesForTarget(const ControlBlock& target) const
{
	if (target.kind == ControlBlock::Loop)
		return target.paramTypes;
	return target.resultTypes;
}

const std::vector<jit_value_t>& OpcodeDispatcher::branchSlotsForTarget(const ControlBlock& target) const
{
	if (target.kind == ControlBlock::Loop)
		return target.paramSlots;
	return target.resultSlots;
}

void OpcodeDispatcher::emitBranchToTarget(ControlBlock& target)
{
	const auto& types = branchTypesForTarget(target);
	const auto& slots = branchSlotsForTarget(target);
	assert(types.size() == slots.size());
	storeStackTopToSlots(slots);
	jit_insn_branch(function, &target.label);
}

void OpcodeDispatcher::emitImplicitFunctionReturn()
{
	const size_t n = currentFunc.results.size();
	if (unreachableCode) {
		// The body ended unreachable; the reachable paths already transferred
		// control, so there is nothing to emit.
		return;
	}
	if (valueStack.size() < n) {
		// The body ended unreachable (e.g. it finished with 'return',
		// 'unreachable' or an unconditional branch): every reachable path
		// already transferred control, so there is nothing to emit here.
		// A stack that is *over*-full is still a codegen bug and asserted below.
		return;
	}
	assert(valueStack.size() == n);
	if (n == 0) {
		emitDropRefLocals();
		jit_insn_return(function, nullptr);
		return;
	}
	if (n == 1) {
		jit_value_t rv = popValue();
		// Deterministic profile: canonicalise an observable NaN before it leaves
		// the function (docs/proposals/PROFILES.md PR-3).
		if (!currentFunc.results.empty()) {
			const WASM::ValueTypeCode code = currentFunc.results[0].val.opcode;
			if (code == WASM::ValueTypeCode::F32)
				rv = canonicalizeFloat(rv, false);
			else if (code == WASM::ValueTypeCode::F64)
				rv = canonicalizeFloat(rv, true);
		}
		emitDropRefLocals();
		jit_insn_return(function, rv);
		return;
	}
	// Multiple results: the caller passed a result buffer as the trailing
	// parameter; write the values there and return void.
	assert(jit_type_get_return(jit_function_get_signature(function)) == jit_type_void);
	jit_value_t out = jit_value_get_param(function,
		static_cast<unsigned int>(currentFunc.params.size() + 1));
	packReturnValues(resultTupleType(currentFunc), out, n);
	// The result(s) are on the way out (owned by the caller); release the
	// frame's own references before returning (docs/proposals/GC.md RC-5).
	emitDropRefLocals();
	jit_insn_return(function, nullptr);
}

jit_value_t OpcodeDispatcher::vmContextValue()
{
	return jit_value_get_param(function, 0);
}

jit_value_t OpcodeDispatcher::checkedTableIndex(jit_value_t table, jit_value_t index, const char* opname,
		WASM::TrapKind kind)
{
	(void)opname;
	jit_value_t widened = jit_insn_convert(function, index, jit_type_ulong, 0);
	jit_value_t tableSize = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, size), jit_type_ulong);
	jit_value_t inBounds = jit_insn_lt(function, widened, tableSize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrap(kind);
	jit_insn_label(function, &cont);
	return widened;
}

jit_value_t OpcodeDispatcher::memoryPointerForIndex(WASM::MemIdx memidx)
{
	// The index is a compile-time constant, so an out-of-range one is a
	// malformed module: reject it here — at compile time, i.e. during
	// instantiation — instead of folding a wild pointer load into an access
	// (docs/proposals/MULTI_MEMORY.md MM-1).
	if (memidx >= internals.memoryRefs.size())
		throw std::runtime_error("memory index out of range for this module");

	// ctx.memories is an array of LinearMemory* owned by the Store. The memory
	// index is a compile-time constant, so it can be folded into the load's
	// byte offset (imports occupy the low indices, then the module's own).
	jit_value_t vm = vmContextValue();
	jit_value_t memories = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, memories), jit_type_void_ptr);
	return jit_insn_load_relative(
		function, memories,
		static_cast<jit_nint>(memidx) * static_cast<jit_nint>(sizeof(WASM::LinearMemory*)),
		jit_type_void_ptr);
}

jit_value_t OpcodeDispatcher::tablePointerForIndex(WASM::TableIdx tableidx)
{
	// Same compile-time rejection as for memories (MULTI_MEMORY.md MM-1).
	if (tableidx >= internals.tableRefs.size())
		throw std::runtime_error("table index out of range for this module");

	// ctx.tables is an array of TableInstance* owned by the Store. As with
	// memories, the (constant) table index folds into the load's byte offset.
	jit_value_t vm = vmContextValue();
	jit_value_t tables = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, tables), jit_type_void_ptr);

	// A module whose only table is an (as-yet unmodelled) imported table has a
	// null table index space. Trap instead of dereferencing null.
	jit_value_t tablesWord = jit_insn_convert(function, tables, jit_type_ulong, 0);
	jit_value_t present = jit_insn_ne(function, tablesWord,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, present, &ok);
	emitTrap(WASM::TrapKind::Internal, "table index space is null (unmodelled imported table)");
	jit_insn_label(function, &ok);

	return jit_insn_load_relative(
		function, tables,
		static_cast<jit_nint>(tableidx) * static_cast<jit_nint>(sizeof(WASM::TableInstance*)),
		jit_type_void_ptr);
}

WASM::MemoryIndexType OpcodeDispatcher::memoryIndexTypeForIndex(WASM::MemIdx memidx) const
{
	// Imported memories occupy the low indices of the memory index space,
	// exactly like ctx.memories (see initializeMemories / resolveImports).
	const size_t imported = module.importMemories.size();
	if (memidx < imported)
		return module.importMemories[memidx].memory.indexType;
	const size_t local = memidx - imported;
	if (local < module.memories.size())
		return module.memories[local].indexType;
	return WASM::MemoryIndexType::I32; // out of range: rejected by memoryPointerForIndex
}

bool OpcodeDispatcher::tableIs64Bit(WASM::TableIdx tableidx) const
{
	// A 64-bit table declares flag bit 2 on its limits, exactly like a
	// memory64 (docs/proposals/MEMORY64.md M4).
	const size_t imported = module.importTables.size();
	if (tableidx < imported)
		return (module.importTables[tableidx].table.limits.flags & 0x04) != 0;
	const size_t local = tableidx - imported;
	if (local < module.tables.size())
		return (module.tables[local].limits.flags & 0x04) != 0;
	return false; // out of range: rejected by tablePointerForIndex
}

jit_value_t OpcodeDispatcher::effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize)
{
	if (ma.isArray) {
		// Multibyte Array Access (docs/proposals/GC_EXTENSIONS.md GX-1):
		// [ref (ref null $t), address i32]. The address is the top operand; the
		// reference is below it (the caller already popped any stored value).
		jit_value_t address = popValue();
		jit_value_t ref = popValue();
		emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));

		const WASM::CoreTypeView view = arrayTypeForIndex(ma.arrayTypeIdx);
		const size_t elementSize = WASM::gcStorageBytes(view.arrayElement().storage);
		jit_value_t refVoid = refAsVoidPtr(ref);
		jit_value_t base = jit_insn_add(function, refVoid,
				jit_value_create_nint_constant(function, jit_type_nint,
						static_cast<jit_nint>(arrayDataOffset(ma.arrayTypeIdx))));

		// The address is an unsigned i32; widen through uint so a negative value
		// becomes a large offset rather than sign-extending.
		jit_value_t addrU = jit_insn_convert(function,
				jit_insn_convert(function, address, jit_type_uint, 0),
				jit_type_ulong, 0);
		jit_value_t byteOff = jit_insn_add(function, addrU,
				jit_value_create_long_constant(function, jit_type_ulong,
						static_cast<jit_long>(ma.offset)));

		// Bounds are over the array's byte payload: ea + width <= len * E.
		jit_value_t len = jit_insn_load_relative(function, refVoid,
				WASM::GcHeader::kArrayLengthOffset, jit_type_uint);
		jit_value_t bound = jit_insn_mul(function,
				jit_insn_convert(function, len, jit_type_ulong, 0),
				jit_value_create_long_constant(function, jit_type_ulong,
						static_cast<jit_long>(elementSize)));
		jit_value_t endOff = byteOff;
		if (accessSize > 0) {
			endOff = jit_insn_add(function, byteOff,
					jit_value_create_long_constant(function, jit_type_ulong,
							static_cast<jit_long>(accessSize)));
		}
		// The offset addition and the width addition must not wrap, and the last
		// accessed byte must still be inside the payload.
		jit_value_t addrNoWrap = jit_insn_ge(function, byteOff, addrU);
		jit_value_t widthNoWrap = jit_insn_ge(function, endOff, byteOff);
		jit_value_t inSize = jit_insn_le(function, endOff, bound);
		jit_value_t inBounds = jit_insn_and(function,
				jit_insn_and(function, addrNoWrap, widthNoWrap), inSize);
		jit_label_t cont = jit_label_undefined;
		jit_insn_branch_if(function, inBounds, &cont);
		emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
		jit_insn_label(function, &cont);
		return jit_insn_add(function, base, byteOff);
	}

	// Multi-memory: every access names its memory (defaulting to 0).
	jit_value_t lm = memoryPointerForIndex(ma.memidx);
	jit_value_t base = jit_insn_load_relative(function, lm, offsetof(WASM::LinearMemory, memoryBase), jit_type_void_ptr);
	jit_type_t memorySizeParams[] = {jit_type_void_ptr};
	jit_type_t memorySizeSig = jit_type_create_signature(jit_abi_cdecl,
		jit_type_ulong, memorySizeParams, 1, 1);
	jit_value_t memorySizeArgs[] = {lm};
	jit_value_t memorySize = jit_insn_call_native(function, "wasm_memory_size",
		reinterpret_cast<void*>(&WASM::wasm_memory_size), memorySizeSig,
		memorySizeArgs, 1, 0);
	// The helper reports a bad memory index; do not compute an address from the
	// value it returned.
	emitPendingTrapCheck();
	jit_value_t wasmOff = popValue();

	// The address operand is the memory's *unsigned* index. Widen it through
	// the unsigned type of the same width: converting a negative i32 straight
	// to unsigned 64 sign-extends, and `addr + offset + width` then wraps the
	// bounds check, letting a wild access through (address.wast's
	// `8u_good3(i32.const -1)` and memory_trap64's `-1` cases).
	jit_value_t indexValue = wasmOff;
	if (memoryIndexTypeForIndex(ma.memidx) != WASM::MemoryIndexType::I64)
		indexValue = jit_insn_convert(function, jit_insn_convert(function, wasmOff, jit_type_uint, 0), jit_type_ulong, 0);
	else
		indexValue = jit_insn_convert(function, wasmOff, jit_type_ulong, 0);

	jit_value_t off = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(ma.offset));
	jit_value_t byteOff = jit_insn_add(function, indexValue, off);
	jit_value_t endOff = byteOff;
	if (accessSize > 0) {
		jit_value_t width = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(accessSize));
		endOff = jit_insn_add(function, byteOff, width);
	}

	// Three conditions, all unsigned 64-bit comparisons: the offset addition
	// must not have wrapped, the width addition must not have wrapped, and the
	// last byte must be inside the memory.
	jit_value_t addrNoWrap = jit_insn_ge(function, byteOff, indexValue);
	jit_value_t widthNoWrap = jit_insn_ge(function, endOff, byteOff);
	jit_value_t inSize = jit_insn_le(function, endOff, memorySize);
	jit_value_t inBounds = jit_insn_and(function, jit_insn_and(function, addrNoWrap, widthNoWrap), inSize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
	jit_insn_label(function, &cont);
	return jit_insn_add(function, base, byteOff);
}

WASM::GlobalType OpcodeDispatcher::globalTypeForIndex(WASM::GlobalIdx idx) const
{
	const size_t nImp = module.importGlobals.size();
	if (idx < nImp)
		return module.importGlobals[idx].global;
	assert(idx - nImp < module.globals.size());
	return module.globals[idx - nImp].type;
}

OpcodeDispatcher::OpcodeDispatcher(jit_context_t context, jit_function_t function,
								   LibJitTypeTranslator& typeTranslator,
								   WASM::ModuleInstance& instance,
								   WASM::ModuleInstanceInternals& internals,
								   const WASM::Module& module, const WASM::FuncType& currentFunc,
								   uint32_t importedFuncCount, uint32_t functionIndex,
								   std::vector<WASM::BranchHint> branchHints,
								   std::vector<jit_value_t>& locals,
								   std::vector<jit_value_t>& valueStack,
								   std::vector<ControlBlock>& controlStack,
								   WASM::ProfileConfig profileConfig)
	: context(context), function(function), typeTranslator(typeTranslator), instance(instance),
	  internals(internals), module(module), currentFunc(currentFunc), importedFuncCount(importedFuncCount),
	  functionIndex(functionIndex),
	  locals(locals), valueStack(valueStack), controlStack(controlStack), branchHints(std::move(branchHints)),
	  deterministicFloats(profileConfig.deterministic())
{
	functionLabel = "function " + module.describeIndex(WASM::NameKind::Function, functionIndex);
	if (const std::string* name = module.findName(WASM::NameKind::Function, functionIndex))
		functionName = internals.internTrapMessage(*name);
}

jit_value_t OpcodeDispatcher::canonicalizeFloat(jit_value_t value, bool isDouble)
{
	if (!deterministicFloats)
		return value;
	jit_type_t type = isDouble ? jit_type_float64 : jit_type_float32;
	jit_type_t params[] = {type};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, type, params, 1, 1);
	jit_value_t args[] = {value};
	void* helper = isDouble
			? reinterpret_cast<void*>(&WASM::deterministicF64)
			: reinterpret_cast<void*>(&WASM::deterministicF32);
	return jit_insn_call_native(function, "wasm.deterministic_float", helper,
			signature, args, 1, 0);
}

const WASM::BranchHint* OpcodeDispatcher::currentBranchHint() const
{
	// Hints are ordered by offset and tiny (at most one entry per annotated
	// conditional branch), so a linear scan is fine and allocation-free.
	for (const WASM::BranchHint& hint : branchHints) {
		if (hint.offset == instructionOffset)
			return &hint;
		if (hint.offset > instructionOffset)
			break;
	}
	return nullptr;
}

jit_type_t OpcodeDispatcher::jitTypeForTypeIdx(WASM::TypeIdx idx) const
{
	assert(idx < internals.translatedTypes.size());
	return static_cast<jit_type_t>(internals.translatedTypes[idx]);
}

jit_type_t OpcodeDispatcher::jitTypeForValueType(const WASM::ValueType& vt)
{
	// A body-inline value type the engine does not model (e.g. a block type of
	// `exnref`) cannot be lowered. Fail the instantiation instead of emitting a
	// null jit_value_t later (docs/UNMODELLED_VALUE_TYPES.md).
	if (!WASM::isModeledValueTypeCode(vt.opcode))
		notImplemented("value type is not modelled (docs/UNMODELLED_VALUE_TYPES.md)");
	return typeTranslator.translateType(vt);
}

std::uint8_t OpcodeDispatcher::continuationKindFor(const WASM::ValueType& vt)
{
	switch (vt.opcode) {
	case WASM::ValueTypeCode::I32:
	case WASM::ValueTypeCode::I8:
	case WASM::ValueTypeCode::I16:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::I32);
	case WASM::ValueTypeCode::I64:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::I64);
	case WASM::ValueTypeCode::F32:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::F32);
	case WASM::ValueTypeCode::F64:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::F64);
	case WASM::ValueTypeCode::V128:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::V128);
	case WASM::ValueTypeCode::Func:
	case WASM::ValueTypeCode::FuncRef:
	case WASM::ValueTypeCode::ExternRef:
	case WASM::ValueTypeCode::AnyRef:
	case WASM::ValueTypeCode::EqRef:
	case WASM::ValueTypeCode::I31Ref:
	case WASM::ValueTypeCode::StructRef:
	case WASM::ValueTypeCode::ArrayRef:
	case WASM::ValueTypeCode::ExnRef:
	case WASM::ValueTypeCode::ContRef:
	case WASM::ValueTypeCode::NullFuncRef:
	case WASM::ValueTypeCode::NullExternRef:
	case WASM::ValueTypeCode::NullExnRef:
	case WASM::ValueTypeCode::NullRef:
	case WASM::ValueTypeCode::StringRef:
	case WASM::ValueTypeCode::StringViewWtf8:
	case WASM::ValueTypeCode::StringViewWtf16:
	case WASM::ValueTypeCode::StringViewIter:
	case WASM::ValueTypeCode::Ref:
	case WASM::ValueTypeCode::RefNull:
		return static_cast<std::uint8_t>(WASM::ContinuationValueKind::Ref);
	default:
		throw std::runtime_error("value type cannot cross a continuation");
		return 0;
	}
}

jit_value_t OpcodeDispatcher::allocateContinuationBuffer(std::size_t count)
{
	const std::size_t bytes = std::max<std::size_t>(count, 1)
			* WASM::kContinuationValueStride;
	return jit_insn_alloca(function, jit_value_create_nint_constant(function,
			jit_type_ulong, static_cast<jit_nint>(bytes)));
}

void OpcodeDispatcher::storeContinuationValues(const std::vector<jit_value_t>& values,
		const std::vector<WASM::StorageType>& types, jit_value_t buffer)
{
	assert(values.size() == types.size());
	for (std::size_t i = 0; i < values.size(); ++i) {
		jit_value_t value = values[i];
		const jit_type_t type = jitTypeForValueType(types[i].val);
		if (jit_value_get_type(value) != type)
			value = jit_insn_convert(function, value, type, 0);
		jit_insn_store_relative(function, buffer,
				static_cast<jit_nint>(i * WASM::kContinuationValueStride), value);
	}
}

void OpcodeDispatcher::loadContinuationValues(jit_value_t buffer,
		const std::vector<WASM::StorageType>& types)
{
	for (std::size_t i = 0; i < types.size(); ++i) {
		jit_value_t value = jit_insn_load_relative(function, buffer,
				static_cast<jit_nint>(i * WASM::kContinuationValueStride),
				jitTypeForValueType(types[i].val));
		pushOwnedValue(value);
	}
}

void OpcodeDispatcher::dropContinuationValues(jit_value_t buffer,
		const std::vector<WASM::StorageType>& types)
{
	for (std::size_t i = 0; i < types.size(); ++i) {
		const std::uint8_t kind = continuationKindFor(types[i].val);
		if (kind != static_cast<std::uint8_t>(WASM::ContinuationValueKind::Ref))
			continue;
		jit_value_t value = jit_insn_load_relative(function, buffer,
				static_cast<jit_nint>(i * WASM::kContinuationValueStride),
				jitTypeForValueType(types[i].val));
		emitDropValue(value);
	}
}

void OpcodeDispatcher::dispatchUnreachable()
{
	emitTrap(WASM::TrapKind::Unreachable);
	markUnreachable();
}

void OpcodeDispatcher::dispatchNop()
{
}

void OpcodeDispatcher::pushBlockFrame(ControlBlock::Kind kind, const WASM::BlockType& arg,
									  std::vector<WASM::CatchClause>&& catches)
{
	// A pathological body must not drive the operand/control bookkeeping (or
	// LibJIT) into the realm of gigabytes; see docs/SAFETY.md.
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock block;
	block.kind = kind;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
	block.paramTypes = paramTypes;
	block.resultTypes = storageTypesForBlockType(arg);
	block.resultSlots = createSlotsForTypes(block.resultTypes);
	block.catchClauses = std::move(catches);
	controlStack.push_back(std::move(block));
}

void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg)
{
	pushBlockFrame(ControlBlock::Block, arg, {});
}

void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock loop;
	loop.kind = ControlBlock::Loop;
	loop.label = jit_label_undefined;
	loop.elseLabel = jit_label_undefined;
	loop.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
	loop.paramTypes = paramTypes;
	loop.resultTypes = storageTypesForBlockType(arg);
	loop.paramSlots = createSlotsForTypes(loop.paramTypes);
	loop.resultSlots = createSlotsForTypes(loop.resultTypes);
	storeStackTopToSlots(loop.paramSlots);
	controlStack.push_back(std::move(loop));

	ControlBlock& current = controlStack.back();
	jit_insn_label(function, &current.label);
	resizeValueStack(current.stackDepth);
	// The loop header label is a merge point (the back-edge target), so the
	// body is reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(current.paramSlots);
}

void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	jit_value_t cond = popValue();
	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock block;
	block.kind = ControlBlock::If;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
	block.paramTypes = paramTypes;
	block.paramSlots = createSlotsForTypes(block.paramTypes);
	block.resultTypes = storageTypesForBlockType(arg);
	block.resultSlots = createSlotsForTypes(block.resultTypes);
	storeStackTopToSlots(block.paramSlots);
	controlStack.push_back(std::move(block));

	jit_insn_branch_if_not(function, cond, &controlStack.back().elseLabel);
}

void OpcodeDispatcher::dispatchElse()
{
	assert(!controlStack.empty());
	ControlBlock& block = controlStack.back();
	assert(block.kind == ControlBlock::If);
	assert(!block.hasElse);

	storeStackTopToSlots(block.resultSlots);
	jit_insn_branch(function, &block.label);

	jit_insn_label(function, &block.elseLabel);
	block.hasElse = true;
	resizeValueStack(block.stackDepth);
	// The else branch is a fresh merge point: reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(block.paramSlots);
}

void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	// Dead code: the tag's operand stack is polymorphic here, and the throw can
	// never execute.
	if (unreachableCode)
		return;
	if (arg >= internals.tagIds.size())
		throw std::runtime_error("throw names an out-of-range tag");

	const WASM::TagId tagId = internals.tagIds[arg];
	const WASM::FuncType& payload = tagPayloadType(arg);
	std::vector<jit_value_t> values(payload.params.size());
	for (size_t i = payload.params.size(); i-- > 0; )
		values[i] = popValue();

	// Materialise the exception first: a `catch_ref` clause needs it, and so
	// does propagation to the caller.
	jit_value_t exn = createException(tagId, payload);
	storeExceptionPayload(exn, payload, values);

	// Innermost enclosing try_table whose clause matches this (compile-time)
	// tag; clauses are tried in source order.
	for (size_t frameIndex = controlStack.size(); frameIndex-- > 0; ) {
		ControlBlock& frame = controlStack[frameIndex];
		if (frame.kind != ControlBlock::TryTable)
			continue;
		for (const WASM::CatchClause& clause : frame.catchClauses) {
			const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			if (!catchAll && tagIdForClause(clause) != tagId)
				continue;
			if (clause.labelIdx >= frameIndex)
				throw std::runtime_error("try_table catch label out of range");
			ControlBlock& target = controlStack[frameIndex - 1 - clause.labelIdx];
			const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			// `catch`/`catch_ref` carry the tag payload; `catch_all` carries none.
			emitThrowToTarget(exn, catchAll ? std::vector<jit_value_t>{} : values,
					target, carryExn);
			markUnreachable();
			return;
		}
	}

	// No handler in this function: hand the exception to the caller.
	callNativeHelper(reinterpret_cast<void*>(wasm_exn_set_pending), jit_type_void,
			{jit_type_void_ptr}, {exn});
	emitReturnZeroes();
	markUnreachable();
}

void OpcodeDispatcher::dispatchThrowRef()
{
	if (unreachableCode)
		return;
	jit_value_t exn = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(exn));
	// The tag id is dynamic, so the handler search runs at runtime over the
	// same lexical frames (EXD2); no match propagates to the caller.
	emitExceptionPropagation(exn);
	markUnreachable();
}

void OpcodeDispatcher::dispatchContNew(WASM::TypeIdx arg)
{
	if (unreachableCode)
		return;
	const WASM::FuncType func = continuationSignature(arg);
	jit_value_t callable = popValue();
	emitTrapIfNull(WASM::TrapKind::NullFunctionReference, refAsVoidPtr(callable));

	std::vector<std::uint8_t> paramKinds;
	paramKinds.reserve(func.params.size());
	for (const WASM::StorageType& type : func.params)
		paramKinds.push_back(continuationKindFor(type.val));
	std::vector<std::uint8_t> resultKinds;
	resultKinds.reserve(func.results.size());
	for (const WASM::StorageType& type : func.results)
		resultKinds.push_back(continuationKindFor(type.val));

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr,
			jit_type_uint, jit_type_uint, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, params, 7, 1);
	jit_value_t args[7] = {
		refAsVoidPtr(callable),
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_void_ptr,
				reinterpret_cast<jit_nint>(&invokeContinuation)),
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.params.size())),
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.results.size())),
		jit_value_create_nint_constant(function, jit_type_void_ptr,
				paramKinds.empty() ? 0 : reinterpret_cast<jit_nint>(
						const_cast<std::uint8_t*>(persistContinuationKinds(paramKinds)))),
		jit_value_create_nint_constant(function, jit_type_void_ptr,
				resultKinds.empty() ? 0 : reinterpret_cast<jit_nint>(
						const_cast<std::uint8_t*>(persistContinuationKinds(resultKinds)))),
	};
	jit_value_t continuation = jit_insn_call_native(function, "wasm_cont_new_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_new_ex), signature, args, 7, 0);
	emitPendingTrapCheck();

	WASM::HeapType heap;
	heap.isTypeIndex = true;
	heap.typeIndex = arg;
	pushOwnedValue(castRefValue(continuation, jitRefTypeForHeapType(heap, false)));
}

void OpcodeDispatcher::dispatchContBind(WASM::TypeIdx arg1, WASM::TypeIdx arg2)
{
	if (unreachableCode)
		return;
	const WASM::FuncType from = continuationSignature(arg1);
	const WASM::FuncType to = continuationSignature(arg2);
	if (from.params.size() < to.params.size())
		throw std::runtime_error("cont.bind: target continuation has more parameters");
	const size_t bound = from.params.size() - to.params.size();
	jit_value_t continuation = popValue();
	std::vector<jit_value_t> values(bound);
	for (size_t i = bound; i--; )
		values[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(continuation));

	std::vector<WASM::StorageType> boundTypes(from.params.begin(),
			from.params.begin() + static_cast<std::ptrdiff_t>(bound));
	jit_value_t buffer = allocateContinuationBuffer(bound);
	storeContinuationValues(values, boundTypes, buffer);

	jit_type_t params[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, params, 3, 1);
	jit_value_t args[3] = {
		refAsVoidPtr(continuation),
		buffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(bound)),
	};
	jit_value_t next = jit_insn_call_native(function, "wasm_cont_bind_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_bind_ex), signature, args, 3, 0);
	emitDropValue(continuation);
	emitPendingTrapCheck();

	WASM::HeapType heap;
	heap.isTypeIndex = true;
	heap.typeIndex = arg2;
	pushOwnedValue(castRefValue(next, jitRefTypeForHeapType(heap, false)));
}

void OpcodeDispatcher::dispatchSuspend(WASM::TagIdx arg)
{
	if (unreachableCode)
		return;
	const WASM::FuncType tag = tagPayloadType(arg);
	std::vector<jit_value_t> values(tag.params.size());
	for (size_t i = tag.params.size(); i--; )
		values[i] = popValue();
	std::vector<std::uint8_t> argKinds;
	argKinds.reserve(tag.params.size());
	for (const WASM::StorageType& type : tag.params)
		argKinds.push_back(continuationKindFor(type.val));
	std::vector<std::uint8_t> resultKinds;
	resultKinds.reserve(tag.results.size());
	for (const WASM::StorageType& type : tag.results)
		resultKinds.push_back(continuationKindFor(type.val));

	jit_value_t argBuffer = allocateContinuationBuffer(values.size());
	storeContinuationValues(values, tag.params, argBuffer);
	jit_value_t resultBuffer = allocateContinuationBuffer(tag.results.size());
	jit_value_t resultCount = jit_value_create(function, jit_type_uint);

	// Explicit suspended-stack roots for a tracing collector. Pointer-typed
	// locals, remaining operands and live control slots are registered before
	// the switch; the worker drops them when the continuation resumes.
	jit_type_t rootParams[1] = {jit_type_void_ptr};
	jit_type_t addRootParamSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, rootParams, 1, 1);
	const auto addRoot = [&](jit_value_t value) {
		if (value == nullptr || jit_value_get_type(value) != jit_type_void_ptr)
			return;
		jit_value_t arg[1] = {value};
		jit_insn_call_native(function, "wasm_cont_add_root",
				reinterpret_cast<void*>(&WASM::wasm_cont_add_root),
				addRootParamSig, arg, 1, 0);
	};
	for (jit_value_t value : locals)
		addRoot(value);
	for (jit_value_t value : valueStack)
		addRoot(value);
	for (const ControlBlock& block : controlStack) {
		for (jit_value_t value : block.resultSlots)
			addRoot(value);
		for (jit_value_t value : block.paramSlots)
			addRoot(value);
	}

	assert(arg < internals.tagIds.size());
	jit_type_t params[8] = {jit_type_int, jit_type_void_ptr, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_int, params, 8, 1);
	jit_value_t args[8] = {
		jit_value_create_nint_constant(function, jit_type_int,
				static_cast<jit_nint>(internals.tagIds[arg])),
		argBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(tag.params.size())),
		jit_value_create_nint_constant(function, jit_type_void_ptr,
				argKinds.empty() ? 0 : reinterpret_cast<jit_nint>(
						const_cast<std::uint8_t*>(persistContinuationKinds(argKinds)))),
		resultBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(tag.results.size())),
		jit_insn_address_of(function, resultCount),
		jit_value_create_nint_constant(function, jit_type_void_ptr,
				resultKinds.empty() ? 0 : reinterpret_cast<jit_nint>(
						const_cast<std::uint8_t*>(persistContinuationKinds(resultKinds)))),
	};
	jit_insn_call_native(function, "wasm_cont_suspend_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_suspend_ex), signature, args, 8, 0);
	emitPendingTrapCheck();
	// A resume_throw injects an exception at this suspension point; it must be
	// routed through the continuation's own try_table before the resumed value
	// is used.
	emitPendingExceptionCheck();
	if (unreachableCode)
		return;
	if (!tag.results.empty())
		loadContinuationValues(resultBuffer, tag.results);
}

void OpcodeDispatcher::dispatchSwitch(WASM::TypeIdx arg1, WASM::TagIdx arg2)
{
	if (unreachableCode)
		return;
	const WASM::FuncType target = continuationSignature(arg1);
	if (target.params.empty())
		throw std::runtime_error("switch: target continuation has no peer parameter");
	const WASM::StorageType& peerParam = target.params.back();
	if ((peerParam.val.opcode != WASM::ValueTypeCode::Ref
				&& peerParam.val.opcode != WASM::ValueTypeCode::RefNull)
			|| peerParam.val.heapType < 0)
		throw std::runtime_error("switch: target's last parameter is not a continuation");
	const WASM::TypeIdx currentType = static_cast<WASM::TypeIdx>(peerParam.val.heapType);
	const WASM::FuncType current = continuationSignature(currentType);
	const WASM::FuncType tag = tagPayloadType(arg2);
	if (!tag.params.empty())
		throw std::runtime_error("switch: control tag has parameters");
	(void)tag;

	jit_value_t targetContinuation = popValue();
	std::vector<jit_value_t> values(target.params.size() - 1);
	for (size_t i = values.size(); i--; )
		values[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(targetContinuation));

	jit_value_t argBuffer = allocateContinuationBuffer(values.size());
	std::vector<WASM::StorageType> targetParams(target.params.begin(),
			target.params.end() - 1);
	storeContinuationValues(values, targetParams, argBuffer);
	jit_value_t resultBuffer = allocateContinuationBuffer(current.params.size());

	jit_type_t fields[10] = {jit_type_int, jit_type_int, jit_type_int,
			jit_type_int, jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t resultType = jit_type_create_struct(fields, 10, 1);
	assert(jit_type_get_size(resultType) == sizeof(WASM::ContinuationResumeResult));
	jit_value_t result = jit_value_create(function, resultType);
	jit_value_t resultAddr = jit_insn_address_of(function, result);

	// Register suspended-stack roots while the peer runs.
	jit_type_t rootParams[1] = {jit_type_void_ptr};
	jit_type_t addRootSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, rootParams, 1, 1);
	const auto addRoot = [&](jit_value_t value) {
		if (value == nullptr || jit_value_get_type(value) != jit_type_void_ptr)
			return;
		jit_value_t arg[1] = {value};
		jit_insn_call_native(function, "wasm_cont_add_root",
				reinterpret_cast<void*>(&WASM::wasm_cont_add_root),
				addRootSig, arg, 1, 0);
	};
	for (jit_value_t value : locals)
		addRoot(value);
	for (jit_value_t value : valueStack)
		addRoot(value);
	for (const ControlBlock& block : controlStack) {
		for (jit_value_t value : block.resultSlots)
			addRoot(value);
		for (jit_value_t value : block.paramSlots)
			addRoot(value);
	}

	jit_type_t params[6] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			jit_type_void_ptr, jit_type_uint, jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, params, 6, 1);
	jit_value_t args[6] = {
		refAsVoidPtr(targetContinuation),
		argBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(values.size())),
		resultBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(current.params.size())),
		resultAddr,
	};
	jit_insn_call_native(function, "wasm_cont_switch",
			reinterpret_cast<void*>(&WASM::wasm_cont_switch),
			signature, args, 6, 0);
	emitDropValue(targetContinuation);
	emitPendingExceptionCheck();
	emitPendingTrapCheck();
	if (unreachableCode)
		return;

	jit_label_t completedLabel = jit_label_undefined;
	jit_label_t doneLabel = jit_label_undefined;
	jit_value_t status = jit_insn_load_relative(function, resultAddr,
			static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, status)),
			jit_type_int);
	jit_insn_branch_if(function,
			jit_insn_eq(function, status, jit_value_create_nint_constant(function,
					jit_type_int, static_cast<jit_nint>(WASM::ContinuationStatus::Completed))),
			&completedLabel);
	emitTrap(WASM::TrapKind::Internal, "continuation switch failed");
	jit_insn_label(function, &completedLabel);
	loadContinuationValues(resultBuffer, current.params);
	jit_insn_label(function, &doneLabel);
}

void OpcodeDispatcher::emitContinuationResultHandling(const WASM::FuncType& func,
		const std::vector<WASM::ContHandlerClause>& handlers,
		jit_value_t resultAddr, jit_value_t resultBuffer,
		jit_value_t tagBuffer, size_t baseDepth)
{
	jit_label_t completedLabel = jit_label_undefined;
	jit_label_t suspendedLabel = jit_label_undefined;
	jit_label_t doneLabel = jit_label_undefined;
	jit_value_t status = jit_insn_load_relative(function, resultAddr,
			static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, status)),
			jit_type_int);
	const auto dropSuspendedContinuation = [&]() {
		jit_value_t next = jit_insn_load_relative(function, resultAddr,
				static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, continuation)),
				jit_type_void_ptr);
		emitDropValue(castRefValue(next, jit_type_void_ptr));
	};
	const auto dropTagValues = [&]() {
		jit_type_t dropParams[4] = {jit_type_void_ptr, jit_type_void_ptr,
				jit_type_uint, jit_type_void_ptr};
		jit_type_t dropSignature = jit_type_create_signature(jit_abi_cdecl,
				jit_type_void, dropParams, 4, 1);
		jit_value_t dropArgs[4] = {
			vmContextValue(),
			jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagKinds)),
					jit_type_void_ptr),
			jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagValueCount)),
					jit_type_uint),
			tagBuffer,
		};
		jit_insn_call_native(function, "wasm_cont_drop_values",
				reinterpret_cast<void*>(&WASM::wasm_cont_drop_values),
				dropSignature, dropArgs, 4, 0);
	};
	jit_insn_branch_if(function,
			jit_insn_eq(function, status, jit_value_create_nint_constant(function,
					jit_type_int, static_cast<jit_nint>(WASM::ContinuationStatus::Completed))),
			&completedLabel);
	jit_insn_branch_if(function,
			jit_insn_eq(function, status, jit_value_create_nint_constant(function,
					jit_type_int, static_cast<jit_nint>(WASM::ContinuationStatus::Suspended))),
			&suspendedLabel);
	emitTrap(WASM::TrapKind::Internal, "continuation resume failed");

	jit_insn_label(function, &completedLabel);
	loadContinuationValues(resultBuffer, func.results);
	jit_insn_branch(function, &doneLabel);

	jit_insn_label(function, &suspendedLabel);
	if (handlers.empty()) {
		dropTagValues();
		dropSuspendedContinuation();
		emitTrap(WASM::TrapKind::Internal, "unhandled continuation suspension");
	} else {
		jit_value_t tag = jit_insn_load_relative(function, resultAddr,
				static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagId)),
				jit_type_int);
		std::vector<jit_label_t> handlerLabels(handlers.size(), jit_label_undefined);
		for (size_t i = 0; i < handlers.size(); ++i) {
			const WASM::ContHandlerClause& handler = handlers[i];
			if (handler.isSwitch)
				continue;
			assert(handler.tag < internals.tagIds.size());
			jit_insn_branch_if(function,
					jit_insn_eq(function, tag, jit_value_create_nint_constant(function,
							jit_type_int, static_cast<jit_nint>(internals.tagIds[handler.tag]))),
					&handlerLabels[i]);
		}
		dropTagValues();
		dropSuspendedContinuation();
		emitTrap(WASM::TrapKind::Internal, "unhandled continuation suspension");

		for (size_t i = 0; i < handlers.size(); ++i) {
			const WASM::ContHandlerClause& handler = handlers[i];
			if (handler.isSwitch)
				continue;
			jit_insn_label(function, &handlerLabels[i]);
			const WASM::FuncType tagType = tagPayloadType(handler.tag);
			loadContinuationValues(tagBuffer, tagType.params);
			if (handler.label >= controlStack.size())
				throw std::runtime_error("resume: handler label is out of range");
			ControlBlock& target = controlStack[controlStack.size() - 1 - handler.label];
			jit_value_t next = jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, continuation)),
					jit_type_void_ptr);
			const std::vector<WASM::StorageType>& handlerTypes =
					target.kind == ControlBlock::Loop ? target.paramTypes : target.resultTypes;
			const jit_type_t contType = handlerTypes.empty()
					? jit_type_void_ptr
					: jitTypeForValueType(handlerTypes.back().val);
			pushOwnedValue(castRefValue(next, contType));
			emitBranchToTarget(target);
			valueStack.resize(baseDepth);
		}
	}
	jit_insn_label(function, &doneLabel);
}

void OpcodeDispatcher::dispatchResume(WASM::TypeIdx arg,
		std::vector<WASM::ContHandlerClause>&& handlers)
{
	if (unreachableCode)
		return;
	const WASM::FuncType func = continuationSignature(arg);

	jit_value_t continuation = popValue();
	std::vector<jit_value_t> stackArgs(func.params.size());
	for (size_t i = func.params.size(); i--; )
		stackArgs[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(continuation));

	const size_t baseDepth = valueStack.size();
	jit_value_t argBuffer = allocateContinuationBuffer(stackArgs.size());
	storeContinuationValues(stackArgs, func.params, argBuffer);
	jit_value_t resultBuffer = allocateContinuationBuffer(func.results.size());
	size_t tagCapacity = 0;
	for (const WASM::ContHandlerClause& handler : handlers)
		tagCapacity = std::max(tagCapacity, tagPayloadType(handler.tag).params.size());
	jit_value_t tagBuffer = allocateContinuationBuffer(tagCapacity);

	jit_type_t fields[10] = {jit_type_int, jit_type_int, jit_type_int,
			jit_type_int, jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t resultType = jit_type_create_struct(fields, 10, 1);
	assert(jit_type_get_size(resultType) == sizeof(WASM::ContinuationResumeResult));
	jit_value_t result = jit_value_create(function, resultType);
	jit_value_t resultAddr = jit_insn_address_of(function, result);

	jit_type_t params[8] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint,
			jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, params, 8, 1);
	jit_value_t args[8] = {
		refAsVoidPtr(continuation),
		argBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.params.size())),
		resultBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.results.size())),
		tagBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(tagCapacity)),
		resultAddr,
	};
	jit_insn_call_native(function, "wasm_cont_resume_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_resume_ex), signature, args, 8, 0);

	emitDropValue(continuation);
	emitPendingExceptionCheck();
	if (unreachableCode)
		return;

	jit_label_t completedLabel = jit_label_undefined;
	jit_label_t suspendedLabel = jit_label_undefined;
	jit_label_t doneLabel = jit_label_undefined;
	jit_value_t status = jit_insn_load_relative(function, resultAddr,
			static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, status)),
			jit_type_int);
	const auto dropSuspendedContinuation = [&]() {
		jit_value_t next = jit_insn_load_relative(function, resultAddr,
				static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, continuation)),
				jit_type_void_ptr);
		emitDropValue(castRefValue(next, jit_type_void_ptr));
	};
	const auto dropTagValues = [&]() {
		jit_type_t dropParams[4] = {jit_type_void_ptr, jit_type_void_ptr,
				jit_type_uint, jit_type_void_ptr};
		jit_type_t dropSignature = jit_type_create_signature(jit_abi_cdecl,
				jit_type_void, dropParams, 4, 1);
		jit_value_t dropArgs[4] = {
			vmContextValue(),
			jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagKinds)),
					jit_type_void_ptr),
			jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagValueCount)),
					jit_type_uint),
			tagBuffer,
		};
		jit_insn_call_native(function, "wasm_cont_drop_values",
				reinterpret_cast<void*>(&WASM::wasm_cont_drop_values),
				dropSignature, dropArgs, 4, 0);
	};
	jit_insn_branch_if(function,
			jit_insn_eq(function, status, jit_value_create_nint_constant(function,
					jit_type_int, static_cast<jit_nint>(WASM::ContinuationStatus::Completed))),
			&completedLabel);
	jit_insn_branch_if(function,
			jit_insn_eq(function, status, jit_value_create_nint_constant(function,
					jit_type_int, static_cast<jit_nint>(WASM::ContinuationStatus::Suspended))),
			&suspendedLabel);
	emitTrap(WASM::TrapKind::Internal, "continuation resume failed");

	jit_insn_label(function, &completedLabel);
	loadContinuationValues(resultBuffer, func.results);
	jit_insn_branch(function, &doneLabel);

	jit_insn_label(function, &suspendedLabel);
	if (handlers.empty()) {
		dropTagValues();
		dropSuspendedContinuation();
		emitTrap(WASM::TrapKind::Internal, "unhandled continuation suspension");
	} else {
		jit_value_t tag = jit_insn_load_relative(function, resultAddr,
				static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, tagId)),
				jit_type_int);
		std::vector<jit_label_t> handlerLabels(handlers.size(), jit_label_undefined);
		for (size_t i = 0; i < handlers.size(); ++i) {
			const WASM::ContHandlerClause& handler = handlers[i];
			if (handler.isSwitch)
				continue;
			assert(handler.tag < internals.tagIds.size());
			jit_insn_branch_if(function,
					jit_insn_eq(function, tag, jit_value_create_nint_constant(function,
							jit_type_int, static_cast<jit_nint>(internals.tagIds[handler.tag]))),
					&handlerLabels[i]);
		}
		dropTagValues();
		dropSuspendedContinuation();
		emitTrap(WASM::TrapKind::Internal, "unhandled continuation suspension");

		for (size_t i = 0; i < handlers.size(); ++i) {
			const WASM::ContHandlerClause& handler = handlers[i];
			if (handler.isSwitch)
				continue;
			jit_insn_label(function, &handlerLabels[i]);
			const WASM::FuncType tagType = tagPayloadType(handler.tag);
			loadContinuationValues(tagBuffer, tagType.params);
			if (handler.label >= controlStack.size())
				throw std::runtime_error("resume: handler label is out of range");
			ControlBlock& target = controlStack[controlStack.size() - 1 - handler.label];
			jit_value_t next = jit_insn_load_relative(function, resultAddr,
					static_cast<jit_nint>(offsetof(WASM::ContinuationResumeResult, continuation)),
					jit_type_void_ptr);
			const std::vector<WASM::StorageType>& handlerTypes =
					target.kind == ControlBlock::Loop ? target.paramTypes : target.resultTypes;
			const jit_type_t contType = handlerTypes.empty()
					? jit_type_void_ptr
					: jitTypeForValueType(handlerTypes.back().val);
			pushOwnedValue(castRefValue(next, contType));
			emitBranchToTarget(target);
			valueStack.resize(baseDepth);
		}
	}

	jit_insn_label(function, &doneLabel);
}


void OpcodeDispatcher::dispatchResumeThrow(WASM::TypeIdx arg1, WASM::TagIdx arg2,
		std::vector<WASM::ContHandlerClause>&& handlers)
{
	if (unreachableCode)
		return;
	const WASM::FuncType func = continuationSignature(arg1);
	const WASM::FuncType exnType = tagPayloadType(arg2);
	jit_value_t continuation = popValue();
	std::vector<jit_value_t> exnValues(exnType.params.size());
	for (size_t i = exnType.params.size(); i--; )
		exnValues[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(continuation));

	const size_t baseDepth = valueStack.size();
	jit_value_t resultBuffer = allocateContinuationBuffer(func.results.size());
	size_t tagCapacity = 0;
	for (const WASM::ContHandlerClause& handler : handlers)
		tagCapacity = std::max(tagCapacity, tagPayloadType(handler.tag).params.size());
	jit_value_t tagBuffer = allocateContinuationBuffer(tagCapacity);

	assert(arg2 < internals.tagIds.size());
	jit_value_t exn = createException(internals.tagIds[arg2], exnType);
	storeExceptionPayload(exn, exnType, exnValues);

	jit_type_t fields[10] = {jit_type_int, jit_type_int, jit_type_int,
			jit_type_int, jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t resultType = jit_type_create_struct(fields, 10, 1);
	jit_value_t result = jit_value_create(function, resultType);
	jit_value_t resultAddr = jit_insn_address_of(function, result);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_void_ptr,
			jit_type_uint, jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, params, 7, 1);
	jit_value_t args[7] = {
		refAsVoidPtr(continuation),
		refAsVoidPtr(exn),
		resultBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.results.size())),
		tagBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(tagCapacity)),
		resultAddr,
	};
	jit_insn_call_native(function, "wasm_cont_resume_throw_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_resume_throw_ex),
			signature, args, 7, 0);
	emitDropValue(continuation);
	emitPendingExceptionCheck();
	if (unreachableCode)
		return;
	emitContinuationResultHandling(func, handlers, resultAddr, resultBuffer,
			tagBuffer, baseDepth);
}

void OpcodeDispatcher::dispatchResumeThrowRef(WASM::TypeIdx arg,
		std::vector<WASM::ContHandlerClause>&& handlers)
{
	if (unreachableCode)
		return;
	const WASM::FuncType func = continuationSignature(arg);
	jit_value_t continuation = popValue();
	jit_value_t exn = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(continuation));
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(exn));

	const size_t baseDepth = valueStack.size();
	jit_value_t resultBuffer = allocateContinuationBuffer(func.results.size());
	size_t tagCapacity = 0;
	for (const WASM::ContHandlerClause& handler : handlers)
		tagCapacity = std::max(tagCapacity, tagPayloadType(handler.tag).params.size());
	jit_value_t tagBuffer = allocateContinuationBuffer(tagCapacity);

	jit_type_t fields[10] = {jit_type_int, jit_type_int, jit_type_int,
			jit_type_int, jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t resultType = jit_type_create_struct(fields, 10, 1);
	jit_value_t result = jit_value_create(function, resultType);
	jit_value_t resultAddr = jit_insn_address_of(function, result);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_void_ptr,
			jit_type_uint, jit_type_void_ptr};
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void, params, 7, 1);
	jit_value_t args[7] = {
		refAsVoidPtr(continuation),
		refAsVoidPtr(exn),
		resultBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(func.results.size())),
		tagBuffer,
		jit_value_create_nint_constant(function, jit_type_uint,
				static_cast<jit_nint>(tagCapacity)),
		resultAddr,
	};
	jit_insn_call_native(function, "wasm_cont_resume_throw_ex",
			reinterpret_cast<void*>(&WASM::wasm_cont_resume_throw_ex),
			signature, args, 7, 0);
	emitDropValue(continuation);
	emitPendingExceptionCheck();
	if (unreachableCode)
		return;
	emitContinuationResultHandling(func, handlers, resultAddr, resultBuffer,
			tagBuffer, baseDepth);
}

void OpcodeDispatcher::dispatchEnd()
{
	if (controlStack.empty())
		return;

	assert(!controlStack.empty());
	ControlBlock block = std::move(controlStack.back());
	controlStack.pop_back();

	if (block.kind == ControlBlock::If && !block.hasElse) {
		// No explicit else. If the then path is reachable it stores its
		// results and jumps over the implicit else; the implicit else yields
		// the block's parameters (validation requires params == results for an
		// `if` with no else). Binding `elseLabel` *before* the result store
		// used to make the else path execute the then path's store, and when
		// the then body ended in `br`/`return` the store was skipped entirely
		// (the `unreachableCode` early-out), leaving the result slots at zero.
		const bool thenReachable = !unreachableCode;
		if (thenReachable) {
			storeStackTopToSlots(block.resultSlots);
			jit_insn_branch(function, &block.label);
		}
		jit_insn_label(function, &block.elseLabel);
		unreachableCode = false;
		const size_t copies = std::min(block.resultSlots.size(), block.paramSlots.size());
		for (size_t i = 0; i < copies; ++i)
			jit_insn_store(function, block.resultSlots[i],
				jit_insn_load(function, block.paramSlots[i]));
		jit_insn_label(function, &block.label);
		resizeValueStack(block.stackDepth);
		restoreValuesFromSlots(block.resultSlots);
		return;
	}

	if (block.kind != ControlBlock::Loop) {
		storeStackTopToSlots(block.resultSlots);
		jit_insn_label(function, &block.label);
	} else {
		storeStackTopToSlots(block.resultSlots);
	}

	resizeValueStack(block.stackDepth);
	// The block's end label is a merge point: code after it is reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(block.resultSlots);
}

void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	emitBranchToLabel(arg);
	markUnreachable();
}

void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	if (arg >= controlStack.size()) {
		// Conditional return: a branch to the function's implicit block.
		jit_value_t cond = popValue();
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, cond, &skip);
		emitFunctionReturn(false);
		jit_insn_label(function, &skip);
		return;
	}

	ControlBlock& target = branchTarget(arg);
	const auto& types = branchTypesForTarget(target);
	const auto& slots = branchSlotsForTarget(target);
	assert(types.size() == slots.size());
	if (!unreachableCode)
		assert(valueStack.size() >= types.size() + 1);

	jit_value_t cond = popValue();
	storeStackTopToSlots(slots);

	// Branch hinting (proposal branch-hinting) may steer the *layout* of the
	// conditional branch. It is a pure optimisation hint and never changes the
	// result; the transformation below is semantics-preserving by construction.
	//
	// LibJIT offers no branch-prediction API, so the only lever available to a
	// single-pass emitter is the polarity of the conditional jump, which decides
	// which direction is the fall-through. For a *forward* target the
	// conditional jump is predicted not-taken, so "condition likely false" is
	// already the best layout. When the hint says a forward branch is likely
	// *true*, invert it: jump away on the unlikely `!cond` and fall through to
	// an unconditional branch, so the likely path never executes a taken
	// conditional jump. (A backward target -- a loop back-edge -- is predicted
	// taken, so it already matches a "likely true" hint and is left alone.)
	const WASM::BranchHint* hint = currentBranchHint();
	const bool backward = (target.kind == ControlBlock::Loop);
	if (hint != nullptr && hint->hint == 1 && !backward) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, cond, &skip);
		jit_insn_branch(function, &target.label);
		jit_insn_label(function, &skip);
	} else {
		jit_insn_branch_if(function, cond, &target.label);
	}
}

void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2)
{
	jit_value_t index = popValue();
	jit_label_t defaultLabel = jit_label_undefined;
	jit_label_t doneLabel = jit_label_undefined;
	// Emitting a branch to the function's implicit label pops the branch values
	// from the compile-time stack (`emitFunctionReturn`). A br_table emits one
	// branch per target, so each case must see the same entry stack.
	const std::vector<jit_value_t> entryStack = valueStack;

	for (size_t i = 0; i < arg1.size(); ++i) {
		jit_value_t caseValue = jit_value_create_nint_constant(
			function, jit_type_int, static_cast<jit_nint>(i));
		jit_value_t match = jit_insn_eq(function, index, caseValue);
		jit_label_t nextLabel = jit_label_undefined;
		jit_insn_branch_if_not(function, match, &nextLabel);
		valueStack = entryStack;
		emitBranchToLabel(arg1[i]);
		jit_insn_label(function, &nextLabel);
	}

	jit_insn_branch(function, &defaultLabel);
	jit_insn_label(function, &defaultLabel);
	valueStack = entryStack;
	emitBranchToLabel(arg2);
	jit_insn_label(function, &doneLabel);
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturn()
{
	if (unreachableCode)
		return; // dead code: nothing to emit
	emitFunctionReturn();
	markUnreachable();
}

void OpcodeDispatcher::emitFunctionReturn(bool consume)
{
	if (unreachableCode)
		return;
	const size_t n = currentFunc.results.size();
	if (valueStack.size() < n) {
		// Dead tail: the reachable paths already transferred control.
		return;
	}
	// `return` only requires the function's result types to be on top; any
	// extra operands below them are discarded by the return. (Unlike the
	// function's implicit end, which must leave exactly the results.)
	if (n == 0) {
		emitDropRefLocals();
		jit_insn_return(function, nullptr);
		return;
	}

	if (consume) {
		if (n == 1) {
			jit_value_t rv = popValue();
			// Deterministic profile: canonicalise an observable NaN before it leaves
			// the function (docs/proposals/PROFILES.md PR-3).
			if (!currentFunc.results.empty()) {
				const WASM::ValueTypeCode code = currentFunc.results[0].val.opcode;
				if (code == WASM::ValueTypeCode::F32)
					rv = canonicalizeFloat(rv, false);
				else if (code == WASM::ValueTypeCode::F64)
					rv = canonicalizeFloat(rv, true);
			}
			emitDropRefLocals();
			jit_insn_return(function, rv);
			return;
		}
		// Multiple results travel through the trailing result-buffer
		// parameter (LibJitTypeTranslation.cpp); the signature returns void.
		assert(jit_type_get_return(jit_function_get_signature(function)) == jit_type_void);
		jit_value_t out = jit_value_get_param(function,
			static_cast<unsigned int>(currentFunc.params.size() + 1));
		packReturnValues(resultTupleType(currentFunc), out, n);
		emitDropRefLocals();
		jit_insn_return(function, nullptr);
		return;
	}

	// Non-consuming: emit the return from a snapshot, leaving the operands on
	// the model stack for the fallthrough path.
	const size_t base = valueStack.size() - n;
	if (n == 1) {
		jit_value_t rv = valueStack[base];
		// Deterministic profile: canonicalise an observable NaN before it leaves
		// the function (docs/proposals/PROFILES.md PR-3).
		if (!currentFunc.results.empty()) {
			const WASM::ValueTypeCode code = currentFunc.results[0].val.opcode;
			if (code == WASM::ValueTypeCode::F32)
				rv = canonicalizeFloat(rv, false);
			else if (code == WASM::ValueTypeCode::F64)
				rv = canonicalizeFloat(rv, true);
		}
		emitDropRefLocals();
		jit_insn_return(function, rv);
		return;
	}
	const size_t depth = valueStack.size();
	const std::vector<jit_value_t> saved(valueStack.begin() + base, valueStack.end());
	for (jit_value_t v : saved)
		valueStack.push_back(v);
	jit_value_t out = jit_value_get_param(function,
		static_cast<unsigned int>(currentFunc.params.size() + 1));
	packReturnValues(resultTupleType(currentFunc), out, n);
	valueStack.resize(depth);
	emitDropRefLocals();
	jit_insn_return(function, nullptr);
}

void OpcodeDispatcher::emitBranchToLabel(WASM::LabelIdx arg)
{
	if (arg >= controlStack.size()) {
		// A branch to the function's implicit block is a return.
		emitFunctionReturn();
		return;
	}
	emitBranchToTarget(branchTarget(arg));
}

void OpcodeDispatcher::dispatchCall(WASM::FuncIdx funcIdx)
{
	WASM::TypeIdx typeIdx = functionTypeIndexForFunc(funcIdx);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	jit_value_t outBuffer = newResultBuffer(calleeSig);
	const unsigned numArgs = 1 + static_cast<unsigned>(calleeSig.params.size())
			+ (outBuffer != nullptr ? 1u : 0u);
	std::vector<jit_value_t> args(numArgs);
	if (outBuffer != nullptr)
		args.back() = outBuffer;

	if (funcIdx < importedFuncCount) {
		const WASM::VMContext* calleeCtx = internals.importStorage[funcIdx].context;
		jit_value_t impBase = jit_insn_load_relative(
			function, vmContextValue(), offsetof(WASM::VMContext, importedFunctions), jit_type_void_ptr);
		jit_value_t off = jit_value_create_nint_constant(
			function, jit_type_nint, static_cast<jit_nint>(funcIdx * sizeof(WASM::Callable)));
		jit_value_t callablePtr = jit_insn_add(function, impBase, off);
	emitTrapIfNull(WASM::TrapKind::Internal, callablePtr, "unresolved imported function");
		jit_value_t fnPtr = jit_insn_load_relative(
			function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
		// If the import has a non-null context (imported from another Wasm
		// module instance), burn that address into the machine code — no
		// trampoline needed.  If null (native/host import), pass the caller's
		// VMContext so the native function can access the calling module's
		// memory, globals, table, and hostData.
		// NOTE: burning calleeCtx into JIT constants is brittle — the callee
		// instance must not move.  We rely on unique_ptr ownership of module
		// instances to guarantee this.
		if (calleeCtx != nullptr) {
			args[0] = jit_value_create_nint_constant(function, jit_type_void_ptr,
				reinterpret_cast<jit_nint>(calleeCtx));
		} else {
			args[0] = vmContextValue();
		}
		for (size_t i = 0; i < calleeSig.params.size(); ++i)
			args[1 + i] = stackArgs[i];
		jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(), numArgs, 0);
		emitPendingExceptionCheck();
		pushCallResults(calleeSig, ret, outBuffer);
		return;
	}

	// Route through the callee's Callable `fnPtr`, which is its trampoline
	// driver (see docs/TAILCALLS.md TC-2). This keeps a tail-call chain that
	// passes through this non-tail call enclosed in a driver, so the chain
	// cannot grow the host stack.
	const uint32_t internalIdx = funcIdx - importedFuncCount;
	WASM::Callable* callable = &internals.internalCallables[internalIdx];
	jit_value_t callableAddr = jit_value_create_nint_constant(
		function, jit_type_void_ptr, reinterpret_cast<jit_nint>(callable));
	jit_value_t calleeCtx = jit_insn_load_relative(
		function, callableAddr, offsetof(WASM::Callable, context), jit_type_void_ptr);
	jit_value_t fnPtr = jit_insn_load_relative(
		function, callableAddr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	args[0] = calleeCtx;
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(), numArgs, 0);
	emitPendingExceptionCheck();
	pushCallResults(calleeSig, ret, outBuffer);
}

void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx typeIdx, WASM::TableIdx tableIdx)
{
	assert(module.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func);
	const WASM::FuncType ft = module.funcTypeAt(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	jit_value_t tableElemIdx = popValue();
	std::vector<jit_value_t> stackArgs(ft.params.size());
	for (size_t i = ft.params.size(); i--; )
		stackArgs[i] = popValue();

	jit_value_t table = tablePointerForIndex(tableIdx);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(table, tableElemIdx, "dispatchCallIndirect");
	jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitTrapIfNull(WASM::TrapKind::UninitializedElement, callablePtr);

	emitCallableTypeCheck(callablePtr, typeIdx);

	jit_value_t fnPtr = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	jit_value_t calleeCtx = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, context), jit_type_void_ptr);

	jit_value_t outBuffer = newResultBuffer(ft);
	std::vector<jit_value_t> args(1 + ft.params.size() + (outBuffer != nullptr ? 1 : 0));
	args[0] = calleeCtx;
	for (size_t i = 0; i < ft.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	if (outBuffer != nullptr)
		args.back() = outBuffer;

	jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(),
											 static_cast<unsigned>(args.size()), 0);
	emitPendingExceptionCheck();
	pushCallResults(ft, ret, outBuffer);
}

bool OpcodeDispatcher::emitDirectTailCall(WASM::FuncIdx funcIdx)
{
	// Native tail calls are only reliable in this LibJIT for a direct call to
	// the function itself. Mutual direct calls and indirect tail calls through
	// a runtime target miscompile (ABI corruption / hang), so they keep the
	// call+return lowering. See docs/TAILCALLS.md §6.
	if (funcIdx < importedFuncCount)
		return false; // imports go through the fnPtr/Callable path
	const uint32_t internalIdx = funcIdx - importedFuncCount;
	if (internalIdx >= internals.compiledFunctions.size())
		return false;
	jit_function_t callee = static_cast<jit_function_t>(internals.compiledFunctions[internalIdx]);
	if (callee != function)
		return false; // only self-recursion is validated

	const WASM::TypeIdx typeIdx = functionTypeIndexForFunc(funcIdx);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	// Pop the call operands (last argument on top), exactly like dispatchCall.
	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	const unsigned numArgs = 1 + static_cast<unsigned>(calleeSig.params.size())
			+ (calleeSig.results.size() > 1 ? 1u : 0u);
	std::vector<jit_value_t> args(numArgs);
	args[0] = vmContextValue();
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	if (calleeSig.results.size() > 1) {
		// Direct self-recursion: the caller's result buffer is the callee's.
		args.back() = jit_value_get_param(function,
			static_cast<unsigned int>(currentFunc.params.size() + 1));
	}

	// The callee's result becomes this function's result; control leaves the
	// frame entirely, so no `return` is emitted and the rest is unreachable.
	jit_insn_call(function, "wasm.tail", callee, calleeJitSig, args.data(), numArgs, JIT_CALL_TAIL);
	markUnreachable();
	return true;
}

bool OpcodeDispatcher::signatureIsUniform(const WASM::FuncType& calleeSig) const
{
	const auto sameStorage = [](const std::vector<WASM::StorageType>& a,
								const std::vector<WASM::StorageType>& b) {
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i].isPacked != b[i].isPacked ||
				a[i].val.opcode != b[i].val.opcode ||
				a[i].val.heapType != b[i].val.heapType)
				return false;
		}
		return true;
	};
	return sameStorage(calleeSig.params, currentFunc.params) &&
		   sameStorage(calleeSig.results, currentFunc.results);
}

void OpcodeDispatcher::emitDummyReturn()
{
	jit_type_t rt = jit_type_get_return(jit_function_get_signature(function));
	if (rt == jit_type_void)
		jit_insn_return(function, nullptr);
	else
		jit_insn_return(function, zeroConstantForType(rt));
}

void OpcodeDispatcher::emitTailCallViaPending(jit_value_t targetCallablePtr,
											  const std::vector<jit_value_t>& args)
{
	jit_value_t state = callNativeHelper(
		reinterpret_cast<void*>(&WASM::currentTailCallState), jit_type_void_ptr, {}, {});

	jit_insn_store_relative(function, state,
		static_cast<jit_nint>(offsetof(WASM::TailCallState, target)), targetCallablePtr);
	for (size_t i = 0; i < args.size(); ++i) {
		jit_insn_store_relative(function, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, args) + i * sizeof(uint64_t)),
			args[i]);
	}
	jit_insn_store_relative(function, state,
		static_cast<jit_nint>(offsetof(WASM::TailCallState, pending)),
		jit_value_create_nint_constant(function, jit_type_int, 1));

	// The driver ignores this value; it exists only to satisfy the ABI.
	emitDummyReturn();
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	if (emitDirectTailCall(arg))
		return; // direct self-recursion: native JIT_CALL_TAIL (C2)

	// Trampoline: a direct call to another function with an identical
	// signature. Uniformity lets the running driver re-dispatch with its own
	// parameter types (covers e.g. mutual even/odd recursion).
	const WASM::TypeIdx typeIdx = functionTypeIndexForFunc(arg);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	if (signatureIsUniform(calleeSig) && calleeSig.params.size() <= WASM::kTailCallArgSlots) {
		jit_value_t target = nullptr;
		if (arg < importedFuncCount) {
			// Imported wasm functions carry the exporter's raw entry point; a
			// native import has none and must use call+return.
			if (internals.importStorage[arg].rawFnPtr != nullptr)
				target = jit_value_create_nint_constant(function, jit_type_void_ptr,
					reinterpret_cast<jit_nint>(&internals.importStorage[arg]));
		} else {
			const uint32_t internalIdx = arg - importedFuncCount;
			if (internalIdx < internals.internalCallables.size())
				target = jit_value_create_nint_constant(function, jit_type_void_ptr,
					reinterpret_cast<jit_nint>(&internals.internalCallables[internalIdx]));
		}
		if (target != nullptr) {
			std::vector<jit_value_t> stackArgs(calleeSig.params.size());
			for (size_t i = calleeSig.params.size(); i--; )
				stackArgs[i] = popValue();
			emitTailCallViaPending(target, stackArgs);
			return;
		}
	}

	dispatchCall(arg);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	// Trampoline when the callee's declared type matches this function's own
	// signature (see docs/TAILCALLS.md TC-2).
	const WASM::FuncType ft = module.funcTypeAt(arg1);
	if (signatureIsUniform(ft) && ft.params.size() <= WASM::kTailCallArgSlots) {
		jit_value_t tableElemIdx = popValue();
		std::vector<jit_value_t> stackArgs(ft.params.size());
		for (size_t i = ft.params.size(); i--; )
			stackArgs[i] = popValue();

		jit_value_t table = tablePointerForIndex(arg2);
		jit_value_t tablePtr = jit_insn_load_relative(
			function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
		jit_value_t checkedIndex = checkedTableIndex(table, tableElemIdx, "dispatchReturnCallIndirect");
		jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitTrapIfNull(WASM::TrapKind::UninitializedElement, callablePtr);
		emitCallableTypeCheck(callablePtr, arg1);
		emitTailCallViaPending(callablePtr, stackArgs);
		return;
	}
	dispatchCallIndirect(arg1, arg2);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg)
{
	jit_value_t callablePtr = popValue();
	dispatchCallThroughCallable(callablePtr, arg);
}

void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg)
{
	// Trampoline when the referenced type matches this function's own signature
	// (see docs/TAILCALLS.md TC-2). This is what makes `return_call_ref`-based
	// self-recursion (e.g. return_call_ref.wast's `count`) constant-stack.
	const WASM::FuncType ft = module.funcTypeAt(arg);
	if (signatureIsUniform(ft) && ft.params.size() <= WASM::kTailCallArgSlots) {
		jit_value_t callablePtr = popValue();
		std::vector<jit_value_t> stackArgs(ft.params.size());
		for (size_t i = ft.params.size(); i--; )
			stackArgs[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullFunctionReference, callablePtr);
		emitCallableTypeCheck(callablePtr, arg);
		emitTailCallViaPending(callablePtr, stackArgs);
		return;
	}
	dispatchCallRef(arg);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2)
{
	// The catch clauses are not a runtime construct: every throw site in the
	// dynamic extent dispatches to them statically (EXD2). The instruction
	// itself therefore behaves exactly like a block with a label at its end.
	pushBlockFrame(ControlBlock::TryTable, arg1, std::move(arg2));
}

void OpcodeDispatcher::dispatchDrop()
{
	// Discarding a reference releases the operand stack's ownership of it.
	emitDropValue(popValue());
}

void OpcodeDispatcher::dispatchSelect()
{
	jit_value_t cond = popValue();
	jit_value_t v2 = popValue();
	jit_value_t v1 = popValue();

	// Both arms must contribute exactly one value to the *compile-time* stack
	// model, so merge them through a slot. (Pushing one value per branch here
	// left the modelled stack one entry too high.)
	jit_value_t result = jit_value_create(function, jit_value_get_type(v1));

	jit_label_t lbTrue = jit_label_undefined;
	jit_label_t lbMerge = jit_label_undefined;
	jit_insn_branch_if(function, cond, &lbTrue);
	jit_insn_store(function, result, v2);
	jit_insn_branch(function, &lbMerge);
	jit_insn_label(function, &lbTrue);
	jit_insn_store(function, result, v1);
	jit_insn_label(function, &lbMerge);

	pushValue(result);
}

void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& arg)
{
	(void)arg;
	dispatchSelect();
}

void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	pushValue(jit_insn_load(function, locals[arg]));
}

void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	jit_value_t v = popValue();
	const bool refLocal = isRefJitValue(locals[arg]);
	// Parameters hold a *borrow* (the caller's value is not owned by us); only
	// non-parameter locals own a reference (docs/proposals/GC.md RC-5).
	if (refLocal && arg >= currentFunc.params.size()) {
		// Retain the new value, release the old, then store (so a
		// self-assignment is safe).
		jit_value_t old = jit_insn_load(function, locals[arg]);
		jit_value_t kept = emitRefClone(v);
		jit_insn_store(function, locals[arg], castRefValue(kept, jit_value_get_type(locals[arg])));
		emitRefDrop(old);
	} else {
		jit_insn_store(function, locals[arg], v);
	}
	if (refLocal)
		emitDropValue(v); // release the operand stack's ownership
}

void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	jit_value_t v = popValue();
	const bool refLocal = isRefJitValue(locals[arg]);
	if (refLocal && arg >= currentFunc.params.size()) {
		jit_value_t old = jit_insn_load(function, locals[arg]);
		jit_value_t kept = emitRefClone(v);
		jit_insn_store(function, locals[arg], castRefValue(kept, jit_value_get_type(locals[arg])));
		emitRefDrop(old);
	} else {
		jit_insn_store(function, locals[arg], v);
	}
	if (refLocal)
		emitDropValue(v);
	pushValue(jit_insn_load(function, locals[arg]));
}
jit_value_t OpcodeDispatcher::globalSlotAddress(WASM::GlobalIdx arg)
{
	jit_value_t vm = vmContextValue();
	// ctx.globals is a Value** (one cell per global index). Load the cell
	// pointer, then return it as the slot address: imported globals point at
	// the exporter's storage, defined ones at the instance's own storage.
	jit_value_t cells = jit_insn_load_relative(function, vm,
			offsetof(WASM::VMContext, globals), jit_type_void_ptr);
	jit_value_t off = jit_value_create_nint_constant(
		function, jit_type_nint, static_cast<jit_nint>(arg * sizeof(WASM::Value*)));
	jit_value_t slot = jit_insn_add(function, cells, off);
	return jit_insn_load_relative(function, slot, 0, jit_type_void_ptr);
}

void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg)
{
	WASM::GlobalType gt = globalTypeForIndex(arg);
	if (gt.contentType.opcode == WASM::ValueTypeCode::V128) {
		// A `v128` global is 16 bytes at the start of its `Value`, so reading it
		// is `v128.load` from there — the same helper and the same verified
		// shape, rather than a struct load this LibJIT build has never been
		// asked for (SIMD-L5).
		jit_value_t out = newV128Slot();
		const jit_type_t types[1] = {jit_type_void_ptr};
		jit_value_t args[1] = {globalSlotAddress(arg)};
		callSimdOutHelper(reinterpret_cast<void*>(wasm_v128_load), out, types, args);
		pushValue(out);
		return;
	}
	jit_type_t jt = jitTypeForValueType(gt.contentType);
	jit_value_t slot = globalSlotAddress(arg);
	pushValue(jit_insn_load_relative(function, slot, 0, jt));
}

void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg)
{
	WASM::GlobalType gt = globalTypeForIndex(arg);
	if (gt.contentType.opcode == WASM::ValueTypeCode::V128) {
		// Symmetric: `v128.store` into the global's 16 bytes. `global.set` of a
		// `v128` is a plain 16-byte copy with no GC barrier (SIMD-L5).
		jit_value_t v = popValue();
		if (unreachableCode)
			return;
		const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
		jit_value_t args[2] = {globalSlotAddress(arg), jit_insn_address_of(function, v)};
		callSimdHelper(reinterpret_cast<void*>(wasm_v128_store), jit_type_void, types, args);
		return;
	}
	jit_value_t v = popValue();
	// An exported global is host-observable, so canonicalise a deterministic NaN.
	if (gt.contentType.opcode == WASM::ValueTypeCode::F32)
		v = canonicalizeFloat(v, false);
	else if (gt.contentType.opcode == WASM::ValueTypeCode::F64)
		v = canonicalizeFloat(v, true);
	jit_value_t slot = globalSlotAddress(arg);
	if (WASM::isReferenceType(gt.contentType.opcode)) {
		// The `ref` member is at offset 0 of the Value union, so `slot` is
		// already the slot address; go through the barrier (GC.md D15), then
		// release the operand stack's ownership.
		emitRefStore(slot, v);
		emitDropValue(v);
		return;
	}
	jit_insn_store_relative(function, slot, 0, v);
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg) {
	jit_value_t index = popValue();
	jit_value_t table = tablePointerForIndex(arg);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_type_t elementType = tableElementJitType(arg);
	jit_value_t checkedIndex = checkedTableIndex(table, index, "dispatchTableGet",
			WASM::TrapKind::OutOfBoundsTableAccess);
	jit_value_t loaded = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	pushValue(castRefValue(loaded, elementType));
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg) {
	jit_value_t value = popValue();
	jit_value_t index = popValue();
	jit_value_t table = tablePointerForIndex(arg);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(table, index, "dispatchTableSet",
			WASM::TrapKind::OutOfBoundsTableAccess);
	// Table slots are reference-typed; go through the write barrier
	// (docs/proposals/GC.md D15).
	jit_value_t slot = jit_insn_load_elem_address(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitRefStore(slot, value);
	emitDropValue(value);
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_int));
}

void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_long));
}

void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float32));
}

void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float64));
}

void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_int);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_uint);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr)
{
	// Memory is an observability point: canonicalise a NaN in deterministic mode.
	jit_value_t val = canonicalizeFloat(popValue(), false);
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr)
{
	jit_value_t val = canonicalizeFloat(popValue(), true);
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_uint, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg)
{
	jit_value_t lm = memoryPointerForIndex(arg);
	jit_type_t params[] = {jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 1, 1);
	jit_value_t args[] = {lm};
	jit_value_t ms = jit_insn_call_native(function, "wasm_memory_size",
		reinterpret_cast<void*>(&WASM::wasm_memory_size), sig, args, 1, 0);
	// The page size is a per-memory field: CUSTOM_PAGE_SIZES.md allows a
	// one-byte page, so dividing by a constant 65536 would be wrong.
	jit_value_t page = jit_insn_load_relative(function, lm,
		offsetof(WASM::LinearMemory, pageSize), jit_type_ulong);
	jit_value_t pages = jit_insn_div(function, ms, page);
	// The result has the memory's index type: i64 for memory64 (M64-1).
	if (memoryIndexTypeForIndex(arg) == WASM::MemoryIndexType::I64)
		pushValue(jit_insn_convert(function, pages, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, pages, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	const bool is64 = memoryIndexTypeForIndex(arg) == WASM::MemoryIndexType::I64;
	jit_value_t delta = popValue();
	// The delta operand has the memory's index type; interpret it as unsigned
	// so a negative i32 becomes a huge (failing) request rather than wrapping.
	delta = jit_insn_convert(function, delta, is64 ? jit_type_ulong : jit_type_uint, 0);
	if (!is64)
		delta = jit_insn_convert(function, delta, jit_type_ulong, 0);

	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_ulong};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 3, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		delta};
	jit_value_t result = jit_insn_call_native(function, "wasm_memory_grow_impl",
											   reinterpret_cast<void*>(wasm_memory_grow_impl), sig, args, 3, 0);
	if (is64)
		pushValue(jit_insn_convert(function, result, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Const(int32_t arg)
{
	pushValue(jit_value_create_nint_constant(function, jit_type_int, static_cast<jit_nint>(arg)));
}

void OpcodeDispatcher::dispatchI64Const(int64_t arg)
{
	pushValue(jit_value_create_long_constant(function, jit_type_long, static_cast<jit_long>(arg)));
}

void OpcodeDispatcher::dispatchF32Const(float arg)
{
	// LibJIT's float-constant API routes the value through an integer and loses
	// the sign of -0.0. Keep each constant in a process-lifetime pool instead
	// and load it: `std::deque` references stay valid across push_back.
	static std::deque<float> pool;
	static std::mutex poolMutex;
	float* slot = nullptr;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		pool.push_back(arg);
		slot = &pool.back();
	}
	jit_value_t address = jit_value_create_nint_constant(
			function, jit_type_void_ptr, reinterpret_cast<jit_nint>(slot));
	pushValue(jit_insn_load_relative(function, address, 0, jit_type_float32));
}

void OpcodeDispatcher::dispatchF64Const(double arg)
{
	// Same -0.0 limitation as dispatchF32Const.
	static std::deque<double> pool;
	static std::mutex poolMutex;
	double* slot = nullptr;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		pool.push_back(arg);
		slot = &pool.back();
	}
	jit_value_t address = jit_value_create_nint_constant(
			function, jit_type_void_ptr, reinterpret_cast<jit_nint>(slot));
	pushValue(jit_insn_load_relative(function, address, 0, jit_type_float64));
}
void OpcodeDispatcher::dispatchI32Eqz()
{
	jit_value_t z = jit_value_create_nint_constant(function, jit_type_int, 0);
	pushValue(jit_insn_eq(function, popValue(), z));
}

void OpcodeDispatcher::dispatchI32Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchI32Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchI32LtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI32LtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI32GtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI32GtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI32LeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI32LeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI32GeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchI32GeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchI64Eqz()
{
	jit_value_t z = jit_value_create_long_constant(function, jit_type_long, 0);
	pushValue(jit_insn_eq(function, popValue(), z));
}

void OpcodeDispatcher::dispatchI64Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchI64Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchI64LtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI64LtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI64GtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI64GtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI64LeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI64LeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI64GeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchI64GeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchF32Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchF32Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchF32Lt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchF32Gt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchF32Le()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchF32Ge()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchF64Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchF64Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchF64Lt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchF64Gt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchF64Le()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchF64Ge()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchI32Clz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_clz), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Ctz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_ctz), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Popcnt()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_popcnt), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchI32Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchI32Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchI32DivS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_div_s),
		jit_type_int, jit_type_int, jit_type_int, a, b));
}

void OpcodeDispatcher::dispatchI32DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_div_u),
		jit_type_uint, jit_type_uint, jit_type_uint, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_rem_s),
		jit_type_int, jit_type_int, jit_type_int, a, b));
}

void OpcodeDispatcher::dispatchI32RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_rem_u),
		jit_type_uint, jit_type_uint, jit_type_uint, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32And()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_and(function, a, b));
}

void OpcodeDispatcher::dispatchI32Or()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_or(function, a, b));
}

void OpcodeDispatcher::dispatchI32Xor()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_xor(function, a, b));
}

void OpcodeDispatcher::dispatchI32Shl()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shl(function, a, b));
}

void OpcodeDispatcher::dispatchI32ShrS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shr(function, a, b));
}

void OpcodeDispatcher::dispatchI32ShrU()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ushr(function, a, b));
}

void OpcodeDispatcher::dispatchI32Rotl()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i32Rotl(function, n, r));
}

void OpcodeDispatcher::dispatchI32Rotr()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i32Rotr(function, n, r));
}
void OpcodeDispatcher::dispatchI64Clz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_clz), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Ctz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_ctz), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Popcnt()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_popcnt), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchI64Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchI64Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchI64DivS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_div_s),
		jit_type_long, jit_type_long, jit_type_long, a, b));
}

void OpcodeDispatcher::dispatchI64DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_div_u),
		jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_rem_s),
		jit_type_long, jit_type_long, jit_type_long, a, b));
}

void OpcodeDispatcher::dispatchI64RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_rem_u),
		jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64And()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_and(function, a, b));
}

void OpcodeDispatcher::dispatchI64Or()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_or(function, a, b));
}

void OpcodeDispatcher::dispatchI64Xor()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_xor(function, a, b));
}

void OpcodeDispatcher::dispatchI64Shl()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shl(function, a, b));
}

void OpcodeDispatcher::dispatchI64ShrS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shr(function, a, b));
}

void OpcodeDispatcher::dispatchI64ShrU()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ushr(function, a, b));
}

void OpcodeDispatcher::dispatchI64Rotl()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i64Rotl(function, n, r));
}

void OpcodeDispatcher::dispatchI64Rotr()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i64Rotr(function, n, r));
}
void OpcodeDispatcher::dispatchF32Abs()
{
	pushValue(jit_insn_abs(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Neg()
{
	pushValue(jit_insn_neg(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Ceil()
{
	pushValue(jit_insn_ceil(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Floor()
{
	pushValue(jit_insn_floor(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Trunc()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f32_trunc_impl),
			jit_type_float32, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchF32Nearest()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f32_nearest_impl),
			jit_type_float32, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchF32Sqrt()
{
	pushValue(jit_insn_sqrt(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchF32Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchF32Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchF32Div()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchF32Min()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_f32_min),
			jit_type_float32, jit_type_float32, jit_type_float32, a, b));
}

void OpcodeDispatcher::dispatchF32Max()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_f32_max),
			jit_type_float32, jit_type_float32, jit_type_float32, a, b));
}

void OpcodeDispatcher::dispatchF32Copysign()
{
	jit_value_t y = popValue();
	jit_value_t x = popValue();
	jit_type_t params[] = {jit_type_float32, jit_type_float32};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_float32, params, 2, 1);
	jit_value_t args[] = {x, y};
	pushValue(jit_insn_call_native(function, "wasm_copysign_f32",
								   reinterpret_cast<void*>(reinterpret_cast<float (*)(float, float)>(wasm_copysign_f32)),
								   sig, args, 2, 0));
}
void OpcodeDispatcher::dispatchF64Abs()
{
	pushValue(jit_insn_abs(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Neg()
{
	pushValue(jit_insn_neg(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Ceil()
{
	pushValue(jit_insn_ceil(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Floor()
{
	pushValue(jit_insn_floor(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Trunc()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f64_trunc_impl),
			jit_type_float64, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchF64Nearest()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f64_nearest_impl),
			jit_type_float64, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchF64Sqrt()
{
	pushValue(jit_insn_sqrt(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchF64Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchF64Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchF64Div()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchF64Min()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_f64_min),
			jit_type_float64, jit_type_float64, jit_type_float64, a, b));
}

void OpcodeDispatcher::dispatchF64Max()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_f64_max),
			jit_type_float64, jit_type_float64, jit_type_float64, a, b));
}

void OpcodeDispatcher::dispatchF64Copysign()
{
	jit_value_t y = popValue();
	jit_value_t x = popValue();
	jit_type_t params[] = {jit_type_float64, jit_type_float64};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_float64, params, 2, 1);
	jit_value_t args[] = {x, y};
	pushValue(jit_insn_call_native(function, "wasm_copysign_f64",
								   reinterpret_cast<void*>(reinterpret_cast<double (*)(double, double)>(wasm_copysign_f64)),
								   sig, args, 2, 0));
}
void OpcodeDispatcher::dispatchI32WrapI64()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF32S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f32_s),
		jit_type_int, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchI32TruncF32U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f32_u),
		jit_type_uint, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF64S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f64_s),
		jit_type_int, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchI32TruncF64U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f64_u),
		jit_type_uint, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64ExtendI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64ExtendI32U()
{
	// Reinterpret the 32-bit value as unsigned first; converting a signed
	// `int` directly to `ulong` would sign-extend.
	jit_value_t v = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, v, jit_type_ulong, 0));
}

void OpcodeDispatcher::dispatchI64TruncF32S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f32_s),
		jit_type_long, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchI64TruncF32U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f32_u),
		jit_type_ulong, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64TruncF64S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f64_s),
		jit_type_long, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchI64TruncF64U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f64_u),
		jit_type_ulong, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI32U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI64U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f32_convert_i64_u_impl),
			jit_type_float32, jit_type_ulong, u));
}

void OpcodeDispatcher::dispatchF32DemoteF64()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI32U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI64U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f64_convert_i64_u_impl),
			jit_type_float64, jit_type_ulong, u));
}

void OpcodeDispatcher::dispatchF64PromoteF32()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchI32ReinterpretF32()
{
	// `reinterpret` exposes the payload: canonicalise a NaN in deterministic mode.
	jit_value_t value = canonicalizeFloat(popValue(), false);
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_reinterpret_f32),
							  jit_type_int, jit_type_float32, value));
}

void OpcodeDispatcher::dispatchI64ReinterpretF64()
{
	jit_value_t value = canonicalizeFloat(popValue(), true);
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_reinterpret_f64),
							  jit_type_long, jit_type_float64, value));
}

void OpcodeDispatcher::dispatchF32ReinterpretI32()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f32_reinterpret_i32),
							  jit_type_float32, jit_type_int, popValue()));
}

void OpcodeDispatcher::dispatchF64ReinterpretI64()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f64_reinterpret_i64),
							  jit_type_float64, jit_type_long, popValue()));
}

void OpcodeDispatcher::dispatchI32Extend8S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_sbyte, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Extend16S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_short, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64Extend8S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_sbyte, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Extend16S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_short, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Extend32S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_int, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType& arg) {
	pushValue(typedNullRef(jitRefTypeForHeapType(arg, true)));
}
void OpcodeDispatcher::dispatchRefIsNull() {
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	pushValue(jit_insn_eq(function, ref, nullRef));
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg) {
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = functionTypeIndexForFunc(arg);
	pushValue(castRefValue(callablePointerForFuncIndex(arg), jitRefTypeForHeapType(ht, false)));
}
void OpcodeDispatcher::dispatchRefEq() {
	jit_value_t rhs = popValue();
	jit_value_t lhs = popValue();
	pushValue(jit_insn_eq(function, refAsVoidPtr(lhs), refAsVoidPtr(rhs)));
}
void OpcodeDispatcher::dispatchRefAsNonNull() {
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(ref));
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg) {
	// Dead code: the compile-time stack is polymorphic (see
	// storeStackTopToSlots) — pushes are no-ops here, so a pop would underflow
	// the model. Nothing to emit either way (GC.md G7/GC-4).
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);

	// `br_on_null l` has type `[t* (ref null ht)] -> [t* (ref ht)]`; the label
	// carries only `t*` — the reference itself stays on the fall-through path.
	if (arg >= controlStack.size()) {
		// Conditional return of the function's results (the `t*` list).
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, isNull, &skip);
		emitFunctionReturn(false);
		jit_insn_label(function, &skip);
		pushValue(ref);
		return;
	}
	ControlBlock& target = branchTarget(arg);
	const auto& slots = branchSlotsForTarget(target);
	// The stack below the popped reference is exactly the label's `t*` list.
	// Storing the reference into `slots.back()` (as this used to) overwrote the
	// last `t*` value with a reference the label does not even expect.
	storeStackTopToSlots(slots);
	jit_insn_branch_if(function, isNull, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg) {
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);

	// `br_on_non_null l` has type `[t* (ref null ht)] -> [t*]`; the branch
	// carries `t* (ref ht)`, so the reference is consumed on *both* paths.
	if (arg >= controlStack.size()) {
		// Conditional return of the function's results (`t* (ref ht)`).
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if(function, isNull, &skip);
		valueStack.push_back(ref);
		emitFunctionReturn(false);
		valueStack.pop_back();
		jit_insn_label(function, &skip);
		return; // fall-through: the reference is consumed
	}
	ControlBlock& target = branchTarget(arg);
	const auto& slots = branchSlotsForTarget(target);
	// Stage the reference over the `t*` operands and store the whole label
	// window; the `t*` values must reach the label too.
	valueStack.push_back(ref);
	storeStackTopToSlots(slots);
	valueStack.pop_back();
	jit_insn_branch_if_not(function, isNull, &target.label);
	// Fall-through: only `t*` remains — the reference is consumed.
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg) {
	const WASM::CoreTypeView st = structTypeForIndex(arg);
	if (st.descriptor())
		throw std::runtime_error("struct.new: use struct.new_desc for a described type");
	const auto fields = st.structFields();
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 3, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_default_impl",
		reinterpret_cast<void*>(wasm_struct_new_default_impl), sig, args, 3, 0);

	std::vector<jit_value_t> values(fields.size());
	for (size_t i = fields.size(); i-- > 0; )
		values[i] = popValue();
	for (size_t i = 0; i < fields.size(); ++i) {
		const WASM::CanonStorageType& storage = fields[i].storage;
		jit_type_t fieldType = typeTranslator.translateCanonStorage(storage);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
			// The new object's reference field owns a count (RC-5).
			jit_value_t slot = jit_insn_add(function, object,
					jit_value_create_nint_constant(function, jit_type_nint,
							structFieldOffset(arg, static_cast<uint32_t>(i))));
			emitRefStore(slot, values[i]);
			continue;
		}
		jit_insn_store_relative(function, object, structFieldOffset(arg, static_cast<uint32_t>(i)),
								castRefValue(values[i], fieldType));
	}
	pushValue(castRefValue(object, structRefType(arg, false)));
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg) {
	if (structTypeForIndex(arg).descriptor())
		throw std::runtime_error("struct.new_default: use struct.new_default_desc for a described type");
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 3, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_default_impl",
		reinterpret_cast<void*>(wasm_struct_new_default_impl), sig, args, 3, 0);
	pushValue(castRefValue(object, structRefType(arg, false)));
}
void OpcodeDispatcher::dispatchStructNewDesc(WASM::TypeIdx arg) {
	const WASM::CoreTypeView st = structTypeForIndex(arg);
	const auto fields = st.structFields();
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_value_t descriptor = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 4, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		refAsVoidPtr(descriptor)
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_desc_impl",
		reinterpret_cast<void*>(wasm_struct_new_desc_impl), sig, args, 4, 0);
	// The helper retained the descriptor; release the operand stack's own
	// ownership of that reference.
	emitDropValue(descriptor);

	std::vector<jit_value_t> values(fields.size());
	for (size_t i = fields.size(); i-- > 0; )
		values[i] = popValue();
	for (size_t i = 0; i < fields.size(); ++i) {
		const WASM::CanonStorageType& storage = fields[i].storage;
		jit_type_t fieldType = typeTranslator.translateCanonStorage(storage);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
			jit_value_t slot = jit_insn_add(function, object,
					jit_value_create_nint_constant(function, jit_type_nint,
							structFieldOffset(arg, static_cast<uint32_t>(i))));
			emitRefStore(slot, values[i]);
			continue;
		}
		jit_insn_store_relative(function, object, structFieldOffset(arg, static_cast<uint32_t>(i)),
				castRefValue(values[i], fieldType));
	}
	pushValue(castRefValue(object, structRefType(arg, false)));
}

void OpcodeDispatcher::dispatchStructNewDefaultDesc(WASM::TypeIdx arg) {
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_value_t descriptor = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 4, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		refAsVoidPtr(descriptor)
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_desc_impl",
		reinterpret_cast<void*>(wasm_struct_new_desc_impl), sig, args, 4, 0);
	emitDropValue(descriptor);
	pushValue(castRefValue(object, structRefType(arg, false)));
}

void OpcodeDispatcher::dispatchRefGetDesc(WASM::TypeIdx arg) {
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_value_t descriptor = jit_insn_call_native(function, "wasm_ref_get_desc_impl",
		reinterpret_cast<void*>(wasm_ref_get_desc_impl), sig, args, 2, 0);
	const WASM::CoreTypeView view = structTypeForIndex(arg);
	if (!view.descriptor())
		throw std::runtime_error("ref.get_desc: type has no descriptor");
	const WASM::CanonHeapType canonical = *view.descriptor();
	WASM::HeapType heap;
	if (canonical.kind == WASM::CanonHeapType::Kind::Global) {
		const auto local = module.typeIndexForId(canonical.id);
		if (!local)
			throw std::runtime_error("ref.get_desc: descriptor type is not local");
		heap.isTypeIndex = true;
		heap.typeIndex = *local;
	} else if (canonical.kind == WASM::CanonHeapType::Kind::Abstract) {
		heap.isTypeIndex = false;
		heap.abstract = canonical.abstract;
	} else {
		throw std::runtime_error("ref.get_desc: unresolved descriptor type");
	}
	heap.isExact = canonical.isExact;
	pushValue(castRefValue(descriptor, jitRefTypeForHeapType(heap, false)));
	emitDropValue(ref);
}

void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) {
	const auto fields = structTypeForIndex(arg1).structFields();
	assert(arg2 < fields.size());
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	jit_type_t fieldType = typeTranslator.translateCanonStorage(fields[arg2].storage);
	pushValue(jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), fieldType));
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) {
	const auto fields = structTypeForIndex(arg1).structFields();
	assert(arg2 < fields.size());
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	const bool isI8 = fields[arg2].storage.isPacked
			&& fields[arg2].storage.packed == WASM::ValueTypeCode::I8;
	jit_type_t loadType = isI8 ? jit_type_sbyte : jit_type_short;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) {
	const auto fields = structTypeForIndex(arg1).structFields();
	assert(arg2 < fields.size());
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	const bool isI8 = fields[arg2].storage.isPacked
			&& fields[arg2].storage.packed == WASM::ValueTypeCode::I8;
	jit_type_t loadType = isI8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) {
	const auto fields = structTypeForIndex(arg1).structFields();
	assert(arg2 < fields.size());
	jit_value_t value = popValue();
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	const jit_nint offset = structFieldOffset(arg1, arg2);
	const WASM::CanonStorageType& storage = fields[arg2].storage;
	if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
		// Reference-typed field: go through the write barrier (retain new,
		// release old), then release the stack's ownership of the new value.
		jit_value_t slot = jit_insn_add(function, refAsVoidPtr(ref),
				jit_value_create_nint_constant(function, jit_type_nint, offset));
		emitRefStore(slot, value);
		emitDropValue(value);
		emitDropValue(ref);
		return;
	}
	jit_type_t fieldType = typeTranslator.translateCanonStorage(storage);
	jit_insn_store_relative(function, refAsVoidPtr(ref), offset,
							castRefValue(value, fieldType));
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg) {
	jit_value_t length = popValue();
	jit_value_t init = popValue();
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, jit_value_create_nint_constant(function, jit_type_uint, 0));
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t cond = jit_insn_lt(function, idx, jit_insn_convert(function, length, jit_type_uint, 0));
	jit_insn_branch_if_not(function, cond, &endLabel);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).arrayElement().storage.value.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
		emitRefStore(slot, init);
	} else {
		jit_insn_store_elem(function, dataPtr, idx, castRefValue(init, elemType));
	}
	jit_insn_store(function, idxSlot, jit_insn_add(function, idx, jit_value_create_nint_constant(function, jit_type_uint, 1)));
	jit_insn_branch(function, &loopLabel);
	jit_insn_label(function, &endLabel);
	pushValue(castRefValue(arrayRef, arrayRefType(arg, false)));
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg) {
	jit_value_t length = popValue();
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg, false)));
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
	std::vector<jit_value_t> values(arg2);
	for (size_t i = arg2; i-- > 0; )
		values[i] = popValue();
	for (uint32_t i = 0; i < arg2; ++i) {
		jit_value_t idx = jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(i));
		if (WASM::isReferenceType(arrayTypeForIndex(arg1).arrayElement().storage.value.opcode)) {
			jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
			emitRefStore(slot, values[i]);
		} else {
			jit_insn_store_elem(function, dataPtr, idx, castRefValue(values[i], elemType));
		}
	}
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
namespace {

// Validate an array.new_data / array.new_elem source range *before* the array is
// allocated (docs/proposals/GC.md GC-5). Without this, an absurd length fails the
// allocation and reports "out of memory" where the spec requires the
// out-of-bounds trap.
void wasm_check_data_segment_range_impl(WASM::VMContext* vm, uint32_t dataIdx,
		uint32_t srcByteOffset, uint32_t lenElems, uint32_t elemSize)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->checkDataSegmentRange(
			dataIdx, srcByteOffset, lenElems, elemSize);
}

void wasm_check_element_segment_range_impl(WASM::VMContext* vm, uint32_t elemIdx,
		uint32_t srcIndex, uint32_t lenElems)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->checkElementSegmentRange(elemIdx, srcIndex, lenElems);
}

} // namespace

void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t length = popValue();
	jit_value_t src = popValue();
	jit_type_t elemType = arrayElementJitType(arg1);
	{
		// The spec checks the source range first: a huge length must report the
		// out-of-bounds access it is, not an allocation failure.
		jit_type_t checkParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
		jit_type_t checkSig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, checkParams, 5, 1);
		jit_value_t checkArgs[] = {
			vmContextValue(),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
			jit_insn_convert(function, src, jit_type_uint, 0),
			jit_insn_convert(function, length, jit_type_uint, 0),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType)))
		};
		jit_insn_call_native(function, "wasm_check_data_segment_range_impl",
			reinterpret_cast<void*>(wasm_check_data_segment_range_impl), checkSig, checkArgs, 5, 0);
	}
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
	jit_type_t initParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, initParams, 5, 1);
	jit_value_t byteLen = jit_insn_mul(function, jit_insn_convert(function, length, jit_type_uint, 0),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dataPtr), jit_insn_convert(function, src, jit_type_uint, 0), byteLen};
	jit_insn_call_native(function, "wasm_buffer_init_from_data_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_data_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t length = popValue();
	jit_value_t src = popValue();
	jit_type_t elemType = arrayElementJitType(arg1);
	{
		// Same ordering rule as array.new_data: out-of-bounds first (GC-5).
		jit_type_t checkParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint};
		jit_type_t checkSig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, checkParams, 4, 1);
		jit_value_t checkArgs[] = {
			vmContextValue(),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
			jit_insn_convert(function, src, jit_type_uint, 0),
			jit_insn_convert(function, length, jit_type_uint, 0)
		};
		jit_insn_call_native(function, "wasm_check_element_segment_range_impl",
			reinterpret_cast<void*>(wasm_check_element_segment_range_impl), checkSig, checkArgs, 4, 0);
	}
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
	jit_type_t initParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, initParams, 5, 1);
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dataPtr), jit_insn_convert(function, src, jit_type_uint, 0),
						  jit_insn_convert(function, length, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_buffer_init_from_elems_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_elems_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
void OpcodeDispatcher::dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) {
	jit_value_t length = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcRef = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 6, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(dst)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(src)),
		refAsVoidPtr(srcRef),
		start,
		length
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_array_impl",
		reinterpret_cast<void*>(wasm_array_new_array_impl), sig, args, 6, 0);
	// The helper reports a trap and returns null; stop before using it.
	emitPendingTrapCheck();
	// The helper copied (and retained) what it needed; release the operand
	// stack's count on the source array.
	emitDropValue(srcRef);
	pushValue(castRefValue(arrayRef, arrayRefType(dst, false)));
}

void OpcodeDispatcher::dispatchArrayNewMemory(WASM::TypeIdx dst, uint32_t src) {
	jit_value_t length = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(dst)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(src)),
		start,
		length
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_memory_impl",
		reinterpret_cast<void*>(wasm_array_new_memory_impl), sig, args, 5, 0);
	emitPendingTrapCheck();
	pushValue(castRefValue(arrayRef, arrayRefType(dst, false)));
}

void OpcodeDispatcher::dispatchArrayNewTable(WASM::TypeIdx dst, uint32_t src) {
	jit_value_t length = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint,
			jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(dst)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(src)),
		start,
		length
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_table_impl",
		reinterpret_cast<void*>(wasm_array_new_table_impl), sig, args, 5, 0);
	emitPendingTrapCheck();
	pushValue(castRefValue(arrayRef, arrayRefType(dst, false)));
}

void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	pushValue(jit_insn_load_elem(function, dataPtr, index, elemType));
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t raw = jit_insn_load_elem(function, dataPtr, index, elemType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t addr = jit_insn_load_elem_address(function, dataPtr, index, elemType);
	const WASM::CanonStorageType& element = arrayTypeForIndex(arg).arrayElement().storage;
	const bool isI8 = element.isPacked && element.packed == WASM::ValueTypeCode::I8;
	jit_type_t loadType = isI8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, addr, 0, loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg) {
	jit_value_t value = popValue();
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).arrayElement().storage.value.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, index, elemType);
		emitRefStore(slot, value);
		emitDropValue(value);
		emitDropValue(ref);
		return;
	}
	jit_insn_store_elem(function, dataPtr, index, castRefValue(value, elemType));
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayLen() {
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref),
			static_cast<jit_nint>(WASM::GcHeader::kArrayLengthOffset), jit_type_uint);
	pushValue(jit_insn_convert(function, len, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t value = popValue();
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t end = jit_insn_add(function, start, len);
	jit_value_t ok = jit_insn_le(function, end, arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, start);
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t loopCond = jit_insn_lt(function, idx, end);
	jit_insn_branch_if_not(function, loopCond, &endLabel);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).arrayElement().storage.value.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
		emitRefStore(slot, value);
	} else {
		jit_insn_store_elem(function, dataPtr, idx, castRefValue(value, elemType));
	}
	jit_insn_store(function, idxSlot, jit_insn_add(function, idx, jit_value_create_nint_constant(function, jit_type_uint, 1)));
	jit_insn_branch(function, &loopLabel);
	jit_insn_label(function, &endLabel);
	emitDropValue(value);
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcRef = popValue();
	jit_value_t dstIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dstRef = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(srcRef));
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(dstRef));
	jit_value_t srcLen = jit_insn_load_relative(function, refAsVoidPtr(srcRef), arrayLengthOffset(arg2), jit_type_uint);
	jit_value_t dstLen = jit_insn_load_relative(function, refAsVoidPtr(dstRef), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t srcOk = jit_insn_le(function, jit_insn_add(function, srcIndex, len), srcLen);
	jit_value_t dstOk = jit_insn_le(function, jit_insn_add(function, dstIndex, len), dstLen);
	jit_value_t ok = jit_insn_and(function, srcOk, dstOk);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t dstElemType = arrayElementJitType(arg1);
	jit_type_t srcElemType = arrayElementJitType(arg2);
	if (jit_type_get_size(dstElemType) != jit_type_get_size(srcElemType))
		emitTrap(WASM::TrapKind::Internal, "array.copy element size mismatch");
	jit_value_t dstPtr = arrayElementBase(dstRef, arg1);
	jit_value_t srcPtr = arrayElementBase(srcRef, arg2);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 8, 1);
	jit_value_t args[] = {
		vmContextValue(), refAsVoidPtr(dstPtr), dstIndex, refAsVoidPtr(srcPtr), srcIndex, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(dstElemType))),
		jit_value_create_nint_constant(function, jit_type_int,
				WASM::isReferenceType(arrayTypeForIndex(arg1).arrayElement().storage.value.opcode) ? 1 : 0)
	};
	jit_insn_call_native(function, "wasm_buffer_copy_impl",
						 reinterpret_cast<void*>(wasm_buffer_copy_impl), sig, args, 8, 0);
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_value_t basePtr = arrayElementBase(ref, arg1);
	jit_value_t dstPtr = jit_insn_load_elem_address(function, basePtr, dst, elemType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t byteLen = jit_insn_mul(function, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	// `src` is a *byte* offset into the data segment; only `len` counts elements
	// (spec: array.init_data). Scaling `src` too made every element wider than a
	// byte read past the end of the segment.
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dstPtr), src, byteLen};
	jit_insn_call_native(function, "wasm_buffer_init_from_data_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_data_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_value_t basePtr = arrayElementBase(ref, arg1);
	jit_value_t dstPtr = jit_insn_load_elem_address(function, basePtr, dst, elemType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dstPtr), src, len};
	jit_insn_call_native(function, "wasm_buffer_init_from_elems_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_elems_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	pushValue(emitRefTypeTest(ref, arg, false));
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	pushValue(emitRefTypeTest(ref, arg, true));
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	jit_value_t ok = emitRefTypeTest(ref, arg, false);
	jit_label_t matched = jit_label_undefined;
	jit_insn_branch_if(function, ok, &matched);
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &matched);
	jit_value_t casted = castRefValue(ref, jitRefTypeForHeapType(arg, false));
	pushValue(casted);
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	jit_value_t ok = emitRefTypeTest(ref, arg, true);
	jit_label_t matched = jit_label_undefined;
	jit_insn_branch_if(function, ok, &matched);
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &matched);
	pushValue(castRefValue(ref, jitRefTypeForHeapType(arg, true)));
}
void OpcodeDispatcher::dispatchRefCastDescEq(const WASM::HeapType& arg) {
	jit_value_t descriptor = popValue();
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref), refAsVoidPtr(descriptor),
			jit_value_create_nint_constant(function, jit_type_int, 0)};
	jit_value_t matched = jit_insn_call_native(function, "wasm_desc_eq_impl",
			reinterpret_cast<void*>(wasm_desc_eq_impl), sig, args, 4, 0);
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, matched, &ok);
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &ok);
	pushValue(castRefValue(ref, jitRefTypeForHeapType(arg, false)));
	emitDropValue(descriptor);
	emitDropValue(ref);
}

void OpcodeDispatcher::dispatchRefCastDescEqNull(const WASM::HeapType& arg) {
	jit_value_t descriptor = popValue();
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref), refAsVoidPtr(descriptor),
			jit_value_create_nint_constant(function, jit_type_int, 1)};
	jit_value_t matched = jit_insn_call_native(function, "wasm_desc_eq_impl",
			reinterpret_cast<void*>(wasm_desc_eq_impl), sig, args, 4, 0);
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, matched, &ok);
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &ok);
	pushValue(castRefValue(ref, jitRefTypeForHeapType(arg, true)));
	emitDropValue(descriptor);
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchBrOnCastDescEq(uint8_t castop, WASM::LabelIdx l,
		WASM::HeapType ht1, WASM::HeapType ht2) {
	(void)ht1;
	const bool targetNullable = (castop & 0x02) != 0;
	if (unreachableCode)
		return;
	jit_value_t descriptor = popValue();
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref), refAsVoidPtr(descriptor),
			jit_value_create_nint_constant(function, jit_type_int, targetNullable ? 1 : 0)};
	jit_value_t matched = jit_insn_call_native(function, "wasm_desc_eq_impl",
			reinterpret_cast<void*>(wasm_desc_eq_impl), sig, args, 4, 0);
	emitDropValue(descriptor);
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);

	if (l >= controlStack.size()) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, matched, &skip);
		valueStack.push_back(casted);
		emitFunctionReturn(false);
		valueStack.pop_back();
		jit_insn_label(function, &skip);
		pushValue(ref);
		return;
	}
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	valueStack.push_back(casted);
	storeStackTopToSlots(slots);
	valueStack.pop_back();
	jit_insn_branch_if(function, matched, &target.label);
	pushValue(ref);
}

void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	// `castop` is the spec's castflags: bit 0 = the *source* is nullable, bit 1 =
	// the *target* is nullable. Only the target's flag changes anything here — it
	// decides both the test (`wasm_ref_matches_heap_type` returns `nullable` for
	// a null reference) and the type the branch carries. The source's flag
	// describes the fall-through type `ht1 \ ht2`, which the engine does not
	// track: values are typed by their JIT type, not by heap type
	// (docs/proposals/GC.md G7 / GC-4).
	const bool targetNullable = (castop & 0x02) != 0;
	(void)ht1;
	// Dead code: the model stack is polymorphic and pushes are no-ops there, so
	// popping would underflow — and there is nothing to branch with.
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, targetNullable);

	// A branch to the function's implicit block is a *conditional return*; the
	// function's result list plays the role of the label's value list
	// (dispatchBrIf handles the same case). The branch carries the casted
	// reference, the fall-through keeps the original one.
	if (l >= controlStack.size()) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, matched, &skip);
		// emitFunctionReturn(false) reads the results from the top of the model
		// stack without consuming them, so stage the branch's reference only.
		valueStack.push_back(casted);
		emitFunctionReturn(false);
		valueStack.pop_back();
		jit_insn_label(function, &skip);
		pushValue(ref);
		return;
	}

	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	// The label's values are `t* rt2`: the operands below the reference are the
	// `t*` list, and the last slot is the reference. Store the whole window
	// with the *branch's* reference staged on top, so a label that carries
	// values actually receives them (`storeStackTopToSlots` aligns the slot
	// window with the top of the model stack). Storing only `slots.back()` left
	// the `t*` slots holding their previous contents.
	valueStack.push_back(casted);
	storeStackTopToSlots(slots);
	valueStack.pop_back();
	jit_insn_branch_if(function, matched, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnCastDescEqFail(uint8_t castop, WASM::LabelIdx l,
		WASM::HeapType ht1, WASM::HeapType ht2) {
	(void)ht1;
	const bool targetNullable = (castop & 0x02) != 0;
	if (unreachableCode)
		return;
	jit_value_t descriptor = popValue();
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref), refAsVoidPtr(descriptor),
			jit_value_create_nint_constant(function, jit_type_int, targetNullable ? 1 : 0)};
	jit_value_t matched = jit_insn_call_native(function, "wasm_desc_eq_impl",
			reinterpret_cast<void*>(wasm_desc_eq_impl), sig, args, 4, 0);
	emitDropValue(descriptor);
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);

	if (l >= controlStack.size()) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if(function, matched, &skip);
		valueStack.push_back(ref);
		emitFunctionReturn(false);
		valueStack.pop_back();
		jit_insn_label(function, &skip);
		pushValue(casted);
		return;
	}
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	valueStack.push_back(ref);
	storeStackTopToSlots(slots);
	valueStack.pop_back();
	jit_insn_branch_if_not(function, matched, &target.label);
	pushValue(casted);
}

void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	// Same flags, opposite branch: fall through with the cast value, branch with
	// the original one (the `ht1 \ ht2` case).
	const bool targetNullable = (castop & 0x02) != 0;
	(void)ht1;
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, targetNullable);

	// Conditional return: branch (return) with the original reference when the
	// cast fails, fall through with the casted one when it succeeds.
	if (l >= controlStack.size()) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if(function, matched, &skip);
		valueStack.push_back(ref);
		emitFunctionReturn(false);
		valueStack.pop_back();
		jit_insn_label(function, &skip);
		pushValue(casted);
		return;
	}

	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	// The branch carries `t* rt1\rt2` — the original reference — so stage it
	// over the `t*` operands and store the whole label window.
	valueStack.push_back(ref);
	storeStackTopToSlots(slots);
	valueStack.pop_back();
	jit_insn_branch_if_not(function, matched, &target.label);
	pushValue(casted);
}
void OpcodeDispatcher::dispatchAnyConvertExtern() {
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_value_t unboxed = jit_insn_call_native(function, "wasm_extern_unbox_impl",
			reinterpret_cast<void*>(wasm_extern_unbox_impl), sig, args, 2, 0);
	// The box (if this was one) owns the inner reference; move that ownership
	// to the result before dropping the input.
	emitDropValue(ref);
	pushValue(unboxed);
}
void OpcodeDispatcher::dispatchExternConvertAny() {
	jit_value_t ref = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_value_t boxed = jit_insn_call_native(function, "wasm_extern_box_impl",
			reinterpret_cast<void*>(wasm_extern_box_impl), sig, args, 2, 0);
	// `externBox` retained the inner reference; the operand stack's count on
	// the input is released here.
	emitDropValue(ref);
	pushValue(boxed);
}
void OpcodeDispatcher::dispatchRefI31() {
	jit_value_t value = popValue();
	pushValue(castRefValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_new_impl),
										   jit_type_void_ptr, jit_type_int, value),
						   jitTypeForValueType(WASM::ValueType{WASM::ValueTypeCode::I31Ref, -1})));
}
void OpcodeDispatcher::dispatchI31GetS() {
	jit_value_t ref = popValue();
	// i31.get_* dereferences the reference, so a null one traps
	// (GC.md: "null i31 reference"). The tagged pointer is only *tested* for
	// null here; the helper still receives the original value.
	emitTrapIfNull(WASM::TrapKind::NullI31Reference, refAsVoidPtr(ref));
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_s_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(ref)));
}
void OpcodeDispatcher::dispatchI31GetU() {
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullI31Reference, refAsVoidPtr(ref));
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_u_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(ref)));
}

// ── Stringref core instructions (docs/proposals/STRINGREF.md STR-3) ─────────

void OpcodeDispatcher::dispatchStringNewUtf8(WASM::MemIdx arg)
{
	jit_value_t len = popValue();
	jit_value_t ptr = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_new_utf8), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_uint,
			memoryPointerForIndex(arg),
			jit_insn_convert(function, ptr, jit_type_uint, 0),
			jit_insn_convert(function, len, jit_type_uint, 0)));
}

void OpcodeDispatcher::dispatchStringNewWtf16(WASM::MemIdx arg)
{
	jit_value_t len = popValue();
	jit_value_t ptr = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_new_wtf16), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_uint,
			memoryPointerForIndex(arg),
			jit_insn_convert(function, ptr, jit_type_uint, 0),
			jit_insn_convert(function, len, jit_type_uint, 0)));
}

void OpcodeDispatcher::dispatchStringNewLossyUtf8(WASM::MemIdx arg)
{
	jit_value_t len = popValue();
	jit_value_t ptr = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_new_lossy_utf8), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_uint,
			memoryPointerForIndex(arg),
			jit_insn_convert(function, ptr, jit_type_uint, 0),
			jit_insn_convert(function, len, jit_type_uint, 0)));
}

void OpcodeDispatcher::dispatchStringNewWtf8(WASM::MemIdx arg)
{
	jit_value_t len = popValue();
	jit_value_t ptr = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_new_wtf8), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_uint, jit_type_uint,
			memoryPointerForIndex(arg),
			jit_insn_convert(function, ptr, jit_type_uint, 0),
			jit_insn_convert(function, len, jit_type_uint, 0)));
}

void OpcodeDispatcher::dispatchStringConst(uint32_t idx)
{
	if (idx >= internals.stringLiterals.size())
		notImplemented("string.const index out of range");
	pushValue(jit_value_create_nint_constant(function, jit_type_void_ptr,
			reinterpret_cast<jit_nint>(internals.stringLiterals[idx])));
}

void OpcodeDispatcher::dispatchStringMeasureUtf8()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_string_measure_utf8), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringMeasureWtf8()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_string_measure_wtf8), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringMeasureWtf16()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_string_measure_wtf16), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringEncodeUtf8(WASM::MemIdx arg)
{
	jit_value_t ptr = popValue();
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_encode_utf8), jit_type_int,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			memoryPointerForIndex(arg), refAsVoidPtr(string),
			jit_insn_convert(function, ptr, jit_type_uint, 0)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringEncodeLossyUtf8(WASM::MemIdx arg)
{
	jit_value_t ptr = popValue();
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_encode_lossy_utf8), jit_type_int,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			memoryPointerForIndex(arg), refAsVoidPtr(string),
			jit_insn_convert(function, ptr, jit_type_uint, 0)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringEncodeWtf8(WASM::MemIdx arg)
{
	jit_value_t ptr = popValue();
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_encode_wtf8), jit_type_int,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			memoryPointerForIndex(arg), refAsVoidPtr(string),
			jit_insn_convert(function, ptr, jit_type_uint, 0)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringEncodeWtf16(WASM::MemIdx arg)
{
	jit_value_t ptr = popValue();
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_string_encode_wtf16), jit_type_int,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
			memoryPointerForIndex(arg), refAsVoidPtr(string),
			jit_insn_convert(function, ptr, jit_type_uint, 0)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringConcat()
{
	jit_value_t right = popValue();
	jit_value_t left = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_string_concat), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_void_ptr,
			refAsVoidPtr(left), refAsVoidPtr(right)));
	emitDropValue(left);
	emitDropValue(right);
}

void OpcodeDispatcher::dispatchStringEq()
{
	jit_value_t right = popValue();
	jit_value_t left = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_string_eq), jit_type_int,
			jit_type_void_ptr, jit_type_void_ptr,
			refAsVoidPtr(left), refAsVoidPtr(right)));
	emitDropValue(left);
	emitDropValue(right);
}

void OpcodeDispatcher::dispatchStringIsUsvSequence()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_string_is_usv_sequence), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

// ── stringview lowering (docs/proposals/STRINGREF.md STR-4) ────────────────

static jit_value_t callStringViewWtf8Encode(jit_function_t fn, void* target,
											jit_value_t memory, jit_value_t view,
											jit_value_t ptr, jit_value_t pos,
											jit_value_t bytes, int encoding)
{
	jit_type_t params[6] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
							jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 6, 1);
	jit_value_t args[6] = {memory, view, ptr, pos, bytes,
			jit_value_create_nint_constant(fn, jit_type_int, encoding)};
	return jit_insn_call_native(fn, "wasm.stringview.wtf8.encode", target, sig, args, 6, 0);
}

static jit_value_t callStringViewWtf16Encode(jit_function_t fn, void* target,
											 jit_value_t memory, jit_value_t view,
											 jit_value_t ptr, jit_value_t pos,
											 jit_value_t units)
{
	jit_type_t params[5] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint,
							jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 5, 1);
	jit_value_t args[5] = {memory, view, ptr, pos, units};
	return jit_insn_call_native(fn, "wasm.stringview.wtf16.encode", target, sig, args, 5, 0);
}

void OpcodeDispatcher::dispatchStringAsWtf8()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(&wasm_string_as_wtf8),
			jit_type_void_ptr, jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringAsWtf16()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(&wasm_string_as_wtf16),
			jit_type_void_ptr, jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringAsIter()
{
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(&wasm_string_as_iter),
			jit_type_void_ptr, jit_type_void_ptr, refAsVoidPtr(string)));
	emitDropValue(string);
}

void OpcodeDispatcher::dispatchStringViewWtf8Advance()
{
	jit_value_t bytes = popValue();
	jit_value_t pos = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf8_advance), jit_type_int,
			jit_type_void_ptr, jit_type_int, jit_type_int,
			refAsVoidPtr(view), pos, bytes));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg)
{
	jit_value_t bytes = popValue();
	jit_value_t pos = popValue();
	jit_value_t ptr = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	jit_value_t packed = callStringViewWtf8Encode(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf8_encode),
			memoryPointerForIndex(arg), refAsVoidPtr(view),
			jit_insn_convert(function, ptr, jit_type_uint, 0), pos, bytes, 0);
	jit_value_t written = jit_insn_convert(function, packed, jit_type_uint, 0);
	jit_value_t next = jit_insn_convert(function,
			jit_insn_shr(function, packed,
				jit_value_create_nint_constant(function, jit_type_uint, 32)),
			jit_type_uint, 0);
	pushValue(next);
	pushValue(written);
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg)
{
	jit_value_t bytes = popValue();
	jit_value_t pos = popValue();
	jit_value_t ptr = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	jit_value_t packed = callStringViewWtf8Encode(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf8_encode),
			memoryPointerForIndex(arg), refAsVoidPtr(view),
			jit_insn_convert(function, ptr, jit_type_uint, 0), pos, bytes, 1);
	jit_value_t written = jit_insn_convert(function, packed, jit_type_uint, 0);
	jit_value_t next = jit_insn_convert(function,
			jit_insn_shr(function, packed,
				jit_value_create_nint_constant(function, jit_type_uint, 32)),
			jit_type_uint, 0);
	pushValue(next);
	pushValue(written);
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg)
{
	jit_value_t bytes = popValue();
	jit_value_t pos = popValue();
	jit_value_t ptr = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	jit_value_t packed = callStringViewWtf8Encode(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf8_encode),
			memoryPointerForIndex(arg), refAsVoidPtr(view),
			jit_insn_convert(function, ptr, jit_type_uint, 0), pos, bytes, 2);
	jit_value_t written = jit_insn_convert(function, packed, jit_type_uint, 0);
	jit_value_t next = jit_insn_convert(function,
			jit_insn_shr(function, packed,
				jit_value_create_nint_constant(function, jit_type_uint, 32)),
			jit_type_uint, 0);
	pushValue(next);
	pushValue(written);
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf8Slice()
{
	jit_value_t end = popValue();
	jit_value_t start = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf8_slice), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_int, jit_type_int,
			refAsVoidPtr(view), start, end));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf16Length()
{
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf16_length), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(view)));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf16GetCodeUnit()
{
	jit_value_t pos = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf16_get_codeunit), jit_type_int,
			jit_type_void_ptr, jit_type_int, refAsVoidPtr(view), pos));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf16Encode(WASM::MemIdx arg)
{
	jit_value_t units = popValue();
	jit_value_t pos = popValue();
	jit_value_t ptr = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callStringViewWtf16Encode(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf16_encode),
			memoryPointerForIndex(arg), refAsVoidPtr(view),
			jit_insn_convert(function, ptr, jit_type_uint, 0), pos, units));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewWtf16Slice()
{
	jit_value_t end = popValue();
	jit_value_t start = popValue();
	jit_value_t view = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeTernary(function,
			reinterpret_cast<void*>(&wasm_stringview_wtf16_slice), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_int, jit_type_int,
			refAsVoidPtr(view), start, end));
	emitDropValue(view);
}

void OpcodeDispatcher::dispatchStringViewIterNext()
{
	jit_value_t iterator = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeUnary(function,
			reinterpret_cast<void*>(&wasm_stringview_iter_next), jit_type_int,
			jit_type_void_ptr, refAsVoidPtr(iterator)));
	emitDropValue(iterator);
}

void OpcodeDispatcher::dispatchStringViewIterAdvance()
{
	jit_value_t count = popValue();
	jit_value_t iterator = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_stringview_iter_advance), jit_type_int,
			jit_type_void_ptr, jit_type_int, refAsVoidPtr(iterator), count));
	emitDropValue(iterator);
}

void OpcodeDispatcher::dispatchStringViewIterRewind()
{
	jit_value_t count = popValue();
	jit_value_t iterator = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_stringview_iter_rewind), jit_type_int,
			jit_type_void_ptr, jit_type_int, refAsVoidPtr(iterator), count));
	emitDropValue(iterator);
}

void OpcodeDispatcher::dispatchStringViewIterSlice()
{
	jit_value_t count = popValue();
	jit_value_t iterator = popValue();
	if (unreachableCode)
		return;
	pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(&wasm_stringview_iter_slice), jit_type_void_ptr,
			jit_type_void_ptr, jit_type_int, refAsVoidPtr(iterator), count));
	emitDropValue(iterator);
}

// ── GC-array string bridge (docs/proposals/STRINGREF.md STR-6) ─────────────

static jit_value_t callStringArrayNew(jit_function_t fn, void* target,
									  jit_value_t array, jit_value_t start,
									  jit_value_t end, jit_value_t dataOffset,
									  jit_value_t elementSize, int mode)
{
	jit_type_t params[6] = {jit_type_void_ptr, jit_type_int, jit_type_int,
							jit_type_uint, jit_type_uint, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 6, 1);
	jit_value_t args[6] = {array, start, end, dataOffset, elementSize,
			jit_value_create_nint_constant(fn, jit_type_int, mode)};
	return jit_insn_call_native(fn, "wasm.string.array.new", target, sig, args, 6, 0);
}

static jit_value_t callStringArrayEncode(jit_function_t fn, void* target,
										 jit_value_t string, jit_value_t array,
										 jit_value_t start, jit_value_t dataOffset,
										 jit_value_t elementSize, int mode)
{
	jit_type_t params[6] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_int,
							jit_type_uint, jit_type_uint, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 6, 1);
	jit_value_t args[6] = {string, array, start, dataOffset, elementSize,
			jit_value_create_nint_constant(fn, jit_type_int, mode)};
	return jit_insn_call_native(fn, "wasm.string.array.encode", target, sig, args, 6, 0);
}

void OpcodeDispatcher::dispatchStringNewArrayImpl(WASM::TypeIdx arg, void* helper, int mode)
{
	jit_value_t end = popValue();
	jit_value_t start = popValue();
	jit_value_t array = popValue();
	if (unreachableCode)
		return;
	const jit_nint dataOffset = arrayDataOffset(arg);
	const jit_nint elementSize = jit_type_get_size(arrayElementJitType(arg));
	pushValue(callStringArrayNew(function, helper, refAsVoidPtr(array), start, end,
			jit_value_create_nint_constant(function, jit_type_uint, dataOffset),
			jit_value_create_nint_constant(function, jit_type_uint, elementSize), mode));
	emitDropValue(array);
}

void OpcodeDispatcher::dispatchStringEncodeArrayImpl(WASM::TypeIdx arg, void* helper, int mode)
{
	jit_value_t start = popValue();
	jit_value_t array = popValue();
	jit_value_t string = popValue();
	if (unreachableCode)
		return;
	const jit_nint dataOffset = arrayDataOffset(arg);
	const jit_nint elementSize = jit_type_get_size(arrayElementJitType(arg));
	pushValue(callStringArrayEncode(function, helper, refAsVoidPtr(string),
			refAsVoidPtr(array), start,
			jit_value_create_nint_constant(function, jit_type_uint, dataOffset),
			jit_value_create_nint_constant(function, jit_type_uint, elementSize), mode));
	emitDropValue(string);
	emitDropValue(array);
}

void OpcodeDispatcher::dispatchStringNewUtf8Array(WASM::TypeIdx arg)
{
	dispatchStringNewArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_new_array), 0);
}

void OpcodeDispatcher::dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg)
{
	dispatchStringNewArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_new_array), 1);
}

void OpcodeDispatcher::dispatchStringNewWtf8Array(WASM::TypeIdx arg)
{
	dispatchStringNewArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_new_array), 2);
}

void OpcodeDispatcher::dispatchStringNewWtf16Array(WASM::TypeIdx arg)
{
	dispatchStringNewArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_new_array), 3);
}

void OpcodeDispatcher::dispatchStringEncodeUtf8Array(WASM::TypeIdx arg)
{
	dispatchStringEncodeArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_encode_array), 0);
}

void OpcodeDispatcher::dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg)
{
	dispatchStringEncodeArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_encode_array), 1);
}

void OpcodeDispatcher::dispatchStringEncodeWtf8Array(WASM::TypeIdx arg)
{
	dispatchStringEncodeArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_encode_array), 2);
}

void OpcodeDispatcher::dispatchStringEncodeWtf16Array(WASM::TypeIdx arg)
{
	dispatchStringEncodeArrayImpl(arg, reinterpret_cast<void*>(&wasm_string_encode_array), 3);
}

void OpcodeDispatcher::dispatchI32TruncSatF32S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f32_s),
							  jit_type_int, jit_type_float32, popValue()));
}
void OpcodeDispatcher::dispatchI32TruncSatF32U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f32_u),
									jit_type_uint, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchI32TruncSatF64S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f64_s),
							  jit_type_int, jit_type_float64, popValue()));
}
void OpcodeDispatcher::dispatchI32TruncSatF64U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f64_u),
									jit_type_uint, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchI64TruncSatF32S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f32_s),
							  jit_type_long, jit_type_float32, popValue()));
}
void OpcodeDispatcher::dispatchI64TruncSatF32U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f32_u),
									jit_type_ulong, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchI64TruncSatF64S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f64_s),
							  jit_type_long, jit_type_float64, popValue()));
}
void OpcodeDispatcher::dispatchI64TruncSatF64U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f64_u),
									jit_type_ulong, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) {
	// memory.init dataidx memidx
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_memory_init_impl",
						 reinterpret_cast<void*>(wasm_memory_init_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg) {
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))};
	jit_insn_call_native(function, "wasm_data_drop_impl",
						 reinterpret_cast<void*>(wasm_data_drop_impl), sig, args, 2, 0);
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) {
	// memory.copy dst_memidx src_memidx (arg1 = dst, arg2 = src).
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		dst, src, len};
	jit_insn_call_native(function, "wasm_memory_copy_impl",
						 reinterpret_cast<void*>(wasm_memory_copy_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg) {
	jit_value_t len = popValue();
	jit_value_t value = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		dst, value, len};
	jit_insn_call_native(function, "wasm_memory_fill_impl",
						 reinterpret_cast<void*>(wasm_memory_fill_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchI32Itt(uint32_t immediate)
{
	// Compile-time only: the marker is a run-time NOP. The callback lets an
	// embedder map the immediate to its own profiler without introducing a
	// per-execution branch (docs/proposals/INSTRUMENT_TRACING.md IT-3).
	WASM::traceImmediate(immediate);
}

void OpcodeDispatcher::dispatchRoundingVariant(uint32_t sub)
{
	const uint32_t rel = (sub - 0x80u) % 0x14u;
	jit_value_t subValue = jit_value_create_nint_constant(function, jit_type_uint, sub);

	if (rel < 5) {
		if (rel == 0) {
			jit_value_t a = popValue();
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_f32_unary),
				jit_type_float32, jit_type_uint, jit_type_float32, subValue, a));
		} else {
			jit_value_t b = popValue();
			jit_value_t a = popValue();
			pushValue(callNativeTernary(function,
				reinterpret_cast<void*>(wasm_rounding_f32_binary),
				jit_type_float32, jit_type_uint, jit_type_float32, jit_type_float32,
				subValue, a, b));
		}
		return;
	}
	if (rel < 10) {
		if (rel == 5) {
			jit_value_t a = popValue();
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_f64_unary),
				jit_type_float64, jit_type_uint, jit_type_float64, subValue, a));
		} else {
			jit_value_t b = popValue();
			jit_value_t a = popValue();
			pushValue(callNativeTernary(function,
				reinterpret_cast<void*>(wasm_rounding_f64_binary),
				jit_type_float64, jit_type_uint, jit_type_float64, jit_type_float64,
				subValue, a, b));
		}
		return;
	}
	if (rel < 15) {
		jit_value_t a = popValue();
		switch (rel) {
		case 10:
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_i32_s_to_f32),
				jit_type_float32, jit_type_uint, jit_type_int, subValue, a));
			break;
		case 11:
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_i32_u_to_f32),
				jit_type_float32, jit_type_uint, jit_type_uint, subValue, a));
			break;
		case 12:
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_i64_s_to_f32),
				jit_type_float32, jit_type_uint, jit_type_long, subValue, a));
			break;
		case 13:
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_i64_u_to_f32),
				jit_type_float32, jit_type_uint, jit_type_ulong, subValue, a));
			break;
		default:
			pushValue(callNativeBinary(function,
				reinterpret_cast<void*>(wasm_rounding_f64_to_f32),
				jit_type_float32, jit_type_uint, jit_type_float64, subValue, a));
			break;
		}
		return;
	}
	jit_value_t a = popValue();
	switch (rel) {
	case 15:
		pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(wasm_rounding_i32_s_to_f64),
			jit_type_float64, jit_type_uint, jit_type_int, subValue, a));
		break;
	case 16:
		pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(wasm_rounding_i32_u_to_f64),
			jit_type_float64, jit_type_uint, jit_type_uint, subValue, a));
		break;
	case 17:
		pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(wasm_rounding_i64_s_to_f64),
			jit_type_float64, jit_type_uint, jit_type_long, subValue, a));
		break;
	case 18:
		pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(wasm_rounding_i64_u_to_f64),
			jit_type_float64, jit_type_uint, jit_type_ulong, subValue, a));
		break;
	default:
		pushValue(callNativeBinary(function,
			reinterpret_cast<void*>(wasm_rounding_f32_to_f64),
			jit_type_float64, jit_type_uint, jit_type_float32, subValue, a));
		break;
	}
}

void OpcodeDispatcher::dispatchMemoryDiscard(WASM::MemIdx arg) {
	jit_value_t len = popValue();
	jit_value_t dst = popValue();
	const bool is64 = memoryIndexTypeForIndex(arg) == WASM::MemoryIndexType::I64;
	// Interpret both operands as unsigned: a negative i32 becomes a huge
	// (failing) request rather than a wrapped small one.
	if (!is64) {
		dst = jit_insn_convert(function, dst, jit_type_uint, 0);
		len = jit_insn_convert(function, len, jit_type_uint, 0);
	}
	dst = jit_insn_convert(function, dst, jit_type_ulong, 0);
	len = jit_insn_convert(function, len, jit_type_ulong, 0);

	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_ulong, jit_type_ulong};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 4, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		dst, len};
	jit_insn_call_native(function, "wasm_memory_discard_impl",
						 reinterpret_cast<void*>(wasm_memory_discard_impl), sig, args, 4, 0);
}
void OpcodeDispatcher::dispatchI64Add128() {
	// [lhs_lo lhs_hi rhs_lo rhs_hi] -> [lo hi]. The operands are converted to
	// unsigned so the carry comparison is unsigned.
	jit_value_t bHi = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t bLo = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t aHi = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t aLo = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t lo = jit_insn_add(function, aLo, bLo);
	jit_value_t carry = jit_insn_lt(function, lo, aLo);
	jit_value_t hi = jit_insn_add(function, jit_insn_add(function, aHi, bHi),
			jit_insn_convert(function, carry, jit_type_ulong, 0));
	pushValue(jit_insn_convert(function, lo, jit_type_long, 0));
	pushValue(jit_insn_convert(function, hi, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchI64Sub128() {
	jit_value_t bHi = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t bLo = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t aHi = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t aLo = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t lo = jit_insn_sub(function, aLo, bLo);
	jit_value_t borrow = jit_insn_lt(function, aLo, bLo);
	jit_value_t hi = jit_insn_sub(function, jit_insn_sub(function, aHi, bHi),
			jit_insn_convert(function, borrow, jit_type_ulong, 0));
	pushValue(jit_insn_convert(function, lo, jit_type_long, 0));
	pushValue(jit_insn_convert(function, hi, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchI64MulWideS() {
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	jit_value_t lo = callNativeBinary(function,
			reinterpret_cast<void*>(wasm_i64_mul_wide_s_lo),
			jit_type_ulong, jit_type_long, jit_type_long, a, b);
	jit_value_t hi = callNativeBinary(function,
			reinterpret_cast<void*>(wasm_i64_mul_wide_s_hi),
			jit_type_ulong, jit_type_long, jit_type_long, a, b);
	pushValue(jit_insn_convert(function, lo, jit_type_long, 0));
	pushValue(jit_insn_convert(function, hi, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchI64MulWideU() {
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t lo = callNativeBinary(function,
			reinterpret_cast<void*>(wasm_i64_mul_wide_u_lo),
			jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	jit_value_t hi = callNativeBinary(function,
			reinterpret_cast<void*>(wasm_i64_mul_wide_u_hi),
			jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	pushValue(jit_insn_convert(function, lo, jit_type_long, 0));
	pushValue(jit_insn_convert(function, hi, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) {
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_table_init_impl",
						 reinterpret_cast<void*>(wasm_table_init_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg) {
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))};
	jit_insn_call_native(function, "wasm_elem_drop_impl",
						 reinterpret_cast<void*>(wasm_elem_drop_impl), sig, args, 2, 0);
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) {
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		dst, src, len};
	jit_insn_call_native(function, "wasm_table_copy_impl",
						 reinterpret_cast<void*>(wasm_table_copy_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg) {
	const bool is64 = tableIs64Bit(arg);
	jit_value_t delta = popValue();
	jit_value_t initRef = popValue();
	// A 64-bit table grows by an i64 delta and reports an i64 old size
	// (docs/proposals/MEMORY64.md M4).
	delta = jit_insn_convert(function, delta, is64 ? jit_type_ulong : jit_type_uint, 0);
	if (!is64)
		delta = jit_insn_convert(function, delta, jit_type_ulong, 0);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_ulong};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 4, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		initRef, delta};
	jit_value_t result = jit_insn_call_native(function, "wasm_table_grow_impl",
											   reinterpret_cast<void*>(wasm_table_grow_impl), sig, args, 4, 0);
	if (is64)
		pushValue(jit_insn_convert(function, result, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg) {
	// An absent table (e.g. an unmodelled imported table) reports size 0.
	jit_value_t vm = vmContextValue();
	jit_value_t tables = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, tables), jit_type_void_ptr);
	jit_value_t size = jit_value_create(function, jit_type_ulong);
	jit_insn_store(function, size,
		jit_value_create_long_constant(function, jit_type_ulong, 0));

	jit_value_t tablesWord = jit_insn_convert(function, tables, jit_type_ulong, 0);
	jit_value_t present = jit_insn_ne(function, tablesWord,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t done = jit_label_undefined;
	jit_insn_branch_if_not(function, present, &done);

	jit_value_t table = jit_insn_load_relative(
		function, tables,
		static_cast<jit_nint>(arg) * static_cast<jit_nint>(sizeof(WASM::TableInstance*)),
		jit_type_void_ptr);
	jit_insn_store(function, size, jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, size), jit_type_ulong));

	jit_insn_label(function, &done);
	// A 64-bit table reports an i64 size (docs/proposals/MEMORY64.md M4).
	if (tableIs64Bit(arg))
		pushValue(jit_insn_convert(function, size, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, size, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg) {
	jit_value_t len = popValue();
	jit_value_t ref = popValue();
	jit_value_t start = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_int, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		start, ref, len};
	jit_insn_call_native(function, "wasm_table_fill_impl",
						 reinterpret_cast<void*>(wasm_table_fill_impl), sig, args, 5, 0);
}
// ── memory (docs/SIMD_LIBJIT.md §6.3) ───────────────────────────────────────
// Each handler is one line: the shape lives in the emit helper above, and the
// width is the number of bytes the instruction actually *reads* — that is what
// the bounds check must cover, and the memarg's alignment plays no part in it.
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg) {
	emitSimdLoad(arg, 16, reinterpret_cast<void*>(wasm_v128_load));
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load8x8_s));
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load8x8_u));
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load16x4_s));
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load16x4_u));
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load32x2_s));
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load32x2_u));
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 1, reinterpret_cast<void*>(wasm_v128_load8_splat));
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 2, reinterpret_cast<void*>(wasm_v128_load16_splat));
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 4, reinterpret_cast<void*>(wasm_v128_load32_splat));
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load64_splat));
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg) {
	emitSimdLoad(arg, 4, reinterpret_cast<void*>(wasm_v128_load32_zero));
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load64_zero));
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg) {
	emitSimdStore(arg, 16, reinterpret_cast<void*>(wasm_v128_store));
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 1, arg2, reinterpret_cast<void*>(wasm_v128_load8_lane));
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 2, arg2, reinterpret_cast<void*>(wasm_v128_load16_lane));
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 4, arg2, reinterpret_cast<void*>(wasm_v128_load32_lane));
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 8, arg2, reinterpret_cast<void*>(wasm_v128_load64_lane));
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 1, arg2, reinterpret_cast<void*>(wasm_v128_store8_lane));
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 2, arg2, reinterpret_cast<void*>(wasm_v128_store16_lane));
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 4, arg2, reinterpret_cast<void*>(wasm_v128_store32_lane));
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 8, arg2, reinterpret_cast<void*>(wasm_v128_store64_lane));
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg) {
	// The 16 immediate bytes are in memory order: bytes 0-7 are the low half,
	// 8-15 the high half, each little-endian (hazard Y11). Lowered *through the
	// helper* rather than four inline lane stores, because
	// `jit_insn_store_relative` does not write a `jit_value_create`d aggregate
	// (verified; docs/SIMD_LIBJIT.md §6.3).
	if (arg.size() < 16)
		return;
	uint64_t lo = 0;
	uint64_t hi = 0;
	for (unsigned i = 0; i < 8; ++i) {
		lo |= static_cast<uint64_t>(arg[i]) << (8u * i);
		hi |= static_cast<uint64_t>(arg[8 + i]) << (8u * i);
	}
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_ulong, jit_type_ulong};
	jit_value_t args[2] = {
		jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(lo)),
		jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(hi))};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_v128_const), out, types, args);
	pushValue(out);
}
// `i8x16.shuffle` and `i8x16.swizzle` are both one-vector table lookups. The
// shuffle selectors are *immediates* (16 bytes), so they travel as two u64s like
// every other 16-byte immediate (docs/SIMD_LIBJIT.md §6.3) rather than being
// materialised in memory just to pass an address.
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg) {
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	if (arg.size() < 16)
		return;
	uint64_t selLo = 0;
	uint64_t selHi = 0;
	for (unsigned i = 0; i < 8; ++i) {
		selLo |= static_cast<uint64_t>(arg[i]) << (8u * i);
		selHi |= static_cast<uint64_t>(arg[8 + i]) << (8u * i);
	}
	jit_value_t b = popValue();   // wasm's `[v128 v128] -> [v128]`: second on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[4] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_ulong, jit_type_ulong};
	jit_value_t args[4] = {jit_insn_address_of(function, a), jit_insn_address_of(function, b),
			jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(selLo)),
			jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(selHi))};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_shuffle), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI8x16Swizzle() {
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t s = popValue();   // the index vector is the second operand
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {jit_insn_address_of(function, a), jit_insn_address_of(function, s)};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_swizzle), out, types, args);
	pushValue(out);
}
// `*.splat`: pop the scalar, write 16 bytes of it into a fresh slot. The scalar
// arrives with its wasm shape's LibJIT type (i32/i64/f32/f64), which is what the
// helper's C signature expects (docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchI8x16Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI16x8Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i16x8_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI32x4Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i32x4_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI64x2Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_long};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i64x2_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF32x4Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_float32};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f32x4_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF64x2Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_float64};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f64x2_splat), out, types, args);
	pushValue(out);
}
// `*.extract_lane`: pop the vector, pass its address + the lane immediate, push
// the scalar result (the scalar-result shape of docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i8x16_extract_lane_s), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i8x16_extract_lane_u), jit_type_int, types, args));
}
// `*.replace_lane`: the stack is [vector, scalar], so the scalar pops first.
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i16x8_extract_lane_s), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i16x8_extract_lane_u), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i16x8_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i32x4_extract_lane), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i32x4_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i64x2_extract_lane), jit_type_long, types, args));
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_long};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i64x2_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_f32x4_extract_lane), jit_type_float32, types, args));
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_float32};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f32x4_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_f64x2_extract_lane), jit_type_float64, types, args));
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_float64};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f64x2_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI8x16Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_eq)); }
void OpcodeDispatcher::dispatchI8x16Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ne)); }
void OpcodeDispatcher::dispatchI8x16LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_lt_s)); }
void OpcodeDispatcher::dispatchI8x16LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_lt_u)); }
void OpcodeDispatcher::dispatchI8x16GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_gt_s)); }
void OpcodeDispatcher::dispatchI8x16GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_gt_u)); }
void OpcodeDispatcher::dispatchI8x16LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_le_s)); }
void OpcodeDispatcher::dispatchI8x16LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_le_u)); }
void OpcodeDispatcher::dispatchI8x16GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ge_s)); }
void OpcodeDispatcher::dispatchI8x16GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ge_u)); }
void OpcodeDispatcher::dispatchI16x8Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_eq)); }
void OpcodeDispatcher::dispatchI16x8Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ne)); }
void OpcodeDispatcher::dispatchI16x8LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_lt_s)); }
void OpcodeDispatcher::dispatchI16x8LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_lt_u)); }
void OpcodeDispatcher::dispatchI16x8GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_gt_s)); }
void OpcodeDispatcher::dispatchI16x8GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_gt_u)); }
void OpcodeDispatcher::dispatchI16x8LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_le_s)); }
void OpcodeDispatcher::dispatchI16x8LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_le_u)); }
void OpcodeDispatcher::dispatchI16x8GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ge_s)); }
void OpcodeDispatcher::dispatchI16x8GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ge_u)); }
void OpcodeDispatcher::dispatchI32x4Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_eq)); }
void OpcodeDispatcher::dispatchI32x4Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ne)); }
void OpcodeDispatcher::dispatchI32x4LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_lt_s)); }
void OpcodeDispatcher::dispatchI32x4LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_lt_u)); }
void OpcodeDispatcher::dispatchI32x4GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_gt_s)); }
void OpcodeDispatcher::dispatchI32x4GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_gt_u)); }
void OpcodeDispatcher::dispatchI32x4LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_le_s)); }
void OpcodeDispatcher::dispatchI32x4LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_le_u)); }
void OpcodeDispatcher::dispatchI32x4GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ge_s)); }
void OpcodeDispatcher::dispatchI32x4GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ge_u)); }
void OpcodeDispatcher::dispatchF32x4Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_eq)); }
void OpcodeDispatcher::dispatchF32x4Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_ne)); }
void OpcodeDispatcher::dispatchF32x4Lt() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_lt)); }
void OpcodeDispatcher::dispatchF32x4Gt() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_gt)); }
void OpcodeDispatcher::dispatchF32x4Le() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_le)); }
void OpcodeDispatcher::dispatchF32x4Ge() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_ge)); }
void OpcodeDispatcher::dispatchF64x2Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_eq)); }
void OpcodeDispatcher::dispatchF64x2Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_ne)); }
void OpcodeDispatcher::dispatchF64x2Lt() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_lt)); }
void OpcodeDispatcher::dispatchF64x2Gt() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_gt)); }
void OpcodeDispatcher::dispatchF64x2Le() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_le)); }
void OpcodeDispatcher::dispatchF64x2Ge() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_ge)); }
// Bitwise ops are whole-vector, so they are the shape `callSimdOutHelper` exists
// for: pop the operands, pass their addresses, write into a fresh slot. The
// helpers are pure and never trap (docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchV128Not() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_v128_not)); }
void OpcodeDispatcher::dispatchV128And() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_and)); }
void OpcodeDispatcher::dispatchV128AndNot() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_andnot)); }
void OpcodeDispatcher::dispatchV128Or() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_or)); }
void OpcodeDispatcher::dispatchV128Xor() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_xor)); }
void OpcodeDispatcher::dispatchV128Bitselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_v128_bitselect)); }
void OpcodeDispatcher::dispatchV128AnyTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_v128_any_true)); }
void OpcodeDispatcher::dispatchI8x16Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_abs)); }
void OpcodeDispatcher::dispatchI8x16Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_neg)); }
void OpcodeDispatcher::dispatchI8x16Popcnt() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_popcnt)); }
void OpcodeDispatcher::dispatchI8x16AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i8x16_all_true)); }
void OpcodeDispatcher::dispatchI8x16Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i8x16_bitmask)); }
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_narrow_i16x8_s)); }
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_narrow_i16x8_u)); }
void OpcodeDispatcher::dispatchI8x16Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shl)); }
void OpcodeDispatcher::dispatchI8x16ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shr_s)); }
void OpcodeDispatcher::dispatchI8x16ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shr_u)); }
void OpcodeDispatcher::dispatchI8x16Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add)); }
void OpcodeDispatcher::dispatchI8x16AddSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add_sat_s)); }
void OpcodeDispatcher::dispatchI8x16AddSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add_sat_u)); }
void OpcodeDispatcher::dispatchI8x16Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub)); }
void OpcodeDispatcher::dispatchI8x16SubSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub_sat_s)); }
void OpcodeDispatcher::dispatchI8x16SubSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub_sat_u)); }
void OpcodeDispatcher::dispatchI8x16MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_min_s)); }
void OpcodeDispatcher::dispatchI8x16MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_min_u)); }
void OpcodeDispatcher::dispatchI8x16MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_max_s)); }
void OpcodeDispatcher::dispatchI8x16MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_max_u)); }
void OpcodeDispatcher::dispatchI8x16AvgrU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_avgr_u)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extadd_pairwise_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extadd_pairwise_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_abs)); }
void OpcodeDispatcher::dispatchI16x8Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_neg)); }
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_q15mulr_sat_s)); }
void OpcodeDispatcher::dispatchI16x8AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i16x8_all_true)); }
void OpcodeDispatcher::dispatchI16x8Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i16x8_bitmask)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_narrow_i32x4_s)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_narrow_i32x4_u)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shl)); }
void OpcodeDispatcher::dispatchI16x8ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shr_s)); }
void OpcodeDispatcher::dispatchI16x8ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shr_u)); }
void OpcodeDispatcher::dispatchI16x8Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add)); }
void OpcodeDispatcher::dispatchI16x8AddSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add_sat_s)); }
void OpcodeDispatcher::dispatchI16x8AddSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add_sat_u)); }
void OpcodeDispatcher::dispatchI16x8Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub)); }
void OpcodeDispatcher::dispatchI16x8SubSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub_sat_s)); }
void OpcodeDispatcher::dispatchI16x8SubSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub_sat_u)); }
void OpcodeDispatcher::dispatchI16x8Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_mul)); }
void OpcodeDispatcher::dispatchI16x8MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_min_s)); }
void OpcodeDispatcher::dispatchI16x8MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_min_u)); }
void OpcodeDispatcher::dispatchI16x8MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_max_s)); }
void OpcodeDispatcher::dispatchI16x8MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_max_u)); }
void OpcodeDispatcher::dispatchI16x8AvgrU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_avgr_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extadd_pairwise_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extadd_pairwise_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_abs)); }
void OpcodeDispatcher::dispatchI32x4Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_neg)); }
void OpcodeDispatcher::dispatchI32x4AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i32x4_all_true)); }
void OpcodeDispatcher::dispatchI32x4Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i32x4_bitmask)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shl)); }
void OpcodeDispatcher::dispatchI32x4ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shr_s)); }
void OpcodeDispatcher::dispatchI32x4ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shr_u)); }
void OpcodeDispatcher::dispatchI32x4Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_add)); }
void OpcodeDispatcher::dispatchI32x4Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_sub)); }
void OpcodeDispatcher::dispatchI32x4Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_mul)); }
void OpcodeDispatcher::dispatchI32x4MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_min_s)); }
void OpcodeDispatcher::dispatchI32x4MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_min_u)); }
void OpcodeDispatcher::dispatchI32x4MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_max_s)); }
void OpcodeDispatcher::dispatchI32x4MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_max_u)); }
void OpcodeDispatcher::dispatchI32x4DotI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_dot_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI64x2Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_abs)); }
void OpcodeDispatcher::dispatchI64x2Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_neg)); }
void OpcodeDispatcher::dispatchI64x2AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i64x2_all_true)); }
void OpcodeDispatcher::dispatchI64x2Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i64x2_bitmask)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_high_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shl)); }
void OpcodeDispatcher::dispatchI64x2ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shr_s)); }
void OpcodeDispatcher::dispatchI64x2ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shr_u)); }
void OpcodeDispatcher::dispatchI64x2Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_add)); }
void OpcodeDispatcher::dispatchI64x2Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_sub)); }
void OpcodeDispatcher::dispatchI64x2Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_mul)); }
void OpcodeDispatcher::dispatchI64x2Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_eq)); }
void OpcodeDispatcher::dispatchI64x2Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_ne)); }
void OpcodeDispatcher::dispatchI64x2LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_lt_s)); }
void OpcodeDispatcher::dispatchI64x2GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_gt_s)); }
void OpcodeDispatcher::dispatchI64x2LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_le_s)); }
void OpcodeDispatcher::dispatchI64x2GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_ge_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_high_i32x4_u)); }
void OpcodeDispatcher::dispatchF32x4Ceil() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_ceil)); }
void OpcodeDispatcher::dispatchF32x4Floor() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_floor)); }
void OpcodeDispatcher::dispatchF32x4Trunc() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_trunc)); }
void OpcodeDispatcher::dispatchF32x4Nearest() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_nearest)); }
void OpcodeDispatcher::dispatchF32x4Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_abs)); }
void OpcodeDispatcher::dispatchF32x4Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_neg)); }
void OpcodeDispatcher::dispatchF32x4Sqrt() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_sqrt)); }
void OpcodeDispatcher::dispatchF32x4Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_add)); }
void OpcodeDispatcher::dispatchF32x4Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_sub)); }
void OpcodeDispatcher::dispatchF32x4Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_mul)); }
void OpcodeDispatcher::dispatchF32x4Div() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_div)); }
void OpcodeDispatcher::dispatchF32x4Min() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_min)); }
void OpcodeDispatcher::dispatchF32x4Max() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_max)); }
void OpcodeDispatcher::dispatchF32x4PMin() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_pmin)); }
void OpcodeDispatcher::dispatchF32x4PMax() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_pmax)); }
void OpcodeDispatcher::dispatchF64x2Ceil() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_ceil)); }
void OpcodeDispatcher::dispatchF64x2Floor() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_floor)); }
void OpcodeDispatcher::dispatchF64x2Trunc() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_trunc)); }
void OpcodeDispatcher::dispatchF64x2Nearest() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_nearest)); }
void OpcodeDispatcher::dispatchF64x2Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_abs)); }
void OpcodeDispatcher::dispatchF64x2Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_neg)); }
void OpcodeDispatcher::dispatchF64x2Sqrt() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_sqrt)); }
void OpcodeDispatcher::dispatchF64x2Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_add)); }
void OpcodeDispatcher::dispatchF64x2Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_sub)); }
void OpcodeDispatcher::dispatchF64x2Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_mul)); }
void OpcodeDispatcher::dispatchF64x2Div() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_div)); }
void OpcodeDispatcher::dispatchF64x2Min() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_min)); }
void OpcodeDispatcher::dispatchF64x2Max() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_max)); }
void OpcodeDispatcher::dispatchF64x2PMin() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_pmin)); }
void OpcodeDispatcher::dispatchF64x2PMax() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_pmax)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_trunc_sat_f32x4_s)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_trunc_sat_f32x4_u)); }
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_convert_i32x4_s)); }
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_convert_i32x4_u)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_trunc_sat_f64x2_s_zero)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_trunc_sat_f64x2_u_zero)); }
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_convert_low_i32x4_s)); }
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_convert_low_i32x4_u)); }
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f32x4_demote_f64x2_zero)); }
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_f64x2_promote_low_f32x4)); }
void OpcodeDispatcher::dispatchI8x16RelaxedSwizzle() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_relaxed_swizzle)); }
void OpcodeDispatcher::dispatchI32x4RelaxedTruncF32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_trunc_f32x4_s)); }
void OpcodeDispatcher::dispatchI32x4RelaxedTruncF32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_trunc_f32x4_u)); }
void OpcodeDispatcher::dispatchI32x4RelaxedTruncF64x2SZero() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_trunc_f64x2_s_zero)); }
void OpcodeDispatcher::dispatchI32x4RelaxedTruncF64x2UZero() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_trunc_f64x2_u_zero)); }
void OpcodeDispatcher::dispatchF32x4RelaxedMadd() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_f32x4_relaxed_madd)); }
void OpcodeDispatcher::dispatchF32x4RelaxedNmadd() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_f32x4_relaxed_nmadd)); }
void OpcodeDispatcher::dispatchF64x2RelaxedMadd() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_f64x2_relaxed_madd)); }
void OpcodeDispatcher::dispatchF64x2RelaxedNmadd() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_f64x2_relaxed_nmadd)); }
void OpcodeDispatcher::dispatchI8x16RelaxedLaneselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_i8x16_relaxed_laneselect)); }
void OpcodeDispatcher::dispatchI16x8RelaxedLaneselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_i16x8_relaxed_laneselect)); }
void OpcodeDispatcher::dispatchI32x4RelaxedLaneselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_laneselect)); }
void OpcodeDispatcher::dispatchI64x2RelaxedLaneselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_i64x2_relaxed_laneselect)); }
void OpcodeDispatcher::dispatchF32x4RelaxedMin() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_relaxed_min)); }
void OpcodeDispatcher::dispatchF32x4RelaxedMax() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f32x4_relaxed_max)); }
void OpcodeDispatcher::dispatchF64x2RelaxedMin() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_relaxed_min)); }
void OpcodeDispatcher::dispatchF64x2RelaxedMax() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_f64x2_relaxed_max)); }
void OpcodeDispatcher::dispatchI16x8RelaxedQ15MulRS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_relaxed_q15mulr_s)); }
void OpcodeDispatcher::dispatchI16x8RelaxedDotI8x16I7x16S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_relaxed_dot_i8x16_i7x16_s)); }
void OpcodeDispatcher::dispatchI32x4RelaxedDotI8x16I7x16AddS() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s)); }
// ── 0xFE atomics (docs/proposals/THREADS.md TH-1) ──────────────────────────
//
// Every access reuses `effectiveMemoryAddress`, so the memory index, the
// unsigned address widening, the wrap-safe bounds check and the allocation
// budget behave exactly as for a scalar load/store. The shared helpers in
// WasmBase (wasm_atomic.hpp) add natural-alignment trapping and perform the
// `__atomic_*` access at seq_cst; here we only adapt the value types.
// wait/notify are the single-agent form (TH-2 / TD9).

jit_value_t OpcodeDispatcher::atomicUintConstant(uint32_t value)
{
	return jit_value_create_nint_constant(function, jit_type_uint,
static_cast<jit_nint>(value));
}

jit_value_t OpcodeDispatcher::callNativeHelper(void* fptr, jit_type_t resultType,
											   std::vector<jit_type_t> paramTypes,
											   std::vector<jit_value_t> args)
{
	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, resultType,
			paramTypes.data(), static_cast<unsigned>(paramTypes.size()), 1);
	jit_value_t result = jit_insn_call_native(function, "wasm.helper", fptr, signature,
			args.data(), static_cast<unsigned>(args.size()), 0);
	// Helpers report a trap and return; the check turns that into an early
	// return before the caller uses the (zeroed) result.
	emitPendingTrapCheck();
	return result;
}

jit_value_t OpcodeDispatcher::emitAtomicLoad(const WASM::MemArg& ma, uint32_t width,
											 jit_type_t resultType)
{
	jit_value_t address = effectiveMemoryAddress(ma, static_cast<jit_nint>(width));
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_load),
			jit_type_ulong, {jit_type_void_ptr, jit_type_uint, jit_type_uint},
			{address, atomicUintConstant(width), atomicUintConstant(ma.ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

void OpcodeDispatcher::emitAtomicStore(const WASM::MemArg& ma, uint32_t width)
{
	// The value is on top of the stack, the address below it.
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = effectiveMemoryAddress(ma, static_cast<jit_nint>(width));
	callNativeHelper(reinterpret_cast<void*>(wasm_atomic_store), jit_type_void,
			{jit_type_void_ptr, jit_type_ulong, jit_type_uint, jit_type_uint},
			{address, value, atomicUintConstant(width), atomicUintConstant(ma.ordering)});
}

jit_value_t OpcodeDispatcher::emitAtomicRmw(const WASM::MemArg& ma, uint32_t width,
											uint32_t op, jit_type_t resultType)
{
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = effectiveMemoryAddress(ma, static_cast<jit_nint>(width));
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_rmw),
			jit_type_ulong,
			{jit_type_void_ptr, jit_type_ulong, jit_type_uint, jit_type_uint, jit_type_uint},
			{address, value, atomicUintConstant(width), atomicUintConstant(op),
			 atomicUintConstant(ma.ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

jit_value_t OpcodeDispatcher::emitAtomicCmpxchg(const WASM::MemArg& ma, uint32_t width,
											   jit_type_t resultType)
{
	// Stack: address, expected, replacement (replacement on top).
	jit_value_t replacement = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = effectiveMemoryAddress(ma, static_cast<jit_nint>(width));
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_cmpxchg),
			jit_type_ulong,
			{jit_type_void_ptr, jit_type_ulong, jit_type_ulong, jit_type_uint, jit_type_uint},
			{address, expected, replacement, atomicUintConstant(width),
			 atomicUintConstant(ma.ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

// ── Shared-everything threads: address-based atomic primitives (SET-3) ──────
//
// The global/table/GC forms share one lowering: compute the field/slot byte
// address, then call the same alignment-checked `__atomic_*` helpers the
// linear-memory atomics use. Reference slots are pointer-sized; the proposal
// requires them to be tear-free, and a naturally aligned pointer exchange is.

jit_value_t OpcodeDispatcher::emitAtomicLoadAddress(jit_value_t address, uint32_t width,
													uint8_t ordering, jit_type_t resultType)
{
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_load),
			jit_type_ulong, {jit_type_void_ptr, jit_type_uint, jit_type_uint},
			{address, atomicUintConstant(width), atomicUintConstant(ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

void OpcodeDispatcher::emitAtomicStoreAddress(jit_value_t address, uint32_t width,
											  uint8_t ordering)
{
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	emitAtomicStoreRaw(address, value, width, ordering);
}

void OpcodeDispatcher::emitAtomicStoreRaw(jit_value_t address, jit_value_t value,
										  uint32_t width, uint8_t ordering)
{
	callNativeHelper(reinterpret_cast<void*>(wasm_atomic_store), jit_type_void,
			{jit_type_void_ptr, jit_type_ulong, jit_type_uint, jit_type_uint},
			{address, value, atomicUintConstant(width), atomicUintConstant(ordering)});
}

jit_value_t OpcodeDispatcher::emitAtomicRmwAddress(jit_value_t address, uint32_t width,
												   uint32_t op, uint8_t ordering,
												   jit_type_t resultType)
{
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	return emitAtomicRmwRaw(address, value, width, op, ordering, resultType);
}

jit_value_t OpcodeDispatcher::emitAtomicRmwRaw(jit_value_t address, jit_value_t value,
											   uint32_t width, uint32_t op,
											   uint8_t ordering, jit_type_t resultType)
{
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_rmw),
			jit_type_ulong,
			{jit_type_void_ptr, jit_type_ulong, jit_type_uint, jit_type_uint, jit_type_uint},
			{address, value, atomicUintConstant(width), atomicUintConstant(op),
			 atomicUintConstant(ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

jit_value_t OpcodeDispatcher::emitAtomicCmpxchgAddress(jit_value_t address, uint32_t width,
													   uint8_t ordering, jit_type_t resultType)
{
	jit_value_t replacement = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	return emitAtomicCmpxchgRaw(address, expected, replacement, width, ordering, resultType);
}

jit_value_t OpcodeDispatcher::emitAtomicCmpxchgRaw(jit_value_t address, jit_value_t expected,
												   jit_value_t replacement, uint32_t width,
												   uint8_t ordering, jit_type_t resultType)
{
	jit_value_t raw = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_cmpxchg),
			jit_type_ulong,
			{jit_type_void_ptr, jit_type_ulong, jit_type_ulong, jit_type_uint, jit_type_uint},
			{address, expected, replacement, atomicUintConstant(width),
			 atomicUintConstant(ordering)});
	return jit_insn_convert(function, raw, resultType, 0);
}

jit_value_t OpcodeDispatcher::tableAtomicSlotAddress(WASM::TableIdx tableIdx)
{
	jit_value_t index = popValue();
	jit_value_t table = tablePointerForIndex(tableIdx);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_value_t checked = checkedTableIndex(table, index, "shared atomic table access",
		WASM::TrapKind::OutOfBoundsTableAccess);
	return jit_insn_load_elem_address(function, tablePtr, checked, jit_type_void_ptr);
}

jit_value_t OpcodeDispatcher::structAtomicFieldAddress(WASM::TypeIdx typeIdx, uint32_t fieldIdx)
{
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	return jit_insn_add(function, refAsVoidPtr(ref),
		jit_value_create_nint_constant(function, jit_type_nint,
									   structFieldOffset(typeIdx, fieldIdx)));
}

jit_value_t OpcodeDispatcher::arrayAtomicElementAddress(WASM::TypeIdx typeIdx)
{
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t length = jit_insn_load_relative(function, refAsVoidPtr(ref),
		arrayLengthOffset(typeIdx), jit_type_uint);
	jit_value_t inBounds = jit_insn_lt(function, index, length);
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &ok);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &ok);
	jit_value_t data = arrayElementBase(ref, typeIdx);
	return jit_insn_load_elem_address(function, data, index, arrayElementJitType(typeIdx));
}

void OpcodeDispatcher::dispatchAtomicFence(uint8_t ordering) {
	callNativeHelper(reinterpret_cast<void*>(wasm_atomic_fence), jit_type_void,
			{jit_type_uint}, {atomicUintConstant(ordering)});
}

void OpcodeDispatcher::dispatchAtomicPause() {
	// Acquire-release proposal: a spin-loop hint with no architectural effect.
	// Lowering it to nothing is explicitly permitted (ARAD2).
}

// ── Shared-everything threads (SET-3): global/table/GC atomics ──────────────
// The ordering byte is passed through to the same `__atomic_*` helpers as the
// linear-memory atomics; address formation is the only new part. A reference
// slot is pointer-sized and naturally aligned.

static uint32_t sharedAtomicWidth(WASM::ValueTypeCode code)
{
	switch (code) {
	case WASM::ValueTypeCode::I32:
	case WASM::ValueTypeCode::F32: return 4;
	case WASM::ValueTypeCode::I64:
	case WASM::ValueTypeCode::F64: return 8;
	case WASM::ValueTypeCode::V128: return 16;
	default: return static_cast<uint32_t>(sizeof(void*)); // references
	}
}

static uint32_t sharedAtomicWidth(const WASM::CanonStorageType& storage)
{
	if (storage.isPacked)
		return storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u;
	return sharedAtomicWidth(storage.value.opcode);
}

void OpcodeDispatcher::dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) {
	const WASM::GlobalType gt = globalTypeForIndex(arg);
	pushValue(emitAtomicLoadAddress(globalSlotAddress(arg),
		sharedAtomicWidth(gt.contentType.opcode), ordering,
		jitTypeForValueType(gt.contentType)));
}

void OpcodeDispatcher::dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) {
	const WASM::GlobalType gt = globalTypeForIndex(arg);
	emitAtomicStoreAddress(globalSlotAddress(arg),
		sharedAtomicWidth(gt.contentType.opcode), ordering);
}

#define WJ_SHARED_GLOBAL_RMW(n, op) \
void OpcodeDispatcher::dispatchGlobalAtomicRmw##n(uint8_t ordering, WASM::GlobalIdx arg) { \
	const WASM::GlobalType gt = globalTypeForIndex(arg); \
	pushValue(emitAtomicRmwAddress(globalSlotAddress(arg), \
		sharedAtomicWidth(gt.contentType.opcode), op, ordering, \
		jitTypeForValueType(gt.contentType))); \
}
WJ_SHARED_GLOBAL_RMW(Add, WASM_ATOMIC_ADD)
WJ_SHARED_GLOBAL_RMW(Sub, WASM_ATOMIC_SUB)
WJ_SHARED_GLOBAL_RMW(And, WASM_ATOMIC_AND)
WJ_SHARED_GLOBAL_RMW(Or, WASM_ATOMIC_OR)
WJ_SHARED_GLOBAL_RMW(Xor, WASM_ATOMIC_XOR)
WJ_SHARED_GLOBAL_RMW(Xchg, WASM_ATOMIC_XCHG)
#undef WJ_SHARED_GLOBAL_RMW

void OpcodeDispatcher::dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) {
	const WASM::GlobalType gt = globalTypeForIndex(arg);
	pushValue(emitAtomicCmpxchgAddress(globalSlotAddress(arg),
		sharedAtomicWidth(gt.contentType.opcode), ordering,
		jitTypeForValueType(gt.contentType)));
}

void OpcodeDispatcher::dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) {
	pushValue(emitAtomicLoadAddress(tableAtomicSlotAddress(arg), sizeof(void*), ordering,
									tableElementJitType(arg)));
}
void OpcodeDispatcher::dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) {
	// Stack: index, value. Pop the value before the address helper consumes the
	// index (the value is on top).
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = tableAtomicSlotAddress(arg);
	emitAtomicStoreRaw(address, value, sizeof(void*), ordering);
}
void OpcodeDispatcher::dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) {
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = tableAtomicSlotAddress(arg);
	pushValue(emitAtomicRmwRaw(address, value, sizeof(void*), WASM_ATOMIC_XCHG, ordering,
							   tableElementJitType(arg)));
}
void OpcodeDispatcher::dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) {
	jit_value_t replacement = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = tableAtomicSlotAddress(arg);
	pushValue(emitAtomicCmpxchgRaw(address, expected, replacement, sizeof(void*), ordering,
								   tableElementJitType(arg)));
}

void OpcodeDispatcher::dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) {
	const auto field = structTypeForIndex(arg1).structFields()[arg2];
	jit_value_t address = structAtomicFieldAddress(arg1, arg2);
	pushValue(emitAtomicLoadAddress(address, sharedAtomicWidth(field.storage), ordering,
								   typeTranslator.translateCanonStorage(field.storage)));
}
void OpcodeDispatcher::dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) {
	const auto field = structTypeForIndex(arg1).structFields()[arg2];
	jit_value_t address = structAtomicFieldAddress(arg1, arg2);
	jit_value_t raw = emitAtomicLoadAddress(address, sharedAtomicWidth(field.storage),
											ordering, jit_type_ulong);
	jit_type_t narrow = field.storage.packed == WASM::ValueTypeCode::I8
		? jit_type_sbyte : jit_type_short;
	pushValue(jit_insn_convert(function,
		jit_insn_convert(function, raw, narrow, 0), jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) {
	const auto field = structTypeForIndex(arg1).structFields()[arg2];
	jit_value_t address = structAtomicFieldAddress(arg1, arg2);
	pushValue(emitAtomicLoadAddress(address, sharedAtomicWidth(field.storage), ordering,
								   jit_type_int));
}
void OpcodeDispatcher::dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) {
	const auto field = structTypeForIndex(arg1).structFields()[arg2];
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = structAtomicFieldAddress(arg1, arg2);
	emitAtomicStoreRaw(address, value, sharedAtomicWidth(field.storage), ordering);
}
#define WJ_SHARED_STRUCT_RMW(n, op) \
void OpcodeDispatcher::dispatchStructAtomicRmw##n(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) { \
	const auto field = structTypeForIndex(arg1).structFields()[arg2]; \
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0); \
	jit_value_t address = structAtomicFieldAddress(arg1, arg2); \
	pushValue(emitAtomicRmwRaw(address, value, sharedAtomicWidth(field.storage), op, ordering, \
							   typeTranslator.translateCanonStorage(field.storage))); \
}
WJ_SHARED_STRUCT_RMW(Add, WASM_ATOMIC_ADD)
WJ_SHARED_STRUCT_RMW(Sub, WASM_ATOMIC_SUB)
WJ_SHARED_STRUCT_RMW(And, WASM_ATOMIC_AND)
WJ_SHARED_STRUCT_RMW(Or, WASM_ATOMIC_OR)
WJ_SHARED_STRUCT_RMW(Xor, WASM_ATOMIC_XOR)
WJ_SHARED_STRUCT_RMW(Xchg, WASM_ATOMIC_XCHG)
#undef WJ_SHARED_STRUCT_RMW
void OpcodeDispatcher::dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) {
	const auto field = structTypeForIndex(arg1).structFields()[arg2];
	jit_value_t replacement = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = structAtomicFieldAddress(arg1, arg2);
	pushValue(emitAtomicCmpxchgRaw(address, expected, replacement,
								   sharedAtomicWidth(field.storage), ordering,
								   typeTranslator.translateCanonStorage(field.storage)));
}

void OpcodeDispatcher::dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) {
	const auto element = arrayTypeForIndex(arg).arrayElement();
	jit_value_t address = arrayAtomicElementAddress(arg);
	pushValue(emitAtomicLoadAddress(address, sharedAtomicWidth(element.storage), ordering,
								   typeTranslator.translateCanonStorage(element.storage)));
}
void OpcodeDispatcher::dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) {
	const auto element = arrayTypeForIndex(arg).arrayElement();
	jit_value_t address = arrayAtomicElementAddress(arg);
	jit_value_t raw = emitAtomicLoadAddress(address, sharedAtomicWidth(element.storage),
											ordering, jit_type_ulong);
	jit_type_t narrow = element.storage.packed == WASM::ValueTypeCode::I8
		? jit_type_sbyte : jit_type_short;
	pushValue(jit_insn_convert(function,
		jit_insn_convert(function, raw, narrow, 0), jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) {
	const auto element = arrayTypeForIndex(arg).arrayElement();
	jit_value_t address = arrayAtomicElementAddress(arg);
	pushValue(emitAtomicLoadAddress(address, sharedAtomicWidth(element.storage), ordering,
								   jit_type_int));
}
void OpcodeDispatcher::dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) {
	const auto element = arrayTypeForIndex(arg).arrayElement();
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = arrayAtomicElementAddress(arg);
	emitAtomicStoreRaw(address, value, sharedAtomicWidth(element.storage), ordering);
}
#define WJ_SHARED_ARRAY_RMW(n, op) \
void OpcodeDispatcher::dispatchArrayAtomicRmw##n(uint8_t ordering, WASM::TypeIdx arg) { \
	const auto element = arrayTypeForIndex(arg).arrayElement(); \
	jit_value_t value = jit_insn_convert(function, popValue(), jit_type_ulong, 0); \
	jit_value_t address = arrayAtomicElementAddress(arg); \
	pushValue(emitAtomicRmwRaw(address, value, sharedAtomicWidth(element.storage), op, ordering, \
							   typeTranslator.translateCanonStorage(element.storage))); \
}
WJ_SHARED_ARRAY_RMW(Add, WASM_ATOMIC_ADD)
WJ_SHARED_ARRAY_RMW(Sub, WASM_ATOMIC_SUB)
WJ_SHARED_ARRAY_RMW(And, WASM_ATOMIC_AND)
WJ_SHARED_ARRAY_RMW(Or, WASM_ATOMIC_OR)
WJ_SHARED_ARRAY_RMW(Xor, WASM_ATOMIC_XOR)
WJ_SHARED_ARRAY_RMW(Xchg, WASM_ATOMIC_XCHG)
#undef WJ_SHARED_ARRAY_RMW
void OpcodeDispatcher::dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) {
	const auto element = arrayTypeForIndex(arg).arrayElement();
	jit_value_t replacement = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = arrayAtomicElementAddress(arg);
	pushValue(emitAtomicCmpxchgRaw(address, expected, replacement,
								   sharedAtomicWidth(element.storage), ordering,
								   typeTranslator.translateCanonStorage(element.storage)));
}

void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg) {
	jit_value_t count = popValue();
	jit_value_t address = effectiveMemoryAddress(arg, 4); // 4-byte access size
	jit_value_t result = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_notify),
			jit_type_uint, {jit_type_void_ptr, jit_type_uint}, {address, count});
	pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg) {
	jit_value_t timeout = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	// The expected operand is a 32-bit bit pattern: zero-extend it, or a
	// negative expectation would not compare equal to the loaded bytes.
	jit_value_t expected = jit_insn_convert(function,
			jit_insn_convert(function, popValue(), jit_type_uint, 0), jit_type_ulong, 0);
	jit_value_t address = effectiveMemoryAddress(arg, 4);
	jit_value_t result = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_wait),
			jit_type_uint,
			{jit_type_void_ptr, jit_type_ulong, jit_type_ulong, jit_type_uint},
			{address, expected, timeout, atomicUintConstant(4)});
	pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg) {
	jit_value_t timeout = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t expected = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t address = effectiveMemoryAddress(arg, 8);
	jit_value_t result = callNativeHelper(reinterpret_cast<void*>(wasm_atomic_wait),
			jit_type_uint,
			{jit_type_void_ptr, jit_type_ulong, jit_type_ulong, jit_type_uint},
			{address, expected, timeout, atomicUintConstant(8)});
	pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}

// One line per opcode. The macro list keeps the 67 handlers auditable against
// `AtomicOpcode` (WasmBase/WasmOpcode.hpp) and impossible to drift from the
// declarations in WasmOpcodeDispatcher.hpp.
#define WASM_ATOMIC_LOADS(X) \
	X(I32AtomicLoad,     4, jit_type_int) \
	X(I64AtomicLoad,     8, jit_type_long) \
	X(I32AtomicLoad8U,   1, jit_type_int) \
	X(I32AtomicLoad16U,  2, jit_type_int) \
	X(I64AtomicLoad8U,   1, jit_type_long) \
	X(I64AtomicLoad16U,  2, jit_type_long) \
	X(I64AtomicLoad32U,  4, jit_type_long)
#define WASM_ATOMIC_LOAD_IMPL(name, width, jt) \
	void OpcodeDispatcher::dispatch##name(WASM::MemArg arg) { \
		pushValue(emitAtomicLoad(arg, width, jt)); \
	}
WASM_ATOMIC_LOADS(WASM_ATOMIC_LOAD_IMPL)
#undef WASM_ATOMIC_LOAD_IMPL
#undef WASM_ATOMIC_LOADS

#define WASM_ATOMIC_STORES(X) \
	X(I32AtomicStore,    4) \
	X(I64AtomicStore,    8) \
	X(I32AtomicStore8,   1) \
	X(I32AtomicStore16,  2) \
	X(I64AtomicStore8,   1) \
	X(I64AtomicStore16,  2) \
	X(I64AtomicStore32,  4)
#define WASM_ATOMIC_STORE_IMPL(name, width) \
	void OpcodeDispatcher::dispatch##name(WASM::MemArg arg) { emitAtomicStore(arg, width); }
WASM_ATOMIC_STORES(WASM_ATOMIC_STORE_IMPL)
#undef WASM_ATOMIC_STORE_IMPL
#undef WASM_ATOMIC_STORES

#define WASM_ATOMIC_RMWS(X) \
	X(I32AtomicRmwAdd,         4, jit_type_int,  WASM_ATOMIC_ADD) \
	X(I64AtomicRmwAdd,         8, jit_type_long, WASM_ATOMIC_ADD) \
	X(I32AtomicRmw8AddU,       1, jit_type_int,  WASM_ATOMIC_ADD) \
	X(I32AtomicRmw16AddU,      2, jit_type_int,  WASM_ATOMIC_ADD) \
	X(I64AtomicRmw8AddU,       1, jit_type_long, WASM_ATOMIC_ADD) \
	X(I64AtomicRmw16AddU,      2, jit_type_long, WASM_ATOMIC_ADD) \
	X(I64AtomicRmw32AddU,      4, jit_type_long, WASM_ATOMIC_ADD) \
	X(I32AtomicRmwSub,         4, jit_type_int,  WASM_ATOMIC_SUB) \
	X(I64AtomicRmwSub,         8, jit_type_long, WASM_ATOMIC_SUB) \
	X(I32AtomicRmw8SubU,       1, jit_type_int,  WASM_ATOMIC_SUB) \
	X(I32AtomicRmw16SubU,      2, jit_type_int,  WASM_ATOMIC_SUB) \
	X(I64AtomicRmw8SubU,       1, jit_type_long, WASM_ATOMIC_SUB) \
	X(I64AtomicRmw16SubU,      2, jit_type_long, WASM_ATOMIC_SUB) \
	X(I64AtomicRmw32SubU,      4, jit_type_long, WASM_ATOMIC_SUB) \
	X(I32AtomicRmwAnd,         4, jit_type_int,  WASM_ATOMIC_AND) \
	X(I64AtomicRmwAnd,         8, jit_type_long, WASM_ATOMIC_AND) \
	X(I32AtomicRmw8AndU,       1, jit_type_int,  WASM_ATOMIC_AND) \
	X(I32AtomicRmw16AndU,      2, jit_type_int,  WASM_ATOMIC_AND) \
	X(I64AtomicRmw8AndU,       1, jit_type_long, WASM_ATOMIC_AND) \
	X(I64AtomicRmw16AndU,      2, jit_type_long, WASM_ATOMIC_AND) \
	X(I64AtomicRmw32AndU,      4, jit_type_long, WASM_ATOMIC_AND) \
	X(I32AtomicRmwOr,          4, jit_type_int,  WASM_ATOMIC_OR) \
	X(I64AtomicRmwOr,          8, jit_type_long, WASM_ATOMIC_OR) \
	X(I32AtomicRmw8OrU,        1, jit_type_int,  WASM_ATOMIC_OR) \
	X(I32AtomicRmw16OrU,       2, jit_type_int,  WASM_ATOMIC_OR) \
	X(I64AtomicRmw8OrU,        1, jit_type_long, WASM_ATOMIC_OR) \
	X(I64AtomicRmw16OrU,       2, jit_type_long, WASM_ATOMIC_OR) \
	X(I64AtomicRmw32OrU,       4, jit_type_long, WASM_ATOMIC_OR) \
	X(I32AtomicRmwXor,         4, jit_type_int,  WASM_ATOMIC_XOR) \
	X(I64AtomicRmwXor,         8, jit_type_long, WASM_ATOMIC_XOR) \
	X(I32AtomicRmw8XorU,       1, jit_type_int,  WASM_ATOMIC_XOR) \
	X(I32AtomicRmw16XorU,      2, jit_type_int,  WASM_ATOMIC_XOR) \
	X(I64AtomicRmw8XorU,       1, jit_type_long, WASM_ATOMIC_XOR) \
	X(I64AtomicRmw16XorU,      2, jit_type_long, WASM_ATOMIC_XOR) \
	X(I64AtomicRmw32XorU,      4, jit_type_long, WASM_ATOMIC_XOR) \
	X(I32AtomicRmwXchg,        4, jit_type_int,  WASM_ATOMIC_XCHG) \
	X(I64AtomicRmwXchg,        8, jit_type_long, WASM_ATOMIC_XCHG) \
	X(I32AtomicRmw8XchgU,      1, jit_type_int,  WASM_ATOMIC_XCHG) \
	X(I32AtomicRmw16XchgU,     2, jit_type_int,  WASM_ATOMIC_XCHG) \
	X(I64AtomicRmw8XchgU,      1, jit_type_long, WASM_ATOMIC_XCHG) \
	X(I64AtomicRmw16XchgU,     2, jit_type_long, WASM_ATOMIC_XCHG) \
	X(I64AtomicRmw32XchgU,     4, jit_type_long, WASM_ATOMIC_XCHG)
#define WASM_ATOMIC_RMW_IMPL(name, width, jt, op) \
	void OpcodeDispatcher::dispatch##name(WASM::MemArg arg) { \
		pushValue(emitAtomicRmw(arg, width, op, jt)); \
	}
WASM_ATOMIC_RMWS(WASM_ATOMIC_RMW_IMPL)
#undef WASM_ATOMIC_RMW_IMPL
#undef WASM_ATOMIC_RMWS

#define WASM_ATOMIC_CMPXCHGS(X) \
	X(I32AtomicRmwCmpxchg,     4, jit_type_int) \
	X(I64AtomicRmwCmpxchg,     8, jit_type_long) \
	X(I32AtomicRmw8CmpxchgU,   1, jit_type_int) \
	X(I32AtomicRmw16CmpxchgU,  2, jit_type_int) \
	X(I64AtomicRmw8CmpxchgU,   1, jit_type_long) \
	X(I64AtomicRmw16CmpxchgU,  2, jit_type_long) \
	X(I64AtomicRmw32CmpxchgU,  4, jit_type_long)
#define WASM_ATOMIC_CMPXCHG_IMPL(name, width, jt) \
	void OpcodeDispatcher::dispatch##name(WASM::MemArg arg) { \
		pushValue(emitAtomicCmpxchg(arg, width, jt)); \
	}
WASM_ATOMIC_CMPXCHGS(WASM_ATOMIC_CMPXCHG_IMPL)
#undef WASM_ATOMIC_CMPXCHG_IMPL
#undef WASM_ATOMIC_CMPXCHGS

}
