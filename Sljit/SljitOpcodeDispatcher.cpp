#include "SljitOpcodeDispatcher.hpp"

#include <cstdio>

#include "SljitModuleCompiler.hpp"

#include "../WasmBase/WasmBackendAbi.hpp"
#include "../WasmBase/WasmTailTrampoline.hpp"
#include "../WasmBase/WasmExn.hpp"
#include "../WasmBase/wasm_atomic.hpp"
#include "../WasmBase/WasmMemory.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace Sljit {

namespace {

//! Thrown internally to abandon one function; never leaves this translation unit.
struct UnsupportedBody {
};

//! Every frame slot is one 16-byte cell. Scalars use the low word; the extra
//! room is what lets a `v128` operand and a scalar share one addressing rule
//! (a `v128` is the whole cell, a scalar its first word).
constexpr std::size_t kSlotBytes = 16;

//! Reserved frame slots that follow the locals and precede the operand stack.
//! Reserved frame cells: context, target, scratch, then the two-word selector
//! block `i8x16.shuffle` passes to its adapter.
//! SLJIT's own ceiling on a function's argument list: four entries, of which the
//! first is the `VMContext*`. `function_check_arguments` in `sljitLir.c:924`
//! rejects the fifth with a hard `return 0`, and letting it do so aborts the
//! process rather than failing the compile.
constexpr sljit_s32 kArgumentSlotCeiling = 4;

constexpr std::size_t kReservedSlots = 10; //!< context, target, scratch, four parked argument registers, the result buffer, the frame, and the exception being routed.

//! Width of a local in bytes, or 0 when the type is outside the supported subset.
bool isReferenceKind(WASM::ValueTypeCode kind) noexcept
{
	switch (kind) {
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
	case WASM::ValueTypeCode::Ref:
	case WASM::ValueTypeCode::RefNull:
		return true;
	default:
		return false;
	}
}

int widthOf(const WASM::ValueType& type)
{
	switch (type.opcode) {
	case WASM::ValueTypeCode::I32:
	case WASM::ValueTypeCode::F32:
		return 4;
	case WASM::ValueTypeCode::I64:
	case WASM::ValueTypeCode::F64:
		return 8;
	case WASM::ValueTypeCode::V128:
		return 16;
	default:
		// A reference is one machine word in a slot.
		return isReferenceKind(type.opcode) ? 8 : 0;
	}
}

//! True for the two floating-point value types.
bool isFloatKind(WASM::ValueTypeCode kind) noexcept
{
	return kind == WASM::ValueTypeCode::F32 || kind == WASM::ValueTypeCode::F64;
}

//! SLJIT argument-type token for one declared value kind.
sljit_s32 argTypeOf(WASM::ValueTypeCode kind) noexcept
{
	switch (kind) {
	case WASM::ValueTypeCode::F32: return SLJIT_ARG_TYPE_F32;
	case WASM::ValueTypeCode::F64: return SLJIT_ARG_TYPE_F64;
	case WASM::ValueTypeCode::I32: return SLJIT_ARG_TYPE_32;
	default: return SLJIT_ARG_TYPE_W;
	}
}

//! A function pointer as the integer operand SLJIT takes for a direct call.
template <typename Fn>
sljit_sw codeAddress(Fn fn)
{
	return reinterpret_cast<sljit_sw>(fn);
}

//! C entry point for the pending-trap probe. Returns a machine word so the
//! comparison in generated code does not depend on how a C `int` return is
//! extended by the host compiler.
extern "C" sljit_sw sljitTrapPending()
{
	return WASM::trapPending() ? 1 : 0;
}

// ── Exception helpers (docs/proposals/EXCEPTIONS.md EX-3/EXD1) ───────────────
// The pending exception is part of the execution state, like the trap slot.
// These wrappers keep the generated call sites to a fixed SLJIT argument shape.

//! Reads the execution state's pending exception, or null.
extern "C" void* sljitExnPending() noexcept
{
	return ::wasm_exn_pending();
}

//! Allocates a Store-owned exception object and returns its payload area.
extern "C" void* sljitExnCreate(WASM::VMContext* vm, int32_t tagId, uint32_t payloadBytes) noexcept
{
	return ::wasm_exn_create(vm, tagId, payloadBytes);
}

//! Leaves an exception pending for the caller (and the embedder).
extern "C" void sljitExnSetPending(void* exn) noexcept
{
	::wasm_exn_set_pending(exn);
}

//! Clears the pending slot when a catch consumes the exception.
extern "C" void sljitExnClearPending() noexcept
{
	::wasm_exn_clear_pending();
}

//! Reads the canonical tag id out of an exception object.
extern "C" int32_t sljitExnTagId(const void* exn) noexcept
{
	return ::wasm_exn_tag_id(exn);
}


// ── Atomic helpers (docs/proposals/THREADS.md TH-1) ────────────────────────
// SLJIT emits at most four C arguments, so the wider helper signatures are
// packed into one 32-bit word here. The unpacking is the only difference from
// the ABI the LibJIT backend calls directly.

extern "C" uint64_t sljitAtomicRmw(void* address, uint64_t value, uint32_t packed) noexcept
{
	return ::wasm_atomic_rmw(address, value, packed & 0xFFu,
			(packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
}

extern "C" uint64_t sljitAtomicCmpxchg(void* address, uint64_t expected, uint64_t replacement,
		uint32_t packed) noexcept
{
	return ::wasm_atomic_cmpxchg(address, expected, replacement, packed & 0xFFu,
			(packed >> 8) & 0xFFu);
}

extern "C" uint32_t sljitAtomicWait(const void* address, uint64_t expected,
		uint64_t timeoutNs, uint32_t width) noexcept
{
	return ::wasm_atomic_wait(address, expected, timeoutNs, width);
}

//! Payload layout of one tag: the canonical aggregate layout the exception ABI
//! documents (`translateStruct` on LibJIT, `computeAggregateLayout` here).
struct ExceptionPayloadLayout {
	std::vector<WASM::FieldLayout> fields; //!< Offset/size/alignment per payload value.
	std::uint32_t size = 0; //!< Total payload bytes, rounded to its alignment.
};

//! Computes the payload layout of a tag's parameter list.
//! \param payload The tag's function type; its parameters are the payload.
//! \return The canonical field layout.
ExceptionPayloadLayout exceptionPayloadLayout(const WASM::FuncType& payload)
{
	ExceptionPayloadLayout layout;
	layout.fields.resize(payload.params.size());
	std::vector<WASM::ValueKind> kinds;
	kinds.reserve(payload.params.size());
	for (const WASM::StorageType& parameter : payload.params)
		kinds.push_back(WASM::valueKindOf(parameter.val.opcode));
	const WASM::AggregateLayout aggregate = WASM::computeAggregateLayout(kinds, layout.fields);
	layout.size = aggregate.size;
	return layout;
}


//! Reports the spec's `unreachable` trap without returning through a Wasm frame.
//! Runtime subtype test against an object-header type id.
//! \param vm Instance context.
//! \param actual Type id read from the object header.
//! \param expected Type id of the target type.
//! \return 1 when the object type is the target or a subtype of it, else 0.
extern "C" uint32_t sljitTypeIsSubtype(WASM::VMContext* vm, uint32_t actual,
		uint32_t expected) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->typeIdIsSubtype(actual, expected) ? 1u : 0u;
}

extern "C" uint32_t sljitTypeIdMatchesKind(WASM::VMContext* vm, uint32_t typeId,
		uint32_t kind) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->typeIdMatchesAbstractKind(typeId, kind) ? 1u : 0u;
}

extern "C" void* sljitArrayNew(WASM::VMContext* vm, uint32_t typeIndex, uint32_t length) noexcept
{
	// An exception must never escape a helper: `std::terminate` would kill the
	// host, and a trap is a returned status (docs/SAFETY.md §2.6).
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateArrayObject(typeIndex, length);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

//! Copies a data segment into an array allocated by `sljitArrayNew`.
//! \param vm Instance context.
//! \param array The array object.
//! \param typeIndex The array's module-local type index.
//! \param packedOffsetAndIndex `dataIndex << 32 | byteOffset`, packed because the
//!        helper ABI carries at most four arguments.
extern "C" void sljitArrayInitFromData(WASM::VMContext* vm, void* array, uint32_t typeIndex,
uint64_t packedOffsetAndIndex) noexcept
{
	// An exception must never escape a helper: `std::terminate` would kill the
	// host, and a trap is a returned status (docs/SAFETY.md §2.6).
	try {
	reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayInitFromData(array, typeIndex,
	static_cast<uint32_t>(packedOffsetAndIndex >> 32),
	static_cast<uint32_t>(packedOffsetAndIndex & 0xFFFFFFFFu));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

extern "C" void* sljitStructNew(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex) noexcept
{
	// An exception must never escape a helper: `std::terminate` would kill the
	// host, and a trap is a returned status (docs/SAFETY.md §2.6).
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

//! Reports a trap of @p kind through the shared trap slot.
//! @param kind The `WASM::TrapKind` enumerator, passed as an immediate.
//! The single entry point: the kind is an explicit argument at every call
//! site, so a wrong kind is visible in the lowering (docs §19, §47).
extern "C" void sljitReportTrap(uint32_t kind) noexcept
{
	WASM::reportTrap(static_cast<WASM::TrapKind>(kind));
}

extern "C" void sljitReportUnreachable() noexcept
{
	WASM::reportTrap(WASM::TrapKind::Unreachable);
}

extern "C" uint32_t sljitI32Popcnt(uint32_t value) noexcept
{
	value -= (value >> 1) & 0x55555555u;
	value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
	value = (value + (value >> 4)) & 0x0F0F0F0Fu;
	value += value >> 8;
	value += value >> 16;
	return value & 0x3Fu;
}

extern "C" uint64_t sljitI64Popcnt(uint64_t value) noexcept
{
	return static_cast<uint64_t>(__builtin_popcountll(value));
}

extern "C" int32_t sljitI32DivS(int32_t lhs, int32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT32_MIN && rhs == -1) {
		WASM::reportTrap(WASM::TrapKind::IntegerOverflow);
		return 0;
	}
	return lhs / rhs;
}

extern "C" uint32_t sljitI32DivU(uint32_t lhs, uint32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs / rhs;
}

extern "C" int32_t sljitI32RemS(int32_t lhs, int32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT32_MIN && rhs == -1)
		return 0;
	return lhs % rhs;
}

extern "C" uint32_t sljitI32RemU(uint32_t lhs, uint32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs % rhs;
}

extern "C" int64_t sljitI64DivS(int64_t lhs, int64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT64_MIN && rhs == -1) {
		WASM::reportTrap(WASM::TrapKind::IntegerOverflow);
		return 0;
	}
	return lhs / rhs;
}

extern "C" uint64_t sljitI64DivU(uint64_t lhs, uint64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs / rhs;
}

extern "C" int64_t sljitI64RemS(int64_t lhs, int64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT64_MIN && rhs == -1)
		return 0;
	return lhs % rhs;
}

extern "C" uint64_t sljitI64RemU(uint64_t lhs, uint64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs % rhs;
}

// ── Floating-point helpers ─────────────────────────────────────────────────
// The rules are the ones the LibJIT backend defines for this engine: min/max
// return a quiet NaN when either operand is NaN and are sign-of-zero aware;
// trunc/nearest quiet a NaN; nearest is ties-to-even.

extern "C" float sljitF32Min(float a, float b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) || std::signbit(b)) ? -0.0f : 0.0f;
	return a < b ? a : b;
}

extern "C" float sljitF32Max(float a, float b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) && std::signbit(b)) ? -0.0f : 0.0f;
	return a > b ? a : b;
}

extern "C" double sljitF64Min(double a, double b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) || std::signbit(b)) ? -0.0 : 0.0;
	return a < b ? a : b;
}

extern "C" double sljitF64Max(double a, double b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) && std::signbit(b)) ? -0.0 : 0.0;
	return a > b ? a : b;
}

extern "C" float sljitF32Sqrt(float x) noexcept { return std::sqrt(x); }
extern "C" float sljitF32Ceil(float x) noexcept { return std::ceil(x); }
extern "C" float sljitF32Floor(float x) noexcept { return std::floor(x); }

extern "C" float sljitF32Trunc(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::trunc(x);
}

extern "C" float sljitF32Nearest(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::nearbyint(x);
}

extern "C" double sljitF64Sqrt(double x) noexcept { return std::sqrt(x); }
extern "C" double sljitF64Ceil(double x) noexcept { return std::ceil(x); }
extern "C" double sljitF64Floor(double x) noexcept { return std::floor(x); }

extern "C" double sljitF64Trunc(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::trunc(x);
}

extern "C" double sljitF64Nearest(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::nearbyint(x);
}

//! The spec splits an unusable truncation into NaN and range failures.
void trapInvalidConversion() noexcept
{
	WASM::reportTrap(WASM::TrapKind::InvalidConversionToInteger);
}

void trapIntegerOverflow() noexcept
{
	WASM::reportTrap(WASM::TrapKind::IntegerOverflow);
}

extern "C" int32_t sljitI32TruncF32S(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -2147483649.0 && d < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}

extern "C" uint32_t sljitI32TruncF32U(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}

extern "C" int32_t sljitI32TruncF64S(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -2147483649.0 && x < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}

extern "C" uint32_t sljitI32TruncF64U(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}

extern "C" int64_t sljitI64TruncF32S(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}

extern "C" uint64_t sljitI64TruncF32U(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}

extern "C" int64_t sljitI64TruncF64S(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}

extern "C" uint64_t sljitI64TruncF64U(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}

// ── Saturating truncation (non-trapping proposal) ──────────────────────────
// These clamp instead of reporting; NaN maps to zero.

extern "C" int32_t sljitI32TruncSatF32S(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 2147483648.0) return INT32_MAX;
	if (d < -2147483648.0) return INT32_MIN;
	return static_cast<int32_t>(x);
}

extern "C" uint32_t sljitI32TruncSatF32U(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 4294967296.0) return UINT32_MAX;
	if (d <= -1.0) return 0;
	return static_cast<uint32_t>(x);
}

extern "C" int32_t sljitI32TruncSatF64S(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 2147483648.0) return INT32_MAX;
	if (x < -2147483648.0) return INT32_MIN;
	return static_cast<int32_t>(x);
}

extern "C" uint32_t sljitI32TruncSatF64U(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 4294967296.0) return UINT32_MAX;
	if (x <= -1.0) return 0;
	return static_cast<uint32_t>(x);
}

extern "C" int64_t sljitI64TruncSatF32S(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 9223372036854775808.0) return INT64_MAX;
	if (d < -9223372036854775808.0) return INT64_MIN;
	return static_cast<int64_t>(x);
}

extern "C" uint64_t sljitI64TruncSatF32U(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 18446744073709551616.0) return UINT64_MAX;
	if (d <= -1.0) return 0;
	return static_cast<uint64_t>(x);
}

extern "C" int64_t sljitI64TruncSatF64S(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 9223372036854775808.0) return INT64_MAX;
	if (x < -9223372036854775808.0) return INT64_MIN;
	return static_cast<int64_t>(x);
}

extern "C" uint64_t sljitI64TruncSatF64U(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 18446744073709551616.0) return UINT64_MAX;
	if (x <= -1.0) return 0;
	return static_cast<uint64_t>(x);
}


//! Checked address of a memory32 access. The immediate offset and the access
//! width travel packed in one word, because an SLJIT call carries at most four
//! integer arguments and `VMContext*` is one of them.

//! Turns the five-argument shuffle helper into the four-argument shape SLJIT's
//! call ABI supports. It contains no semantics: it only forwards to the shared
//! helper, which stays the single definition of `i8x16.shuffle`.
extern "C" void sljitShuffleAdapter(const void* a, const void* b,
		const uint64_t* selectors, void* out)
{
	wasm_i8x16_shuffle(a, b, selectors[0], selectors[1], out);
}

extern "C" void* sljitMemoryAddress(WASM::VMContext* vm, uint32_t memoryIndex,
		uint32_t address, uint64_t offsetAndWidth) noexcept
{
	const uint32_t offset = static_cast<uint32_t>(offsetAndWidth);
	const uint32_t width = static_cast<uint32_t>(offsetAndWidth >> 32);
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory index is invalid");
		return nullptr;
	}
	WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT linear memory is null");
		return nullptr;
	}
	const uint64_t effective = static_cast<uint64_t>(address) + offset;
	const uint64_t size = WASM::wasm_memory_size(memory);
	if (effective > size || width > size - effective) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	if (memory->memoryBase == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT linear-memory base is null");
		return nullptr;
	}
	return memory->memoryBase + effective;
}

//! Checked address of a memory64 access. The memory index and width travel in
//! one word; the 64-bit address and the 64-bit immediate offset are separate.
extern "C" void* sljitMemoryAddress64(WASM::VMContext* vm, uint64_t memoryAndWidth,
		uint64_t address, uint64_t offset) noexcept
{
	const uint32_t memoryIndex = static_cast<uint32_t>(memoryAndWidth);
	const uint32_t width = static_cast<uint32_t>(memoryAndWidth >> 32);
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64 index is invalid");
		return nullptr;
	}
	WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64 memory is null");
		return nullptr;
	}
	if (address > UINT64_MAX - offset) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	const uint64_t effective = address + offset;
	const uint64_t size = WASM::wasm_memory_size(memory);
	if (effective > size || width > size - effective) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	if (memory->memoryBase == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64 base is null");
		return nullptr;
	}
	return memory->memoryBase + effective;
}

extern "C" uint32_t sljitMemorySize(WASM::VMContext* vm, uint32_t memoryIndex) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.size index is invalid");
		return 0;
	}
	const WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr || memory->pageSize == 0) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.size memory is invalid");
		return 0;
	}
	return static_cast<uint32_t>(WASM::wasm_memory_size(memory) / memory->pageSize);
}

extern "C" uint64_t sljitMemorySize64(WASM::VMContext* vm, uint32_t memoryIndex) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.size index is invalid");
		return 0;
	}
	const WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr || memory->pageSize == 0) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.size memory is invalid");
		return 0;
	}
	return WASM::wasm_memory_size(memory) / memory->pageSize;
}

extern "C" uint64_t sljitMemoryGrow64(WASM::VMContext* vm, uint32_t memoryIndex,
		uint64_t delta) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.grow index is invalid");
		return UINT64_MAX;
	}
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	try {
		const int64_t previous = instance->growMemory(memoryIndex, delta);
		return previous < 0 ? UINT64_MAX : static_cast<uint64_t>(previous);
	} catch (...) {
		return UINT64_MAX;
	}
}

extern "C" uint32_t sljitMemoryGrow(WASM::VMContext* vm, uint32_t memoryIndex,
		uint32_t delta) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.grow index is invalid");
		return UINT32_MAX;
	}
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	try {
		const int64_t previous = instance->growMemory(memoryIndex, delta);
		return previous < 0 ? UINT32_MAX : static_cast<uint32_t>(previous);
	} catch (...) {
		// memory.grow is specified to return -1 when growth cannot be fulfilled.
		return UINT32_MAX;
	}
}

extern "C" void sljitMemoryCopy(WASM::VMContext* vm, uint64_t memoryPair,
		uint64_t addressPair, uint32_t length) noexcept
{
	const uint32_t destinationMemory = static_cast<uint32_t>(memoryPair);
	const uint32_t sourceMemory = static_cast<uint32_t>(memoryPair >> 32);
	const uint32_t destination = static_cast<uint32_t>(addressPair);
	const uint32_t source = static_cast<uint32_t>(addressPair >> 32);
	if (vm == nullptr || vm->memories == nullptr
			|| destinationMemory >= vm->memoryCount || sourceMemory >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.copy index is invalid");
		return;
	}
	WASM::LinearMemory* dst = vm->memories[destinationMemory];
	WASM::LinearMemory* src = vm->memories[sourceMemory];
	if (dst == nullptr || src == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.copy memory is null");
		return;
	}
	const uint64_t dstSize = WASM::wasm_memory_size(dst);
	const uint64_t srcSize = WASM::wasm_memory_size(src);
	if (destination > dstSize || length > dstSize - destination
			|| source > srcSize || length > srcSize - source) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	if (length == 0)
		return;
	if (dst->memoryBase == nullptr || src->memoryBase == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.copy base is null");
		return;
	}
	std::memmove(dst->memoryBase + destination, src->memoryBase + source, length);
}

extern "C" void sljitMemoryFill(WASM::VMContext* vm, uint64_t memoryAndValue,
		uint32_t destination, uint32_t length) noexcept
{
	const uint32_t memoryIndex = static_cast<uint32_t>(memoryAndValue);
	const uint32_t value = static_cast<uint32_t>(memoryAndValue >> 32);
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.fill index is invalid");
		return;
	}
	WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.fill memory is null");
		return;
	}
	const uint64_t size = WASM::wasm_memory_size(memory);
	if (destination > size || length > size - destination) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return;
	}
	if (length == 0)
		return;
	if (memory->memoryBase == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.fill base is null");
		return;
	}
	std::memset(memory->memoryBase + destination, static_cast<unsigned char>(value), length);
}

extern "C" void sljitAtomicTableSet(WASM::VMContext* vm, uint64_t tableAndOrdering,
		uint64_t index, void* value) noexcept
{
	wasm_atomic_table_set(vm, static_cast<uint32_t>(tableAndOrdering), index, value,
			static_cast<uint32_t>(tableAndOrdering >> 32));
}

extern "C" void* sljitAtomicTableXchg(WASM::VMContext* vm, uint64_t tableAndOrdering,
		uint64_t index, void* value) noexcept
{
	return wasm_atomic_table_xchg(vm, static_cast<uint32_t>(tableAndOrdering), index, value,
			static_cast<uint32_t>(tableAndOrdering >> 32));
}

extern "C" void* sljitAtomicTableCmpxchg(WASM::VMContext* vm, uint64_t tableAndOrdering,
		uint64_t index, const void** pair) noexcept
{
	return wasm_atomic_table_cmpxchg(vm, static_cast<uint32_t>(tableAndOrdering), index,
			const_cast<void*>(pair[0]), const_cast<void*>(pair[1]),
			static_cast<uint32_t>(tableAndOrdering >> 32));
}

extern "C" void sljitMemoryCopy64(WASM::VMContext* vm, uint64_t memoryPair,
const uint64_t* slots) noexcept
{
const uint32_t destinationMemory = static_cast<uint32_t>(memoryPair);
const uint32_t sourceMemory = static_cast<uint32_t>(memoryPair >> 32);
// One operand-stack slot is 16 bytes (two 64-bit words).
const uint64_t destination = slots[0];
const uint64_t source = slots[2];
const uint64_t length = slots[4];
if (vm == nullptr || vm->memories == nullptr
|| destinationMemory >= vm->memoryCount || sourceMemory >= vm->memoryCount) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.copy index is invalid");
return;
}
WASM::LinearMemory* dst = vm->memories[destinationMemory];
WASM::LinearMemory* src = vm->memories[sourceMemory];
if (dst == nullptr || src == nullptr) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.copy memory is null");
return;
}
const uint64_t dstSize = WASM::wasm_memory_size(dst);
const uint64_t srcSize = WASM::wasm_memory_size(src);
if (destination > dstSize || length > dstSize - destination
|| source > srcSize || length > srcSize - source) {
WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
return;
}
if (length == 0)
return;
if (dst->memoryBase == nullptr || src->memoryBase == nullptr) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.copy base is null");
return;
}
std::memmove(dst->memoryBase + destination, src->memoryBase + source,
static_cast<size_t>(length));
}

extern "C" void sljitMemoryFill64(WASM::VMContext* vm, uint64_t memoryAndValue,
uint64_t destination, uint64_t length) noexcept
{
const uint32_t memoryIndex = static_cast<uint32_t>(memoryAndValue);
const uint32_t value = static_cast<uint32_t>(memoryAndValue >> 32);
if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.fill index is invalid");
return;
}
WASM::LinearMemory* memory = vm->memories[memoryIndex];
if (memory == nullptr) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.fill memory is null");
return;
}
const uint64_t size = WASM::wasm_memory_size(memory);
if (destination > size || length > size - destination) {
WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
return;
}
if (length == 0)
return;
if (memory->memoryBase == nullptr) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.fill base is null");
return;
}
std::memset(memory->memoryBase + destination, static_cast<unsigned char>(value),
static_cast<size_t>(length));
}

extern "C" void sljitMemoryInit64(WASM::VMContext* vm, uint64_t indexPair,
const uint64_t* slots) noexcept
{
const uint32_t dataIndex = static_cast<uint32_t>(indexPair);
const uint32_t memoryIndex = static_cast<uint32_t>(indexPair >> 32);
const uint64_t destination = slots[0];
const uint32_t source = static_cast<uint32_t>(slots[2]);
const uint32_t length = static_cast<uint32_t>(slots[4]);
if (vm == nullptr) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.init VMContext is null");
return;
}
try {
reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit64(
memoryIndex, dataIndex, destination, source, length);
} catch (...) {
WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory64.init helper failed");
}
}

extern "C" void sljitMemoryInit(WASM::VMContext* vm, uint64_t indexPair,
		uint64_t addressPair, uint32_t length) noexcept
{
	const uint32_t dataIndex = static_cast<uint32_t>(indexPair);
	const uint32_t memoryIndex = static_cast<uint32_t>(indexPair >> 32);
	const uint32_t destination = static_cast<uint32_t>(addressPair);
	const uint32_t source = static_cast<uint32_t>(addressPair >> 32);
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.init VMContext is null");
		return;
	}
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit(
				memoryIndex, dataIndex, destination, source, length);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT memory.init helper failed");
	}
}

extern "C" void sljitDataDrop(WASM::VMContext* vm, uint32_t dataIndex) noexcept
{
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT data.drop VMContext is null");
		return;
	}
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->dataDrop(dataIndex);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT data.drop helper failed");
	}
}

extern "C" void sljitTableInit(WASM::VMContext* vm, uint64_t indexPair,
		uint64_t addressPair, uint32_t length) noexcept
{
	const uint32_t elementIndex = static_cast<uint32_t>(indexPair);
	const uint32_t tableIndex = static_cast<uint32_t>(indexPair >> 32);
	const uint32_t destination = static_cast<uint32_t>(addressPair);
	const uint32_t source = static_cast<uint32_t>(addressPair >> 32);
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.init VMContext is null");
		return;
	}
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->tableInit(
				elementIndex, tableIndex, destination, source, length);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.init helper failed");
	}
}

extern "C" void sljitElemDrop(WASM::VMContext* vm, uint32_t elementIndex) noexcept
{
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT elem.drop VMContext is null");
		return;
	}
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->elemDrop(elementIndex);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT elem.drop helper failed");
	}
}

extern "C" void sljitTableCopy(WASM::VMContext* vm, uint64_t tablePair,
		uint64_t addressPair, uint32_t length) noexcept
{
	const uint32_t destinationTable = static_cast<uint32_t>(tablePair);
	const uint32_t sourceTable = static_cast<uint32_t>(tablePair >> 32);
	const uint32_t destination = static_cast<uint32_t>(addressPair);
	const uint32_t source = static_cast<uint32_t>(addressPair >> 32);
	if (vm == nullptr || vm->tables == nullptr
			|| destinationTable >= vm->tableCount || sourceTable >= vm->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.copy index is invalid");
		return;
	}
	WASM::TableInstance* dst = vm->tables[destinationTable];
	WASM::TableInstance* src = vm->tables[sourceTable];
	if (dst == nullptr || src == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.copy table is null");
		return;
	}
	if (destination > dst->size || length > dst->size - destination
			|| source > src->size || length > src->size - source) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	if (length == 0)
		return;
	if (dst->base == nullptr || src->base == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.copy storage is null");
		return;
	}
	std::memmove(dst->base + destination, src->base + source,
			static_cast<std::size_t>(length) * sizeof(WASM::Callable*));
}

extern "C" uint32_t sljitTableSize(WASM::VMContext* vm, uint32_t tableIndex) noexcept
{
	if (vm == nullptr || vm->tables == nullptr || tableIndex >= vm->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.size index is invalid");
		return 0;
	}
	const WASM::TableInstance* table = vm->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT table.size table is null");
		return 0;
	}
	return static_cast<uint32_t>(table->size);
}

//! `call_indirect`'s runtime type check, mirroring the AsmJit backend's rule.
extern "C" WASM::Callable* sljitResolveIndirectCallable(WASM::VMContext* vm,
		uint32_t tableIndex, uint32_t elementIndex, uint32_t expectedTypeValue) noexcept
{
	if (vm == nullptr || vm->tables == nullptr || tableIndex >= vm->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call_indirect table index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = vm->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call_indirect table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return nullptr;
	}
	if (table->base == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call_indirect table storage is null");
		return nullptr;
	}
	WASM::Callable* callable = table->base[elementIndex];
	if (callable == nullptr) {
		WASM::reportTrap(WASM::TrapKind::UninitializedElement);
		return nullptr;
	}
	if (callable->fnPtr == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call_indirect target has no entry point");
		return nullptr;
	}
	const WASM::TypeId expected{expectedTypeValue};
	const WASM::TypeId none{WASM::TypeId::kNone};
	if (callable->typeId != none && callable->typeId != expected) {
		try {
			if (vm->module == nullptr
					|| !vm->module->store().types().matches(callable->typeId, expected)) {
				WASM::reportTrap(WASM::TrapKind::IndirectCallTypeMismatch);
				return nullptr;
			}
		} catch (...) {
			WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call_indirect type check failed");
			return nullptr;
		}
	}
	return callable;
}

//! SLJIT argument description for a Wasm call: a pointer context followed by
//! the declared value kinds. Integer and floating-point arguments occupy
//! independent register sequences.
sljit_s32 callArgTypes(const WASM::FuncType& type, WASM::ValueTypeCode resultKind,
		int resultWidth)
{
	sljit_s32 types = resultWidth == 0
			? SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
			: SLJIT_ARG_RETURN(argTypeOf(resultKind));
	types |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
	for (std::size_t i = 0; i < type.params.size(); ++i)
		types |= SLJIT_ARG_VALUE(argTypeOf(type.params[i].val.opcode),
				static_cast<sljit_s32>(i) + 2);
	return types;
}

} // namespace

OpcodeDispatcher::OpcodeDispatcher(const WASM::Module& module, const WASM::FuncType& funcType,
		const WASM::FunctionBody& body, std::uint32_t functionIndex, bool frameCallAbi,
		const std::vector<WASM::TagId>& tagIds, WASM::ModuleInstanceInternals& internals)
	: module_(module)
	, funcType_(funcType)
	, body_(body)
	, tagIds_(tagIds)
	, functionIndex_(functionIndex)
	, internals_(internals)
	, frameAbi_(frameCallAbi)
{
	const std::string* name = module.findName(WASM::NameKind::Function, functionIndex);
	if (name != nullptr)
		functionName_ = name->c_str();

	frameKinds_.reserve(funcType.params.size());
	for (const auto& parameter : funcType.params)
		frameKinds_.push_back(WASM::valueKindOf(parameter.val.opcode));

	WASM::PreparedFunctionStack prepared;
	try {
		prepared.prepare(funcType, body);
	} catch (const std::exception& error) {
		// A frame beyond the engine's limit is refused for this function only;
		// the rest of the module still compiles.
		unsupportedReason_ = std::string("the function frame is too large for the SLJIT backend: ")
				+ error.what();
		return;
	}

	parameterCount_ = prepared.parameterCount;
	localSlots_ = prepared.allLocals.size();

	paramWidths_.reserve(parameterCount_);
	paramKinds_.reserve(parameterCount_);
	for (std::uint32_t i = 0; i < parameterCount_; ++i) {
		paramWidths_.push_back(widthOf(prepared.allLocals[i]));
		paramKinds_.push_back(prepared.allLocals[i].opcode);
	}

	localWidths_.reserve(localSlots_);
	localKinds_.reserve(localSlots_);
	for (const WASM::ValueType& local : prepared.allLocals) {
		localWidths_.push_back(widthOf(local));
		localKinds_.push_back(local.opcode);
	}

	// The three-parameter limit is a *register-form* limit: it is the point where
	// SLJIT's argument entries run out. In frame form the parameters travel in the
	// caller's frame, so any count is expressible -- that is the whole reason the
	// frame form exists (docs/backends/STAGING_CALL_ABI.md).
	if (!frameAbi_ && parameterCount_ > kMaxWasmParameters) {
		unsupportedReason_ = "the SLJIT backend supports at most three parameters";
		return;
	}
	for (int width : localWidths_) {
		if (width == 0) {
			unsupportedReason_ = "the SLJIT backend only models i32, i64, f32 and f64 locals";
			return;
		}
	}
	for (const WASM::ValueType& result : prepared.allReturns) {
		resultKinds_.push_back(result.opcode);
		resultWidths_.push_back(widthOf(result));
	}
	// The register form has nowhere to put a second result, so it keeps refusing. The
	// frame form does: the result block is a sequence of slots
	// (docs/backends/EXPORT_SIGNATURE.md section 40).
	if (prepared.allReturns.size() > 1 && !frameAbi_) {
		unsupportedReason_ = "the SLJIT backend does not support multiple results";
		return;
	}
	if (prepared.allReturns.size() == 1) {
		resultKind_ = prepared.allReturns[0].opcode;
		resultWidth_ = widthOf(prepared.allReturns[0]);
		if (resultWidth_ == 0) {
			unsupportedReason_ = "the SLJIT backend only returns i32, i64, f32 and f64";
			resultKind_ = WASM::ValueTypeCode::Void;
			resultWidth_ = 0;
			return;
		}
	}
	supported_ = true;
}

bool OpcodeDispatcher::compile(void*& entry)
{
	if (!supported_)
		return false;

	// Pass 1 sizes the frame: `sljit_emit_enter` fixes the local area before any
	// body instruction is emitted, so the operand-stack high-water mark has to be
	// known first.
	maxStackDepth_ = 0;
	if (!run(true))
		return false;

	// Pass 2 emits. Any failure releases the compiler: no partially emitted
	// function is ever installed.
	if (!run(false)) {
		releaseCompiler();
		return false;
	}
	const bool emitted = finish(entry);
	releaseCompiler();
	return emitted;
}

bool OpcodeDispatcher::run(bool measure)
{
	measureOnly_ = measure;
	stackWidths_.clear();
	controls_.clear();
	trapJumps_.clear();
	reachable_ = true;
	ok_ = true;

	if (!measure && !beginFunction())
		return false;

	std::span<const std::byte> codeSpan(
			reinterpret_cast<const std::byte*>(body_.code.data()), body_.code.size());
	Euph::Io::ConstBufferDevice device(codeSpan);
	WASM::FastMemReader stream(device);
	try {
		readCode(stream);
	} catch (const UnsupportedBody&) {
		return false;
	}
	// Fidelity guard (docs/backends/ASMJIT_SLJIT_P3_GC1.md §24): a valid body ends
	// with exactly the results its signature declares. AsmJit enforces the same rule
	// and SLJIT did not, so an invalid module was compiled instead of refused. Dead
	// code after an `unreachable` is exempt: Wasm makes the stack polymorphic there.
	if (!measure && reachable_) {
		const std::size_t expectedResults = resultWidth_ == 0 ? 0u : 1u;
		if (stackWidths_.size() != expectedResults) {
			unsupportedReason_ = "the function body leaves "
				+ std::to_string(stackWidths_.size())
				+ " operand-stack value(s) but the signature declares "
				+ std::to_string(expectedResults);
			ok_ = false;
		}
	}
	return ok_;
}

bool OpcodeDispatcher::beginFunction()
{
	// Cross-function trampolines are register-form only: the driver's host
	// signature is the function signature, and the frame ABI needs a frame
	// pointer that TailCallState does not carry.
	tailTrampoline_ = WASM::tailCallTrampolinesEligible(module_, frameAbi_);
	compiler_ = sljit_create_compiler(nullptr);
	if (compiler_ == nullptr) {
		unsupportedReason_ = "SLJIT could not create a compiler";
		return false;
	}

	const std::size_t frameSlots = localSlots_ + kReservedSlots + maxStackDepth_
			+ (frameAbi_ ? 16u : 0u); // 256 bytes of staging scratch for callee frames
	if (frameSlots > SLJIT_MAX_LOCAL_SIZE / kSlotBytes) {
		unsupportedReason_ = "the function frame exceeds SLJIT's local-area limit";
		return false;
	}

	// The argument list is `VMContext*` followed by the Wasm parameters. Integer
	// arguments arrive in S0, S1, ... and floating-point arguments in FR0, FR1,
	// ... ; the counters are independent, as the SLJIT call ABI specifies.
	sljit_s32 argTypes = resultWidth_ == 0 || resultWidth_ == 16
			? SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
			: SLJIT_ARG_RETURN(argTypeOf(resultKind_));
	// A v128 parameter is a 16-byte integer pair on the SysV boundary, so it
	// occupies two argument slots and every later parameter shifts by two. The
	// walk comes from `WASM::mapArgSlots` rather than being spelled out here: its
	// contract is that every backend indexing parameters positionally uses it,
	// because hand arithmetic for this mapping produced three separate corruption
	// investigations in a positional backend.
	argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
	std::vector<WASM::ValueKind> parameterKinds;
	parameterKinds.reserve(parameterCount_);
	for (std::uint32_t i = 0; i < parameterCount_; ++i)
		parameterKinds.push_back(WASM::valueKindOf(paramKinds_[i]));
	const std::vector<WASM::ArgSlot> parameterSlots = WASM::mapArgSlots(parameterKinds);
	sljit_s32 nextSlot = 2;
	if (!frameAbi_) {
	for (const WASM::ArgSlot& slot : parameterSlots) {
		const sljit_s32 first = 2 + static_cast<sljit_s32>(slot.firstSlot);
		if (slot.slots == 2) {
			argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, first);
			argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, first + 1);
			nextSlot = first + 2;
		} else {
			argTypes |= SLJIT_ARG_VALUE(argTypeOf(paramKinds_[slot.parameter]), first);
			nextSlot = first + 1;
		}
	}
	} else {
		// Frame form: two C arguments -- the context and the frame pointer. No
		// parameter occupies a register, so no ceiling applies
		// (docs/backends/STAGING_CALL_ABI.md).
		argTypes = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
				| SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1)
				| SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 2);
		nextSlot = 3;
		frameResultOffset_ = static_cast<sljit_sw>(WASM::frameArgumentsBytes(frameKinds_));
	}

	// A `v128` result cannot come back in one return register, so the caller
	// supplies a 16-byte out-buffer as a trailing pointer argument -- the same
	// path multi-value takes (docs/ABI_REFACTOR_STUDY.md §25). It must be added
	// *before* `savedCount`, which is derived from `nextSlot`.
	const bool resultViaBuffer = !frameAbi_ && resultWidth_ == 16;
	if (resultViaBuffer) {
		argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, nextSlot);
		++nextSlot;
	}

	// Four integer scratch registers: R0-R3 carry a helper call with up to four
	// integer arguments. Floating-point scratch registers are counted
	// separately by SLJIT; three cover FR0/FR1 for the emitted operations and
	// any call's float arguments.
	std::uint32_t floatParameters = 0;
	for (WASM::ValueTypeCode kind : paramKinds_)
		if (isFloatKind(kind))
			++floatParameters;
	const std::uint32_t floatScratch = std::max<std::uint32_t>(3, floatParameters);
	const sljit_s32 savedCount = nextSlot - 1;

	// SLJIT requires every declared argument slot to own a saved register, and the
	// target provides only SLJIT_NUMBER_OF_SAVED_REGISTERS of them (6 on x86-64).
	// A `v128` parameter takes two slots, so `(v128, v128, v128)` needs seven and
	// cannot be expressed. Refuse it here: letting `sljit_emit_enter` see the
	// count would trip SLJIT's own assertion and abort the whole process, which
	// would take down every other module in the run
	// (docs/ABI_REFACTOR_STUDY.md §35, §36).
	if (!frameAbi_ && savedCount > kArgumentSlotCeiling) {
		unsupportedReason_ = "the SLJIT backend cannot pass "
			+ std::to_string(static_cast<unsigned>(savedCount))
			+ " argument slots: the target has only "
			+ std::to_string(kArgumentSlotCeiling)
			+ ", so this signature needs fewer vector parameters";
		return false;
	}
	emit(sljit_emit_enter(compiler_, 0, argTypes,
			4 | SLJIT_ENTER_FLOAT(static_cast<sljit_s32>(floatScratch)),
			savedCount, static_cast<sljit_s32>(frameSlots * kSlotBytes)));
	if (!ok_) {
		// A failed prologue leaves SLJIT in its error state; emitting more would
		// only trip its own argument checks.
		unsupportedReason_ = "SLJIT rejected the function prologue";
		return false;
	}

	// Keep the context in the frame: helper calls use R0-R3 and the context is
	// needed before every one of them.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), contextSlotOffset(),
			SLJIT_S0, 0));

	// Keep the out-buffer pointer too: the value is produced after helper calls,
	// which clobber R0-R3 (the S registers are not held across one either).
	if (resultViaBuffer) {
		// Argument index i arrives in S(i - 1); `nextSlot` has already been
		// advanced past the buffer, so its register is S(nextSlot - 2) -- the
		// highest register `savedCount` preserves.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				resultBufferSlotOffset(), SLJIT_S(nextSlot - 2), 0));
	}

	// Keep the staging frame pointer: the prologue and the epilogue reach the
	// parameters and results through it, and helper calls clobber R0-R3.
	if (frameAbi_) {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				frameSlotOffset(), SLJIT_S(1), 0));
	}

	// The guest stack guard (WasmStack.hpp): every compiled function calls it
	// before its body runs.
	loadContext(SLJIT_R0);
	emitHelper(SLJIT_ARGS1V(P), &WASM::wasm_stack_check);
	emitPendingTrapCheck();

	std::uint32_t integerSlot = 1;
	std::uint32_t floatIndex = 0;
	for (std::uint32_t i = 0; i < parameterCount_; ++i) {
		if (frameAbi_) {
			// Frame form: the value lives at its offset in the caller's frame. The
			// pointer is reloaded through R2 each time because helper calls clobber
			// R0-R3 (the S registers are not held across one either).
			const sljit_sw offset = static_cast<sljit_sw>(
					WASM::frameArgumentOffset(frameKinds_, i));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0,
					SLJIT_MEM1(SLJIT_SP), frameSlotOffset()));
			if (paramWidths_[i] == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_R2), offset));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						localSlotOffset(i), SLJIT_R0, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_R2), offset + 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						localSlotOffset(i) + 8, SLJIT_R0, 0));
			} else {
				emit(sljit_emit_op1(compiler_,
						paramWidths_[i] == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
						SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R2), offset));
				emit(sljit_emit_op1(compiler_,
						paramWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
						SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_R0, 0));
			}
			continue;
		}
		if (isFloatKind(paramKinds_[i])) {
			emit(sljit_emit_fop1(compiler_,
					paramWidths_[i] == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
					SLJIT_MEM1(SLJIT_SP), localSlotOffset(i),
					SLJIT_FR(static_cast<sljit_s32>(floatIndex)), 0));
			++floatIndex;
		} else if (paramWidths_[i] == 16) {
			// The two halves arrive in two consecutive argument registers.
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					localSlotOffset(i),
					SLJIT_S(static_cast<sljit_s32>(integerSlot)), 0));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					localSlotOffset(i) + 8,
					SLJIT_S(static_cast<sljit_s32>(integerSlot + 1)), 0));
			integerSlot += 2;
		} else {
			emit(sljit_emit_op1(compiler_, paramWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
					SLJIT_MEM1(SLJIT_SP), localSlotOffset(i),
					SLJIT_S(static_cast<sljit_s32>(integerSlot)), 0));
			++integerSlot;
		}
	}
	// Declared locals start at zero, as the spec requires.
	for (std::size_t i = parameterCount_; i < localSlots_; ++i) {
		if (localWidths_[i] == 16) {
			// A v128 local occupies the whole 16-byte cell.
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					localSlotOffset(i), SLJIT_IMM, 0));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					localSlotOffset(i) + 8, SLJIT_IMM, 0));
		} else {
			emit(sljit_emit_op1(compiler_, localWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
					SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_IMM, 0));
		}
	}
	// The body-entry label is the loop target of a direct self tail call; the
	// prologue above has already run for this frame.
	bodyStart_ = sljit_emit_label(compiler_);
	if (bodyStart_ == nullptr)
		ok_ = false;
	return ok_;
}

bool OpcodeDispatcher::finish(void*& entry)
{
	if (!ok_) {
		unsupportedReason_ = "SLJIT rejected an instruction while emitting the function";
		return false;
	}

	// The trap path: every pending-trap branch joins here.
	sljit_label* trapLabel = sljit_emit_label(compiler_);
	if (trapLabel == nullptr) {
		unsupportedReason_ = "SLJIT could not place the trap epilogue";
		return false;
	}
	for (sljit_jump* jump : trapJumps_) {
		if (jump != nullptr)
			sljit_set_label(jump, trapLabel);
	}

	// Name the function in the diagnostic, then return a benign zero result:
	// a trap is a pending status, never a non-local jump (WasmTrap.hpp).
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
			static_cast<sljit_sw>(functionIndex_)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
			codeAddress(functionName_)));
	emitHelper(SLJIT_ARGS3V(32, 32, P), &WASM::wasm_trap_annotate);
	if (frameAbi_ || resultWidth_ == 0 || resultWidth_ == 16) {
		emit(sljit_emit_return_void(compiler_));
	} else if (isFloatKind(resultKind_)) {
		emit(resultWidth_ == 4
				? sljit_emit_fset32(compiler_, SLJIT_FR0, 0.0f)
				: sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0));
		emit(sljit_emit_return(compiler_,
				resultWidth_ == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64, SLJIT_FR0, 0));
	} else {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0));
		emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_R0, 0));
	}

	if (!ok_) {
		unsupportedReason_ = "SLJIT rejected the trap epilogue";
		return false;
	}

	void* code = sljit_generate_code(compiler_, 0, nullptr);
	if (code == nullptr) {
		unsupportedReason_ = "SLJIT could not generate code for the function";
		return false;
	}
	entry = code;
	return true;
}

void OpcodeDispatcher::releaseCompiler()
{
	if (compiler_ == nullptr)
		return;
	sljit_free_compiler(compiler_);
	compiler_ = nullptr;
}

void OpcodeDispatcher::emit(sljit_s32 status)
{
	if (status != SLJIT_SUCCESS)
		ok_ = false;
}

void OpcodeDispatcher::push(int width)
{
	stackWidths_.push_back(width);
	if (stackWidths_.size() > maxStackDepth_)
		maxStackDepth_ = stackWidths_.size();
}

int OpcodeDispatcher::pop()
{
	const int width = stackWidths_.empty() ? 0 : stackWidths_.back();
	if (!stackWidths_.empty())
		stackWidths_.pop_back();
	return width;
}

void OpcodeDispatcher::require(bool condition, const char* reason)
{
	if (condition)
		return;
	unsupportedReason_ = reason;
	throw UnsupportedBody{};
}

void OpcodeDispatcher::requireStack(std::size_t count)
{
	require(stackWidths_.size() >= count,
			"the body consumes more operands than it produces");
}

void OpcodeDispatcher::requireLocal(std::size_t index, const char* what)
{
	if (index < localSlots_)
		return;
	unsupportedReason_ = std::string(what) + " names a local outside the function frame";
	throw UnsupportedBody{};
}

void OpcodeDispatcher::markUnreachable()
{
	reachable_ = false;
	const std::size_t base = controls_.empty() ? 0 : controls_.back().baseDepth;
	if (stackWidths_.size() > base)
		stackWidths_.resize(base);
}

sljit_sw OpcodeDispatcher::localSlotOffset(std::size_t index) const
{
	return static_cast<sljit_sw>(index * kSlotBytes);
}

sljit_sw OpcodeDispatcher::stackSlotOffset(std::size_t depth) const
{
	return static_cast<sljit_sw>((localSlots_ + kReservedSlots + depth) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::contextSlotOffset() const
{
	return static_cast<sljit_sw>((localSlots_ + 0) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::targetSlotOffset() const
{
	return static_cast<sljit_sw>((localSlots_ + 1) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::scratchSlotOffset() const
{
	return static_cast<sljit_sw>((localSlots_ + 2) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::payloadSlotOffset(std::size_t index) const
{
	// Reserved slot 3 is unused elsewhere; `emitPendingTrapCheck` parks the
	// caller's R0 here so the probe cannot destroy it.
	return static_cast<sljit_sw>((localSlots_ + 3 + index) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::resultBufferSlotOffset() const
{
	// Reserved slot 7, after the four payload slots: the out-buffer pointer has
	// to survive helper calls, which clobber R0-R3.
	return static_cast<sljit_sw>((localSlots_ + 7) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::frameSlotOffset() const
{
	// Reserved slot 8, after the result buffer: the staging-frame pointer has to
	// survive helper calls, which clobber R0-R3.
	return static_cast<sljit_sw>((localSlots_ + 8) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::callFrameOffset() const
{
	// The staging scratch area sits after the operand stack, so a callee frame is
	// built without disturbing any live operand.
	return static_cast<sljit_sw>(
			(localSlots_ + kReservedSlots + maxStackDepth_) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::exceptionPtrSlotOffset() const
{
	// Reserved slot 9, after the staging-frame pointer: the exception being
	// routed has to survive the helper calls that clear the slot and read the
	// tag id.
	return static_cast<sljit_sw>((localSlots_ + 9) * kSlotBytes);
}

sljit_sw OpcodeDispatcher::selectorBlockOffset() const
{
	return static_cast<sljit_sw>((localSlots_ + 3) * kSlotBytes);
}

void OpcodeDispatcher::loadSlot(int reg, std::size_t depth, int width)
{
	emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV_U32 : SLJIT_MOV, reg, 0,
			SLJIT_MEM1(SLJIT_SP), stackSlotOffset(depth)));
}

void OpcodeDispatcher::storeSlot(int reg, std::size_t depth, int width)
{
	emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), stackSlotOffset(depth), reg, 0));
}

void OpcodeDispatcher::loadContext(int reg)
{
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, reg, 0, SLJIT_MEM1(SLJIT_SP),
			contextSlotOffset()));
}

void OpcodeDispatcher::transferToBase(const std::vector<int>& widths, std::size_t base)
{
	const std::size_t count = widths.size();
	if (count == 0)
		return;
	// The destination is always below the source, so an ascending copy never
	// overwrites a value that has not been read yet.
	for (std::size_t i = 0; i < count; ++i) {
		const std::size_t source = stackWidths_.size() - count + i;
		loadSlot(SLJIT_R0, source, widths[i]);
		storeSlot(SLJIT_R0, base + i, widths[i]);
	}
}

void OpcodeDispatcher::emitReturnValue()
{
	if (frameAbi_ && resultKinds_.size() > 1) {
		// Multi-result in frame form (docs/backends/EXPORT_SIGNATURE.md section 40):
		// every result is written into the caller's result block, in declaration
		// order, 8 bytes per scalar slot and 16 per vector, then the function returns
		// void. This must precede the `resultWidth_ == 0` test, which is 0 for a
		// multi-result signature.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP),
				frameSlotOffset()));
		std::uint32_t offset = frameResultOffset_;
		const std::size_t resultCount = resultKinds_.size();
		for (std::size_t i = 0; i < resultCount; ++i) {
			const std::size_t index = stackWidths_.size() - resultCount + i;
			const int width = resultWidths_[i];
			if (width == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(index)));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
						static_cast<sljit_sw>(offset), SLJIT_R0, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(index) + 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
						static_cast<sljit_sw>(offset + 8), SLJIT_R0, 0));
				offset += 16;
				continue;
			}
			loadSlot(SLJIT_R0, index, width);
			emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
					SLJIT_MEM1(SLJIT_R2), static_cast<sljit_sw>(offset), SLJIT_R0, 0));
			offset += 8;
		}
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	if (resultWidth_ == 0) {
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	requireStack(1);
	if (frameAbi_) {
		// Frame form: the single result is written into the caller's frame at the
		// result block, then the function returns void.
		const std::size_t top = stackWidths_.size() - 1;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP),
				frameSlotOffset()));
		if (resultWidth_ == 16) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(top)));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
					frameResultOffset_, SLJIT_R0, 0));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(top) + 8));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2),
					frameResultOffset_ + 8, SLJIT_R0, 0));
		} else {
			loadSlot(SLJIT_R0, top, resultWidth_);
			emit(sljit_emit_op1(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
					SLJIT_MEM1(SLJIT_R2), frameResultOffset_, SLJIT_R0, 0));
		}
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	if (resultWidth_ == 16) {
		// The vector occupies one 16-byte operand slot (kSlotBytes), so the two
		// words are contiguous in the frame. R2/R3 are scratch here: the body is
		// finished and nothing after this reads a live value.
		const std::size_t top = stackWidths_.size() - 1;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP),
				resultBufferSlotOffset()));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R2, 0, SLJIT_IMM, 8));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				stackSlotOffset(top)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2), 0, SLJIT_R0, 0));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				stackSlotOffset(top) + 8));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R3), 0, SLJIT_R0, 0));
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	if (isFloatKind(resultKind_)) {
		loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, resultWidth_);
		emit(sljit_emit_return(compiler_,
				resultWidth_ == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64, SLJIT_FR0, 0));
		return;
	}
	// An i32 comes back zero-extended from MOV_U32, and the C ABI of the caller
	// reads only the low 32 bits of the return register. The *operation* passed
	// to `sljit_emit_return` must match the declared return width, though: SLJIT
	// asserts that a 32-bit return uses MOV32.
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, resultWidth_);
	emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_R0, 0));
}

void OpcodeDispatcher::emitPendingTrapCheck()
{
	if (measureOnly_)
		return;
	// The probe calls a helper, which clobbers R0-R3, and callers legitimately
	// hold the value they just produced in R0 (a fresh allocation, typically).
	// Park it in the reserved payload slot and restore it on the fallthrough
	// path; the trap path never needs it (docs/backends/ASMJIT_SLJIT_P3_GC1.md §44).
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), payloadSlotOffset(),
			SLJIT_R0, 0));
	emitHelper(SLJIT_ARGS0(W), &sljitTrapPending);
	sljit_jump* jump = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (jump == nullptr) {
		ok_ = false;
		return;
	}
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
			payloadSlotOffset()));
	trapJumps_.push_back(jump);
}

WASM::FuncType OpcodeDispatcher::tagPayloadType(WASM::TagIdx tagIdx)
{
	const std::size_t imported = module_.importTags.size();
	std::uint32_t typeIdx = 0;
	if (tagIdx < imported) {
		typeIdx = module_.importTags[tagIdx].typeIdx;
	} else {
		const std::size_t local = tagIdx - imported;
		require(local < module_.tags.size(), "throw names an out-of-range tag");
		typeIdx = module_.tags[local].typeIdx;
	}
	require(typeIdx < module_.typeCount()
					&& module_.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func,
			"the tag does not name a function type");
	return module_.funcTypeAt(typeIdx);
}

WASM::TagId OpcodeDispatcher::tagIdForClause(const WASM::CatchClause& clause)
{
	require(clause.tagIdx < tagIds_.size(), "try_table catch names an out-of-range tag");
	return tagIds_[clause.tagIdx];
}

void OpcodeDispatcher::emitZeroReturn()
{
	// The exception leaves the function, but the generated signature still has to
	// return a well-formed value. Zeroes are never observed by the caller: it
	// sees the pending slot first (docs/ABI.md "Exceptions").
	if (frameAbi_ || resultWidth_ == 0 || resultWidth_ == 16) {
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	if (isFloatKind(resultKind_)) {
		emit(resultWidth_ == 4
				? sljit_emit_fset32(compiler_, SLJIT_FR0, 0.0f)
				: sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0));
		emit(sljit_emit_return(compiler_,
				resultWidth_ == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64, SLJIT_FR0, 0));
		return;
	}
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0));
	emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_R0, 0));
}

void OpcodeDispatcher::emitPendingExceptionCheck()
{
	if (measureOnly_)
		return;
	// A trap wins over a pending Wasm exception and must never be routed into a
	// `try_table` handler: a trap is not catchable by Wasm code (EXD2).
	emitPendingTrapCheck();

	emitHelper(SLJIT_ARGS0(P), &sljitExnPending);
	sljit_jump* none = sljit_emit_cmp(compiler_, SLJIT_EQUAL,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (none == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			exceptionPtrSlotOffset(), SLJIT_R0, 0));
	emitExceptionPropagation();
	sljit_label* continuation = sljit_emit_label(compiler_);
	if (continuation == nullptr)
		ok_ = false;
	else if (none != nullptr)
		sljit_set_label(none, continuation);
}

void OpcodeDispatcher::emitExceptionPropagation()
{
	if (measureOnly_)
		return;
	// The pending pointer survives the helpers in the reserved slot.
	emitHelper(SLJIT_ARGS0(P), &sljitExnPending);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			exceptionPtrSlotOffset(), SLJIT_R0, 0));

	// R0 carries the thrown tag id for the comparison chain.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
			SLJIT_MEM1(SLJIT_SP), exceptionPtrSlotOffset()));
	emitHelper(SLJIT_ARGS1(32, P), &sljitExnTagId);

	for (std::size_t frameIndex = controls_.size(); frameIndex-- > 0; ) {
		ControlFrame& frame = controls_[frameIndex];
		if (!frame.tryTable)
			continue;
		for (const WASM::CatchClause& clause : frame.catchClauses) {
			require(clause.labelIdx < frameIndex,
					"the try_table catch label targets the function label");
			ControlFrame& target = controls_[frameIndex - 1 - clause.labelIdx];
			const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			if (catchAll) {
				emitCatchBranch(target, WASM::FuncType{}, carryExn);
				return;
			}
			const WASM::FuncType payload = tagPayloadType(clause.tagIdx);
			const WASM::TagId tagId = tagIdForClause(clause);
			sljit_jump* mismatch = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
					SLJIT_R0, 0, SLJIT_IMM, static_cast<sljit_sw>(tagId));
			if (mismatch == nullptr)
				ok_ = false;
			emitCatchBranch(target, payload, carryExn);
			sljit_label* next = sljit_emit_label(compiler_);
			if (next == nullptr)
				ok_ = false;
			else if (mismatch != nullptr)
				sljit_set_label(mismatch, next);
		}
	}

	// No handler in this function: the pending slot is already set, so return
	// zeroes and let the caller re-propagate it.
	emitZeroReturn();
}

void OpcodeDispatcher::emitCatchBranch(ControlFrame& target, const WASM::FuncType& payload,
		bool carryExn)
{
	const std::vector<int>& targetWidths =
			target.kind == ControlKind::Loop ? target.paramWidths : target.resultWidths;
	const std::size_t payloadCount = payload.params.size();
	require(targetWidths.size() == payloadCount + (carryExn ? 1u : 0u),
			"the try_table catch arity does not match its target label");

	// A caught exception is consumed: clearing first means the caller and the
	// embedder no longer observe it (EXD1).
	emitHelper(SLJIT_ARGS0V(), &sljitExnClearPending);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
			SLJIT_MEM1(SLJIT_SP), exceptionPtrSlotOffset()));

	const ExceptionPayloadLayout layout = exceptionPayloadLayout(payload);
	for (std::size_t i = 0; i < payloadCount; ++i) {
		const sljit_sw offset = static_cast<sljit_sw>(layout.fields[i].offset);
		const std::size_t slot = target.baseDepth + i;
		const WASM::ValueTypeCode kind = payload.params[i].val.opcode;
		const int width = widthOf(payload.params[i].val);
		if (width == 16) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
					SLJIT_MEM1(SLJIT_R1), offset));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(slot), SLJIT_R0, 0));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
					SLJIT_MEM1(SLJIT_R1), offset + 8));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(slot) + 8, SLJIT_R0, 0));
		} else if (isFloatKind(kind)) {
			emit(sljit_emit_fop1(compiler_,
					width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
					SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R1), offset));
			storeFloatSlot(SLJIT_FR0, slot, width);
		} else {
			emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
					SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), offset));
			storeSlot(SLJIT_R0, slot, width);
		}
	}
	if (carryExn) {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
				SLJIT_MEM1(SLJIT_SP), exceptionPtrSlotOffset()));
		storeSlot(SLJIT_R0, target.baseDepth + payloadCount, 8);
	}

	sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (jump == nullptr) {
		ok_ = false;
		return;
	}
	if (target.kind == ControlKind::Loop) {
		if (target.begin == nullptr) {
			ok_ = false;
			return;
		}
		sljit_set_label(jump, target.begin);
	} else {
		target.endJumps.push_back(jump);
		target.endTargeted = true;
	}
}

void OpcodeDispatcher::emitTrap(WASM::TrapKind kind)
{
	// Reporting a kind needs a register, and the call that follows clobbers
	// R0-R3. All four are parked, so a caller may hold anything across this
	// primitive (docs/backends/ASMJIT_SLJIT_P3_GC1.md §48).
	const sljit_s32 parked[4] = {SLJIT_R0, SLJIT_R1, SLJIT_R2, SLJIT_R3};
	for (std::size_t i = 0; i < 4; ++i)
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				payloadSlotOffset(i), parked[i], 0));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
			static_cast<sljit_sw>(kind)));
	emitHelper(SLJIT_ARGS1V(32), &sljitReportTrap);
	emitTrapBranch();
	for (std::size_t i = 0; i < 4; ++i)
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, parked[i], 0, SLJIT_MEM1(SLJIT_SP),
				payloadSlotOffset(i)));
}

void OpcodeDispatcher::emitTrapBranch()
{
	if (measureOnly_)
		return;
	sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (jump == nullptr)
		ok_ = false;
	trapJumps_.push_back(jump);
}

void OpcodeDispatcher::onUnhandledOpcode(const char* name)
{
	unsupportedReason_ = std::string("the SLJIT backend does not lower ") + name
			+ " (docs/WRITING_A_BACKEND.md §12)";
	throw UnsupportedBody{};
}

void OpcodeDispatcher::dispatchNop()
{
}

void OpcodeDispatcher::dispatchUnreachable()
{
	if (!reachable_)
		return;
	emitHelper(SLJIT_ARGS0V(), &sljitReportUnreachable);
	emitTrapBranch();
	markUnreachable();
}

OpcodeDispatcher::BlockSignature OpcodeDispatcher::blockSignature(const WASM::BlockType& type)
{
	BlockSignature signature;
	if (type.isVoid())
		return signature;
	if (!type.isTypeIndex) {
		switch (type.valType) {
		case WASM::ValueTypeCode::I32:
		case WASM::ValueTypeCode::F32:
			signature.results.push_back(4);
			break;
		case WASM::ValueTypeCode::I64:
		case WASM::ValueTypeCode::F64:
			signature.results.push_back(8);
			break;
		case WASM::ValueTypeCode::V128:
			require(false, "the SLJIT backend does not lower v128 block types yet");
			break;
		default:
			require(false, "the SLJIT backend only models i32, i64, f32 and f64 block types");
		}
		return signature;
	}
	require(type.typeIndex >= 0
					&& static_cast<std::uint32_t>(type.typeIndex) < module_.typeCount(),
			"the block type index is out of range");
	const WASM::FuncType blockType = module_.funcTypeAt(static_cast<std::uint32_t>(type.typeIndex));
	for (const WASM::StorageType& parameter : blockType.params) {
		const int width = widthOf(parameter.val);
		require(width != 0 && width != 16,
				"the SLJIT backend does not lower v128 block parameters yet");
		signature.params.push_back(width);
	}
	for (const WASM::StorageType& result : blockType.results) {
		const int width = widthOf(result.val);
		require(width != 0 && width != 16,
				"the SLJIT backend does not lower v128 block results yet");
		signature.results.push_back(width);
	}
	return signature;
}

void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& type)
{
	const BlockSignature signature = blockSignature(type);
	ControlFrame frame;
	frame.kind = ControlKind::Block;
	frame.paramWidths = signature.params;
	frame.resultWidths = signature.results;
	frame.entryReachable = reachable_;
	if (reachable_) {
		requireStack(signature.params.size());
		frame.baseDepth = stackWidths_.size() - signature.params.size();
	} else {
		frame.baseDepth = stackWidths_.size();
	}
	// A block's parameters are already in the slots the label expects, so no
	// data movement is needed at entry.
	controls_.push_back(std::move(frame));
}

void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& type)
{
	const BlockSignature signature = blockSignature(type);
	ControlFrame frame;
	frame.kind = ControlKind::Loop;
	frame.paramWidths = signature.params;
	frame.resultWidths = signature.results;
	frame.entryReachable = reachable_;
	if (reachable_) {
		requireStack(signature.params.size());
		frame.baseDepth = stackWidths_.size() - signature.params.size();
	} else {
		frame.baseDepth = stackWidths_.size();
	}
	if (!measureOnly_)
		frame.begin = sljit_emit_label(compiler_);
	if (!measureOnly_ && frame.begin == nullptr)
		ok_ = false;
	controls_.push_back(std::move(frame));
}

void OpcodeDispatcher::dispatchIf(const WASM::BlockType& type)
{
	require(reachable_ ? !stackWidths_.empty() : true,
			"the if condition stack underflowed");
	const BlockSignature signature = blockSignature(type);
	ControlFrame frame;
	frame.kind = ControlKind::If;
	frame.paramWidths = signature.params;
	frame.resultWidths = signature.results;
	frame.entryReachable = reachable_;
	if (reachable_) {
		requireStack(1 + signature.params.size());
		require(stackWidths_.back() == 4, "the if condition must be an i32");
		pop();
		frame.baseDepth = stackWidths_.size() - signature.params.size();
		if (!measureOnly_) {
			loadSlot(SLJIT_R0, stackWidths_.size(), 4);
			frame.alternateJump = sljit_emit_cmp(compiler_, SLJIT_EQUAL | SLJIT_32,
					SLJIT_R0, 0, SLJIT_IMM, 0);
			if (frame.alternateJump == nullptr)
				ok_ = false;
		}
	} else {
		frame.baseDepth = stackWidths_.size();
	}
	controls_.push_back(std::move(frame));
}

void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& type,
		std::vector<WASM::CatchClause>&& catches)
{
	const BlockSignature signature = blockSignature(type);
	ControlFrame frame;
	frame.kind = ControlKind::Block;
	frame.tryTable = true;
	frame.catchClauses = std::move(catches);
	frame.paramWidths = signature.params;
	frame.resultWidths = signature.results;
	frame.entryReachable = reachable_;
	if (reachable_) {
		requireStack(signature.params.size());
		frame.baseDepth = stackWidths_.size() - signature.params.size();
	} else {
		frame.baseDepth = stackWidths_.size();
	}
	controls_.push_back(std::move(frame));
	const std::size_t frameIndex = controls_.size() - 1;
	ControlFrame& tryFrame = controls_[frameIndex];

	// Validate every clause and, in the measuring pass, reserve room for the
	// payload slots a catch writes into its target label. A catch can target a
	// block whose end the body never reaches normally, so the operand-stack
	// high-water mark alone does not necessarily cover those slots.
	for (const WASM::CatchClause& clause : tryFrame.catchClauses) {
		require(clause.labelIdx < frameIndex,
				"the try_table catch label targets the function label");
		ControlFrame& target = controls_[frameIndex - 1 - clause.labelIdx];
		const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
				|| clause.kind == WASM::CatchKind::CatchAllRef;
		const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
				|| clause.kind == WASM::CatchKind::CatchAllRef;
		const std::size_t payloadCount = catchAll
				? 0u : tagPayloadType(clause.tagIdx).params.size();
		const std::size_t targetWidths = target.kind == ControlKind::Loop
				? target.paramWidths.size() : target.resultWidths.size();
		require(targetWidths == payloadCount + (carryExn ? 1u : 0u),
				"the try_table catch arity does not match its target label");
		if (measureOnly_) {
			const std::size_t bound = target.baseDepth + targetWidths;
			if (bound > maxStackDepth_)
				maxStackDepth_ = bound;
		}
	}
}

void OpcodeDispatcher::dispatchElse()
{
	require(!controls_.empty() && controls_.back().kind == ControlKind::If
					&& !controls_.back().hasElse,
			"an else does not match the open block");
	ControlFrame& frame = controls_.back();
	if (reachable_ && !measureOnly_) {
		sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
		if (jump == nullptr)
			ok_ = false;
		else
			frame.endJumps.push_back(jump);
	}
	if (!measureOnly_ && frame.alternateJump != nullptr) {
		sljit_label* label = sljit_emit_label(compiler_);
		if (label == nullptr)
			ok_ = false;
		else
			sljit_set_label(frame.alternateJump, label);
	}
	frame.hasElse = true;
	// Remember whether the then arm fell through: the else arm may end in a
	// tail call, and the block still has a live result from the other arm.
	frame.thenReachable = reachable_;
	reachable_ = frame.entryReachable;
	stackWidths_.resize(frame.baseDepth);
	if (reachable_) {
		for (int width : frame.paramWidths)
			push(width);
	}
}

void OpcodeDispatcher::dispatchEnd()
{
	if (controls_.empty()) {
		// The function's terminating `end` is an implicit return.
		if (!measureOnly_) {
			if (reachable_)
				emitReturnValue();
			else if (frameAbi_ || resultWidth_ == 0) {
				// Frame mode returns void; a typed return here would violate the
				// emitted signature (this path is dead after a tail call).
				emit(sljit_emit_return_void(compiler_));
			} else if (resultWidth_ != 0) {
				if (isFloatKind(resultKind_)) {
					emit(resultWidth_ == 4
							? sljit_emit_fset32(compiler_, SLJIT_FR0, 0.0f)
							: sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0));
					emit(sljit_emit_return(compiler_,
							resultWidth_ == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
							SLJIT_FR0, 0));
				} else {
					emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0));
					emit(sljit_emit_return(compiler_,
							resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV, SLJIT_R0, 0));
				}
			} else {
				emit(sljit_emit_return_void(compiler_));
			}
		}
		reachable_ = false;
		return;
	}

	ControlFrame frame = std::move(controls_.back());
	controls_.pop_back();

	bool endReachable = false;
	if (frame.kind == ControlKind::If && !frame.hasElse) {
		require(frame.resultWidths.empty(),
				"an if without else cannot produce a result");
		endReachable = frame.entryReachable || frame.endTargeted;
		if (!measureOnly_ && frame.alternateJump != nullptr) {
			sljit_label* label = sljit_emit_label(compiler_);
			if (label == nullptr)
				ok_ = false;
			else
				sljit_set_label(frame.alternateJump, label);
		}
	} else {
		endReachable = (frame.entryReachable && reachable_)
				|| frame.thenReachable || frame.endTargeted;
	}

	if (!measureOnly_) {
		sljit_label* endLabel = sljit_emit_label(compiler_);
		if (endLabel == nullptr) {
			ok_ = false;
		} else {
			for (sljit_jump* jump : frame.endJumps) {
				if (jump != nullptr)
					sljit_set_label(jump, endLabel);
			}
		}
	}

	reachable_ = endReachable;
	stackWidths_.resize(frame.baseDepth);
	if (endReachable) {
		for (int width : frame.resultWidths)
			push(width);
	}
}

void OpcodeDispatcher::emitBranchTo(ControlFrame& target)
{
	const bool toLoop = target.kind == ControlKind::Loop;
	const std::vector<int>& widths = toLoop ? target.paramWidths : target.resultWidths;
	require(stackWidths_.size() >= widths.size(),
			"the branch does not carry the label's values");
	if (measureOnly_) {
		if (!toLoop)
			target.endTargeted = true;
		return;
	}
	transferToBase(widths, target.baseDepth);
	sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (jump == nullptr) {
		ok_ = false;
		return;
	}
	if (toLoop) {
		if (target.begin == nullptr) {
			ok_ = false;
			return;
		}
		sljit_set_label(jump, target.begin);
	} else {
		target.endJumps.push_back(jump);
		target.endTargeted = true;
	}
}

void OpcodeDispatcher::dispatchBr(WASM::LabelIdx depth)
{
	require(reachable_, "a br appears in unreachable code");
	require(depth <= controls_.size(), "the br label depth is out of range");
	if (depth == controls_.size()) {
		if (!measureOnly_)
			emitReturnValue();
		markUnreachable();
		return;
	}
	emitBranchTo(controls_[controls_.size() - 1 - depth]);
	markUnreachable();
}

void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx depth)
{
	require(reachable_, "a br_if appears in unreachable code");
	require(depth <= controls_.size(), "the br_if label depth is out of range");
	requireStack(1);
	require(stackWidths_.back() == 4, "the br_if condition must be an i32");
	pop();
	if (!measureOnly_)
		loadSlot(SLJIT_R0, stackWidths_.size(), 4);

	if (depth == controls_.size()) {
		if (!measureOnly_) {
			sljit_jump* skip = sljit_emit_cmp(compiler_, SLJIT_EQUAL | SLJIT_32,
					SLJIT_R0, 0, SLJIT_IMM, 0);
			if (skip == nullptr)
				ok_ = false;
			emitReturnValue();
			sljit_label* skipLabel = sljit_emit_label(compiler_);
			if (skipLabel == nullptr)
				ok_ = false;
			else if (skip != nullptr)
				sljit_set_label(skip, skipLabel);
		}
		return;
	}

	ControlFrame& target = controls_[controls_.size() - 1 - depth];
	const std::vector<int>& widths =
			target.kind == ControlKind::Loop ? target.paramWidths : target.resultWidths;
	require(stackWidths_.size() >= widths.size(),
			"the br_if does not carry the label's values");
	if (measureOnly_) {
		if (target.kind != ControlKind::Loop)
			target.endTargeted = true;
		return;
	}

	sljit_jump* skip = sljit_emit_cmp(compiler_, SLJIT_EQUAL | SLJIT_32,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (skip == nullptr)
		ok_ = false;
	transferToBase(widths, target.baseDepth);
	sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (jump == nullptr) {
		ok_ = false;
	} else if (target.kind == ControlKind::Loop) {
		if (target.begin == nullptr)
			ok_ = false;
		else
			sljit_set_label(jump, target.begin);
	} else {
		target.endJumps.push_back(jump);
		target.endTargeted = true;
	}
	sljit_label* skipLabel = sljit_emit_label(compiler_);
	if (skipLabel == nullptr)
		ok_ = false;
	else if (skip != nullptr)
		sljit_set_label(skip, skipLabel);
}

void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& depths,
		WASM::LabelIdx defaultDepth)
{
	require(reachable_, "a br_table appears in unreachable code");
	requireStack(1);
	require(stackWidths_.back() == 4, "the br_table selector must be an i32");
	pop();

	const std::vector<int> functionWidths =
			resultWidth_ == 0 ? std::vector<int>{} : std::vector<int>{ resultWidth_ };
	const auto widthsOf = [&](WASM::LabelIdx depth) -> const std::vector<int>& {
		if (depth == controls_.size())
			return functionWidths;
		const ControlFrame& target = controls_[controls_.size() - 1 - depth];
		return target.kind == ControlKind::Loop ? target.paramWidths : target.resultWidths;
	};
	require(defaultDepth <= controls_.size(), "the br_table label depth is out of range");
	const std::vector<int> reference = widthsOf(defaultDepth);
	for (WASM::LabelIdx depth : depths) {
		require(depth <= controls_.size(), "the br_table label depth is out of range");
		require(widthsOf(depth) == reference,
				"br_table targets require identical label signatures");
	}
	require(stackWidths_.size() >= reference.size(),
			"the br_table does not carry the label's values");

	if (!measureOnly_) {
		loadSlot(SLJIT_R0, stackWidths_.size(), 4);
	}

	std::vector<sljit_jump*> matches;
	if (!measureOnly_) {
		matches.reserve(depths.size());
		for (std::size_t i = 0; i < depths.size(); ++i) {
			sljit_jump* match = sljit_emit_cmp(compiler_, SLJIT_EQUAL | SLJIT_32,
					SLJIT_R0, 0, SLJIT_IMM, static_cast<sljit_sw>(i));
			if (match == nullptr)
				ok_ = false;
			matches.push_back(match);
		}
	}
	sljit_jump* defaultJump = nullptr;
	if (!measureOnly_) {
		defaultJump = sljit_emit_jump(compiler_, SLJIT_JUMP);
		if (defaultJump == nullptr)
			ok_ = false;
	}

	const auto emitTarget = [&](WASM::LabelIdx depth) {
		if (depth == controls_.size()) {
			emitReturnValue();
			return;
		}
		ControlFrame& target = controls_[controls_.size() - 1 - depth];
		const std::vector<int>& widths =
				target.kind == ControlKind::Loop ? target.paramWidths : target.resultWidths;
		transferToBase(widths, target.baseDepth);
		sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
		if (jump == nullptr) {
			ok_ = false;
			return;
		}
		if (target.kind == ControlKind::Loop) {
			if (target.begin == nullptr)
				ok_ = false;
			else
				sljit_set_label(jump, target.begin);
		} else {
			target.endJumps.push_back(jump);
			target.endTargeted = true;
		}
	};

	if (!measureOnly_) {
		for (std::size_t i = 0; i < depths.size(); ++i) {
			sljit_label* caseLabel = sljit_emit_label(compiler_);
			if (caseLabel == nullptr) {
				ok_ = false;
				continue;
			}
			if (matches[i] != nullptr)
				sljit_set_label(matches[i], caseLabel);
			emitTarget(depths[i]);
		}
		sljit_label* defaultLabel = sljit_emit_label(compiler_);
		if (defaultLabel == nullptr)
			ok_ = false;
		else if (defaultJump != nullptr)
			sljit_set_label(defaultJump, defaultLabel);
		emitTarget(defaultDepth);
	} else {
		for (WASM::LabelIdx depth : depths) {
			if (depth != controls_.size()
					&& controls_[controls_.size() - 1 - depth].kind != ControlKind::Loop)
				controls_[controls_.size() - 1 - depth].endTargeted = true;
		}
		if (defaultDepth != controls_.size()
				&& controls_[controls_.size() - 1 - defaultDepth].kind != ControlKind::Loop)
			controls_[controls_.size() - 1 - defaultDepth].endTargeted = true;
	}
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturn()
{
	require(reachable_, "a return appears in unreachable code");
	if (!measureOnly_)
		emitReturnValue();
	markUnreachable();
}

void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	// Dead code: the tag's operand stack is polymorphic and the throw can never
	// execute.
	if (!reachable_)
		return;
	require(arg < tagIds_.size(), "throw names an out-of-range tag");

	const WASM::TagId tagId = tagIds_[arg];
	const WASM::FuncType payload = tagPayloadType(arg);
	const std::size_t payloadCount = payload.params.size();
	requireStack(payloadCount);
	const std::size_t base = stackWidths_.size() - payloadCount;

	if (!measureOnly_) {
		const ExceptionPayloadLayout layout = exceptionPayloadLayout(payload);

		// Materialise the exception object: its payload is written below, and
		// both a local catch and a caller need the object, not the stack values.
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(tagId)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(layout.size)));
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitExnCreate);
		// An allocation failure is a trap; otherwise R0 is the payload pointer.
		emitPendingTrapCheck();
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				exceptionPtrSlotOffset(), SLJIT_R0, 0));

		for (std::size_t i = 0; i < payloadCount; ++i) {
			const sljit_sw offset = static_cast<sljit_sw>(layout.fields[i].offset);
			const int width = widthOf(payload.params[i].val);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
					SLJIT_MEM1(SLJIT_SP), exceptionPtrSlotOffset()));
			if (width == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(base + i)));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1),
						offset, SLJIT_R0, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(base + i) + 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1),
						offset + 8, SLJIT_R0, 0));
			} else if (isFloatKind(payload.params[i].val.opcode)) {
				loadFloatSlot(SLJIT_FR0, base + i, width);
				emit(sljit_emit_fop1(compiler_,
						width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
						SLJIT_MEM1(SLJIT_R1), offset, SLJIT_FR0, 0));
			} else {
				loadSlot(SLJIT_R0, base + i, width);
				emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
						SLJIT_MEM1(SLJIT_R1), offset, SLJIT_R0, 0));
			}
		}

		// Publish the exception, then route it through the local try_tables.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
				SLJIT_MEM1(SLJIT_SP), exceptionPtrSlotOffset()));
		emitHelper(SLJIT_ARGS1V(P), &sljitExnSetPending);
		emitExceptionPropagation();
	}

	stackWidths_.resize(base);
	markUnreachable();
}

void OpcodeDispatcher::dispatchThrowRef()
{
	if (!reachable_)
		return;
	requireStack(1);
	require(stackWidths_.back() == 8, "throw_ref requires an exnref operand");
	const std::size_t depth = stackWidths_.size() - 1;

	if (!measureOnly_) {
		loadSlot(SLJIT_R0, depth, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullReference);
		sljit_label* after = sljit_emit_label(compiler_);
		if (after == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, after);
		emitHelper(SLJIT_ARGS1V(P), &sljitExnSetPending);
		emitExceptionPropagation();
	}

	pop();
	markUnreachable();
}

void OpcodeDispatcher::emitResolvedCall(const WASM::FuncType& type, int callableReg)
{
	// Load the target and its context out of the resolved `Callable`.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(callableReg),
			static_cast<sljit_sw>(offsetof(WASM::Callable, fnPtr))));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(callableReg),
			static_cast<sljit_sw>(offsetof(WASM::Callable, context))));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), targetSlotOffset(),
			SLJIT_R1, 0));

	// A native import carries no context: it runs against the caller's.
	emit(sljit_emit_op2u(compiler_, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R2, 0, SLJIT_IMM, 0));
	emit(sljit_emit_select(compiler_, SLJIT_EQUAL, SLJIT_R2,
			SLJIT_MEM1(SLJIT_SP), contextSlotOffset(), SLJIT_R2));

	// Frame form: pack the arguments into the caller's staging scratch area, call
	// `(context, frame)`, then copy the results back into the operand slots
	// (docs/backends/STAGING_CALL_ABI.md, step 3b).
	if (frameAbi_) {
		const std::size_t base = stackWidths_.size() - type.params.size();
		std::vector<WASM::ValueKind> argumentKinds;
		argumentKinds.reserve(type.params.size());
		for (const WASM::StorageType& parameter : type.params) {
			if (isFloatKind(parameter.val.opcode))
				onUnhandledOpcode("a float argument to a frame-form call");
			argumentKinds.push_back(WASM::valueKindOf(parameter.val.opcode));
		}
		const std::uint32_t argumentBytes = WASM::frameArgumentsBytes(argumentKinds);
		const int resultWidth = type.results.empty() ? 0 : widthOf(type.results.front().val);
		const std::uint32_t resultBytes = resultWidth == 0 ? 0u
				: WASM::frameValueBytes(WASM::valueKindOf(type.results.front().val.opcode));
		if (argumentBytes + resultBytes > 256u) {
			onUnhandledOpcode("a frame-form call whose frame exceeds the staging scratch");
			return;
		}

		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R2, 0));
		// `sljit_get_local_base(compiler, dst, dstw, offset)`: dstw must be zero.
		emit(sljit_get_local_base(compiler_, SLJIT_R1, 0, callFrameOffset()));
		for (std::size_t i = 0; i < type.params.size(); ++i) {
			const sljit_sw offset = static_cast<sljit_sw>(WASM::frameArgumentOffset(
					argumentKinds, static_cast<std::uint32_t>(i)));
			const int width = widthOf(type.params[i].val);
			if (width == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(base + i)));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), offset,
						SLJIT_R3, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(base + i) + 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), offset + 8,
						SLJIT_R3, 0));
			} else {
				loadSlot(SLJIT_R3, base + i, width);
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), offset,
						SLJIT_R3, 0));
			}
		}
		emit(sljit_emit_icall(compiler_, SLJIT_CALL, SLJIT_ARGS2V(P, P),
				SLJIT_MEM1(SLJIT_SP), targetSlotOffset()));
		if (resultWidth != 0) {
			// R1 is caller-saved, so the scratch address is taken again.
			emit(sljit_get_local_base(compiler_, SLJIT_R1, 0, callFrameOffset()));
			if (resultWidth == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0,
						SLJIT_MEM1(SLJIT_R1), static_cast<sljit_sw>(argumentBytes)));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						stackSlotOffset(base), SLJIT_R3, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0,
						SLJIT_MEM1(SLJIT_R1), static_cast<sljit_sw>(argumentBytes) + 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						stackSlotOffset(base) + 8, SLJIT_R3, 0));
			} else {
				emit(sljit_emit_op1(compiler_, resultWidth == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
						SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1),
						static_cast<sljit_sw>(argumentBytes)));
				storeSlot(SLJIT_R3, base, resultWidth);
			}
		}
		emitPendingExceptionCheck();
		return;
	}

	const std::size_t base = stackWidths_.size() - type.params.size();
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R2, 0));
	std::uint32_t integerIndex = 0;
	std::uint32_t floatIndex = 0;
	for (std::size_t i = 0; i < type.params.size(); ++i) {
		const WASM::ValueTypeCode kind = type.params[i].val.opcode;
		const int width = widthOf(type.params[i].val);
		if (isFloatKind(kind)) {
			loadFloatSlot(SLJIT_FR(static_cast<sljit_s32>(floatIndex)), base + i, width);
			++floatIndex;
		} else {
			loadSlot(SLJIT_R(static_cast<int>(integerIndex) + 1), base + i, width);
			++integerIndex;
		}
	}

	const int resultWidth = type.results.empty() ? 0 : widthOf(type.results.front().val);
	const WASM::ValueTypeCode resultKind = type.results.empty()
			? WASM::ValueTypeCode::Void : type.results.front().val.opcode;
	emit(sljit_emit_icall(compiler_, SLJIT_CALL,
			callArgTypes(type, resultKind, resultWidth),
			SLJIT_MEM1(SLJIT_SP), targetSlotOffset()));
	if (resultWidth != 0) {
		if (isFloatKind(resultKind))
			storeFloatSlot(SLJIT_FR0, base, resultWidth);
		else
			storeSlot(SLJIT_R0, base, resultWidth);
	}
	emitPendingExceptionCheck();
}

void OpcodeDispatcher::emitCall(WASM::FuncIdx index, bool isTail)
{
	(void)isTail;
	require(reachable_, "a call appears in unreachable code");
	const std::size_t importedCount = module_.importFunctions.size();
	const std::size_t total = importedCount + module_.internalFunctionTypeIndices.size();
	require(index < total, "the call function index is out of range");
	const std::uint32_t typeIndex = index < importedCount
			? module_.importFunctions[index].typeIdx
			: module_.internalFunctionTypeIndices[index - importedCount];
	const WASM::FuncType calledType = module_.funcTypeAt(typeIndex);
	if (0 == static_cast<int>(frameAbi_))
		require(calledType.params.size() <= kMaxWasmParameters,
			"the SLJIT backend cannot call a function with more than three parameters");
	require(calledType.results.size() <= 1,
			"the SLJIT backend cannot call a multi-result function");
	for (const WASM::StorageType& parameter : calledType.params)
		require(widthOf(parameter.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	for (const WASM::StorageType& result : calledType.results)
		require(widthOf(result.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	requireStack(calledType.params.size());

	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(index)));
		emitHelper(SLJIT_ARGS2(P, P, 32), &ModuleCompiler::resolveCallable);
		sljit_jump* resolved = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (resolved == nullptr)
			ok_ = false;
		emitTrapBranch();
		sljit_label* continueLabel = sljit_emit_label(compiler_);
		if (continueLabel == nullptr)
			ok_ = false;
		else if (resolved != nullptr)
			sljit_set_label(resolved, continueLabel);
		emitResolvedCall(calledType, SLJIT_R0);
	}

	stackWidths_.resize(stackWidths_.size() - calledType.params.size());
	if (!calledType.results.empty())
		push(widthOf(calledType.results.front().val));
}

void OpcodeDispatcher::emitCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex,
		bool isTail)
{
	(void)isTail;
	require(reachable_, "a call_indirect appears in unreachable code");
	requireTableIndex(tableIndex);
	require(static_cast<std::uint32_t>(typeIndex) < module_.typeCount(),
			"the call_indirect type index is out of range");
	const WASM::FuncType calledType = module_.funcTypeAt(typeIndex);
	if (0 == static_cast<int>(frameAbi_))
		require(calledType.params.size() <= kMaxWasmParameters,
			"the SLJIT backend cannot call a function with more than three parameters");
	require(calledType.results.size() <= 1,
			"the SLJIT backend cannot call a multi-result function");
	for (const WASM::StorageType& parameter : calledType.params)
		require(widthOf(parameter.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	for (const WASM::StorageType& result : calledType.results)
		require(widthOf(result.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	requireStack(calledType.params.size() + 1);
	require(stackWidths_.back() == 4, "the call_indirect selector must be an i32");
	// The selector sits above the arguments; consume it before the shared call
	// path computes the argument-slot base.
	const std::size_t selectorDepth = stackWidths_.size() - 1;
	pop();

	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(tableIndex)));
		loadSlot(SLJIT_R2, selectorDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>(module_.typeId(WASM::LocalTypeIdx{ typeIndex }).value)));
		emitHelper(SLJIT_ARGS4(P, P, 32, 32, 32), &sljitResolveIndirectCallable);
		sljit_jump* resolved = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (resolved == nullptr)
			ok_ = false;
		emitTrapBranch();
		sljit_label* continueLabel = sljit_emit_label(compiler_);
		if (continueLabel == nullptr)
			ok_ = false;
		else if (resolved != nullptr)
			sljit_set_label(resolved, continueLabel);
		emitResolvedCall(calledType, SLJIT_R0);
	}

	stackWidths_.resize(stackWidths_.size() - calledType.params.size());
	if (!calledType.results.empty())
		push(widthOf(calledType.results.front().val));
}

void OpcodeDispatcher::dispatchCall(WASM::FuncIdx arg)
{
	emitCall(arg, false);
}

void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	emitCallIndirect(arg1, arg2, false);
}

void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	// A direct tail call to this function is a loop: the frame is reused and the
	// host stack does not grow. Mutual and indirect tail calls still take the
	// semantic call + return lowering until the cross-function trampoline lands.
	if (arg == functionIndex_) {
		emitSelfTailCall();
		return;
	}
	const std::uint32_t importedCount = static_cast<std::uint32_t>(module_.importFunctions.size());
	if (arg < importedCount + module_.internalFunctionTypeIndices.size()) {
		const std::uint32_t typeIndex = arg < importedCount
				? module_.importFunctions[arg].typeIdx
				: module_.internalFunctionTypeIndices[arg - importedCount];
		const WASM::FuncType calleeType = module_.funcTypeAt(typeIndex);
		const bool supported = frameAbi_
				? WASM::tailCallFrameSupported(calleeType)
				: WASM::tailCallSlotsSupported(calleeType);
		if (tailTrampoline_ && supported) {
			emitTailCallState(arg, calleeType);
			return;
		}
	}
	// A frame-form tail call is a call plus a frame-mode return: the callee
	// writes its result into the staging frame and `emitReturnValue` moves it
	// into the caller's result block. This is not a true trampoline (the host
	// stack still grows); the register form has the same limitation.
	emitCall(arg, true);
	if (!measureOnly_)
		emitReturnValue();
	markUnreachable();
}

void OpcodeDispatcher::emitSelfTailCall()
{
	require(reachable_, "a return_call appears in unreachable code");
	const std::size_t parameterCount = funcType_.params.size();
	requireStack(parameterCount);
	const std::size_t base = stackWidths_.size() - parameterCount;

	if (!measureOnly_) {
		// The new arguments become the parameter locals. A `v128` parameter is a
		// 16-byte cell, so it uses the same slot copy as a vector local.
		for (std::size_t i = 0; i < parameterCount; ++i) {
			const int width = widthOf(funcType_.params[i].val);
			if (width == 16) {
				copyStackToLocal(base + i, i);
			} else if (isFloatKind(funcType_.params[i].val.opcode)) {
				loadFloatSlot(SLJIT_FR0, base + i, width);
				emit(sljit_emit_fop1(compiler_,
						width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
						SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_FR0, 0));
			} else {
				loadSlot(SLJIT_R0, base + i, width);
				emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
						SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_R0, 0));
			}
		}
		// A tail call is a fresh invocation: declared locals start at zero.
		for (std::size_t i = parameterCount; i < localSlots_; ++i) {
			if (localWidths_[i] == 16) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						localSlotOffset(i), SLJIT_IMM, 0));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						localSlotOffset(i) + 8, SLJIT_IMM, 0));
			} else {
				emit(sljit_emit_op1(compiler_,
						localWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
						SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_IMM, 0));
			}
		}
		sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
		if (jump == nullptr)
			ok_ = false;
		else if (bodyStart_ != nullptr)
			sljit_set_label(jump, bodyStart_);
		else
			ok_ = false;
	}

	stackWidths_.resize(base);
	markUnreachable();
}

void OpcodeDispatcher::emitTailCallState(WASM::FuncIdx arg, const WASM::FuncType& type)
{
	require(reachable_, "a return_call appears in unreachable code");
	requireStack(type.params.size());
	const std::size_t base = stackWidths_.size() - type.params.size();

	if (!measureOnly_) {
		// state = currentTailCallState(); the helper returns it in R0 and clobbers
		// R1-R3, so every state write happens after it without another call.
		emitHelper(SLJIT_ARGS0(P), &WASM::currentTailCallState);
		const std::uint32_t importedCount = static_cast<std::uint32_t>(module_.importFunctions.size());
		const void* target = arg < importedCount
				? static_cast<const void*>(&internals_.importStorage[arg])
				: static_cast<const void*>(&internals_.internalCallables[arg - importedCount]);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(reinterpret_cast<std::uintptr_t>(target))));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
				static_cast<sljit_sw>(offsetof(WASM::TailCallState, target)), SLJIT_R1, 0));
		for (std::size_t i = 0; i < type.params.size(); ++i) {
			const int width = widthOf(type.params[i].val);
			if (width == 16) {
				// A vector occupies the whole uniform slot.
				for (int word = 0; word < 2; ++word) {
					emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
							SLJIT_MEM1(SLJIT_SP),
							stackSlotOffset(base + i) + word * 8));
					emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
							static_cast<sljit_sw>(offsetof(WASM::TailCallState, args)
									+ i * WASM::kTailCallSlotBytes + word * 8),
							SLJIT_R1, 0));
				}
			} else {
				loadSlot(SLJIT_R1, base + i, width);
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
						static_cast<sljit_sw>(offsetof(WASM::TailCallState, args)
								+ i * WASM::kTailCallSlotBytes), SLJIT_R1, 0));
			}
		}
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R0),
				static_cast<sljit_sw>(offsetof(WASM::TailCallState, pending)), SLJIT_IMM, 1));
		if (frameAbi_)
			emit(sljit_emit_return_void(compiler_));
		else
			emitTailCallDummyReturn();
	}

	stackWidths_.resize(base);
	markUnreachable();
}

void OpcodeDispatcher::emitTailCallDummyReturn()
{
	if (resultKinds_.size() > 1 || resultWidth_ == 0) {
		emit(sljit_emit_return_void(compiler_));
	} else if (isFloatKind(resultKind_)) {
		// The driver ignores the dummy result; it exists only for the ABI.
		emit(resultWidth_ == 4
				? sljit_emit_fset32(compiler_, SLJIT_FR0, 0.0f)
				: sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0));
		emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
				SLJIT_FR0, 0));
	} else {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0));
		emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_R1, 0));
	}
}

void OpcodeDispatcher::emitTailCallIndirectState(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex)
{
	require(reachable_, "a return_call_indirect appears in unreachable code");
	requireTableIndex(tableIndex);
	require(static_cast<std::uint32_t>(typeIndex) < module_.typeCount(),
			"the return_call_indirect type index is out of range");
	const WASM::FuncType calledType = module_.funcTypeAt(typeIndex);
	require(calledType.results.size() <= 1,
			"the SLJIT backend cannot tail-call a multi-result function");
	for (const WASM::StorageType& parameter : calledType.params)
		require(widthOf(parameter.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	for (const WASM::StorageType& result : calledType.results)
		require(widthOf(result.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	requireStack(calledType.params.size() + 1);
	require(stackWidths_.back() == 4, "the return_call_indirect selector must be an i32");
	const std::size_t selectorDepth = stackWidths_.size() - 1;
	const std::size_t base = selectorDepth - calledType.params.size();

	if (measureOnly_) {
		stackWidths_.resize(base);
		markUnreachable();
		return;
	}

	pop();
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(tableIndex)));
	loadSlot(SLJIT_R2, selectorDepth, 4);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(module_.typeId(WASM::LocalTypeIdx{ typeIndex }).value)));
	emitHelper(SLJIT_ARGS4(P, P, 32, 32, 32), &sljitResolveIndirectCallable);
	sljit_jump* resolved = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (resolved == nullptr)
		ok_ = false;
	emitTrapBranch();
	sljit_label* continueLabel = sljit_emit_label(compiler_);
	if (continueLabel == nullptr)
		ok_ = false;
	else if (resolved != nullptr)
		sljit_set_label(resolved, continueLabel);

	// A callable without a thunk (an embedder-registered foreign funcref) takes
	// the semantic call + return path below.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::Callable, tailEntry))));
	sljit_jump* fallback = sljit_emit_cmp(compiler_, SLJIT_EQUAL,
			SLJIT_R1, 0, SLJIT_IMM, 0);
	if (fallback == nullptr)
		ok_ = false;

	// Reach the state helper without losing the callable: it lives in the
	// reserved scratch slot until the helper returns.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset(), SLJIT_R0, 0));
	emitHelper(SLJIT_ARGS0(P), &WASM::currentTailCallState);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset()));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::TailCallState, target)), SLJIT_R1, 0));
	for (std::size_t i = 0; i < calledType.params.size(); ++i) {
		loadSlot(SLJIT_R1, base + i, widthOf(calledType.params[i].val));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
				static_cast<sljit_sw>(offsetof(WASM::TailCallState, args)
						+ i * WASM::kTailCallSlotBytes), SLJIT_R1, 0));
	}
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::TailCallState, pending)), SLJIT_IMM, 1));
	emitTailCallDummyReturn();

	sljit_label* fallbackLabel = sljit_emit_label(compiler_);
	if (fallbackLabel == nullptr)
		ok_ = false;
	else if (fallback != nullptr)
		sljit_set_label(fallback, fallbackLabel);
	emitResolvedCall(calledType, SLJIT_R0);
	emitReturnValue();

	stackWidths_.resize(base);
	markUnreachable();
}

void OpcodeDispatcher::emitNullRefCheck(int callableReg)
{
	sljit_jump* nonNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			callableReg, 0, SLJIT_IMM, 0);
	if (nonNull == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(WASM::TrapKind::NullFunctionReference)));
	emitHelper(SLJIT_ARGS1V(32), &sljitReportTrap);
	emitTrapBranch();
	sljit_label* continuation = sljit_emit_label(compiler_);
	if (continuation == nullptr)
		ok_ = false;
	else if (nonNull != nullptr)
		sljit_set_label(nonNull, continuation);
}

void OpcodeDispatcher::emitTailCallStateFromReg(int callableReg, const WASM::FuncType& type)
{
	// The callable must survive `currentTailCallState`, which clobbers R0-R3.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset(), callableReg, 0));
	emitHelper(SLJIT_ARGS0(P), &WASM::currentTailCallState);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset()));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::TailCallState, target)), SLJIT_R1, 0));
	const std::size_t base = stackWidths_.size() - type.params.size();
	for (std::size_t i = 0; i < type.params.size(); ++i) {
		const int width = widthOf(type.params[i].val);
		if (width == 16) {
			for (int word = 0; word < 2; ++word) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
						SLJIT_MEM1(SLJIT_SP), stackSlotOffset(base + i) + word * 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
						static_cast<sljit_sw>(offsetof(WASM::TailCallState, args)
								+ i * WASM::kTailCallSlotBytes + word * 8), SLJIT_R1, 0));
			}
		} else {
			loadSlot(SLJIT_R1, base + i, width);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
					static_cast<sljit_sw>(offsetof(WASM::TailCallState, args)
							+ i * WASM::kTailCallSlotBytes), SLJIT_R1, 0));
		}
	}
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::TailCallState, pending)), SLJIT_IMM, 1));
	if (frameAbi_)
		emit(sljit_emit_return_void(compiler_));
	else
		emitTailCallDummyReturn();
}

void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx typeIndex)
{
	require(reachable_, "a call_ref appears in unreachable code");
	require(static_cast<std::uint32_t>(typeIndex) < module_.typeCount(),
			"the call_ref type index is out of range");
	const WASM::FuncType calledType = module_.funcTypeAt(typeIndex);
	require(calledType.results.size() <= 1,
			"the SLJIT backend cannot call a multi-result function");
	if (!frameAbi_)
		require(calledType.params.size() <= kMaxWasmParameters,
				"the SLJIT backend cannot call a function with more than three parameters");
	for (const WASM::StorageType& parameter : calledType.params)
		require(widthOf(parameter.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	for (const WASM::StorageType& result : calledType.results)
		require(widthOf(result.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	requireStack(calledType.params.size() + 1);
	const std::size_t callableDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, callableDepth, 8);
		emitNullRefCheck(SLJIT_R0);
		emitResolvedCall(calledType, SLJIT_R0);
	}
	stackWidths_.resize(stackWidths_.size() - calledType.params.size());
	if (!calledType.results.empty())
		push(widthOf(calledType.results.front().val));
}

void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx typeIndex)
{
	require(reachable_, "a return_call_ref appears in unreachable code");
	require(static_cast<std::uint32_t>(typeIndex) < module_.typeCount(),
			"the return_call_ref type index is out of range");
	const WASM::FuncType calledType = module_.funcTypeAt(typeIndex);
	require(calledType.results.size() <= 1,
			"the SLJIT backend cannot tail-call a multi-result function");
	if (!frameAbi_)
		require(calledType.params.size() <= kMaxWasmParameters,
				"the SLJIT backend cannot tail-call a function with more than three parameters");
	for (const WASM::StorageType& parameter : calledType.params)
		require(widthOf(parameter.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	for (const WASM::StorageType& result : calledType.results)
		require(widthOf(result.val) != 0, "the SLJIT backend only calls i32/i64 signatures");
	requireStack(calledType.params.size() + 1);
	const std::size_t callableDepth = stackWidths_.size() - 1;
	const std::size_t base = callableDepth - calledType.params.size();
	pop();
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, callableDepth, 8);
		emitNullRefCheck(SLJIT_R0);
		const bool supported = frameAbi_
				? WASM::tailCallFrameSupported(calledType)
				: WASM::tailCallSlotsSupported(calledType);
		if (tailTrampoline_ && supported) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0),
					static_cast<sljit_sw>(offsetof(WASM::Callable, tailEntry))));
			sljit_jump* fallback = sljit_emit_cmp(compiler_, SLJIT_EQUAL,
					SLJIT_R1, 0, SLJIT_IMM, 0);
			if (fallback == nullptr)
				ok_ = false;
			emitTailCallStateFromReg(SLJIT_R0, calledType);
			sljit_label* fallbackLabel = sljit_emit_label(compiler_);
			if (fallbackLabel == nullptr)
				ok_ = false;
			else if (fallback != nullptr)
				sljit_set_label(fallback, fallbackLabel);
			emitResolvedCall(calledType, SLJIT_R0);
			emitReturnValue();
		} else {
			emitResolvedCall(calledType, SLJIT_R0);
			emitReturnValue();
		}
	}
	stackWidths_.resize(base);
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	if (tailTrampoline_ && !frameAbi_) {
		require(static_cast<std::uint32_t>(arg1) < module_.typeCount(),
				"the return_call_indirect type index is out of range");
		const WASM::FuncType calledType = module_.funcTypeAt(arg1);
		if (WASM::tailCallSlotsSupported(calledType)) {
			emitTailCallIndirectState(arg1, arg2);
			return;
		}
	}
	// A frame-form tail call is a call plus a frame-mode return: the callee
	// writes its result into the staging frame and `emitReturnValue` moves it
	// into the caller's result block. This is not a true trampoline (the host
	// stack still grows); the register form has the same limitation.
	emitCallIndirect(arg1, arg2, true);
	if (!measureOnly_)
		emitReturnValue();
	markUnreachable();
}

void OpcodeDispatcher::dispatchDrop()
{
	if (!reachable_)
		return;
	requireStack(1);
	pop();
}

void OpcodeDispatcher::dispatchSelect()
{
	if (!reachable_)
		return;
	requireStack(3);
	const int valueWidth = stackWidths_[stackWidths_.size() - 3];
	require(stackWidths_[stackWidths_.size() - 2] == valueWidth,
			"select requires two values of the same type");
	require(stackWidths_.back() == 4, "select requires an i32 condition");
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
		emit(sljit_emit_op2u(compiler_, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0,
				SLJIT_IMM, 0));
		if (valueWidth == 16) {
			// A v128 is 16 bytes, so both words are selected under the same flags.
			const sljit_sw aOffset = stackSlotOffset(stackWidths_.size() - 3);
			const sljit_sw bOffset = stackSlotOffset(stackWidths_.size() - 2);
			for (int word = 0; word < 2; ++word) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
						SLJIT_MEM1(SLJIT_SP), aOffset + word * 8));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0,
						SLJIT_MEM1(SLJIT_SP), bOffset + word * 8));
				emit(sljit_emit_select(compiler_, SLJIT_NOT_EQUAL,
						SLJIT_R1, SLJIT_R1, 0, SLJIT_R2));
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
						aOffset + word * 8, SLJIT_R1, 0));
			}
		} else {
			loadSlot(SLJIT_R1, stackWidths_.size() - 3, valueWidth);
			loadSlot(SLJIT_R2, stackWidths_.size() - 2, valueWidth);
			emit(sljit_emit_select(compiler_,
					SLJIT_NOT_EQUAL | (valueWidth == 4 ? SLJIT_32 : 0),
					SLJIT_R1, SLJIT_R1, 0, SLJIT_R2));
			storeSlot(SLJIT_R1, stackWidths_.size() - 3, valueWidth);
		}
	}
	stackWidths_.resize(stackWidths_.size() - 2);
}

void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& types)
{
	require(types.size() == 1, "select_t requires exactly one value type");
	require(widthOf(types.front()) != 0,
			"select_t only supports the modelled value types");
	dispatchSelect();
}

void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	if (!reachable_)
		return;
	requireLocal(arg, "local.get");
	push(localWidths_[arg]);
	if (measureOnly_)
		return;
	if (localWidths_[arg] == 16) {
		copyLocalToStack(arg, stackWidths_.size() - 1);
		return;
	}
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg)));
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, localWidths_[arg]);
}

void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	if (!reachable_)
		return;
	requireLocal(arg, "local.set");
	requireStack(1);
	pop();
	if (measureOnly_)
		return;
	if (localWidths_[arg] == 16) {
		copyStackToLocal(stackWidths_.size(), arg);
		return;
	}
	loadSlot(SLJIT_R0, stackWidths_.size(), localWidths_[arg]);
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg), SLJIT_R0, 0));
}

void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	if (!reachable_)
		return;
	requireLocal(arg, "local.tee");
	requireStack(1);
	if (measureOnly_)
		return;
	if (localWidths_[arg] == 16) {
		copyStackToLocal(stackWidths_.size() - 1, arg);
		return;
	}
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, localWidths_[arg]);
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg), SLJIT_R0, 0));
}

void OpcodeDispatcher::dispatchI32Const(std::int32_t arg)
{
	if (!reachable_)
		return;
	push(4);
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP),
			stackSlotOffset(stackWidths_.size() - 1), SLJIT_IMM,
			static_cast<sljit_sw>(arg)));
}

void OpcodeDispatcher::dispatchI64Const(std::int64_t arg)
{
	if (!reachable_)
		return;
	push(8);
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			stackSlotOffset(stackWidths_.size() - 1), SLJIT_IMM,
			static_cast<sljit_sw>(arg)));
}

void OpcodeDispatcher::emitIntegerBinary(sljit_s32 wordOp, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	const sljit_s32 load = width == 4 ? SLJIT_MOV_U32 : SLJIT_MOV;
	const sljit_s32 store = width == 4 ? SLJIT_MOV32 : SLJIT_MOV;
	const sljit_s32 op = width == 4 ? (wordOp | SLJIT_32) : wordOp;
	const sljit_sw left = stackSlotOffset(stackWidths_.size() - 1);
	const sljit_sw right = stackSlotOffset(stackWidths_.size());
	emit(sljit_emit_op1(compiler_, load, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), left));
	emit(sljit_emit_op1(compiler_, load, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), right));
	emit(sljit_emit_op2(compiler_, op, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0));
	emit(sljit_emit_op1(compiler_, store, SLJIT_MEM1(SLJIT_SP), left, SLJIT_R0, 0));
}

void OpcodeDispatcher::emitIntegerUnary(sljit_s32 wordOp, int width, int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	const sljit_s32 op = width == 4 ? (wordOp | SLJIT_32) : wordOp;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	emit(sljit_emit_op1(compiler_, op, SLJIT_R0, 0, SLJIT_R0, 0));
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, resultWidth);
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::emitCompare(sljit_s32 condition, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	const sljit_s32 type = width == 4 ? (condition | SLJIT_32) : condition;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	loadSlot(SLJIT_R1, stackWidths_.size(), width);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
	sljit_jump* taken = sljit_emit_cmp(compiler_, type, SLJIT_R0, 0, SLJIT_R1, 0);
	if (taken == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
	sljit_label* done = sljit_emit_label(compiler_);
	if (done == nullptr)
		ok_ = false;
	else if (taken != nullptr)
		sljit_set_label(taken, done);
	storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::emitEqz(int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	const sljit_s32 type = width == 4 ? (SLJIT_EQUAL | SLJIT_32) : SLJIT_EQUAL;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
	sljit_jump* taken = sljit_emit_cmp(compiler_, type, SLJIT_R0, 0, SLJIT_IMM, 0);
	if (taken == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
	sljit_label* done = sljit_emit_label(compiler_);
	if (done == nullptr)
		ok_ = false;
	else if (taken != nullptr)
		sljit_set_label(taken, done);
	storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::emitCheckedBinary(sljit_sw address, int width,
		const char* operation)
{
	if (!reachable_)
		return;
	(void)operation;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	loadSlot(SLJIT_R1, stackWidths_.size(), width);
	emitHelper(width == 4 ? SLJIT_ARGS2(32, 32, 32) : SLJIT_ARGS2(W, W, W), address);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	emitPendingTrapCheck();
	stackWidths_.back() = width;
}

void OpcodeDispatcher::loadFloatSlot(int freg, std::size_t depth, int width)
{
	emit(sljit_emit_fop1(compiler_, width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
			freg, 0, SLJIT_MEM1(SLJIT_SP), stackSlotOffset(depth)));
}

void OpcodeDispatcher::storeFloatSlot(int freg, std::size_t depth, int width)
{
	emit(sljit_emit_fop1(compiler_, width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
			SLJIT_MEM1(SLJIT_SP), stackSlotOffset(depth), freg, 0));
}

void OpcodeDispatcher::emitFloatUnary(sljit_s32 wordOp, int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	const sljit_s32 op = width == 4 ? (wordOp | SLJIT_32) : wordOp;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	emit(sljit_emit_fop1(compiler_, op, SLJIT_FR0, 0, SLJIT_FR0, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
}

void OpcodeDispatcher::emitFloatBinary(sljit_s32 wordOp, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	const sljit_s32 op = width == 4 ? (wordOp | SLJIT_32) : wordOp;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	loadFloatSlot(SLJIT_FR1, stackWidths_.size(), width);
	emit(sljit_emit_fop2(compiler_, op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
}

void OpcodeDispatcher::emitFloatHelper1(sljit_sw address, int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	emitHelper(width == 4 ? SLJIT_ARGS1(F32, F32) : SLJIT_ARGS1(F64, F64), address);
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
}

void OpcodeDispatcher::emitFloatHelper2(sljit_sw address, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	loadFloatSlot(SLJIT_FR1, stackWidths_.size(), width);
	emitHelper(width == 4 ? SLJIT_ARGS2(F32, F32, F32)
						  : SLJIT_ARGS2(F64, F64, F64), address);
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
}

void OpcodeDispatcher::emitFloatCompare(sljit_s32 condition, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	const sljit_s32 type = width == 4 ? (condition | SLJIT_32) : condition;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	loadFloatSlot(SLJIT_FR1, stackWidths_.size(), width);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
	sljit_jump* taken = sljit_emit_fcmp(compiler_, type,
			SLJIT_FR0, 0, SLJIT_FR1, 0);
	if (taken == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
	sljit_label* done = sljit_emit_label(compiler_);
	if (done == nullptr)
		ok_ = false;
	else if (taken != nullptr)
		sljit_set_label(taken, done);
	storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::emitFloatConst(double value, int width, std::size_t depth)
{
	if (measureOnly_)
		return;
	if (width == 4)
		emit(sljit_emit_fset32(compiler_, SLJIT_FR0, static_cast<float>(value)));
	else
		emit(sljit_emit_fset64(compiler_, SLJIT_FR0, value));
	storeFloatSlot(SLJIT_FR0, depth, width);
}

void OpcodeDispatcher::emitMemoryLoadFloat(const WASM::MemArg& arg, int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, static_cast<std::uint32_t>(width),
				stackWidths_.size() - 1);
		emit(sljit_emit_fop1(compiler_, width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
				SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R0), 0));
		storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	}
	stackWidths_.back() = width;
}

void OpcodeDispatcher::emitMemoryStoreFloat(const WASM::MemArg& arg, int width)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	pop();
	if (measureOnly_)
		return;
	emitCheckedMemoryAddress(arg, static_cast<std::uint32_t>(width), stackWidths_.size());
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() + 1, width);
	emit(sljit_emit_fop1(compiler_, width == 4 ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
			SLJIT_MEM1(SLJIT_R0), 0, SLJIT_FR0, 0));
}

void OpcodeDispatcher::emitIntToFloat(sljit_s32 conversion, int sourceWidth,
		int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, sourceWidth);
	emit(sljit_emit_fop1(compiler_, conversion, SLJIT_FR0, 0, SLJIT_R0, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, resultWidth);
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::emitFloatToInt(sljit_sw address, int sourceWidth,
		int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, sourceWidth);
	if (resultWidth == 4)
		emitHelper(sourceWidth == 4 ? SLJIT_ARGS1(32, F32) : SLJIT_ARGS1(32, F64),
				address);
	else
		emitHelper(sourceWidth == 4 ? SLJIT_ARGS1(W, F32) : SLJIT_ARGS1(W, F64),
				address);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, resultWidth);
	emitPendingTrapCheck();
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::emitReinterpret(int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, width);
	stackWidths_.back() = width;
}


void OpcodeDispatcher::dispatchF32Const(float arg)
{
	if (!reachable_)
		return;
	push(4);
	if (measureOnly_)
		return;
	emitFloatConst(arg, 4, stackWidths_.size() - 1);
}

void OpcodeDispatcher::dispatchF64Const(double arg)
{
	if (!reachable_)
		return;
	push(8);
	if (measureOnly_)
		return;
	emitFloatConst(arg, 8, stackWidths_.size() - 1);
}

void OpcodeDispatcher::dispatchF32Abs() { emitFloatUnary(SLJIT_ABS_F64, 4); }
void OpcodeDispatcher::dispatchF32Neg() { emitFloatUnary(SLJIT_NEG_F64, 4); }
void OpcodeDispatcher::dispatchF32Sqrt() { emitFloatHelper1(codeAddress(&sljitF32Sqrt), 4); }
void OpcodeDispatcher::dispatchF32Ceil() { emitFloatHelper1(codeAddress(&sljitF32Ceil), 4); }
void OpcodeDispatcher::dispatchF32Floor() { emitFloatHelper1(codeAddress(&sljitF32Floor), 4); }
void OpcodeDispatcher::dispatchF32Trunc() { emitFloatHelper1(codeAddress(&sljitF32Trunc), 4); }
void OpcodeDispatcher::dispatchF32Nearest() { emitFloatHelper1(codeAddress(&sljitF32Nearest), 4); }
void OpcodeDispatcher::dispatchF32Add() { emitFloatBinary(SLJIT_ADD_F64, 4); }
void OpcodeDispatcher::dispatchF32Sub() { emitFloatBinary(SLJIT_SUB_F64, 4); }
void OpcodeDispatcher::dispatchF32Mul() { emitFloatBinary(SLJIT_MUL_F64, 4); }
void OpcodeDispatcher::dispatchF32Div() { emitFloatBinary(SLJIT_DIV_F64, 4); }
void OpcodeDispatcher::dispatchF32Min() { emitFloatHelper2(codeAddress(&sljitF32Min), 4); }
void OpcodeDispatcher::dispatchF32Max() { emitFloatHelper2(codeAddress(&sljitF32Max), 4); }

void OpcodeDispatcher::dispatchF32Copysign()
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 4);
	loadFloatSlot(SLJIT_FR1, stackWidths_.size(), 4);
	emit(sljit_emit_fop2r(compiler_, SLJIT_COPYSIGN_F32, SLJIT_FR0,
			SLJIT_FR0, 0, SLJIT_FR1, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 4);
}

void OpcodeDispatcher::dispatchF64Abs() { emitFloatUnary(SLJIT_ABS_F64, 8); }
void OpcodeDispatcher::dispatchF64Neg() { emitFloatUnary(SLJIT_NEG_F64, 8); }
void OpcodeDispatcher::dispatchF64Sqrt() { emitFloatHelper1(codeAddress(&sljitF64Sqrt), 8); }
void OpcodeDispatcher::dispatchF64Ceil() { emitFloatHelper1(codeAddress(&sljitF64Ceil), 8); }
void OpcodeDispatcher::dispatchF64Floor() { emitFloatHelper1(codeAddress(&sljitF64Floor), 8); }
void OpcodeDispatcher::dispatchF64Trunc() { emitFloatHelper1(codeAddress(&sljitF64Trunc), 8); }
void OpcodeDispatcher::dispatchF64Nearest() { emitFloatHelper1(codeAddress(&sljitF64Nearest), 8); }
void OpcodeDispatcher::dispatchF64Add() { emitFloatBinary(SLJIT_ADD_F64, 8); }
void OpcodeDispatcher::dispatchF64Sub() { emitFloatBinary(SLJIT_SUB_F64, 8); }
void OpcodeDispatcher::dispatchF64Mul() { emitFloatBinary(SLJIT_MUL_F64, 8); }
void OpcodeDispatcher::dispatchF64Div() { emitFloatBinary(SLJIT_DIV_F64, 8); }
void OpcodeDispatcher::dispatchF64Min() { emitFloatHelper2(codeAddress(&sljitF64Min), 8); }
void OpcodeDispatcher::dispatchF64Max() { emitFloatHelper2(codeAddress(&sljitF64Max), 8); }

void OpcodeDispatcher::dispatchF64Copysign()
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 8);
	loadFloatSlot(SLJIT_FR1, stackWidths_.size(), 8);
	emit(sljit_emit_fop2r(compiler_, SLJIT_COPYSIGN_F64, SLJIT_FR0,
			SLJIT_FR0, 0, SLJIT_FR1, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchF32Eq() { emitFloatCompare(SLJIT_ORDERED_EQUAL, 4); }
void OpcodeDispatcher::dispatchF32Ne() { emitFloatCompare(SLJIT_UNORDERED_OR_NOT_EQUAL, 4); }
void OpcodeDispatcher::dispatchF32Lt() { emitFloatCompare(SLJIT_ORDERED_LESS, 4); }
void OpcodeDispatcher::dispatchF32Gt() { emitFloatCompare(SLJIT_ORDERED_GREATER, 4); }
void OpcodeDispatcher::dispatchF32Le() { emitFloatCompare(SLJIT_ORDERED_LESS_EQUAL, 4); }
void OpcodeDispatcher::dispatchF32Ge() { emitFloatCompare(SLJIT_ORDERED_GREATER_EQUAL, 4); }
void OpcodeDispatcher::dispatchF64Eq() { emitFloatCompare(SLJIT_ORDERED_EQUAL, 8); }
void OpcodeDispatcher::dispatchF64Ne() { emitFloatCompare(SLJIT_UNORDERED_OR_NOT_EQUAL, 8); }
void OpcodeDispatcher::dispatchF64Lt() { emitFloatCompare(SLJIT_ORDERED_LESS, 8); }
void OpcodeDispatcher::dispatchF64Gt() { emitFloatCompare(SLJIT_ORDERED_GREATER, 8); }
void OpcodeDispatcher::dispatchF64Le() { emitFloatCompare(SLJIT_ORDERED_LESS_EQUAL, 8); }
void OpcodeDispatcher::dispatchF64Ge() { emitFloatCompare(SLJIT_ORDERED_GREATER_EQUAL, 8); }

void OpcodeDispatcher::dispatchF32ConvertI32S()
{ emitIntToFloat(SLJIT_CONV_F32_FROM_S32, 4, 4); }
void OpcodeDispatcher::dispatchF32ConvertI32U()
{ emitIntToFloat(SLJIT_CONV_F32_FROM_U32, 4, 4); }
void OpcodeDispatcher::dispatchF32ConvertI64S()
{ emitIntToFloat(SLJIT_CONV_F32_FROM_SW, 8, 4); }
void OpcodeDispatcher::dispatchF32ConvertI64U()
{ emitIntToFloat(SLJIT_CONV_F32_FROM_UW, 8, 4); }
void OpcodeDispatcher::dispatchF64ConvertI32S()
{ emitIntToFloat(SLJIT_CONV_F64_FROM_S32, 4, 8); }
void OpcodeDispatcher::dispatchF64ConvertI32U()
{ emitIntToFloat(SLJIT_CONV_F64_FROM_U32, 4, 8); }
void OpcodeDispatcher::dispatchF64ConvertI64S()
{ emitIntToFloat(SLJIT_CONV_F64_FROM_SW, 8, 8); }
void OpcodeDispatcher::dispatchF64ConvertI64U()
{ emitIntToFloat(SLJIT_CONV_F64_FROM_UW, 8, 8); }

void OpcodeDispatcher::dispatchF32DemoteF64()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 8);
	emit(sljit_emit_fop1(compiler_, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0,
			SLJIT_FR0, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchF64PromoteF32()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 4);
	emit(sljit_emit_fop1(compiler_, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0,
			SLJIT_FR0, 0));
	storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, 8);
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchI32TruncF32S()
{ emitFloatToInt(codeAddress(&sljitI32TruncF32S), 4, 4); }
void OpcodeDispatcher::dispatchI32TruncF32U()
{ emitFloatToInt(codeAddress(&sljitI32TruncF32U), 4, 4); }
void OpcodeDispatcher::dispatchI32TruncF64S()
{ emitFloatToInt(codeAddress(&sljitI32TruncF64S), 8, 4); }
void OpcodeDispatcher::dispatchI32TruncF64U()
{ emitFloatToInt(codeAddress(&sljitI32TruncF64U), 8, 4); }
void OpcodeDispatcher::dispatchI64TruncF32S()
{ emitFloatToInt(codeAddress(&sljitI64TruncF32S), 4, 8); }
void OpcodeDispatcher::dispatchI64TruncF32U()
{ emitFloatToInt(codeAddress(&sljitI64TruncF32U), 4, 8); }
void OpcodeDispatcher::dispatchI64TruncF64S()
{ emitFloatToInt(codeAddress(&sljitI64TruncF64S), 8, 8); }
void OpcodeDispatcher::dispatchI64TruncF64U()
{ emitFloatToInt(codeAddress(&sljitI64TruncF64U), 8, 8); }

void OpcodeDispatcher::dispatchI32TruncSatF32S()
{ emitFloatToInt(codeAddress(&sljitI32TruncSatF32S), 4, 4); }
void OpcodeDispatcher::dispatchI32TruncSatF32U()
{ emitFloatToInt(codeAddress(&sljitI32TruncSatF32U), 4, 4); }
void OpcodeDispatcher::dispatchI32TruncSatF64S()
{ emitFloatToInt(codeAddress(&sljitI32TruncSatF64S), 8, 4); }
void OpcodeDispatcher::dispatchI32TruncSatF64U()
{ emitFloatToInt(codeAddress(&sljitI32TruncSatF64U), 8, 4); }
void OpcodeDispatcher::dispatchI64TruncSatF32S()
{ emitFloatToInt(codeAddress(&sljitI64TruncSatF32S), 4, 8); }
void OpcodeDispatcher::dispatchI64TruncSatF32U()
{ emitFloatToInt(codeAddress(&sljitI64TruncSatF32U), 4, 8); }
void OpcodeDispatcher::dispatchI64TruncSatF64S()
{ emitFloatToInt(codeAddress(&sljitI64TruncSatF64S), 8, 8); }
void OpcodeDispatcher::dispatchI64TruncSatF64U()
{ emitFloatToInt(codeAddress(&sljitI64TruncSatF64U), 8, 8); }

void OpcodeDispatcher::dispatchI32ReinterpretF32() { emitReinterpret(4); }
void OpcodeDispatcher::dispatchI64ReinterpretF64() { emitReinterpret(8); }
void OpcodeDispatcher::dispatchF32ReinterpretI32() { emitReinterpret(4); }
void OpcodeDispatcher::dispatchF64ReinterpretI64() { emitReinterpret(8); }

void OpcodeDispatcher::dispatchF32Load(WASM::MemArg arg) { emitMemoryLoadFloat(arg, 4); }
void OpcodeDispatcher::dispatchF64Load(WASM::MemArg arg) { emitMemoryLoadFloat(arg, 8); }
void OpcodeDispatcher::dispatchF32Store(WASM::MemArg arg) { emitMemoryStoreFloat(arg, 4); }
void OpcodeDispatcher::dispatchF64Store(WASM::MemArg arg) { emitMemoryStoreFloat(arg, 8); }

void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg)
{
	if (!reachable_)
		return;
	require(arg.size() == 16, "the v128 constant is not 16 bytes");
	push(16);
	if (measureOnly_)
		return;
	std::uint64_t lo = 0;
	std::uint64_t hi = 0;
	std::memcpy(&lo, arg.data(), sizeof(lo));
	std::memcpy(&hi, arg.data() + sizeof(lo), sizeof(hi));
	const sljit_sw offset = stackSlotOffset(stackWidths_.size() - 1);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), offset,
			SLJIT_IMM, static_cast<sljit_sw>(lo)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), offset + 8,
			SLJIT_IMM, static_cast<sljit_sw>(hi)));
}

void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg)
{
	if (!reachable_)
		return;
	requireStack(1);
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, 16, depth);
		const sljit_sw offset = stackSlotOffset(depth);
		for (int word = 0; word < 2; ++word) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
					SLJIT_MEM1(SLJIT_R0), word * 8));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					offset + word * 8, SLJIT_R1, 0));
		}
	}
	pop();
	push(16);
}

void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t addressDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, 16, addressDepth);
		const sljit_sw offset = stackSlotOffset(valueDepth);
		for (int word = 0; word < 2; ++word) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
					SLJIT_MEM1(SLJIT_SP), offset + word * 8));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0),
					word * 8, SLJIT_R1, 0));
		}
	}
	pop();
	pop();
}

void OpcodeDispatcher::dispatchI32x4Splat()
{
	if (!reachable_)
		return;
	requireStack(1);
	const std::size_t depth = stackWidths_.size() - 1;
	pop();
	push(16);
	if (measureOnly_)
		return;
	slotAddress(SLJIT_R1, depth);
	loadSlot(SLJIT_R0, depth, 4);
	emitHelper(SLJIT_ARGS2V(32, P), &wasm_i32x4_splat);
}

void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t lane)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		slotAddress(SLJIT_R0, stackWidths_.size() - 1);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, lane));
		emitHelper(SLJIT_ARGS2(32, P, 32), &wasm_i32x4_extract_lane);
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t lane)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t vectorDepth = stackWidths_.size() - 2;
	const std::size_t scalarDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		slotAddress(SLJIT_R0, vectorDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, lane));
		loadSlot(SLJIT_R2, scalarDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_R0, 0));
		emitHelper(SLJIT_ARGS4V(P, 32, 32, P), &wasm_i32x4_replace_lane);
	}
	pop();
}

void OpcodeDispatcher::dispatchV128Not() { emitV128Unary(codeAddress(&wasm_v128_not)); }

void OpcodeDispatcher::emitV128Unary(sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		// R0 is the helper's first argument, and sljit_get_local_base clobbers
		// it on x86, so the output address is materialised last.
		slotAddress(SLJIT_R1, stackWidths_.size() - 1);
		slotAddress(SLJIT_R0, stackWidths_.size() - 1);
		emitHelper(SLJIT_ARGS2V(P, P), address);
	}
}

void OpcodeDispatcher::emitV128Reduce(sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		slotAddress(SLJIT_R0, stackWidths_.size() - 1);
		emitHelper(SLJIT_ARGS1(32, P), address);
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::emitV128SplatFloat(sljit_sw address, int width)
{
	if (!reachable_)
		return;
	requireStack(1);
	const std::size_t depth = stackWidths_.size() - 1;
	pop();
	push(16);
	if (measureOnly_)
		return;
	// R0 is the output pointer; the scalar travels in FR0.
	slotAddress(SLJIT_R0, depth);
	loadFloatSlot(SLJIT_FR0, depth, width);
	emitHelper(width == 4 ? SLJIT_ARGS2V(F32, P) : SLJIT_ARGS2V(F64, P), address);
}

void OpcodeDispatcher::emitV128ExtractFloat(sljit_sw address, int width, uint8_t lane)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		slotAddress(SLJIT_R0, stackWidths_.size() - 1);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, lane));
		emitHelper(width == 4 ? SLJIT_ARGS2(F32, P, 32) : SLJIT_ARGS2(F64, P, 32), address);
		storeFloatSlot(SLJIT_FR0, stackWidths_.size() - 1, width);
	}
	stackWidths_.back() = width;
}

void OpcodeDispatcher::emitV128ReplaceFloat(sljit_sw address, int width, uint8_t lane)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t vectorDepth = stackWidths_.size() - 2;
	const std::size_t scalarDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		// Integer arguments are numbered per class: a -> R0, lane -> R1,
		// out -> R2; the new lane value travels in FR0.
		slotAddress(SLJIT_R0, vectorDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, lane));
		loadFloatSlot(SLJIT_FR0, scalarDepth, width);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_R0, 0));
		emitHelper(width == 4 ? SLJIT_ARGS4V(P, 32, F32, P) : SLJIT_ARGS4V(P, 32, F64, P), address);
	}
	pop();
}

void OpcodeDispatcher::emitV128LoadTransform(const WASM::MemArg& arg, std::uint32_t width,
		sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(1);
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, depth);
		// The result cell is the address cell; the pointer is parked in the
		// reserved scratch slot while the cell address is materialised, because
		// sljit_get_local_base clobbers R0 on x86.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset(), SLJIT_R0, 0));
		slotAddress(SLJIT_R1, depth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset()));
		emitHelper(SLJIT_ARGS2V(P, P), address);
	}
	pop();
	push(16);
}

void OpcodeDispatcher::emitV128LoadLane(const WASM::MemArg& arg, std::uint32_t width,
		uint8_t lane, sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t addressDepth = stackWidths_.size() - 2;
	const std::size_t vectorDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, addressDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset(), SLJIT_R0, 0));
		// The pair collapses to one entry in the address cell, so that cell is
		// the helper output; `a` stays in the vector cell.
		slotAddress(SLJIT_R3, addressDepth);
		slotAddress(SLJIT_R1, vectorDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, lane));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset()));
		emitHelper(SLJIT_ARGS4V(P, P, 32, P), address);
	}
	pop();
	pop();
	push(16);
}

void OpcodeDispatcher::emitV128StoreLane(const WASM::MemArg& arg, std::uint32_t width,
		uint8_t lane, sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t addressDepth = stackWidths_.size() - 2;
	const std::size_t vectorDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, addressDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset(), SLJIT_R0, 0));
		slotAddress(SLJIT_R1, vectorDepth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, lane));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset()));
		emitHelper(SLJIT_ARGS3V(P, P, 32), address);
	}
	pop();
	pop();
}

void OpcodeDispatcher::emitV128Shuffle(std::span<uint8_t> selectors, sljit_sw address)
{
	if (!reachable_)
		return;
	require(selectors.size() == 16, "the shuffle selector is not 16 bytes");
	requireStack(2);
	const std::size_t aDepth = stackWidths_.size() - 2;
	const std::size_t bDepth = stackWidths_.size() - 1;
	// A shuffle may read any byte of either operand, so the result cannot be
	// written into either cell: it goes to a scratch cell and is copied down.
	push(16);
	const std::size_t outDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		std::uint64_t lo = 0;
		std::uint64_t hi = 0;
		std::memcpy(&lo, selectors.data(), sizeof(lo));
		std::memcpy(&hi, selectors.data() + sizeof(lo), sizeof(hi));
		// The selector block lives in the reserved frame area; the adapter turns
		// the five-argument helper into the four arguments SLJIT can pass.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				selectorBlockOffset(), SLJIT_IMM, static_cast<sljit_sw>(lo)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				selectorBlockOffset() + 8, SLJIT_IMM, static_cast<sljit_sw>(hi)));
		slotAddress(SLJIT_R1, bDepth);
		emit(sljit_get_local_base(compiler_, SLJIT_R2, 0, selectorBlockOffset()));
		slotAddress(SLJIT_R3, outDepth);
		// R0 is materialised last: sljit_get_local_base clobbers it on x86.
		slotAddress(SLJIT_R0, aDepth);
		emitHelper(SLJIT_ARGS4V(P, P, P, P), address);
		for (int word = 0; word < 2; ++word) {
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(outDepth) + word * 8));
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
					stackSlotOffset(aDepth) + word * 8, SLJIT_R0, 0));
		}
	}
	pop();
	pop();
}

void OpcodeDispatcher::emitV128Shift(sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t vectorDepth = stackWidths_.size() - 2;
	const std::size_t countDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		// R0 is the helper's first argument, and sljit_get_local_base clobbers
		// it on x86, so it is materialised last.
		slotAddress(SLJIT_R2, vectorDepth);
		loadSlot(SLJIT_R1, countDepth, 4);
		slotAddress(SLJIT_R0, vectorDepth);
		emitHelper(SLJIT_ARGS3V(P, 32, P), address);
	}
	pop();
}

void OpcodeDispatcher::emitV128Binary(sljit_sw address)
{
	if (!reachable_)
		return;
	requireStack(2);
	if (!measureOnly_) {
		slotAddress(SLJIT_R1, stackWidths_.size() - 1);
		slotAddress(SLJIT_R2, stackWidths_.size() - 2);
		slotAddress(SLJIT_R0, stackWidths_.size() - 2);
		emitHelper(SLJIT_ARGS3V(P, P, P), address);
	}
	pop();
}

void OpcodeDispatcher::dispatchV128And() { emitV128Binary(codeAddress(&wasm_v128_and)); }
void OpcodeDispatcher::dispatchV128AndNot() { emitV128Binary(codeAddress(&wasm_v128_andnot)); }
void OpcodeDispatcher::dispatchV128Or() { emitV128Binary(codeAddress(&wasm_v128_or)); }
void OpcodeDispatcher::dispatchV128Xor() { emitV128Binary(codeAddress(&wasm_v128_xor)); }

void OpcodeDispatcher::dispatchV128Bitselect()
{
	if (!reachable_)
		return;
	requireStack(3);
	if (!measureOnly_) {
		slotAddress(SLJIT_R1, stackWidths_.size() - 2);
		slotAddress(SLJIT_R2, stackWidths_.size() - 1);
		slotAddress(SLJIT_R3, stackWidths_.size() - 3);
		slotAddress(SLJIT_R0, stackWidths_.size() - 3);
		emitHelper(SLJIT_ARGS4V(P, P, P, P), &wasm_v128_bitselect);
	}
	pop();
	pop();
}

void OpcodeDispatcher::dispatchV128AnyTrue()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		const sljit_sw offset = stackSlotOffset(stackWidths_.size() - 1);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
				SLJIT_MEM1(SLJIT_SP), offset));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
				SLJIT_MEM1(SLJIT_SP), offset + 8));
		emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
		sljit_jump* taken = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (taken == nullptr)
			ok_ = false;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (taken != nullptr)
			sljit_set_label(taken, done);
		storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchI8x16Shl() { emitV128Shift(codeAddress(&wasm_i8x16_shl)); }
void OpcodeDispatcher::dispatchI8x16ShrS() { emitV128Shift(codeAddress(&wasm_i8x16_shr_s)); }
void OpcodeDispatcher::dispatchI8x16ShrU() { emitV128Shift(codeAddress(&wasm_i8x16_shr_u)); }
void OpcodeDispatcher::dispatchI16x8Shl() { emitV128Shift(codeAddress(&wasm_i16x8_shl)); }
void OpcodeDispatcher::dispatchI16x8ShrS() { emitV128Shift(codeAddress(&wasm_i16x8_shr_s)); }
void OpcodeDispatcher::dispatchI16x8ShrU() { emitV128Shift(codeAddress(&wasm_i16x8_shr_u)); }
void OpcodeDispatcher::dispatchI32x4Shl() { emitV128Shift(codeAddress(&wasm_i32x4_shl)); }
void OpcodeDispatcher::dispatchI32x4ShrS() { emitV128Shift(codeAddress(&wasm_i32x4_shr_s)); }
void OpcodeDispatcher::dispatchI32x4ShrU() { emitV128Shift(codeAddress(&wasm_i32x4_shr_u)); }
void OpcodeDispatcher::dispatchI64x2Shl() { emitV128Shift(codeAddress(&wasm_i64x2_shl)); }
void OpcodeDispatcher::dispatchI64x2ShrS() { emitV128Shift(codeAddress(&wasm_i64x2_shr_s)); }
void OpcodeDispatcher::dispatchI64x2ShrU() { emitV128Shift(codeAddress(&wasm_i64x2_shr_u)); }

void OpcodeDispatcher::dispatchI8x16MinS() { emitV128Binary(codeAddress(&wasm_i8x16_min_s)); }
void OpcodeDispatcher::dispatchI8x16MinU() { emitV128Binary(codeAddress(&wasm_i8x16_min_u)); }
void OpcodeDispatcher::dispatchI8x16MaxS() { emitV128Binary(codeAddress(&wasm_i8x16_max_s)); }
void OpcodeDispatcher::dispatchI8x16MaxU() { emitV128Binary(codeAddress(&wasm_i8x16_max_u)); }
void OpcodeDispatcher::dispatchI16x8MinS() { emitV128Binary(codeAddress(&wasm_i16x8_min_s)); }
void OpcodeDispatcher::dispatchI16x8MinU() { emitV128Binary(codeAddress(&wasm_i16x8_min_u)); }
void OpcodeDispatcher::dispatchI16x8MaxS() { emitV128Binary(codeAddress(&wasm_i16x8_max_s)); }
void OpcodeDispatcher::dispatchI16x8MaxU() { emitV128Binary(codeAddress(&wasm_i16x8_max_u)); }
void OpcodeDispatcher::dispatchI32x4MinS() { emitV128Binary(codeAddress(&wasm_i32x4_min_s)); }
void OpcodeDispatcher::dispatchI32x4MinU() { emitV128Binary(codeAddress(&wasm_i32x4_min_u)); }
void OpcodeDispatcher::dispatchI32x4MaxS() { emitV128Binary(codeAddress(&wasm_i32x4_max_s)); }
void OpcodeDispatcher::dispatchI32x4MaxU() { emitV128Binary(codeAddress(&wasm_i32x4_max_u)); }
void OpcodeDispatcher::dispatchI8x16AddSatS() { emitV128Binary(codeAddress(&wasm_i8x16_add_sat_s)); }
void OpcodeDispatcher::dispatchI8x16AddSatU() { emitV128Binary(codeAddress(&wasm_i8x16_add_sat_u)); }
void OpcodeDispatcher::dispatchI8x16SubSatS() { emitV128Binary(codeAddress(&wasm_i8x16_sub_sat_s)); }
void OpcodeDispatcher::dispatchI8x16SubSatU() { emitV128Binary(codeAddress(&wasm_i8x16_sub_sat_u)); }
void OpcodeDispatcher::dispatchI16x8AddSatS() { emitV128Binary(codeAddress(&wasm_i16x8_add_sat_s)); }
void OpcodeDispatcher::dispatchI16x8AddSatU() { emitV128Binary(codeAddress(&wasm_i16x8_add_sat_u)); }
void OpcodeDispatcher::dispatchI16x8SubSatS() { emitV128Binary(codeAddress(&wasm_i16x8_sub_sat_s)); }
void OpcodeDispatcher::dispatchI16x8SubSatU() { emitV128Binary(codeAddress(&wasm_i16x8_sub_sat_u)); }
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS() { emitV128Binary(codeAddress(&wasm_i16x8_q15mulr_sat_s)); }
void OpcodeDispatcher::dispatchI8x16AvgrU() { emitV128Binary(codeAddress(&wasm_i8x16_avgr_u)); }
void OpcodeDispatcher::dispatchI16x8AvgrU() { emitV128Binary(codeAddress(&wasm_i16x8_avgr_u)); }

void OpcodeDispatcher::dispatchI8x16Abs() { emitV128Unary(codeAddress(&wasm_i8x16_abs)); }
void OpcodeDispatcher::dispatchI16x8Abs() { emitV128Unary(codeAddress(&wasm_i16x8_abs)); }
void OpcodeDispatcher::dispatchI32x4Abs() { emitV128Unary(codeAddress(&wasm_i32x4_abs)); }
void OpcodeDispatcher::dispatchI64x2Abs() { emitV128Unary(codeAddress(&wasm_i64x2_abs)); }
void OpcodeDispatcher::dispatchI8x16Popcnt() { emitV128Unary(codeAddress(&wasm_i8x16_popcnt)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S() { emitV128Unary(codeAddress(&wasm_i16x8_extend_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U() { emitV128Unary(codeAddress(&wasm_i16x8_extend_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S() { emitV128Unary(codeAddress(&wasm_i16x8_extend_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U() { emitV128Unary(codeAddress(&wasm_i16x8_extend_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S() { emitV128Unary(codeAddress(&wasm_i32x4_extend_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U() { emitV128Unary(codeAddress(&wasm_i32x4_extend_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S() { emitV128Unary(codeAddress(&wasm_i32x4_extend_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U() { emitV128Unary(codeAddress(&wasm_i32x4_extend_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S() { emitV128Unary(codeAddress(&wasm_i64x2_extend_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U() { emitV128Unary(codeAddress(&wasm_i64x2_extend_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S() { emitV128Unary(codeAddress(&wasm_i64x2_extend_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U() { emitV128Unary(codeAddress(&wasm_i64x2_extend_high_i32x4_u)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S() { emitV128Unary(codeAddress(&wasm_i16x8_extadd_pairwise_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U() { emitV128Unary(codeAddress(&wasm_i16x8_extadd_pairwise_i8x16_u)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S() { emitV128Unary(codeAddress(&wasm_i32x4_extadd_pairwise_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U() { emitV128Unary(codeAddress(&wasm_i32x4_extadd_pairwise_i16x8_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S() { emitV128Binary(codeAddress(&wasm_i16x8_extmul_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U() { emitV128Binary(codeAddress(&wasm_i16x8_extmul_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S() { emitV128Binary(codeAddress(&wasm_i16x8_extmul_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U() { emitV128Binary(codeAddress(&wasm_i16x8_extmul_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S() { emitV128Binary(codeAddress(&wasm_i32x4_extmul_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U() { emitV128Binary(codeAddress(&wasm_i32x4_extmul_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S() { emitV128Binary(codeAddress(&wasm_i32x4_extmul_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U() { emitV128Binary(codeAddress(&wasm_i32x4_extmul_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S() { emitV128Binary(codeAddress(&wasm_i64x2_extmul_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U() { emitV128Binary(codeAddress(&wasm_i64x2_extmul_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S() { emitV128Binary(codeAddress(&wasm_i64x2_extmul_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U() { emitV128Binary(codeAddress(&wasm_i64x2_extmul_high_i32x4_u)); }


void OpcodeDispatcher::dispatchI8x16NarrowI16x8S() { emitV128Binary(codeAddress(&wasm_i8x16_narrow_i16x8_s)); }
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U() { emitV128Binary(codeAddress(&wasm_i8x16_narrow_i16x8_u)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S() { emitV128Binary(codeAddress(&wasm_i16x8_narrow_i32x4_s)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U() { emitV128Binary(codeAddress(&wasm_i16x8_narrow_i32x4_u)); }
void OpcodeDispatcher::dispatchI32x4DotI16x8S() { emitV128Binary(codeAddress(&wasm_i32x4_dot_i16x8_s)); }

void OpcodeDispatcher::dispatchF32x4Add() { emitV128Binary(codeAddress(&wasm_f32x4_add)); }
void OpcodeDispatcher::dispatchF32x4Sub() { emitV128Binary(codeAddress(&wasm_f32x4_sub)); }
void OpcodeDispatcher::dispatchF32x4Mul() { emitV128Binary(codeAddress(&wasm_f32x4_mul)); }
void OpcodeDispatcher::dispatchF32x4Div() { emitV128Binary(codeAddress(&wasm_f32x4_div)); }
void OpcodeDispatcher::dispatchF32x4Min() { emitV128Binary(codeAddress(&wasm_f32x4_min)); }
void OpcodeDispatcher::dispatchF32x4Max() { emitV128Binary(codeAddress(&wasm_f32x4_max)); }
void OpcodeDispatcher::dispatchF32x4PMin() { emitV128Binary(codeAddress(&wasm_f32x4_pmin)); }
void OpcodeDispatcher::dispatchF32x4PMax() { emitV128Binary(codeAddress(&wasm_f32x4_pmax)); }
void OpcodeDispatcher::dispatchF32x4Eq() { emitV128Binary(codeAddress(&wasm_f32x4_eq)); }
void OpcodeDispatcher::dispatchF32x4Ne() { emitV128Binary(codeAddress(&wasm_f32x4_ne)); }
void OpcodeDispatcher::dispatchF32x4Lt() { emitV128Binary(codeAddress(&wasm_f32x4_lt)); }
void OpcodeDispatcher::dispatchF32x4Gt() { emitV128Binary(codeAddress(&wasm_f32x4_gt)); }
void OpcodeDispatcher::dispatchF32x4Le() { emitV128Binary(codeAddress(&wasm_f32x4_le)); }
void OpcodeDispatcher::dispatchF32x4Ge() { emitV128Binary(codeAddress(&wasm_f32x4_ge)); }
void OpcodeDispatcher::dispatchF64x2Add() { emitV128Binary(codeAddress(&wasm_f64x2_add)); }
void OpcodeDispatcher::dispatchF64x2Sub() { emitV128Binary(codeAddress(&wasm_f64x2_sub)); }
void OpcodeDispatcher::dispatchF64x2Mul() { emitV128Binary(codeAddress(&wasm_f64x2_mul)); }
void OpcodeDispatcher::dispatchF64x2Div() { emitV128Binary(codeAddress(&wasm_f64x2_div)); }
void OpcodeDispatcher::dispatchF64x2Min() { emitV128Binary(codeAddress(&wasm_f64x2_min)); }
void OpcodeDispatcher::dispatchF64x2Max() { emitV128Binary(codeAddress(&wasm_f64x2_max)); }
void OpcodeDispatcher::dispatchF64x2PMin() { emitV128Binary(codeAddress(&wasm_f64x2_pmin)); }
void OpcodeDispatcher::dispatchF64x2PMax() { emitV128Binary(codeAddress(&wasm_f64x2_pmax)); }
void OpcodeDispatcher::dispatchF64x2Eq() { emitV128Binary(codeAddress(&wasm_f64x2_eq)); }
void OpcodeDispatcher::dispatchF64x2Ne() { emitV128Binary(codeAddress(&wasm_f64x2_ne)); }
void OpcodeDispatcher::dispatchF64x2Lt() { emitV128Binary(codeAddress(&wasm_f64x2_lt)); }
void OpcodeDispatcher::dispatchF64x2Gt() { emitV128Binary(codeAddress(&wasm_f64x2_gt)); }
void OpcodeDispatcher::dispatchF64x2Le() { emitV128Binary(codeAddress(&wasm_f64x2_le)); }
void OpcodeDispatcher::dispatchF64x2Ge() { emitV128Binary(codeAddress(&wasm_f64x2_ge)); }

void OpcodeDispatcher::dispatchF32x4Ceil() { emitV128Unary(codeAddress(&wasm_f32x4_ceil)); }
void OpcodeDispatcher::dispatchF32x4Floor() { emitV128Unary(codeAddress(&wasm_f32x4_floor)); }
void OpcodeDispatcher::dispatchF32x4Trunc() { emitV128Unary(codeAddress(&wasm_f32x4_trunc)); }
void OpcodeDispatcher::dispatchF32x4Nearest() { emitV128Unary(codeAddress(&wasm_f32x4_nearest)); }
void OpcodeDispatcher::dispatchF32x4Abs() { emitV128Unary(codeAddress(&wasm_f32x4_abs)); }
void OpcodeDispatcher::dispatchF32x4Neg() { emitV128Unary(codeAddress(&wasm_f32x4_neg)); }
void OpcodeDispatcher::dispatchF32x4Sqrt() { emitV128Unary(codeAddress(&wasm_f32x4_sqrt)); }
void OpcodeDispatcher::dispatchF64x2Ceil() { emitV128Unary(codeAddress(&wasm_f64x2_ceil)); }
void OpcodeDispatcher::dispatchF64x2Floor() { emitV128Unary(codeAddress(&wasm_f64x2_floor)); }
void OpcodeDispatcher::dispatchF64x2Trunc() { emitV128Unary(codeAddress(&wasm_f64x2_trunc)); }
void OpcodeDispatcher::dispatchF64x2Nearest() { emitV128Unary(codeAddress(&wasm_f64x2_nearest)); }
void OpcodeDispatcher::dispatchF64x2Abs() { emitV128Unary(codeAddress(&wasm_f64x2_abs)); }
void OpcodeDispatcher::dispatchF64x2Neg() { emitV128Unary(codeAddress(&wasm_f64x2_neg)); }
void OpcodeDispatcher::dispatchF64x2Sqrt() { emitV128Unary(codeAddress(&wasm_f64x2_sqrt)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S() { emitV128Unary(codeAddress(&wasm_i32x4_trunc_sat_f32x4_s)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U() { emitV128Unary(codeAddress(&wasm_i32x4_trunc_sat_f32x4_u)); }
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S() { emitV128Unary(codeAddress(&wasm_f32x4_convert_i32x4_s)); }
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U() { emitV128Unary(codeAddress(&wasm_f32x4_convert_i32x4_u)); }
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S() { emitV128Unary(codeAddress(&wasm_f64x2_convert_low_i32x4_s)); }
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U() { emitV128Unary(codeAddress(&wasm_f64x2_convert_low_i32x4_u)); }
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4() { emitV128Unary(codeAddress(&wasm_f64x2_promote_low_f32x4)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero() { emitV128Unary(codeAddress(&wasm_i32x4_trunc_sat_f64x2_s_zero)); }
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero() { emitV128Unary(codeAddress(&wasm_i32x4_trunc_sat_f64x2_u_zero)); }
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero() { emitV128Unary(codeAddress(&wasm_f32x4_demote_f64x2_zero)); }

void OpcodeDispatcher::dispatchF32x4Splat() { emitV128SplatFloat(codeAddress(&wasm_f32x4_splat), 4); }
void OpcodeDispatcher::dispatchF64x2Splat() { emitV128SplatFloat(codeAddress(&wasm_f64x2_splat), 8); }
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t lane) { emitV128ExtractFloat(codeAddress(&wasm_f32x4_extract_lane), 4, lane); }
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t lane) { emitV128ExtractFloat(codeAddress(&wasm_f64x2_extract_lane), 8, lane); }
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t lane) { emitV128ReplaceFloat(codeAddress(&wasm_f32x4_replace_lane), 4, lane); }
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t lane) { emitV128ReplaceFloat(codeAddress(&wasm_f64x2_replace_lane), 8, lane); }

void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load8x8_s)); }
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load8x8_u)); }
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load16x4_s)); }
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load16x4_u)); }
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load32x2_s)); }
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load32x2_u)); }
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg) { emitV128LoadTransform(arg, 1, codeAddress(&wasm_v128_load8_splat)); }
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg) { emitV128LoadTransform(arg, 2, codeAddress(&wasm_v128_load16_splat)); }
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg) { emitV128LoadTransform(arg, 4, codeAddress(&wasm_v128_load32_splat)); }
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load64_splat)); }
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg) { emitV128LoadTransform(arg, 4, codeAddress(&wasm_v128_load32_zero)); }
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg) { emitV128LoadTransform(arg, 8, codeAddress(&wasm_v128_load64_zero)); }

void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128LoadLane(arg1, 1, arg2, codeAddress(&wasm_v128_load8_lane)); }
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128LoadLane(arg1, 2, arg2, codeAddress(&wasm_v128_load16_lane)); }
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128LoadLane(arg1, 4, arg2, codeAddress(&wasm_v128_load32_lane)); }
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128LoadLane(arg1, 8, arg2, codeAddress(&wasm_v128_load64_lane)); }
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128StoreLane(arg1, 1, arg2, codeAddress(&wasm_v128_store8_lane)); }
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128StoreLane(arg1, 2, arg2, codeAddress(&wasm_v128_store16_lane)); }
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128StoreLane(arg1, 4, arg2, codeAddress(&wasm_v128_store32_lane)); }
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) { emitV128StoreLane(arg1, 8, arg2, codeAddress(&wasm_v128_store64_lane)); }

void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> selectors) { emitV128Shuffle(selectors, codeAddress(&sljitShuffleAdapter)); }
void OpcodeDispatcher::dispatchI8x16Swizzle() { emitV128Binary(codeAddress(&wasm_i8x16_swizzle)); }

void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType&)
{
	if (!reachable_)
		return;
	// A reference is one null word: the backend does not model typed nulls.
	push(8);
	if (measureOnly_)
		return;
	storeSlot(SLJIT_IMM, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchRefIsNull()
{
	if (!reachable_)
		return;
	requireStack(1);
	require(stackWidths_.back() == 8, "ref.is_null requires a reference operand");
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0));
		sljit_jump* taken = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
		if (taken == nullptr)
			ok_ = false;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 1));
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (taken != nullptr)
			sljit_set_label(taken, done);
		storeSlot(SLJIT_R1, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx depth)
{
	require(reachable_, "a br_on_null appears in unreachable code");
	require(depth <= controls_.size(), "the br_on_null label depth is out of range");
	requireStack(1);
	require(stackWidths_.back() == 8, "br_on_null requires a reference operand");
	// The reference stays on the stack for the non-null path.
	if (depth == controls_.size()) {
		require(resultWidth_ == 0, "br_on_null to the function label requires a void result");
		if (measureOnly_)
			return;
		loadSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitReturnValue();
		sljit_label* skip = sljit_emit_label(compiler_);
		if (skip == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, skip);
		return;
	}
	ControlFrame& target = controls_[controls_.size() - 1 - depth];
	const std::vector<int>& widths =
			target.kind == ControlKind::Loop ? target.paramWidths : target.resultWidths;
	require(widths.empty(), "br_on_null requires a label without results or parameters");
	if (measureOnly_) {
		if (target.kind != ControlKind::Loop)
			target.endTargeted = true;
		return;
	}
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
	sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (notNull == nullptr)
		ok_ = false;
	sljit_jump* jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (jump == nullptr) {
		ok_ = false;
	} else if (target.kind == ControlKind::Loop) {
		if (target.begin == nullptr)
			ok_ = false;
		else
			sljit_set_label(jump, target.begin);
	} else {
		target.endJumps.push_back(jump);
		target.endTargeted = true;
	}
	sljit_label* skip = sljit_emit_label(compiler_);
	if (skip == nullptr)
		ok_ = false;
	else if (notNull != nullptr)
		sljit_set_label(notNull, skip);
}

void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Struct, "struct.new_default requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcStructSize(fields))));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitStructNew);
		emitPendingTrapCheck(); // ALLOC_TRAPCHECK
		// Default fields are zero; the heap does not promise pre-zeroed memory.
		for (std::size_t i = 0; i < fields.size(); ++i) {
			require(!fields[i].storage.isPacked, "the SLJIT backend does not lower packed struct fields yet");
			const WASM::ValueTypeCode opcode = fields[i].storage.value.opcode;
			const int width = (opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8;
			require(opcode != WASM::ValueTypeCode::V128,
					"the SLJIT backend does not lower v128 struct fields yet");
			emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
					SLJIT_MEM1(SLJIT_R0),
					static_cast<sljit_sw>(WASM::gcStructFieldOffset(fields, i)),
					SLJIT_IMM, 0));
		}
	}
	push(8);
	if (!measureOnly_)
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Struct, "struct.new requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	requireStack(fields.size());
	for (const WASM::CanonFieldType& field : fields)
		require(field.storage.isPacked || field.storage.value.opcode != WASM::ValueTypeCode::V128,
				"the SLJIT backend does not lower v128 struct fields yet");
	const std::size_t size = WASM::gcStructSize(fields);
	if (!measureOnly_) {
		// VMContext*, size, type index -> new object pointer in R0.
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(size)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitStructNew);
		emitPendingTrapCheck(); // ALLOC_TRAPCHECK
		// Fields are stacked in declaration order, so store them in reverse.
		for (std::size_t i = fields.size(); i-- > 0;) {
			const std::size_t offset = WASM::gcStructFieldOffset(fields, i);
			const std::size_t depth = stackWidths_.size() - 1;
			// A packed field stores only its low byte or word.
			sljit_s32 storeOp = SLJIT_MOV;
			if (fields[i].storage.isPacked)
				storeOp = fields[i].storage.packed == WASM::ValueTypeCode::I8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16;
			else if (stackWidths_.back() == 4)
				storeOp = SLJIT_MOV32;
			loadSlot(SLJIT_R1, depth, 4);
			emit(sljit_emit_op1(compiler_, storeOp,
					SLJIT_MEM1(SLJIT_R0), static_cast<sljit_sw>(offset), SLJIT_R1, 0));
			pop();
		}
	} else {
		for (std::size_t i = 0; i < fields.size(); ++i)
			pop();
	}
	push(8);
	if (!measureOnly_)
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg)
{
	emitArrayGetPacked(arg, true);
}

void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg)
{
	emitArrayGetPacked(arg, false);
}

void OpcodeDispatcher::emitArrayGetPacked(WASM::TypeIdx arg, bool signExtend)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.get_s/u requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked, "array.get_s/u requires a packed element type");
	const bool isI8 = element.storage.packed == WASM::ValueTypeCode::I8;
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, refDepth, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullArrayReference);
		sljit_label* afterNull = sljit_emit_label(compiler_);
		if (afterNull == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, afterNull);
		loadSlot(SLJIT_R2, indexDepth, 4);
		loadSlot(SLJIT_R1, refDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
		sljit_jump* inBounds = sljit_emit_cmp(compiler_, SLJIT_LESS, SLJIT_R2, 0, SLJIT_R3, 0);
		if (inBounds == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
		sljit_label* afterBounds = sljit_emit_label(compiler_);
		if (afterBounds == nullptr)
			ok_ = false;
		else if (inBounds != nullptr)
			sljit_set_label(inBounds, afterBounds);
		// Reload after the trap blocks; fold the offset into the register.
		loadSlot(SLJIT_R1, refDepth, 8);
		loadSlot(SLJIT_R2, indexDepth, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage))));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage))));
		const sljit_s32 loadOp = isI8 ? (signExtend ? SLJIT_MOV_S8 : SLJIT_MOV_U8)
				: (signExtend ? SLJIT_MOV_S16 : SLJIT_MOV_U16);
		emit(sljit_emit_op1(compiler_, loadOp, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1), 0));
		storeSlot(SLJIT_R3, refDepth, 4);
	}
	pop();
	pop();
	push(4);
}

void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.set requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, refDepth, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullArrayReference);
		sljit_label* afterNull = sljit_emit_label(compiler_);
		if (afterNull == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, afterNull);
		loadSlot(SLJIT_R1, refDepth, 8);
		loadSlot(SLJIT_R2, indexDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
		sljit_jump* inBounds = sljit_emit_cmp(compiler_, SLJIT_LESS, SLJIT_R2, 0, SLJIT_R3, 0);
		if (inBounds == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
		sljit_label* afterBounds = sljit_emit_label(compiler_);
		if (afterBounds == nullptr)
			ok_ = false;
		else if (inBounds != nullptr)
			sljit_set_label(inBounds, afterBounds);
		// The trap blocks clobber R0-R3, so everything is reloaded here. The
		// address is folded into one register exactly as the read path does it,
		// instead of using a memory operand with a displacement.
		loadSlot(SLJIT_R1, refDepth, 8);
		loadSlot(SLJIT_R2, indexDepth, 4);
		loadSlot(SLJIT_R3, valueDepth, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage))));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage))));
		const sljit_s32 storeOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
				: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
		emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R3, 0));
	}
	pop();
	pop();
	pop();
}

void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.get requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(!element.storage.isPacked, "the SLJIT backend does not lower packed array elements yet");
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	require(opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const int width = (opcode == WASM::ValueTypeCode::I32
			|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8;
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, refDepth, 8);
		// Null reference: trap before touching the header.
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullArrayReference);
		sljit_label* afterNull = sljit_emit_label(compiler_);
		if (afterNull == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, afterNull);
		// Bounds: index must be below the length word (both unsigned).
		loadSlot(SLJIT_R2, indexDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
		sljit_jump* inBounds = sljit_emit_cmp(compiler_, SLJIT_LESS, SLJIT_R2, 0, SLJIT_R3, 0);
		if (inBounds == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
		sljit_label* afterBounds = sljit_emit_label(compiler_);
		if (afterBounds == nullptr)
			ok_ = false;
		else if (inBounds != nullptr)
			sljit_set_label(inBounds, afterBounds);
		// The trap blocks above call helpers, which clobber R0-R3: reload the
		// object and the index before computing the element address.
		loadSlot(SLJIT_R1, refDepth, 8);
		loadSlot(SLJIT_R2, indexDepth, 4);

		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage))));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0));
		emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV, SLJIT_R2, 0,
				SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage))));
		storeSlot(SLJIT_R2, refDepth, width);
	}
	pop();
	pop();
	push(width);
}

void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView dstView = module_.typeView(arg1);
	require(dstView.kind() == WASM::CanonTypeKind::Array,
			"array.copy requires an array type");
	const WASM::CanonFieldType& element = dstView.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(5);
	const std::size_t dstDepth = stackWidths_.size() - 5;
	const std::size_t dstIndexDepth = stackWidths_.size() - 4;
	const std::size_t srcDepth = stackWidths_.size() - 3;
	const std::size_t srcIndexDepth = stackWidths_.size() - 2;
	const std::size_t lenDepth = stackWidths_.size() - 1;
	if (measureOnly_) {
		for (int i = 0; i < 5; ++i)
			pop();
		return;
	}
	const sljit_sw dataOffset = static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage));
	const sljit_sw elementBytes = static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage));
	const sljit_s32 loadOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
			: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
	const sljit_s32 storeOp = loadOp;
	// Four trap blocks, each followed by a reload: both references are non-null and
	// both ranges are inside their array.
	const auto emitNullCheck = [&](std::size_t depth) {
		loadSlot(SLJIT_R0, depth, 8);
		struct sljit_jump* notNull =
				sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullArrayReference);
		struct sljit_label* after = sljit_emit_label(compiler_);
		if (after == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, after);
	};
	const auto emitRangeCheck = [&](std::size_t arrayDepth, std::size_t indexDepth) {
		loadSlot(SLJIT_R1, arrayDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
		loadSlot(SLJIT_R0, indexDepth, 4);
		loadSlot(SLJIT_R3, lenDepth, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R3, 0));
		struct sljit_jump* inBounds =
				sljit_emit_cmp(compiler_, SLJIT_LESS_EQUAL, SLJIT_R0, 0, SLJIT_R2, 0);
		if (inBounds == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
		struct sljit_label* after = sljit_emit_label(compiler_);
		if (after == nullptr)
			ok_ = false;
		else if (inBounds != nullptr)
			sljit_set_label(inBounds, after);
	};
	emitNullCheck(dstDepth);
	emitNullCheck(srcDepth);
	emitRangeCheck(dstDepth, dstIndexDepth);
	emitRangeCheck(srcDepth, srcIndexDepth);
	// R0 = destination cursor, R1 = source cursor, R2 = element counter.
	loadSlot(SLJIT_R0, dstDepth, 8);
	loadSlot(SLJIT_R3, dstIndexDepth, 4);
	if (elementBytes != 1)
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, dataOffset));
	loadSlot(SLJIT_R1, srcDepth, 8);
	loadSlot(SLJIT_R3, srcIndexDepth, 4);
	if (elementBytes != 1)
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, dataOffset));
	loadSlot(SLJIT_R2, lenDepth, 4);
	// memmove rule: copy forward when the destination starts at or below the
	// source, otherwise walk both pointers backwards from the end.
	// A label in SLJIT is a *position*, not a handle: `sljit_emit_label` binds it
	// where it is called, so the join label must be created after both loops.
	struct sljit_jump* toForward =
			sljit_emit_cmp(compiler_, SLJIT_LESS_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
	if (toForward == nullptr)
		ok_ = false;
	// --- backward loop (fallthrough) ---
	emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R3, 0, SLJIT_R2, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R3, 0));
	struct sljit_label* backStart = sljit_emit_label(compiler_);
	struct sljit_jump* backEnd =
			sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
	if (backEnd == nullptr)
		ok_ = false;
	emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op1(compiler_, loadOp, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1), 0));
	emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1));
	struct sljit_jump* backEdge = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (backEdge == nullptr)
		ok_ = false;
	else if (backStart != nullptr)
		sljit_set_label(backEdge, backStart);
	struct sljit_jump* skipForward = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (skipForward == nullptr)
		ok_ = false;
	// --- forward loop ---
	struct sljit_label* forwardStart = sljit_emit_label(compiler_);
	if (toForward != nullptr && forwardStart != nullptr)
		sljit_set_label(toForward, forwardStart);
	struct sljit_jump* forwardEnd =
			sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
	if (forwardEnd == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, loadOp, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R1), 0));
	emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1));
	struct sljit_jump* forwardBack = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (forwardBack == nullptr)
		ok_ = false;
	else if (forwardStart != nullptr)
		sljit_set_label(forwardBack, forwardStart);
	struct sljit_label* doneLabel = sljit_emit_label(compiler_);
	if (doneLabel != nullptr) {
		if (backEnd != nullptr)
			sljit_set_label(backEnd, doneLabel);
		if (forwardEnd != nullptr)
			sljit_set_label(forwardEnd, doneLabel);
		if (skipForward != nullptr)
			sljit_set_label(skipForward, doneLabel);
	}
	for (int i = 0; i < 5; ++i)
		pop();
}

void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.fill requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(4);
	// Operand order (spec): [array, offset, value, count]. The offset is the
	// third slot from the top and the value the second; the reversed naming made
	// the fill use the value as the offset (D-2).
	const std::size_t arrayDepth = stackWidths_.size() - 4;
	const std::size_t indexDepth = stackWidths_.size() - 3;
	const std::size_t valueDepth = stackWidths_.size() - 2;
	const std::size_t lenDepth = stackWidths_.size() - 1;
	if (measureOnly_) {
		for (int i = 0; i < 4; ++i)
			pop();
		return;
	}
	const sljit_sw dataOffset = static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage));
	const sljit_sw elementBytes = static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage));
	const sljit_s32 storeOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
			: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
	// Null check first, so a null array reports the array kind and not a
	// bounds failure (§8.3 of docs/ABI_PORTABILITY.md).
	loadSlot(SLJIT_R0, arrayDepth, 8);
	struct sljit_jump* notNull =
			sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
	if (notNull == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::NullArrayReference);
	struct sljit_label* afterNull = sljit_emit_label(compiler_);
	if (afterNull == nullptr)
		ok_ = false;
	else if (notNull != nullptr)
		sljit_set_label(notNull, afterNull);
	// Bounds: (u64)index + (u64)len <= (u64)arrayLength. Both operands are
	// zero-extended 32-bit values, so the 64-bit sum cannot wrap.
	loadSlot(SLJIT_R1, arrayDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
			static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
	loadSlot(SLJIT_R0, indexDepth, 4);
	loadSlot(SLJIT_R3, lenDepth, 4);
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R3, 0));
	struct sljit_jump* inBounds =
			sljit_emit_cmp(compiler_, SLJIT_LESS_EQUAL, SLJIT_R0, 0, SLJIT_R2, 0);
	if (inBounds == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	struct sljit_label* afterBounds = sljit_emit_label(compiler_);
	if (afterBounds == nullptr)
		ok_ = false;
	else if (inBounds != nullptr)
		sljit_set_label(inBounds, afterBounds);
	// Reload after the trap blocks (they clobber R0-R3), then the counted
	// store loop: cursor advances, counter counts down.
	loadSlot(SLJIT_R0, arrayDepth, 8);
	loadSlot(SLJIT_R1, indexDepth, 4);
	if (elementBytes != 1)
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM,
				elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, dataOffset));
	loadSlot(SLJIT_R1, lenDepth, 4);
	loadSlot(SLJIT_R3, valueDepth, width == 8 ? 8 : 4);
	struct sljit_label* loopStart = sljit_emit_label(compiler_);
	struct sljit_jump* loopEnd =
			sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
	if (loopEnd == nullptr)
		ok_ = false;
	emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R3, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, elementBytes));
	emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1));
	struct sljit_jump* backEdge = sljit_emit_jump(compiler_, SLJIT_JUMP);
	if (backEdge == nullptr)
		ok_ = false;
	else if (loopStart != nullptr)
		sljit_set_label(backEdge, loopStart);
	struct sljit_label* loopDone = sljit_emit_label(compiler_);
	if (loopDone == nullptr)
		ok_ = false;
	else if (loopEnd != nullptr)
		sljit_set_label(loopEnd, loopDone);
	for (int i = 0; i < 4; ++i)
		pop();
}

void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2)
{
if (!reachable_)
return;
const WASM::CoreTypeView view = module_.typeView(arg1);
require(view.kind() == WASM::CanonTypeKind::Array, "array.new_data requires an array type");
const WASM::CanonFieldType& element = view.arrayElement();
require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
"the SLJIT backend does not lower v128 array elements yet");
requireStack(2);
const std::size_t offsetDepth = stackWidths_.size() - 2;
const std::size_t lenDepth = stackWidths_.size() - 1;
if (measureOnly_) {
pop();
pop();
push(8);
return;
}
// (vm, type index, length)
loadSlot(SLJIT_R2, lenDepth, 4);
emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
static_cast<sljit_sw>(arg1)));
loadContext(SLJIT_R0);
emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitArrayNew);
emitPendingTrapCheck(); // ALLOC_TRAPCHECK
// The object has to survive the copy call, so it is parked in the length
// slot; that slot's value is dead once the allocation has used it.
	storeSlot(SLJIT_R0, lenDepth, 8);
	// Pack `dataIndex << 32 | byteOffset` because the helper ABI carries four
	// arguments at most: R1 is scratch until the packed value moves to R3.
	loadSlot(SLJIT_R1, offsetDepth, 4);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
static_cast<sljit_sw>(arg2)));
	emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 32));
	emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_R1, 0));
	// (vm, array, type index, packed offset and index)
	loadSlot(SLJIT_R1, lenDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
static_cast<sljit_sw>(arg1)));
	loadContext(SLJIT_R0);
	emitHelper(SLJIT_ARGS4V(P, P, 32, W), &sljitArrayInitFromData);
	loadSlot(SLJIT_R0, lenDepth, 8);
	pop();
	pop();
	push(8);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.new_default requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(1);
	if (!measureOnly_) {
		loadSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitArrayNew);
		emitPendingTrapCheck(); // ALLOC_TRAPCHECK
		// Zero-fill loop. The count is a runtime value, so this is the first
		// backend-emitted counted loop: R1 counts down to zero while R3 walks the
		// data pointer, so every store is a constant-offset MEM1 (never MEM2).
		const sljit_sw dataOffset = static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage));
		const sljit_sw elementBytes = static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage));
		const sljit_s32 storeOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
				: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, dataOffset));
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 4);
		struct sljit_label* loopStart = sljit_emit_label(compiler_);
		struct sljit_jump* loopEnd =
				sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R3), 0, SLJIT_IMM, 0));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, elementBytes));
		emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1));
		struct sljit_jump* backEdge = sljit_emit_jump(compiler_, SLJIT_JUMP);
		sljit_set_label(backEdge, loopStart);
		sljit_set_label(loopEnd, sljit_emit_label(compiler_));
	}
	pop();
	push(8);
	if (!measureOnly_)
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg1);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.new_fixed requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(arg2);
	if (!measureOnly_) {
		// The element count is an immediate, so every store offset is constant.
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg1)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg2)));
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitArrayNew);
		emitPendingTrapCheck(); // ALLOC_TRAPCHECK
		const sljit_sw dataOffset =
				static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage));
		const sljit_sw elementBytes =
				static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage));
		const sljit_s32 storeOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
				: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
		for (std::uint32_t i = arg2; i-- > 0;) {
			loadSlot(SLJIT_R1, stackWidths_.size() - 1, 4);
			emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0),
					dataOffset + static_cast<sljit_sw>(i) * elementBytes, SLJIT_R1, 0));
			pop();
		}
	} else {
		for (std::uint32_t i = 0; i < arg2; ++i)
			pop();
	}
	push(8);
	if (!measureOnly_)
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "array.new requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	require(element.storage.isPacked || element.storage.value.opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 array elements yet");
	const WASM::ValueTypeCode opcode = element.storage.value.opcode;
	const bool packed = element.storage.isPacked;
	const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
	const int width = packed ? (packedI8 ? 1 : 2)
			: ((opcode == WASM::ValueTypeCode::I32
					|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8);
	requireStack(2);
	const std::size_t elementDepth = stackWidths_.size() - 2;
	const std::size_t lengthDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		// (vm, type index, length) — three arguments, inside SLJIT's limit.
		loadSlot(SLJIT_R2, lengthDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadContext(SLJIT_R0);
		emitHelper(SLJIT_ARGS3(P, P, 32, 32), &sljitArrayNew);
		emitPendingTrapCheck(); // ALLOC_TRAPCHECK
		// The helper clobbers R1-R3, so the initialiser and the length are
		// reloaded afterwards. **Every** element must be initialised, not just
		// element 0: this is the counted store loop, the same shape array.fill
		// and array.new_default use.
		const sljit_sw dataOffset = static_cast<sljit_sw>(WASM::gcArrayDataOffset(element.storage));
		const sljit_sw elementBytes = static_cast<sljit_sw>(WASM::gcStorageBytes(element.storage));
		const sljit_s32 storeOp = packed ? (packedI8 ? SLJIT_MOV_U8 : SLJIT_MOV_U16)
				: (width == 4 ? SLJIT_MOV32 : SLJIT_MOV);
		loadSlot(SLJIT_R1, elementDepth, width == 8 ? 8 : 4);
		loadSlot(SLJIT_R2, lengthDepth, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, dataOffset));
		struct sljit_label* fillStart = sljit_emit_label(compiler_);
		struct sljit_jump* fillEnd =
				sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
		if (fillEnd == nullptr)
			ok_ = false;
		emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R3), 0, SLJIT_R1, 0));
		emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, elementBytes));
		emit(sljit_emit_op2(compiler_, SLJIT_SUB, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1));
		struct sljit_jump* fillBack = sljit_emit_jump(compiler_, SLJIT_JUMP);
		if (fillBack == nullptr)
			ok_ = false;
		else if (fillStart != nullptr)
			sljit_set_label(fillBack, fillStart);
		struct sljit_label* fillDone = sljit_emit_label(compiler_);
		if (fillDone == nullptr)
			ok_ = false;
		else if (fillEnd != nullptr)
			sljit_set_label(fillEnd, fillDone);
	}
	pop();
	pop();
	push(8);
	if (!measureOnly_)
		storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::dispatchArrayLen()
{
	if (!reachable_)
		return;
	requireStack(1);
	require(stackWidths_.back() == 8, "array.len requires a reference operand");
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullArrayReference);
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, done);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::GcHeader::kArrayLengthOffset)));
		storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2)
{
	emitStructGetPacked(arg1, arg2, true);
}

void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2)
{
	emitStructGetPacked(arg1, arg2, false);
}

void OpcodeDispatcher::emitStructGetPacked(WASM::TypeIdx arg1, uint32_t arg2, bool signExtend)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg1);
	require(view.kind() == WASM::CanonTypeKind::Struct, "struct.get_s/u requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	require(arg2 < fields.size(), "struct.get_s/u field index is out of range");
	require(fields[arg2].storage.isPacked, "struct.get_s/u requires a packed field");
	requireStack(1);
	require(stackWidths_.back() == 8, "struct.get_s/u requires a reference operand");
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullStructureReference);
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, done);
		// The trap block clobbered R1: reload, then read the packed field.
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 8);
		const bool isI8 = fields[arg2].storage.packed == WASM::ValueTypeCode::I8;
		const sljit_s32 loadOp = isI8 ? (signExtend ? SLJIT_MOV_S8 : SLJIT_MOV_U8)
				: (signExtend ? SLJIT_MOV_S16 : SLJIT_MOV_U16);
		emit(sljit_emit_op1(compiler_, loadOp, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::gcStructFieldOffset(fields, arg2))));
		storeSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg1);
	require(view.kind() == WASM::CanonTypeKind::Struct, "struct.set requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	require(arg2 < fields.size(), "struct.set field index is out of range");
	requireStack(2);
	require(!fields[arg2].storage.isPacked, "the SLJIT backend does not lower packed struct fields yet");
	const WASM::ValueTypeCode opcode = fields[arg2].storage.value.opcode;
	require(opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 struct fields yet");
	const int width = (opcode == WASM::ValueTypeCode::I32
			|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8;
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, stackWidths_.size() - 2, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullReference);
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, done);
		loadSlot(SLJIT_R2, stackWidths_.size() - 1, width);
		emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::gcStructFieldOffset(fields, arg2)),
				SLJIT_R2, 0));
	}
	pop();
	pop();
}

void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg1);
	require(view.kind() == WASM::CanonTypeKind::Struct, "struct.get requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	require(arg2 < fields.size(), "struct.get field index is out of range");
	requireStack(1);
	require(stackWidths_.back() == 8, "struct.get requires a reference operand");
	require(!fields[arg2].storage.isPacked,
			"the SLJIT backend does not lower packed struct fields yet");
	const WASM::ValueTypeCode opcode = fields[arg2].storage.value.opcode;
	require(opcode != WASM::ValueTypeCode::V128,
			"the SLJIT backend does not lower v128 struct fields yet");
	// A reference field is pointer-sized. Loading only the low 32 bits leaves the
	// upper half of the 8-byte stack slot untouched, turning a null field into a
	// stale non-null pointer (D-3). The load and the slot width both follow the
	// field type.
	const int width = (opcode == WASM::ValueTypeCode::I32
			|| opcode == WASM::ValueTypeCode::F32) ? 4 : 8;
	if (!measureOnly_) {
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 8);
		// A null object traps; the field load itself is one memory access.
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullStructureReference);
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, done);
		emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
				static_cast<sljit_sw>(WASM::gcStructFieldOffset(fields, arg2))));
		storeSlot(SLJIT_R2, stackWidths_.size() - 1, width);
	}
	stackWidths_.back() = width;
}

void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(1);
	const int indexWidth = stackWidths_.back();
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, depth, indexWidth);
		emitHelper(SLJIT_ARGS3(P, P, 32, W), &WASM::wasm_table_get);
		storeSlot(SLJIT_R0, depth, 8);
		emitPendingTrapCheck();
	}
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(2);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const int indexWidth = stackWidths_[indexDepth];
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, indexDepth, indexWidth);
		loadSlot(SLJIT_R3, valueDepth, 8);
		emitHelper(SLJIT_ARGS4V(P, 32, W, P), &WASM::wasm_table_set);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 2);
}

void OpcodeDispatcher::dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(1);
	const int indexWidth = stackWidths_.back();
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, depth, indexWidth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>(ordering)));
		emitHelper(SLJIT_ARGS4(P, P, 32, W, 32), &WASM::wasm_atomic_table_get);
		storeSlot(SLJIT_R0, depth, 8);
		emitPendingTrapCheck();
	}
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(2);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const int indexWidth = stackWidths_[indexDepth];
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(ordering) << 32)
						| static_cast<std::uint64_t>(arg))));
		loadSlot(SLJIT_R2, indexDepth, indexWidth);
		loadSlot(SLJIT_R3, valueDepth, 8);
		emitHelper(SLJIT_ARGS4V(P, W, W, P), &sljitAtomicTableSet);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(indexDepth);
}

void OpcodeDispatcher::dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(2);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const int indexWidth = stackWidths_[indexDepth];
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(ordering) << 32)
						| static_cast<std::uint64_t>(arg))));
		loadSlot(SLJIT_R2, indexDepth, indexWidth);
		loadSlot(SLJIT_R3, valueDepth, 8);
		emitHelper(SLJIT_ARGS4(P, P, W, W, P), &sljitAtomicTableXchg);
		storeSlot(SLJIT_R0, indexDepth, 8);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(indexDepth + 1);
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	requireStack(3);
	const std::size_t replacementDepth = stackWidths_.size() - 1;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 3;
	const int indexWidth = stackWidths_[indexDepth];
	if (!measureOnly_) {
		// The expected/replacement pair travels through the scratch cell.
		loadSlot(SLJIT_R1, expectedDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset(), SLJIT_R1, 0));
		loadSlot(SLJIT_R1, replacementDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset() + 8, SLJIT_R1, 0));
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(ordering) << 32)
						| static_cast<std::uint64_t>(arg))));
		loadSlot(SLJIT_R2, indexDepth, indexWidth);
		emit(sljit_get_local_base(compiler_, SLJIT_R3, 0, scratchSlotOffset()));
		emitHelper(SLJIT_ARGS4(P, P, W, W, W), &sljitAtomicTableCmpxchg);
		storeSlot(SLJIT_R0, indexDepth, 8);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(indexDepth + 1);
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg)
{
	if (!reachable_)
		return;
	push(8);
	if (measureOnly_)
		return;
	// SLJIT resolves the callable at run time: its own `resolveCallable` maps a
	// function index to the instance's Callable, which is what a reference is.
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(arg)));
	emitHelper(SLJIT_ARGS2(P, P, 32), &ModuleCompiler::resolveCallable);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
}

void OpcodeDispatcher::emitRefCast(const WASM::HeapType& arg, bool nullable)
{
	if (!arg.isTypeIndex) {
		// Only the kinds a GC object can have are answerable from its header.
		require(arg.abstract == WASM::AbstractHeapType::Any
			|| arg.abstract == WASM::AbstractHeapType::Eq
			|| arg.abstract == WASM::AbstractHeapType::Struct
			|| arg.abstract == WASM::AbstractHeapType::Array,
			"this abstract heap type is not lowered yet");
	}
	requireStack(1);
	require(stackWidths_.back() == 8, "ref.cast requires a reference operand");
	if (measureOnly_)
		return;
	const std::size_t depth = stackWidths_.size() - 1;
	const bool kindQuery = !arg.isTypeIndex;
	const std::uint32_t expected = kindQuery
			? static_cast<std::uint32_t>(static_cast<std::int32_t>(arg.abstract))
			: module_.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
	loadSlot(SLJIT_R1, depth, 8);
	// The nullable form passes null through; the non-null form traps on it.
	sljit_jump* nullPasses = nullptr;
	if (nullable) {
		nullPasses = sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (nullPasses == nullptr)
			ok_ = false;
	} else {
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullReference);
		sljit_label* afterNull = sljit_emit_label(compiler_);
		if (afterNull == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, afterNull);
		// The trap block called a helper: reload the reference.
		loadSlot(SLJIT_R1, depth, 8);
	}
	// Three arguments fill R0, R1 and R2: actual in R1, expected in R2.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
		static_cast<sljit_sw>(WASM::GcHeader::kTypeIdOffset)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
		static_cast<sljit_sw>(expected)));
	loadContext(SLJIT_R0);
	if (kindQuery)
		emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIdMatchesKind);
	else
		emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIsSubtype);
	// The call clobbers the object pointer; restore it for the value push.
	loadSlot(SLJIT_R1, depth, 8);
	sljit_jump* matches = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0,
		SLJIT_IMM, 0);
	if (matches == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::CastFailure);
	sljit_label* afterCast = sljit_emit_label(compiler_);
	if (afterCast == nullptr)
		ok_ = false;
	else if (matches != nullptr)
		sljit_set_label(matches, afterCast);
	if (nullable && nullPasses != nullptr)
		sljit_set_label(nullPasses, afterCast);
}

void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg)
{
	if (!reachable_)
		return;
	emitRefCast(arg, false);
}

void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg)
{
	if (!reachable_)
		return;
	emitRefCast(arg, true);
}

void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg)
{
	if (!reachable_)
		return;
	if (!arg.isTypeIndex) {
		// Only the kinds a GC object can have are answerable from its header.
		require(arg.abstract == WASM::AbstractHeapType::Any
			|| arg.abstract == WASM::AbstractHeapType::Eq
			|| arg.abstract == WASM::AbstractHeapType::Struct
			|| arg.abstract == WASM::AbstractHeapType::Array,
			"this abstract heap type is not lowered yet");
	}
	requireStack(1);
	require(stackWidths_.back() == 8, "ref.test requires a reference operand");
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
	const bool kindQuery = !arg.isTypeIndex;
	const std::uint32_t expected = kindQuery
			? static_cast<std::uint32_t>(static_cast<std::int32_t>(arg.abstract))
			: module_.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
		loadSlot(SLJIT_R1, depth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
		sljit_jump* isNull = sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (isNull == nullptr)
			ok_ = false;
		// Three arguments fill R0, R1 and R2: actual in R1, expected in R2.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
			static_cast<sljit_sw>(WASM::GcHeader::kTypeIdOffset)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(expected)));
		loadContext(SLJIT_R0);
		if (kindQuery)
			emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIdMatchesKind);
		else
			emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIsSubtype);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_R0, 0));
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else {
			if (isNull != nullptr) sljit_set_label(isNull, done);
		}
		storeSlot(SLJIT_R2, depth, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg)
{
	if (!reachable_)
		return;
	if (!arg.isTypeIndex) {
		// Only the kinds a GC object can have are answerable from its header.
		require(arg.abstract == WASM::AbstractHeapType::Any
			|| arg.abstract == WASM::AbstractHeapType::Eq
			|| arg.abstract == WASM::AbstractHeapType::Struct
			|| arg.abstract == WASM::AbstractHeapType::Array,
			"this abstract heap type is not lowered yet");
	}
	requireStack(1);
	require(stackWidths_.back() == 8, "ref.test requires a reference operand");
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
	const bool kindQuery = !arg.isTypeIndex;
	const std::uint32_t expected = kindQuery
			? static_cast<std::uint32_t>(static_cast<std::int32_t>(arg.abstract))
			: module_.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
		loadSlot(SLJIT_R1, depth, 8);
		// A null passes the nullable test; otherwise the type id decides.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
		sljit_jump* isNull = sljit_emit_cmp(compiler_, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
		if (isNull == nullptr)
			ok_ = false;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
		// Three arguments fill R0, R1 and R2: actual in R1, expected in R2.
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
			static_cast<sljit_sw>(WASM::GcHeader::kTypeIdOffset)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(expected)));
		loadContext(SLJIT_R0);
		if (kindQuery)
			emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIdMatchesKind);
		else
			emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIsSubtype);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else {
			if (isNull != nullptr) sljit_set_label(isNull, done);
		}
		storeSlot(SLJIT_R2, depth, 4);
	}
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchRefAsNonNull()
{
	if (!reachable_)
		return;
	requireStack(1);
	require(stackWidths_.back() == 8, "ref.as_non_null requires a reference operand");
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
		sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
				SLJIT_R0, 0, SLJIT_IMM, 0);
		if (notNull == nullptr)
			ok_ = false;
		emitTrap(WASM::TrapKind::NullReference);
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (notNull != nullptr)
			sljit_set_label(notNull, done);
	}
}

void OpcodeDispatcher::dispatchRefEq()
{
	if (!reachable_)
		return;
	requireStack(2);
	require(stackWidths_.back() == 8 && stackWidths_[stackWidths_.size() - 2] == 8,
			"ref.eq requires two reference operands");
	if (!measureOnly_) {
		loadSlot(SLJIT_R0, stackWidths_.size() - 2, 8);
		loadSlot(SLJIT_R1, stackWidths_.size() - 1, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0));
		sljit_jump* taken = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
		if (taken == nullptr)
			ok_ = false;
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1));
		sljit_label* done = sljit_emit_label(compiler_);
		if (done == nullptr)
			ok_ = false;
		else if (taken != nullptr)
			sljit_set_label(taken, done);
		storeSlot(SLJIT_R2, stackWidths_.size() - 2, 4);
	}
	pop();
}

void OpcodeDispatcher::dispatchI8x16Add() { emitV128Binary(codeAddress(&wasm_i8x16_add)); }
void OpcodeDispatcher::dispatchI8x16Sub() { emitV128Binary(codeAddress(&wasm_i8x16_sub)); }
void OpcodeDispatcher::dispatchI16x8Add() { emitV128Binary(codeAddress(&wasm_i16x8_add)); }
void OpcodeDispatcher::dispatchI16x8Sub() { emitV128Binary(codeAddress(&wasm_i16x8_sub)); }
void OpcodeDispatcher::dispatchI16x8Mul() { emitV128Binary(codeAddress(&wasm_i16x8_mul)); }
void OpcodeDispatcher::dispatchI32x4Add() { emitV128Binary(codeAddress(&wasm_i32x4_add)); }
void OpcodeDispatcher::dispatchI32x4Sub() { emitV128Binary(codeAddress(&wasm_i32x4_sub)); }
void OpcodeDispatcher::dispatchI32x4Mul() { emitV128Binary(codeAddress(&wasm_i32x4_mul)); }
void OpcodeDispatcher::dispatchI64x2Add() { emitV128Binary(codeAddress(&wasm_i64x2_add)); }
void OpcodeDispatcher::dispatchI64x2Sub() { emitV128Binary(codeAddress(&wasm_i64x2_sub)); }
void OpcodeDispatcher::dispatchI64x2Mul() { emitV128Binary(codeAddress(&wasm_i64x2_mul)); }
void OpcodeDispatcher::dispatchI8x16Eq() { emitV128Binary(codeAddress(&wasm_i8x16_eq)); }
void OpcodeDispatcher::dispatchI8x16Ne() { emitV128Binary(codeAddress(&wasm_i8x16_ne)); }
void OpcodeDispatcher::dispatchI8x16LtS() { emitV128Binary(codeAddress(&wasm_i8x16_lt_s)); }
void OpcodeDispatcher::dispatchI8x16LtU() { emitV128Binary(codeAddress(&wasm_i8x16_lt_u)); }
void OpcodeDispatcher::dispatchI8x16GtS() { emitV128Binary(codeAddress(&wasm_i8x16_gt_s)); }
void OpcodeDispatcher::dispatchI8x16GtU() { emitV128Binary(codeAddress(&wasm_i8x16_gt_u)); }
void OpcodeDispatcher::dispatchI8x16LeS() { emitV128Binary(codeAddress(&wasm_i8x16_le_s)); }
void OpcodeDispatcher::dispatchI8x16LeU() { emitV128Binary(codeAddress(&wasm_i8x16_le_u)); }
void OpcodeDispatcher::dispatchI8x16GeS() { emitV128Binary(codeAddress(&wasm_i8x16_ge_s)); }
void OpcodeDispatcher::dispatchI8x16GeU() { emitV128Binary(codeAddress(&wasm_i8x16_ge_u)); }
void OpcodeDispatcher::dispatchI16x8Eq() { emitV128Binary(codeAddress(&wasm_i16x8_eq)); }
void OpcodeDispatcher::dispatchI16x8Ne() { emitV128Binary(codeAddress(&wasm_i16x8_ne)); }
void OpcodeDispatcher::dispatchI16x8LtS() { emitV128Binary(codeAddress(&wasm_i16x8_lt_s)); }
void OpcodeDispatcher::dispatchI16x8LtU() { emitV128Binary(codeAddress(&wasm_i16x8_lt_u)); }
void OpcodeDispatcher::dispatchI16x8GtS() { emitV128Binary(codeAddress(&wasm_i16x8_gt_s)); }
void OpcodeDispatcher::dispatchI16x8GtU() { emitV128Binary(codeAddress(&wasm_i16x8_gt_u)); }
void OpcodeDispatcher::dispatchI16x8LeS() { emitV128Binary(codeAddress(&wasm_i16x8_le_s)); }
void OpcodeDispatcher::dispatchI16x8LeU() { emitV128Binary(codeAddress(&wasm_i16x8_le_u)); }
void OpcodeDispatcher::dispatchI16x8GeS() { emitV128Binary(codeAddress(&wasm_i16x8_ge_s)); }
void OpcodeDispatcher::dispatchI16x8GeU() { emitV128Binary(codeAddress(&wasm_i16x8_ge_u)); }
void OpcodeDispatcher::dispatchI32x4Eq() { emitV128Binary(codeAddress(&wasm_i32x4_eq)); }
void OpcodeDispatcher::dispatchI32x4Ne() { emitV128Binary(codeAddress(&wasm_i32x4_ne)); }
void OpcodeDispatcher::dispatchI32x4LtS() { emitV128Binary(codeAddress(&wasm_i32x4_lt_s)); }
void OpcodeDispatcher::dispatchI32x4LtU() { emitV128Binary(codeAddress(&wasm_i32x4_lt_u)); }
void OpcodeDispatcher::dispatchI32x4GtS() { emitV128Binary(codeAddress(&wasm_i32x4_gt_s)); }
void OpcodeDispatcher::dispatchI32x4GtU() { emitV128Binary(codeAddress(&wasm_i32x4_gt_u)); }
void OpcodeDispatcher::dispatchI32x4LeS() { emitV128Binary(codeAddress(&wasm_i32x4_le_s)); }
void OpcodeDispatcher::dispatchI32x4LeU() { emitV128Binary(codeAddress(&wasm_i32x4_le_u)); }
void OpcodeDispatcher::dispatchI32x4GeS() { emitV128Binary(codeAddress(&wasm_i32x4_ge_s)); }
void OpcodeDispatcher::dispatchI32x4GeU() { emitV128Binary(codeAddress(&wasm_i32x4_ge_u)); }
void OpcodeDispatcher::dispatchI64x2Eq() { emitV128Binary(codeAddress(&wasm_i64x2_eq)); }
void OpcodeDispatcher::dispatchI64x2Ne() { emitV128Binary(codeAddress(&wasm_i64x2_ne)); }
void OpcodeDispatcher::dispatchI64x2LtS() { emitV128Binary(codeAddress(&wasm_i64x2_lt_s)); }
void OpcodeDispatcher::dispatchI64x2GtS() { emitV128Binary(codeAddress(&wasm_i64x2_gt_s)); }
void OpcodeDispatcher::dispatchI64x2LeS() { emitV128Binary(codeAddress(&wasm_i64x2_le_s)); }
void OpcodeDispatcher::dispatchI64x2GeS() { emitV128Binary(codeAddress(&wasm_i64x2_ge_s)); }

void OpcodeDispatcher::dispatchI8x16Neg() { emitV128Unary(codeAddress(&wasm_i8x16_neg)); }
void OpcodeDispatcher::dispatchI16x8Neg() { emitV128Unary(codeAddress(&wasm_i16x8_neg)); }
void OpcodeDispatcher::dispatchI32x4Neg() { emitV128Unary(codeAddress(&wasm_i32x4_neg)); }
void OpcodeDispatcher::dispatchI64x2Neg() { emitV128Unary(codeAddress(&wasm_i64x2_neg)); }

void OpcodeDispatcher::dispatchI8x16AllTrue() { emitV128Reduce(codeAddress(&wasm_i8x16_all_true)); }
void OpcodeDispatcher::dispatchI16x8AllTrue() { emitV128Reduce(codeAddress(&wasm_i16x8_all_true)); }
void OpcodeDispatcher::dispatchI32x4AllTrue() { emitV128Reduce(codeAddress(&wasm_i32x4_all_true)); }
void OpcodeDispatcher::dispatchI64x2AllTrue() { emitV128Reduce(codeAddress(&wasm_i64x2_all_true)); }

void OpcodeDispatcher::dispatchI32Add() { emitIntegerBinary(SLJIT_ADD, 4); }

void OpcodeDispatcher::dispatchI32Sub() { emitIntegerBinary(SLJIT_SUB, 4); }
void OpcodeDispatcher::dispatchI32Mul() { emitIntegerBinary(SLJIT_MUL, 4); }
void OpcodeDispatcher::dispatchI32And() { emitIntegerBinary(SLJIT_AND, 4); }
void OpcodeDispatcher::dispatchI32Or() { emitIntegerBinary(SLJIT_OR, 4); }
void OpcodeDispatcher::dispatchI32Xor() { emitIntegerBinary(SLJIT_XOR, 4); }
// Wasm masks a shift count by the operand width, which is SLJIT's `M` (masked)
// shift form; the plain form leaves an out-of-range count undefined.
void OpcodeDispatcher::dispatchI32Shl() { emitIntegerBinary(SLJIT_MSHL, 4); }
void OpcodeDispatcher::dispatchI32ShrS() { emitIntegerBinary(SLJIT_MASHR, 4); }
void OpcodeDispatcher::dispatchI32ShrU() { emitIntegerBinary(SLJIT_MLSHR, 4); }
void OpcodeDispatcher::dispatchI32Rotl() { emitIntegerBinary(SLJIT_ROTL, 4); }
void OpcodeDispatcher::dispatchI32Rotr() { emitIntegerBinary(SLJIT_ROTR, 4); }
void OpcodeDispatcher::dispatchI32Clz() { emitIntegerUnary(SLJIT_CLZ, 4, 4); }
void OpcodeDispatcher::dispatchI32Ctz() { emitIntegerUnary(SLJIT_CTZ, 4, 4); }

void OpcodeDispatcher::dispatchI32Popcnt()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	emitHelper(SLJIT_ARGS1(32, 32), &sljitI32Popcnt);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchI32DivS() { emitCheckedBinary(codeAddress(&sljitI32DivS), 4, "i32.div_s"); }
void OpcodeDispatcher::dispatchI32DivU() { emitCheckedBinary(codeAddress(&sljitI32DivU), 4, "i32.div_u"); }
void OpcodeDispatcher::dispatchI32RemS() { emitCheckedBinary(codeAddress(&sljitI32RemS), 4, "i32.rem_s"); }
void OpcodeDispatcher::dispatchI32RemU() { emitCheckedBinary(codeAddress(&sljitI32RemU), 4, "i32.rem_u"); }

void OpcodeDispatcher::dispatchI32Eqz() { emitEqz(4); }
void OpcodeDispatcher::dispatchI32Eq() { emitCompare(SLJIT_EQUAL, 4); }
void OpcodeDispatcher::dispatchI32Ne() { emitCompare(SLJIT_NOT_EQUAL, 4); }
void OpcodeDispatcher::dispatchI32LtS() { emitCompare(SLJIT_SIG_LESS, 4); }
void OpcodeDispatcher::dispatchI32LtU() { emitCompare(SLJIT_LESS, 4); }
void OpcodeDispatcher::dispatchI32GtS() { emitCompare(SLJIT_SIG_GREATER, 4); }
void OpcodeDispatcher::dispatchI32GtU() { emitCompare(SLJIT_GREATER, 4); }
void OpcodeDispatcher::dispatchI32LeS() { emitCompare(SLJIT_SIG_LESS_EQUAL, 4); }
void OpcodeDispatcher::dispatchI32LeU() { emitCompare(SLJIT_LESS_EQUAL, 4); }
void OpcodeDispatcher::dispatchI32GeS() { emitCompare(SLJIT_SIG_GREATER_EQUAL, 4); }
void OpcodeDispatcher::dispatchI32GeU() { emitCompare(SLJIT_GREATER_EQUAL, 4); }

void OpcodeDispatcher::dispatchI32Extend8S() { emitIntegerUnary(SLJIT_MOV32_S8, 4, 4); }
void OpcodeDispatcher::dispatchI32Extend16S() { emitIntegerUnary(SLJIT_MOV32_S16, 4, 4); }

void OpcodeDispatcher::dispatchI32WrapI64()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	stackWidths_.back() = 4;
}

void OpcodeDispatcher::dispatchI64Add() { emitIntegerBinary(SLJIT_ADD, 8); }
void OpcodeDispatcher::dispatchI64Sub() { emitIntegerBinary(SLJIT_SUB, 8); }
void OpcodeDispatcher::dispatchI64Mul() { emitIntegerBinary(SLJIT_MUL, 8); }
void OpcodeDispatcher::dispatchI64And() { emitIntegerBinary(SLJIT_AND, 8); }
void OpcodeDispatcher::dispatchI64Or() { emitIntegerBinary(SLJIT_OR, 8); }
void OpcodeDispatcher::dispatchI64Xor() { emitIntegerBinary(SLJIT_XOR, 8); }
void OpcodeDispatcher::dispatchI64Shl() { emitIntegerBinary(SLJIT_MSHL, 8); }
void OpcodeDispatcher::dispatchI64ShrS() { emitIntegerBinary(SLJIT_MASHR, 8); }
void OpcodeDispatcher::dispatchI64ShrU() { emitIntegerBinary(SLJIT_MLSHR, 8); }
void OpcodeDispatcher::dispatchI64Rotl() { emitIntegerBinary(SLJIT_ROTL, 8); }
void OpcodeDispatcher::dispatchI64Rotr() { emitIntegerBinary(SLJIT_ROTR, 8); }

void OpcodeDispatcher::dispatchI64DivS() { emitCheckedBinary(codeAddress(&sljitI64DivS), 8, "i64.div_s"); }
void OpcodeDispatcher::dispatchI64DivU() { emitCheckedBinary(codeAddress(&sljitI64DivU), 8, "i64.div_u"); }
void OpcodeDispatcher::dispatchI64RemS() { emitCheckedBinary(codeAddress(&sljitI64RemS), 8, "i64.rem_s"); }
void OpcodeDispatcher::dispatchI64RemU() { emitCheckedBinary(codeAddress(&sljitI64RemU), 8, "i64.rem_u"); }

void OpcodeDispatcher::dispatchI64Eqz() { emitEqz(8); }
void OpcodeDispatcher::dispatchI64Eq() { emitCompare(SLJIT_EQUAL, 8); }
void OpcodeDispatcher::dispatchI64Ne() { emitCompare(SLJIT_NOT_EQUAL, 8); }
void OpcodeDispatcher::dispatchI64LtS() { emitCompare(SLJIT_SIG_LESS, 8); }
void OpcodeDispatcher::dispatchI64LtU() { emitCompare(SLJIT_LESS, 8); }
void OpcodeDispatcher::dispatchI64GtS() { emitCompare(SLJIT_SIG_GREATER, 8); }
void OpcodeDispatcher::dispatchI64GtU() { emitCompare(SLJIT_GREATER, 8); }
void OpcodeDispatcher::dispatchI64LeS() { emitCompare(SLJIT_SIG_LESS_EQUAL, 8); }
void OpcodeDispatcher::dispatchI64LeU() { emitCompare(SLJIT_LESS_EQUAL, 8); }
void OpcodeDispatcher::dispatchI64GeS() { emitCompare(SLJIT_SIG_GREATER_EQUAL, 8); }
void OpcodeDispatcher::dispatchI64GeU() { emitCompare(SLJIT_GREATER_EQUAL, 8); }

void OpcodeDispatcher::dispatchI64Extend8S() { emitIntegerUnary(SLJIT_MOV_S8, 8, 8); }
void OpcodeDispatcher::dispatchI64Extend16S() { emitIntegerUnary(SLJIT_MOV_S16, 8, 8); }
void OpcodeDispatcher::dispatchI64Extend32S() { emitIntegerUnary(SLJIT_MOV_S32, 8, 8); }

void OpcodeDispatcher::dispatchI64ExtendI32S()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0));
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
	stackWidths_.back() = 8;
}

void OpcodeDispatcher::dispatchI64ExtendI32U()
{
	if (!reachable_)
		return;
	requireStack(1);
	if (measureOnly_)
		return;
	loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
	storeSlot(SLJIT_R0, stackWidths_.size() - 1, 8);
	stackWidths_.back() = 8;
}

const WASM::GlobalType& OpcodeDispatcher::requireGlobalIndex(WASM::GlobalIdx index)
{
	require(index < module_.globals.size(), "the global index is out of range");
	const WASM::GlobalType& type = module_.globals[index].type;
	require(widthOf(type.contentType) != 0,
			"the SLJIT backend only models i32, i64, f32 and f64 globals");
	return type;
}

void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType& type = requireGlobalIndex(arg);
	const int width = widthOf(type.contentType);
	push(width);
	if (measureOnly_)
		return;
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::VMContext, globals))));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(arg * sizeof(WASM::Value*))));
	emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(width == 4 ? offsetof(WASM::Value, i32)
					: offsetof(WASM::Value, i64))));
	storeSlot(SLJIT_R1, stackWidths_.size() - 1, width);
}

void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType& type = requireGlobalIndex(arg);
	require(type.isMutable, "global.set references an immutable global");
	requireStack(1);
	pop();
	if (measureOnly_)
		return;
	const int width = widthOf(type.contentType);
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::VMContext, globals))));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(arg * sizeof(WASM::Value*))));
	loadSlot(SLJIT_R1, stackWidths_.size(), width);
	emit(sljit_emit_op1(compiler_, width == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(width == 4 ? offsetof(WASM::Value, i32)
					: offsetof(WASM::Value, i64)), SLJIT_R1, 0));
}

const WASM::MemoryType& OpcodeDispatcher::requireMemoryIndex(WASM::MemIdx index)
{
	require(index < module_.memories.size(), "the memory index is out of range");
	return module_.memories[index];
}

void OpcodeDispatcher::emitCheckedMemoryAddress(const WASM::MemArg& arg,
		std::uint32_t width, std::size_t addressDepth)
{
	const WASM::MemoryType& memory = requireMemoryIndex(arg.memidx);
	const bool is64 = memory.indexType == WASM::MemoryIndexType::I64;
	require(!arg.isArray, "the SLJIT backend does not lower GC-array access");
	if (!is64)
		require(arg.offset <= UINT32_MAX, "the memory32 offset exceeds the encoded u32 range");

	loadContext(SLJIT_R0);
	if (is64) {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(width) << 32)
						| static_cast<std::uint64_t>(arg.memidx))));
		loadSlot(SLJIT_R2, addressDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg.offset)));
		emitHelper(SLJIT_ARGS4(P, P, W, W, W), &sljitMemoryAddress64);
	} else {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg.memidx)));
		loadSlot(SLJIT_R2, addressDepth, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(width) << 32)
						| arg.offset)));
		emitHelper(SLJIT_ARGS4(P, P, 32, 32, W), &sljitMemoryAddress);
	}
	// The pointer survives the pending-trap probe in the scratch slot.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset(), SLJIT_R0, 0));
	emitPendingTrapCheck();
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset()));
}

void OpcodeDispatcher::slotAddress(int reg, std::size_t depth)
{
	emit(sljit_get_local_base(compiler_, reg, 0, stackSlotOffset(depth)));
}

void OpcodeDispatcher::copyLocalToStack(std::size_t localIndex, std::size_t depth)
{
	for (int word = 0; word < 2; ++word) {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				localSlotOffset(localIndex) + word * 8));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				stackSlotOffset(depth) + word * 8, SLJIT_R0, 0));
	}
}

void OpcodeDispatcher::copyStackToLocal(std::size_t depth, std::size_t localIndex)
{
	for (int word = 0; word < 2; ++word) {
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				stackSlotOffset(depth) + word * 8));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				localSlotOffset(localIndex) + word * 8, SLJIT_R0, 0));
	}
}

void OpcodeDispatcher::emitMemoryLoad(const WASM::MemArg& arg, std::uint32_t width,
		sljit_s32 loadOp, int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(1);
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, stackWidths_.size() - 1);
		emit(sljit_emit_op1(compiler_, loadOp, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), 0));
		storeSlot(SLJIT_R1, stackWidths_.size() - 1, resultWidth);
	}
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::emitMemoryStore(const WASM::MemArg& arg, std::uint32_t width,
		sljit_s32 storeOp)
{
	if (!reachable_)
		return;
	requireStack(2);
	pop();
	pop();
	if (measureOnly_)
		return;
	emitCheckedMemoryAddress(arg, width, stackWidths_.size());
	loadSlot(SLJIT_R1, stackWidths_.size() + 1, static_cast<int>(width));
	emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R1, 0));
}


void OpcodeDispatcher::emitAtomicLoad(const WASM::MemArg& arg, std::uint32_t width,
		int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(1);
	const std::size_t depth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, depth);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(width)));
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg.ordering)));
		emitHelper(SLJIT_ARGS3(W, P, 32, 32), &wasm_atomic_load);
		emitPendingTrapCheck();
		storeSlot(SLJIT_R0, depth, resultWidth);
	}
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::emitAtomicStore(const WASM::MemArg& arg, std::uint32_t width,
		int valueWidth)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	const std::size_t addressDepth = stackWidths_.size() - 2;
	pop();
	pop();
	if (measureOnly_)
		return;
	emitCheckedMemoryAddress(arg, width, addressDepth);
	loadSlot(SLJIT_R1, valueDepth, valueWidth);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(width)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(arg.ordering)));
	emitHelper(SLJIT_ARGS4V(P, W, 32, 32), &wasm_atomic_store);
	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitAtomicRmw(const WASM::MemArg& arg, std::uint32_t width,
		std::uint32_t op, int valueWidth, int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	const std::size_t addressDepth = stackWidths_.size() - 2;
	pop();
	pop();
	if (measureOnly_) {
		push(resultWidth);
		return;
	}
	emitCheckedMemoryAddress(arg, width, addressDepth);
	loadSlot(SLJIT_R1, valueDepth, valueWidth);
	const std::uint32_t packed = width | (op << 8) | (arg.ordering << 16);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(packed)));
	emitHelper(SLJIT_ARGS3(W, P, W, 32), &sljitAtomicRmw);
	emitPendingTrapCheck();
	storeSlot(SLJIT_R0, addressDepth, resultWidth);
	push(resultWidth);
}

void OpcodeDispatcher::emitAtomicCmpxchg(const WASM::MemArg& arg, std::uint32_t width,
		int valueWidth, int resultWidth)
{
	if (!reachable_)
		return;
	requireStack(3);
	const std::size_t replacementDepth = stackWidths_.size() - 1;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	const std::size_t addressDepth = stackWidths_.size() - 3;
	pop();
	pop();
	pop();
	if (measureOnly_) {
		push(resultWidth);
		return;
	}
	emitCheckedMemoryAddress(arg, width, addressDepth);
	loadSlot(SLJIT_R1, expectedDepth, valueWidth);
	loadSlot(SLJIT_R2, replacementDepth, valueWidth);
	const std::uint32_t packed = width | (arg.ordering << 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(packed)));
	emitHelper(SLJIT_ARGS4(W, P, W, W, 32), &sljitAtomicCmpxchg);
	emitPendingTrapCheck();
	storeSlot(SLJIT_R0, addressDepth, resultWidth);
	push(resultWidth);
}

void OpcodeDispatcher::dispatchI32AtomicLoad(WASM::MemArg arg)
{ emitAtomicLoad(arg, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicLoad(WASM::MemArg arg)
{ emitAtomicLoad(arg, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicLoad8U(WASM::MemArg arg)
{ emitAtomicLoad(arg, 1, 4); }
void OpcodeDispatcher::dispatchI32AtomicLoad16U(WASM::MemArg arg)
{ emitAtomicLoad(arg, 2, 4); }
void OpcodeDispatcher::dispatchI64AtomicLoad8U(WASM::MemArg arg)
{ emitAtomicLoad(arg, 1, 8); }
void OpcodeDispatcher::dispatchI64AtomicLoad16U(WASM::MemArg arg)
{ emitAtomicLoad(arg, 2, 8); }
void OpcodeDispatcher::dispatchI64AtomicLoad32U(WASM::MemArg arg)
{ emitAtomicLoad(arg, 4, 8); }
void OpcodeDispatcher::dispatchI32AtomicStore(WASM::MemArg arg)
{ emitAtomicStore(arg, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicStore(WASM::MemArg arg)
{ emitAtomicStore(arg, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicStore8(WASM::MemArg arg)
{ emitAtomicStore(arg, 1, 4); }
void OpcodeDispatcher::dispatchI32AtomicStore16(WASM::MemArg arg)
{ emitAtomicStore(arg, 2, 4); }
void OpcodeDispatcher::dispatchI64AtomicStore8(WASM::MemArg arg)
{ emitAtomicStore(arg, 1, 8); }
void OpcodeDispatcher::dispatchI64AtomicStore16(WASM::MemArg arg)
{ emitAtomicStore(arg, 2, 8); }
void OpcodeDispatcher::dispatchI64AtomicStore32(WASM::MemArg arg)
{ emitAtomicStore(arg, 4, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwAdd(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 0, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwAdd(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 0, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8AddU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 0, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16AddU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 0, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8AddU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 0, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16AddU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 0, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32AddU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 0, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwSub(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 1, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwSub(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 1, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8SubU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 1, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16SubU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 1, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8SubU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 1, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16SubU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 1, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32SubU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 1, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwAnd(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 2, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwAnd(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 2, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8AndU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 2, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16AndU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 2, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8AndU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 2, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16AndU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 2, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32AndU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 2, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwOr(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 3, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwOr(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 3, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8OrU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 3, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16OrU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 3, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8OrU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 3, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16OrU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 3, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32OrU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 3, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwXor(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 4, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwXor(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 4, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8XorU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 4, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16XorU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 4, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8XorU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 4, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16XorU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 4, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32XorU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 4, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwXchg(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 5, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwXchg(WASM::MemArg arg)
{ emitAtomicRmw(arg, 8, 5, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8XchgU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 5, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16XchgU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 5, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8XchgU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 1, 5, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16XchgU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 2, 5, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32XchgU(WASM::MemArg arg)
{ emitAtomicRmw(arg, 4, 5, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 4, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 8, 8, 8); }
void OpcodeDispatcher::dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 1, 4, 4); }
void OpcodeDispatcher::dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 2, 4, 4); }
void OpcodeDispatcher::dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 1, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 2, 8, 8); }
void OpcodeDispatcher::dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg)
{ emitAtomicCmpxchg(arg, 4, 8, 8); }
void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg)
{
	if (!reachable_)
		return;
	requireStack(2);
	const std::size_t countDepth = stackWidths_.size() - 1;
	const std::size_t addressDepth = stackWidths_.size() - 2;
	pop();
	pop();
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, 4, addressDepth);
		loadSlot(SLJIT_R1, countDepth, 4);
		emitHelper(SLJIT_ARGS2(32, P, 32), &wasm_atomic_notify);
		emitPendingTrapCheck();
		storeSlot(SLJIT_R0, addressDepth, 4);
	}
	push(4);
}

void OpcodeDispatcher::emitAtomicWait(const WASM::MemArg& arg, std::uint32_t width, int valueWidth)
{
	if (!reachable_)
		return;
	requireStack(3);
	const std::size_t timeoutDepth = stackWidths_.size() - 1;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	const std::size_t addressDepth = stackWidths_.size() - 3;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitCheckedMemoryAddress(arg, width, addressDepth);
		loadSlot(SLJIT_R1, expectedDepth, valueWidth);
		loadSlot(SLJIT_R2, timeoutDepth, 8);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>(width)));
		emitHelper(SLJIT_ARGS4(32, P, W, W, 32), &sljitAtomicWait);
		emitPendingTrapCheck();
		storeSlot(SLJIT_R0, addressDepth, 4);
	}
	push(4);
}

void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg)
{ emitAtomicWait(arg, 4, 4); }
void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg)
{ emitAtomicWait(arg, 8, 8); }

void OpcodeDispatcher::dispatchAtomicFence(uint8_t ordering)
{
	if (!reachable_ || measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS1V(32), &wasm_atomic_fence);
}

void OpcodeDispatcher::dispatchAtomicPause()
{
	if (!reachable_ || measureOnly_)
		return;
	emitHelper(SLJIT_ARGS0V(), &wasm_atomic_pause);
}


// ── Shared-everything atomics: numeric slots ────────────────────────────────
// The same helper ABI as the linear-memory family. Reference slots need the
// GC retain/release barrier and are refused below; the table forms are
// reference-only and remain refused. Numeric globals, struct fields and array
// elements share the address-plus-width lowering.

static std::uint32_t sharedAtomicWidth(WASM::ValueTypeCode code) noexcept
{
	switch (code) {
	case WASM::ValueTypeCode::I32:
	case WASM::ValueTypeCode::F32: return 4;
	case WASM::ValueTypeCode::I64:
	case WASM::ValueTypeCode::F64: return 8;
	case WASM::ValueTypeCode::V128: return 0;
	default: return 8; // references
	}
}

//! Four-argument reference cmpxchg wrapper: the expected/replacement pair lives
//! in the scratch cell because the C ABI call ceiling is four arguments.
extern "C" void* sljitAtomicRefCmpxchg(WASM::VMContext* vm, void** address,
		const void** pair, uint32_t ordering) noexcept
{
	return wasm_atomic_ref_cmpxchg(vm, address, const_cast<void*>(pair[0]),
			const_cast<void*>(pair[1]), ordering);
}

void OpcodeDispatcher::emitSharedAtomicRefStore(uint8_t ordering, std::size_t valueDepth)
{
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset(), SLJIT_R0, 0));
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset()));
	loadSlot(SLJIT_R2, valueDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS4(P, P, P, P, 32), &wasm_atomic_ref_store);
	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitSharedAtomicRefXchg(uint8_t ordering, std::size_t valueDepth,
		std::size_t resultDepth)
{
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset(), SLJIT_R0, 0));
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset()));
	loadSlot(SLJIT_R2, valueDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS4(P, P, P, P, 32), &wasm_atomic_ref_xchg);
	storeSlot(SLJIT_R0, resultDepth, 8);
	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitSharedAtomicRefCmpxchg(uint8_t ordering,
		std::size_t expectedDepth, std::size_t replacementDepth, std::size_t resultDepth)
{
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset(), SLJIT_R0, 0));
	loadSlot(SLJIT_R1, expectedDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset(), SLJIT_R1, 0));
	loadSlot(SLJIT_R1, replacementDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset() + 8, SLJIT_R1, 0));
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP),
			targetSlotOffset()));
	emit(sljit_get_local_base(compiler_, SLJIT_R2, 0, scratchSlotOffset()));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS4(P, P, P, P, 32), &sljitAtomicRefCmpxchg);
	storeSlot(SLJIT_R0, resultDepth, 8);
	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitGlobalAtomicAddress(WASM::GlobalIdx arg)
{
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(offsetof(WASM::VMContext, globals))));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0),
			static_cast<sljit_sw>(arg * sizeof(WASM::Value*))));
}

void OpcodeDispatcher::emitStructAtomicAddress(std::size_t refDepth, std::size_t fieldOffset)
{
	loadSlot(SLJIT_R1, refDepth, 8);
	sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R1, 0, SLJIT_IMM, 0);
	if (notNull == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::NullStructureReference);
	sljit_label* done = sljit_emit_label(compiler_);
	if (done == nullptr)
		ok_ = false;
	else if (notNull != nullptr)
		sljit_set_label(notNull, done);
	loadSlot(SLJIT_R1, refDepth, 8);
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(fieldOffset)));
}

void OpcodeDispatcher::emitArrayAtomicAddress(std::size_t refDepth, std::size_t indexDepth,
		std::size_t lengthOffset, std::size_t dataOffset, std::uint32_t elementBytes)
{
	loadSlot(SLJIT_R1, refDepth, 8);
	sljit_jump* notNull = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R1, 0, SLJIT_IMM, 0);
	if (notNull == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::NullArrayReference);
	sljit_label* afterNull = sljit_emit_label(compiler_);
	if (afterNull == nullptr)
		ok_ = false;
	else if (notNull != nullptr)
		sljit_set_label(notNull, afterNull);
	loadSlot(SLJIT_R1, refDepth, 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
			static_cast<sljit_sw>(lengthOffset)));
	loadSlot(SLJIT_R3, indexDepth, 4);
	sljit_jump* inBounds = sljit_emit_cmp(compiler_, SLJIT_LESS,
			SLJIT_R3, 0, SLJIT_R2, 0);
	if (inBounds == nullptr)
		ok_ = false;
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	sljit_label* afterBounds = sljit_emit_label(compiler_);
	if (afterBounds == nullptr)
		ok_ = false;
	else if (inBounds != nullptr)
		sljit_set_label(inBounds, afterBounds);
	loadSlot(SLJIT_R1, refDepth, 8);
	loadSlot(SLJIT_R2, indexDepth, 4);
	if (elementBytes != 1)
		emit(sljit_emit_op2(compiler_, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM,
				static_cast<sljit_sw>(elementBytes)));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0));
	emit(sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(dataOffset)));
}

void OpcodeDispatcher::emitSharedAtomicLoad(uint8_t ordering, std::uint32_t width,
		int resultWidth, std::size_t depth, int signBits)
{
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(width)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS3(W, P, 32, 32), &wasm_atomic_load);
	emitPendingTrapCheck();
	if (signBits > 0) {
		const sljit_sw shift = 64 - signBits;
		emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, shift));
		emit(sljit_emit_op2(compiler_, SLJIT_ASHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, shift));
	}
	storeSlot(SLJIT_R0, depth, resultWidth);
}

void OpcodeDispatcher::emitSharedAtomicStore(uint8_t ordering, std::uint32_t width,
		int valueWidth, std::size_t valueDepth)
{
	loadSlot(SLJIT_R1, valueDepth, valueWidth);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(width)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(ordering)));
	emitHelper(SLJIT_ARGS4V(P, W, 32, 32), &wasm_atomic_store);
	emitPendingTrapCheck();
}

void OpcodeDispatcher::emitSharedAtomicRmw(uint8_t ordering, std::uint32_t width,
		std::uint32_t op, int valueWidth, std::size_t valueDepth, std::size_t resultDepth)
{
	loadSlot(SLJIT_R1, valueDepth, valueWidth);
	const std::uint32_t packed = width | (op << 8) | (static_cast<std::uint32_t>(ordering) << 16);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
			static_cast<sljit_sw>(packed)));
	emitHelper(SLJIT_ARGS3(W, P, W, 32), &sljitAtomicRmw);
	emitPendingTrapCheck();
	storeSlot(SLJIT_R0, resultDepth, valueWidth);
}

void OpcodeDispatcher::emitSharedAtomicCmpxchg(uint8_t ordering, std::uint32_t width,
		int valueWidth, std::size_t expectedDepth, std::size_t replacementDepth,
		std::size_t resultDepth)
{
	loadSlot(SLJIT_R1, expectedDepth, valueWidth);
	loadSlot(SLJIT_R2, replacementDepth, valueWidth);
	const std::uint32_t packed = width | (static_cast<std::uint32_t>(ordering) << 8);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>(packed)));
	emitHelper(SLJIT_ARGS4(W, P, W, W, 32), &sljitAtomicCmpxchg);
	emitPendingTrapCheck();
	storeSlot(SLJIT_R0, resultDepth, valueWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	push(resultWidth);
	if (measureOnly_)
		return;
	emitGlobalAtomicAddress(arg);
	emitSharedAtomicLoad(ordering, width, resultWidth, stackWidths_.size() - 1);
}

void OpcodeDispatcher::dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.set references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (measureOnly_)
		return;
	emitGlobalAtomicAddress(arg);
	emitSharedAtomicStore(ordering, width, resultWidth, valueDepth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(1);
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, resultWidth, valueDepth, valueDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg)
{
	if (!reachable_)
		return;
	const WASM::GlobalType type = requireGlobalIndex(arg);
	const std::uint32_t width = sharedAtomicWidth(type.contentType.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 globals yet");
	const int resultWidth = width <= 4 ? 4 : 8;
	require(type.isMutable, "global.atomic.rmw references an immutable global");
	requireStack(2);
	const std::size_t replacementDepth = stackWidths_.size() - 1;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	pop();
	pop();
	if (!measureOnly_) {
		emitGlobalAtomicAddress(arg);
		emitSharedAtomicCmpxchg(ordering, width, resultWidth, expectedDepth, replacementDepth, expectedDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	requireStack(1);
	const std::size_t refDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, 0);
	}
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(storage.isPacked, "struct.atomic.get_s requires a packed field");
	requireStack(1);
	const std::size_t refDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, width * 8);
	}
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(storage.isPacked, "struct.atomic.get_u requires a packed field");
	requireStack(1);
	const std::size_t refDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, 0);
	}
	stackWidths_.back() = resultWidth;
}

void OpcodeDispatcher::dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (measureOnly_)
		return;
	emitStructAtomicAddress(refDepth, offset);
	if (!storage.isPacked && isReferenceType(storage.value.opcode)) {
		emitSharedAtomicRefStore(ordering, valueDepth);
		return;
	}
	emitSharedAtomicStore(ordering, width, valueWidth, valueDepth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		if (!storage.isPacked && isReferenceType(storage.value.opcode))
			emitSharedAtomicRefXchg(ordering, valueDepth, refDepth);
		else
	emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2)
{
	if (!reachable_)
		return;
	const auto fields = module_.typeView(arg1).structFields();
	require(arg2 < fields.size(), "the struct field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 fields yet");
	const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(field.isMutable, "struct.atomic operation on an immutable field");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	const std::size_t replacementDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitStructAtomicAddress(refDepth, offset);
		if (!storage.isPacked && isReferenceType(storage.value.opcode))
			emitSharedAtomicRefCmpxchg(ordering, expectedDepth, replacementDepth, refDepth);
		else
	emitSharedAtomicCmpxchg(ordering, width, valueWidth, expectedDepth, replacementDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, 0);
	}
	stackWidths_.resize(refDepth);
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(storage.isPacked, "array.atomic.get_s requires a packed element");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, width * 8);
	}
	stackWidths_.resize(refDepth);
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(storage.isPacked, "array.atomic.get_u requires a packed element");
	requireStack(2);
	const std::size_t refDepth = stackWidths_.size() - 2;
	const std::size_t indexDepth = stackWidths_.size() - 1;
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, resultWidth, refDepth, 0);
	}
	stackWidths_.resize(refDepth);
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (measureOnly_)
		return;
	emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
	if (!storage.isPacked && isReferenceType(storage.value.opcode)) {
			emitSharedAtomicRefStore(ordering, valueDepth);
			return;
		}
		emitSharedAtomicStore(ordering, width, valueWidth, valueDepth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(3);
	const std::size_t refDepth = stackWidths_.size() - 3;
	const std::size_t indexDepth = stackWidths_.size() - 2;
	const std::size_t valueDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		if (!storage.isPacked && isReferenceType(storage.value.opcode))
				emitSharedAtomicRefXchg(ordering, valueDepth, refDepth);
			else
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, valueWidth, valueDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg)
{
	if (!reachable_)
		return;
	const WASM::CoreTypeView view = module_.typeView(arg);
	require(view.kind() == WASM::CanonTypeKind::Array, "an array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicWidth(storage.value.opcode);
	require(width != 0,
			"the SLJIT backend does not lower shared atomics for reference or v128 elements yet");
	const std::size_t lengthOffset = WASM::GcHeader::kArrayLengthOffset;
	const std::size_t dataOffset = WASM::gcArrayDataOffset(storage);
	const std::uint32_t elementBytes = WASM::gcStorageBytes(storage);
	const int resultWidth = width <= 4 ? 4 : 8;
	const int valueWidth = width <= 4 ? 4 : 8;
	require(element.isMutable, "array.atomic operation on an immutable array");
	requireStack(4);
	const std::size_t refDepth = stackWidths_.size() - 4;
	const std::size_t indexDepth = stackWidths_.size() - 3;
	const std::size_t expectedDepth = stackWidths_.size() - 2;
	const std::size_t replacementDepth = stackWidths_.size() - 1;
	pop();
	pop();
	pop();
	pop();
	if (!measureOnly_) {
		emitArrayAtomicAddress(refDepth, indexDepth, lengthOffset, dataOffset, elementBytes);
		if (!storage.isPacked && isReferenceType(storage.value.opcode))
				emitSharedAtomicRefCmpxchg(ordering, expectedDepth, replacementDepth, refDepth);
			else
		emitSharedAtomicCmpxchg(ordering, width, valueWidth, expectedDepth, replacementDepth, refDepth);
	}
	push(resultWidth);
}

void OpcodeDispatcher::dispatchI32Load(WASM::MemArg arg)
{ emitMemoryLoad(arg, 4, SLJIT_MOV_U32, 4); }
void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg arg)
{ emitMemoryLoad(arg, 1, SLJIT_MOV_S8, 4); }
void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg arg)
{ emitMemoryLoad(arg, 1, SLJIT_MOV_U8, 4); }
void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg arg)
{ emitMemoryLoad(arg, 2, SLJIT_MOV_S16, 4); }
void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg arg)
{ emitMemoryLoad(arg, 2, SLJIT_MOV_U16, 4); }
void OpcodeDispatcher::dispatchI32Store(WASM::MemArg arg)
{ emitMemoryStore(arg, 4, SLJIT_MOV32); }
void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg arg)
{ emitMemoryStore(arg, 1, SLJIT_MOV_U8); }
void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg arg)
{ emitMemoryStore(arg, 2, SLJIT_MOV_U16); }
void OpcodeDispatcher::dispatchI64Load(WASM::MemArg arg)
{ emitMemoryLoad(arg, 8, SLJIT_MOV, 8); }
void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg arg)
{ emitMemoryLoad(arg, 1, SLJIT_MOV_S8, 8); }
void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg arg)
{ emitMemoryLoad(arg, 1, SLJIT_MOV_U8, 8); }
void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg arg)
{ emitMemoryLoad(arg, 2, SLJIT_MOV_S16, 8); }
void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg arg)
{ emitMemoryLoad(arg, 2, SLJIT_MOV_U16, 8); }
void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg arg)
{ emitMemoryLoad(arg, 4, SLJIT_MOV_S32, 8); }
void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg arg)
{ emitMemoryLoad(arg, 4, SLJIT_MOV_U32, 8); }
void OpcodeDispatcher::dispatchI64Store(WASM::MemArg arg)
{ emitMemoryStore(arg, 8, SLJIT_MOV); }
void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg arg)
{ emitMemoryStore(arg, 1, SLJIT_MOV_U8); }
void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg arg)
{ emitMemoryStore(arg, 2, SLJIT_MOV_U16); }
void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg arg)
{ emitMemoryStore(arg, 4, SLJIT_MOV32); }

void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg)
{
	if (!reachable_)
		return;
	const bool is64 = requireMemoryIndex(arg).indexType == WASM::MemoryIndexType::I64;
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		if (is64)
			emitHelper(SLJIT_ARGS2(W, P, 32), &sljitMemorySize64);
		else
			emitHelper(SLJIT_ARGS2(32, P, 32), &sljitMemorySize);
		storeSlot(SLJIT_R0, stackWidths_.size(), is64 ? 8 : 4);
		emitPendingTrapCheck();
	}
	push(is64 ? 8 : 4);
}

void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	if (!reachable_)
		return;
	const bool is64 = requireMemoryIndex(arg).indexType == WASM::MemoryIndexType::I64;
	const int width = is64 ? 8 : 4;
	requireStack(1);
	pop();
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, stackWidths_.size(), width);
		if (is64)
			emitHelper(SLJIT_ARGS3(W, P, 32, W), &sljitMemoryGrow64);
		else
			emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitMemoryGrow);
		storeSlot(SLJIT_R0, stackWidths_.size(), width);
		emitPendingTrapCheck();
	}
	push(width);
}

void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2)
{
	if (!reachable_)
		return;
	const auto destinationType = requireMemoryIndex(arg1).indexType;
	const auto sourceType = requireMemoryIndex(arg2).indexType;
	require(destinationType == sourceType,
			"memory.copy memories must have the same index type");
	requireStack(3);
	const std::size_t base = stackWidths_.size() - 3;
	if (!measureOnly_) {
		if (destinationType == WASM::MemoryIndexType::I32) {
			loadContext(SLJIT_R0);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
					static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
							| static_cast<std::uint64_t>(arg1))));
			loadSlot(SLJIT_R2, base, 4);
			loadSlot(SLJIT_R3, base + 1, 4);
			emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0,
					SLJIT_IMM, 32));
			emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0));
			loadSlot(SLJIT_R3, base + 2, 4);
			emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitMemoryCopy);
		} else {
			// Three arguments: the helper reads destination/source/length from the
			// three operand slots (16 bytes apart) instead of a fourth register.
			slotAddress(SLJIT_R2, base);
			loadContext(SLJIT_R0);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
					static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
							| static_cast<std::uint64_t>(arg1))));
			emitHelper(SLJIT_ARGS3V(P, W, W), &sljitMemoryCopy64);
		}
		emitPendingTrapCheck();
	}
	stackWidths_.resize(base);
}

void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg)
{
	if (!reachable_)
		return;
	const bool is64 = requireMemoryIndex(arg).indexType == WASM::MemoryIndexType::I64;
	requireStack(3);
	const std::size_t base = stackWidths_.size() - 3;
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		loadSlot(SLJIT_R1, base + 1, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0,
				SLJIT_IMM, 32));
		emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0,
				SLJIT_IMM, static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, base, is64 ? 8 : 4);
		loadSlot(SLJIT_R3, base + 2, is64 ? 8 : 4);
		if (is64)
			emitHelper(SLJIT_ARGS4V(P, W, W, W), &sljitMemoryFill64);
		else
			emitHelper(SLJIT_ARGS4V(P, W, 32, 32), &sljitMemoryFill);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(base);
}

void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2)
{
	if (!reachable_)
		return;
	const bool is64 = requireMemoryIndex(arg2).indexType == WASM::MemoryIndexType::I64;
	require(arg1 < module_.dataSegments.size(), "the data segment index is out of range");
	requireStack(3);
	const std::size_t base = stackWidths_.size() - 3;
	if (!measureOnly_) {
		if (is64) {
			slotAddress(SLJIT_R2, base);
			loadContext(SLJIT_R0);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
					static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
							| static_cast<std::uint64_t>(arg1))));
			emitHelper(SLJIT_ARGS3V(P, W, W), &sljitMemoryInit64);
		} else {
			loadContext(SLJIT_R0);
			emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
					static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
							| static_cast<std::uint64_t>(arg1))));
			loadSlot(SLJIT_R2, base, 4);
			loadSlot(SLJIT_R3, base + 1, 4);
			emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0,
					SLJIT_IMM, 32));
			emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0));
			loadSlot(SLJIT_R3, base + 2, 4);
			emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitMemoryInit);
		}
		emitPendingTrapCheck();
	}
	stackWidths_.resize(base);
}

void OpcodeDispatcher::dispatchDataDrop(uint32_t arg)
{
	if (!reachable_)
		return;
	require(arg < module_.dataSegments.size(), "the data segment index is out of range");
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS2V(P, 32), &sljitDataDrop);
		emitPendingTrapCheck();
	}
}

void OpcodeDispatcher::requireTableIndex(WASM::TableIdx index)
{
	require(index < module_.importTables.size() + module_.tables.size(),
			"the table index is out of range");
}

void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2)
{
	if (!reachable_)
		return;
	requireTableIndex(arg2);
	require(arg1 < module_.elementSegments.size(),
			"the element segment index is out of range");
	requireStack(3);
	if (!measureOnly_) {
		const std::size_t base = stackWidths_.size() - 3;
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
						| static_cast<std::uint64_t>(arg1))));
		loadSlot(SLJIT_R2, base, 4);
		loadSlot(SLJIT_R3, base + 1, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0,
				SLJIT_IMM, 32));
		emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0));
		loadSlot(SLJIT_R3, base + 2, 4);
		emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitTableInit);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 3);
}

void OpcodeDispatcher::dispatchElemDrop(uint32_t arg)
{
	if (!reachable_)
		return;
	require(arg < module_.elementSegments.size(),
			"the element segment index is out of range");
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS2V(P, 32), &sljitElemDrop);
		emitPendingTrapCheck();
	}
}

void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2)
{
	if (!reachable_)
		return;
	requireTableIndex(arg1);
	requireTableIndex(arg2);
	requireStack(3);
	if (!measureOnly_) {
		const std::size_t base = stackWidths_.size() - 3;
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(arg2) << 32)
						| static_cast<std::uint64_t>(arg1))));
		loadSlot(SLJIT_R2, base, 4);
		loadSlot(SLJIT_R3, base + 1, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0,
				SLJIT_IMM, 32));
		emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0));
		loadSlot(SLJIT_R3, base + 2, 4);
		emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitTableCopy);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 3);
}

void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg)
{
	if (!reachable_)
		return;
	requireTableIndex(arg);
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS2(32, P, 32), &sljitTableSize);
		storeSlot(SLJIT_R0, stackWidths_.size(), 4);
		emitPendingTrapCheck();
	}
	push(4);
}

} // namespace Sljit

















