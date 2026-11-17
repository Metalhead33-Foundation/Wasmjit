#include "AsmjitModuleCompiler.hpp"
#include "AsmjitEmitter.hpp"
#include <cstdlib>
#include <string_view>

#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmSafety.hpp"
#include "../WasmBase/WasmBackendAbi.hpp"
#include "../WasmBase/WasmDeterminism.hpp"
#include "../WasmBase/WasmExn.hpp"
#include "../WasmBase/wasm_atomic.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmStringOps.hpp"
#include "../WasmBase/WasmRoundingOps.hpp"
#include "../WasmBase/WasmContinuation.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmTracing.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"
#include "../WasmBase/wasm_f16.hpp"
#include "../WasmBase/WasmGcHeader.hpp"
#include "../WasmBase/WasmType.hpp"
#include "../WasmBase/WasmTailTrampoline.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AsmJitBackend {

namespace {


void checkError(asmjit::Error error, const char* operation)
{
	if (error != asmjit::Error::kOk)
		throw std::runtime_error(std::string(operation) + ": "
				+ std::to_string(static_cast<unsigned>(error)));
}

void requireInteger(const WASM::ValueType& type, const char* role)
{
	if (type.opcode != WASM::ValueTypeCode::I32
			&& type.opcode != WASM::ValueTypeCode::I64)
		throw std::runtime_error(std::string("AsmJit compiler supports only i32/i64 ")
				+ role);
}

//! True for the two scalar floating-point value types.
bool isFloatKind(WASM::ValueTypeCode kind) noexcept
{
	return kind == WASM::ValueTypeCode::F32 || kind == WASM::ValueTypeCode::F64;
}

//! Slot width of one declared scalar value kind, or 0 when it is outside the
//! scalar subset (references, v128).
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
	case WASM::ValueTypeCode::StringRef:
	case WASM::ValueTypeCode::StringViewWtf8:
	case WASM::ValueTypeCode::StringViewWtf16:
	case WASM::ValueTypeCode::StringViewIter:
		return true;
	default:
		return false;
	}
}

int kindWidth(WASM::ValueTypeCode kind) noexcept
{
	switch (kind) {
	case WASM::ValueTypeCode::I32:
	case WASM::ValueTypeCode::F32:
		return 4;
	case WASM::ValueTypeCode::I64:
	case WASM::ValueTypeCode::F64:
		return 8;
	default:
		// A reference is one machine word in the value model.
		return isReferenceKind(kind) ? 8 : 0;
	}
}

//! Frame/lane width of a local: a v128 occupies a whole 16-byte buffer.
int slotWidthOf(WASM::ValueTypeCode kind) noexcept
{
	return kind == WASM::ValueTypeCode::V128 ? 16 : kindWidth(kind);
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
//! Width of a shared-atomic numeric slot, or 0 for a type this backend refuses.
uint32_t sharedAtomicNumericWidth(WASM::ValueTypeCode code) noexcept
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

//! Refuses a parameter/local/result outside the scalar subset.
void requireScalar(const WASM::ValueType& type, const char* role)
{
	if (kindWidth(type.opcode) == 0)
		throw std::runtime_error(std::string("AsmJit supports only i32/i64/f32/f64 ")
				+ role);
}

void requireI32(const WASM::ValueType& type, const char* role)
{
	if (type.opcode != WASM::ValueTypeCode::I32)
		throw std::runtime_error(std::string("AsmJit currently supports only i32 ") + role);
}

// ── Scalar floating-point helpers (shared semantics with the SLJIT backend) ──
// The rules are the ones the LibJIT backend defines for this engine: min/max
// return a quiet NaN when either operand is NaN and are sign-of-zero aware;
// trunc/nearest quiet a NaN; nearest is ties-to-even.

extern "C" float asmjitF32Min(float a, float b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) || std::signbit(b)) ? -0.0f : 0.0f;
	return a < b ? a : b;
}

extern "C" float asmjitF32Max(float a, float b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<float>::quiet_NaN();
	if (a == 0.0f && b == 0.0f)
		return (std::signbit(a) && std::signbit(b)) ? -0.0f : 0.0f;
	return a > b ? a : b;
}

extern "C" double asmjitF64Min(double a, double b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) || std::signbit(b)) ? -0.0 : 0.0;
	return a < b ? a : b;
}

extern "C" double asmjitF64Max(double a, double b) noexcept
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<double>::quiet_NaN();
	if (a == 0.0 && b == 0.0)
		return (std::signbit(a) && std::signbit(b)) ? -0.0 : 0.0;
	return a > b ? a : b;
}

extern "C" float asmjitF32Sqrt(float x) noexcept { return std::sqrt(x); }
//! `ceil` and `floor` must return an *arithmetic* NaN for a signalling input:
//! the host libm may preserve the sNaN bit pattern, but the spec requires the
//! quiet bit. Returning a quiet NaN explicitly also pins the result across
//! hosts and rounding modes (docs/proposals/PROFILES.md PR-3).
extern "C" float asmjitF32Ceil(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::ceil(x);
}

extern "C" float asmjitF32Floor(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::floor(x);
}

extern "C" float asmjitF32Trunc(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::trunc(x);
}

extern "C" float asmjitF32Nearest(float x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<float>::quiet_NaN();
	return std::nearbyint(x);
}

extern "C" double asmjitF64Sqrt(double x) noexcept { return std::sqrt(x); }
extern "C" double asmjitF64Ceil(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::ceil(x);
}

extern "C" double asmjitF64Floor(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::floor(x);
}

extern "C" double asmjitF64Trunc(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::trunc(x);
}

extern "C" double asmjitF64Nearest(double x) noexcept
{
	if (std::isnan(x))
		return std::numeric_limits<double>::quiet_NaN();
	return std::nearbyint(x);
}

//! Canonicalise an f32 NaN by bit pattern, preserving every non-NaN value
//! exactly (docs/proposals/PROFILES.md PR-3).
extern "C" uint32_t asmjitCanonicalF32Bits(uint32_t bits) noexcept
{
	const uint32_t magnitude = bits & 0x7fffffffu;
	if ((magnitude & 0x7f800000u) == 0x7f800000u && (magnitude & 0x007fffffu) != 0)
		return WASM::kCanonicalF32Bits;
	return bits;
}

//! The f64 counterpart of `asmjitCanonicalF32Bits`.
extern "C" uint64_t asmjitCanonicalF64Bits(uint64_t bits) noexcept
{
	const uint64_t magnitude = bits & 0x7fffffffffffffffull;
	if ((magnitude & 0x7ff0000000000000ull) == 0x7ff0000000000000ull
			&& (magnitude & 0x000fffffffffffffull) != 0)
		return WASM::kCanonicalF64Bits;
	return bits;
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

extern "C" int32_t asmjitI32TruncF32S(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -2147483649.0 && d < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}

extern "C" uint32_t asmjitI32TruncF32U(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}

extern "C" int32_t asmjitI32TruncF64S(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -2147483649.0 && x < 2147483648.0))
		trapIntegerOverflow();
	return static_cast<int32_t>(x);
}

extern "C" uint32_t asmjitI32TruncF64U(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 4294967296.0))
		trapIntegerOverflow();
	return static_cast<uint32_t>(x);
}

extern "C" int64_t asmjitI64TruncF32S(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}

extern "C" uint64_t asmjitI64TruncF32U(float x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	const double d = x;
	if (!(d > -1.0 && d < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}

extern "C" int64_t asmjitI64TruncF64S(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
		trapIntegerOverflow();
	return static_cast<int64_t>(x);
}

extern "C" uint64_t asmjitI64TruncF64U(double x) noexcept
{
	if (std::isnan(x)) { trapInvalidConversion(); return 0; }
	if (!(x > -1.0 && x < 18446744073709551616.0))
		trapIntegerOverflow();
	return static_cast<uint64_t>(x);
}

// ── Saturating truncation (non-trapping proposal) ──────────────────────────
// These clamp instead of reporting; NaN maps to zero.

extern "C" int32_t asmjitI32TruncSatF32S(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 2147483648.0) return INT32_MAX;
	if (d < -2147483648.0) return INT32_MIN;
	return static_cast<int32_t>(x);
}

extern "C" uint32_t asmjitI32TruncSatF32U(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 4294967296.0) return UINT32_MAX;
	if (d <= -1.0) return 0;
	return static_cast<uint32_t>(x);
}

extern "C" int32_t asmjitI32TruncSatF64S(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 2147483648.0) return INT32_MAX;
	if (x < -2147483648.0) return INT32_MIN;
	return static_cast<int32_t>(x);
}

extern "C" uint32_t asmjitI32TruncSatF64U(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 4294967296.0) return UINT32_MAX;
	if (x <= -1.0) return 0;
	return static_cast<uint32_t>(x);
}

extern "C" int64_t asmjitI64TruncSatF32S(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 9223372036854775808.0) return INT64_MAX;
	if (d < -9223372036854775808.0) return INT64_MIN;
	return static_cast<int64_t>(x);
}

extern "C" uint64_t asmjitI64TruncSatF32U(float x) noexcept
{
	if (std::isnan(x)) return 0;
	const double d = x;
	if (d >= 18446744073709551616.0) return UINT64_MAX;
	if (d <= -1.0) return 0;
	return static_cast<uint64_t>(x);
}

extern "C" int64_t asmjitI64TruncSatF64S(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 9223372036854775808.0) return INT64_MAX;
	if (x < -9223372036854775808.0) return INT64_MIN;
	return static_cast<int64_t>(x);
}

extern "C" uint64_t asmjitI64TruncSatF64U(double x) noexcept
{
	if (std::isnan(x)) return 0;
	if (x >= 18446744073709551616.0) return UINT64_MAX;
	if (x <= -1.0) return 0;
	return static_cast<uint64_t>(x);
}

// ── Integer-to-float and float-to-float conversions ────────────────────────
// These are C casts rather than SSE conversions so both partial backends share
// one rounding rule (the host's round-to-nearest-even, which is what Wasm
// requires) and unsigned 64-bit inputs need no target-specific sequence.

extern "C" float asmjitF32FromI32S(int32_t x) noexcept { return static_cast<float>(x); }
extern "C" float asmjitF32FromI32U(uint32_t x) noexcept { return static_cast<float>(x); }
extern "C" float asmjitF32FromI64S(int64_t x) noexcept { return static_cast<float>(x); }
extern "C" float asmjitF32FromI64U(uint64_t x) noexcept { return static_cast<float>(x); }
extern "C" double asmjitF64FromI32S(int32_t x) noexcept { return static_cast<double>(x); }
extern "C" double asmjitF64FromI32U(uint32_t x) noexcept { return static_cast<double>(x); }
extern "C" double asmjitF64FromI64S(int64_t x) noexcept { return static_cast<double>(x); }
extern "C" double asmjitF64FromI64U(uint64_t x) noexcept { return static_cast<double>(x); }
extern "C" float asmjitF32DemoteF64(double x) noexcept { return static_cast<float>(x); }
extern "C" double asmjitF64PromoteF32(float x) noexcept { return static_cast<double>(x); }


asmjit::x86::Gp newIntegerRegister(asmjit::x86::Compiler& compiler,
		const WASM::ValueType& type, const char* name)
{
	// A float value travels as its bit pattern in a Gp (4 bytes for f32, 8 for
	// f64); materialising it in an Xmm happens at the instruction or ABI
	// boundary. The width rule is therefore the same for both classes.
	return kindWidth(type.opcode) == 8
			? compiler.new_gp64(name) : compiler.new_gp32(name);
}

int64_t asmjitI64DivS(int64_t lhs, int64_t rhs) noexcept
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

uint64_t asmjitI64DivU(uint64_t lhs, uint64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs / rhs;
}

int64_t asmjitI64RemS(int64_t lhs, int64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT64_MIN && rhs == -1)
		return 0;
	return lhs % rhs;
}

uint64_t asmjitI64RemU(uint64_t lhs, uint64_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs % rhs;
}

uint32_t asmjitI32Popcnt(uint32_t value) noexcept
{
	value -= (value >> 1) & 0x55555555u;
	value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
	value = (value + (value >> 4)) & 0x0F0F0F0Fu;
	value += value >> 8;
	value += value >> 16;
	return value & 0x3Fu;
}

uint64_t asmjitI64Popcnt(uint64_t value) noexcept
{
	return static_cast<uint64_t>(__builtin_popcountll(value));
}

int32_t asmjitI32DivS(int32_t lhs, int32_t rhs) noexcept
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

uint32_t asmjitI32DivU(uint32_t lhs, uint32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs / rhs;
}

int32_t asmjitI32RemS(int32_t lhs, int32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	if (lhs == INT32_MIN && rhs == -1)
		return 0;
	return lhs % rhs;
}

uint32_t asmjitI32RemU(uint32_t lhs, uint32_t rhs) noexcept
{
	if (rhs == 0) {
		WASM::reportTrap(WASM::TrapKind::IntegerDivideByZero);
		return 0;
	}
	return lhs % rhs;
}

WASM::Callable* asmjitResolveIndirectCallable(WASM::VMContext* vm, uint32_t tableIndex,
		uint32_t elementIndex, uint32_t expectedTypeValue) noexcept
{
	if (vm == nullptr || vm->tables == nullptr || tableIndex >= vm->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit call_indirect table index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = vm->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit call_indirect table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		// `call_indirect` reports an out-of-range index as an undefined element;
		// a null slot is an uninitialized element. `table.get`/`table.set` use
		// the ordinary OOB table-access trap.
		WASM::reportTrap(WASM::TrapKind::UndefinedElement);
		return nullptr;
	}
	if (table->base == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit call_indirect table storage is null");
		return nullptr;
	}
	WASM::Callable* callable = table->base[elementIndex];
	if (callable == nullptr) {
		WASM::reportTrap(WASM::TrapKind::UninitializedElement);
		return nullptr;
	}
	if (callable->fnPtr == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit call_indirect target has no entry point");
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
			WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit call_indirect type check failed");
			return nullptr;
		}
	}
	return callable;
}

void asmjitReportNullI31() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullI31Reference);
}

void asmjitReportCastFailure() noexcept
{
	WASM::reportTrap(WASM::TrapKind::CastFailure);
}

void asmjitReportNullStructure() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullStructureReference);
}

void asmjitReportNullArray() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullArrayReference);
}

void asmjitReportArrayOob() noexcept
{
	WASM::reportTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
}

void* asmjitArrayNewData(WASM::VMContext* vm, uint32_t typeIndex, uint32_t dataIdx,
		uint64_t packedSrcAndLength) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewData(typeIndex,
				dataIdx, static_cast<uint32_t>(packedSrcAndLength >> 32),
				static_cast<uint32_t>(packedSrcAndLength & 0xFFFFFFFFu));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitArrayNewArray(WASM::VMContext* vm, void* source,
		uint64_t packedTypes, uint64_t packedRange) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewArray(
				static_cast<uint32_t>(packedTypes & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedTypes >> 32), source,
				static_cast<uint32_t>(packedRange & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedRange >> 32));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitArrayNewMemory(WASM::VMContext* vm, uint64_t packedTypes,
		uint64_t packedRange) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewMemory(
				static_cast<uint32_t>(packedTypes & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedTypes >> 32),
				static_cast<uint32_t>(packedRange & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedRange >> 32));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitArrayNewTable(WASM::VMContext* vm, uint64_t packedTypes,
		uint64_t packedRange) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewTable(
				static_cast<uint32_t>(packedTypes & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedTypes >> 32),
				static_cast<uint32_t>(packedRange & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedRange >> 32));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitArrayNewElem(WASM::VMContext* vm, uint32_t typeIndex, uint32_t elemIdx,
		uint64_t packedSrcAndLength) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayNewElem(typeIndex,
				elemIdx, static_cast<uint32_t>(packedSrcAndLength >> 32),
				static_cast<uint32_t>(packedSrcAndLength & 0xFFFFFFFFu));
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitArrayNew(WASM::VMContext* vm, uint32_t typeIndex, uint32_t length) noexcept
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

uint32_t asmjitTypeIsSubtype(WASM::VMContext* vm, uint32_t actual, uint32_t expected) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->typeIdIsSubtype(actual, expected) ? 1u : 0u;
}

uint32_t asmjitTypeIdMatchesKind(WASM::VMContext* vm, uint32_t typeId, uint32_t kind) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->typeIdMatchesAbstractKind(typeId, kind) ? 1u : 0u;
}

int32_t asmjitRefMatchesHeapType(WASM::VMContext* vm, void* ref,
		uint64_t packedHeapAndType, uint32_t nullable) noexcept
{
	WASM::HeapType heap;
	const uint32_t isTypeIndex = static_cast<uint32_t>(packedHeapAndType >> 32);
	const uint32_t value = static_cast<uint32_t>(packedHeapAndType & 0xFFFFFFFFu);
	heap.isTypeIndex = isTypeIndex != 0;
	if (heap.isTypeIndex)
		heap.typeIndex = value;
	else
		heap.abstract = static_cast<WASM::AbstractHeapType>(static_cast<int32_t>(value));
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->refMatchesHeapType(
				ref, heap, nullable != 0) ? 1 : 0;
	} catch (...) {
		return 0;
	}
}

void* asmjitExternBox(WASM::VMContext* vm, void* ref) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->externBox(ref);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void* asmjitExternUnbox(WASM::VMContext* vm, void* ref) noexcept
{
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->externUnbox(ref);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void asmjitArrayInitFromData(WASM::VMContext* vm, void* array, uint32_t typeIndex,
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


void asmjitArrayInitFromElem(WASM::VMContext* vm, void* array, uint32_t typeIndex,
		uint64_t packedOffsetAndIndex) noexcept
{
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayInitFromElem(array, typeIndex,
				static_cast<uint32_t>(packedOffsetAndIndex >> 32),
				packedOffsetAndIndex & 0xFFFFFFFFu);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

void asmjitCheckElementRange(WASM::VMContext* vm, uint32_t elemIdx,
		uint32_t srcIndex, uint32_t len) noexcept
{
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->checkElementSegmentRange(
				elemIdx, srcIndex, len);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

void asmjitCheckDataRange(WASM::VMContext* vm, uint32_t dataIdx,
		uint32_t srcOffset, uint32_t len, uint32_t elemSize) noexcept
{
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->checkDataSegmentRange(
				dataIdx, srcOffset, len, elemSize);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

void asmjitArrayInitDataRange(WASM::VMContext* vm, void* array,
		uint64_t packedTypeAndData, uint64_t packedDstAndSrc, uint32_t len) noexcept
{
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayInitDataRange(array,
				static_cast<uint32_t>(packedTypeAndData >> 32),
				static_cast<uint32_t>(packedTypeAndData & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedDstAndSrc >> 32),
				static_cast<uint32_t>(packedDstAndSrc & 0xFFFFFFFFu), len);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

void asmjitArrayInitElemRange(WASM::VMContext* vm, void* array,
		uint64_t packedTypeAndElem, uint64_t packedDstAndSrc, uint32_t len) noexcept
{
	try {
		reinterpret_cast<WASM::ModuleInstance*>(vm)->arrayInitElemRange(array,
				static_cast<uint32_t>(packedTypeAndElem >> 32),
				static_cast<uint32_t>(packedTypeAndElem & 0xFFFFFFFFu),
				static_cast<uint32_t>(packedDstAndSrc >> 32),
				static_cast<uint32_t>(packedDstAndSrc & 0xFFFFFFFFu), len);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
	}
}

void* asmjitStructNew(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex) noexcept
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

void* asmjitStructNewDesc(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex,
		void* descriptor) noexcept
{
	// Custom Descriptors: the heap retains the descriptor into the new object's
	// header slot. The operand-stack value is a borrow in this backend's
	// reference model, so no drop follows the call.
	try {
		return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(
				size, typeIndex, descriptor);
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal);
		return nullptr;
	}
}

void asmjitReportNullReference() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullReference);
}

void* asmjitRefGetDesc(WASM::VMContext* vm, void* ref) noexcept
{
	// `ref.get_desc` has two distinct traps: a null structure reference and a
	// reference whose header carries no descriptor (Custom Descriptors).
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

int32_t asmjitDescEq(WASM::VMContext* vm, void* ref, void* descriptor,
		int32_t allowNull) noexcept
{
	// One predicate serves `ref.cast_desc_eq`, its `_null` form and both
	// `br_on_cast_desc_eq` directions.
	WASM::ModuleInstance* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	if (descriptor == nullptr) {
		WASM::reportTrap(WASM::TrapKind::NullReference,
				"cast_desc_eq: null custom descriptor");
		return 0;
	}
	if (ref == nullptr) {
		if (allowNull != 0)
			return 1;
		WASM::reportTrap(WASM::TrapKind::NullStructureReference,
				"cast_desc_eq: null structure reference");
		return 0;
	}
	return instance->structDescriptor(ref) == descriptor ? 1 : 0;
}

void asmjitReportNullFunctionReference() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullFunctionReference);
}

void asmjitReportInternal(const char* detail) noexcept
{
	WASM::reportTrap(WASM::TrapKind::Internal, detail);
}

uint32_t asmjitMemorySize(WASM::VMContext* vm, uint32_t memoryIndex) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.size index is invalid");
		return 0;
	}
	const WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr || memory->pageSize == 0) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.size memory is invalid");
		return 0;
	}
	return static_cast<uint32_t>(WASM::wasm_memory_size(memory) / memory->pageSize);
}

uint32_t asmjitMemoryGrow(WASM::VMContext* vm, uint32_t memoryIndex, uint32_t delta) noexcept
{
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.grow VMContext is null");
		return UINT32_MAX;
	}
	if (vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.grow index is invalid");
		return UINT32_MAX;
	}
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	try {
		const int64_t previous = instance->growMemory(memoryIndex, delta);
		return previous < 0 ? UINT32_MAX : static_cast<uint32_t>(previous);
	} catch (...) {
		// memory.grow is specified to return -1 when growth cannot be fulfilled.
		// Never allow a host allocation exception to unwind through JIT frames.
		return UINT32_MAX;
	}
}

uint64_t asmjitMemorySize64(WASM::VMContext* vm, uint32_t memoryIndex) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.size index is invalid");
		return 0;
	}
	const WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr || memory->pageSize == 0) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.size memory is invalid");
		return 0;
	}
	return WASM::wasm_memory_size(memory) / memory->pageSize;
}

uint64_t asmjitMemoryGrow64(WASM::VMContext* vm, uint32_t memoryIndex,
		uint64_t delta) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.grow index is invalid");
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

void* asmjitMemoryAddress64(WASM::VMContext* vm, uint32_t memoryIndex,
		uint64_t address, uint64_t offset, uint32_t width) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64 index is invalid");
		return nullptr;
	}
	WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64 memory is null");
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
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64 base is null");
		return nullptr;
	}
	return memory->memoryBase + effective;
}

void* asmjitMemoryAddress(WASM::VMContext* vm, uint32_t memoryIndex,
		uint32_t address, uint32_t offset, uint32_t width) noexcept
{
	if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory index is invalid");
		return nullptr;
	}
	WASM::LinearMemory* memory = vm->memories[memoryIndex];
	if (memory == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit linear memory is null");
		return nullptr;
	}
	const uint64_t effective = static_cast<uint64_t>(address) + offset;
	const uint64_t size = WASM::wasm_memory_size(memory);
	if (effective > size || width > size - effective) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
		return nullptr;
	}
	if (memory->memoryBase == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit linear-memory base is null");
		return nullptr;
	}
	return memory->memoryBase + effective;
}


void asmjitMemoryCopy(WASM::VMContext* vm, uint32_t destinationMemory,
        uint32_t sourceMemory, uint32_t destination, uint32_t source,
        uint32_t length) noexcept
{
    if (vm == nullptr || vm->memories == nullptr
            || destinationMemory >= vm->memoryCount || sourceMemory >= vm->memoryCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.copy index is invalid");
        return;
    }
    WASM::LinearMemory* dst = vm->memories[destinationMemory];
    WASM::LinearMemory* src = vm->memories[sourceMemory];
    if (dst == nullptr || src == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.copy memory is null");
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
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.copy base is null");
        return;
    }
    std::memmove(dst->memoryBase + destination, src->memoryBase + source, length);
}

void asmjitMemoryFill(WASM::VMContext* vm, uint32_t memoryIndex,
        uint32_t destination, uint32_t value, uint32_t length) noexcept
{
    if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.fill index is invalid");
        return;
    }
    WASM::LinearMemory* memory = vm->memories[memoryIndex];
    if (memory == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.fill memory is null");
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
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.fill base is null");
        return;
    }
    std::memset(memory->memoryBase + destination, static_cast<unsigned char>(value), length);
}


void asmjitMemoryInit(WASM::VMContext* vm, uint32_t dataIndex, uint32_t memoryIndex,
        uint32_t destination, uint32_t source, uint32_t length) noexcept
{
    if (vm == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.init VMContext is null");
        return;
    }
    try {
        reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit(
                memoryIndex, dataIndex, destination, source, length);
    } catch (...) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory.init helper failed");
    }
}

//! 64-bit-offset memory.copy for a memory64 destination/source.
void asmjitMemoryCopy64(WASM::VMContext* vm, uint32_t destinationMemory,
        uint32_t sourceMemory, uint64_t destination, uint64_t source,
        uint64_t length) noexcept
{
    if (vm == nullptr || vm->memories == nullptr
            || destinationMemory >= vm->memoryCount || sourceMemory >= vm->memoryCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.copy index is invalid");
        return;
    }
    WASM::LinearMemory* dst = vm->memories[destinationMemory];
    WASM::LinearMemory* src = vm->memories[sourceMemory];
    if (dst == nullptr || src == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.copy memory is null");
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
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.copy base is null");
        return;
    }
    std::memmove(dst->memoryBase + destination, src->memoryBase + source,
            static_cast<size_t>(length));
}

//! 64-bit-offset memory.fill for a memory64 destination.
void asmjitMemoryFill64(WASM::VMContext* vm, uint32_t memoryIndex,
        uint64_t destination, uint32_t value, uint64_t length) noexcept
{
    if (vm == nullptr || vm->memories == nullptr || memoryIndex >= vm->memoryCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.fill index is invalid");
        return;
    }
    WASM::LinearMemory* memory = vm->memories[memoryIndex];
    if (memory == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.fill memory is null");
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
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.fill base is null");
        return;
    }
    std::memset(memory->memoryBase + destination, static_cast<unsigned char>(value),
            static_cast<size_t>(length));
}

//! 64-bit-offset memory.init for a memory64 destination.
void asmjitMemoryInit64(WASM::VMContext* vm, uint32_t dataIndex, uint32_t memoryIndex,
        uint64_t destination, uint32_t source, uint32_t length) noexcept
{
    if (vm == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.init VMContext is null");
        return;
    }
    try {
        reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit64(
                memoryIndex, dataIndex, destination, source, length);
    } catch (...) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit memory64.init helper failed");
    }
}

void asmjitDataDrop(WASM::VMContext* vm, uint32_t dataIndex) noexcept
{
    if (vm == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit data.drop VMContext is null");
        return;
    }
    try {
        reinterpret_cast<WASM::ModuleInstance*>(vm)->dataDrop(dataIndex);
    } catch (...) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit data.drop helper failed");
    }
}


void asmjitTableInit(WASM::VMContext* vm, uint32_t elementIndex, uint32_t tableIndex,
        uint32_t destination, uint32_t source, uint32_t length) noexcept
{
    if (vm == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.init VMContext is null");
        return;
    }
    try {
        reinterpret_cast<WASM::ModuleInstance*>(vm)->tableInit(
                elementIndex, tableIndex, destination, source, length);
    } catch (...) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.init helper failed");
    }
}

void asmjitElemDrop(WASM::VMContext* vm, uint32_t elementIndex) noexcept
{
    if (vm == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit elem.drop VMContext is null");
        return;
    }
    try {
        reinterpret_cast<WASM::ModuleInstance*>(vm)->elemDrop(elementIndex);
    } catch (...) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit elem.drop helper failed");
    }
}



void asmjitTableCopy(WASM::VMContext* vm, uint32_t destinationTable, uint32_t sourceTable,
        uint32_t destination, uint32_t source, uint32_t length) noexcept
{
    if (vm == nullptr || vm->tables == nullptr
            || destinationTable >= vm->tableCount || sourceTable >= vm->tableCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.copy index is invalid");
        return;
    }
    WASM::TableInstance* dst = vm->tables[destinationTable];
    WASM::TableInstance* src = vm->tables[sourceTable];
    if (dst == nullptr || src == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.copy table is null");
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
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.copy storage is null");
        return;
    }
    std::memmove(dst->base + destination, src->base + source,
            static_cast<size_t>(length) * sizeof(WASM::Callable*));
}

uint64_t asmjitTableSize(WASM::VMContext* vm, uint32_t tableIndex) noexcept
{
    if (vm == nullptr || vm->tables == nullptr || tableIndex >= vm->tableCount) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.size index is invalid");
        return 0;
    }
    const WASM::TableInstance* table = vm->tables[tableIndex];
    if (table == nullptr) {
        WASM::reportTrap(WASM::TrapKind::Internal, "AsmJit table.size table is null");
        return 0;
    }
    return table->size;
}

//! 64x64 -> 128 multiplication in portable 32-bit limbs (NUM-1).
void asmjitMulWideU64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi)
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

void asmjitMulWideS64(int64_t a, int64_t b, uint64_t& lo, uint64_t& hi)
{
    asmjitMulWideU64(static_cast<uint64_t>(a), static_cast<uint64_t>(b), lo, hi);
    if (a < 0)
        hi -= static_cast<uint64_t>(b);
    if (b < 0)
        hi -= static_cast<uint64_t>(a);
}

extern "C" void asmjitI64MulWideU(uint64_t* out, uint64_t a, uint64_t b) noexcept
{
    asmjitMulWideU64(a, b, out[0], out[1]);
}

extern "C" void asmjitI64MulWideS(uint64_t* out, int64_t a, int64_t b) noexcept
{
    asmjitMulWideS64(a, b, out[0], out[1]);
}

asmjit::FuncSignature stackCheckSignature()
{
	asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
	signature.set_ret(asmjit::TypeId::kVoid);
	signature.add_arg(asmjit::TypeId::kUIntPtr);
	return signature;
}



class IntegerFunctionEmitter final : public WASM::OpcodeDispatcher
{
private:
	//! A structured-control frame, defined with the other private state below.
	struct ControlFrame;

public:
	IntegerFunctionEmitter(asmjit::x86::Compiler& compiler, asmjit::ujit::UniCompiler& universal,
			asmjit::FuncNode& function,
			const WASM::FuncType& type, const WASM::FunctionBody& body,
			const WASM::Module& module, WASM::ModuleInstanceInternals& internals,
			const std::vector<asmjit::FuncNode*>& functions,
			const std::vector<std::unique_ptr<asmjit::FuncSignature>>& signatures,
			uint32_t functionIndex, bool ownFrameForm,
			std::span<const std::uint8_t> frameByType, bool deterministicFloats,
			bool swapsGuestAccesses)
		: compiler(compiler), emitter(compiler, universal, swapsGuestAccesses), module(module),
		  internals(internals),
		  functions(functions), signatures(signatures), functionIndex(functionIndex),
		  frameMode(ownFrameForm), frameByType(frameByType),
		  deterministicFloats(deterministicFloats)
	{
		// The diagnostic names the function; interning keeps the pointer valid
		// for the instance's lifetime.
		if (const std::string* name = module.findName(WASM::NameKind::Function, functionIndex);
				name != nullptr)
			functionName = internals.internTrapMessage(*name);
		WASM::PreparedFunctionStack stack;
		stack.prepare(type, body);

		if (type.results.size() > 1 && !frameMode)
			throw std::runtime_error("AsmJit initial compiler does not support multi-value results");
		// A vector parameter is exactly what the frame form exists to carry, so
		// the entry refusal applies only to the register form.
		const bool frameAbi = frameMode;
		for (const auto& parameter : type.params) {
			if (!frameAbi)
				requireScalar(parameter.val, "parameters");
		}
		for (const auto& result : type.results) {
			// A v128 result is allowed: it travels through the caller-supplied buffer,
			// which the signature spells as a trailing pointer and a void return.
			if (result.val.opcode != WASM::ValueTypeCode::V128)
				requireScalar(result.val, "results");
		}
		for (const auto& local : stack.allLocals) {
			if (local.opcode != WASM::ValueTypeCode::V128)
				requireScalar(local, "locals");
		}
		resultType = type.results.empty()
				? WASM::ValueTypeCode::Void : type.results.front().val.opcode;
		// Only the register form needs the caller-supplied result buffer; in frame
		// mode the result block of the frame *is* that buffer (step 5e).
		bufferedVectorResult = resultType == WASM::ValueTypeCode::V128 && !frameAbi;
		if (bufferedVectorResult) {
			// The caller supplies a 16-byte buffer as a trailing pointer argument; keep
			// it in a stack slot so helper calls cannot clobber it.
			v128ResultSlot = compiler.new_stack(16, 16, "v128ResultBuffer");
			asmjit::x86::Gp bufferPointer = compiler.new_gp_ptr("v128ResultBufferIn");
			function.set_arg(static_cast<uint32_t>(stack.parameterCount) + 1,
					bufferPointer);
			emitter.mov(v128ResultSlot, bufferPointer);
		}
		exceptionPtrSlot = compiler.new_stack(8, 8, "pendingException");

		locals.reserve(stack.allLocals.size());
		vmContext = compiler.new_gp_ptr("vmContext");
		function.set_arg(0, vmContext);

		const asmjit::FuncSignature checkSignature = stackCheckSignature();
		asmjit::InvokeNode* checkCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(checkCall),
				reinterpret_cast<uint64_t>(&WASM::wasm_stack_check), checkSignature),
				"AsmJit failed to emit the guest stack guard call");
		checkCall->set_arg(0, vmContext);

		// wasm_stack_check reports into the current execution state's pending
		// slot and returns. Generated frames must observe that slot and return
		// before executing guest instructions; C++ exceptions cannot unwind
		// through this frame (docs/SAFETY.md §2.4).
		asmjit::FuncSignature trapSlotSignature(asmjit::CallConvId::kCDecl);
		trapSlotSignature.set_ret(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* trapSlotCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(trapSlotCall),
				reinterpret_cast<uint64_t>(&WASM::wasm_trap_slot), trapSlotSignature),
				"AsmJit failed to emit pending-trap slot lookup");
		asmjit::x86::Gp trapSlot = compiler.new_gp_ptr("trapSlot");
		trapSlotCall->set_ret(0, trapSlot);
		asmjit::x86::Gp pending = compiler.new_gp32("pendingTrap");
		emitter.mov(pending, asmjit::x86::dword_ptr(trapSlot,
				static_cast<int32_t>(offsetof(WASM::TrapSlot, pending))));
		asmjit::Label bodyLabel = compiler.new_label();
		emitter.j(bodyLabel, asmjit::ujit::test_z(pending, pending));
		if (frameMode || type.results.empty() || bufferedVectorResult) {
			// Frame mode belongs here too: the signature returns void because the
			// results travel in the frame, and on a trap they are undefined, so
			// there is nothing to store (step 5c).
			checkError(compiler.ret(), "AsmJit trap-path void return emission failed");
		} else if (isFloatKind(resultType)) {
			auto zero = compiler.new_xmm("trapFloatResult");
			emitter.vectorZero(zero);
			checkError(compiler.ret(zero), "AsmJit trap-path float return emission failed");
		} else {
			asmjit::x86::Gp zero = kindWidth(resultType) == 8
					? compiler.new_gp64("trapResult") : compiler.new_gp32("trapResult");
			emitter.xor_(zero, zero);
			checkError(compiler.ret(zero), "AsmJit trap-path result return emission failed");
		}
		checkError(compiler.bind(bodyLabel), "AsmJit body label binding failed");

		if (frameAbi) {
			// Staging-frame ABI (step 5b): the parameters are already in the
			// caller's frame, so each one is copied out of it instead of arriving
			// in a register. A `v128` is 16 bytes and gets a local buffer, the
			// same kind the register form allocates for a vector local.
			framePointer = compiler.new_gp_ptr("frame");
			function.set_arg(1, framePointer);
			uint32_t offset = 0;
			for (uint32_t i = 0; i < stack.parameterCount; ++i) {
				const WASM::ValueType& declared = stack.allLocals[i];
				if (declared.opcode == WASM::ValueTypeCode::V128) {
					asmjit::x86::Gp buffer = newVectorBuffer("v128Param");
					asmjit::x86::Gp source = compiler.new_gp_ptr("paramAddress");
					emitter.lea(source, asmjit::x86::Mem(framePointer, offset));
					copyVector(buffer, source);
					locals.push_back(buffer);
					offset += 16;
					continue;
				}
				// Scalars -- integers and float bit patterns alike -- load
				// straight out of the frame into their register.
				asmjit::x86::Gp parameter = newIntegerRegister(compiler, declared, "parameter");
				emitter.mov(parameter, asmjit::x86::Mem(framePointer, offset));
				locals.push_back(parameter);
				offset += 8;
			}
		} else
		for (uint32_t i = 0; i < stack.parameterCount; ++i) {
			const WASM::ValueType& declared = stack.allLocals[i];
			if (isFloatKind(declared.opcode)) {
				// The ABI argument is an Xmm; the body works on its bit pattern.
				auto parameter = compiler.new_xmm("parameter");
				function.set_arg(i + 1, parameter);
				const int width = kindWidth(declared.opcode);
				auto bits = width == 4
						? compiler.new_gp32("parameterBits")
						: compiler.new_gp64("parameterBits");
				if (width == 4)
					emitter.vectorToGp32(bits, parameter);
				else
					emitter.vectorToGp64(bits, parameter);
				locals.push_back(bits);
			} else {
				asmjit::x86::Gp parameter = newIntegerRegister(compiler, declared, "parameter");
				function.set_arg(i + 1, parameter);
				locals.push_back(parameter);
			}
		}
		for (uint32_t i = 0; i < stack.localCount; ++i) {
			const WASM::ValueTypeCode kind = stack.allLocals[stack.parameterCount + i].opcode;
			if (kind == WASM::ValueTypeCode::V128) {
				asmjit::x86::Gp local = newVectorBuffer("v128Local");
				zeroVector(local);
				locals.push_back(local);
			} else {
				asmjit::x86::Gp local = newIntegerRegister(compiler,
						stack.allLocals[stack.parameterCount + i], "local");
				emitter.xor_(local, local);
				locals.push_back(local);
			}
		}
		localWidths.reserve(stack.allLocals.size());
		localKinds.reserve(stack.allLocals.size());
		for (const auto& local : stack.allLocals) {
			localWidths.push_back(slotWidthOf(local.opcode));
			localKinds.push_back(local.opcode);
		}
		parameterCount = stack.parameterCount;
		// A reference parameter is the caller's borrow, so the slot takes its own
		// count now. This sits before the body label below, outside the loop a
		// direct self tail call jumps to: that path moves the argument counts
		// into the parameter slots itself (they travel in the operand stack, so
		// their counts cross over rather than being retained twice).
		emitReferenceParameterRetains();
		// The body-entry label is the loop target of a direct self tail call; the
		// prologue above (context, trap slot, locals, parameter retains) has
		// already run for this frame.
		bodyStart = compiler.new_label();
		checkError(compiler.bind(bodyStart), "AsmJit body-start label binding failed");
		resultCount = static_cast<uint32_t>(type.results.size());
		resultTypes.reserve(type.results.size());
		for (const auto& result : type.results)
			resultTypes.push_back(result.val.opcode);
		frameResultOffset = frameResultOffsetBytes(type);
	}

	void compile(const WASM::FunctionBody& body, const WASM::FuncType& type)
	{
		const std::span<const std::byte> code(
				reinterpret_cast<const std::byte*>(body.code.data()), body.code.size());
		Euph::Io::ConstBufferDevice device(code);
		WASM::WasmStream<Euph::Io::ConstBufferDevice> stream(device);
		readCode(stream);

		if (resultCount == 0) {
			if (!armTerminated && !values.empty())
				throw std::runtime_error("AsmJit function ended with an unexpected operand stack value");
			// A function whose body ended in dead code carries whatever
			// placeholders the dead structured blocks synthesized; there is no
			// live path to observe them. Drop them and emit the void return.
			values.clear();
			// The implicit end is a return too, so every reference local gives
			// its count back here exactly as an explicit `return` does.
			emitDropReferenceLocals();
			checkError(compiler.ret(), "AsmJit void function return emission failed");
		} else {
			const std::size_t expectedResults = frameMode ? resultCount : 1u;
			// A function label branch emits its return and leaves the following
			// bytes as polymorphic dead code. That code can push arbitrary
			// placeholders before the function's final `end`; they are not live
			// values. Keep the last `expectedResults` so the dead return still
			// has the right operand shape.
			if (inDeadCode() && values.size() > expectedResults)
				values.erase(values.begin(),
						values.end() - static_cast<std::ptrdiff_t>(expectedResults));
			if (values.size() != expectedResults)
				throw std::runtime_error("AsmJit function ended with the wrong number of results");
			// The implicit end releases every reference local's count here, then
			// hands the operand stack's own count to the caller through the
			// result, exactly as an explicit `return` does.
			emitDropReferenceLocals();
			if (frameMode && resultCount > 1) {
				// Multi-result (docs/ABI.md): every
				// result goes into the frame in declaration order, 8 bytes per scalar
				// slot and 16 for a `v128` taken from its local buffer.
				asmjit::x86::Gp destination = compiler.new_gp_ptr("resultAddress");
				emitter.lea(destination,
						asmjit::x86::Mem(framePointer, frameResultOffset));
				const std::size_t firstResult = values.size() - resultCount;
				uint32_t offset = 0;
				for (uint32_t i = 0; i < resultCount; ++i) {
					const WASM::ValueTypeCode code = resultTypes[i];
					const asmjit::x86::Gp value = values[firstResult + i];
					if (code == WASM::ValueTypeCode::V128) {
						for (int word = 0; word < 2; ++word) {
							asmjit::x86::Gp bits = compiler.new_gp64("frameResultWord");
							emitter.mov(bits,
									asmjit::x86::qword_ptr(value, word * 8));
							emitter.mov(
									asmjit::x86::qword_ptr(destination, offset + word * 8), bits);
						}
						offset += 16;
						continue;
					}
					const asmjit::x86::Gp stored = isFloatKind(code)
							? canonicalizeFloatBits(value, kindWidth(code)) : value;
					emitter.mov(
							asmjit::x86::Mem(destination, offset, stored.size()), stored);
					offset += 8;
				}
				checkError(compiler.ret(), "AsmJit frame result return failed");
			} else if (frameMode) {
				emitFrameReturnValue(values.back());
			} else if (isFloatKind(resultType)) {
				const int width = kindWidth(resultType);
				checkError(compiler.ret(materializeFloat(
						canonicalizeFloatBits(values.back(), width), width)),
						"AsmJit float function return emission failed");
			} else if (bufferedVectorResult) {
				// The value is the address of the vector 16-byte buffer; copy it into the
				// caller buffer and return void.
				asmjit::x86::Gp destination = compiler.new_gp_ptr("v128ResultOut");
				emitter.mov(destination, v128ResultSlot);
				asmjit::x86::Gp source = values.back();
				asmjit::x86::Gp word = compiler.new_gp64("v128ResultWord");
				emitter.mov(word, asmjit::x86::qword_ptr(source, 0));
				emitter.mov(asmjit::x86::qword_ptr(destination, 0), word);
				emitter.mov(word, asmjit::x86::qword_ptr(source, 8));
				emitter.mov(asmjit::x86::qword_ptr(destination, 8), word);
				checkError(compiler.ret(), "AsmJit v128 result return emission failed");
			} else {
				checkError(compiler.ret(values.back()),
						"AsmJit function return emission failed");
			}
		}
		(void)type;
	}

protected:
	void dispatchI32Const(int32_t value) override
	{
		asmjit::x86::Gp reg = compiler.new_gp32("constant");
		emitter.movImm(reg, value);
		values.push_back(reg);
	}

	void dispatchLocalGet(WASM::LocalIdx index) override
	{
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size())
			throw std::runtime_error("AsmJit local.get index is out of range");
		if (localWidths[i] == 16) {
			asmjit::x86::Gp copy = newVectorBuffer("v128Copy");
			copyVector(copy, locals[i]);
			values.push_back(copy);
			return;
		}
		asmjit::x86::Gp copy = locals[i].size() == 8
				? compiler.new_gp64("localCopy") : compiler.new_gp32("localCopy");
		emitter.mov(copy, locals[i]);
		if (isReferenceKind(localKinds[i])) {
			// The slot keeps its own count; the clone is the operand stack's.
			copy = emitRefClone(copy);
			markReference(copy);
		}
		values.push_back(copy);
	}

	void dispatchLocalSet(WASM::LocalIdx index) override
	{
		if (inDeadCode())
			return;
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size() || values.empty())
			throw std::runtime_error("AsmJit local.set stack/index underflow");
		const asmjit::x86::Gp value = values.back();
		values.pop_back();
		if (localWidths[i] == 16) {
			copyVector(locals[i], value);
			return;
		}
		if (isReferenceKind(localKinds[i])) {
			// The slot owns one count and the stack one: the barrier retains for
			// the slot, and `set` consumes the stack's value, so its count goes.
			referenceValues.erase(value.id());
			emitLocalRefStore(i, value);
			emitRefDrop(value);
			return;
		}
		emitter.mov(locals[i], value);
	}

	void dispatchLocalTee(WASM::LocalIdx index) override
	{
		if (inDeadCode())
			return;
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size() || values.empty())
			throw std::runtime_error("AsmJit local.tee stack/index underflow");
		if (localWidths[i] == 16) {
			copyVector(locals[i], values.back());
			return;
		}
		if (isReferenceKind(localKinds[i])) {
			// A `tee` leaves the value on the stack, so the slot and the stack each
			// own a count: the barrier retains for the slot and the stack keeps the
			// one it already holds -- the value itself must not be released.
			emitLocalRefStore(i, values.back());
			return;
		}
		asmjit::x86::Gp copy = locals[i].size() == 8
				? compiler.new_gp64("teeCopy") : compiler.new_gp32("teeCopy");
		emitter.mov(copy, values.back());
		emitter.mov(locals[i], copy);
		values.back() = copy;
	}


    void dispatchSelect() override
    {
        emitSelect();
    }

    void dispatchSelectT(std::vector<WASM::ValueType>&& types) override
    {
        if (types.size() != 1)
            throw std::runtime_error("AsmJit select_t currently supports exactly one value type");
        // A v128 value is an immutable 16-byte buffer, so selecting its address
        // selects the value; no byte-wise select is needed.
        if (types.front().opcode != WASM::ValueTypeCode::V128)
            requireScalar(types.front(), "select_t values");
        emitSelect();
    }

	void pushStringValue(const asmjit::x86::Gp& value)
	{
		values.push_back(value);
		stringValues.insert(value.id());
		referenceValues.insert(value.id());
	}

	//! Marks a value that was just pushed as owning one reference count.
	void markReference(const asmjit::x86::Gp& value)
	{
		referenceValues.insert(value.id());
	}

	//! Reports whether a value owns a reference count.
	bool isReferenceValue(const asmjit::x86::Gp& value) const
	{
		return referenceValues.count(value.id()) != 0;
	}

	//! Emits the reference write barrier for a local: retain the incoming value
	//! for the slot, store it, release the value the slot held.
	void emitLocalRefStore(std::size_t index, const asmjit::x86::Gp& value)
	{
		// The old value is copied into a fresh register first: writing the local
		// overwrites its virtual register, so a handle to it would read the new
		// value afterwards.
		const asmjit::x86::Gp old = compiler.new_gp64("localOldRef");
		emitter.mov(old, locals[index]);
		const asmjit::x86::Gp kept = emitRefClone(value);
		emitter.mov(locals[index], kept);
		emitRefDrop(old);
	}

	//! Retains every reference parameter into its own local slot, so the local
	//! ownership rules hold for every instruction -- including one that
	//! re-executes in a loop -- without per-instruction bookkeeping.
	void emitReferenceParameterRetains()
	{
		const std::size_t count = std::min<std::size_t>(parameterCount, localKinds.size());
		for (std::size_t i = 0; i < count; ++i) {
			if (!isReferenceKind(localKinds[i]) || localWidths[i] != 8)
				continue;
			const asmjit::x86::Gp kept = emitRefClone(locals[i]);
			emitter.mov(locals[i], kept);
		}
	}

	asmjit::x86::Gp emitRefClone(const asmjit::x86::Gp& value)
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_ref_clone), signature),
				"AsmJit reference clone helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, value);
		auto result = compiler.new_gp_ptr("referenceClone");
		invoke->set_ret(0, result);
		return result;
	}

	void emitRefDrop(const asmjit::x86::Gp& value)
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_ref_drop), signature),
				"AsmJit reference drop helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, value);
		emitPendingTrapReturn();
	}

	//! Emits the reference write barrier for a store-owned slot that lives in
	//! memory (a struct field, an array element).
	//! \param address Address of the slot being written.
	//! \param value New reference value.
	void emitRefStore(const asmjit::x86::Gp& address, const asmjit::x86::Gp& value)
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_ref_store), signature),
				"AsmJit reference store barrier call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, address);
		invoke->set_arg(2, value);
		emitPendingTrapReturn();
	}

	void dispatchDrop() override
	{
		// Polymorphic dead code after a terminator has no operand stack to
		// consume; validation has already checked the shape of the live path.
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit drop operand stack underflow");
		auto dropped = values.back();
		values.pop_back();
		const bool reference = referenceValues.erase(dropped.id()) != 0;
		stringValues.erase(dropped.id());
		// Discarding a reference releases the operand stack's ownership of it; a
		// scalar has no count to give back.
		if (reference) {
			emitRefDrop(dropped);
			return;
		}
	}

	void dispatchBlock(const WASM::BlockType& type) override
	{
		pushControlFrame(ControlKind::Block, type);
	}

	void dispatchTryTable(const WASM::BlockType& type,
			std::vector<WASM::CatchClause>&& catches) override
	{
		pushControlFrame(ControlKind::Block, type);
		ControlFrame& frame = controls.back();
		frame.tryTable = true;
		frame.catchClauses = std::move(catches);
		const std::size_t frameIndex = controls.size() - 1;
		for (const WASM::CatchClause& clause : frame.catchClauses) {
			if (clause.labelIdx > frameIndex)
				throw std::runtime_error("AsmJit try_table catch label is out of range");
			const bool functionLabel = clause.labelIdx == frameIndex;
			ControlFrame* target = functionLabel
					? nullptr : &controls[frameIndex - 1 - clause.labelIdx];
			const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
					|| clause.kind == WASM::CatchKind::CatchAllRef;
			const std::size_t payloadCount = catchAll
					? 0u : tagPayloadType(clause.tagIdx).params.size();
			const std::size_t carried = payloadCount + (carryExn ? 1u : 0u);
			const std::size_t targetValues = functionLabel
					? resultCount
					: (target->kind == ControlKind::Loop
						? target->paramCount : target->resultCount);
			if (targetValues != carried)
				throw std::runtime_error("AsmJit try_table catch arity does not match its target label");
		}
	}

	void dispatchThrow(WASM::TagIdx arg) override
	{
		// A throw is a terminator. A following `throw` is polymorphic code and
		// may legally have no operand to pop; `inDeadCode()` is false when the
		// module has tags, so test the arm state directly.
		if (armTerminated) {
			emitUnreachablePlaceholders();
			return;
		}
		if (arg >= internals.tagIds.size())
			throw std::runtime_error("AsmJit throw names an out-of-range tag");
		const WASM::TagId tagId = internals.tagIds[arg];
		const WASM::FuncType payload = tagPayloadType(arg);
		if (values.size() < payload.params.size())
			throw std::runtime_error("AsmJit throw operand stack underflow");
		std::vector<asmjit::x86::Gp> payloadValues(payload.params.size());
		for (std::size_t i = payload.params.size(); i-- > 0; ) {
			payloadValues[i] = values.back();
			values.pop_back();
		}
		const ExceptionPayloadLayout layout = exceptionPayloadLayout(payload);

		asmjit::FuncSignature createSignature(asmjit::CallConvId::kCDecl);
		createSignature.set_ret(asmjit::TypeId::kUIntPtr);
		createSignature.add_arg(asmjit::TypeId::kUIntPtr);
		createSignature.add_arg(asmjit::TypeId::kInt32);
		createSignature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* create = nullptr;
		checkError(compiler.invoke(asmjit::Out(create),
				reinterpret_cast<uint64_t>(&::wasm_exn_create), createSignature),
				"AsmJit failed to emit exception allocation");
		auto exn = compiler.new_gp_ptr("thrownException");
		create->set_arg(0, vmContext);
		create->set_arg(1, asmjit::Imm(static_cast<int32_t>(tagId)));
		create->set_arg(2, asmjit::Imm(static_cast<uint32_t>(layout.size)));
		create->set_ret(0, exn);
		emitPendingTrapReturn();
		emitter.mov(exceptionPtrSlot, exn);

		for (std::size_t i = 0; i < payloadValues.size(); ++i) {
			const int32_t offset = static_cast<int32_t>(layout.fields[i].offset);
			const int width = kindWidth(payload.params[i].val.opcode);
			if (width == 16) {
				for (int word = 0; word < 2; ++word) {
					auto bits = compiler.new_gp64("thrownPayloadWord");
					emitter.mov(bits,
							asmjit::x86::qword_ptr(payloadValues[i], word * 8));
					emitter.mov(
							asmjit::x86::qword_ptr(exn, offset + word * 8), bits);
				}
				continue;
			}
			emitter.mov(
					asmjit::x86::Mem(exn, offset, payloadValues[i].size()),
					payloadValues[i]);
		}

		asmjit::FuncSignature setSignature(asmjit::CallConvId::kCDecl);
		setSignature.set_ret(asmjit::TypeId::kVoid);
		setSignature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* set = nullptr;
		checkError(compiler.invoke(asmjit::Out(set),
				reinterpret_cast<uint64_t>(&::wasm_exn_set_pending), setSignature),
				"AsmJit failed to emit pending-exception store");
		set->set_arg(0, exn);
		emitExceptionPropagation();
		emitUnreachablePlaceholders();
		// The instruction after a throw is polymorphic. In a module with tags
		// `inDeadCode()` cannot key off `armTerminated` alone (catch edges must
		// stay reachable), but the arm itself is terminated.
		armTerminated = true;
	}

	void dispatchThrowRef() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit throw_ref operand stack underflow");
		auto exn = values.back();
		values.pop_back();
		auto notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(exn, exn));
		asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
		reportSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* report = nullptr;
		checkError(compiler.invoke(asmjit::Out(report),
				reinterpret_cast<uint64_t>(&asmjitReportNullReference), reportSignature),
				"AsmJit throw_ref null trap emission failed");
		emitZeroReturn();
		checkError(compiler.bind(notNull), "AsmJit throw_ref continuation binding failed");
		emitter.mov(exceptionPtrSlot, exn);
		asmjit::FuncSignature setSignature(asmjit::CallConvId::kCDecl);
		setSignature.set_ret(asmjit::TypeId::kVoid);
		setSignature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* set = nullptr;
		checkError(compiler.invoke(asmjit::Out(set),
				reinterpret_cast<uint64_t>(&::wasm_exn_set_pending), setSignature),
				"AsmJit failed to emit throw_ref pending store");
		set->set_arg(0, exn);
		emitExceptionPropagation();
		emitUnreachablePlaceholders();
	}

	void dispatchLoop(const WASM::BlockType& type) override
	{
		pushControlFrame(ControlKind::Loop, type);
		checkError(compiler.bind(controls.back().begin), "AsmJit loop label binding failed");
	}

	void dispatchIf(const WASM::BlockType& type) override
	{
		if (inDeadCode() && values.empty()) {
			// Truly dead `if`: still open the structured frame so `else`/`end`
			// pair up, but there is no condition to test and no branch to emit.
			// If values *are* present, lower the condition normally: in an EH
			// module this is a catch entry, and in ordinary dead code the
			// placeholder condition must still be consumed.
			pushControlFrame(ControlKind::If, type);
			controls.back().alternate = compiler.new_label();
			return;
		}
		if (values.empty())
			throw std::runtime_error("AsmJit if condition stack underflow");
		auto condition = values.back();
		values.pop_back();
		pushControlFrame(ControlKind::If, type);
		auto& frame = controls.back();
		frame.alternate = compiler.new_label();
		emitter.j(frame.alternate, asmjit::ujit::test_z(condition, condition));
	}

	void dispatchElse() override
	{
		if (controls.empty() || controls.back().kind != ControlKind::If || controls.back().hasElse)
			throw std::runtime_error("AsmJit encountered an unmatched or duplicate else");
		ControlFrame& frame = controls.back();
		frame.thenReachable = !armTerminated;
		if (frame.thenReachable)
			storeBlockResult(frame, "if then");
		emitter.jmp(frame.end);
		checkError(compiler.bind(frame.alternate), "AsmJit else label binding failed");
		values.resize(frame.stackHeight);
		for (const asmjit::x86::Gp& param : frame.params)
			values.push_back(param);
		armTerminated = !frame.entryReachable;
		frame.hasElse = true;
	}

	void dispatchEnd() override
	{
		if (controls.empty())
			return;
		ControlFrame frame = controls.back();
		controls.pop_back();
		bool endReachable = true;
		if (frame.kind == ControlKind::If && !frame.hasElse) {
			// An if-without-else may produce results only when its parameters
			// already have the result shape. The true arm can compute a
			// different value than the parameters; the implicit false arm
			// forwards the parameters.
			if (frame.resultCount != 0) {
				if (frame.resultCount != frame.paramCount)
					throw std::runtime_error("AsmJit if-without-else has mismatched parameters and results");
			}
			if (!armTerminated
					&& values.size() != frame.stackHeight + frame.paramCount)
				throw std::runtime_error("AsmJit if arm leaves an unexpected operand stack shape");
			if (!armTerminated) {
				// The true arm produced its result; store it before jumping over
				// the implicit-else forwarding block.
				storeBlockResult(frame, "implicit if");
				emitter.jmp(frame.end);
			}
			checkError(compiler.bind(frame.alternate), "AsmJit implicit else label binding failed");
			for (std::size_t i = 0; i < frame.resultCount; ++i)
				transferValue(frame.results[i], frame.params[i],
						frame.resultTypes[i], "AsmJit implicit-else result transfer failed");
		} else {
			const bool armLive = !armTerminated;
			endReachable = armLive || frame.endTargeted;
			if (frame.kind == ControlKind::If && frame.hasElse)
				endReachable = endReachable || frame.thenReachable;
			if (armLive)
				storeBlockResult(frame, "structured block");
		}
		checkError(compiler.bind(frame.end), "AsmJit structured block end binding failed");
		values.resize(frame.stackHeight);
		for (const asmjit::x86::Gp& result : frame.results)
			values.push_back(result);
		armTerminated = !endReachable;
	}

	void dispatchBrIf(WASM::LabelIdx depth) override
	{
		if (inDeadCode() && values.empty())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit br_if condition stack underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_if label depth is out of range");
		const auto condition = values.back();
		values.pop_back();
		if (depth == controls.size()) {
			if (values.size() < resultCount) {
				if (inDeadCode()) return;
				throw std::runtime_error("AsmJit conditional function branch has too few results");
			}
			auto skip = compiler.new_label();
			emitter.j(skip, asmjit::ujit::test_z(condition, condition));
			emitFunctionReturnValues();
			checkError(compiler.bind(skip), "AsmJit conditional function branch label binding failed");
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const bool loopTarget = target.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? target.paramCount : target.resultCount;
		// A branch consumes its target's arity values and discards everything
		// above the target's height. A loop back-edge is no exception: only the
		// top `paramCount` operands become the next iteration's parameters, and
		// any extra live values are dropped by the branch.
		const bool branchShapeMatches = values.size() >= target.stackHeight + branchValues;
		if (!branchShapeMatches) {
			if (inDeadCode()) return;
			throw std::runtime_error("AsmJit br_if operand stack does not match its target label");
		}
		const asmjit::Label label = loopTarget ? target.begin : target.end;
		if (branchValues != 0) {
			auto skip = compiler.new_label();
			emitter.j(skip, asmjit::ujit::test_z(condition, condition));
			const size_t first = values.size() - branchValues;
			const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? target.params : target.results;
			const std::vector<WASM::ValueTypeCode>& codes = loopTarget
					? target.paramTypes : target.resultTypes;
			for (size_t i = 0; i < branchValues; ++i)
				transferValue(destinations[i], values[first + i], codes[i],
						"AsmJit br_if result transfer failed");
			emitter.jmp(label);
			if (!loopTarget)
				target.endTargeted = true;
			checkError(compiler.bind(skip), "AsmJit br_if skip label binding failed");
		} else {
			emitter.j(label, asmjit::ujit::test_nz(condition, condition));
			// A zero-arity branch still makes its target's `end` reachable:
			// control continues after that block even though nothing is
			// transferred. Without this, a `br_if` to an empty block leaves the
			// block end marked dead, and every instruction after it is skipped
			// as unreachable (loop.wast's `nesting` lost its division and
			// decrement this way).
			if (!loopTarget)
				target.endTargeted = true;
		}
	}

	void dispatchBr(WASM::LabelIdx depth) override
	{
		if (inDeadCode() && values.empty())
			return;
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br label depth is out of range");
		if (depth == controls.size()) {
			if (values.size() < resultCount) {
				if (inDeadCode()) return;
				throw std::runtime_error("AsmJit function branch has too few results");
			}
			emitFunctionReturnValues();
			armTerminated = true;
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const bool loopTarget = target.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? target.paramCount : target.resultCount;
		// See dispatchBrIf: a branch may leave extra operands below its target's
		// arguments; only underflow is invalid.
		const bool branchShapeMatches = values.size() >= target.stackHeight + branchValues;
		if (!branchShapeMatches) {
			if (inDeadCode()) return;
			throw std::runtime_error("AsmJit br operand stack does not match its target label");
		}
		if (branchValues != 0) {
			const size_t first = values.size() - branchValues;
			const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? target.params : target.results;
			const std::vector<WASM::ValueTypeCode>& codes = loopTarget
					? target.paramTypes : target.resultTypes;
			for (size_t i = 0; i < branchValues; ++i)
				transferValue(destinations[i], values[first + i], codes[i],
						"AsmJit br result transfer failed");
		}
		const asmjit::Label label = loopTarget ? target.begin : target.end;
		emitter.jmp(label);
		if (!loopTarget)
			target.endTargeted = true;
		armTerminated = true;
	}
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& depths, WASM::LabelIdx defaultDepth) override
	{
		if (inDeadCode() && values.empty())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit br_table selector stack underflow");
		const auto selector = values.back();
		values.pop_back();
		size_t expectedBranchValues = 0;
		bool first = true;
		auto validateTarget = [&](WASM::LabelIdx depth) {
			if (depth > controls.size())
				throw std::runtime_error("AsmJit br_table label depth is out of range");
			size_t branchValues = resultCount;
			if (depth < controls.size()) {
				const ControlFrame& target = controls[controls.size() - 1 - depth];
				branchValues = target.kind == ControlKind::Loop
						? target.paramCount : target.resultCount;
			}
			if (first) {
				expectedBranchValues = branchValues;
				first = false;
			} else if (branchValues != expectedBranchValues) {
				// The labels must agree on arity, but not on identity: their
				// nesting can differ, and a reference target may accept a
				// subtype (`(ref null $t)` into `funcref`). Each target's own
				// types drive its transfer below, exactly as the validator
				// checked them. The branch discards everything above the
				// *taken* target's height, so equal stack heights are not
				// required either.
				throw std::runtime_error("AsmJit br_table targets require identical arity");
			}
		};
		for (WASM::LabelIdx depth : depths)
			validateTarget(depth);
		validateTarget(defaultDepth);
		// A branch consumes its label's arity values and discards everything
		// above the target's stack height; extra live values below the branch
		// operands are therefore legal. Only an underflow is a mismatch.
		if (values.size() < expectedBranchValues) {
			if (inDeadCode()) return;
			throw std::runtime_error("AsmJit br_table operand stack does not match its target labels");
		}
		auto emitTarget = [&](WASM::LabelIdx depth) {
			if (depth == controls.size()) {
				// Every case still needs the same operand stack after this target.
				// `dispatchReturn` consumes it and marks the arm terminated, so
				// restore both for the next case; otherwise its dead-code guard
				// would suppress every return after the first.
				const auto saved = values;
				const bool savedTerminated = armTerminated;
				armTerminated = false;
				dispatchReturn();
				values = saved;
				armTerminated = savedTerminated;
				return;
			}
			ControlFrame& target = controls[controls.size() - 1 - depth];
			const bool loopTarget = target.kind == ControlKind::Loop;
			const size_t transferCount = loopTarget ? target.paramCount : target.resultCount;
			if (transferCount != 0) {
				const size_t firstValue = values.size() - transferCount;
				const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? target.params : target.results;
				const std::vector<WASM::ValueTypeCode>& codes = loopTarget
						? target.paramTypes : target.resultTypes;
				for (size_t i = 0; i < transferCount; ++i)
					transferValue(destinations[i], values[firstValue + i], codes[i],
							"AsmJit br_table result transfer failed");
			}
			const asmjit::Label label = loopTarget ? target.begin : target.end;
			emitter.jmp(label);
			if (!loopTarget)
				target.endTargeted = true;
		};
		for (size_t i = 0; i < depths.size(); ++i) {
			auto matched = compiler.new_label();
			auto next = compiler.new_label();
			emitter.j(matched, asmjit::ujit::cmp_eq(selector, static_cast<uint32_t>(i)));
			emitter.jmp(next);
			checkError(compiler.bind(matched), "AsmJit br_table match label binding failed");
			emitTarget(depths[i]);
			checkError(compiler.bind(next), "AsmJit br_table next-case label binding failed");
		}
		emitTarget(defaultDepth);
		armTerminated = true;
		// Do not clear `values`: a target block's `end` resizes them back to its
		// own stack height, preserving the base values below the branch operands.
		// Clearing here inserted default-constructed registers and made the
		// following `i32.add` in a nested block fail with InvalidRegType.
	}

	void dispatchReturnCall(WASM::FuncIdx index) override
	{
		if (inDeadCode())
			return;
		armTerminated = true;
		const size_t importedCount = module.importFunctions.size();
		const size_t totalCount = importedCount + module.internalFunctionTypeIndices.size();
		if (index >= totalCount)
			throw std::runtime_error("AsmJit return_call function index is out of range");
		const uint32_t typeIndex = index < importedCount
				? module.importFunctions[index].typeIdx
				: module.internalFunctionTypeIndices[index - importedCount];
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		// A tail call discards everything below its arguments; only underflow is
		// invalid (same rule as a branch to a function-end label).
		if (values.size() < calledType.params.size())
			throw std::runtime_error("AsmJit return_call has too few arguments");
		if (index == functionIndex) {
			// Direct self-recursion: overwrite the parameter locals, reset the rest,
			// and loop to the body start. The frame is reused, so the host stack does
			// not grow. Mutual/indirect chains still use call + return until the
			// cross-function trampoline lands.
			std::vector<asmjit::x86::Gp> selfArgs(calledType.params.size());
			for (std::size_t i = calledType.params.size(); i-- > 0; ) {
				selfArgs[i] = values.back();
				values.pop_back();
			}
			for (std::size_t i = 0; i < selfArgs.size(); ++i) {
				if (localWidths[i] == 16)
					copyVector(locals[i], selfArgs[i]);
				else
					emitter.mov(locals[i], selfArgs[i]);
			}
			for (std::size_t i = parameterCount; i < locals.size(); ++i) {
				if (localWidths[i] == 16)
					zeroVector(locals[i]);
				else
					emitter.xor_(locals[i], locals[i]);
			}
			emitter.jmp(bodyStart);
			emitUnreachablePlaceholders();
			return;
		}
		const bool tailSupported = calleeUsesFrameForm(typeIndex)
				? WASM::tailCallFrameSupported(calledType)
				: WASM::tailCallSlotsSupported(calledType);
		if (WASM::tailCallTrampolinesEligible(module, frameByType) && tailSupported) {
			// Cross-function trampoline: the driver wrapping this function observes
			// the pending state and re-enters the target through its type thunk.
			std::vector<asmjit::x86::Gp> tailArgs(calledType.params.size());
			for (std::size_t i = calledType.params.size(); i-- > 0; ) {
				tailArgs[i] = values.back();
				values.pop_back();
			}
			const std::size_t imported = module.importFunctions.size();
			const void* target = index < imported
					? static_cast<const void*>(&internals.importStorage[index])
					: static_cast<const void*>(&internals.internalCallables[index - imported]);
			auto targetRegister = compiler.new_gp64("tailTarget");
			emitter.mov(targetRegister,
					asmjit::Imm(reinterpret_cast<uint64_t>(target)));
			emitTailCallState(targetRegister, tailArgs, calledType);
			emitUnreachablePlaceholders();
			return;
		}
		if (frameMode) {
			// Frame form: the callee writes into the staging frame and the tail call
			// becomes a call plus a frame-mode return. The host stack still grows;
			// the register form's call+return has the same limitation.
			if (calledType.results.size() != resultCount)
				throw std::runtime_error("AsmJit frame tail-call result arity differs from the caller");
			for (std::size_t i = 0; i < resultCount; ++i)
				if (calledType.results[i].val.opcode != resultTypes[i])
					throw std::runtime_error("AsmJit frame tail-call result type differs from the caller");
			dispatchCall(index);
			if (resultCount == 0) {
				checkError(compiler.ret(), "AsmJit frame tail-call void return failed");
				return;
			}
			if (resultCount > 1)
				throw std::runtime_error("AsmJit frame tail calls with multiple results are not implemented");
			emitFrameReturnValue(values.back());
			return;
		}
		dispatchCall(index);
		dispatchReturn();
	}

	void dispatchReturnCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex) override
	{
		if (inDeadCode())
			return;
		armTerminated = true;
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		// Extra values below the callee arguments are discarded by the tail call.
		if (values.size() < calledType.params.size() + 1)
			throw std::runtime_error("AsmJit return_call_indirect has too few arguments");
		if (frameMode) {
			if (calledType.results.size() != resultCount)
				throw std::runtime_error("AsmJit frame tail-call result arity differs from the caller");
			for (std::size_t i = 0; i < resultCount; ++i)
				if (calledType.results[i].val.opcode != resultTypes[i])
					throw std::runtime_error("AsmJit frame tail-call result type differs from the caller");
			const bool calleeFrameForm = calleeUsesFrameForm(typeIndex);
			const bool tailFits = calleeFrameForm
					? WASM::tailCallFrameSupported(calledType)
					: WASM::tailCallSlotsSupported(calledType);
			if (WASM::tailCallTrampolinesEligible(module, frameByType) && tailFits) {
				const ResolvedIndirect resolved = resolveIndirectTarget(typeIndex, tableIndex);
				auto thunk = compiler.new_gp_ptr("tailThunk");
				emitter.mov(thunk, asmjit::x86::ptr(resolved.callable,
						static_cast<int32_t>(offsetof(WASM::Callable, tailEntry))));
				auto fallback = compiler.new_label();
				emitter.j(fallback, asmjit::ujit::test_z(thunk, thunk));
				emitTailCallState(resolved.callable, resolved.arguments, calledType);
				checkError(compiler.bind(fallback), "AsmJit frame tail-call fallback label failed");
				emitIndirectInvocation(resolved, calledType, typeIndex);
				dispatchReturn();
				return;
			}
			dispatchCallIndirect(typeIndex, tableIndex);
			dispatchReturn();
			return;
		}
		const bool calleeFrameForm = calleeUsesFrameForm(typeIndex);
		const bool tailFits = calleeFrameForm
				? WASM::tailCallFrameSupported(calledType)
				: WASM::tailCallSlotsSupported(calledType);
		if (WASM::tailCallTrampolinesEligible(module, frameByType) && tailFits) {
			const ResolvedIndirect resolved = resolveIndirectTarget(typeIndex, tableIndex);
			auto thunk = compiler.new_gp_ptr("tailThunk");
			emitter.mov(thunk, asmjit::x86::ptr(resolved.callable,
					static_cast<int32_t>(offsetof(WASM::Callable, tailEntry))));
			auto fallback = compiler.new_label();
			emitter.j(fallback, asmjit::ujit::test_z(thunk, thunk));
			emitTailCallState(resolved.callable, resolved.arguments, calledType);
			checkError(compiler.bind(fallback), "AsmJit tail-call fallback label binding failed");
			emitIndirectInvocation(resolved, calledType, typeIndex);
			dispatchReturn();
			return;
		}
		dispatchCallIndirect(typeIndex, tableIndex);
		dispatchReturn();
	}

	struct ResolvedIndirect {
		asmjit::x86::Gp callable;
		std::vector<asmjit::x86::Gp> arguments;
	};

	//! Validates an indirect call and resolves its table slot to a Callable.
	//! The parameters are the declared callee type and the table index; the
	//! result is the callable plus its already-popped arguments.
	ResolvedIndirect resolveIndirectTarget(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex)
	{
		if (tableIndex >= module.importTables.size() + module.tables.size())
			throw std::runtime_error("AsmJit call_indirect references an unknown table");
		if (typeIndex >= signatures.size() || !signatures[typeIndex])
			throw std::runtime_error("AsmJit call_indirect references an unavailable function type");
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (!calleeUsesFrameForm(typeIndex) && calledType.results.size() > 1)
			throw std::runtime_error("AsmJit register-form call_indirect supports at most one result");
		// Frame mode handles a v128 parameter/result through the frame block; the
		// register form has no aggregate spelling for it.
		for (const auto& parameter : calledType.params)
			if (!calleeUsesFrameForm(typeIndex)
					|| parameter.val.opcode != WASM::ValueTypeCode::V128)
				requireScalar(parameter.val, "indirect-call parameters");
		for (const auto& result : calledType.results)
			if (!frameMode || result.val.opcode != WASM::ValueTypeCode::V128)
				requireScalar(result.val, "indirect-call results");
		if (values.size() < calledType.params.size() + 1)
			throw std::runtime_error("AsmJit call_indirect operand stack underflow");

		auto elementIndex = values.back();
		values.pop_back();
		std::vector<asmjit::x86::Gp> arguments(calledType.params.size());
		for (size_t i = calledType.params.size(); i > 0; --i) {
			arguments[i - 1] = values.back();
			values.pop_back();
		}

		asmjit::FuncSignature resolverSignature(asmjit::CallConvId::kCDecl);
		resolverSignature.set_ret(asmjit::TypeId::kUIntPtr);
		resolverSignature.add_arg(asmjit::TypeId::kUIntPtr);
		resolverSignature.add_arg(asmjit::TypeId::kUInt32);
		resolverSignature.add_arg(asmjit::TypeId::kUInt32);
		resolverSignature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* resolver = nullptr;
		auto table = compiler.new_gp32("indirectTableIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		auto expected = compiler.new_gp32("indirectExpectedType");
		emitter.mov(expected, module.typeId(WASM::LocalTypeIdx{typeIndex}).value);
		checkError(compiler.invoke(asmjit::Out(resolver),
				reinterpret_cast<uint64_t>(&asmjitResolveIndirectCallable), resolverSignature),
				"AsmJit indirect-call resolver emission failed");
		resolver->set_arg(0, vmContext);
		resolver->set_arg(1, table);
		resolver->set_arg(2, elementIndex);
		resolver->set_arg(3, expected);
		auto callable = compiler.new_gp_ptr("indirectCallable");
		resolver->set_ret(0, callable);
		emitPendingTrapReturn();
		return {callable, std::move(arguments)};
	}

	//! Emits the context selection and the indirect invocation itself.
	//! \param resolved Resolved callable and arguments. \param calledType Declared signature.
	void emitIndirectInvocation(const ResolvedIndirect& resolved, const WASM::FuncType& calledType,
			uint32_t typeIndex)
	{
		auto target = compiler.new_gp_ptr("indirectFunction");
		emitter.mov(target, asmjit::x86::ptr(resolved.callable,
				static_cast<int32_t>(offsetof(WASM::Callable, fnPtr))));
		auto calleeContext = compiler.new_gp_ptr("indirectContext");
		emitter.mov(calleeContext, asmjit::x86::ptr(resolved.callable,
				static_cast<int32_t>(offsetof(WASM::Callable, context))));
		auto callerContext = compiler.new_label();
		emitter.j(callerContext, asmjit::ujit::test_nz(calleeContext, calleeContext));
		emitter.mov(calleeContext, vmContext);
		checkError(compiler.bind(callerContext), "AsmJit indirect context label binding failed");
		// A parameter is the caller's borrow: the counts the operand stack holds
		// must survive the invoke, so each reference argument is parked in its
		// own stack slot first and released from there afterwards.
		const std::vector<asmjit::x86::Mem> keptArguments =
				spillCallReferenceArguments(calledType, resolved.arguments);
		if (calleeUsesFrameForm(typeIndex)) {
			uint32_t argBytes = 0;
			for (const WASM::StorageType& parameter : calledType.params)
				argBytes += parameter.val.opcode == WASM::ValueTypeCode::V128 ? 16u : 8u;
			asmjit::x86::Mem frameStorage = compiler.new_stack(
					frameCallStorageBytes(calledType), 16, "indirectFrame");
			asmjit::x86::Gp frameAddress = compiler.new_gp_ptr("indirectFrame");
			emitter.lea(frameAddress, frameStorage);
			uint32_t offset = 0;
			for (std::size_t i = 0; i < resolved.arguments.size(); ++i) {
				if (calledType.params[i].val.opcode == WASM::ValueTypeCode::V128) {
					for (int word = 0; word < 2; ++word) {
						auto bits = compiler.new_gp64("indirectFrameWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(resolved.arguments[i], word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(frameAddress, offset + word * 8), bits);
					}
					offset += 16;
					continue;
				}
				emitter.mov(
						asmjit::x86::Mem(frameAddress, offset, resolved.arguments[i].size()),
						resolved.arguments[i]);
				offset += 8;
			}
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]),
					"AsmJit indirect frame invocation failed");
			invoke->set_arg(0, calleeContext);
			invoke->set_arg(1, frameAddress);
			uint32_t resultOffset = offset;
			for (const WASM::StorageType& resultType : calledType.results) {
				if (resultType.val.opcode == WASM::ValueTypeCode::V128) {
					auto buffer = newVectorBuffer("indirectVectorResult");
					for (int word = 0; word < 2; ++word) {
						auto bits = compiler.new_gp64("indirectResultWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(frameAddress, resultOffset + word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(buffer, word * 8), bits);
					}
					values.push_back(buffer);
					resultOffset += 16;
				} else {
					auto result = newIntegerRegister(compiler, resultType.val, "indirectResult");
					emitter.mov(result,
							asmjit::x86::Mem(frameAddress, resultOffset, result.size()));
					// A reference result arrives owning one count (the callee hands
					// over the count its operand stack held).
					if (isReferenceKind(resultType.val.opcode))
						markReference(result);
					values.push_back(result);
					resultOffset += 8;
				}
			}
			emitPendingTrapReturn();
			emitPendingExceptionCheck();
			dropCallReferenceArguments(keptArguments);
			return;
		}

		auto preparedArguments = prepareCallArguments(calledType, resolved.arguments);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]),
				"AsmJit indirect function invocation emission failed");
		invoke->set_arg(0, calleeContext);
		setCallArguments(invoke, preparedArguments);
		emitCallResult(invoke, calledType, "indirectCallResult");
		emitPendingTrapReturn();
		emitPendingExceptionCheck();
		dropCallReferenceArguments(keptArguments);
	}

	//! Traps when a function reference is null, continuing on the non-null path.
	void emitNullRefCheck(const asmjit::x86::Gp& callable)
	{
		auto nonNull = compiler.new_label();
		emitter.j(nonNull, asmjit::ujit::test_nz(callable, callable));
		asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
		reportSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* report = nullptr;
		checkError(compiler.invoke(asmjit::Out(report),
				reinterpret_cast<uint64_t>(&asmjitReportNullFunctionReference), reportSignature),
				"AsmJit null-reference trap emission failed");
		emitZeroReturn();
		checkError(compiler.bind(nonNull), "AsmJit null-reference continuation binding failed");
	}

	//! Invokes a `Callable` whose address is already in a register.
	void emitCallRefInvocation(const asmjit::x86::Gp& callable,
			const std::vector<asmjit::x86::Gp>& arguments,
			const WASM::FuncType& calledType, uint32_t typeIndex)
	{
		auto target = compiler.new_gp_ptr("refFunction");
		emitter.mov(target, asmjit::x86::ptr(callable,
				static_cast<int32_t>(offsetof(WASM::Callable, fnPtr))));
		auto calleeContext = compiler.new_gp_ptr("refContext");
		emitter.mov(calleeContext, asmjit::x86::ptr(callable,
				static_cast<int32_t>(offsetof(WASM::Callable, context))));
		auto callerContext = compiler.new_label();
		emitter.j(callerContext, asmjit::ujit::test_nz(calleeContext, calleeContext));
		emitter.mov(calleeContext, vmContext);
		checkError(compiler.bind(callerContext), "AsmJit call_ref context label binding failed");
		// A parameter is the caller's borrow: the counts the operand stack holds
		// must survive the invoke, so each reference argument is parked in its
		// own stack slot first and released from there afterwards.
		const std::vector<asmjit::x86::Mem> keptArguments =
				spillCallReferenceArguments(calledType, arguments);
		if (calleeUsesFrameForm(typeIndex)) {
			// Staging-frame ABI: pack the arguments, invoke (ctx, frame), read the
			// result block back into the value stack.
			uint32_t argBytes = 0;
			for (const WASM::StorageType& parameter : calledType.params)
				argBytes += parameter.val.opcode == WASM::ValueTypeCode::V128 ? 16u : 8u;
			asmjit::x86::Mem frameStorage = compiler.new_stack(
					frameCallStorageBytes(calledType), 16, "refCallFrame");
			asmjit::x86::Gp frameAddress = compiler.new_gp_ptr("refCallFrame");
			emitter.lea(frameAddress, frameStorage);
			uint32_t offset = 0;
			for (std::size_t i = 0; i < arguments.size(); ++i) {
				if (calledType.params[i].val.opcode == WASM::ValueTypeCode::V128) {
					for (int word = 0; word < 2; ++word) {
						auto bits = compiler.new_gp64("refCallFrameWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(arguments[i], word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(frameAddress, offset + word * 8), bits);
					}
					offset += 16;
					continue;
				}
				emitter.mov(
						asmjit::x86::Mem(frameAddress, offset, arguments[i].size()),
						arguments[i]);
				offset += 8;
			}
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]),
					"AsmJit call_ref frame invocation failed");
			invoke->set_arg(0, calleeContext);
			invoke->set_arg(1, frameAddress);
			uint32_t resultOffset = offset;
			for (const WASM::StorageType& resultType : calledType.results) {
				if (resultType.val.opcode == WASM::ValueTypeCode::V128) {
					auto buffer = newVectorBuffer("callRefVectorResult");
					for (int word = 0; word < 2; ++word) {
						auto bits = compiler.new_gp64("callRefResultWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(frameAddress, resultOffset + word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(buffer, word * 8), bits);
					}
					values.push_back(buffer);
					resultOffset += 16;
				} else {
					auto result = newIntegerRegister(compiler, resultType.val, "callRefResult");
					emitter.mov(result,
							asmjit::x86::Mem(frameAddress, resultOffset, result.size()));
					// A reference result arrives owning one count (the callee hands
					// over the count its operand stack held).
					if (isReferenceKind(resultType.val.opcode))
						markReference(result);
					values.push_back(result);
					resultOffset += 8;
				}
			}
			emitPendingTrapReturn();
			emitPendingExceptionCheck();
			dropCallReferenceArguments(keptArguments);
			return;
		}
		auto preparedArguments = prepareCallArguments(calledType, arguments);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]),
				"AsmJit call_ref invocation emission failed");
		invoke->set_arg(0, calleeContext);
		setCallArguments(invoke, preparedArguments);
		emitCallResult(invoke, calledType, "callRefResult");
		emitPendingTrapReturn();
		emitPendingExceptionCheck();
		dropCallReferenceArguments(keptArguments);
	}

	void dispatchCallRef(WASM::TypeIdx typeIndex) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (inDeadCode())
			return;
		if (!calleeUsesFrameForm(typeIndex) && calledType.results.size() > 1)
			throw std::runtime_error("AsmJit register-form call_ref supports at most one result");
		if (!calleeUsesFrameForm(typeIndex)) {
			for (const auto& parameter : calledType.params)
				requireScalar(parameter.val, "call_ref parameters");
			for (const auto& result : calledType.results)
				requireScalar(result.val, "call_ref results");
		}
		if (values.size() < calledType.params.size() + 1)
			throw std::runtime_error("AsmJit call_ref operand stack underflow");
		auto callable = values.back();
		values.pop_back();
		std::vector<asmjit::x86::Gp> arguments(calledType.params.size());
		for (size_t i = calledType.params.size(); i > 0; --i) {
			arguments[i - 1] = values.back();
			values.pop_back();
		}
		emitNullRefCheck(callable);
		emitCallRefInvocation(callable, arguments, calledType, typeIndex);
	}

	void dispatchReturnCallRef(WASM::TypeIdx typeIndex) override
	{
		if (inDeadCode())
			return;
		armTerminated = true;
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		// Extra values below the callee arguments/reference are discarded by the
		// tail call.
		if (values.size() < calledType.params.size() + 1)
			throw std::runtime_error("AsmJit return_call_ref has too few arguments");
		auto callable = values.back();
		values.pop_back();
		std::vector<asmjit::x86::Gp> arguments(calledType.params.size());
		for (size_t i = calledType.params.size(); i > 0; --i) {
			arguments[i - 1] = values.back();
			values.pop_back();
		}
		emitNullRefCheck(callable);
		const bool refTailSupported = calleeUsesFrameForm(typeIndex)
				? WASM::tailCallFrameSupported(calledType)
				: WASM::tailCallSlotsSupported(calledType);
		if (WASM::tailCallTrampolinesEligible(module, frameByType) && refTailSupported) {
			auto thunk = compiler.new_gp_ptr("tailThunk");
			emitter.mov(thunk, asmjit::x86::ptr(callable,
					static_cast<int32_t>(offsetof(WASM::Callable, tailEntry))));
			auto fallback = compiler.new_label();
			emitter.j(fallback, asmjit::ujit::test_z(thunk, thunk));
			emitTailCallState(callable, arguments, calledType);
			checkError(compiler.bind(fallback), "AsmJit return_call_ref fallback label failed");
			emitCallRefInvocation(callable, arguments, calledType, typeIndex);
			dispatchReturn();
			return;
		}
		emitCallRefInvocation(callable, arguments, calledType, typeIndex);
		dispatchReturn();
	}

	void dispatchCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (inDeadCode())
			return;
		const ResolvedIndirect resolved = resolveIndirectTarget(typeIndex, tableIndex);
		emitIndirectInvocation(resolved, calledType, typeIndex);
	}

	void dispatchCall(WASM::FuncIdx index) override
	{
		if (inDeadCode())
			return;
		const size_t importedCount = module.importFunctions.size();
		const size_t totalCount = importedCount + module.internalFunctionTypeIndices.size();
		if (index >= totalCount)
			throw std::runtime_error("AsmJit call function index is out of range");

		uint32_t typeIndex = 0;
		uint64_t target = 0;
		asmjit::Label targetLabel;
		bool isInternal = index >= importedCount;
		if (isInternal) {
			const size_t internalIndex = static_cast<size_t>(index) - importedCount;
			typeIndex = module.internalFunctionTypeIndices[internalIndex];
			targetLabel = functions[internalIndex]->label();
		} else {
			typeIndex = module.importFunctions[index].typeIdx;
			if (index >= internals.importStorage.size()
					|| internals.importStorage[index].fnPtr == nullptr)
				throw std::runtime_error("AsmJit call references an unresolved function import");
			target = reinterpret_cast<uint64_t>(internals.importStorage[index].fnPtr);
		}

		if (typeIndex >= signatures.size() || !signatures[typeIndex])
			throw std::runtime_error("AsmJit call references an unavailable function signature");
		// The translated signature table is spelled in *this module's* form. When
		// the callee was compiled in the other one, its own spelling is translated
		// on demand -- the mixed-graph path, which is rare enough that a cached
		// second table would be wasted memory (docs/ABI.md).
		bool calleeFrameForm = isInternal
				? calleeUsesFrameForm(typeIndex)
				: WASM::callableUsesFrameForm(internals.importStorage[index]);
		// A native import is a host C function and always takes the register form;
		// resolve that **before** picking the signature, because the per-type table
		// spells the import's declared type in *its* form, which may be the frame
		// (`GAP-1`, `GR-8`). A wide native import is then refused by the register
		// checks below rather than entered through the wrong signature.
		if (!isInternal && !WASM::callableIsCompiled(internals.importStorage[index]))
			calleeFrameForm = false;
		std::unique_ptr<asmjit::FuncSignature> foreignFormSignature;
		// The table spells each type in *that type's* form, so compare the callee
		// with the table entry -- not with this function's own form, which differs
		// in a mixed module and would reuse a frame signature for a register call.
		if (calleeFrameForm != calleeUsesFrameForm(typeIndex))
			foreignFormSignature = std::make_unique<asmjit::FuncSignature>(
					TypeTranslator::translateFunctionType(
							module.funcTypeAt(typeIndex), calleeFrameForm));
		const asmjit::FuncSignature& calleeSignature = foreignFormSignature != nullptr
				? *foreignFormSignature : *signatures[typeIndex];
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (inDeadCode())
			return;
		if (!calleeFrameForm && calledType.results.size() > 1)
			throw std::runtime_error("AsmJit register-form calls support at most one result");
		if (!calleeFrameForm) {
			for (const auto& parameter : calledType.params)
				requireScalar(parameter.val, "call parameters");
			for (const auto& result : calledType.results)
				requireScalar(result.val, "call results");
		}
		if (values.size() < calledType.params.size())
			throw std::runtime_error("AsmJit call operand stack underflow");

		std::vector<asmjit::x86::Gp> arguments(calledType.params.size());
		for (size_t i = calledType.params.size(); i > 0; --i) {
			arguments[i - 1] = values.back();
			values.pop_back();
		}

		// Wasm-to-Wasm imports execute in the exporting instance's context;
		// native imports (null context) execute against the caller's context.
		// The context operand must be ready at the call site, so it is
		// materialised before the invoke node is appended.
		WASM::VMContext* importedCallee = isInternal ? nullptr
				: internals.importStorage[index].context;
		asmjit::x86::Gp importedContext;
		if (importedCallee != nullptr) {
			importedContext = compiler.new_gp_ptr("importedVmContext");
			emitter.mov(importedContext,
					reinterpret_cast<uint64_t>(importedCallee));
		}

		// A parameter is the caller's borrow: the counts the operand stack holds
		// for reference arguments must survive the invoke, so each one is parked
		// in its own stack slot here and released from there after the call.
		const std::vector<asmjit::x86::Mem> keptArguments =
				spillCallReferenceArguments(calledType, arguments);

		if (calleeFrameForm) {
			// A native import is a host C function with the typed register ABI, not a
			// staging-frame callee. It never reaches this branch -- `calleeFrameForm`
			// is false for a callable with no compiled entry -- so the register path
			// below handles it.
			if (!isInternal && importedCallee == nullptr) {
				for (const WASM::StorageType& parameter : calledType.params)
					if (parameter.val.opcode == WASM::ValueTypeCode::V128)
						throw std::runtime_error(
								"AsmJit frame ABI: a native import cannot take a v128 parameter");
				if (calledType.results.size() > 1
						|| (!calledType.results.empty()
							&& calledType.results.front().val.opcode == WASM::ValueTypeCode::V128))
					throw std::runtime_error(
							"AsmJit frame ABI: a native import cannot return a v128 or multiple results");
				const asmjit::FuncSignature nativeSignature =
						TypeTranslator::translateFunctionType(calledType, false);
				auto preparedArguments = prepareCallArguments(calledType, arguments);
				asmjit::InvokeNode* invoke = nullptr;
				checkError(compiler.invoke(asmjit::Out(invoke), target, nativeSignature),
						"AsmJit native-import invocation failed");
				invoke->set_arg(0, vmContext);
				setCallArguments(invoke, preparedArguments);
				emitCallResult(invoke, calledType, "nativeImportResult");
				retainNativeCallResult(calledType);
				emitPendingTrapReturn();
				emitPendingExceptionCheck();
				dropCallReferenceArguments(keptArguments);
				return;
			}
			// Staging-frame ABI (step 5d): the arguments are packed into a frame
			// built BEFORE the invoke node exists, and the results are read back
			// out of the same block, which starts where the arguments end.
			uint32_t argBytes = 0;
			for (const WASM::StorageType& p : calledType.params)
				argBytes += p.val.opcode == WASM::ValueTypeCode::V128 ? 16u : 8u;
			asmjit::x86::Mem frameStorage = compiler.new_stack(
					frameCallStorageBytes(calledType), 16, "callFrame");
			asmjit::x86::Gp frameAddress = compiler.new_gp_ptr("callFrame");
			emitter.lea(frameAddress, frameStorage);
			uint32_t offset = 0;
			for (size_t i = 0; i < arguments.size(); ++i) {
				const WASM::ValueType& declared = calledType.params[i].val;
				if (declared.opcode == WASM::ValueTypeCode::V128) {
					for (int word = 0; word < 2; ++word) {
						asmjit::x86::Gp bits = compiler.new_gp64("callFrameWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(arguments[i], word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(frameAddress, offset + word * 8), bits);
					}
					offset += 16;
					continue;
				}
				emitter.mov(
						asmjit::x86::Mem(frameAddress, offset, arguments[i].size()),
						arguments[i]);
				offset += 8;
			}
			asmjit::InvokeNode* frameInvoke = nullptr;
			const asmjit::Error frameError = isInternal
					? compiler.invoke(asmjit::Out(frameInvoke), targetLabel, calleeSignature)
					: compiler.invoke(asmjit::Out(frameInvoke), target, calleeSignature);
			checkError(frameError, "AsmJit frame-call emission failed");
			if (importedCallee == nullptr)
				frameInvoke->set_arg(0, vmContext);
			else
				frameInvoke->set_arg(0, importedContext);
			frameInvoke->set_arg(1, frameAddress);
			uint32_t resultOffset = offset;
			for (const WASM::StorageType& resultType : calledType.results) {
				if (resultType.val.opcode == WASM::ValueTypeCode::V128) {
					asmjit::x86::Gp buffer = newVectorBuffer("callVectorResult");
					for (int word = 0; word < 2; ++word) {
						asmjit::x86::Gp bits = compiler.new_gp64("callResultWord");
						emitter.mov(bits,
								asmjit::x86::qword_ptr(frameAddress, resultOffset + word * 8));
						emitter.mov(
								asmjit::x86::qword_ptr(buffer, word * 8), bits);
					}
					values.push_back(buffer);
					resultOffset += 16;
				} else {
					asmjit::x86::Gp resultRegister =
							newIntegerRegister(compiler, resultType.val, "callResult");
					emitter.mov(resultRegister,
							asmjit::x86::Mem(frameAddress, resultOffset, resultRegister.size()));
					// A reference result arrives owning one count.
					if (isReferenceKind(resultType.val.opcode))
						markReference(resultRegister);
					values.push_back(resultRegister);
					resultOffset += 8;
				}
			}
			emitPendingTrapReturn();
			emitPendingExceptionCheck();
			dropCallReferenceArguments(keptArguments);
			return;
		}

		auto preparedArguments = prepareCallArguments(calledType, arguments);
		asmjit::InvokeNode* invoke = nullptr;
		const asmjit::Error error = isInternal
				? compiler.invoke(asmjit::Out(invoke), targetLabel, calleeSignature)
				: compiler.invoke(asmjit::Out(invoke), target, calleeSignature);
		checkError(error, "AsmJit function-call emission failed");
		if (importedCallee == nullptr)
			invoke->set_arg(0, vmContext);
		else
			invoke->set_arg(0, importedContext);
		setCallArguments(invoke, preparedArguments);
		emitCallResult(invoke, calledType, "callResult");
		// A native import keeps its own count of a reference result, so the
		// operand stack takes one of its own; a Wasm callee hands its count over
		// and cloning would leave the stack holding two.
		if (!isInternal && importedCallee == nullptr)
			retainNativeCallResult(calledType);
		emitPendingTrapReturn();
		emitPendingExceptionCheck();
		dropCallReferenceArguments(keptArguments);
	}

	//! Names this function and instruction on a trap that a helper reported
	//! without a site (docs/DIAGNOSTICS.md DG-2). The core keeps the first
	//! annotation, so an outer generated frame never rewrites an inner one.
	void emitTrapSiteAnnotation()
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_trap_annotate), signature),
				"AsmJit trap-site annotation call failed");
		invoke->set_arg(0, asmjit::Imm(functionIndex));
		invoke->set_arg(1, asmjit::Imm(static_cast<uint32_t>(instructionOffset)));
		invoke->set_arg(2, asmjit::Imm(reinterpret_cast<uint64_t>(functionName)));
	}

	void emitPendingTrapReturn()
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* slotCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(slotCall),
				reinterpret_cast<uint64_t>(&WASM::wasm_trap_slot), signature),
				"AsmJit failed to emit post-call trap-slot lookup");
		auto slot = compiler.new_gp_ptr("postCallTrapSlot");
		slotCall->set_ret(0, slot);
		auto pending = compiler.new_gp32("postCallPending");
		emitter.mov(pending, asmjit::x86::dword_ptr(slot,
				static_cast<int32_t>(offsetof(WASM::TrapSlot, pending))));
		auto noTrap = compiler.new_label();
		emitter.j(noTrap, asmjit::ujit::test_z(pending, pending));
		emitTrapSiteAnnotation();
		// A trap leaves through the return below, so the frame's reference
		// locals give their counts back here instead of at an epilogue that
		// never runs (see `emitTrapLocalRelease`).
		emitTrapLocalRelease();
		if (frameMode || resultCount == 0) {
			// Frame mode returns void: the results live in the caller's frame, and
			// on a trap they are undefined, so there is nothing to return
			// (step 5d). Without this the post-call trap return disagrees with the
			// signature and finalisation fails with error 25 at EVERY call site.
			checkError(compiler.ret(), "AsmJit post-call void trap return failed");
		} else if (isFloatKind(resultType)) {
			auto zero = compiler.new_xmm("postCallTrapFloatResult");
			emitter.vectorZero(zero);
			checkError(compiler.ret(zero),
					"AsmJit post-call trap float result return failed");
		} else {
			auto zero = kindWidth(resultType) == 8
					? compiler.new_gp64("postCallTrapResult")
					: compiler.new_gp32("postCallTrapResult");
			emitter.xor_(zero, zero);
			checkError(compiler.ret(zero), "AsmJit post-call trap result return failed");
		}
		checkError(compiler.bind(noTrap), "AsmJit post-call continuation binding failed");
	}

	// ── Exceptions (docs/proposals/EXCEPTIONS.md EX-3/EXD1) ────────────────
	// A structured block on this backend carries at most one result, so a catch
	// routes at most one payload value; a multi-field tag still throws and
	// propagates, it just cannot be caught inside the function yet.

	WASM::FuncType tagPayloadType(WASM::TagIdx tagIdx)
	{
		const std::size_t imported = module.importTags.size();
		std::uint32_t typeIdx = 0;
		if (tagIdx < imported) {
			typeIdx = module.importTags[tagIdx].typeIdx;
		} else {
			const std::size_t local = tagIdx - imported;
			if (local >= module.tags.size())
				throw std::runtime_error("AsmJit throw names an out-of-range tag");
			typeIdx = module.tags[local].typeIdx;
		}
		if (typeIdx >= module.typeCount()
				|| module.typeView(typeIdx).kind() != WASM::CanonTypeKind::Func)
			throw std::runtime_error("AsmJit tag does not name a function type");
		return module.funcTypeAt(typeIdx);
	}

	WASM::TagId tagIdForClause(const WASM::CatchClause& clause)
	{
		if (clause.tagIdx >= internals.tagIds.size())
			throw std::runtime_error("AsmJit try_table catch names an out-of-range tag");
		return internals.tagIds[clause.tagIdx];
	}

	void emitZeroReturn()
	{
		// The zero return is the trap and exception epilogue: the frame exits
		// here without reaching `emitFunctionReturnValues`, so the reference
		// locals give their counts back first.
		emitTrapLocalRelease();
		if (frameMode || resultCount == 0 || bufferedVectorResult) {
			checkError(compiler.ret(), "AsmJit exception-path void return failed");
			return;
		}
		if (isFloatKind(resultType)) {
			auto zero = compiler.new_xmm("exceptionFloatResult");
			emitter.vectorZero(zero);
			checkError(compiler.ret(zero), "AsmJit exception-path float return failed");
			return;
		}
		asmjit::x86::Gp zero = kindWidth(resultType) == 8
				? compiler.new_gp64("exceptionResult") : compiler.new_gp32("exceptionResult");
		emitter.xor_(zero, zero);
		checkError(compiler.ret(zero), "AsmJit exception-path result return failed");
	}

	//! Writes a pending tail call and returns a typed zero.
	//! \param target Callable pointer to re-enter.
	//! \param args Argument values, already popped from the operand stack.
	void emitTailCallState(const asmjit::x86::Gp& target,
			const std::vector<asmjit::x86::Gp>& args, const WASM::FuncType& type)
	{
		asmjit::FuncSignature stateSignature(asmjit::CallConvId::kCDecl);
		stateSignature.set_ret(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* stateInvoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(stateInvoke),
				reinterpret_cast<uint64_t>(&WASM::currentTailCallState), stateSignature),
				"AsmJit tail-call state call emission failed");
		auto state = compiler.new_gp_ptr("tailState");
		stateInvoke->set_ret(0, state);
		emitter.mov(asmjit::x86::Mem(state,
				static_cast<int32_t>(offsetof(WASM::TailCallState, target)), 8),
				target);
		for (std::size_t i = 0; i < args.size(); ++i) {
			const int32_t offset = static_cast<int32_t>(offsetof(WASM::TailCallState, args)
					+ i * WASM::kTailCallSlotBytes);
			if (i < type.params.size() && !type.params[i].isPacked
					&& type.params[i].val.opcode == WASM::ValueTypeCode::V128) {
				auto vector = compiler.new_xmm("tailVector");
				emitter.vectorLoad128(vector, asmjit::x86::ptr(args[i]));
				emitter.vectorStore128(asmjit::x86::Mem(state, offset, 16), vector);
			} else {
				emitter.mov(asmjit::x86::Mem(state, offset, args[i].size()),
						args[i]);
			}
		}
		emitter.mov(asmjit::x86::Mem(state,
				static_cast<int32_t>(offsetof(WASM::TailCallState, pending)), 4),
				asmjit::Imm(1));
		emitZeroReturn();
	}

	void emitFrameReturnValue(const asmjit::x86::Gp& value)
	{
		// Staging-frame ABI: the result goes into the caller's result block and the
		// signature returns void. A scalar float becomes host-observable here, so
		// the deterministic profile canonicalises it first
		// (docs/proposals/PROFILES.md PR-3).
		const asmjit::x86::Gp stored = isFloatKind(resultType)
				? canonicalizeFloatBits(value, kindWidth(resultType)) : value;
		asmjit::x86::Gp destination = compiler.new_gp_ptr("frameResultAddress");
		emitter.lea(destination,
				asmjit::x86::Mem(framePointer, frameResultOffset));
		if (resultType == WASM::ValueTypeCode::V128) {
			for (int word = 0; word < 2; ++word) {
				auto bits = compiler.new_gp64("frameResultWord");
				emitter.mov(bits, asmjit::x86::qword_ptr(stored, word * 8));
				emitter.mov(
						asmjit::x86::qword_ptr(destination, word * 8), bits);
			}
		} else {
			emitter.mov(
					asmjit::x86::Mem(destination, 0, stored.size()), stored);
		}
		checkError(compiler.ret(), "AsmJit frame result return failed");
	}

	void emitUnreachablePlaceholders()
	{
		// The throw is a terminator. The body after it is polymorphic, so the
		// compile-time value stack is reset to the block's height and filled with
		// zero placeholders; the emitted instructions after the propagation are
		// dead (every path returned or branched).
		const std::size_t stackHeight = controls.empty() ? 0 : controls.back().stackHeight;
		const std::size_t placeholderCount = controls.empty()
				? resultCount : controls.back().resultCount;
		values.resize(stackHeight);
		for (std::size_t i = 0; i < placeholderCount; ++i) {
			auto zero = compiler.new_gp32("exceptionStackPlaceholder");
			emitter.xor_(zero, zero);
			values.push_back(zero);
		}
	}

	void emitPendingExceptionCheck()
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* call = nullptr;
		checkError(compiler.invoke(asmjit::Out(call),
				reinterpret_cast<uint64_t>(&::wasm_exn_pending), signature),
				"AsmJit failed to emit pending-exception lookup");
		auto exn = compiler.new_gp_ptr("pendingException");
		call->set_ret(0, exn);
		auto none = compiler.new_label();
		emitter.j(none, asmjit::ujit::test_z(exn, exn));
		emitter.mov(exceptionPtrSlot, exn);
		emitExceptionPropagation();
		checkError(compiler.bind(none), "AsmJit pending-exception continuation failed");
	}

	void emitExceptionPropagation()
	{
		auto exn = compiler.new_gp_ptr("routedException");
		emitter.mov(exn, exceptionPtrSlot);
		asmjit::FuncSignature tagSignature(asmjit::CallConvId::kCDecl);
		tagSignature.set_ret(asmjit::TypeId::kInt32);
		tagSignature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* tagCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(tagCall),
				reinterpret_cast<uint64_t>(&::wasm_exn_tag_id), tagSignature),
				"AsmJit failed to emit exception tag-id lookup");
		auto tag = compiler.new_gp32("routedExceptionTag");
		tagCall->set_arg(0, exn);
		tagCall->set_ret(0, tag);

		for (std::size_t frameIndex = controls.size(); frameIndex-- > 0; ) {
			ControlFrame& frame = controls[frameIndex];
			if (!frame.tryTable)
				continue;
			for (const WASM::CatchClause& clause : frame.catchClauses) {
				if (clause.labelIdx > frameIndex)
					throw std::runtime_error("AsmJit try_table catch label is out of range");
				const bool functionLabel = clause.labelIdx == frameIndex;
				ControlFrame* target = functionLabel
						? nullptr : &controls[frameIndex - 1 - clause.labelIdx];
				const bool catchAll = clause.kind == WASM::CatchKind::CatchAll
						|| clause.kind == WASM::CatchKind::CatchAllRef;
				const bool carryExn = clause.kind == WASM::CatchKind::CatchRef
						|| clause.kind == WASM::CatchKind::CatchAllRef;
				if (catchAll) {
					if (functionLabel)
						emitCatchReturn(WASM::FuncType{}, carryExn);
					else
						emitCatchBranch(*target, WASM::FuncType{}, carryExn);
					return;
				}
				const WASM::FuncType payload = tagPayloadType(clause.tagIdx);
				const WASM::TagId tagId = tagIdForClause(clause);
				auto next = compiler.new_label();
				emitter.j(next, asmjit::ujit::cmp_ne(tag, static_cast<uint32_t>(tagId)));
				if (functionLabel)
					emitCatchReturn(payload, carryExn);
				else
					emitCatchBranch(*target, payload, carryExn);
				checkError(compiler.bind(next), "AsmJit exception next-clause binding failed");
			}
		}

		// No handler in this function: the pending slot is already set, so return
		// zeroes and let the caller re-propagate it.
		emitZeroReturn();
	}

	void emitCatchReturn(const WASM::FuncType& payload, bool carryExn)
	{
		const std::size_t payloadCount = payload.params.size();
		const std::size_t carried = payloadCount + (carryExn ? 1u : 0u);
		if (carried != resultCount)
			throw std::runtime_error("AsmJit catch arity does not match the function label");

		// A caught exception is consumed. The return path mirrors the ordinary
		// function-label branch: clear the pending slot first, then materialise
		// the payload as the function results.
		asmjit::FuncSignature clearSignature(asmjit::CallConvId::kCDecl);
		clearSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* clear = nullptr;
		checkError(compiler.invoke(asmjit::Out(clear),
				reinterpret_cast<uint64_t>(&::wasm_exn_clear_pending), clearSignature),
				"AsmJit failed to emit pending-exception clear");
		auto exn = compiler.new_gp_ptr("caughtFunctionException");
		emitter.mov(exn, exceptionPtrSlot);
		const ExceptionPayloadLayout layout = payloadCount != 0
				? exceptionPayloadLayout(payload) : ExceptionPayloadLayout{};

		std::vector<asmjit::x86::Gp> caught;
		caught.reserve(carried);
		for (std::size_t i = 0; i < payloadCount; ++i) {
			const int width = kindWidth(payload.params[i].val.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error(
						"AsmJit cannot route this catch payload to a function result");
			asmjit::x86::Gp value = width == 8
					? compiler.new_gp64("caughtFunctionPayload")
					: compiler.new_gp32("caughtFunctionPayload");
			emitter.mov(value,
					asmjit::x86::Mem(exn, static_cast<int32_t>(layout.fields[i].offset),
							value.size()));
			caught.push_back(value);
		}
		if (carryExn)
			caught.push_back(exn);

		const std::vector<asmjit::x86::Gp> savedValues = values;
		values = std::move(caught);
		emitFunctionReturnValues();
		values = savedValues;
	}

	void emitCatchBranch(ControlFrame& target, const WASM::FuncType& payload, bool carryExn)
	{
		const std::size_t payloadCount = payload.params.size();
		const std::size_t carried = payloadCount + (carryExn ? 1u : 0u);
		const bool loopTarget = target.kind == ControlKind::Loop;
		const std::size_t targetValues = loopTarget ? target.paramCount : target.resultCount;
		if (targetValues != carried)
			throw std::runtime_error("AsmJit catch arity does not match its target label");

		// A caught exception is consumed: clearing first means the caller and the
		// embedder no longer observe it (EXD1). A zero-payload catch to a loop is
		// still a catch, so the clear also precedes the jump in that case.
		asmjit::FuncSignature clearSignature(asmjit::CallConvId::kCDecl);
		clearSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* clear = nullptr;
		checkError(compiler.invoke(asmjit::Out(clear),
				reinterpret_cast<uint64_t>(&::wasm_exn_clear_pending), clearSignature),
				"AsmJit failed to emit pending-exception clear");
		auto exn = compiler.new_gp_ptr("caughtException");
		emitter.mov(exn, exceptionPtrSlot);
		const ExceptionPayloadLayout layout = payloadCount != 0
				? exceptionPayloadLayout(payload) : ExceptionPayloadLayout{};
		for (std::size_t i = 0; i < payloadCount; ++i) {
			const WASM::ValueTypeCode code = payload.params[i].val.opcode;
			if (code == WASM::ValueTypeCode::V128) {
				if (!loopTarget)
					throw std::runtime_error("AsmJit cannot route a v128 catch payload to a block result");
				for (int word = 0; word < 2; ++word) {
					auto bits = compiler.new_gp64("caughtPayloadWord");
					emitter.mov(bits,
							asmjit::x86::qword_ptr(exn, static_cast<int32_t>(layout.fields[i].offset)
									+ word * 8));
					emitter.mov(
							asmjit::x86::qword_ptr(target.params[i], word * 8), bits);
				}
				continue;
			}
			const int width = kindWidth(code);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit cannot route this catch payload yet");
			asmjit::x86::Gp value = width == 8
					? compiler.new_gp64("caughtPayload")
					: compiler.new_gp32("caughtPayload");
			emitter.mov(value,
					asmjit::x86::Mem(exn, static_cast<int32_t>(layout.fields[i].offset),
							value.size()));
			if (loopTarget)
				emitter.mov(target.params[i], value);
			else
				emitter.mov(target.results[i], value);
		}
		if (carryExn) {
			if (loopTarget)
				emitter.mov(target.params[payloadCount], exn);
			else
				emitter.mov(target.results[payloadCount], exn);
		}
		// A catch is a branch to its target label. In particular, a block whose
		// body always throws has its *end* reached only through this edge; without
		// this flag the block's continuation is emitted as dead code.
		if (!loopTarget)
			target.endTargeted = true;
		emitter.jmp(loopTarget ? target.begin : target.end);
	}

// ── Memory atomics (docs/proposals/THREADS.md TH-1) ─────────────────────
// The bounds-checked address comes from the same helper as every scalar
// access; the atomicity and the alignment trap live in the shared
// `wasm_atomic_*` ABI, so a target without its own atomic lowering still
// executes these correctly.

asmjit::x86::Gp atomicValue64(const asmjit::x86::Gp& value, int valueWidth)
{
if (valueWidth == 8)
return value;
auto widened = compiler.new_gp64("atomicValue");
emitter.mov(widened.r32(), value.r32());
return widened;
}

void emitAtomicLoad(const WASM::MemArg& arg, uint32_t width, int valueWidth)
{
if (values.empty())
throw std::runtime_error("AsmJit atomic load operand stack underflow");
auto wasmAddress = values.back();
values.pop_back();
auto pointer = emitMemoryAddress(arg, width, wasmAddress);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_load), signature),
"AsmJit atomic load emission failed");
auto loaded = compiler.new_gp64("atomicLoaded");
invoke->set_arg(0, pointer);
invoke->set_arg(1, asmjit::Imm(width));
invoke->set_arg(2, asmjit::Imm(arg.ordering));
invoke->set_ret(0, loaded);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(loaded);
} else {
auto result = compiler.new_gp32("atomicLoadResult");
emitter.mov(result, loaded.r32());
values.push_back(result);
}
}

void emitAtomicStore(const WASM::MemArg& arg, uint32_t width, int valueWidth)
{
if (values.size() < 2)
throw std::runtime_error("AsmJit atomic store operand stack underflow");
auto value = values.back(); values.pop_back();
auto wasmAddress = values.back(); values.pop_back();
auto pointer = emitMemoryAddress(arg, width, wasmAddress);
auto value64 = atomicValue64(value, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kVoid);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_store), signature),
"AsmJit atomic store emission failed");
invoke->set_arg(0, pointer);
invoke->set_arg(1, value64);
invoke->set_arg(2, asmjit::Imm(width));
invoke->set_arg(3, asmjit::Imm(arg.ordering));
emitPendingTrapReturn();
}

void emitAtomicRmw(const WASM::MemArg& arg, uint32_t width, uint32_t op, int valueWidth)
{
if (values.size() < 2)
throw std::runtime_error("AsmJit atomic rmw operand stack underflow");
auto value = values.back(); values.pop_back();
auto wasmAddress = values.back(); values.pop_back();
auto pointer = emitMemoryAddress(arg, width, wasmAddress);
auto value64 = atomicValue64(value, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_rmw), signature),
"AsmJit atomic rmw emission failed");
auto old = compiler.new_gp64("atomicOldValue");
invoke->set_arg(0, pointer);
invoke->set_arg(1, value64);
invoke->set_arg(2, asmjit::Imm(width));
invoke->set_arg(3, asmjit::Imm(op));
invoke->set_arg(4, asmjit::Imm(arg.ordering));
invoke->set_ret(0, old);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(old);
} else {
auto result = compiler.new_gp32("atomicRmwResult");
emitter.mov(result, old.r32());
values.push_back(result);
}
}

void emitAtomicCmpxchg(const WASM::MemArg& arg, uint32_t width, int valueWidth)
{
if (values.size() < 3)
throw std::runtime_error("AsmJit atomic cmpxchg operand stack underflow");
auto replacement = values.back(); values.pop_back();
auto expected = values.back(); values.pop_back();
auto wasmAddress = values.back(); values.pop_back();
auto pointer = emitMemoryAddress(arg, width, wasmAddress);
auto expected64 = atomicValue64(expected, valueWidth);
auto replacement64 = atomicValue64(replacement, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_cmpxchg), signature),
"AsmJit atomic cmpxchg emission failed");
auto old = compiler.new_gp64("atomicCmpxchgResult");
invoke->set_arg(0, pointer);
invoke->set_arg(1, expected64);
invoke->set_arg(2, replacement64);
invoke->set_arg(3, asmjit::Imm(width));
invoke->set_arg(4, asmjit::Imm(arg.ordering));
invoke->set_ret(0, old);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(old);
} else {
auto result = compiler.new_gp32("atomicCmpxchgTruncated");
emitter.mov(result, old.r32());
values.push_back(result);
}
}

void dispatchMemoryAtomicNotify(WASM::MemArg arg) override
{
if (values.size() < 2)
throw std::runtime_error("AsmJit atomic notify operand stack underflow");
auto count = values.back(); values.pop_back();
auto wasmAddress = values.back(); values.pop_back();
auto pointer = emitMemoryAddress(arg, 4, wasmAddress);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_notify), signature),
"AsmJit atomic notify emission failed");
auto woken = compiler.new_gp32("atomicWoken");
invoke->set_arg(0, pointer);
invoke->set_arg(1, count);
invoke->set_ret(0, woken);
emitPendingTrapReturn();
values.push_back(woken);
}

void emitAtomicWait(const WASM::MemArg& arg, uint32_t width, int valueWidth)
{
if (values.size() < 3)
throw std::runtime_error("AsmJit atomic wait operand stack underflow");
auto timeout = values.back(); values.pop_back();
auto expected = values.back(); values.pop_back();
auto wasmAddress = values.back(); values.pop_back();
auto pointer = emitMemoryAddress(arg, width, wasmAddress);
auto expected64 = atomicValue64(expected, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_wait), signature),
"AsmJit atomic wait emission failed");
auto status = compiler.new_gp32("atomicWaitStatus");
invoke->set_arg(0, pointer);
invoke->set_arg(1, expected64);
invoke->set_arg(2, timeout);
invoke->set_arg(3, asmjit::Imm(width));
invoke->set_ret(0, status);
emitPendingTrapReturn();
values.push_back(status);
}

void dispatchMemoryAtomicWait32(WASM::MemArg arg) override
{ emitAtomicWait(arg, 4, 4); }
void dispatchMemoryAtomicWait64(WASM::MemArg arg) override
{ emitAtomicWait(arg, 8, 8); }

void dispatchAtomicFence(uint8_t ordering) override
{
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kVoid);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_fence), signature),
"AsmJit atomic fence emission failed");
invoke->set_arg(0, asmjit::Imm(ordering));
}

void dispatchAtomicPause() override
{
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kVoid);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_pause), signature),
"AsmJit atomic pause emission failed");
}

	void dispatchI32AtomicLoad(WASM::MemArg arg) override { emitAtomicLoad(arg, 4, 4); }
	void dispatchI64AtomicLoad(WASM::MemArg arg) override { emitAtomicLoad(arg, 8, 8); }
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override { emitAtomicLoad(arg, 1, 4); }
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override { emitAtomicLoad(arg, 2, 4); }
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override { emitAtomicLoad(arg, 1, 8); }
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override { emitAtomicLoad(arg, 2, 8); }
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override { emitAtomicLoad(arg, 4, 8); }
	void dispatchI32AtomicStore(WASM::MemArg arg) override { emitAtomicStore(arg, 4, 4); }
	void dispatchI64AtomicStore(WASM::MemArg arg) override { emitAtomicStore(arg, 8, 8); }
	void dispatchI32AtomicStore8(WASM::MemArg arg) override { emitAtomicStore(arg, 1, 4); }
	void dispatchI32AtomicStore16(WASM::MemArg arg) override { emitAtomicStore(arg, 2, 4); }
	void dispatchI64AtomicStore8(WASM::MemArg arg) override { emitAtomicStore(arg, 1, 8); }
	void dispatchI64AtomicStore16(WASM::MemArg arg) override { emitAtomicStore(arg, 2, 8); }
	void dispatchI64AtomicStore32(WASM::MemArg arg) override { emitAtomicStore(arg, 4, 8); }
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 0, 4); }
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 0, 8); }
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 0, 4); }
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 0, 4); }
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 0, 8); }
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 0, 8); }
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 0, 8); }
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 1, 4); }
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 1, 8); }
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 1, 4); }
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 1, 4); }
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 1, 8); }
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 1, 8); }
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 1, 8); }
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 2, 4); }
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 2, 8); }
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 2, 4); }
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 2, 4); }
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 2, 8); }
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 2, 8); }
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 2, 8); }
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 3, 4); }
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 3, 8); }
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 3, 4); }
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 3, 4); }
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 3, 8); }
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 3, 8); }
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 3, 8); }
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 4, 4); }
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 4, 8); }
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 4, 4); }
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 4, 4); }
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 4, 8); }
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 4, 8); }
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 4, 8); }
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 5, 4); }
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override { emitAtomicRmw(arg, 8, 5, 8); }
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 5, 4); }
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 5, 4); }
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override { emitAtomicRmw(arg, 1, 5, 8); }
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override { emitAtomicRmw(arg, 2, 5, 8); }
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override { emitAtomicRmw(arg, 4, 5, 8); }
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 4, 4); }
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 8, 8); }
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 1, 4); }
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 2, 4); }
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 1, 8); }
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 2, 8); }
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override { emitAtomicCmpxchg(arg, 4, 8); }


// ── Shared-everything atomics: numeric slots ───────────────────────────────
// The address comes from the same object/global machinery as the non-atomic
// path; the atomicity and ordering live in the shared helper ABI. Reference
// slots need the GC barrier and are refused below, as are table slots.

void emitSharedAtomicLoad(uint8_t ordering, uint32_t width, int valueWidth,
const asmjit::x86::Gp& address, int signBits = 0)
{
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_load), signature),
"AsmJit shared atomic load emission failed");
auto loaded = compiler.new_gp64("sharedAtomicLoaded");
invoke->set_arg(0, address);
invoke->set_arg(1, asmjit::Imm(width));
invoke->set_arg(2, asmjit::Imm(ordering));
invoke->set_ret(0, loaded);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(loaded);
return;
}
auto result = compiler.new_gp32("sharedAtomicResult");
emitter.mov(result, loaded.r32());
if (signBits == 8) {
auto signExtended = compiler.new_gp32("sharedAtomicSign");
emitter.movsx8(signExtended, result);
result = signExtended;
} else if (signBits == 16) {
auto signExtended = compiler.new_gp32("sharedAtomicSign");
emitter.movsx16(signExtended, result);
result = signExtended;
}
values.push_back(result);
}

void emitSharedAtomicStore(uint8_t ordering, uint32_t width, int valueWidth,
const asmjit::x86::Gp& address, const asmjit::x86::Gp& value)
{
auto value64 = atomicValue64(value, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kVoid);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_store), signature),
"AsmJit shared atomic store emission failed");
invoke->set_arg(0, address);
invoke->set_arg(1, value64);
invoke->set_arg(2, asmjit::Imm(width));
invoke->set_arg(3, asmjit::Imm(ordering));
emitPendingTrapReturn();
}

void emitSharedAtomicRmw(uint8_t ordering, uint32_t width, uint32_t op, int valueWidth,
const asmjit::x86::Gp& address, const asmjit::x86::Gp& value)
{
auto value64 = atomicValue64(value, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_rmw), signature),
"AsmJit shared atomic rmw emission failed");
auto old = compiler.new_gp64("sharedAtomicOld");
invoke->set_arg(0, address);
invoke->set_arg(1, value64);
invoke->set_arg(2, asmjit::Imm(width));
invoke->set_arg(3, asmjit::Imm(op));
invoke->set_arg(4, asmjit::Imm(ordering));
invoke->set_ret(0, old);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(old);
} else {
auto result = compiler.new_gp32("sharedAtomicRmwResult");
emitter.mov(result, old.r32());
values.push_back(result);
}
}

void emitSharedAtomicRefStore(uint8_t ordering, const asmjit::x86::Gp& address,
        const asmjit::x86::Gp& value)
{
    asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
    signature.set_ret(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUInt32);
    asmjit::InvokeNode* invoke = nullptr;
    checkError(compiler.invoke(asmjit::Out(invoke),
            reinterpret_cast<uint64_t>(&::wasm_atomic_ref_store), signature),
            "AsmJit shared atomic reference store emission failed");
    invoke->set_arg(0, vmContext);
    invoke->set_arg(1, address);
    invoke->set_arg(2, value);
    invoke->set_arg(3, asmjit::Imm(ordering));
    emitPendingTrapReturn();
}

void emitSharedAtomicRefXchg(uint8_t ordering, const asmjit::x86::Gp& address,
        const asmjit::x86::Gp& value)
{
    asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
    signature.set_ret(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUInt32);
    asmjit::InvokeNode* invoke = nullptr;
    checkError(compiler.invoke(asmjit::Out(invoke),
            reinterpret_cast<uint64_t>(&::wasm_atomic_ref_xchg), signature),
            "AsmJit shared atomic reference xchg emission failed");
    invoke->set_arg(0, vmContext);
    invoke->set_arg(1, address);
    invoke->set_arg(2, value);
    invoke->set_arg(3, asmjit::Imm(ordering));
    auto old = compiler.new_gp_ptr("sharedAtomicRefOld");
    invoke->set_ret(0, old);
    emitPendingTrapReturn();
    values.push_back(old);
}

void emitSharedAtomicRefCmpxchg(uint8_t ordering, const asmjit::x86::Gp& address,
        const asmjit::x86::Gp& expected, const asmjit::x86::Gp& replacement)
{
    asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
    signature.set_ret(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUIntPtr);
    signature.add_arg(asmjit::TypeId::kUInt32);
    asmjit::InvokeNode* invoke = nullptr;
    checkError(compiler.invoke(asmjit::Out(invoke),
            reinterpret_cast<uint64_t>(&::wasm_atomic_ref_cmpxchg), signature),
            "AsmJit shared atomic reference cmpxchg emission failed");
    invoke->set_arg(0, vmContext);
    invoke->set_arg(1, address);
    invoke->set_arg(2, expected);
    invoke->set_arg(3, replacement);
    invoke->set_arg(4, asmjit::Imm(ordering));
    auto old = compiler.new_gp_ptr("sharedAtomicRefCmpOld");
    invoke->set_ret(0, old);
    emitPendingTrapReturn();
    values.push_back(old);
}

void emitSharedAtomicCmpxchg(uint8_t ordering, uint32_t width, int valueWidth,
const asmjit::x86::Gp& address, const asmjit::x86::Gp& expected,
const asmjit::x86::Gp& replacement)
{
auto expected64 = atomicValue64(expected, valueWidth);
auto replacement64 = atomicValue64(replacement, valueWidth);
asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
signature.set_ret(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUIntPtr);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt64);
signature.add_arg(asmjit::TypeId::kUInt32);
signature.add_arg(asmjit::TypeId::kUInt32);
asmjit::InvokeNode* invoke = nullptr;
checkError(compiler.invoke(asmjit::Out(invoke),
reinterpret_cast<uint64_t>(&::wasm_atomic_cmpxchg), signature),
"AsmJit shared atomic cmpxchg emission failed");
auto old = compiler.new_gp64("sharedAtomicCmpxchgResult");
invoke->set_arg(0, address);
invoke->set_arg(1, expected64);
invoke->set_arg(2, replacement64);
invoke->set_arg(3, asmjit::Imm(width));
invoke->set_arg(4, asmjit::Imm(ordering));
invoke->set_ret(0, old);
emitPendingTrapReturn();
if (valueWidth == 8) {
values.push_back(old);
} else {
auto result = compiler.new_gp32("sharedAtomicCmpxchgTruncated");
emitter.mov(result, old.r32());
values.push_back(result);
}
}

asmjit::x86::Gp emitStructAtomicAddress(const asmjit::x86::Gp& ref, uint32_t fieldOffset)
{
auto notNull = compiler.new_label();
emitter.j(notNull, asmjit::ujit::test_nz(ref, ref));
asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
reportSignature.set_ret(asmjit::TypeId::kVoid);
asmjit::InvokeNode* report = nullptr;
checkError(compiler.invoke(asmjit::Out(report),
reinterpret_cast<uint64_t>(&asmjitReportNullStructure), reportSignature),
"AsmJit shared struct null trap emission failed");
emitZeroReturn();
checkError(compiler.bind(notNull), "AsmJit shared struct null continuation failed");
auto address = compiler.new_gp_ptr("sharedStructAddress");
emitter.mov(address, ref);
emitter.add(address, asmjit::Imm(fieldOffset));
return address;
}

asmjit::x86::Gp emitArrayAtomicAddress(const asmjit::x86::Gp& ref,
const asmjit::x86::Gp& index, uint32_t lengthOffset, uint32_t dataOffset,
uint32_t elementBytes)
{
auto notNull = compiler.new_label();
emitter.j(notNull, asmjit::ujit::test_nz(ref, ref));
asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
nullSignature.set_ret(asmjit::TypeId::kVoid);
asmjit::InvokeNode* nullReport = nullptr;
checkError(compiler.invoke(asmjit::Out(nullReport),
reinterpret_cast<uint64_t>(&asmjitReportNullArray), nullSignature),
"AsmJit shared array null trap emission failed");
emitZeroReturn();
checkError(compiler.bind(notNull), "AsmJit shared array null continuation failed");

auto length = compiler.new_gp32("sharedArrayLength");
emitter.headerLoad32(length, asmjit::x86::dword_ptr(ref, lengthOffset));
auto inBounds = compiler.new_label();
emitter.j(inBounds, asmjit::ujit::ucmp_lt(index, length));
asmjit::FuncSignature oobSignature(asmjit::CallConvId::kCDecl);
oobSignature.set_ret(asmjit::TypeId::kVoid);
asmjit::InvokeNode* oobReport = nullptr;
checkError(compiler.invoke(asmjit::Out(oobReport),
reinterpret_cast<uint64_t>(&asmjitReportArrayOob), oobSignature),
"AsmJit shared array bounds trap emission failed");
emitZeroReturn();
checkError(compiler.bind(inBounds), "AsmJit shared array bounds continuation failed");

auto offset = compiler.new_gp64("sharedArrayOffset");
emitter.mov(offset.r32(), index.r32());
if (elementBytes != 1) {
int shift = 0;
for (uint32_t bytes = elementBytes; bytes > 1; bytes >>= 1)
++shift;
emitter.shl(offset, shift);
}
auto address = compiler.new_gp_ptr("sharedArrayAddress");
emitter.mov(address, ref);
emitter.add(address, offset);
emitter.add(address, asmjit::Imm(dataOffset));
return address;
}

	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicLoad(ordering, width, valueWidth, cell);
	}

	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.set references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.set operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		if (WASM::isReferenceType(type.contentType.opcode)) {
emitSharedAtomicRefStore(ordering, cell, value);
return;
}
emitSharedAtomicStore(ordering, width, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		if (WASM::isReferenceType(type.contentType.opcode)) {
emitSharedAtomicRefXchg(ordering, cell, value);
return;
}
emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, valueWidth, cell, value);
	}

	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override
	{
		const WASM::GlobalType& type = validateGlobalIndex(arg);
		const std::uint32_t width = sharedAtomicNumericWidth(type.contentType.opcode);
		if (width == 0)
			throw std::runtime_error("AsmJit shared atomic: v128 global slots are not atomic-addressable");
		const int valueWidth = width <= 4 ? 4 : 8;
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.atomic.rmw references an immutable global");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit global.atomic.rmw operand stack underflow");
		auto replacement = values.back(); values.pop_back();
		auto expected = values.back(); values.pop_back();
		auto cell = emitGlobalCell(arg);
		if (WASM::isReferenceType(type.contentType.opcode)) {
emitSharedAtomicRefCmpxchg(ordering, cell, expected, replacement);
return;
}
emitSharedAtomicCmpxchg(ordering, width, valueWidth, cell, expected, replacement);
	}

	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (values.empty())
			throw std::runtime_error("AsmJit struct.atomic.get operand stack underflow");
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, 0);
	}

	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!storage.isPacked)
			throw std::runtime_error("AsmJit struct.atomic.get_s requires a packed field");
		if (values.empty())
			throw std::runtime_error("AsmJit struct.atomic.get operand stack underflow");
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, width * 8);
	}

	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!storage.isPacked)
			throw std::runtime_error("AsmJit struct.atomic.get_u requires a packed field");
		if (values.empty())
			throw std::runtime_error("AsmJit struct.atomic.get operand stack underflow");
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, 0);
	}

	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.set operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
emitSharedAtomicRefStore(ordering, address, value);
return;
}
emitSharedAtomicStore(ordering, width, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
emitSharedAtomicRefXchg(ordering, address, value);
return;
}
emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, valueWidth, address, value);
	}

	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override
	{
	const WASM::CoreTypeView view = module.typeView(arg1);
	if (view.kind() != WASM::CanonTypeKind::Struct)
		throw std::runtime_error("AsmJit shared struct atomic requires a struct type");
	const std::span<const WASM::CanonFieldType> fields = view.structFields();
	if (arg2 >= fields.size())
		throw std::runtime_error("AsmJit shared struct atomic field index is out of range");
	const WASM::CanonFieldType& field = fields[arg2];
	const WASM::CanonStorageType& storage = field.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 field slots are not atomic-addressable");
	const uint32_t fieldOffset = static_cast<uint32_t>(WASM::gcStructFieldOffset(fields, arg2));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!field.isMutable)
			throw std::runtime_error("AsmJit struct.atomic operation on an immutable field");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit struct.atomic.rmw operand stack underflow");
		auto replacement = values.back(); values.pop_back();
		auto expected = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitStructAtomicAddress(ref, fieldOffset);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
emitSharedAtomicRefCmpxchg(ordering, address, expected, replacement);
return;
}
emitSharedAtomicCmpxchg(ordering, width, valueWidth, address, expected, replacement);
	}

	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.atomic.get operand stack underflow");
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, 0);
	}

	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!storage.isPacked)
			throw std::runtime_error("AsmJit array.atomic.get_s requires a packed element");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.atomic.get operand stack underflow");
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, width * 8);
	}

	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!storage.isPacked)
			throw std::runtime_error("AsmJit array.atomic.get_u requires a packed element");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.atomic.get operand stack underflow");
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicLoad(ordering, width, valueWidth, address, 0);
	}

	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.set operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
			emitSharedAtomicRefStore(ordering, address, value);
			return;
		}
		emitSharedAtomicStore(ordering, width, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_ADD, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_SUB, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_AND, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_OR, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XOR, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
			emitSharedAtomicRefXchg(ordering, address, value);
			return;
		}
		emitSharedAtomicRmw(ordering, width, WASM_ATOMIC_XCHG, valueWidth, address, value);
	}

	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override
	{
	const WASM::CoreTypeView view = module.typeView(arg);
	if (view.kind() != WASM::CanonTypeKind::Array)
		throw std::runtime_error("AsmJit shared array atomic requires an array type");
	const WASM::CanonFieldType& element = view.arrayElement();
	const WASM::CanonStorageType& storage = element.storage;
	std::uint32_t width = storage.isPacked
			? (storage.packed == WASM::ValueTypeCode::I8 ? 1u : 2u)
			: sharedAtomicNumericWidth(storage.value.opcode);
	if (width == 0)
		throw std::runtime_error("AsmJit shared atomic: v128 element slots are not atomic-addressable");
	const uint32_t lengthOffset = static_cast<uint32_t>(WASM::GcHeader::kArrayLengthOffset);
	const uint32_t dataOffset = static_cast<uint32_t>(WASM::gcArrayDataOffset(storage));
	const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
	const int valueWidth = width <= 4 ? 4 : 8;
		if (!element.isMutable)
			throw std::runtime_error("AsmJit array.atomic operation on an immutable array");
		if (values.size() < 4)
			throw std::runtime_error("AsmJit array.atomic.rmw operand stack underflow");
		auto replacement = values.back(); values.pop_back();
		auto expected = values.back(); values.pop_back();
		auto index = values.back(); values.pop_back();
		auto ref = values.back(); values.pop_back();
		auto address = emitArrayAtomicAddress(ref, index, lengthOffset, dataOffset, elementBytes);
		if (!storage.isPacked && WASM::isReferenceType(storage.value.opcode)) {
			emitSharedAtomicRefCmpxchg(ordering, address, expected, replacement);
			return;
		}
		emitSharedAtomicCmpxchg(ordering, width, valueWidth, address, expected, replacement);
	}

	void dispatchGlobalGet(WASM::GlobalIdx index) override { emitGlobalGet(index); }
	void dispatchGlobalSet(WASM::GlobalIdx index) override { emitGlobalSet(index); }

	void dispatchMemorySize(WASM::MemIdx index) override { emitMemorySize(index); }
	void dispatchMemoryGrow(WASM::MemIdx index) override { emitMemoryGrow(index); }

    void dispatchMemoryCopy(WASM::MemIdx destination, WASM::MemIdx source) override
    {
        emitMemoryCopy(destination, source);
    }

    void dispatchMemoryFill(WASM::MemIdx index) override
    {
        emitMemoryFill(index);
    }

    void dispatchMemoryInit(uint32_t dataIndex, WASM::MemIdx memoryIndex) override
    {
        emitMemoryInit(dataIndex, memoryIndex);
    }

    void dispatchDataDrop(uint32_t dataIndex) override
    {
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        auto index = compiler.new_gp32("dataDropIndex");
        emitter.mov(index, dataIndex);
        asmjit::InvokeNode* invoke = nullptr;
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitDataDrop), signature),
                "AsmJit data.drop helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, index);
        emitPendingTrapReturn();
    }




    void dispatchTableInit(uint32_t elementIndex, WASM::TableIdx tableIndex) override
    {
        if (static_cast<size_t>(tableIndex) >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.init table index is out of range");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit table.init operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto source = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        for (int i = 0; i < 6; ++i)
            signature.add_arg(i == 0 ? asmjit::TypeId::kUIntPtr : asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto element = compiler.new_gp32("tableInitElementIndex");
        auto table = compiler.new_gp32("tableInitTableIndex");
        emitter.mov(element, elementIndex);
        emitter.mov(table, static_cast<uint32_t>(tableIndex));
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitTableInit), signature),
                "AsmJit table.init helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, element);
        invoke->set_arg(2, table);
        invoke->set_arg(3, destination);
        invoke->set_arg(4, source);
        invoke->set_arg(5, length);
        emitPendingTrapReturn();
    }

    void dispatchElemDrop(uint32_t elementIndex) override
    {
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto element = compiler.new_gp32("elementDropIndex");
        emitter.mov(element, elementIndex);
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitElemDrop), signature),
                "AsmJit elem.drop helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, element);
        emitPendingTrapReturn();
    }




    void dispatchTableFill(WASM::TableIdx tableIndex) override
    {
        if (tableIndex >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.fill references an unknown table");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit table.fill operand stack underflow");
        auto length = values.back(); values.pop_back();
        auto element = values.back(); values.pop_back();
        auto start = values.back(); values.pop_back();
        auto widen = [&](const asmjit::x86::Gp& value) {
            if (value.size() == 8)
                return value;
            auto wide = compiler.new_gp64("tableFillWord");
            emitter.movzx32(wide, value);
            return wide;
        };
        auto packed = compiler.new_stack(48, 16, "tableFillArguments");
        auto packedPtr = compiler.new_gp_ptr("tableFillArgumentsPointer");
        emitter.lea(packedPtr, packed);
        const auto packedAt = [&](int offset) {
            auto slot = packed;
            slot.set_offset(offset);
            slot.set_size(8);
            return slot;
        };
        emitter.mov(packedAt(0), widen(start));
        emitter.mov(packedAt(16), element);
        emitter.mov(packedAt(32), widen(length));
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        auto tableReg = compiler.new_gp32("tableFillIndex");
        emitter.mov(tableReg, static_cast<uint32_t>(tableIndex));
        asmjit::InvokeNode* invoke = nullptr;
        checkError(compiler.invoke(asmjit::Out(invoke),
                reinterpret_cast<uint64_t>(&WASM::wasm_table_fill_args), signature),
                "AsmJit table.fill helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, tableReg);
        invoke->set_arg(2, packedPtr);
        emitPendingTrapReturn();
    }

    void dispatchTableCopy(WASM::TableIdx destinationTable, WASM::TableIdx sourceTable) override
    {
        if (static_cast<size_t>(destinationTable) >= module.importTables.size() + module.tables.size()
                || static_cast<size_t>(sourceTable) >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.copy table index is out of range");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit table.copy operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto source = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        for (int i = 0; i < 6; ++i)
            signature.add_arg(i == 0 ? asmjit::TypeId::kUIntPtr : asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto dstTable = compiler.new_gp32("tableCopyDestinationTable");
        auto srcTable = compiler.new_gp32("tableCopySourceTable");
        emitter.mov(dstTable, static_cast<uint32_t>(destinationTable));
        emitter.mov(srcTable, static_cast<uint32_t>(sourceTable));
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitTableCopy), signature),
                "AsmJit table.copy helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, dstTable);
        invoke->set_arg(2, srcTable);
        invoke->set_arg(3, destination);
        invoke->set_arg(4, source);
        invoke->set_arg(5, length);
        emitPendingTrapReturn();
    }

    void dispatchTableGrow(WASM::TableIdx tableIndex) override
    {
        if (static_cast<size_t>(tableIndex) >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.grow table index is out of range");
        if (values.size() < 2)
            throw std::runtime_error("AsmJit table.grow operand stack underflow");
        auto delta = values.back(); values.pop_back();
        auto init = values.back(); values.pop_back();
        const bool table64 = tableIndex < module.importTables.size()
                ? (module.importTables[tableIndex].table.limits.flags & 0x04) != 0
                : (module.tables[tableIndex - module.importTables.size()].limits.flags & 0x04) != 0;
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kUInt64);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt64);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        auto index = compiler.new_gp32("tableGrowIndex");
        emitter.mov(index, static_cast<uint32_t>(tableIndex));
        // The core takes a 64-bit delta. For a 32-bit table, zero-extend the
        // i32 operand; a table64 operand is already 64-bit.
        asmjit::x86::Gp delta64 = delta;
        if (delta.size() != 8) {
            delta64 = compiler.new_gp64("tableGrowDelta");
            emitter.mov(delta64.r32(), delta.r32());
        }
        asmjit::InvokeNode* invoke = nullptr;
        checkError(compiler.invoke(asmjit::Out(invoke),
                reinterpret_cast<uint64_t>(&WASM::wasm_table_grow), signature),
                "AsmJit table.grow helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, index);
        invoke->set_arg(2, delta64);
        invoke->set_arg(3, init);
        auto result = compiler.new_gp64("tableGrowResult");
        invoke->set_ret(0, result);
        values.push_back(table64 ? result : result.r32());
        emitPendingTrapReturn();
    }

    void dispatchTableSize(WASM::TableIdx tableIndex) override
    {
        if (static_cast<size_t>(tableIndex) >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.size table index is out of range");
        const bool table64 = tableIndex < module.importTables.size()
                ? (module.importTables[tableIndex].table.limits.flags & 0x04) != 0
                : (module.tables[tableIndex - module.importTables.size()].limits.flags & 0x04) != 0;
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(table64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto index = compiler.new_gp32("tableSizeIndex");
        emitter.mov(index, static_cast<uint32_t>(tableIndex));
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitTableSize), signature),
                "AsmJit table.size helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, index);
        auto size = table64 ? compiler.new_gp64("tableSizeResult")
                : compiler.new_gp32("tableSizeResult");
        invoke->set_ret(0, size);
        values.push_back(size);
        emitPendingTrapReturn();
    }


	// ── v128 (first SIMD slice) ─────────────────────────────────────────────
	// Every operation calls the core's shared helper ABI; the backend only
	// transports 16-byte buffers.

	void dispatchV128Const(std::span<uint8_t> arg) override
	{
		if (arg.size() != 16)
			throw std::runtime_error("AsmJit v128.const payload is not 16 bytes");
		uint64_t lo = 0;
		uint64_t hi = 0;
		std::memcpy(&lo, arg.data(), sizeof(lo));
		std::memcpy(&hi, arg.data() + sizeof(lo), sizeof(hi));
		auto buffer = newVectorBuffer("v128Constant");
		// A 64-bit immediate needs a register: `mov r/m64, imm64` does not exist on
		// x86, so storing a qword immediate straight into memory is lossy.
		auto low = compiler.new_gp64("v128ConstLow");
		emitter.mov(low, lo);
		emitter.mov(asmjit::x86::qword_ptr(buffer, 0), low);
		auto high = compiler.new_gp64("v128ConstHigh");
		emitter.mov(high, hi);
		emitter.mov(asmjit::x86::qword_ptr(buffer, 8), high);
		values.push_back(buffer);
	}

	void dispatchV128Load(WASM::MemArg arg) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128.load address underflow");
		auto address = values.back();
		values.pop_back();
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array v128.load reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, 16, reference, address);
		} else {
			pointer = emitMemoryAddress(arg, 16, address);
		}
		auto buffer = newVectorBuffer("v128Load");
		copyVector(buffer, pointer);
		values.push_back(buffer);
	}

	void dispatchV128Store(WASM::MemArg arg) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128.store operand underflow");
		auto value = values.back();
		values.pop_back();
		auto address = values.back();
		values.pop_back();
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array v128.store reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, 16, reference, address);
		} else {
			pointer = emitMemoryAddress(arg, 16, address);
		}
		copyVector(pointer, value);
	}

	void emitVectorSplatInteger(uint64_t helper, int width, const char* name)
	{
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + name + " operand underflow");
		auto scalar = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("vectorSplat");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(width == 8 ? asmjit::TypeId::kInt64 : asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				(std::string("AsmJit ") + name + " helper call failed").c_str());
		invoke->set_arg(0, width == 8 ? asmjit::Reg(scalar.r64()) : asmjit::Reg(scalar.r32()));
		invoke->set_arg(1, buffer);
		values.push_back(buffer);
	}

	void dispatchI8x16Splat() override
	{ emitVectorSplatInteger(reinterpret_cast<uint64_t>(&wasm_i8x16_splat), 4, "i8x16.splat"); }
	void dispatchI16x8Splat() override
	{ emitVectorSplatInteger(reinterpret_cast<uint64_t>(&wasm_i16x8_splat), 4, "i16x8.splat"); }
	void dispatchI64x2Splat() override
	{ emitVectorSplatInteger(reinterpret_cast<uint64_t>(&wasm_i64x2_splat), 8, "i64x2.splat"); }

	void dispatchI32x4Splat() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i32x4.splat operand underflow");
		auto scalar = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("i32x4Splat");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_i32x4_splat), signature),
				"AsmJit i32x4.splat helper call failed");
		invoke->set_arg(0, scalar);
		invoke->set_arg(1, buffer);
		values.push_back(buffer);
	}

	void emitVectorExtractLane(uint64_t helper, uint8_t lane, int width, const char* name)
	{
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + name + " operand underflow");
		auto vector = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(width == 8 ? asmjit::TypeId::kInt64 : asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				(std::string("AsmJit ") + name + " helper call failed").c_str());
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		auto result = width == 8
				? compiler.new_gp64("vectorLane") : compiler.new_gp32("vectorLane");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

	void emitVectorReplaceLane(uint64_t helper, uint8_t lane, int width,
			const char* name)
	{
		if (values.size() < 2)
			throw std::runtime_error(std::string("AsmJit ") + name + " operand underflow");
		auto scalar = values.back();
		values.pop_back();
		auto vector = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("vectorReplace");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(width == 8 ? asmjit::TypeId::kInt64 : asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				(std::string("AsmJit ") + name + " helper call failed").c_str());
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		invoke->set_arg(2, width == 8 ? asmjit::Reg(scalar.r64()) : asmjit::Reg(scalar.r32()));
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	void dispatchI8x16ExtractLaneS(uint8_t lane) override
	{ emitVectorExtractLane(reinterpret_cast<uint64_t>(&wasm_i8x16_extract_lane_s), lane, 4, "i8x16.extract_lane_s"); }
	void dispatchI8x16ExtractLaneU(uint8_t lane) override
	{ emitVectorExtractLane(reinterpret_cast<uint64_t>(&wasm_i8x16_extract_lane_u), lane, 4, "i8x16.extract_lane_u"); }
	void dispatchI16x8ExtractLaneS(uint8_t lane) override
	{ emitVectorExtractLane(reinterpret_cast<uint64_t>(&wasm_i16x8_extract_lane_s), lane, 4, "i16x8.extract_lane_s"); }
	void dispatchI16x8ExtractLaneU(uint8_t lane) override
	{ emitVectorExtractLane(reinterpret_cast<uint64_t>(&wasm_i16x8_extract_lane_u), lane, 4, "i16x8.extract_lane_u"); }
	void dispatchI64x2ExtractLane(uint8_t lane) override
	{ emitVectorExtractLane(reinterpret_cast<uint64_t>(&wasm_i64x2_extract_lane), lane, 8, "i64x2.extract_lane"); }
	void dispatchI8x16ReplaceLane(uint8_t lane) override
	{ emitVectorReplaceLane(reinterpret_cast<uint64_t>(&wasm_i8x16_replace_lane), lane, 4, "i8x16.replace_lane"); }
	void dispatchI16x8ReplaceLane(uint8_t lane) override
	{ emitVectorReplaceLane(reinterpret_cast<uint64_t>(&wasm_i16x8_replace_lane), lane, 4, "i16x8.replace_lane"); }
	void dispatchI64x2ReplaceLane(uint8_t lane) override
	{ emitVectorReplaceLane(reinterpret_cast<uint64_t>(&wasm_i64x2_replace_lane), lane, 8, "i64x2.replace_lane"); }

	void dispatchI32x4ExtractLane(uint8_t lane) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i32x4.extract_lane operand underflow");
		auto vector = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_i32x4_extract_lane), signature),
				"AsmJit i32x4.extract_lane helper call failed");
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		auto result = compiler.new_gp32("i32x4Lane");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

	void dispatchI32x4ReplaceLane(uint8_t lane) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32x4.replace_lane operand underflow");
		auto scalar = values.back();
		values.pop_back();
		auto vector = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("i32x4Replace");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_i32x4_replace_lane), signature),
				"AsmJit i32x4.replace_lane helper call failed");
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		invoke->set_arg(2, scalar);
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	void dispatchV128Not() override { emitVectorHelper1(&wasm_v128_not); }
	void dispatchV128And() override { emitVectorBinaryNative("AsmJit v128.and failed", 0); }
	void dispatchV128AndNot() override { emitVectorBinaryNative("AsmJit v128.andnot failed", 1); }
	void dispatchV128Or() override { emitVectorBinaryNative("AsmJit v128.or failed", 2); }
	void dispatchV128Xor() override { emitVectorBinaryNative("AsmJit v128.xor failed", 3); }

	void dispatchV128Bitselect() override
	{
		if (values.size() < 3)
			throw std::runtime_error("AsmJit v128.bitselect operand underflow");
		asmjit::x86::Gp c = values.back();
		values.pop_back();
		asmjit::x86::Gp b = values.back();
		values.pop_back();
		asmjit::x86::Gp a = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128Bitselect");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		for (int i = 0; i < 4; ++i)
			signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_v128_bitselect), signature),
				"AsmJit v128.bitselect helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, b);
		invoke->set_arg(2, c);
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	void dispatchV128AnyTrue() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128.any_true operand underflow");
		auto vector = values.back();
		values.pop_back();
		auto low = compiler.new_gp64("v128AnyLow");
		auto high = compiler.new_gp64("v128AnyHigh");
		emitter.mov(low, asmjit::x86::qword_ptr(vector, 0));
		emitter.mov(high, asmjit::x86::qword_ptr(vector, 8));
		emitter.or_(low, high);
		auto result = compiler.new_gp32("v128AnyResult");
		// One universal `select` replaces the zero/test/setcc triple: it defines
		// the result on both paths.
		emitter.selectFlag(result, low, asmjit::ujit::test_nz(low, low));
		values.push_back(result);
	}

	//! Packs a constant high word and a register low word into one 64-bit value.
	asmjit::x86::Gp packImmPair(uint32_t high, uint32_t low, const char* name)
	{
		auto packed = compiler.new_gp64(name);
		emitter.mov(packed, asmjit::Imm(
				(static_cast<uint64_t>(high) << 32) | low));
		return packed;
	}

	//! Packs two 32-bit registers into one 64-bit value, high word from `high`.
	asmjit::x86::Gp packRegPair(const asmjit::x86::Gp& high,
			const asmjit::x86::Gp& low, const char* name)
	{
		auto packed = compiler.new_gp64(name);
		emitter.mov(packed.r32(), low.r32());
		auto highWide = compiler.new_gp64("packedHigh");
		emitter.mov(highWide.r32(), high.r32());
		emitter.shl(highWide, 32);
		emitter.or_(packed, highWide);
		return packed;
	}

	//! Calls a unary v128 helper (`a`, `out`) and pushes `out`.
	void emitVectorHelper1(void (*helper)(const void*, void*))
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128 helper operand underflow");
		auto a = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128Result");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit v128 helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, buffer);
		values.push_back(buffer);
	}

	//! Calls a binary v128 helper (`a`, `b`, `out`) and pushes `out`.
	//! Native 16-byte binary op on two buffers, no helper call.
	//! Kinds: 0 and, 1 andnot (a & ~b), 2 or, 3 xor, 4..7 `paddb/w/d/q`,
	//! 8..11 `psubb/w/d/q`. The result lands in a fresh buffer, exactly as
	//! `emitVectorHelper2` leaves it (`SIMD-8`).
	//! The portable helper each native kind falls back to (`WASMJIT_SIMD_NATIVE=0`).
	//! Whether the backend may use its own vector instructions (`SIMD-8`).
	//! `WASMJIT_SIMD_NATIVE=0` forces the portable helper path, for A/B runs.
	static bool nativeSimdEnabled()
	{
		static const bool enabled = [] {
			const char* value = std::getenv("WASMJIT_SIMD_NATIVE");
			return value == nullptr || std::string_view(value) != "0";
		}();
		return enabled;
	}

	static void (*helperForNativeKind(int kind))(const void*, const void*, void*)
	{
		// Every kind is listed explicitly and an unknown one returns null. A
		// `default:` that named *a* helper is how the second `SIMD-8` draft made
		// `i8x16.max_u` answer zero: a new kind silently fell through to the
		// wrong fallback. The caller refuses a null mapping instead.
		switch (kind) {
		case 0: return &wasm_v128_and;
		case 1: return &wasm_v128_andnot;
		case 2: return &wasm_v128_or;
		case 3: return &wasm_v128_xor;
		case 4: return &wasm_i8x16_add;
		case 5: return &wasm_i16x8_add;
		case 6: return &wasm_i32x4_add;
		case 7: return &wasm_i64x2_add;
		case 8: return &wasm_i8x16_sub;
		case 9: return &wasm_i16x8_sub;
		case 10: return &wasm_i32x4_sub;
		case 11: return &wasm_i64x2_sub;
		case 12: return &wasm_i8x16_avgr_u;
		case 13: return &wasm_i16x8_avgr_u;
		case 14: return &wasm_i8x16_min_u;
		case 15: return &wasm_i16x8_min_s;
		case 16: return &wasm_i8x16_max_u;
		case 17: return &wasm_i16x8_max_s;
		case 18: return &wasm_i8x16_eq;
		case 19: return &wasm_i16x8_eq;
		case 20: return &wasm_i32x4_eq;
		case 21: return &wasm_i8x16_ne;
		case 22: return &wasm_i16x8_ne;
		case 23: return &wasm_i32x4_ne;
		case 24: return &wasm_i8x16_lt_s;
		case 25: return &wasm_i16x8_lt_s;
		case 26: return &wasm_i32x4_lt_s;
		case 27: return &wasm_i8x16_gt_s;
		case 28: return &wasm_i16x8_gt_s;
		case 29: return &wasm_i32x4_gt_s;
		case 30: return &wasm_i8x16_le_s;
		case 31: return &wasm_i16x8_le_s;
		case 32: return &wasm_i32x4_le_s;
		case 33: return &wasm_i8x16_ge_s;
		case 34: return &wasm_i16x8_ge_s;
		case 35: return &wasm_i32x4_ge_s;
		default: return nullptr;
		}
	}

	//! Emits one native v128 binary operation.
	//!
	//! The description the dispatchers pass is kept at the call site as the
	//! statement of what the operation is; the universal operation names the
	//! lane width and signedness, so the emitter no longer needs it.
	void emitVectorBinaryNative(const char* /*description*/, int kind)
	{
		if (!nativeSimdEnabled())
		{
			auto fallback = helperForNativeKind(kind);
			if (fallback == nullptr)
				throw std::runtime_error("AsmJit v128 native kind has no fallback helper");
			emitVectorHelper2(fallback);
			return;
		}
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 binary operand underflow");
		auto b = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		auto result = newVectorBuffer("v128Result");
		auto lhs = compiler.new_xmm("v128NativeA");
		auto rhs = compiler.new_xmm("v128NativeB");
		emitter.vectorLoad128(lhs, asmjit::x86::xmmword_ptr(a));
		emitter.vectorLoad128(rhs, asmjit::x86::xmmword_ptr(b));
		// Every comparison result is a per-lane all-ones mask, exactly wasm's, and
		// the universal operation names the signedness, so neither the `pandn`
		// destination trick nor the `pcmpgt` operand swap survives the conversion.
		asmjit::x86::Vec out = lhs;
		auto invert = [&](const asmjit::x86::Vec& mask) {
			emitter.vectorNot(mask);
		};
		using Ops = asmjit::ujit::UniOpVVV;
		switch (kind) {
		case 0: emitter.vector3(Ops::kAndU32, lhs, lhs, rhs); break;
		case 1: emitter.vector3(Ops::kBicU32, lhs, lhs, rhs); break;
		case 2: emitter.vector3(Ops::kOrU32, lhs, lhs, rhs); break;
		case 3: emitter.vector3(Ops::kXorU32, lhs, lhs, rhs); break;
		case 4: emitter.vector3(Ops::kAddU8, lhs, lhs, rhs); break;
		case 5: emitter.vector3(Ops::kAddU16, lhs, lhs, rhs); break;
		case 6: emitter.vector3(Ops::kAddU32, lhs, lhs, rhs); break;
		case 7: emitter.vector3(Ops::kAddU64, lhs, lhs, rhs); break;
		case 8: emitter.vector3(Ops::kSubU8, lhs, lhs, rhs); break;
		case 9: emitter.vector3(Ops::kSubU16, lhs, lhs, rhs); break;
		case 10: emitter.vector3(Ops::kSubU32, lhs, lhs, rhs); break;
		case 11: emitter.vector3(Ops::kSubU64, lhs, lhs, rhs); break;
		case 12: emitter.vector3(Ops::kAvgrU8, lhs, lhs, rhs); break;
		case 13: emitter.vector3(Ops::kAvgrU16, lhs, lhs, rhs); break;
		case 14: emitter.vector3(Ops::kMinU8, lhs, lhs, rhs); break;
		case 15: emitter.vector3(Ops::kMinI16, lhs, lhs, rhs); break;
		case 16: emitter.vector3(Ops::kMaxU8, lhs, lhs, rhs); break;
		case 17: emitter.vector3(Ops::kMaxI16, lhs, lhs, rhs); break;
		case 18: emitter.vector3(Ops::kCmpEqU8, lhs, lhs, rhs); break;
		case 19: emitter.vector3(Ops::kCmpEqU16, lhs, lhs, rhs); break;
		case 20: emitter.vector3(Ops::kCmpEqU32, lhs, lhs, rhs); break;
		case 21: emitter.vector3(Ops::kCmpEqU8, lhs, lhs, rhs); invert(lhs); break;
		case 22: emitter.vector3(Ops::kCmpEqU16, lhs, lhs, rhs); invert(lhs); break;
		case 23: emitter.vector3(Ops::kCmpEqU32, lhs, lhs, rhs); invert(lhs); break;
		case 24: emitter.vector3(Ops::kCmpLtI8, lhs, lhs, rhs); break;
		case 25: emitter.vector3(Ops::kCmpLtI16, lhs, lhs, rhs); break;
		case 26: emitter.vector3(Ops::kCmpLtI32, lhs, lhs, rhs); break;
		case 27: emitter.vector3(Ops::kCmpGtI8, lhs, lhs, rhs); break;
		case 28: emitter.vector3(Ops::kCmpGtI16, lhs, lhs, rhs); break;
		case 29: emitter.vector3(Ops::kCmpGtI32, lhs, lhs, rhs); break;
		case 30: emitter.vector3(Ops::kCmpLeI8, lhs, lhs, rhs); break;
		case 31: emitter.vector3(Ops::kCmpLeI16, lhs, lhs, rhs); break;
		case 32: emitter.vector3(Ops::kCmpLeI32, lhs, lhs, rhs); break;
		case 33: emitter.vector3(Ops::kCmpGeI8, lhs, lhs, rhs); break;
		case 34: emitter.vector3(Ops::kCmpGeI16, lhs, lhs, rhs); break;
		case 35: emitter.vector3(Ops::kCmpGeI32, lhs, lhs, rhs); break;
		default: throw std::runtime_error("unknown AsmJit v128 native kind");
		}
		emitter.vectorStore128(asmjit::x86::xmmword_ptr(result), out);
		values.push_back(result);
	}

	void emitVectorHelper2(void (*helper)(const void*, const void*, void*))
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 helper operand underflow");
		auto b = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128Result");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit v128 helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, b);
		invoke->set_arg(2, buffer);
		values.push_back(buffer);
	}

	//! Calls a ternary v128 helper (`a`, `b`, `c`, `out`) and pushes `out`.
	void emitVectorHelper3(void (*helper)(const void*, const void*, const void*, void*))
	{
		if (values.size() < 3)
			throw std::runtime_error("AsmJit v128 helper operand underflow");
		auto c = values.back();
		values.pop_back();
		auto b = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128Result");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		for (int i = 0; i < 4; ++i)
			signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit v128 helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, b);
		invoke->set_arg(2, c);
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	//! Calls a v128 reduction helper and pushes its i32 result.
	void emitVectorReduce(int32_t (*helper)(const void*))
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128 reduction operand underflow");
		auto a = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit v128 reduction helper call failed");
		invoke->set_arg(0, a);
		auto result = compiler.new_gp32("v128Reduce");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

	//! Calls a shift helper over the top vector and its scalar count.
	void emitVectorShift(void (*helper)(const void*, int32_t, void*))
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 shift operand underflow");
		auto count = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128Shift");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit v128 shift helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, count);
		invoke->set_arg(2, buffer);
		values.push_back(buffer);
	}

	//! The portable helper each native shift kind falls back to
	//! (`WASMJIT_SIMD_NATIVE=0`).
	static void (*helperForNativeShiftKind(int kind))(const void*, int32_t, void*)
	{
		switch (kind) {
		case 0: return &wasm_i16x8_shl;
		case 1: return &wasm_i16x8_shr_s;
		case 2: return &wasm_i16x8_shr_u;
		case 3: return &wasm_i32x4_shl;
		case 4: return &wasm_i32x4_shr_s;
		default: return &wasm_i32x4_shr_u;
		}
	}

	//! Native 16-byte shift on two buffers, no helper call (`SIMD-8`).
	//! Kinds: 0..2 `i16x8` `shl`/`shr_s`/`shr_u`, 3..5 the `i32x4` trio.
	//! SSE2's packed shift by a vector reads a per-lane count from the low 64
	//! bits and zeroes lanes when the count exceeds the lane width, so the count
	//! is masked to `laneBits - 1` first -- wasm's `count mod laneBits`.
	void emitVectorShiftNative(const char* name, int kind)
	{
		if (!nativeSimdEnabled())
		{
			emitVectorShift(helperForNativeShiftKind(kind));
			return;
		}
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 shift operand underflow");
		auto count = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		auto result = newVectorBuffer("v128Shift");
		auto vector = compiler.new_xmm("v128ShiftValue");
		auto laneCount = compiler.new_xmm("v128ShiftCount");
		auto mask = compiler.new_xmm("v128ShiftMask");
		emitter.vectorLoad128(vector, asmjit::x86::xmmword_ptr(a));
		// The count move zeroes the rest of the vector, and the mask leaves
		// `count % laneBits` in every lane — what the per-lane shift expects.
		// The mask itself is 15 (words) or 31 (dwords) in every lane.
		emitter.gpToVector32(laneCount, count.r32());
		const bool words = kind < 3;
		const uint64_t maskBits = words ? 0x000F000F000F000Full
				: 0x0000001F0000001Full;
		auto maskBuffer = newVectorBuffer("v128ShiftMaskBits");
		auto maskValue = compiler.new_gp64("v128ShiftMaskValue");
		emitter.mov(maskValue, maskBits);
		emitter.mov(asmjit::x86::qword_ptr(maskBuffer, 0), maskValue);
		emitter.mov(asmjit::x86::qword_ptr(maskBuffer, 8), maskValue);
		emitter.vectorLoad128(mask, asmjit::x86::xmmword_ptr(maskBuffer));
		emitter.vector3(asmjit::ujit::UniOpVVV::kAndU32, laneCount, laneCount, mask);
		// A per-lane variable-count shift has no universal operation: the
		// vocabulary offers immediate counts only, so these six instructions are
		// emitted through the host and listed in Asmjit.md "The host-specific set".
		switch (kind) {
		case 0: checkError(emitter.host().psllw(vector, laneCount), name); break;
		case 1: checkError(emitter.host().psraw(vector, laneCount), name); break;
		case 2: checkError(emitter.host().psrlw(vector, laneCount), name); break;
		case 3: checkError(emitter.host().pslld(vector, laneCount), name); break;
		case 4: checkError(emitter.host().psrad(vector, laneCount), name); break;
		default: checkError(emitter.host().psrld(vector, laneCount), name); break;
		}
		emitter.vectorStore128(asmjit::x86::xmmword_ptr(result), vector);
		values.push_back(result);
	}

	void dispatchI8x16Shl() override { emitVectorShift(&wasm_i8x16_shl); }
	void dispatchI8x16ShrS() override { emitVectorShift(&wasm_i8x16_shr_s); }
	void dispatchI8x16ShrU() override { emitVectorShift(&wasm_i8x16_shr_u); }
	void dispatchI16x8Shl() override { emitVectorShiftNative("AsmJit i16x8.shl failed", 0); }
	void dispatchI16x8ShrS() override { emitVectorShiftNative("AsmJit i16x8.shr_s failed", 1); }
	void dispatchI16x8ShrU() override { emitVectorShiftNative("AsmJit i16x8.shr_u failed", 2); }
	void dispatchI32x4Shl() override { emitVectorShiftNative("AsmJit i32x4.shl failed", 3); }
	void dispatchI32x4ShrS() override { emitVectorShiftNative("AsmJit i32x4.shr_s failed", 4); }
	void dispatchI32x4ShrU() override { emitVectorShiftNative("AsmJit i32x4.shr_u failed", 5); }
	void dispatchI64x2Shl() override { emitVectorShift(&wasm_i64x2_shl); }
	void dispatchI64x2ShrS() override { emitVectorShift(&wasm_i64x2_shr_s); }
	void dispatchI64x2ShrU() override { emitVectorShift(&wasm_i64x2_shr_u); }

	void dispatchI8x16MinS() override { emitVectorHelper2(&wasm_i8x16_min_s); }
	void dispatchI8x16MinU() override { emitVectorBinaryNative("AsmJit i8x16.min_u failed", 14); }
	void dispatchI8x16MaxS() override { emitVectorHelper2(&wasm_i8x16_max_s); }
	void dispatchI8x16MaxU() override { emitVectorBinaryNative("AsmJit i8x16.max_u failed", 16); }
	void dispatchI16x8MinS() override { emitVectorBinaryNative("AsmJit i16x8.min_s failed", 15); }
	void dispatchI16x8MinU() override { emitVectorHelper2(&wasm_i16x8_min_u); }
	void dispatchI16x8MaxS() override { emitVectorBinaryNative("AsmJit i16x8.max_s failed", 17); }
	void dispatchI16x8MaxU() override { emitVectorHelper2(&wasm_i16x8_max_u); }
	void dispatchI32x4MinS() override { emitVectorHelper2(&wasm_i32x4_min_s); }
	void dispatchI32x4MinU() override { emitVectorHelper2(&wasm_i32x4_min_u); }
	void dispatchI32x4MaxS() override { emitVectorHelper2(&wasm_i32x4_max_s); }
	void dispatchI32x4MaxU() override { emitVectorHelper2(&wasm_i32x4_max_u); }
	void dispatchI8x16AddSatS() override { emitVectorHelper2(&wasm_i8x16_add_sat_s); }
	void dispatchI8x16AddSatU() override { emitVectorHelper2(&wasm_i8x16_add_sat_u); }
	void dispatchI8x16SubSatS() override { emitVectorHelper2(&wasm_i8x16_sub_sat_s); }
	void dispatchI8x16SubSatU() override { emitVectorHelper2(&wasm_i8x16_sub_sat_u); }
	void dispatchI16x8AddSatS() override { emitVectorHelper2(&wasm_i16x8_add_sat_s); }
	void dispatchI16x8AddSatU() override { emitVectorHelper2(&wasm_i16x8_add_sat_u); }
	void dispatchI16x8SubSatS() override { emitVectorHelper2(&wasm_i16x8_sub_sat_s); }
	void dispatchI16x8SubSatU() override { emitVectorHelper2(&wasm_i16x8_sub_sat_u); }
	void dispatchI16x8Q15MulRSatS() override { emitVectorHelper2(&wasm_i16x8_q15mulr_sat_s); }
	void dispatchI8x16AvgrU() override { emitVectorBinaryNative("AsmJit i8x16.avgr_u failed", 12); }
	void dispatchI16x8AvgrU() override { emitVectorBinaryNative("AsmJit i16x8.avgr_u failed", 13); }

	void dispatchI8x16Abs() override { emitVectorHelper1(&wasm_i8x16_abs); }
	void dispatchI16x8Abs() override { emitVectorHelper1(&wasm_i16x8_abs); }
	void dispatchI32x4Abs() override { emitVectorHelper1(&wasm_i32x4_abs); }
	void dispatchI64x2Abs() override { emitVectorHelper1(&wasm_i64x2_abs); }
	void dispatchI8x16Popcnt() override { emitVectorHelper1(&wasm_i8x16_popcnt); }
	void dispatchI16x8ExtendLowI8x16S() override { emitVectorHelper1(&wasm_i16x8_extend_low_i8x16_s); }
	void dispatchI16x8ExtendLowI8x16U() override { emitVectorHelper1(&wasm_i16x8_extend_low_i8x16_u); }
	void dispatchI16x8ExtendHighI8x16S() override { emitVectorHelper1(&wasm_i16x8_extend_high_i8x16_s); }
	void dispatchI16x8ExtendHighI8x16U() override { emitVectorHelper1(&wasm_i16x8_extend_high_i8x16_u); }
	void dispatchI32x4ExtendLowI16x8S() override { emitVectorHelper1(&wasm_i32x4_extend_low_i16x8_s); }
	void dispatchI32x4ExtendLowI16x8U() override { emitVectorHelper1(&wasm_i32x4_extend_low_i16x8_u); }
	void dispatchI32x4ExtendHighI16x8S() override { emitVectorHelper1(&wasm_i32x4_extend_high_i16x8_s); }
	void dispatchI32x4ExtendHighI16x8U() override { emitVectorHelper1(&wasm_i32x4_extend_high_i16x8_u); }
	void dispatchI64x2ExtendLowI32x4S() override { emitVectorHelper1(&wasm_i64x2_extend_low_i32x4_s); }
	void dispatchI64x2ExtendLowI32x4U() override { emitVectorHelper1(&wasm_i64x2_extend_low_i32x4_u); }
	void dispatchI64x2ExtendHighI32x4S() override { emitVectorHelper1(&wasm_i64x2_extend_high_i32x4_s); }
	void dispatchI64x2ExtendHighI32x4U() override { emitVectorHelper1(&wasm_i64x2_extend_high_i32x4_u); }
	void dispatchI16x8ExtAddPairwiseI8x16S() override { emitVectorHelper1(&wasm_i16x8_extadd_pairwise_i8x16_s); }
	void dispatchI16x8ExtAddPairwiseI8x16U() override { emitVectorHelper1(&wasm_i16x8_extadd_pairwise_i8x16_u); }
	void dispatchI32x4ExtAddPairwiseI16x8S() override { emitVectorHelper1(&wasm_i32x4_extadd_pairwise_i16x8_s); }
	void dispatchI32x4ExtAddPairwiseI16x8U() override { emitVectorHelper1(&wasm_i32x4_extadd_pairwise_i16x8_u); }
	void dispatchI16x8ExtMulLowI8x16S() override { emitVectorHelper2(&wasm_i16x8_extmul_low_i8x16_s); }
	void dispatchI16x8ExtMulLowI8x16U() override { emitVectorHelper2(&wasm_i16x8_extmul_low_i8x16_u); }
	void dispatchI16x8ExtMulHighI8x16S() override { emitVectorHelper2(&wasm_i16x8_extmul_high_i8x16_s); }
	void dispatchI16x8ExtMulHighI8x16U() override { emitVectorHelper2(&wasm_i16x8_extmul_high_i8x16_u); }
	void dispatchI32x4ExtMulLowI16x8S() override { emitVectorHelper2(&wasm_i32x4_extmul_low_i16x8_s); }
	void dispatchI32x4ExtMulLowI16x8U() override { emitVectorHelper2(&wasm_i32x4_extmul_low_i16x8_u); }
	void dispatchI32x4ExtMulHighI16x8S() override { emitVectorHelper2(&wasm_i32x4_extmul_high_i16x8_s); }
	void dispatchI32x4ExtMulHighI16x8U() override { emitVectorHelper2(&wasm_i32x4_extmul_high_i16x8_u); }
	void dispatchI64x2ExtMulLowI32x4S() override { emitVectorHelper2(&wasm_i64x2_extmul_low_i32x4_s); }
	void dispatchI64x2ExtMulLowI32x4U() override { emitVectorHelper2(&wasm_i64x2_extmul_low_i32x4_u); }
	void dispatchI64x2ExtMulHighI32x4S() override { emitVectorHelper2(&wasm_i64x2_extmul_high_i32x4_s); }
	void dispatchI64x2ExtMulHighI32x4U() override { emitVectorHelper2(&wasm_i64x2_extmul_high_i32x4_u); }


	void dispatchI8x16NarrowI16x8S() override { emitVectorHelper2(&wasm_i8x16_narrow_i16x8_s); }
	void dispatchI8x16NarrowI16x8U() override { emitVectorHelper2(&wasm_i8x16_narrow_i16x8_u); }
	void dispatchI16x8NarrowI32x4S() override { emitVectorHelper2(&wasm_i16x8_narrow_i32x4_s); }
	void dispatchI16x8NarrowI32x4U() override { emitVectorHelper2(&wasm_i16x8_narrow_i32x4_u); }
	void dispatchI32x4DotI16x8S() override { emitVectorHelper2(&wasm_i32x4_dot_i16x8_s); }

	void dispatchF32x4Add() override { emitVectorHelper2(&wasm_f32x4_add); }
	void dispatchF32x4Sub() override { emitVectorHelper2(&wasm_f32x4_sub); }
	void dispatchF32x4Mul() override { emitVectorHelper2(&wasm_f32x4_mul); }
	void dispatchF32x4Div() override { emitVectorHelper2(&wasm_f32x4_div); }
	void dispatchF32x4Min() override { emitVectorHelper2(&wasm_f32x4_min); }
	void dispatchF32x4Max() override { emitVectorHelper2(&wasm_f32x4_max); }
	void dispatchF32x4PMin() override { emitVectorHelper2(&wasm_f32x4_pmin); }
	void dispatchF32x4PMax() override { emitVectorHelper2(&wasm_f32x4_pmax); }
	void dispatchF32x4Eq() override { emitVectorHelper2(&wasm_f32x4_eq); }
	void dispatchF32x4Ne() override { emitVectorHelper2(&wasm_f32x4_ne); }
	void dispatchF32x4Lt() override { emitVectorHelper2(&wasm_f32x4_lt); }
	void dispatchF32x4Gt() override { emitVectorHelper2(&wasm_f32x4_gt); }
	void dispatchF32x4Le() override { emitVectorHelper2(&wasm_f32x4_le); }
	void dispatchF32x4Ge() override { emitVectorHelper2(&wasm_f32x4_ge); }
	void dispatchF64x2Add() override { emitVectorHelper2(&wasm_f64x2_add); }
	void dispatchF64x2Sub() override { emitVectorHelper2(&wasm_f64x2_sub); }
	void dispatchF64x2Mul() override { emitVectorHelper2(&wasm_f64x2_mul); }
	void dispatchF64x2Div() override { emitVectorHelper2(&wasm_f64x2_div); }
	void dispatchF64x2Min() override { emitVectorHelper2(&wasm_f64x2_min); }
	void dispatchF64x2Max() override { emitVectorHelper2(&wasm_f64x2_max); }
	void dispatchF64x2PMin() override { emitVectorHelper2(&wasm_f64x2_pmin); }
	void dispatchF64x2PMax() override { emitVectorHelper2(&wasm_f64x2_pmax); }
	void dispatchF64x2Eq() override { emitVectorHelper2(&wasm_f64x2_eq); }
	void dispatchF64x2Ne() override { emitVectorHelper2(&wasm_f64x2_ne); }
	void dispatchF64x2Lt() override { emitVectorHelper2(&wasm_f64x2_lt); }
	void dispatchF64x2Gt() override { emitVectorHelper2(&wasm_f64x2_gt); }
	void dispatchF64x2Le() override { emitVectorHelper2(&wasm_f64x2_le); }
	void dispatchF64x2Ge() override { emitVectorHelper2(&wasm_f64x2_ge); }

	void dispatchF32x4Ceil() override { emitVectorHelper1(&wasm_f32x4_ceil); }
	void dispatchF32x4Floor() override { emitVectorHelper1(&wasm_f32x4_floor); }
	void dispatchF32x4Trunc() override { emitVectorHelper1(&wasm_f32x4_trunc); }
	void dispatchF32x4Nearest() override { emitVectorHelper1(&wasm_f32x4_nearest); }
	void dispatchF32x4Abs() override { emitVectorHelper1(&wasm_f32x4_abs); }
	void dispatchF32x4Neg() override { emitVectorHelper1(&wasm_f32x4_neg); }
	void dispatchF32x4Sqrt() override { emitVectorHelper1(&wasm_f32x4_sqrt); }
	void dispatchF64x2Ceil() override { emitVectorHelper1(&wasm_f64x2_ceil); }
	void dispatchF64x2Floor() override { emitVectorHelper1(&wasm_f64x2_floor); }
	void dispatchF64x2Trunc() override { emitVectorHelper1(&wasm_f64x2_trunc); }
	void dispatchF64x2Nearest() override { emitVectorHelper1(&wasm_f64x2_nearest); }
	void dispatchF64x2Abs() override { emitVectorHelper1(&wasm_f64x2_abs); }
	void dispatchF64x2Neg() override { emitVectorHelper1(&wasm_f64x2_neg); }
	void dispatchF64x2Sqrt() override { emitVectorHelper1(&wasm_f64x2_sqrt); }
	void dispatchI32x4TruncSatF32x4S() override { emitVectorHelper1(&wasm_i32x4_trunc_sat_f32x4_s); }
	void dispatchI32x4TruncSatF32x4U() override { emitVectorHelper1(&wasm_i32x4_trunc_sat_f32x4_u); }
	void dispatchF32x4ConvertI32x4S() override { emitVectorHelper1(&wasm_f32x4_convert_i32x4_s); }
	void dispatchF32x4ConvertI32x4U() override { emitVectorHelper1(&wasm_f32x4_convert_i32x4_u); }
	void dispatchF64x2ConvertLowI32x4S() override { emitVectorHelper1(&wasm_f64x2_convert_low_i32x4_s); }
	void dispatchF64x2ConvertLowI32x4U() override { emitVectorHelper1(&wasm_f64x2_convert_low_i32x4_u); }
	void dispatchF64x2PromoteLowF32x4() override { emitVectorHelper1(&wasm_f64x2_promote_low_f32x4); }
	void dispatchI32x4TruncSatF64x2SZero() override { emitVectorHelper1(&wasm_i32x4_trunc_sat_f64x2_s_zero); }
	void dispatchI32x4TruncSatF64x2UZero() override { emitVectorHelper1(&wasm_i32x4_trunc_sat_f64x2_u_zero); }
	void dispatchF32x4DemoteF64x2Zero() override { emitVectorHelper1(&wasm_f32x4_demote_f64x2_zero); }

	// ── float lane access (the first float-bearing helper signatures) ────────
	// The value stack carries floats as bit patterns in Gp (the P3 model), so
	// each call materialises an Xmm argument or moves the Xmm result back.

	//! `f32x4.splat`/`f64x2.splat`: replicate the scalar into every lane.
	void emitVectorSplatFloat(int width, uint64_t helper)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128 float splat operand underflow");
		auto bits = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128FloatSplat");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		// The argument is materialised before the call node is appended.
		const auto argument = materializeFloat(bits, width);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 float splat helper call failed");
		invoke->set_arg(0, argument);
		invoke->set_arg(1, buffer);
		values.push_back(buffer);
	}

	//! `f32x4.extract_lane`/`f64x2.extract_lane`: push the lane's bits.
	void emitVectorExtractFloat(int width, uint64_t helper, uint8_t lane)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128 float extract operand underflow");
		auto vector = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 float extract helper call failed");
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		auto result = compiler.new_xmm("v128FloatLane");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, width));
	}

	//! `f32x4.replace_lane`/`f64x2.replace_lane`.
	void emitVectorReplaceFloat(int width, uint64_t helper, uint8_t lane)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 float replace operand underflow");
		auto scalar = values.back();
		values.pop_back();
		auto vector = values.back();
		values.pop_back();
		auto buffer = newVectorBuffer("v128FloatReplace");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		const auto argument = materializeFloat(scalar, width);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 float replace helper call failed");
		invoke->set_arg(0, vector);
		invoke->set_arg(1, asmjit::Imm(lane));
		invoke->set_arg(2, argument);
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	void dispatchF32x4Splat() override
	{ emitVectorSplatFloat(4, reinterpret_cast<uint64_t>(&wasm_f32x4_splat)); }
	void dispatchF64x2Splat() override
	{ emitVectorSplatFloat(8, reinterpret_cast<uint64_t>(&wasm_f64x2_splat)); }
	void dispatchF32x4ExtractLane(uint8_t lane) override
	{ emitVectorExtractFloat(4, reinterpret_cast<uint64_t>(&wasm_f32x4_extract_lane), lane); }
	void dispatchSIMDF16x8(WASM::SIMDOpcode opcode, uint8_t lane) override
	{
		if (inDeadCode())
			return;
		const uint32_t op = static_cast<uint32_t>(opcode);
		if (op == 0x120u) {
			emitVectorSplatFloat(4, reinterpret_cast<uint64_t>(&wasm_f16x8_splat));
			return;
		}
		if (op == 0x121u) {
			emitVectorExtractFloat(4, reinterpret_cast<uint64_t>(&wasm_f16x8_extract_lane), lane);
			return;
		}
		if (op == 0x122u) {
			emitVectorReplaceFloat(4, reinterpret_cast<uint64_t>(&wasm_f16x8_replace_lane), lane);
			return;
		}
		const auto emitOp = [&](uint64_t helper, int operands, int extra) {
			if (values.size() < static_cast<std::size_t>(operands + extra))
				throw std::runtime_error("AsmJit f16x8 helper operand underflow");
			std::vector<asmjit::x86::Gp> sources(static_cast<std::size_t>(operands + extra));
			for (int i = operands + extra; i > 0; --i) {
				sources[static_cast<std::size_t>(i - 1)] = values.back();
				values.pop_back();
			}
			auto buffer = newVectorBuffer("f16x8Result");
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(asmjit::TypeId::kVoid);
			signature.add_arg(asmjit::TypeId::kUInt32);
			for (int i = 0; i <= operands + extra; ++i)
				signature.add_arg(asmjit::TypeId::kUIntPtr);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
					"AsmJit f16x8 helper call failed");
			invoke->set_arg(0, asmjit::Imm(op));
			for (int i = 0; i < operands + extra; ++i)
				invoke->set_arg(static_cast<uint32_t>(i + 1),
						sources[static_cast<std::size_t>(i)]);
			invoke->set_arg(static_cast<uint32_t>(operands + extra + 1), buffer);
			values.push_back(buffer);
		};
		if ((op >= 0x130u && op <= 0x136u) || (op >= 0x145u && op <= 0x14Du)) {
			emitOp(reinterpret_cast<uint64_t>(&wasm_f16x8_unary), 1, 0);
			return;
		}
		if (op >= 0x137u && op <= 0x144u) {
			emitOp(reinterpret_cast<uint64_t>(&wasm_f16x8_binary), 2, 0);
			return;
		}
		if (op == 0x14Eu || op == 0x14Fu) {
			emitOp(reinterpret_cast<uint64_t>(&wasm_f16x8_ternary), 3, 0);
			return;
		}
		throw std::runtime_error("AsmJit does not lower this f16x8 sub-opcode");
	}
	void dispatchF64x2ExtractLane(uint8_t lane) override
	{ emitVectorExtractFloat(8, reinterpret_cast<uint64_t>(&wasm_f64x2_extract_lane), lane); }
	void dispatchF32x4ReplaceLane(uint8_t lane) override
	{ emitVectorReplaceFloat(4, reinterpret_cast<uint64_t>(&wasm_f32x4_replace_lane), lane); }
	void dispatchF64x2ReplaceLane(uint8_t lane) override
	{ emitVectorReplaceFloat(8, reinterpret_cast<uint64_t>(&wasm_f64x2_replace_lane), lane); }

	// ── v128 transport (extending/splat/zero loads, lane loads and stores) ──

	//! Calls a load-transform helper over a checked address.
	void emitVectorLoadTransform(WASM::MemArg arg, uint32_t width, uint64_t helper)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128 load-transform address underflow");
		auto address = values.back();
		values.pop_back();
		auto pointer = emitMemoryAddress(arg, width, address);
		auto buffer = newVectorBuffer("v128Transform");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 load-transform helper call failed");
		invoke->set_arg(0, pointer);
		invoke->set_arg(1, buffer);
		values.push_back(buffer);
	}

	//! Calls a lane-load helper over the address and the vector below it.
	void emitVectorLoadLane(WASM::MemArg arg, uint32_t width, uint8_t lane, uint64_t helper)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 load-lane operand underflow");
		auto vector = values.back();
		values.pop_back();
		auto address = values.back();
		values.pop_back();
		auto pointer = emitMemoryAddress(arg, width, address);
		auto buffer = newVectorBuffer("v128LoadLane");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 load-lane helper call failed");
		invoke->set_arg(0, pointer);
		invoke->set_arg(1, vector);
		invoke->set_arg(2, asmjit::Imm(lane));
		invoke->set_arg(3, buffer);
		values.push_back(buffer);
	}

	//! Calls a lane-store helper over the address and the vector below it.
	void emitVectorStoreLane(WASM::MemArg arg, uint32_t width, uint8_t lane, uint64_t helper)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit v128 store-lane operand underflow");
		auto vector = values.back();
		values.pop_back();
		auto address = values.back();
		values.pop_back();
		auto pointer = emitMemoryAddress(arg, width, address);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit v128 store-lane helper call failed");
		invoke->set_arg(0, pointer);
		invoke->set_arg(1, vector);
		invoke->set_arg(2, asmjit::Imm(lane));
	}

	void dispatchV128Load8x8S(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load8x8_s)); }
	void dispatchV128Load8x8U(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load8x8_u)); }
	void dispatchV128Load16x4S(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load16x4_s)); }
	void dispatchV128Load16x4U(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load16x4_u)); }
	void dispatchV128Load32x2S(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load32x2_s)); }
	void dispatchV128Load32x2U(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load32x2_u)); }
	void dispatchV128Load8Splat(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 1, reinterpret_cast<uint64_t>(&wasm_v128_load8_splat)); }
	void dispatchV128Load16Splat(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 2, reinterpret_cast<uint64_t>(&wasm_v128_load16_splat)); }
	void dispatchV128Load32Splat(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 4, reinterpret_cast<uint64_t>(&wasm_v128_load32_splat)); }
	void dispatchV128Load64Splat(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load64_splat)); }
	void dispatchV128Load32Zero(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 4, reinterpret_cast<uint64_t>(&wasm_v128_load32_zero)); }
	void dispatchV128Load64Zero(WASM::MemArg arg) override
	{ emitVectorLoadTransform(arg, 8, reinterpret_cast<uint64_t>(&wasm_v128_load64_zero)); }

	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorLoadLane(arg1, 1, arg2, reinterpret_cast<uint64_t>(&wasm_v128_load8_lane)); }
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorLoadLane(arg1, 2, arg2, reinterpret_cast<uint64_t>(&wasm_v128_load16_lane)); }
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorLoadLane(arg1, 4, arg2, reinterpret_cast<uint64_t>(&wasm_v128_load32_lane)); }
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorLoadLane(arg1, 8, arg2, reinterpret_cast<uint64_t>(&wasm_v128_load64_lane)); }
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorStoreLane(arg1, 1, arg2, reinterpret_cast<uint64_t>(&wasm_v128_store8_lane)); }
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorStoreLane(arg1, 2, arg2, reinterpret_cast<uint64_t>(&wasm_v128_store16_lane)); }
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorStoreLane(arg1, 4, arg2, reinterpret_cast<uint64_t>(&wasm_v128_store32_lane)); }
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override
	{ emitVectorStoreLane(arg1, 8, arg2, reinterpret_cast<uint64_t>(&wasm_v128_store64_lane)); }

	void dispatchI8x16Swizzle() override { emitVectorHelper2(&wasm_i8x16_swizzle); }

	void dispatchI8x16RelaxedSwizzle() override
	{ emitVectorHelper2(&wasm_i8x16_relaxed_swizzle); }
	void dispatchI32x4RelaxedTruncF32x4S() override
	{ emitVectorHelper1(&wasm_i32x4_relaxed_trunc_f32x4_s); }
	void dispatchI32x4RelaxedTruncF32x4U() override
	{ emitVectorHelper1(&wasm_i32x4_relaxed_trunc_f32x4_u); }
	void dispatchI32x4RelaxedTruncF64x2SZero() override
	{ emitVectorHelper1(&wasm_i32x4_relaxed_trunc_f64x2_s_zero); }
	void dispatchI32x4RelaxedTruncF64x2UZero() override
	{ emitVectorHelper1(&wasm_i32x4_relaxed_trunc_f64x2_u_zero); }
	void dispatchF32x4RelaxedMadd() override
	{ emitVectorHelper3(&wasm_f32x4_relaxed_madd); }
	void dispatchF32x4RelaxedNmadd() override
	{ emitVectorHelper3(&wasm_f32x4_relaxed_nmadd); }
	void dispatchF64x2RelaxedMadd() override
	{ emitVectorHelper3(&wasm_f64x2_relaxed_madd); }
	void dispatchF64x2RelaxedNmadd() override
	{ emitVectorHelper3(&wasm_f64x2_relaxed_nmadd); }
	void dispatchI8x16RelaxedLaneselect() override
	{ emitVectorHelper3(&wasm_i8x16_relaxed_laneselect); }
	void dispatchI16x8RelaxedLaneselect() override
	{ emitVectorHelper3(&wasm_i16x8_relaxed_laneselect); }
	void dispatchI32x4RelaxedLaneselect() override
	{ emitVectorHelper3(&wasm_i32x4_relaxed_laneselect); }
	void dispatchI64x2RelaxedLaneselect() override
	{ emitVectorHelper3(&wasm_i64x2_relaxed_laneselect); }
	void dispatchF32x4RelaxedMin() override
	{ emitVectorHelper2(&wasm_f32x4_relaxed_min); }
	void dispatchF32x4RelaxedMax() override
	{ emitVectorHelper2(&wasm_f32x4_relaxed_max); }
	void dispatchF64x2RelaxedMin() override
	{ emitVectorHelper2(&wasm_f64x2_relaxed_min); }
	void dispatchF64x2RelaxedMax() override
	{ emitVectorHelper2(&wasm_f64x2_relaxed_max); }
	void dispatchI16x8RelaxedQ15MulRS() override
	{ emitVectorHelper2(&wasm_i16x8_relaxed_q15mulr_s); }
	void dispatchI16x8RelaxedDotI8x16I7x16S() override
	{ emitVectorHelper2(&wasm_i16x8_relaxed_dot_i8x16_i7x16_s); }
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override
	{ emitVectorHelper3(&wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s); }

	void dispatchRefNull(const WASM::HeapType&) override
	{
		// A reference is one null word: the backend does not model typed nulls.
		auto reference = compiler.new_gp64("nullRef");
		emitter.xor_(reference, reference);
		values.push_back(reference);
	}

	void dispatchRefI31() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit ref.i31 operand underflow");
		auto source = values.back();
		values.pop_back();
		// An i31 is a tagged immediate: (zero-extended value << 1) | 1.
		auto reference = compiler.new_gp64("i31Ref");
		emitter.mov(reference.r32(), source.r32());
		emitter.shl(reference, 1);
		emitter.or_(reference, 1);
		// An i31 is a tagged immediate: uncounted, but still a reference, so a
		// consumer must not treat it as a scalar.
		markReference(reference);
		values.push_back(reference);
	}

	void emitI31Get(bool signExtend)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i31.get operand underflow");
		auto reference = values.back();
		values.pop_back();
		auto notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(reference, reference));
		asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
		reportSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* report = nullptr;
		checkError(compiler.invoke(asmjit::Out(report),
				reinterpret_cast<uint64_t>(&asmjitReportNullI31), reportSignature),
				"AsmJit i31.get null trap emission failed");
		emitZeroReturn();
		checkError(compiler.bind(notNull), "AsmJit i31.get continuation binding failed");
		auto result = compiler.new_gp32("i31Value");
		emitter.mov(result, reference.r32());
		if (signExtend)
			emitter.sar(result, asmjit::Imm(1));
		else
			emitter.shr(result, asmjit::Imm(1));
		values.push_back(result);
	}

	void dispatchI31GetS() override { emitI31Get(true); }
	void dispatchI31GetU() override { emitI31Get(false); }

	void emitExternConversion(void* (*helper)(WASM::VMContext*, void*),
			const char* operation)
	{
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + operation
					+ " operand underflow");
		auto reference = values.back();
		values.pop_back();
		const bool owned = referenceValues.erase(reference.id()) != 0;
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(helper), signature),
				"AsmJit extern conversion helper failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		auto result = compiler.new_gp64("externConversion");
		invoke->set_ret(0, result);
		// The result replaces the operand on the stack, and the stack owns one
		// count of every reference it holds -- but the count does not simply
		// "move". `extern.convert_any` of an internal reference returns a fresh
		// box, which starts unowned, and `any.convert_extern` may free the box it
		// consumes, releasing the inner reference with it. So the result takes the
		// count *before* the operand gives its own up.
		if (owned) {
			const asmjit::x86::Gp kept = emitRefClone(result);
			emitter.mov(result, kept);
			markReference(result);
		}
		values.push_back(result);
		if (owned)
			emitRefDrop(reference);
	}

	void dispatchAnyConvertExtern() override
	{ emitExternConversion(&asmjitExternUnbox, "any.convert_extern"); }
	void dispatchExternConvertAny() override
	{ emitExternConversion(&asmjitExternBox, "extern.convert_any"); }

	void dispatchRefIsNull() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit ref.is_null operand underflow");
		auto reference = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("refIsNull");
		// One universal `select` replaces the zero/test/setcc triple, so the
		// result is defined whether or not the reference is null.
		emitter.selectFlag(result, reference,
				asmjit::ujit::test_z(reference, reference));
		// The operand was the operand stack's and the result is an i32, so the
		// stack gives its count up; the answer is already a plain integer.
		if (referenceValues.erase(reference.id()) != 0)
			emitRefDrop(reference);
		values.push_back(result);
	}

	void dispatchBrOnNull(WASM::LabelIdx depth) override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit br_on_null operand underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_on_null label depth is out of range");
		auto reference = values.back();
		if (depth == controls.size()) {
			if (resultCount != 0)
				throw std::runtime_error("AsmJit br_on_null to the function label requires a void result");
			auto skip = compiler.new_label();
			emitter.j(skip, asmjit::ujit::test_nz(reference, reference));
			checkError(compiler.ret(), "AsmJit br_on_null return failed");
			checkError(compiler.bind(skip), "AsmJit br_on_null label binding failed");
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const bool loopTarget = target.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? target.paramCount : target.resultCount;
		const size_t refDepth = values.size() - 1;
		if (branchValues > refDepth || refDepth - branchValues < target.stackHeight)
			throw std::runtime_error("AsmJit br_on_null operand stack does not match its target label");
		const asmjit::Label label = loopTarget ? target.begin : target.end;
		auto skip = compiler.new_label();
		emitter.j(skip, asmjit::ujit::test_nz(reference, reference));
		// The null branch carries the values below the reference, not the ref.
		const size_t first = refDepth - branchValues;
		const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? target.params : target.results;
		const std::vector<WASM::ValueTypeCode>& codes = loopTarget
				? target.paramTypes : target.resultTypes;
		for (size_t i = 0; i < branchValues; ++i)
			transferValue(destinations[i], values[first + i], codes[i],
					"AsmJit br_on_null result transfer failed");
		emitter.jmp(label);
		if (!loopTarget)
			target.endTargeted = true;
		checkError(compiler.bind(skip), "AsmJit br_on_null skip binding failed");
	}

	void dispatchBrOnNonNull(WASM::LabelIdx depth) override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit br_on_non_null operand underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_on_non_null label depth is out of range");
		auto reference = values.back();
		if (depth == controls.size())
			throw std::runtime_error("AsmJit br_on_non_null to the function label is not lowered yet");
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const bool loopTarget = target.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? target.paramCount : target.resultCount;
		if (branchValues > values.size() || values.size() - branchValues < target.stackHeight)
			throw std::runtime_error("AsmJit br_on_non_null operand stack does not match its target label");
		const asmjit::Label label = loopTarget ? target.begin : target.end;
		auto skip = compiler.new_label();
		emitter.j(skip, asmjit::ujit::test_z(reference, reference));
		const size_t first = values.size() - branchValues;
		const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? target.params : target.results;
		const std::vector<WASM::ValueTypeCode>& codes = loopTarget
				? target.paramTypes : target.resultTypes;
		for (size_t i = 0; i < branchValues; ++i)
			transferValue(destinations[i], values[first + i], codes[i],
					"AsmJit br_on_non_null result transfer failed");
		emitter.jmp(label);
		if (!loopTarget)
			target.endTargeted = true;
		checkError(compiler.bind(skip), "AsmJit br_on_non_null skip binding failed");
		// Fallthrough consumes the reference: it is a move, not a clone. Control
		// only reaches this point when the reference was null, so the release is
		// a no-op at run time and the count is not taken twice.
		if (referenceValues.erase(reference.id()) != 0)
			emitRefDrop(reference);
		values.pop_back();
	}

	void emitBrOnCast(uint8_t castop, WASM::LabelIdx depth, const WASM::HeapType& target,
			bool onFailure)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit br_on_cast operand underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_on_cast label depth is out of range");
		auto reference = values.back();
		const uint32_t heapValue = target.isTypeIndex ? target.typeIndex
				: static_cast<uint32_t>(static_cast<int32_t>(target.abstract));
		auto packed = packImmPair(target.isTypeIndex ? 1u : 0u, heapValue, "brOnCastHeap");
		asmjit::FuncSignature predicate(asmjit::CallConvId::kCDecl);
		predicate.set_ret(asmjit::TypeId::kInt32);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUInt64);
		predicate.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitRefMatchesHeapType), predicate),
				"AsmJit br_on_cast predicate call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, packed);
		invoke->set_arg(3, asmjit::Imm((castop & 0x02) != 0 ? 1 : 0));
		auto matches = compiler.new_gp32("brOnCastMatch");
		invoke->set_ret(0, matches);
		if (depth == controls.size()) {
			// Function label: the branch returns the top result values.
			if (values.size() < resultCount)
				throw std::runtime_error("AsmJit br_on_cast function branch has too few results");
			auto fallthrough = compiler.new_label();
			emitter.j(fallthrough, onFailure ? asmjit::ujit::test_nz(matches, matches)
					: asmjit::ujit::test_z(matches, matches));
			emitFunctionReturnValues();
			checkError(compiler.bind(fallthrough), "AsmJit br_on_cast function label binding failed");
			return;
		}
		ControlFrame& frame = controls[controls.size() - 1 - depth];
		const bool loopTarget = frame.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? frame.paramCount : frame.resultCount;
		if (branchValues > values.size() || values.size() - branchValues < frame.stackHeight)
			throw std::runtime_error("AsmJit br_on_cast operand stack does not match its target label");
		auto fallthrough = compiler.new_label();
		emitter.j(fallthrough, onFailure ? asmjit::ujit::test_nz(matches, matches)
					: asmjit::ujit::test_z(matches, matches));
		const size_t first = values.size() - branchValues;
		const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? frame.params : frame.results;
		const std::vector<WASM::ValueTypeCode>& codes = loopTarget
				? frame.paramTypes : frame.resultTypes;
		for (size_t i = 0; i < branchValues; ++i)
			transferValue(destinations[i], values[first + i], codes[i],
					"AsmJit br_on_cast result transfer failed");
		emitter.jmp(loopTarget ? frame.begin : frame.end);
		if (!loopTarget)
			frame.endTargeted = true;
		checkError(compiler.bind(fallthrough), "AsmJit br_on_cast fallthrough binding failed");
	}

	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx depth,
			WASM::HeapType, WASM::HeapType target) override
	{ emitBrOnCast(castop, depth, target, false); }
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx depth,
			WASM::HeapType, WASM::HeapType target) override
	{ emitBrOnCast(castop, depth, target, true); }

	void dispatchStructNewDefault(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.new_default requires a struct type");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitStructNew), signature),
				"AsmJit struct.new_default allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(WASM::gcStructSize(fields))));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg)));
		auto object = compiler.new_gp_ptr("structObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		for (std::size_t i = 0; i < fields.size(); ++i) {
			const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, i));
			if (fields[i].storage.isPacked) {
				// A packed field is one or two bytes and its default value is zero for
				// both, so it is one store of the field's own width.
				const bool isI8 = fields[i].storage.packed == WASM::ValueTypeCode::I8;
				if (isI8)
					emitter.mov(asmjit::x86::byte_ptr(object, offset), asmjit::Imm(0));
				else
					emitter.mov(asmjit::x86::word_ptr(object, offset), asmjit::Imm(0));
				continue;
			}
			const int width = kindWidth(fields[i].storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new_default does not lower this field type yet");
			if (width == 4)
				emitter.mov(asmjit::x86::dword_ptr(object, offset),
						static_cast<uint32_t>(0));
			else
				emitter.mov(asmjit::x86::qword_ptr(object, offset),
						static_cast<uint64_t>(0));
		}
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.push_back(emitRefClone(object));
		markReference(values.back());
	}

	void dispatchStructNew(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.new requires a struct type");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (values.size() < fields.size())
			throw std::runtime_error("AsmJit struct.new operand stack underflow");
		// Fields are stacked in declaration order; store them in reverse.
		std::vector<asmjit::x86::Gp> operands(fields.size());
		for (std::size_t i = fields.size(); i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		const std::size_t size = WASM::gcStructSize(fields);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitStructNew), signature),
				"AsmJit struct.new allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(size)));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg)));
		auto object = compiler.new_gp_ptr("structObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		for (std::size_t i = 0; i < fields.size(); ++i) {
			const auto& field = fields[i];
			const std::size_t offset = WASM::gcStructFieldOffset(fields, i);
			if (field.storage.isPacked) {
				// A packed field keeps only its low byte or word.
				if (field.storage.packed == WASM::ValueTypeCode::I8)
					emitter.guestStore8(asmjit::x86::byte_ptr(object,
						static_cast<int32_t>(offset)), operands[i]);
				else
					emitter.guestStore16(asmjit::x86::word_ptr(object,
						static_cast<int32_t>(offset)), operands[i]);
				continue;
			}
			const int width = kindWidth(field.storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new does not lower this field type yet");
			if (width == 4)
				emitter.guestStore32(asmjit::x86::dword_ptr(object,
					static_cast<int32_t>(offset)), operands[i]);
			else
				emitter.guestStore64(asmjit::x86::qword_ptr(object,
					static_cast<int32_t>(offset)), operands[i]);
		}
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.push_back(emitRefClone(object));
		markReference(values.back());
	}

	void dispatchStructNewDesc(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.new_desc requires a struct type");
		if (!view.descriptor().has_value())
			throw std::runtime_error("AsmJit struct.new_desc requires a type with a descriptor clause");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (values.size() < fields.size() + 1)
			throw std::runtime_error("AsmJit struct.new_desc operand stack underflow");
		// The descriptor sits on top of the field operands.
		auto descriptor = values.back();
		values.pop_back();
		std::vector<asmjit::x86::Gp> operands(fields.size());
		for (std::size_t i = fields.size(); i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		const std::size_t size = WASM::gcStructSize(fields);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitStructNewDesc), signature),
				"AsmJit struct.new_desc allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(size)));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg)));
		invoke->set_arg(3, descriptor);
		auto object = compiler.new_gp_ptr("structObjectDesc");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		for (std::size_t i = 0; i < fields.size(); ++i) {
			const auto& field = fields[i];
			const std::size_t offset = WASM::gcStructFieldOffset(fields, i);
			if (field.storage.isPacked) {
				if (field.storage.packed == WASM::ValueTypeCode::I8)
					emitter.guestStore8(asmjit::x86::byte_ptr(object,
						static_cast<int32_t>(offset)), operands[i]);
				else
					emitter.guestStore16(asmjit::x86::word_ptr(object,
						static_cast<int32_t>(offset)), operands[i]);
				continue;
			}
			const int width = kindWidth(field.storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new_desc does not lower this field type yet");
			if (width == 4)
				emitter.guestStore32(asmjit::x86::dword_ptr(object,
					static_cast<int32_t>(offset)), operands[i]);
			else
				emitter.guestStore64(asmjit::x86::qword_ptr(object,
					static_cast<int32_t>(offset)), operands[i]);
		}
		// The new object retained the descriptor; the operand's own count goes,
		// and the stack then owns one count of the new object.
		emitRefDrop(descriptor);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.push_back(emitRefClone(object));
		markReference(values.back());
	}

	void dispatchStructNewDefaultDesc(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.new_default_desc requires a struct type");
		if (!view.descriptor().has_value())
			throw std::runtime_error("AsmJit struct.new_default_desc requires a type with a descriptor clause");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (values.empty())
			throw std::runtime_error("AsmJit struct.new_default_desc operand stack underflow");
		auto descriptor = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitStructNewDesc), signature),
				"AsmJit struct.new_default_desc allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(WASM::gcStructSize(fields))));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg)));
		invoke->set_arg(3, descriptor);
		auto object = compiler.new_gp_ptr("structObjectDefaultDesc");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		// The new object retained the descriptor; the operand's own count goes.
		emitRefDrop(descriptor);
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (fields[i].storage.isPacked)
				throw std::runtime_error("AsmJit struct.new_default_desc does not lower packed fields yet");
			const int width = kindWidth(fields[i].storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new_default_desc does not lower this field type yet");
			const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, i));
			if (width == 4)
				emitter.mov(asmjit::x86::dword_ptr(object, offset),
						static_cast<uint32_t>(0));
			else
				emitter.mov(asmjit::x86::qword_ptr(object, offset),
						static_cast<uint64_t>(0));
		}
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.push_back(emitRefClone(object));
		markReference(values.back());
	}

	void dispatchRefGetDesc(WASM::TypeIdx) override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit ref.get_desc operand stack underflow");
		auto reference = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitRefGetDesc), signature),
				"AsmJit ref.get_desc helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		auto descriptor = compiler.new_gp_ptr("descriptorRef");
		invoke->set_ret(0, descriptor);
		emitPendingTrapReturn();
		// `ref.get_desc` yields a reference in place of the one it consumed, so
		// the operand stack's one count moves onto the descriptor.
		if (referenceValues.erase(reference.id()) != 0)
			markReference(descriptor);
		values.push_back(descriptor);
	}

	void emitRefCastDescEq(bool nullable)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit cast_desc_eq operand stack underflow");
		auto descriptor = values.back();
		values.pop_back();
		auto reference = values.back();
		values.pop_back();
		asmjit::FuncSignature predicate(asmjit::CallConvId::kCDecl);
		predicate.set_ret(asmjit::TypeId::kInt32);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUInt32);
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitDescEq), predicate),
				"AsmJit cast_desc_eq predicate call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, descriptor);
		invoke->set_arg(3, asmjit::Imm(nullable ? 1 : 0));
		auto matches = compiler.new_gp32("descEqMatch");
		invoke->set_ret(0, matches);
		asmjit::Label afterCast = compiler.new_label();
		emitter.j(afterCast, asmjit::ujit::test_nz(matches, matches));
		asmjit::InvokeNode* failCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(failCall),
				reinterpret_cast<uint64_t>(&asmjitReportCastFailure), voidSignature),
				"AsmJit cast_desc_eq failure trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(afterCast), "AsmJit cast_desc_eq label binding failed");
		// Success path only: the descriptor operand is consumed.
		emitRefDrop(descriptor);
		values.push_back(reference);
	}

	void dispatchRefCastDescEq(const WASM::HeapType&) override
	{ emitRefCastDescEq(false); }
	void dispatchRefCastDescEqNull(const WASM::HeapType&) override
	{ emitRefCastDescEq(true); }

	void emitBrOnCastDescEq(uint8_t castop, WASM::LabelIdx depth, bool onFailure)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit br_on_cast_desc_eq operand underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_on_cast_desc_eq label depth is out of range");
		auto descriptor = values.back();
		values.pop_back();
		auto reference = values.back();
		asmjit::FuncSignature predicate(asmjit::CallConvId::kCDecl);
		predicate.set_ret(asmjit::TypeId::kInt32);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitDescEq), predicate),
				"AsmJit br_on_cast_desc_eq predicate call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, descriptor);
		invoke->set_arg(3, asmjit::Imm((castop & 0x02) != 0 ? 1 : 0));
		auto matches = compiler.new_gp32("brDescEqMatch");
		invoke->set_ret(0, matches);
		// The descriptor is consumed on both paths; `values` still holds the
		// reference, which the branch transfers.
		emitRefDrop(descriptor);
		if (depth == controls.size()) {
			if (values.size() < resultCount)
				throw std::runtime_error("AsmJit br_on_cast_desc_eq function branch has too few results");
			auto fallthrough = compiler.new_label();
			emitter.j(fallthrough, onFailure ? asmjit::ujit::test_nz(matches, matches)
					: asmjit::ujit::test_z(matches, matches));
			emitFunctionReturnValues();
			checkError(compiler.bind(fallthrough),
					"AsmJit br_on_cast_desc_eq fallthrough binding failed");
			return;
		}
		ControlFrame& frame = controls[controls.size() - 1 - depth];
		const bool loopTarget = frame.kind == ControlKind::Loop;
		const size_t branchValues = loopTarget ? frame.paramCount : frame.resultCount;
		if (branchValues > values.size() || values.size() - branchValues < frame.stackHeight)
			throw std::runtime_error("AsmJit br_on_cast_desc_eq operand stack does not match its target label");
		auto fallthrough = compiler.new_label();
		emitter.j(fallthrough, onFailure ? asmjit::ujit::test_nz(matches, matches)
					: asmjit::ujit::test_z(matches, matches));
		const size_t first = values.size() - branchValues;
		const std::vector<asmjit::x86::Gp>& destinations = loopTarget ? frame.params : frame.results;
		const std::vector<WASM::ValueTypeCode>& codes = loopTarget
				? frame.paramTypes : frame.resultTypes;
		for (size_t i = 0; i < branchValues; ++i)
			transferValue(destinations[i], values[first + i], codes[i],
					"AsmJit br_on_cast_desc_eq result transfer failed");
		emitter.jmp(loopTarget ? frame.begin : frame.end);
		if (!loopTarget)
			frame.endTargeted = true;
		checkError(compiler.bind(fallthrough),
				"AsmJit br_on_cast_desc_eq fallthrough binding failed");
	}

	void dispatchBrOnCastDescEq(uint8_t castop, WASM::LabelIdx depth,
			WASM::HeapType, WASM::HeapType) override
	{ emitBrOnCastDescEq(castop, depth, false); }
	void dispatchBrOnCastDescEqFail(uint8_t castop, WASM::LabelIdx depth,
			WASM::HeapType, WASM::HeapType) override
	{ emitBrOnCastDescEq(castop, depth, true); }

	void emitArrayGetPacked(WASM::TypeIdx arg, bool signExtend)
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.get_s/u requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		if (!element.storage.isPacked)
			throw std::runtime_error("AsmJit array.get_s/u requires a packed element type");
		const bool isI8 = element.storage.packed == WASM::ValueTypeCode::I8;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.get_s/u operand stack underflow");
		auto index = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.get_s/u null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.get_s/u null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		emitter.headerLoad32(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
		asmjit::Label inBounds = compiler.new_label();
		emitter.j(inBounds, asmjit::ujit::ucmp_lt(index, length));
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.get_s/u bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.get_s/u bounds label binding failed");
		auto byteOffset = compiler.new_gp64("arrayByteOffset");
		emitter.mov(byteOffset.r32(), index);
		emitter.shl(byteOffset, isI8 ? 0 : 1);
		emitter.add(byteOffset,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage)));
		auto result = compiler.new_gp32("arrayPackedElement");
		if (isI8) {
			if (signExtend)
				emitter.guestLoad8S(result, asmjit::x86::byte_ptr(object, byteOffset));
			else
				emitter.guestLoad8Z(result, asmjit::x86::byte_ptr(object, byteOffset));
		} else {
			if (signExtend)
				emitter.guestLoad16S(result, asmjit::x86::word_ptr(object, byteOffset));
			else
				emitter.guestLoad16Z(result, asmjit::x86::word_ptr(object, byteOffset));
		}
		// The container operand was the operand stack's, so its count goes
		// back. The element is already in a fresh register, so the drop cannot
		// invalidate it, and a packed element holds no reference of its own.
		if (referenceValues.erase(object.id()) != 0)
			emitRefDrop(object);
		values.push_back(result);
	}

	void dispatchArrayGetS(WASM::TypeIdx arg) override { emitArrayGetPacked(arg, true); }
	void dispatchArrayGetU(WASM::TypeIdx arg) override { emitArrayGetPacked(arg, false); }

	void dispatchArraySet(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.set requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2)
				: kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.set does not lower this element type yet");
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.set operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.set null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.set null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		emitter.headerLoad32(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
		asmjit::Label inBounds = compiler.new_label();
		emitter.j(inBounds, asmjit::ujit::ucmp_lt(index, length));
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.set bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.set bounds label binding failed");
		// Fold the offset into one register, exactly as the read path does.
		auto address = compiler.new_gp_ptr("arraySetAddress");
		emitter.mov(address, object);
		auto byteOffset = compiler.new_gp64("arraySetOffset");
		emitter.mov(byteOffset.r32(), index);
		const int scale = width == 1 ? 0 : (width == 2 ? 1 : (width == 4 ? 2 : 3));
		emitter.shl(byteOffset, scale);
		emitter.add(address, byteOffset);
		emitter.add(address,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage)));
		if (!packed && isReferenceKind(element.storage.value.opcode)) {
			// A reference element is store-owned: the barrier retains the new
			// value for the element and releases the one it held. The operand
			// stack then gives up the counts it held for both operands -- the
			// container and the value -- which is safe from the registers after
			// the store, because both were computed before it.
			emitRefStore(address, value);
			emitRefDrop(object);
			emitRefDrop(value);
			return;
		}
		if (packed) {
			if (packedI8)
				emitter.guestStore8(asmjit::x86::byte_ptr(address), value);
			else
				emitter.guestStore16(asmjit::x86::word_ptr(address), value);
		} else if (width == 4)
			emitter.guestStore32(asmjit::x86::dword_ptr(address), value);
		else
			emitter.guestStore64(asmjit::x86::qword_ptr(address), value);
		// The element holds no reference, so the container operand's count is
		// the only one to give back.
		emitRefDrop(object);
	}

	void dispatchArrayGet(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.get requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		if (element.storage.isPacked)
			throw std::runtime_error("AsmJit array.get does not lower packed elements yet");
		const int width = kindWidth(element.storage.value.opcode);
		if (width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.get does not lower this element type yet");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.get operand stack underflow");
		auto index = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		// Null reference first, then bounds.
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.get null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.get null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		emitter.headerLoad32(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
		asmjit::Label inBounds = compiler.new_label();
		emitter.j(inBounds, asmjit::ujit::ucmp_lt(index, length));
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.get bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.get bounds label binding failed");
		// Element address = object + dataOffset + index * elementSize, computed in a
		// register so the memory operand stays a simple base+index form.
		auto byteOffset = compiler.new_gp64("arrayByteOffset");
		emitter.mov(byteOffset.r32(), index);
		emitter.shl(byteOffset, width == 4 ? 2 : 3);
		emitter.add(byteOffset,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage)));
		auto result = width == 4 ? compiler.new_gp32("arrayElementValue")
				: compiler.new_gp64("arrayElementValue");
		if (width == 4)
			emitter.guestLoad32Z(result, asmjit::x86::dword_ptr(object, byteOffset));
		else
			emitter.guestLoad64(result, asmjit::x86::qword_ptr(object, byteOffset));
		// The container operand belonged to the operand stack, so its count goes
		// back. The element is already in a fresh register, so no spill is
		// needed, and the drop cannot invalidate it.
		emitRefDrop(object);
		if (isReferenceKind(element.storage.value.opcode)) {
			// A reference read out of the element belongs to the element; the
			// operand stack takes its own count, or a later `drop` would release
			// the element's and leave the array pointing at a reclaimed object.
			result = emitRefClone(result);
			markReference(result);
		}
		values.push_back(result);
	}

	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.new_data operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto offset = values.back(); values.pop_back();
		auto packed = packRegPair(offset, length, "arrayNewDataSrcLen");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNewData), signature),
				"AsmJit array.new_data allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(arg1)));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg2)));
		invoke->set_arg(3, packed);
		auto object = compiler.new_gp_ptr("arrayObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		values.push_back(object);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	void dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) override
	{
		if (values.size() < 3)
			throw std::runtime_error("AsmJit array.new_array operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto start = values.back(); values.pop_back();
		auto source = values.back(); values.pop_back();
		auto packedTypes = packImmPair(src, dst, "arrayNewArrayTypes");
		auto packedRange = packRegPair(length, start, "arrayNewArrayRange");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNewArray), signature),
				"AsmJit array.new_array helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, source);
		invoke->set_arg(2, packedTypes);
		invoke->set_arg(3, packedRange);
		auto result = compiler.new_gp_ptr("arrayNewArrayResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
		emitPendingTrapReturn();
	}

	void emitArrayNewFromIndex(const char* opcode, uint32_t dst, uint32_t src,
			uint64_t helper)
	{
		if (values.size() < 2)
			throw std::runtime_error(std::string("AsmJit ") + opcode
					+ " operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto start = values.back(); values.pop_back();
		auto packedTypes = packImmPair(src, dst, "arrayNewIndexTypes");
		auto packedRange = packRegPair(length, start, "arrayNewIndexRange");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		const std::string failure = std::string("AsmJit ") + opcode + " helper call failed";
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				failure.c_str());
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, packedTypes);
		invoke->set_arg(2, packedRange);
		auto result = compiler.new_gp_ptr("arrayNewIndexResult");
		invoke->set_ret(0, result);
		// A fresh array is unowned; the operand stack owns one count per reference
		// it holds, so the push takes one (RC-5).
		values.push_back(emitRefClone(result));
		markReference(values.back());
		emitPendingTrapReturn();
	}

	void dispatchArrayNewMemory(WASM::TypeIdx dst, WASM::MemIdx src) override
	{
		emitArrayNewFromIndex("array.new_memory", dst, src,
				reinterpret_cast<uint64_t>(&asmjitArrayNewMemory));
	}

	void dispatchArrayNewTable(WASM::TypeIdx dst, WASM::TableIdx src) override
	{
		emitArrayNewFromIndex("array.new_table", dst, src,
				reinterpret_cast<uint64_t>(&asmjitArrayNewTable));
	}

	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.new_elem operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto offset = values.back(); values.pop_back();
		auto packed = packRegPair(offset, length, "arrayNewElemSrcLen");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNewElem), signature),
				"AsmJit array.new_elem allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(arg1)));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg2)));
		invoke->set_arg(3, packed);
		auto object = compiler.new_gp_ptr("arrayObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		values.push_back(object);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		if (values.size() < 4)
			throw std::runtime_error("AsmJit array.init_data operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto source = values.back(); values.pop_back();
		auto destination = values.back(); values.pop_back();
		auto reference = values.back(); values.pop_back();
		auto packedType = packImmPair(arg1, arg2, "arrayInitTypeData");
		auto packedDst = packRegPair(destination, source, "arrayInitDataRange");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayInitDataRange), signature),
				"AsmJit array.init_data helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, packedType);
		invoke->set_arg(3, packedDst);
		invoke->set_arg(4, length);
		emitPendingTrapReturn();
		// The array operand was the operand stack's; the helper only wrote into
		// it (the data segment is store-owned, not the stack's).
		if (referenceValues.erase(reference.id()) != 0)
			emitRefDrop(reference);
	}

	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		if (values.size() < 4)
			throw std::runtime_error("AsmJit array.init_elem operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto source = values.back(); values.pop_back();
		auto destination = values.back(); values.pop_back();
		auto reference = values.back(); values.pop_back();
		auto packedType = packImmPair(arg1, arg2, "arrayInitTypeElem");
		auto packedDst = packRegPair(destination, source, "arrayInitElemRange");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayInitElemRange), signature),
				"AsmJit array.init_elem helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, packedType);
		invoke->set_arg(3, packedDst);
		invoke->set_arg(4, length);
		emitPendingTrapReturn();
		// The array operand was the operand stack's; the helper only wrote into
		// it (the element segment is store-owned, not the stack's).
		if (referenceValues.erase(reference.id()) != 0)
			emitRefDrop(reference);
	}

	void dispatchArrayNewDefault(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.new_default requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2) : kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.new_default does not lower this element type yet");
		if (values.empty())
			throw std::runtime_error("AsmJit array.new_default operand stack underflow");
		// The length operand stays live across the allocation call, the same way
		// array.new keeps its initialiser live.
		asmjit::x86::Gp length = values.back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNew), signature),
				"AsmJit array.new_default allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(arg)));
		invoke->set_arg(2, length);
		auto object = compiler.new_gp_ptr("arrayObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		// Counted zero-fill loop: counter counts down, cursor advances, so every
		// store is a zero-displacement store through a walking pointer.
		const auto dataOffset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		const auto elementBytes = static_cast<int32_t>(WASM::gcStorageBytes(element.storage));
		auto counter = compiler.new_gp_ptr("defaultCounter");
		auto cursor = compiler.new_gp_ptr("defaultCursor");
		// r32 -> r32 so the 32-bit length zero-extends: copying the whole 64-bit
		// register would keep whatever garbage sits above bit 31 and turn the
		// countdown into a runaway loop.
		emitter.mov(counter.r32(), length.r32());
		emitter.lea(cursor, asmjit::x86::ptr(object, dataOffset));
		asmjit::Label loopStart = compiler.new_label();
		asmjit::Label loopDone = compiler.new_label();
		checkError(compiler.bind(loopStart), "AsmJit array.new_default label bind failed");
		emitter.j(loopDone, asmjit::ujit::test_z(counter, counter));
		if (width == 1)
			emitter.mov(asmjit::x86::byte_ptr(cursor), asmjit::Imm(0));
		else if (width == 2)
			emitter.mov(asmjit::x86::word_ptr(cursor), asmjit::Imm(0));
		else if (width == 4)
			emitter.mov(asmjit::x86::dword_ptr(cursor), asmjit::Imm(0));
		else
			emitter.mov(asmjit::x86::qword_ptr(cursor), asmjit::Imm(0));
		emitter.add(cursor, asmjit::Imm(elementBytes));
		emitter.dec(counter);
		emitter.jmp(loopStart);
		checkError(compiler.bind(loopDone), "AsmJit array.new_default label bind failed");
		values.pop_back();
		values.push_back(object);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		const WASM::CoreTypeView view = module.typeView(arg1);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.new_fixed requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2)
				: kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.new_fixed does not lower this element type yet");
		if (values.size() < arg2)
			throw std::runtime_error("AsmJit array.new_fixed operand stack underflow");
		std::vector<asmjit::x86::Gp> operands(arg2);
		for (uint32_t i = arg2; i-- > 0; ) {
			operands[i] = values.back();
			values.pop_back();
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNew), signature),
				"AsmJit array.new_fixed allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(arg1)));
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(arg2)));
		auto object = compiler.new_gp_ptr("arrayObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		const auto dataOffset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		const auto elementBytes = static_cast<int32_t>(WASM::gcStorageBytes(element.storage));
		for (uint32_t i = 0; i < arg2; ++i) {
			const auto offset = static_cast<int32_t>(dataOffset + static_cast<int32_t>(i) * elementBytes);
			if (packed) {
				if (packedI8)
					emitter.guestStore8(asmjit::x86::byte_ptr(object, offset),
					operands[i]);
				else
					emitter.guestStore16(asmjit::x86::word_ptr(object, offset),
					operands[i]);
			} else if (width == 4)
				emitter.guestStore32(asmjit::x86::dword_ptr(object, offset), operands[i]);
			else
				emitter.guestStore64(asmjit::x86::qword_ptr(object, offset), operands[i]);
		}
		values.push_back(object);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override
	{
		const WASM::CoreTypeView dstView = module.typeView(arg1);
		if (dstView.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.copy requires an array type");
		const WASM::CanonFieldType& element = dstView.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2) : kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.copy does not lower this element type yet");
		if (values.size() < 5)
			throw std::runtime_error("AsmJit array.copy operand stack underflow");
		auto length = values.back();
		values.pop_back();
		auto srcIndex = values.back();
		values.pop_back();
		auto srcObject = values.back();
		values.pop_back();
		auto dstIndex = values.back();
		values.pop_back();
		auto dstObject = values.back();
		values.pop_back();
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		const auto trapCall = [&](const auto& reportFn) {
			asmjit::InvokeNode* node = nullptr;
			checkError(compiler.invoke(asmjit::Out(node),
					reinterpret_cast<uint64_t>(reportFn), voidSignature),
					"AsmJit array.copy trap emission failed");
			emitPendingTrapReturn();
		};
		const auto nullCheck = [&](const asmjit::x86::Gp& object) {
			asmjit::Label ok = compiler.new_label();
			emitter.j(ok, asmjit::ujit::test_nz(object, object));
			trapCall(&asmjitReportNullArray);
			checkError(compiler.bind(ok), "AsmJit array.copy null label binding failed");
		};
		const auto rangeCheck = [&](const asmjit::x86::Gp& object, const asmjit::x86::Gp& index) {
			auto arrayLength = compiler.new_gp64("arrayLength");
			emitter.headerLoad32(arrayLength, asmjit::x86::dword_ptr(object,
					static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
			auto endIndex = compiler.new_gp64("arrayCopyEndIndex");
			emitter.mov(endIndex.r32(), index);
			auto wideLength = compiler.new_gp64("arrayCopyLength");
			emitter.mov(wideLength.r32(), length);
			emitter.add(endIndex, wideLength);
			asmjit::Label inBounds = compiler.new_label();
			emitter.j(inBounds, asmjit::ujit::ucmp_le(endIndex, arrayLength));
			trapCall(&asmjitReportArrayOob);
			checkError(compiler.bind(inBounds), "AsmJit array.copy range label binding failed");
		};
		nullCheck(dstObject);
		nullCheck(srcObject);
		rangeCheck(dstObject, dstIndex);
		rangeCheck(srcObject, srcIndex);
		const auto dataOffset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		const auto elementBytes = static_cast<int32_t>(WASM::gcStorageBytes(element.storage));
		int shift = 0;
		for (int bytes = elementBytes; bytes > 1; bytes >>= 1)
			++shift;
		const auto cursorFor = [&](const asmjit::x86::Gp& object, const asmjit::x86::Gp& index,
				const char* name) {
			auto cursor = compiler.new_gp64(name);
			emitter.mov(cursor, object);
			emitter.add(cursor, asmjit::Imm(dataOffset));
			auto offset = compiler.new_gp64("arrayCopyOffset");
			emitter.mov(offset.r32(), index);
			if (shift != 0)
				emitter.shl(offset, shift);
			emitter.add(cursor, offset);
			return cursor;
		};
		auto dstCursor = cursorFor(dstObject, dstIndex, "arrayCopyDst");
		auto srcCursor = cursorFor(srcObject, srcIndex, "arrayCopySrc");
		auto counter = compiler.new_gp64("arrayCopyCounter");
		emitter.mov(counter.r32(), length);
		asmjit::Label forward = compiler.new_label();
		asmjit::Label done = compiler.new_label();
		// memmove rule: forward when the destination starts at or below the source.
		emitter.j(forward, asmjit::ujit::ucmp_le(dstCursor, srcCursor));
		auto endOffset = compiler.new_gp64("arrayCopyEndOffset");
		emitter.mov(endOffset.r32(), length);
		if (shift != 0)
			emitter.shl(endOffset, shift);
		emitter.add(dstCursor, endOffset);
		emitter.add(srcCursor, endOffset);
		asmjit::Label back = compiler.new_label();
		checkError(compiler.bind(back), "AsmJit array.copy back label failed");
		emitter.j(done, asmjit::ujit::test_z(counter, counter));
		emitter.sub(dstCursor, asmjit::Imm(elementBytes));
		emitter.sub(srcCursor, asmjit::Imm(elementBytes));
		emitCopyTransfer(packed, packedI8, width, dstCursor, srcCursor);
		emitter.dec(counter);
		emitter.jmp(back);
		checkError(compiler.bind(forward), "AsmJit array.copy forward label failed");
		emitter.j(done, asmjit::ujit::test_z(counter, counter));
		emitCopyTransfer(packed, packedI8, width, dstCursor, srcCursor);
		emitter.add(dstCursor, asmjit::Imm(elementBytes));
		emitter.add(srcCursor, asmjit::Imm(elementBytes));
		emitter.dec(counter);
		emitter.jmp(forward);
		checkError(compiler.bind(done), "AsmJit array.copy done label failed");
		// Both containers were the operand stack's; the copy loop only read one
		// and wrote the other, so each one's count goes back.
		if (referenceValues.erase(dstObject.id()) != 0)
			emitRefDrop(dstObject);
		if (referenceValues.erase(srcObject.id()) != 0)
			emitRefDrop(srcObject);
	}

	void emitCopyTransfer(bool packed, bool packedI8, int width, const asmjit::x86::Gp& dst,
			const asmjit::x86::Gp& src)
	{
		auto value = compiler.new_gp64("arrayCopyValue");
		if (packed) {
			if (packedI8) {
				emitter.mov(value.r32(), asmjit::x86::byte_ptr(src));
				emitter.mov(asmjit::x86::byte_ptr(dst), value.r8());
			} else {
				emitter.mov(value.r32(), asmjit::x86::word_ptr(src));
				emitter.mov(asmjit::x86::word_ptr(dst), value.r16());
			}
		} else if (width == 4) {
			emitter.mov(value.r32(), asmjit::x86::dword_ptr(src));
			emitter.mov(asmjit::x86::dword_ptr(dst), value.r32());
		} else {
			emitter.mov(value, asmjit::x86::qword_ptr(src));
			emitter.mov(asmjit::x86::qword_ptr(dst), value);
		}
	}

	void dispatchArrayFill(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.fill requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2) : kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.fill does not lower this element type yet");
		if (values.size() < 4)
			throw std::runtime_error("AsmJit array.fill operand stack underflow");
		// Operand order (spec): [array, offset, value, count]. The value is the
		// second-from-top slot and the offset the third; naming them the other
		// way round made the fill treat the value as the offset (D-2).
		auto length = values.back();
		values.pop_back();
		auto elementValue = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		// Null first, then the range, so a null array reports the array kind.
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.fill null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.fill null label binding failed");
		// Bounds: (u64)index + (u64)length <= (u64)arrayLength.
		auto arrayLength = compiler.new_gp64("arrayLength");
		emitter.headerLoad32(arrayLength, asmjit::x86::dword_ptr(object,
					static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
		auto endIndex = compiler.new_gp64("arrayFillEndIndex");
		emitter.mov(endIndex.r32(), index);
		auto wideLength = compiler.new_gp64("arrayFillLength");
		emitter.mov(wideLength.r32(), length);
		emitter.add(endIndex, wideLength);
		asmjit::Label inBounds = compiler.new_label();
		emitter.j(inBounds, asmjit::ujit::ucmp_le(endIndex, arrayLength));
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.fill bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.fill bounds label binding failed");
		// Counted store loop: cursor advances, counter counts down.
		const auto dataOffset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		const auto elementBytes = static_cast<int32_t>(WASM::gcStorageBytes(element.storage));
		auto byteOffset = compiler.new_gp64("arrayFillByteOffset");
		emitter.mov(byteOffset.r32(), index);
		if (elementBytes != 1) {
			int shift = 0;
			for (int bytes = elementBytes; bytes > 1; bytes >>= 1)
				++shift;
			emitter.shl(byteOffset, shift);
		}
		auto cursor = compiler.new_gp64("arrayFillCursor");
		emitter.mov(cursor, object);
		emitter.add(cursor, asmjit::Imm(dataOffset));
		emitter.add(cursor, byteOffset);
		auto counter = compiler.new_gp64("arrayFillCounter");
		emitter.mov(counter.r32(), length);
		asmjit::Label loopStart = compiler.new_label();
		asmjit::Label loopDone = compiler.new_label();
		checkError(compiler.bind(loopStart), "AsmJit array.fill loop label failed");
		emitter.j(loopDone, asmjit::ujit::test_z(counter, counter));
		if (packed) {
			if (packedI8)
				emitter.mov(asmjit::x86::byte_ptr(cursor), elementValue.r8());
			else
				emitter.mov(asmjit::x86::word_ptr(cursor), elementValue.r16());
		} else if (width == 4)
			emitter.mov(asmjit::x86::dword_ptr(cursor), elementValue);
		else
			emitter.mov(asmjit::x86::qword_ptr(cursor), elementValue);
		emitter.add(cursor, asmjit::Imm(elementBytes));
		emitter.dec(counter);
		emitter.jmp(loopStart);
		checkError(compiler.bind(loopDone), "AsmJit array.fill loop label binding failed");
		// The array operand was the operand stack's and the fill only wrote
		// through it, so its count goes back. The fill value is left alone: a
		// reference element type has no per-element retain in this lowering yet,
		// and releasing the value here would leave every element pointing at a
		// dead object.
		if (referenceValues.erase(object.id()) != 0)
			emitRefDrop(object);
	}

	void dispatchArrayNew(WASM::TypeIdx arg) override
	{
		const WASM::CoreTypeView view = module.typeView(arg);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit array.new requires an array type");
		const WASM::CanonFieldType& element = view.arrayElement();
		const bool packed = element.storage.isPacked;
		const bool packedI8 = packed && element.storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2)
				: kindWidth(element.storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit array.new does not lower this element type yet");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit array.new operand stack underflow");
		auto length = values.back();
		values.pop_back();
		auto elementValue = values.back();
		values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitArrayNew), signature),
				"AsmJit array.new allocation call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(arg)));
		invoke->set_arg(2, length);
		auto object = compiler.new_gp_ptr("arrayObject");
		invoke->set_ret(0, object);
		emitPendingTrapReturn();
		// Every element must be initialised, not just element 0: the same counted
		// store loop array.fill uses.
		const auto dataOffset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		const auto elementBytes = static_cast<int32_t>(WASM::gcStorageBytes(element.storage));
		auto fillCursor = compiler.new_gp64("arrayNewCursor");
		emitter.mov(fillCursor, object);
		emitter.add(fillCursor, asmjit::Imm(dataOffset));
		auto fillCounter = compiler.new_gp64("arrayNewCounter");
		emitter.mov(fillCounter.r32(), length);
		asmjit::Label fillStart = compiler.new_label();
		asmjit::Label fillDone = compiler.new_label();
		checkError(compiler.bind(fillStart), "AsmJit array.new loop label failed");
		emitter.j(fillDone, asmjit::ujit::test_z(fillCounter, fillCounter));
		if (packed) {
			if (packedI8)
				emitter.mov(asmjit::x86::byte_ptr(fillCursor), elementValue.r8());
			else
				emitter.mov(asmjit::x86::word_ptr(fillCursor), elementValue.r16());
		} else if (width == 4)
			emitter.mov(asmjit::x86::dword_ptr(fillCursor), elementValue);
		else
			emitter.mov(asmjit::x86::qword_ptr(fillCursor), elementValue);
		emitter.add(fillCursor, asmjit::Imm(elementBytes));
		emitter.dec(fillCounter);
		emitter.jmp(fillStart);
		checkError(compiler.bind(fillDone), "AsmJit array.new loop label binding failed");
		values.push_back(object);
		// A fresh object is created unowned; the operand stack owns one count
		// per reference it holds, so the push takes one (RC-5).
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	void dispatchArrayLen() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit array.len operand stack underflow");
		auto object = values.back();
		values.pop_back();
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), nullSignature),
				"AsmJit array.len null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.len label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		emitter.headerLoad32(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset)));
		// The array operand was the operand stack's and the result is an i32,
		// so the stack gives its count up.
		if (referenceValues.erase(object.id()) != 0)
			emitRefDrop(object);
		values.push_back(length);
	}

	void emitStructGetPacked(WASM::TypeIdx arg1, uint32_t arg2, bool signExtend)
	{
		const WASM::CoreTypeView view = module.typeView(arg1);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.get_s/u requires a struct type");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (arg2 >= fields.size())
			throw std::runtime_error("AsmJit struct.get_s/u field index is out of range");
		if (!fields[arg2].storage.isPacked)
			throw std::runtime_error("AsmJit struct.get_s/u requires a packed field");
		if (values.empty())
			throw std::runtime_error("AsmJit struct.get_s/u operand stack underflow");
		auto object = values.back();
		values.pop_back();
		const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, arg2));
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullStructure), nullSignature),
				"AsmJit struct.get_s/u null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit struct.get_s/u label binding failed");
		auto result = compiler.new_gp32("structPackedField");
		const bool isI8 = fields[arg2].storage.packed == WASM::ValueTypeCode::I8;
		if (isI8) {
			if (signExtend)
				emitter.guestLoad8S(result, asmjit::x86::byte_ptr(object, offset));
			else
				emitter.guestLoad8Z(result, asmjit::x86::byte_ptr(object, offset));
		} else {
			if (signExtend)
				emitter.guestLoad16S(result, asmjit::x86::word_ptr(object, offset));
			else
				emitter.guestLoad16Z(result, asmjit::x86::word_ptr(object, offset));
		}
		// The container operand was the operand stack's, so its count goes
		// back. The field value is already in a fresh register and a packed
		// field holds no reference of its own.
		if (referenceValues.erase(object.id()) != 0)
			emitRefDrop(object);
		values.push_back(result);
	}

	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override
	{ emitStructGetPacked(arg1, arg2, true); }
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override
	{ emitStructGetPacked(arg1, arg2, false); }

	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		const WASM::CoreTypeView view = module.typeView(arg1);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.set requires a struct type");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (arg2 >= fields.size())
			throw std::runtime_error("AsmJit struct.set field index is out of range");
		const bool packed = fields[arg2].storage.isPacked;
		const bool packedI8 = packed && fields[arg2].storage.packed == WASM::ValueTypeCode::I8;
		const int width = packed ? (packedI8 ? 1 : 2)
				: kindWidth(fields[arg2].storage.value.opcode);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::runtime_error("AsmJit struct.set does not lower this field type yet");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.set operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, arg2));
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullStructure), nullSignature),
				"AsmJit struct.set null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit struct.set label binding failed");
		if (!packed && isReferenceKind(fields[arg2].storage.value.opcode)) {
			// A reference field is store-owned: the barrier retains the new value
			// for the field and releases the one it held. The operand stack then
			// gives up the counts it held for both operands -- the container and
			// the value -- which is safe from the registers after the store.
			asmjit::x86::Gp fieldAddress = compiler.new_gp_ptr("structSetField");
			emitter.lea(fieldAddress, asmjit::x86::qword_ptr(object, offset));
			emitRefStore(fieldAddress, value);
			emitRefDrop(object);
			emitRefDrop(value);
			return;
		}
		if (packed) {
			if (packedI8)
				emitter.guestStore8(asmjit::x86::byte_ptr(object, offset), value);
			else
				emitter.guestStore16(asmjit::x86::word_ptr(object, offset), value);
		} else if (width == 4)
			emitter.guestStore32(asmjit::x86::dword_ptr(object, offset), value);
		else
			emitter.guestStore64(asmjit::x86::qword_ptr(object, offset), value);
		// The field holds no reference, so the container operand's count is the
		// only one to give back.
		emitRefDrop(object);
	}

	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override
	{
		const WASM::CoreTypeView view = module.typeView(arg1);
		if (view.kind() != WASM::CanonTypeKind::Struct)
			throw std::runtime_error("AsmJit struct.get requires a struct type");
		const std::span<const WASM::CanonFieldType> fields = view.structFields();
		if (arg2 >= fields.size())
			throw std::runtime_error("AsmJit struct.get field index is out of range");
		if (values.empty())
			throw std::runtime_error("AsmJit struct.get operand stack underflow");
		const auto& field = fields[arg2];
		if (field.storage.isPacked)
			throw std::runtime_error("AsmJit struct.get does not lower packed fields yet");
		const int width = kindWidth(field.storage.value.opcode);
		if (width != 4 && width != 8)
			throw std::runtime_error("AsmJit struct.get does not lower this field type yet");
		auto object = values.back();
		values.pop_back();
		const std::size_t offset = WASM::gcStructFieldOffset(fields, arg2);
		// A null object traps, exactly as ref.as_non_null does.
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(object, object));
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullStructure), nullSignature),
				"AsmJit struct.get null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit struct.get label binding failed");
		auto result = width == 4 ? compiler.new_gp32("structField")
				: compiler.new_gp64("structField");
		if (width == 4)
			emitter.guestLoad32Z(result, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(offset)));
		else
			emitter.guestLoad64(result, asmjit::x86::qword_ptr(object,
				static_cast<int32_t>(offset)));
		// The container operand belonged to the operand stack, so its count goes
		// back. The field value is already in a fresh register, so no spill is
		// needed and the drop cannot invalidate it.
		emitRefDrop(object);
		if (isReferenceKind(field.storage.value.opcode)) {
			// A reference read out of the field belongs to the field; the operand
			// stack takes its own count, or a later `drop` would release the
			// field's and leave the struct pointing at a reclaimed object.
			result = emitRefClone(result);
			markReference(result);
		}
		values.push_back(result);
	}

	void dispatchTableGet(WASM::TableIdx tableIndex) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit table.get operand stack underflow");
		if (tableIndex >= module.importTables.size() + module.tables.size())
			throw std::runtime_error("AsmJit table.get references an unknown table");
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableGetIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableGetElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_table_get), signature),
				"AsmJit table.get helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		auto result = compiler.new_gp_ptr("tableGetResult");
		invoke->set_ret(0, result);
		emitPendingTrapReturn();
		// The entry is store-owned by the table; the operand stack takes its own
		// count, or a later `drop` would release the table's.
		result = emitRefClone(result);
		markReference(result);
		values.push_back(result);
	}

	void dispatchTableSet(WASM::TableIdx tableIndex) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit table.set operand stack underflow");
		if (tableIndex >= module.importTables.size() + module.tables.size())
			throw std::runtime_error("AsmJit table.set references an unknown table");
		auto value = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableSetIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableSetElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_table_set), signature),
				"AsmJit table.set helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		invoke->set_arg(3, value);
		emitPendingTrapReturn();
		// The slot went through the reference write barrier, which retained its
		// own count and released the value it overwrote; the count the operand
		// stack held on the incoming value is given back here (RC-5).
		if (isReferenceValue(value)) {
			referenceValues.erase(value.id());
			emitRefDrop(value);
		}
	}

	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx tableIndex) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit table.atomic.get operand stack underflow");
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableAtomicGetIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableAtomicGetElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_atomic_table_get), signature),
				"AsmJit table.atomic.get helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		invoke->set_arg(3, asmjit::Imm(ordering));
		auto result = compiler.new_gp_ptr("tableAtomicGetResult");
		invoke->set_ret(0, result);
		emitPendingTrapReturn();
		// The entry is store-owned by the table; the operand stack takes its own
		// count, or a later `drop` would release the table's.
		result = emitRefClone(result);
		markReference(result);
		values.push_back(result);
	}

	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx tableIndex) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit table.atomic.set operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableAtomicSetIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableAtomicSetElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_atomic_table_set), signature),
				"AsmJit table.atomic.set helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		invoke->set_arg(3, value);
		invoke->set_arg(4, asmjit::Imm(ordering));
		emitPendingTrapReturn();
	}

	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx tableIndex) override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit table.atomic.xchg operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableAtomicXchgIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableAtomicXchgElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_atomic_table_xchg), signature),
				"AsmJit table.atomic.xchg helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		invoke->set_arg(3, value);
		invoke->set_arg(4, asmjit::Imm(ordering));
		auto result = compiler.new_gp_ptr("tableAtomicXchgResult");
		invoke->set_ret(0, result);
		emitPendingTrapReturn();
		values.push_back(result);
	}

	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx tableIndex) override
	{
		if (values.size() < 3)
			throw std::runtime_error("AsmJit table.atomic.cmpxchg operand stack underflow");
		auto replacement = values.back();
		values.pop_back();
		auto expected = values.back();
		values.pop_back();
		auto index = values.back();
		values.pop_back();
		auto table = compiler.new_gp32("tableAtomicCmpxchgIndex");
		emitter.mov(table, static_cast<uint32_t>(tableIndex));
		asmjit::x86::Gp index64 = index;
		if (index.size() == 4) {
			index64 = compiler.new_gp64("tableAtomicCmpxchgElement");
			emitter.mov(index64.r32(), index.r32());
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_atomic_table_cmpxchg), signature),
				"AsmJit table.atomic.cmpxchg helper emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, table);
		invoke->set_arg(2, index64);
		invoke->set_arg(3, expected);
		invoke->set_arg(4, replacement);
		invoke->set_arg(5, asmjit::Imm(ordering));
		auto result = compiler.new_gp_ptr("tableAtomicCmpxchgResult");
		invoke->set_ret(0, result);
		emitPendingTrapReturn();
		values.push_back(result);
	}

	void dispatchRefFunc(WASM::FuncIdx index) override
	{
		const size_t importedCount = module.importFunctions.size();
		const size_t totalCount = importedCount + module.internalFunctionTypeIndices.size();
		if (index >= totalCount)
			throw std::runtime_error("AsmJit ref.func index is out of range");
		// A function reference is the instance's Callable*, the representation
		// SLJIT and LibJIT already use: call_ref needs the context and dispatch
		// fields, and pointer equality is the same as code-address equality.
		const void* referencePtr = index >= importedCount
				? static_cast<const void*>(&internals.internalCallables[index - importedCount])
				: static_cast<const void*>(&internals.importStorage[index]);
		if (index < importedCount
				&& (index >= internals.importStorage.size()
						|| internals.importStorage[index].fnPtr == nullptr))
			throw std::runtime_error("AsmJit ref.func references an unresolved function import");
		auto reference = compiler.new_gp_ptr("funcRef");
		emitter.mov(reference,
				asmjit::Imm(reinterpret_cast<uint64_t>(referencePtr)));
		// The Callable is store-owned by the instance; the operand stack takes
		// its own count so a later `drop` cannot release the instance's.
		markReference(reference);
		values.push_back(reference);
	}

	void emitRefCast(const WASM::HeapType& arg, bool nullable)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit ref.cast operand stack underflow");
		auto reference = values.back();
		const uint32_t value = arg.isTypeIndex ? arg.typeIndex
				: static_cast<uint32_t>(static_cast<int32_t>(arg.abstract));
		auto packed = packImmPair(arg.isTypeIndex ? 1u : 0u, value, "refCastHeap");
		asmjit::FuncSignature predicate(asmjit::CallConvId::kCDecl);
		predicate.set_ret(asmjit::TypeId::kInt32);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUInt64);
		predicate.add_arg(asmjit::TypeId::kUInt32);
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::Label afterCast = compiler.new_label();
		if (nullable) {
			emitter.j(afterCast, asmjit::ujit::test_z(reference, reference));
		} else {
			asmjit::Label notNull = compiler.new_label();
			emitter.j(notNull, asmjit::ujit::test_nz(reference, reference));
			asmjit::InvokeNode* nullCall = nullptr;
			checkError(compiler.invoke(asmjit::Out(nullCall),
					reinterpret_cast<uint64_t>(&asmjitReportCastFailure), voidSignature),
					"AsmJit ref.cast null trap emission failed");
			emitPendingTrapReturn();
			checkError(compiler.bind(notNull), "AsmJit ref.cast null label binding failed");
		}
		asmjit::InvokeNode* castCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(castCall),
				reinterpret_cast<uint64_t>(&asmjitRefMatchesHeapType), predicate),
				"AsmJit ref.cast predicate call failed");
		castCall->set_arg(0, vmContext);
		castCall->set_arg(1, reference);
		castCall->set_arg(2, packed);
		castCall->set_arg(3, asmjit::Imm(0));
		auto matches = compiler.new_gp32("refCastMatch");
		castCall->set_ret(0, matches);
		emitter.j(afterCast, asmjit::ujit::test_nz(matches, matches));
		asmjit::InvokeNode* failCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(failCall),
				reinterpret_cast<uint64_t>(&asmjitReportCastFailure), voidSignature),
				"AsmJit ref.cast failure trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(afterCast), "AsmJit ref.cast label binding failed");
	}

	void dispatchRefCast(const WASM::HeapType& arg) override { emitRefCast(arg, false); }
	void dispatchRefCastNull(const WASM::HeapType& arg) override { emitRefCast(arg, true); }

	void emitRefTest(const WASM::HeapType& arg, bool nullable)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit ref.test operand stack underflow");
		auto reference = values.back();
		values.pop_back();
		const uint32_t value = arg.isTypeIndex ? arg.typeIndex
				: static_cast<uint32_t>(static_cast<int32_t>(arg.abstract));
		auto packed = packImmPair(arg.isTypeIndex ? 1u : 0u, value, "refTestHeap");
		asmjit::FuncSignature predicate(asmjit::CallConvId::kCDecl);
		predicate.set_ret(asmjit::TypeId::kInt32);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUIntPtr);
		predicate.add_arg(asmjit::TypeId::kUInt64);
		predicate.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitRefMatchesHeapType), predicate),
				"AsmJit ref.test predicate call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, reference);
		invoke->set_arg(2, packed);
		invoke->set_arg(3, asmjit::Imm(nullable ? 1 : 0));
		auto result = compiler.new_gp32("refTest");
		invoke->set_ret(0, result);
		// The tested reference was the operand stack's and the result is an
		// i32, so the stack gives its count up.
		if (referenceValues.erase(reference.id()) != 0)
			emitRefDrop(reference);
		values.push_back(result);
	}

	void dispatchRefTest(const WASM::HeapType& arg) override { emitRefTest(arg, false); }
	void dispatchRefTestNull(const WASM::HeapType& arg) override { emitRefTest(arg, true); }

	void dispatchRefAsNonNull() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit ref.as_non_null operand underflow");
		auto reference = values.back();
		asmjit::Label notNull = compiler.new_label();
		emitter.j(notNull, asmjit::ujit::test_nz(reference, reference));
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitReportNullReference), signature),
				"AsmJit ref.as_non_null trap helper emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit ref.as_non_null label binding failed");
	}

	void dispatchRefEq() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit ref.eq operand underflow");
		auto rhs = values.back();
		values.pop_back();
		auto lhs = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("refEq");
		emitter.selectFlag(result, lhs, asmjit::ujit::cmp_eq(lhs, rhs));
		// Both operands were the operand stack's and the result is an i32, so
		// each one's count goes back. One `erase` per operand also covers the
		// case where both operands are the same value.
		const bool lhsOwned = referenceValues.erase(lhs.id()) != 0;
		const bool rhsOwned = referenceValues.erase(rhs.id()) != 0;
		if (lhsOwned)
			emitRefDrop(lhs);
		if (rhsOwned)
			emitRefDrop(rhs);
		values.push_back(result);
	}

	void dispatchI8x16Shuffle(std::span<uint8_t> selectors) override
	{
		if (selectors.size() != 16)
			throw std::runtime_error("AsmJit i8x16.shuffle selector is not 16 bytes");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i8x16.shuffle operand underflow");
		auto b = values.back();
		values.pop_back();
		auto a = values.back();
		values.pop_back();
		uint64_t lo = 0;
		uint64_t hi = 0;
		std::memcpy(&lo, selectors.data(), sizeof(lo));
		std::memcpy(&hi, selectors.data() + sizeof(lo), sizeof(hi));
		auto buffer = newVectorBuffer("v128Shuffle");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_i8x16_shuffle), signature),
				"AsmJit i8x16.shuffle helper call failed");
		invoke->set_arg(0, a);
		invoke->set_arg(1, b);
		invoke->set_arg(2, asmjit::Imm(static_cast<int64_t>(lo)));
		invoke->set_arg(3, asmjit::Imm(static_cast<int64_t>(hi)));
		invoke->set_arg(4, buffer);
		values.push_back(buffer);
	}

	void dispatchI8x16Add() override { emitVectorBinaryNative("AsmJit i8x16.add failed", 4); }
	void dispatchI8x16Sub() override { emitVectorBinaryNative("AsmJit i8x16.sub failed", 8); }
	void dispatchI16x8Add() override { emitVectorBinaryNative("AsmJit i16x8.add failed", 5); }
	void dispatchI16x8Sub() override { emitVectorBinaryNative("AsmJit i16x8.sub failed", 9); }
	void dispatchI16x8Mul() override { emitVectorHelper2(&wasm_i16x8_mul); }
	void dispatchI32x4Add() override { emitVectorBinaryNative("AsmJit i32x4.add failed", 6); }
	void dispatchI32x4Sub() override { emitVectorBinaryNative("AsmJit i32x4.sub failed", 10); }
	void dispatchI32x4Mul() override { emitVectorHelper2(&wasm_i32x4_mul); }
	void dispatchI64x2Add() override { emitVectorBinaryNative("AsmJit i64x2.add failed", 7); }
	void dispatchI64x2Sub() override { emitVectorBinaryNative("AsmJit i64x2.sub failed", 11); }
	void dispatchI64x2Mul() override { emitVectorHelper2(&wasm_i64x2_mul); }
	void dispatchI8x16Eq() override { emitVectorBinaryNative("AsmJit i8x16.eq failed", 18); }
	void dispatchI8x16Ne() override { emitVectorBinaryNative("AsmJit i8x16.ne failed", 21); }
	void dispatchI8x16LtS() override { emitVectorBinaryNative("AsmJit i8x16.lt_s failed", 24); }
	void dispatchI8x16LtU() override { emitVectorHelper2(&wasm_i8x16_lt_u); }
	void dispatchI8x16GtS() override { emitVectorBinaryNative("AsmJit i8x16.gt_s failed", 27); }
	void dispatchI8x16GtU() override { emitVectorHelper2(&wasm_i8x16_gt_u); }
	void dispatchI8x16LeS() override { emitVectorBinaryNative("AsmJit i8x16.le_s failed", 30); }
	void dispatchI8x16LeU() override { emitVectorHelper2(&wasm_i8x16_le_u); }
	void dispatchI8x16GeS() override { emitVectorBinaryNative("AsmJit i8x16.ge_s failed", 33); }
	void dispatchI8x16GeU() override { emitVectorHelper2(&wasm_i8x16_ge_u); }
	void dispatchI16x8Eq() override { emitVectorBinaryNative("AsmJit i16x8.eq failed", 19); }
	void dispatchI16x8Ne() override { emitVectorBinaryNative("AsmJit i16x8.ne failed", 22); }
	void dispatchI16x8LtS() override { emitVectorBinaryNative("AsmJit i16x8.lt_s failed", 25); }
	void dispatchI16x8LtU() override { emitVectorHelper2(&wasm_i16x8_lt_u); }
	void dispatchI16x8GtS() override { emitVectorBinaryNative("AsmJit i16x8.gt_s failed", 28); }
	void dispatchI16x8GtU() override { emitVectorHelper2(&wasm_i16x8_gt_u); }
	void dispatchI16x8LeS() override { emitVectorBinaryNative("AsmJit i16x8.le_s failed", 31); }
	void dispatchI16x8LeU() override { emitVectorHelper2(&wasm_i16x8_le_u); }
	void dispatchI16x8GeS() override { emitVectorBinaryNative("AsmJit i16x8.ge_s failed", 34); }
	void dispatchI16x8GeU() override { emitVectorHelper2(&wasm_i16x8_ge_u); }
	void dispatchI32x4Eq() override { emitVectorBinaryNative("AsmJit i32x4.eq failed", 20); }
	void dispatchI32x4Ne() override { emitVectorBinaryNative("AsmJit i32x4.ne failed", 23); }
	void dispatchI32x4LtS() override { emitVectorBinaryNative("AsmJit i32x4.lt_s failed", 26); }
	void dispatchI32x4LtU() override { emitVectorHelper2(&wasm_i32x4_lt_u); }
	void dispatchI32x4GtS() override { emitVectorBinaryNative("AsmJit i32x4.gt_s failed", 29); }
	void dispatchI32x4GtU() override { emitVectorHelper2(&wasm_i32x4_gt_u); }
	void dispatchI32x4LeS() override { emitVectorBinaryNative("AsmJit i32x4.le_s failed", 32); }
	void dispatchI32x4LeU() override { emitVectorHelper2(&wasm_i32x4_le_u); }
	void dispatchI32x4GeS() override { emitVectorBinaryNative("AsmJit i32x4.ge_s failed", 35); }
	void dispatchI32x4GeU() override { emitVectorHelper2(&wasm_i32x4_ge_u); }
	void dispatchI64x2Eq() override { emitVectorHelper2(&wasm_i64x2_eq); }
	void dispatchI64x2Ne() override { emitVectorHelper2(&wasm_i64x2_ne); }
	void dispatchI64x2LtS() override { emitVectorHelper2(&wasm_i64x2_lt_s); }
	void dispatchI64x2GtS() override { emitVectorHelper2(&wasm_i64x2_gt_s); }
	void dispatchI64x2LeS() override { emitVectorHelper2(&wasm_i64x2_le_s); }
	void dispatchI64x2GeS() override { emitVectorHelper2(&wasm_i64x2_ge_s); }

	void dispatchI8x16Neg() override { emitVectorHelper1(&wasm_i8x16_neg); }
	void dispatchI16x8Neg() override { emitVectorHelper1(&wasm_i16x8_neg); }
	void dispatchI32x4Neg() override { emitVectorHelper1(&wasm_i32x4_neg); }
	void dispatchI64x2Neg() override { emitVectorHelper1(&wasm_i64x2_neg); }

	void dispatchI8x16AllTrue() override { emitVectorReduce(&wasm_i8x16_all_true); }
	void dispatchI16x8AllTrue() override { emitVectorReduce(&wasm_i16x8_all_true); }
	void dispatchI32x4AllTrue() override { emitVectorReduce(&wasm_i32x4_all_true); }
	void dispatchI64x2AllTrue() override { emitVectorReduce(&wasm_i64x2_all_true); }
	void dispatchI8x16Bitmask() override { emitVectorReduce(&wasm_i8x16_bitmask); }
	void dispatchI16x8Bitmask() override { emitVectorReduce(&wasm_i16x8_bitmask); }
	void dispatchI32x4Bitmask() override { emitVectorReduce(&wasm_i32x4_bitmask); }
	void dispatchI64x2Bitmask() override { emitVectorReduce(&wasm_i64x2_bitmask); }

	void dispatchI32Load(WASM::MemArg arg) override { emitMemoryLoad(arg, 4, LoadKind::I32); }
	void dispatchI32Load8S(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I8Signed); }
	void dispatchI32Load8U(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I8Unsigned); }
	void dispatchI32Load16S(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I16Signed); }
	void dispatchI32Load16U(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I16Unsigned); }
	void dispatchI32Store(WASM::MemArg arg) override { emitMemoryStore(arg, 4); }
	void dispatchI32Store8(WASM::MemArg arg) override { emitMemoryStore(arg, 1); }
	void dispatchI32Store16(WASM::MemArg arg) override { emitMemoryStore(arg, 2); }
	void dispatchI64Load(WASM::MemArg arg) override { emitMemoryLoad(arg, 8, LoadKind::I64); }
	void dispatchI64Load8S(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I64I8Signed); }
	void dispatchI64Load8U(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I64I8Unsigned); }
	void dispatchI64Load16S(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I64I16Signed); }
	void dispatchI64Load16U(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I64I16Unsigned); }
	void dispatchI64Load32S(WASM::MemArg arg) override { emitMemoryLoad(arg, 4, LoadKind::I64I32Signed); }
	void dispatchI64Load32U(WASM::MemArg arg) override { emitMemoryLoad(arg, 4, LoadKind::I64I32Unsigned); }
	void dispatchI64Store(WASM::MemArg arg) override { emitMemoryStore(arg, 8); }
	void dispatchI64Store8(WASM::MemArg arg) override { emitMemoryStore(arg, 1); }
	void dispatchI64Store16(WASM::MemArg arg) override { emitMemoryStore(arg, 2); }
	void dispatchI64Store32(WASM::MemArg arg) override { emitMemoryStore(arg, 4); }

	void dispatchUnreachable() override
	{
		armTerminated = true;
		const char* detail = internals.internTrapMessage("unreachable in function "
				+ module.describeIndex(WASM::NameKind::Function, functionIndex));
		// Report *at* the site: `unreachable` has a compile-time location, so it
		// must not depend on a helper annotation to acquire one
		// (docs/DIAGNOSTICS.md DG-2).
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_trap_report_at), signature),
				"AsmJit unreachable trap helper emission failed");
		invoke->set_arg(0, asmjit::Imm(static_cast<uint32_t>(WASM::TrapKind::Unreachable)));
		invoke->set_arg(1, asmjit::Imm(reinterpret_cast<uint64_t>(detail)));
		invoke->set_arg(2, asmjit::Imm(functionIndex));
		invoke->set_arg(3, asmjit::Imm(static_cast<uint32_t>(instructionOffset)));
		invoke->set_arg(4, asmjit::Imm(reinterpret_cast<uint64_t>(functionName)));
		const size_t stackHeight = controls.empty() ? 0 : controls.back().stackHeight;
		const size_t placeholderCount = controls.empty() ? resultCount : controls.back().resultCount;
		values.resize(stackHeight);
		// An explicit `unreachable` is an exit path like any other: the frame
		// returns from here without reaching the return epilogue, so the reference
		// locals give their counts back first (docs/WRITING_A_BACKEND.md section 6).
		emitTrapLocalRelease();
		if (frameMode || resultCount == 0 || bufferedVectorResult) {
			checkError(compiler.ret(), "AsmJit unreachable void return emission failed");
		} else if (resultCount == 1) {
			const WASM::ValueTypeCode code = resultTypes.front();
			if (isFloatKind(code)) {
				auto zero = compiler.new_xmm("unreachableFloatResult");
				emitter.vectorZero(zero);
				checkError(compiler.ret(zero), "AsmJit unreachable float return emission failed");
			} else {
				auto zero = kindWidth(code) == 8
						? compiler.new_gp64("unreachableResult")
						: compiler.new_gp32("unreachableResult");
				emitter.xor_(zero, zero);
				checkError(compiler.ret(zero), "AsmJit unreachable result return emission failed");
			}
		} else {
			checkError(compiler.ret(), "AsmJit unreachable multi-result return failed");
		}
		for (size_t i = 0; i < placeholderCount; ++i) {
			auto placeholder = compiler.new_gp32("unreachableStackPlaceholder");
			emitter.xor_(placeholder, placeholder);
			values.push_back(placeholder);
		}
	}

	void dispatchNop() override {}

	void dispatchI32Extend8S() override { emitSignExtend(8); }
	void dispatchI32Extend16S() override { emitSignExtend(16); }
	void dispatchI32WrapI64() override { emitI32WrapI64(); }
	void dispatchI64ExtendI32S() override { emitI64ExtendI32(true); }
	void dispatchI64ExtendI32U() override { emitI64ExtendI32(false); }
	void dispatchI64Extend8S() override { emitI64SignExtend(8); }
	void dispatchI64Extend16S() override { emitI64SignExtend(16); }
	void dispatchI64Extend32S() override { emitI64SignExtend(32); }

	void dispatchI32Clz() override
	{
		if (inDeadCode()) return; // polymorphic dead code after br/return
		emitBitCount(BitCountOp::LeadingZeros);
	}
	void dispatchI32Ctz() override
	{
		if (inDeadCode()) return;
		emitBitCount(BitCountOp::TrailingZeros);
	}
	void dispatchI32Popcnt() override
	{
		if (inDeadCode()) return;
		emitUnaryCall(&asmjitI32Popcnt, "i32.popcnt");
	}
	void dispatchI32DivS() override { emitCheckedBinaryCall(&asmjitI32DivS, "i32.div_s"); }
	void dispatchI32DivU() override { emitCheckedBinaryCall(&asmjitI32DivU, "i32.div_u"); }
	void dispatchI32RemS() override { emitCheckedBinaryCall(&asmjitI32RemS, "i32.rem_s"); }
	void dispatchI32RemU() override { emitCheckedBinaryCall(&asmjitI32RemU, "i32.rem_u"); }

	void dispatchI64Const(int64_t value) override
	{
		auto reg = compiler.new_gp64("i64Constant");
		emitter.movImm(reg, value);
		values.push_back(reg);
	}

	void dispatchI64Eqz() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit i64.eqz operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("i64Eqz");
		emitter.selectFlag(result, value, asmjit::ujit::test_z(value, value));
		values.push_back(result);
	}

	void dispatchI64Eq() override { emitCompare(asmjit::x86::CondCode::kE); }
	void dispatchI64Ne() override { emitCompare(asmjit::x86::CondCode::kNE); }
	void dispatchI64LtS() override { emitCompare(asmjit::x86::CondCode::kL); }
	void dispatchI64LtU() override { emitCompare(asmjit::x86::CondCode::kB); }
	void dispatchI64GtS() override { emitCompare(asmjit::x86::CondCode::kG); }
	void dispatchI64GtU() override { emitCompare(asmjit::x86::CondCode::kA); }
	void dispatchI64LeS() override { emitCompare(asmjit::x86::CondCode::kLE); }
	void dispatchI64LeU() override { emitCompare(asmjit::x86::CondCode::kBE); }
	void dispatchI64GeS() override { emitCompare(asmjit::x86::CondCode::kGE); }
	void dispatchI64GeU() override { emitCompare(asmjit::x86::CondCode::kAE); }

	void dispatchStringConst(uint32_t idx) override
	{
		if (inDeadCode())
			return;
		if (idx >= internals.stringLiterals.size())
			throw std::runtime_error("AsmJit string.const index is out of range");
		auto result = compiler.new_gp_ptr("stringConstant");
		emitter.mov(result,
				reinterpret_cast<uint64_t>(internals.stringLiterals[idx]));
		values.push_back(result);
	}

	void dispatchI32Itt(uint32_t immediate) override
	{
		// Compile-time-only marker: report the immediate and emit no instruction.
		WASM::traceImmediate(immediate);
	}

	void dispatchRoundingVariant(uint32_t sub) override
	{
		if (inDeadCode())
			return;
		const uint32_t relative = (sub - 0x80u) % 0x14u;
		const auto unary = [&](uint64_t helper, int width) {
			if (values.empty())
				throw std::runtime_error("AsmJit rounding operand underflow");
			const auto bits = values.back(); values.pop_back();
			const auto argument = materializeFloat(bits, width);
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(width == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			signature.add_arg(asmjit::TypeId::kUInt32);
			signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
					"AsmJit rounding unary helper failed");
			invoke->set_arg(0, asmjit::Imm(sub));
			invoke->set_arg(1, argument);
			auto result = compiler.new_xmm("roundingResult");
			invoke->set_ret(0, result);
			values.push_back(bitsOfFloat(result, width));
		};
		const auto binary = [&](uint64_t helper, int width) {
			if (values.size() < 2)
				throw std::runtime_error("AsmJit rounding operand underflow");
			const auto second = values.back(); values.pop_back();
			const auto first = values.back(); values.pop_back();
			const auto a = materializeFloat(first, width);
			const auto b = materializeFloat(second, width);
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(width == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			signature.add_arg(asmjit::TypeId::kUInt32);
			signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
					"AsmJit rounding binary helper failed");
			invoke->set_arg(0, asmjit::Imm(sub));
			invoke->set_arg(1, a);
			invoke->set_arg(2, b);
			auto result = compiler.new_xmm("roundingResult");
			invoke->set_ret(0, result);
			values.push_back(bitsOfFloat(result, width));
		};
		const auto convertInt = [&](uint64_t helper, int intWidth, int floatWidth) {
			if (values.empty())
				throw std::runtime_error("AsmJit rounding conversion underflow");
			const auto source = values.back(); values.pop_back();
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(floatWidth == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			signature.add_arg(asmjit::TypeId::kUInt32);
			signature.add_arg(intWidth == 4 ? asmjit::TypeId::kInt32
					: asmjit::TypeId::kInt64);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
					"AsmJit rounding conversion helper failed");
			invoke->set_arg(0, asmjit::Imm(sub));
			invoke->set_arg(1, source);
			auto result = compiler.new_xmm("roundingResult");
			invoke->set_ret(0, result);
			values.push_back(bitsOfFloat(result, floatWidth));
		};
		const auto convertFloat = [&](uint64_t helper, int sourceWidth, int resultWidth) {
			if (values.empty())
				throw std::runtime_error("AsmJit rounding conversion underflow");
			const auto bits = values.back(); values.pop_back();
			const auto argument = materializeFloat(bits, sourceWidth);
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(resultWidth == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			signature.add_arg(asmjit::TypeId::kUInt32);
			signature.add_arg(sourceWidth == 4 ? asmjit::TypeId::kFloat32
					: asmjit::TypeId::kFloat64);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
					"AsmJit rounding float conversion failed");
			invoke->set_arg(0, asmjit::Imm(sub));
			invoke->set_arg(1, argument);
			auto result = compiler.new_xmm("roundingResult");
			invoke->set_ret(0, result);
			values.push_back(bitsOfFloat(result, resultWidth));
		};

		if (relative < 5) {
			if (relative == 0)
				unary(reinterpret_cast<uint64_t>(&wasm_rounding_f32_unary), 4);
			else
				binary(reinterpret_cast<uint64_t>(&wasm_rounding_f32_binary), 4);
		} else if (relative < 10) {
			if (relative == 5)
				unary(reinterpret_cast<uint64_t>(&wasm_rounding_f64_unary), 8);
			else
				binary(reinterpret_cast<uint64_t>(&wasm_rounding_f64_binary), 8);
		} else if (relative < 15) {
			switch (relative) {
			case 10: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i32_s_to_f32), 4, 4); break;
			case 11: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i32_u_to_f32), 4, 4); break;
			case 12: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i64_s_to_f32), 8, 4); break;
			case 13: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i64_u_to_f32), 8, 4); break;
			default: convertFloat(reinterpret_cast<uint64_t>(&wasm_rounding_f64_to_f32), 8, 4); break;
			}
		} else {
			switch (relative) {
			case 15: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i32_s_to_f64), 4, 8); break;
			case 16: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i32_u_to_f64), 4, 8); break;
			case 17: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i64_s_to_f64), 8, 8); break;
			case 18: convertInt(reinterpret_cast<uint64_t>(&wasm_rounding_i64_u_to_f64), 8, 8); break;
			default: convertFloat(reinterpret_cast<uint64_t>(&wasm_rounding_f32_to_f64), 4, 8); break;
			}
		}
	}

	void dispatchMemoryDiscard(WASM::MemIdx memory) override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit memory.discard operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto destination = values.back(); values.pop_back();
		const bool is64 = validateMemoryIndex(memory).indexType
				== WASM::MemoryIndexType::I64;
		auto widen = [&](const asmjit::x86::Gp& value) {
			if (value.size() == 8)
				return value;
			auto wide = compiler.new_gp64("memoryDiscardOperand");
			emitter.mov(wide.r32(), value.r32());
			return wide;
		};
		auto destination64 = widen(destination);
		auto length64 = widen(length);
		auto memoryIndex = compiler.new_gp32("memoryDiscardIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(memory));
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_memory_discard), signature),
				"AsmJit memory.discard helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, destination64);
		invoke->set_arg(3, length64);
		emitPendingTrapReturn();
	}

	void dispatchStringMeasureUtf8() override
	{
		emitStringMeasure(reinterpret_cast<uint64_t>(&wasm_string_measure_utf8));
	}
	void dispatchStringMeasureWtf8() override
	{
		emitStringMeasure(reinterpret_cast<uint64_t>(&wasm_string_measure_wtf8));
	}
	void dispatchStringMeasureWtf16() override
	{
		emitStringMeasure(reinterpret_cast<uint64_t>(&wasm_string_measure_wtf16));
	}
	void dispatchStringIsUsvSequence() override
	{
		emitStringMeasure(reinterpret_cast<uint64_t>(&wasm_string_is_usv_sequence));
	}

	void dispatchStringEq() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit string.eq operand stack underflow");
		auto right = values.back(); values.pop_back();
		auto left = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_eq), signature),
				"AsmJit string.eq helper call failed");
		invoke->set_arg(0, left);
		invoke->set_arg(1, right);
		auto result = compiler.new_gp32("stringEqResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringConcat() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit string.concat operand stack underflow");
		auto right = values.back(); values.pop_back();
		auto left = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_concat), signature),
				"AsmJit string.concat helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, left);
		invoke->set_arg(2, right);
		auto result = compiler.new_gp_ptr("stringConcatResult");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	void emitStringMeasure(uint64_t helper)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit string measure operand stack underflow");
		auto string = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit string measure helper call failed");
		invoke->set_arg(0, string);
		auto result = compiler.new_gp32("stringMeasureResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringNewUtf8(WASM::MemIdx memory) override
	{
		emitStringNew(memory, reinterpret_cast<uint64_t>(&wasm_string_new_utf8));
	}
	void dispatchStringNewLossyUtf8(WASM::MemIdx memory) override
	{
		emitStringNew(memory, reinterpret_cast<uint64_t>(&wasm_string_new_lossy_utf8));
	}
	void dispatchStringNewWtf8(WASM::MemIdx memory) override
	{
		emitStringNew(memory, reinterpret_cast<uint64_t>(&wasm_string_new_wtf8));
	}
	void dispatchStringNewWtf16(WASM::MemIdx memory) override
	{
		emitStringNew(memory, reinterpret_cast<uint64_t>(&wasm_string_new_wtf16));
	}
	void dispatchStringEncodeUtf8(WASM::MemIdx memory) override
	{
		emitStringEncode(memory, reinterpret_cast<uint64_t>(&wasm_string_encode_utf8));
	}
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx memory) override
	{
		emitStringEncode(memory, reinterpret_cast<uint64_t>(&wasm_string_encode_lossy_utf8));
	}
	void dispatchStringEncodeWtf8(WASM::MemIdx memory) override
	{
		emitStringEncode(memory, reinterpret_cast<uint64_t>(&wasm_string_encode_wtf8));
	}
	void dispatchStringEncodeWtf16(WASM::MemIdx memory) override
	{
		emitStringEncode(memory, reinterpret_cast<uint64_t>(&wasm_string_encode_wtf16));
	}

	void emitStringNew(WASM::MemIdx memory, uint64_t helper)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit string.new operand stack underflow");
		auto length = values.back(); values.pop_back();
		auto pointer = values.back(); values.pop_back();
		auto memoryIndex = compiler.new_gp32("stringMemoryIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(memory));
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit string.new helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, pointer);
		invoke->set_arg(3, length);
		auto result = compiler.new_gp_ptr("stringNewResult");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	void emitStringEncode(WASM::MemIdx memory, uint64_t helper)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit string.encode operand stack underflow");
		auto pointer = values.back(); values.pop_back();
		auto string = values.back(); values.pop_back();
		auto memoryIndex = compiler.new_gp32("stringMemoryIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(memory));
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit string.encode helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, string);
		invoke->set_arg(3, pointer);
		auto result = compiler.new_gp32("stringEncodeResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringNewUtf8Array(WASM::TypeIdx type) override
	{
		emitStringNewArray(type, 0);
	}
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx type) override
	{
		emitStringNewArray(type, 1);
	}
	void dispatchStringNewWtf8Array(WASM::TypeIdx type) override
	{
		emitStringNewArray(type, 2);
	}
	void dispatchStringNewWtf16Array(WASM::TypeIdx type) override
	{
		emitStringNewArray(type, 3);
	}
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx type) override
	{
		emitStringEncodeArray(type, 0);
	}
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx type) override
	{
		emitStringEncodeArray(type, 1);
	}
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx type) override
	{
		emitStringEncodeArray(type, 2);
	}
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx type) override
	{
		emitStringEncodeArray(type, 3);
	}

	void emitStringViewCreate(uint64_t helper)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit string view operand stack underflow");
		auto string = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit string view helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, string);
		auto result = compiler.new_gp_ptr("stringViewResult");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	void emitStringViewRange(uint64_t helper)
	{
		if (inDeadCode())
			return;
		if (values.size() < 3)
			throw std::runtime_error("AsmJit stringview slice operand stack underflow");
		auto end = values.back(); values.pop_back();
		auto start = values.back(); values.pop_back();
		auto view = values.back(); values.pop_back();
		auto range = packRegPair(end, start, "stringViewRange");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit stringview slice helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, view);
		invoke->set_arg(2, range);
		auto result = compiler.new_gp_ptr("stringViewSlice");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	void emitStringViewWtf8Encode(uint64_t helper, WASM::MemIdx memory)
	{
		if (inDeadCode())
			return;
		validateMemoryIndex(memory);
		if (values.size() < 4)
			throw std::runtime_error("AsmJit stringview encode operand stack underflow");
		auto bytes = values.back(); values.pop_back();
		auto position = values.back(); values.pop_back();
		auto pointer = values.back(); values.pop_back();
		auto view = values.back(); values.pop_back();
		auto packedMemory = compiler.new_gp64("stringViewMemory");
		auto memoryIndex = compiler.new_gp64("stringViewMemoryIndex");
		emitter.mov(packedMemory.r32(), pointer.r32());
		emitter.shl(packedMemory, 8);
		emitter.mov(memoryIndex.r32(), static_cast<uint32_t>(memory));
		emitter.or_(packedMemory, memoryIndex);
		auto packedPosition = packRegPair(bytes, position, "stringViewPosition");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit stringview encode helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, view);
		invoke->set_arg(2, packedMemory);
		invoke->set_arg(3, packedPosition);
		auto result = compiler.new_gp64("stringViewEncoded");
		invoke->set_ret(0, result);
		auto nextPosition = compiler.new_gp32("stringViewNext");
		auto high = compiler.new_gp64("stringViewNextHigh");
		emitter.mov(high, result);
		emitter.shr(high, 32);
		emitter.mov(nextPosition.r32(), high.r32());
		auto written = compiler.new_gp32("stringViewWritten");
		emitter.mov(written.r32(), result.r32());
		values.push_back(nextPosition);
		values.push_back(written);
		emitPendingTrapReturn();
	}

	void emitStringViewWtf16Encode(WASM::MemIdx memory)
	{
		if (inDeadCode())
			return;
		validateMemoryIndex(memory);
		if (values.size() < 4)
			throw std::runtime_error("AsmJit stringview_wtf16.encode operand stack underflow");
		auto units = values.back(); values.pop_back();
		auto position = values.back(); values.pop_back();
		auto pointer = values.back(); values.pop_back();
		auto view = values.back(); values.pop_back();
		auto packedMemory = compiler.new_gp64("stringViewMemory");
		auto memoryIndex = compiler.new_gp64("stringViewMemoryIndex");
		emitter.mov(packedMemory.r32(), pointer.r32());
		emitter.shl(packedMemory, 8);
		emitter.mov(memoryIndex.r32(), static_cast<uint32_t>(memory));
		emitter.or_(packedMemory, memoryIndex);
		auto packedPosition = packRegPair(units, position, "stringViewPosition");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf16_encode), signature),
				"AsmJit stringview_wtf16.encode helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, view);
		invoke->set_arg(2, packedMemory);
		invoke->set_arg(3, packedPosition);
		auto result = compiler.new_gp32("stringViewEncodedUnits");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringAsWtf8() override
	{
		emitStringViewCreate(reinterpret_cast<uint64_t>(&wasm_string_as_wtf8));
	}
	void dispatchStringAsWtf16() override
	{
		emitStringViewCreate(reinterpret_cast<uint64_t>(&wasm_string_as_wtf16));
	}
	void dispatchStringAsIter() override
	{
		emitStringViewCreate(reinterpret_cast<uint64_t>(&wasm_string_as_iter));
	}

	void dispatchStringViewWtf8Advance() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 3)
			throw std::runtime_error("AsmJit stringview_wtf8.advance operand stack underflow");
		auto bytes = values.back(); values.pop_back();
		auto position = values.back(); values.pop_back();
		auto view = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf8_advance), signature),
				"AsmJit stringview_wtf8.advance helper call failed");
		invoke->set_arg(0, view);
		invoke->set_arg(1, position);
		invoke->set_arg(2, bytes);
		auto result = compiler.new_gp32("stringViewPosition");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx memory) override
	{
		emitStringViewWtf8Encode(
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf8_encode_utf8), memory);
	}
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx memory) override
	{
		emitStringViewWtf8Encode(
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf8_encode_lossy_utf8),
				memory);
	}
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx memory) override
	{
		emitStringViewWtf8Encode(
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf8_encode_wtf8), memory);
	}
	void dispatchStringViewWtf8Slice() override
	{
		emitStringViewRange(
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf8_slice));
	}

	void dispatchStringViewWtf16Length() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit stringview_wtf16.length operand stack underflow");
		auto view = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf16_length), signature),
				"AsmJit stringview_wtf16.length helper call failed");
		invoke->set_arg(0, view);
		auto result = compiler.new_gp32("stringViewLength");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringViewWtf16GetCodeUnit() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit stringview_wtf16.get_codeunit operand stack underflow");
		auto position = values.back(); values.pop_back();
		auto view = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf16_get_codeunit),
				signature),
				"AsmJit stringview_wtf16.get_codeunit helper call failed");
		invoke->set_arg(0, view);
		invoke->set_arg(1, position);
		auto result = compiler.new_gp32("stringViewCodeUnit");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringViewWtf16Encode(WASM::MemIdx memory) override
	{
		emitStringViewWtf16Encode(memory);
	}
	void dispatchStringViewWtf16Slice() override
	{
		emitStringViewRange(
				reinterpret_cast<uint64_t>(&wasm_string_view_wtf16_slice));
	}

	void dispatchStringViewIterNext() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit stringview_iter.next operand stack underflow");
		auto iterator = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_iter_next), signature),
				"AsmJit stringview_iter.next helper call failed");
		invoke->set_arg(0, iterator);
		auto result = compiler.new_gp32("stringIterCodePoint");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void emitStringIterMove(uint64_t helper, const char* opcode)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error(std::string("AsmJit ") + opcode
					+ " operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto iterator = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kInt32);
		asmjit::InvokeNode* invoke = nullptr;
		const std::string failure = std::string("AsmJit ") + opcode
				+ " helper call failed";
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				failure.c_str());
		invoke->set_arg(0, iterator);
		invoke->set_arg(1, count);
		auto result = compiler.new_gp32("stringIterMoved");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchStringViewIterAdvance() override
	{
		emitStringIterMove(
				reinterpret_cast<uint64_t>(&wasm_string_view_iter_advance),
				"stringview_iter.advance");
	}
	void dispatchStringViewIterRewind() override
	{
		emitStringIterMove(
				reinterpret_cast<uint64_t>(&wasm_string_view_iter_rewind),
				"stringview_iter.rewind");
	}

	void dispatchStringViewIterSlice() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit stringview_iter.slice operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto iterator = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_view_iter_slice), signature),
				"AsmJit stringview_iter.slice helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, iterator);
		invoke->set_arg(2, count);
		auto result = compiler.new_gp_ptr("stringIterSlice");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	WASM::FuncType continuationSignature(WASM::TypeIdx type) const
	{
		if (type >= module.typeCount())
			throw std::runtime_error("AsmJit continuation type index is out of range");
		const WASM::CoreTypeView view = module.typeView(type);
		if (view.kind() != WASM::CanonTypeKind::Cont)
			throw std::runtime_error("AsmJit continuation instruction does not name a cont type");
		const WASM::CanonHeapType func = view.contFunc();
		if (func.kind != WASM::CanonHeapType::Kind::Global)
			throw std::runtime_error("AsmJit continuation function type is not module-local");
		for (uint32_t i = 0; i < module.typeCount(); ++i)
			if (module.typeIds[i] == func.id)
				return module.funcTypeAt(i);
		throw std::runtime_error("AsmJit continuation function type is not in this module");
	}

	void dispatchContNew(WASM::TypeIdx type) override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit cont.new operand stack underflow");
		auto callable = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_cont_new_ops), signature),
				"AsmJit cont.new helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, callable);
		invoke->set_arg(2, asmjit::Imm(static_cast<uint32_t>(type)));
		auto result = compiler.new_gp_ptr("continuation");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void emitContinuationFrameArguments(const asmjit::x86::Mem& frame,
			const std::vector<asmjit::x86::Gp>& operands,
			const WASM::FuncType& function)
	{
		auto at = [&](int32_t offset) {
			auto memory = frame;
			memory.set_offset(frame.offset() + offset);
			return memory;
		};
		for (std::size_t i = 0; i < operands.size(); ++i) {
			const int width = kindWidth(function.params[i].val.opcode);
			const int32_t offset = static_cast<int32_t>(i * WASM::kContinuationValueStride);
			if (width == 16) {
				auto high = compiler.new_gp64("continuationVectorHigh");
				emitter.mov(high,
						asmjit::x86::qword_ptr(operands[i], 8));
				auto low = compiler.new_gp64("continuationVectorLow");
				emitter.mov(low, asmjit::x86::qword_ptr(operands[i]));
				emitter.mov(at(offset), low);
				emitter.mov(at(offset + 8), high);
			} else if (width == 8) {
				emitter.mov(at(offset), operands[i]);
			} else {
				emitter.mov(at(offset), operands[i]);
			}
		}
	}

	void dispatchResumeThrow(WASM::TypeIdx type, WASM::TagIdx tag,
			std::vector<WASM::ContHandlerClause>&& handlers) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType payload = tagPayloadType(tag);
		if (values.size() < payload.params.size() + 1)
			throw std::runtime_error("AsmJit resume_throw operand stack underflow");
		auto continuation = values.back(); values.pop_back();
		std::vector<asmjit::x86::Gp> payloadValues(payload.params.size());
		for (std::size_t i = payload.params.size(); i-- > 0;) {
			payloadValues[i] = values.back();
			values.pop_back();
		}
		const ExceptionPayloadLayout layout = exceptionPayloadLayout(payload);
		asmjit::FuncSignature createSignature(asmjit::CallConvId::kCDecl);
		createSignature.set_ret(asmjit::TypeId::kUIntPtr);
		createSignature.add_arg(asmjit::TypeId::kUIntPtr);
		createSignature.add_arg(asmjit::TypeId::kInt32);
		createSignature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* create = nullptr;
		checkError(compiler.invoke(asmjit::Out(create),
				reinterpret_cast<uint64_t>(&::wasm_exn_create), createSignature),
				"AsmJit resume_throw exception allocation failed");
		auto exn = compiler.new_gp_ptr("resumeThrownException");
		create->set_arg(0, vmContext);
		create->set_arg(1, asmjit::Imm(static_cast<int32_t>(internals.tagIds[tag])));
		create->set_arg(2, asmjit::Imm(static_cast<uint32_t>(layout.size)));
		create->set_ret(0, exn);
		emitPendingTrapReturn();
		for (std::size_t i = 0; i < payloadValues.size(); ++i) {
			const int32_t offset = static_cast<int32_t>(layout.fields[i].offset);
			if (kindWidth(payload.params[i].val.opcode) == 16) {
				for (int word = 0; word < 2; ++word) {
					auto bits = compiler.new_gp64("resumeThrowPayloadWord");
					emitter.mov(bits,
							asmjit::x86::qword_ptr(payloadValues[i], word * 8));
					emitter.mov(
							asmjit::x86::qword_ptr(exn, offset + word * 8), bits);
				}
			} else {
				emitter.mov(asmjit::x86::qword_ptr(exn, offset),
						payloadValues[i]);
			}
		}
		emitter.mov(exceptionPtrSlot, exn);
		values.push_back(continuation);
		resumeThrowActive_ = true;
		dispatchResume(type, std::move(handlers));
		resumeThrowActive_ = false;
	}

	void dispatchResumeThrowRef(WASM::TypeIdx type,
			std::vector<WASM::ContHandlerClause>&& handlers) override
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit resume_throw_ref operand stack underflow");
		auto continuation = values.back(); values.pop_back();
		auto exn = values.back(); values.pop_back();
		emitter.mov(exceptionPtrSlot, exn);
		values.push_back(continuation);
		resumeThrowActive_ = true;
		dispatchResume(type, std::move(handlers));
		resumeThrowActive_ = false;
	}

	void dispatchResume(WASM::TypeIdx type,
			std::vector<WASM::ContHandlerClause>&& handlers) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType function = continuationSignature(type);
		if (values.size() < function.params.size() + 1)
			throw std::runtime_error("AsmJit resume operand stack underflow");
		auto continuation = values.back(); values.pop_back();
		std::vector<asmjit::x86::Gp> operands(function.params.size());
		for (std::size_t i = function.params.size(); i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		asmjit::x86::Mem frame = compiler.new_stack(sizeof(WASM::ContinuationOpFrame), 16,
				"continuationFrame");
		auto at = [&](int32_t offset) {
			auto memory = frame;
			memory.set_offset(frame.offset() + offset);
			return memory;
		};
		emitContinuationFrameArguments(frame, operands, function);
		if (resumeThrowActive_) {
			auto exception = compiler.new_gp_ptr("resumeException");
			emitter.mov(exception, exceptionPtrSlot);
			auto exceptionSlot = frame;
			exceptionSlot.set_offset(frame.offset() + static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, exception)));
			exceptionSlot.set_size(8);
			emitter.mov(exceptionSlot, exception);
		} else {
			auto exceptionSlot = frame;
			exceptionSlot.set_offset(frame.offset() + static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, exception)));
			exceptionSlot.set_size(8);
			emitter.mov(exceptionSlot, asmjit::Imm(0));
		}
		auto frameAddress = compiler.new_gp_ptr("continuationFrameAddress");
		emitter.lea(frameAddress, frame);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_cont_resume_ops), signature),
				"AsmJit resume helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, continuation);
		invoke->set_arg(2, asmjit::Imm(static_cast<uint32_t>(type)));
		invoke->set_arg(3, frameAddress);
		// The resume consumes the original handle.
		emitRefDrop(continuation);
		emitPendingTrapReturn();
		auto status = compiler.new_gp32("continuationStatus");
		emitter.mov(status, at(static_cast<int32_t>(
				offsetof(WASM::ContinuationOpFrame, result)
				+ offsetof(WASM::ContinuationResumeResult, status))));
		asmjit::Label completed = compiler.new_label();
		asmjit::Label suspended = compiler.new_label();
		asmjit::Label done = compiler.new_label();
		emitter.j(completed, asmjit::ujit::cmp_eq(status,
				static_cast<int32_t>(WASM::ContinuationStatus::Completed)));
		emitter.j(suspended, asmjit::ujit::cmp_eq(status,
				static_cast<int32_t>(WASM::ContinuationStatus::Suspended)));
		asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
		reportSignature.set_ret(asmjit::TypeId::kVoid);
		reportSignature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* report = nullptr;
		checkError(compiler.invoke(asmjit::Out(report),
				reinterpret_cast<uint64_t>(&asmjitReportInternal), reportSignature),
				"AsmJit continuation resume failure trap failed");
		report->set_arg(0, asmjit::Imm(reinterpret_cast<uint64_t>(
				"continuation resume failed")));
		emitPendingTrapReturn();
		checkError(compiler.bind(completed), "AsmJit continuation completed label failed");
		for (std::size_t i = 0; i < function.results.size(); ++i) {
			auto result = newIntegerRegister(compiler, function.results[i].val,
					"continuationResult");
			const int32_t offset = static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, results)
					+ i * WASM::kContinuationValueStride);
			if (kindWidth(function.results[i].val.opcode) == 16) {
				for (int word = 0; word < 2; ++word) {
					auto bits = compiler.new_gp64("continuationResultWord");
					emitter.mov(bits, at(offset + word * 8));
					emitter.mov(asmjit::x86::qword_ptr(result, word * 8), bits);
				}
			} else {
				emitter.mov(result, at(offset));
			}
			values.push_back(result);
		}
		emitter.jmp(done);
		checkError(compiler.bind(suspended), "AsmJit continuation suspended label failed");
		auto dropSuspended = [&]() {
			auto next = compiler.new_gp_ptr("suspendedContinuation");
			emitter.mov(next, at(static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, result)
					+ offsetof(WASM::ContinuationResumeResult, continuation))));
			emitRefDrop(next);
		};
		if (handlers.empty()) {
			dropSuspended();
			asmjit::InvokeNode* unhandled = nullptr;
			checkError(compiler.invoke(asmjit::Out(unhandled),
					reinterpret_cast<uint64_t>(&asmjitReportInternal), reportSignature),
					"AsmJit unhandled suspension trap failed");
			unhandled->set_arg(0, asmjit::Imm(reinterpret_cast<uint64_t>(
					"unhandled continuation suspension")));
			emitPendingTrapReturn();
		} else {
			auto tagId = compiler.new_gp32("continuationTagId");
			emitter.mov(tagId, at(static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, result)
					+ offsetof(WASM::ContinuationResumeResult, tagId))));
			std::vector<asmjit::Label> handlerLabels(handlers.size());
			for (std::size_t i = 0; i < handlers.size(); ++i) {
				handlerLabels[i] = compiler.new_label();
				if (handlers[i].isSwitch)
					continue;
				emitter.j(handlerLabels[i], asmjit::ujit::cmp_eq(tagId, static_cast<int32_t>(
						internals.tagIds[handlers[i].tag])));
			}
			dropSuspended();
			asmjit::InvokeNode* unhandled = nullptr;
			checkError(compiler.invoke(asmjit::Out(unhandled),
					reinterpret_cast<uint64_t>(&asmjitReportInternal), reportSignature),
					"AsmJit unhandled suspension trap failed");
			unhandled->set_arg(0, asmjit::Imm(reinterpret_cast<uint64_t>(
					"unhandled continuation suspension")));
			emitPendingTrapReturn();
			for (std::size_t i = 0; i < handlers.size(); ++i) {
				if (handlers[i].isSwitch)
					continue;
				checkError(compiler.bind(handlerLabels[i]),
						"AsmJit handler label binding failed");
				if (handlers[i].label >= controls.size())
					throw std::runtime_error("resume: handler label is out of range");
				ControlFrame& target = controls[controls.size() - 1 - handlers[i].label];
				const bool loopTarget = target.kind == ControlKind::Loop;
				const WASM::FuncType tagType = tagPayloadType(handlers[i].tag);
				const std::size_t saved = values.size();
				for (std::size_t j = 0; j < tagType.params.size(); ++j) {
					auto value = newIntegerRegister(compiler, tagType.params[j].val,
							"handlerPayload");
					const int32_t offset = static_cast<int32_t>(
							offsetof(WASM::ContinuationOpFrame, tags)
							+ j * WASM::kContinuationValueStride);
					emitter.mov(value, at(offset));
					values.push_back(value);
				}
				auto next = compiler.new_gp_ptr("handlerContinuation");
				emitter.mov(next, at(static_cast<int32_t>(
						offsetof(WASM::ContinuationOpFrame, result)
						+ offsetof(WASM::ContinuationResumeResult, continuation))));
				values.push_back(next);
				const std::size_t branchValues = loopTarget ? target.paramCount
						: target.resultCount;
				if (values.size() < branchValues)
					throw std::runtime_error("AsmJit handler stack underflow");
				const std::size_t first = values.size() - branchValues;
				const std::vector<asmjit::x86::Gp>& destinations = loopTarget
						? target.params : target.results;
				const std::vector<WASM::ValueTypeCode>& codes = loopTarget
						? target.paramTypes : target.resultTypes;
				for (std::size_t j = 0; j < branchValues; ++j)
					transferValue(destinations[j], values[first + j], codes[j],
							"AsmJit handler value transfer failed");
				emitter.jmp(loopTarget ? target.begin : target.end);
				if (!loopTarget)
					target.endTargeted = true;
				values.resize(saved);
				armTerminated = false;
			}
		}
		checkError(compiler.bind(done), "AsmJit continuation done label failed");
	}

	void dispatchContBind(WASM::TypeIdx from, WASM::TypeIdx to) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType fromType = continuationSignature(from);
		const WASM::FuncType toType = continuationSignature(to);
		if (fromType.params.size() < toType.params.size())
			throw std::runtime_error("AsmJit cont.bind target continuation has more parameters");
		const std::size_t bound = fromType.params.size() - toType.params.size();
		if (values.size() < bound + 1)
			throw std::runtime_error("AsmJit cont.bind operand stack underflow");
		auto continuation = values.back(); values.pop_back();
		std::vector<asmjit::x86::Gp> operands(bound);
		for (std::size_t i = bound; i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		asmjit::x86::Mem frame = compiler.new_stack(sizeof(WASM::ContinuationOpFrame), 16,
				"continuationFrame");
		WASM::FuncType boundFunction = fromType;
		boundFunction.results.clear();
		boundFunction.params.resize(bound);
		emitContinuationFrameArguments(frame, operands, boundFunction);
		auto frameAddress = compiler.new_gp_ptr("continuationFrameAddress");
		emitter.lea(frameAddress, frame);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_cont_bind_ops), signature),
				"AsmJit cont.bind helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, continuation);
		invoke->set_arg(2, asmjit::Imm(static_cast<uint64_t>(from)
				| (static_cast<uint64_t>(to) << 32)));
		invoke->set_arg(3, frameAddress);
		auto result = compiler.new_gp_ptr("boundContinuation");
		invoke->set_ret(0, result);
		values.push_back(result);
		// `cont.bind` consumes its continuation operand, exactly as `resume`
		// and `switch` do: the helper returns a new handle that shares the
		// worker, so the source's count is only safe to give back once the
		// result holds it.
		emitRefDrop(continuation);
		emitPendingTrapReturn();
	}

	void dispatchSuspend(WASM::TagIdx tag) override
	{
		if (inDeadCode())
			return;
		const WASM::FuncType payload = tagPayloadType(tag);
		if (values.size() < payload.params.size())
			throw std::runtime_error("AsmJit suspend operand stack underflow");
		std::vector<asmjit::x86::Gp> operands(payload.params.size());
		for (std::size_t i = payload.params.size(); i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		asmjit::x86::Mem frame = compiler.new_stack(sizeof(WASM::ContinuationOpFrame), 16,
				"continuationFrame");
		auto at = [&](int32_t offset) {
			auto memory = frame;
			memory.set_offset(frame.offset() + offset);
			return memory;
		};
		emitContinuationFrameArguments(frame, operands, payload);
		auto frameAddress = compiler.new_gp_ptr("continuationFrameAddress");
		emitter.lea(frameAddress, frame);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_cont_suspend_ops), signature),
				"AsmJit suspend helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, asmjit::Imm(static_cast<uint64_t>(tag)
				| (static_cast<uint64_t>(internals.tagIds[tag]) << 32)));
		invoke->set_arg(2, frameAddress);
		emitPendingTrapReturn();
		emitPendingExceptionCheck();
		for (std::size_t i = 0; i < payload.results.size(); ++i) {
			auto result = newIntegerRegister(compiler, payload.results[i].val,
					"continuationSuspendResult");
			const int32_t offset = static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, results)
					+ i * WASM::kContinuationValueStride);
			if (kindWidth(payload.results[i].val.opcode) == 16) {
				for (int word = 0; word < 2; ++word) {
					auto bits = compiler.new_gp64("continuationSuspendWord");
					emitter.mov(bits, at(offset + word * 8));
					emitter.mov(asmjit::x86::qword_ptr(result, word * 8), bits);
				}
			} else {
				emitter.mov(result, at(offset));
			}
			values.push_back(result);
		}
	}

	void dispatchSwitch(WASM::TypeIdx type, WASM::TagIdx tag) override
	{
		if (inDeadCode())
			return;
		(void)tag;
		const WASM::FuncType target = continuationSignature(type);
		if (target.params.empty())
			throw std::runtime_error("AsmJit switch target continuation has no peer parameter");
		const WASM::StorageType& peer = target.params.back();
		if ((peer.val.opcode != WASM::ValueTypeCode::Ref
				&& peer.val.opcode != WASM::ValueTypeCode::RefNull)
				|| peer.val.heapType < 0)
			throw std::runtime_error("AsmJit switch target's last parameter is not a continuation");
		const WASM::TypeIdx currentType = static_cast<WASM::TypeIdx>(peer.val.heapType);
		const WASM::FuncType current = continuationSignature(currentType);
		const std::size_t argCount = target.params.size() - 1;
		if (values.size() < argCount + 1)
			throw std::runtime_error("AsmJit switch operand stack underflow");
		auto continuation = values.back(); values.pop_back();
		std::vector<asmjit::x86::Gp> operands(argCount);
		for (std::size_t i = argCount; i-- > 0;) {
			operands[i] = values.back();
			values.pop_back();
		}
		asmjit::x86::Mem frame = compiler.new_stack(sizeof(WASM::ContinuationOpFrame), 16,
				"continuationFrame");
		auto at = [&](int32_t offset) {
			auto memory = frame;
			memory.set_offset(frame.offset() + offset);
			return memory;
		};
		WASM::FuncType argumentFunction = target;
		argumentFunction.params.resize(argCount);
		emitContinuationFrameArguments(frame, operands, argumentFunction);
		auto frameAddress = compiler.new_gp_ptr("continuationFrameAddress");
		emitter.lea(frameAddress, frame);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::wasm_cont_switch_ops), signature),
				"AsmJit switch helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, continuation);
		invoke->set_arg(2, asmjit::Imm(static_cast<uint32_t>(argCount)));
		invoke->set_arg(3, frameAddress);
		emitRefDrop(continuation);
		emitPendingTrapReturn();
		auto status = compiler.new_gp32("switchStatus");
		emitter.mov(status, at(static_cast<int32_t>(
				offsetof(WASM::ContinuationOpFrame, result)
				+ offsetof(WASM::ContinuationResumeResult, status))));
		asmjit::Label completed = compiler.new_label();
		emitter.j(completed, asmjit::ujit::cmp_eq(status,
				static_cast<int32_t>(WASM::ContinuationStatus::Completed)));
		asmjit::FuncSignature reportSignature(asmjit::CallConvId::kCDecl);
		reportSignature.set_ret(asmjit::TypeId::kVoid);
		reportSignature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* report = nullptr;
		checkError(compiler.invoke(asmjit::Out(report),
				reinterpret_cast<uint64_t>(&asmjitReportInternal), reportSignature),
				"AsmJit switch failure trap failed");
		report->set_arg(0, asmjit::Imm(reinterpret_cast<uint64_t>(
				"continuation switch failed")));
		emitPendingTrapReturn();
		checkError(compiler.bind(completed), "AsmJit switch completed label failed");
		for (std::size_t i = 0; i < current.params.size(); ++i) {
			auto result = newIntegerRegister(compiler, current.params[i].val,
					"switchResult");
			const int32_t offset = static_cast<int32_t>(
					offsetof(WASM::ContinuationOpFrame, results)
					+ i * WASM::kContinuationValueStride);
			emitter.mov(result, at(offset));
			values.push_back(result);
		}
	}

	void emitStringNewArray(WASM::TypeIdx type, int mode)
	{
		if (inDeadCode())
			return;
		const WASM::CoreTypeView view = module.typeView(type);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit string array constructor requires an array type");
		const WASM::CanonStorageType& storage = view.arrayElement().storage;
		const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
		if (mode == 3 ? elementBytes != 2 : elementBytes != 1)
			throw std::runtime_error("AsmJit string array element size mismatch");
		const uint64_t layout = static_cast<uint64_t>(WASM::gcArrayDataOffset(storage))
				| (static_cast<uint64_t>(elementBytes) << 32)
				| (static_cast<uint64_t>(mode) << 40);
		if (values.size() < 3)
			throw std::runtime_error("AsmJit string array constructor operand stack underflow");
		auto end = values.back(); values.pop_back();
		auto start = values.back(); values.pop_back();
		auto array = values.back(); values.pop_back();
		auto range = compiler.new_gp64("stringArrayRange");
		auto end64 = compiler.new_gp64("stringArrayEnd");
		emitter.mov(range.r32(), start.r32());
		emitter.mov(end64.r32(), end.r32());
		emitter.shl(end64, 32);
		emitter.or_(range, end64);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_new_array_packed), signature),
				"AsmJit string array constructor helper call failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, array);
		invoke->set_arg(2, range);
		invoke->set_arg(3, asmjit::Imm(layout));
		auto result = compiler.new_gp_ptr("stringNewArrayResult");
		invoke->set_ret(0, result);
		pushStringValue(result);
		emitPendingTrapReturn();
	}

	void emitStringEncodeArray(WASM::TypeIdx type, int mode)
	{
		if (inDeadCode())
			return;
		const WASM::CoreTypeView view = module.typeView(type);
		if (view.kind() != WASM::CanonTypeKind::Array)
			throw std::runtime_error("AsmJit string array encoder requires an array type");
		const WASM::CanonStorageType& storage = view.arrayElement().storage;
		const uint32_t elementBytes = static_cast<uint32_t>(WASM::gcStorageBytes(storage));
		if (mode == 3 ? elementBytes != 2 : elementBytes != 1)
			throw std::runtime_error("AsmJit string array element size mismatch");
		const uint64_t layout = static_cast<uint64_t>(WASM::gcArrayDataOffset(storage))
				| (static_cast<uint64_t>(elementBytes) << 32)
				| (static_cast<uint64_t>(mode) << 40);
		if (values.size() < 3)
			throw std::runtime_error("AsmJit string array encoder operand stack underflow");
		auto start = values.back(); values.pop_back();
		auto array = values.back(); values.pop_back();
		auto string = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_string_encode_array_packed), signature),
				"AsmJit string array encoder helper call failed");
		invoke->set_arg(0, string);
		invoke->set_arg(1, array);
		invoke->set_arg(2, start);
		invoke->set_arg(3, asmjit::Imm(layout));
		auto result = compiler.new_gp32("stringEncodeArrayResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void dispatchI64Add() override { emitIntegerBinary(BinaryOp::Add); }
	void dispatchI64Sub() override { emitIntegerBinary(BinaryOp::Sub); }
	void dispatchI64Mul() override { emitIntegerBinary(BinaryOp::Mul); }

	void dispatchI64Add128() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 4)
			throw std::runtime_error("AsmJit i64.add128 operand stack underflow");
		auto bHi = values.back(); values.pop_back();
		auto bLo = values.back(); values.pop_back();
		auto aHi = values.back(); values.pop_back();
		auto aLo = values.back(); values.pop_back();
		auto lo = compiler.new_gp64("add128Low");
		emitter.mov(lo, aLo);
		emitter.add(lo, bLo);
		auto hi = compiler.new_gp64("add128High");
		emitter.mov(hi, aHi);
		emitter.add(hi, bHi);
		// The universal layer has no carry chain, so the carry out of the low
		// half is detected as that add's unsigned wraparound and applied to the
		// high half explicitly — the same value `adc` would have added.
		auto incremented = compiler.new_gp64("add128Carry");
		emitter.mov(incremented, hi);
		emitter.add(incremented, asmjit::Imm(1));
		emitter.select(hi, incremented, hi, asmjit::ujit::ucmp_lt(lo, aLo));
		values.push_back(lo);
		values.push_back(hi);
	}

	void dispatchI64Sub128() override
	{
		if (inDeadCode())
			return;
		if (values.size() < 4)
			throw std::runtime_error("AsmJit i64.sub128 operand stack underflow");
		auto bHi = values.back(); values.pop_back();
		auto bLo = values.back(); values.pop_back();
		auto aHi = values.back(); values.pop_back();
		auto aLo = values.back(); values.pop_back();
		auto lo = compiler.new_gp64("sub128Low");
		emitter.mov(lo, aLo);
		emitter.sub(lo, bLo);
		auto hi = compiler.new_gp64("sub128High");
		emitter.mov(hi, aHi);
		emitter.sub(hi, bHi);
		// The borrow out of the low half, detected as its unsigned wraparound and
		// subtracted from the high half — the same value `sbb` would have taken.
		auto decremented = compiler.new_gp64("sub128Borrow");
		emitter.mov(decremented, hi);
		emitter.sub(decremented, asmjit::Imm(1));
		emitter.select(hi, decremented, hi, asmjit::ujit::ucmp_lt(aLo, bLo));
		values.push_back(lo);
		values.push_back(hi);
	}

	void dispatchI64MulWideS() override { emitI64MulWide(true); }
	void dispatchI64MulWideU() override { emitI64MulWide(false); }

	void emitI64MulWide(bool isSigned)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i64.mul_wide operand stack underflow");
		auto b = values.back(); values.pop_back();
		auto a = values.back(); values.pop_back();
		auto storage = compiler.new_stack(16, 8, "mulWideResult");
		auto out = compiler.new_gp_ptr("mulWideResultPointer");
		emitter.lea(out, storage);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		const uint64_t helper = isSigned
				? reinterpret_cast<uint64_t>(&asmjitI64MulWideS)
				: reinterpret_cast<uint64_t>(&asmjitI64MulWideU);
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit i64.mul_wide helper call failed");
		invoke->set_arg(0, out);
		invoke->set_arg(1, a);
		invoke->set_arg(2, b);

		auto lowSlot = storage;
		lowSlot.set_size(8);
		auto highSlot = storage;
		highSlot.set_size(8);
		highSlot.set_offset(storage.offset() + 8);
		auto lo = compiler.new_gp64("mulWideLow");
		emitter.mov(lo, lowSlot);
		auto hi = compiler.new_gp64("mulWideHigh");
		emitter.mov(hi, highSlot);
		values.push_back(lo);
		values.push_back(hi);
	}

	void dispatchI64And() override { emitIntegerBinary(BinaryOp::And); }
	void dispatchI64Or() override { emitIntegerBinary(BinaryOp::Or); }
	void dispatchI64Xor() override { emitIntegerBinary(BinaryOp::Xor); }
	void dispatchI64Shl() override { emitIntegerShift(ShiftOp::Left); }
	void dispatchI64ShrS() override { emitIntegerShift(ShiftOp::ArithmeticRight); }
	void dispatchI64ShrU() override { emitIntegerShift(ShiftOp::LogicalRight); }
	void dispatchI64Rotl() override { emitIntegerShift(ShiftOp::RotateLeft); }
	void dispatchI64Rotr() override { emitIntegerShift(ShiftOp::RotateRight); }
	void dispatchI64Clz() override
	{
		if (inDeadCode()) return;
		emitBitCount64(BitCountOp::LeadingZeros);
	}
	void dispatchI64Ctz() override
	{
		if (inDeadCode()) return;
		emitBitCount64(BitCountOp::TrailingZeros);
	}
	void dispatchI64Popcnt() override
	{
		if (inDeadCode()) return;
		emitUnaryCall64(&asmjitI64Popcnt, "i64.popcnt");
	}
	void dispatchI64DivS() override { emitCheckedI64Call(&asmjitI64DivS, "i64.div_s"); }
	void dispatchI64DivU() override { emitCheckedI64Call(&asmjitI64DivU, "i64.div_u"); }
	void dispatchI64RemS() override { emitCheckedI64Call(&asmjitI64RemS, "i64.rem_s"); }
	void dispatchI64RemU() override { emitCheckedI64Call(&asmjitI64RemU, "i64.rem_u"); }

	void dispatchI32Eqz() override
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit i32.eqz operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("eqz");
		emitter.selectFlag(result, value, asmjit::ujit::test_z(value, value));
		values.push_back(result);
	}

	void dispatchI32Eq() override { emitCompare(asmjit::x86::CondCode::kE); }
	void dispatchI32Ne() override { emitCompare(asmjit::x86::CondCode::kNE); }
	void dispatchI32LtS() override { emitCompare(asmjit::x86::CondCode::kL); }
	void dispatchI32LtU() override { emitCompare(asmjit::x86::CondCode::kB); }
	void dispatchI32GtS() override { emitCompare(asmjit::x86::CondCode::kG); }
	void dispatchI32GtU() override { emitCompare(asmjit::x86::CondCode::kA); }
	void dispatchI32LeS() override { emitCompare(asmjit::x86::CondCode::kLE); }
	void dispatchI32LeU() override { emitCompare(asmjit::x86::CondCode::kBE); }
	void dispatchI32GeS() override { emitCompare(asmjit::x86::CondCode::kGE); }
	void dispatchI32GeU() override { emitCompare(asmjit::x86::CondCode::kAE); }

	void dispatchI32And() override { emitBinary(BinaryOp::And); }
	void dispatchI32Or() override { emitBinary(BinaryOp::Or); }
	void dispatchI32Xor() override { emitBinary(BinaryOp::Xor); }
	void dispatchI32Shl() override { emitShift(ShiftOp::Left); }
	void dispatchI32ShrS() override { emitShift(ShiftOp::ArithmeticRight); }
	void dispatchI32ShrU() override { emitShift(ShiftOp::LogicalRight); }
	void dispatchI32Rotl() override { emitShift(ShiftOp::RotateLeft); }
	void dispatchI32Rotr() override { emitShift(ShiftOp::RotateRight); }

	void dispatchI32Add() override { emitBinary(BinaryOp::Add); }
	void dispatchI32Sub() override { emitBinary(BinaryOp::Sub); }
	void dispatchI32Mul() override { emitBinary(BinaryOp::Mul); }

	void dispatchF32Const(float value) override
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		auto reg = compiler.new_gp32("f32Constant");
		emitter.mov(reg, bits);
		values.push_back(reg);
	}

	void dispatchF64Const(double value) override
	{
		uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		auto reg = compiler.new_gp64("f64Constant");
		emitter.mov(reg, bits);
		values.push_back(reg);
	}

	void dispatchF32Abs() override { emitFloatAbs(); }
	void dispatchF32Neg() override { emitFloatNeg(4); }
	void dispatchF32Sqrt() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF32Sqrt), 4); }
	void dispatchF32Ceil() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF32Ceil), 4); }
	void dispatchF32Floor() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF32Floor), 4); }
	void dispatchF32Trunc() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF32Trunc), 4); }
	void dispatchF32Nearest() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF32Nearest), 4); }
	void dispatchF32Add() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kAddF32S, asmjit::ujit::UniOpVVV::kAddF64S, 4); }
	void dispatchF32Sub() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kSubF32S, asmjit::ujit::UniOpVVV::kSubF64S, 4); }
	void dispatchF32Mul() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kMulF32S, asmjit::ujit::UniOpVVV::kMulF64S, 4); }
	void dispatchF32Div() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kDivF32S, asmjit::ujit::UniOpVVV::kDivF64S, 4); }
	void dispatchF32Min() override
	{ emitFloatHelper2(reinterpret_cast<uint64_t>(&asmjitF32Min), 4); }
	void dispatchF32Max() override
	{ emitFloatHelper2(reinterpret_cast<uint64_t>(&asmjitF32Max), 4); }
	void dispatchF32Copysign() override { emitFloatCopysign(4); }

	void dispatchF64Abs() override { emitFloatAbs(); }
	void dispatchF64Neg() override { emitFloatNeg(8); }
	void dispatchF64Sqrt() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF64Sqrt), 8); }
	void dispatchF64Ceil() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF64Ceil), 8); }
	void dispatchF64Floor() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF64Floor), 8); }
	void dispatchF64Trunc() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF64Trunc), 8); }
	void dispatchF64Nearest() override
	{ emitFloatHelper1(reinterpret_cast<uint64_t>(&asmjitF64Nearest), 8); }
	void dispatchF64Add() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kAddF32S, asmjit::ujit::UniOpVVV::kAddF64S, 8); }
	void dispatchF64Sub() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kSubF32S, asmjit::ujit::UniOpVVV::kSubF64S, 8); }
	void dispatchF64Mul() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kMulF32S, asmjit::ujit::UniOpVVV::kMulF64S, 8); }
	void dispatchF64Div() override
	{ emitFloatBinary(asmjit::ujit::UniOpVVV::kDivF32S, asmjit::ujit::UniOpVVV::kDivF64S, 8); }
	void dispatchF64Min() override
	{ emitFloatHelper2(reinterpret_cast<uint64_t>(&asmjitF64Min), 8); }
	void dispatchF64Max() override
	{ emitFloatHelper2(reinterpret_cast<uint64_t>(&asmjitF64Max), 8); }
	void dispatchF64Copysign() override { emitFloatCopysign(8); }

	void dispatchF32Eq() override { emitFloatCompare(FloatCompareOp::Eq, 4); }
	void dispatchF32Ne() override { emitFloatCompare(FloatCompareOp::Ne, 4); }
	void dispatchF32Lt() override { emitFloatCompare(FloatCompareOp::Lt, 4); }
	void dispatchF32Gt() override { emitFloatCompare(FloatCompareOp::Gt, 4); }
	void dispatchF32Le() override { emitFloatCompare(FloatCompareOp::Le, 4); }
	void dispatchF32Ge() override { emitFloatCompare(FloatCompareOp::Ge, 4); }
	void dispatchF64Eq() override { emitFloatCompare(FloatCompareOp::Eq, 8); }
	void dispatchF64Ne() override { emitFloatCompare(FloatCompareOp::Ne, 8); }
	void dispatchF64Lt() override { emitFloatCompare(FloatCompareOp::Lt, 8); }
	void dispatchF64Gt() override { emitFloatCompare(FloatCompareOp::Gt, 8); }
	void dispatchF64Le() override { emitFloatCompare(FloatCompareOp::Le, 8); }
	void dispatchF64Ge() override { emitFloatCompare(FloatCompareOp::Ge, 8); }

	void dispatchF32ConvertI32S() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF32FromI32S), 4, 4); }
	void dispatchF32ConvertI32U() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF32FromI32U), 4, 4); }
	void dispatchF32ConvertI64S() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF32FromI64S), 8, 4); }
	void dispatchF32ConvertI64U() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF32FromI64U), 8, 4); }
	void dispatchF64ConvertI32S() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF64FromI32S), 4, 8); }
	void dispatchF64ConvertI32U() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF64FromI32U), 4, 8); }
	void dispatchF64ConvertI64S() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF64FromI64S), 8, 8); }
	void dispatchF64ConvertI64U() override
	{ emitIntToFloat(reinterpret_cast<uint64_t>(&asmjitF64FromI64U), 8, 8); }
	void dispatchF32DemoteF64() override
	{ emitFloatToFloat(reinterpret_cast<uint64_t>(&asmjitF32DemoteF64), 8, 4); }
	void dispatchF64PromoteF32() override
	{ emitFloatToFloat(reinterpret_cast<uint64_t>(&asmjitF64PromoteF32), 4, 8); }

	void dispatchI32TruncF32S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncF32S), 4, 4); }
	void dispatchI32TruncF32U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncF32U), 4, 4); }
	void dispatchI32TruncF64S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncF64S), 8, 4); }
	void dispatchI32TruncF64U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncF64U), 8, 4); }
	void dispatchI64TruncF32S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncF32S), 4, 8); }
	void dispatchI64TruncF32U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncF32U), 4, 8); }
	void dispatchI64TruncF64S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncF64S), 8, 8); }
	void dispatchI64TruncF64U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncF64U), 8, 8); }

	void dispatchI32TruncSatF32S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncSatF32S), 4, 4); }
	void dispatchI32TruncSatF32U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncSatF32U), 4, 4); }
	void dispatchI32TruncSatF64S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncSatF64S), 8, 4); }
	void dispatchI32TruncSatF64U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI32TruncSatF64U), 8, 4); }
	void dispatchI64TruncSatF32S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncSatF32S), 4, 8); }
	void dispatchI64TruncSatF32U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncSatF32U), 4, 8); }
	void dispatchI64TruncSatF64S() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncSatF64S), 8, 8); }
	void dispatchI64TruncSatF64U() override
	{ emitFloatToInt(reinterpret_cast<uint64_t>(&asmjitI64TruncSatF64U), 8, 8); }

	// A reinterpretation keeps the same width and the same bit pattern; the Gp
	// already carries it. A float-to-integer form is an observability point, so
	// the deterministic profile canonicalises the top value first
	// (docs/proposals/PROFILES.md PR-3).
	void dispatchI32ReinterpretF32() override { canonicalizeTopFloat(4); }
	void dispatchI64ReinterpretF64() override { canonicalizeTopFloat(8); }
	void dispatchF32ReinterpretI32() override {}
	void dispatchF64ReinterpretI64() override {}

	void dispatchF32Load(WASM::MemArg arg) override
	{ emitMemoryLoad(arg, 4, LoadKind::F32); }
	void dispatchF32LoadF16(WASM::MemArg arg) override { emitF16Load(arg); }
	void dispatchF32StoreF16(WASM::MemArg arg) override { emitF16Store(arg); }
	void dispatchF64Load(WASM::MemArg arg) override
	{ emitMemoryLoad(arg, 8, LoadKind::F64); }
	void dispatchF32Store(WASM::MemArg arg) override { emitMemoryStore(arg, 4, true); }
	void dispatchF64Store(WASM::MemArg arg) override { emitMemoryStore(arg, 8, true); }


	//! Releases one count for every reference local of the frame.
	//!
	//! This is the one release primitive: the ordinary epilogue
	//! (`emitFunctionReturnValues`, and the implicit end), and every exit path
	//! that does not reach it -- a reported trap, an exception propagating out
	//! of the function, a tail call that leaves through the pending tail-call
	//! state -- all come here, so no exit releases a local twice.
	//!
	//! The re-entrancy guard matters because `wasm_ref_drop` is itself a call
	//! whose pending-trap return would otherwise re-enter this loop and release
	//! locals this path has already dropped. With it, a trap raised *inside* the
	//! release stops the release -- a leak in a case that is already a trap,
	//! never a count dropped twice.
	void emitDropReferenceLocals()
	{
		if (releasingTrapLocals)
			return;
		releasingTrapLocals = true;
		// Every reference local owns one count (parameters take theirs in the
		// prologue), so the epilogue gives each one back. `wasm_ref_drop` is the
		// universal release: it dispatches to the continuation registry, the
		// string store, the GC heap or the host bridge as the value requires.
		for (std::size_t i = 0; i < localKinds.size(); ++i) {
			if (!isReferenceKind(localKinds[i]) || localWidths[i] != 8)
				continue;
			emitRefDrop(locals[i]);
		}
		releasingTrapLocals = false;
	}

	//! Releases every reference local on an exit path that does not run the
	//! ordinary epilogue -- a reported trap, or an exception propagating out of
	//! the function.
	//!
	//! Without it such a path returns while the frame's reference locals still
	//! hold their counts, which a trapping loop turns into an unbounded leak:
	//! the trap is reported, the frame returns, and nothing ever releases the
	//! locals it owned.
	void emitTrapLocalRelease()
	{
		emitDropReferenceLocals();
	}

	//! Emits the return for the current value stack in either ABI. Shared by
	//! `return`, a `br` to the function label and a conditional function branch.
	void emitFunctionReturnValues()
	{
		emitDropReferenceLocals();
		if (resultCount == 0) {
			checkError(compiler.ret(), "AsmJit return emission failed");
			return;
		}
		if (values.empty())
			throw std::runtime_error("AsmJit return operand stack underflow");
		if (frameMode) {
			if (resultCount == 1) {
				emitFrameReturnValue(values.back());
				return;
			}
			const std::size_t first = values.size() - resultCount;
			uint32_t offset = frameResultOffset;
			for (std::size_t i = 0; i < resultCount; ++i) {
				const WASM::ValueTypeCode kind = resultTypes[i];
				const asmjit::x86::Gp value = values[first + i];
				if (kind == WASM::ValueTypeCode::V128) {
					for (int word = 0; word < 2; ++word) {
						auto bits = compiler.new_gp64("frameResultWord");
						emitter.mov(bits,
									asmjit::x86::qword_ptr(value, word * 8));
						emitter.mov(
									asmjit::x86::qword_ptr(framePointer, offset + word * 8), bits);
					}
					offset += 16;
				} else {
					const asmjit::x86::Gp stored = isFloatKind(kind)
							? canonicalizeFloatBits(value, kindWidth(kind)) : value;
					const int width = stored.size();
					emitter.mov(
								asmjit::x86::Mem(framePointer, offset, width), stored);
					offset += 8;
				}
			}
			checkError(compiler.ret(), "AsmJit frame multi-result return failed");
			return;
		}
		if (isFloatKind(resultType)) {
			const int width = kindWidth(resultType);
			checkError(compiler.ret(materializeFloat(
					canonicalizeFloatBits(values.back(), width), width)),
					"AsmJit float return emission failed");
		} else {
			checkError(compiler.ret(values.back()), "AsmJit return emission failed");
		}
	}

	void dispatchReturn() override
	{
		if (inDeadCode())
			return;
		armTerminated = true;
		emitFunctionReturnValues();
	}

private:
	enum class BitCountOp { LeadingZeros, TrailingZeros };
	enum class LoadKind {
		I32, I8Signed, I8Unsigned, I16Signed, I16Unsigned,
		I64, I64I8Signed, I64I8Unsigned, I64I16Signed, I64I16Unsigned,
		I64I32Signed, I64I32Unsigned,
		F32, F64
	};
	enum class FloatCompareOp { Eq, Ne, Lt, Le, Gt, Ge };

	enum class ControlKind { Block, Loop, If };
	struct ControlFrame {
		ControlKind kind = ControlKind::Block;
		size_t stackHeight = 0;
		size_t resultCount = 0;
			//! Declared parameter types, in order (block/loop parameters).
		std::vector<WASM::ValueTypeCode> paramTypes;
		//! One register per declared parameter, the values the body starts with.
		std::vector<asmjit::x86::Gp> params;
		std::size_t paramCount = 0;
		//! Declared result types, in order (multi-value blocks included).
		std::vector<WASM::ValueTypeCode> resultTypes;
		//! One register per declared result, filled by a branch and read at `end`.
		std::vector<asmjit::x86::Gp> results;
		asmjit::Label begin;
		asmjit::Label alternate;
		asmjit::Label end;
		bool hasElse = false;
		//! True when the frame was entered in live code.
		bool entryReachable = true;
		//! True when the then arm fell through to `else`.
		bool thenReachable = false;
		//! True once a `br` targets this block's end.
		bool endTargeted = false;
		//! True for a `try_table` frame; its clauses route a pending exception.
		bool tryTable = false;
		//! Decoded catch clauses of a `try_table`, in source order.
		std::vector<WASM::CatchClause> catchClauses;
	};

	enum class BinaryOp { Add, Sub, Mul, And, Or, Xor };
	enum class ShiftOp { Left, ArithmeticRight, LogicalRight, RotateLeft, RotateRight };

	// ── Float materialisation ───────────────────────────────────────────────
	// A float value travels through the value stack as its IEEE bit pattern in
	// a Gp. These two helpers are the only places a scalar float becomes an
	// Xmm register (or goes back), which keeps the register-based emitter and
	// its block/select bookkeeping unchanged.

	//! Moves a float bit pattern into an Xmm register.
	asmjit::x86::Vec materializeFloat(const asmjit::x86::Gp& bits, int width)
	{
		asmjit::x86::Vec xmm = compiler.new_xmm("floatValue");
		if (width == 4)
			emitter.gpToVector32(xmm, bits);
		else
			emitter.gpToVector64(xmm, bits);
		return xmm;
	}

	//! Copies a float value out of an Xmm into a fresh Gp slot.
	asmjit::x86::Gp bitsOfFloat(const asmjit::x86::Vec& xmm, int width)
	{
		asmjit::x86::Gp bits = width == 4
				? compiler.new_gp32("floatBits") : compiler.new_gp64("floatBits");
		if (width == 4)
			emitter.vectorToGp32(bits, xmm);
		else
			emitter.vectorToGp64(bits, xmm);
		return bits;
	}

	//! Canonicalises an observable scalar float bit pattern when the
	//! deterministic profile is active (docs/proposals/PROFILES.md PR-3).
	//! The operation is on the bits, not an Xmm value: a float move may quiet a
	//! signalling NaN before it can be classified, and every non-NaN value must
	//! stay bit-exact.
	asmjit::x86::Gp canonicalizeFloatBits(const asmjit::x86::Gp& bits, int width)
	{
		if (!deterministicFloats)
			return bits;
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(width == 4 ? asmjit::TypeId::kUInt32 : asmjit::TypeId::kUInt64);
		signature.add_arg(width == 4 ? asmjit::TypeId::kUInt32 : asmjit::TypeId::kUInt64);
		const uint64_t helper = width == 4
				? reinterpret_cast<uint64_t>(&asmjitCanonicalF32Bits)
				: reinterpret_cast<uint64_t>(&asmjitCanonicalF64Bits);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), helper, signature),
				"AsmJit deterministic NaN helper call emission failed");
		invoke->set_arg(0, bits);
		asmjit::x86::Gp result = width == 4
				? compiler.new_gp32("canonicalNaN") : compiler.new_gp64("canonicalNaN");
		invoke->set_ret(0, result);
		return result;
	}

	//! Replaces the top scalar float on the value stack with its deterministic
	//! representation. Used by float-to-integer `reinterpret`, whose payload
	//! becomes host-observable.
	void canonicalizeTopFloat(int width)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit reinterpret operand stack underflow");
		values.back() = canonicalizeFloatBits(values.back(), width);
	}

	//! Returns one value from the function, materialising a float result.
	void emitFunctionReturn(const asmjit::x86::Gp& value)
	{
		if (isFloatKind(resultType)) {
			const int width = kindWidth(resultType);
			checkError(compiler.ret(materializeFloat(
					canonicalizeFloatBits(value, width), width)),
					"AsmJit float function-branch return failed");
		} else {
			checkError(compiler.ret(value), "AsmJit function-branch return failed");
		}
	}

	// ── v128 as a 16-byte stack buffer ──────────────────────────────────────
	// A v128 value is the address of a 16-byte stack buffer in a Gp, so the
	// register-based value stack, block transfer, select and locals keep their
	// existing shape. The buffer is immutable once produced; every operation
	// allocates a fresh one, which is what makes the representation safe across
	// loop iterations (one buffer per emission, rewritten before each use).

	//! Allocates a 16-byte stack buffer and returns its address.
	asmjit::x86::Gp newVectorBuffer(const char* name)
	{
		asmjit::x86::Mem storage = compiler.new_stack(16, 16, name);
		auto address = compiler.new_gp_ptr(name);
		emitter.lea(address, storage);
		return address;
	}

	//! Bytes a staging frame needs for a call: 8 per scalar slot, 16 per v128,
	//! covering both the parameter block and the result block. A multi-result
	//! callee writes every result slot, so a fixed `args + 16` reservation is an
	//! out-of-bounds write that corrupts the caller's own stack slots.
	static uint32_t frameCallStorageBytes(const WASM::FuncType& functionType)
	{
		uint32_t bytes = frameResultOffsetBytes(functionType);
		for (const auto& result : functionType.results)
			bytes += result.val.opcode == WASM::ValueTypeCode::V128 ? 16u : 8u;
		return bytes < 16 ? 16u : bytes;
	}

	//! Bytes the parameters occupy in a staging frame: 8 per scalar, 16 per v128.
	static uint32_t frameResultOffsetBytes(const WASM::FuncType& functionType)
	{
		uint32_t bytes = 0;
		for (const auto& parameter : functionType.params)
			bytes += parameter.val.opcode == WASM::ValueTypeCode::V128 ? 16u : 8u;
		return bytes;
	}

	//! Copies 16 bytes between two buffer addresses.
	void copyVector(const asmjit::x86::Gp& destination, const asmjit::x86::Gp& source)
	{
		for (int word = 0; word < 2; ++word) {
			auto value = compiler.new_gp64("v128Word");
			emitter.mov(value,
					asmjit::x86::qword_ptr(source, word * 8));
			emitter.mov(asmjit::x86::qword_ptr(destination, word * 8), value);
		}
	}

	//! Transfers one block/loop value, copying a vector's 16 bytes.
	//! \param destination Destination buffer or register.
	//! \param source Source buffer or register.
	//! \param code Value type code.
	//! \param description Error description.
	void transferValue(const asmjit::x86::Gp& destination, const asmjit::x86::Gp& source,
			WASM::ValueTypeCode code, const char* description)
	{
		if (code == WASM::ValueTypeCode::V128)
			copyVector(destination, source);
		else
			emitter.mov(destination, source);
	}

	//! Writes 16 zero bytes to a buffer.
	void zeroVector(const asmjit::x86::Gp& buffer)
	{
		for (int word = 0; word < 2; ++word) {
			emitter.mov(asmjit::x86::qword_ptr(buffer, word * 8),
					static_cast<uint64_t>(0));
		}
	}

	//! Sets a call's value arguments from prepared operands.
	void setCallArguments(asmjit::InvokeNode* invoke,
			const std::vector<asmjit::Reg>& prepared)
	{
		for (size_t i = 0; i < prepared.size(); ++i)
			invoke->set_arg(i + 1, prepared[i]);
	}

	//! Spills the reference-typed arguments of a call into stack slots, so the
	//! counts the operand stack owns survive the invoke, which clobbers every
	//! argument register.
	//! \param type Signature of the callee.
	//! \param arguments Argument values, already popped from the operand stack.
	//! \return One stack slot per reference argument, in argument order.
	std::vector<asmjit::x86::Mem> spillCallReferenceArguments(const WASM::FuncType& type,
			const std::vector<asmjit::x86::Gp>& arguments)
	{
		std::vector<asmjit::x86::Mem> kept;
		for (std::size_t i = 0; i < arguments.size() && i < type.params.size(); ++i) {
			// A packed parameter travels as a scalar; a v128 is 16 bytes and is
			// not a reference either.
			if (type.params[i].isPacked || !isReferenceKind(type.params[i].val.opcode))
				continue;
			asmjit::x86::Mem slot = compiler.new_stack(8, 8, "refArgKeep");
			emitter.mov(slot, arguments[i]);
			kept.push_back(slot);
		}
		return kept;
	}

	//! Releases the operand stack's counts for a call's reference arguments.
	//! A parameter is the caller's borrow, so the caller gives its count back
	//! once the callee has returned, having retained whatever it keeps.
	//! \param kept Slots written by spillCallReferenceArguments().
	void dropCallReferenceArguments(const std::vector<asmjit::x86::Mem>& kept)
	{
		for (const asmjit::x86::Mem& slot : kept) {
			auto value = compiler.new_gp64("refArgDrop");
			emitter.mov(value, slot);
			emitRefDrop(value);
		}
	}

	//! Takes the operand stack's own count of a native import's reference
	//! result. A host C function hands back a *borrow*: it keeps its own count,
	//! so the operand stack clones before it can hold the value across a
	//! safepoint or release it (contrast a Wasm callee, which hands over the
	//! count its own operand stack held).
	//! \param type Signature of the callee.
	void retainNativeCallResult(const WASM::FuncType& type)
	{
		if (type.results.empty() || values.empty())
			return;
		if (!isReferenceKind(type.results.front().val.opcode))
			return;
		values.back() = emitRefClone(values.back());
		markReference(values.back());
	}

	//! Materialises every call argument before the invoke node is appended.
	std::vector<asmjit::Reg> prepareCallArguments(const WASM::FuncType& type,
			const std::vector<asmjit::x86::Gp>& arguments)
	{
		std::vector<asmjit::Reg> prepared;
		prepared.reserve(arguments.size());
		for (size_t i = 0; i < arguments.size(); ++i) {
			const WASM::ValueType& declared = type.params[i].val;
			if (isFloatKind(declared.opcode))
				prepared.emplace_back(
						materializeFloat(arguments[i], kindWidth(declared.opcode)));
			else
				prepared.emplace_back(arguments[i]);
		}
		return prepared;
	}

	//! Publishes a call's single result, moving a float result out of its Xmm.
	void emitCallResult(asmjit::InvokeNode* invoke, const WASM::FuncType& type,
			const char* name)
	{
		if (type.results.empty())
			return;
		const WASM::ValueType& declared = type.results.front().val;
		if (isFloatKind(declared.opcode)) {
			auto result = compiler.new_xmm("callFloatResult");
			invoke->set_ret(0, result);
			values.push_back(bitsOfFloat(result, kindWidth(declared.opcode)));
		} else {
			auto result = newIntegerRegister(compiler, declared, name);
			invoke->set_ret(0, result);
			// A reference result arrives owning one count: the callee hands over
			// the count its own operand stack held, so the slot is marked rather
			// than cloned, and the caller's `drop` (or its `return`) releases it.
			if (isReferenceKind(declared.opcode))
				markReference(result);
			values.push_back(result);
		}
	}
	void emitFloatBinary(asmjit::ujit::UniOpVVV op32, asmjit::ujit::UniOpVVV op64, int width)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit float binary operand stack underflow");
		const auto second = values.back(); values.pop_back();
		const auto first = values.back(); values.pop_back();
		const auto lhs = materializeFloat(first, width);
		const auto rhs = materializeFloat(second, width);
		emitter.vector3(width == 4 ? op32 : op64, lhs, lhs, rhs);
		values.push_back(bitsOfFloat(lhs, width));
	}

	//! Calls a one-argument float helper over the top slot.
	void emitFloatHelper1(uint64_t function, int width)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit float helper operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		// The argument must be materialised before the invoke node is appended:
		// AsmJit emits nodes in program order, so instructions created after the
		// call would run after it.
		const auto argument = materializeFloat(bits, width);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), function, signature),
				"AsmJit float helper call emission failed");
		invoke->set_arg(0, argument);
		auto result = compiler.new_xmm("floatHelperResult");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, width));
	}

	//! Calls a two-argument float helper over the top two slots.
	void emitFloatHelper2(uint64_t function, int width)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit float helper operand stack underflow");
		const auto second = values.back(); values.pop_back();
		const auto first = values.back(); values.pop_back();
		const auto firstArgument = materializeFloat(first, width);
		const auto secondArgument = materializeFloat(second, width);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		signature.add_arg(width == 4 ? asmjit::TypeId::kFloat32 : asmjit::TypeId::kFloat64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), function, signature),
				"AsmJit float helper call emission failed");
		invoke->set_arg(0, firstArgument);
		invoke->set_arg(1, secondArgument);
		auto result = compiler.new_xmm("floatHelperResult");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, width));
	}

	//! Emits a float comparison of the top two slots, producing an i32 0/1.
	void emitFloatCompare(FloatCompareOp operation, int width)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit float compare operand stack underflow");
		const auto second = values.back(); values.pop_back();
		const auto first = values.back(); values.pop_back();
		const auto lhs = materializeFloat(first, width);
		const auto rhs = materializeFloat(second, width);

		// The universal comparison is the IEEE-754 predicate itself: it yields a
		// per-lane all-ones mask and already answers "false" for a NaN operand,
		// so the parity (`kP`/`kNP`) juggling the host sequence needed — and its
		// zero-before-compare rule — are gone. The mask becomes the i32 flag by
		// taking its low lane and masking it to one bit.
		using Ops = asmjit::ujit::UniOpVVV;
		auto mask = compiler.new_xmm("floatMask");
		switch (operation) {
		case FloatCompareOp::Eq:
			emitter.vector3(width == 4 ? Ops::kCmpEqF32S : Ops::kCmpEqF64S, mask, lhs, rhs);
			break;
		case FloatCompareOp::Ne:
			emitter.vector3(width == 4 ? Ops::kCmpNeF32S : Ops::kCmpNeF64S, mask, lhs, rhs);
			break;
		case FloatCompareOp::Lt:
			emitter.vector3(width == 4 ? Ops::kCmpLtF32S : Ops::kCmpLtF64S, mask, lhs, rhs);
			break;
		case FloatCompareOp::Le:
			emitter.vector3(width == 4 ? Ops::kCmpLeF32S : Ops::kCmpLeF64S, mask, lhs, rhs);
			break;
		case FloatCompareOp::Gt:
			emitter.vector3(width == 4 ? Ops::kCmpGtF32S : Ops::kCmpGtF64S, mask, lhs, rhs);
			break;
		case FloatCompareOp::Ge:
			emitter.vector3(width == 4 ? Ops::kCmpGeF32S : Ops::kCmpGeF64S, mask, lhs, rhs);
			break;
		}
		auto flag = compiler.new_gp32("floatFlag");
		emitter.vectorToGp32(flag, mask);
		emitter.and_(flag, asmjit::Imm(1));
		values.push_back(flag);
	}

	//! Absolute value as a bit operation (clear the sign bit).
	void emitFloatAbs()
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit f32.abs operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		emitter.shl(bits, 1);
		emitter.shr(bits, 1);
		values.push_back(bits);
	}

	//! Negation as a bit operation (toggle the sign bit).
	void emitFloatNeg(int width)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit f32.neg operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		const auto sign = width == 4
				? compiler.new_gp32("floatSign") : compiler.new_gp64("floatSign");
		emitter.mov(sign, 1);
		emitter.shl(sign, width == 4 ? 31 : 63);
		emitter.xor_(bits, sign);
		values.push_back(bits);
	}

	//! `copysign` as bit operations: keep x's magnitude and y's sign.
	void emitFloatCopysign(int width)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit f32.copysign operand stack underflow");
		const auto y = values.back(); values.pop_back();
		const auto x = values.back(); values.pop_back();
		const auto sign = width == 4
				? compiler.new_gp32("copySign") : compiler.new_gp64("copySign");
		const auto mask = width == 4
				? compiler.new_gp32("signMask") : compiler.new_gp64("signMask");
		emitter.mov(mask, 1);
		emitter.shl(mask, width == 4 ? 31 : 63);
		emitter.mov(sign, y);
		emitter.and_(sign, mask);
		emitter.shl(x, 1);
		emitter.shr(x, 1);
		emitter.or_(x, sign);
		values.push_back(x);
	}

	//! Calls a checked float-to-integer helper over the top slot.
	void emitFloatToInt(uint64_t function, int floatWidth, int intWidth)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit float-to-int operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		const auto argument = materializeFloat(bits, floatWidth);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(intWidth == 4 ? asmjit::TypeId::kInt32 : asmjit::TypeId::kInt64);
		signature.add_arg(floatWidth == 4 ? asmjit::TypeId::kFloat32
										  : asmjit::TypeId::kFloat64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), function, signature),
				"AsmJit float-to-int helper emission failed");
		invoke->set_arg(0, argument);
		auto result = intWidth == 4
				? compiler.new_gp32("truncResult") : compiler.new_gp64("truncResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	//! Calls an integer-to-float helper over the top slot.
	void emitIntToFloat(uint64_t function, int intWidth, int floatWidth)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit int-to-float operand stack underflow");
		const auto source = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(floatWidth == 4 ? asmjit::TypeId::kFloat32
										  : asmjit::TypeId::kFloat64);
		signature.add_arg(intWidth == 4 ? asmjit::TypeId::kInt32 : asmjit::TypeId::kInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), function, signature),
				"AsmJit int-to-float helper emission failed");
		invoke->set_arg(0, source);
		auto result = compiler.new_xmm("convertResult");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, floatWidth));
	}

	//! Calls a float-to-float helper (demote/promote).
	void emitFloatToFloat(uint64_t function, int sourceWidth, int resultWidth)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit float conversion operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		const auto argument = materializeFloat(bits, sourceWidth);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(resultWidth == 4 ? asmjit::TypeId::kFloat32
										   : asmjit::TypeId::kFloat64);
		signature.add_arg(sourceWidth == 4 ? asmjit::TypeId::kFloat32
										   : asmjit::TypeId::kFloat64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), function, signature),
				"AsmJit float conversion helper emission failed");
		invoke->set_arg(0, argument);
		auto result = compiler.new_xmm("convertResult");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, resultWidth));
	}


	void emitSelect()
    {
        if (inDeadCode())
            return;
        if (values.size() < 3)
            throw std::runtime_error("AsmJit select operand stack underflow");
        const auto condition = values.back();
        values.pop_back();
        const auto second = values.back();
        values.pop_back();
        const auto first = values.back();
        values.pop_back();
        if (condition.size() != 4 || first.size() != second.size()
                || (first.size() != 4 && first.size() != 8))
            throw std::runtime_error("AsmJit select requires an i32 condition and matching i32/i64/f32/f64 values");
        // Both arms were pushed, so the operand stack owns a count for each even
        // though only one becomes the result. The winner's count crosses over
        // (its slot survives); the loser's must be released, and which arm loses
        // is only known at run time.
        const bool referenceArms = isReferenceValue(first) || isReferenceValue(second);
        referenceValues.erase(first.id());
        referenceValues.erase(second.id());
        auto result = first.size() == 8
                ? compiler.new_gp64("selectResult") : compiler.new_gp32("selectResult");
        auto loser = first.size() == 8
                ? compiler.new_gp64("selectLoser") : compiler.new_gp32("selectLoser");
        auto chooseSecond = compiler.new_label();
        auto done = compiler.new_label();
        emitter.mov(result, first);
        if (referenceArms)
            emitter.mov(loser, second);
        emitter.j(chooseSecond, asmjit::ujit::test_z(condition, condition));
        emitter.jmp(done);
        checkError(compiler.bind(chooseSecond), "AsmJit select second-value label failed");
        emitter.mov(result, second);
        if (referenceArms)
            emitter.mov(loser, first);
        checkError(compiler.bind(done), "AsmJit select completion label failed");
        if (referenceArms) {
            // Release the arm the condition did not pick. Dropping a null
            // reference is a no-op, so a null arm is free.
            emitRefDrop(loser);
            markReference(result);
        }
        values.push_back(result);
    }

    void emitMemoryCopy(WASM::MemIdx destination, WASM::MemIdx source)
    {
        const WASM::MemoryIndexType destinationType =
                validateMemoryIndex(destination).indexType;
        const WASM::MemoryIndexType sourceType = validateMemoryIndex(source).indexType;
        if (destinationType != sourceType)
            throw std::runtime_error("AsmJit memory.copy memories have different index types");
        const bool is64 = destinationType == WASM::MemoryIndexType::I64;
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.copy operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto sourceAddress = values.back(); values.pop_back();
        const auto destinationAddress = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto dstMemory = compiler.new_gp32("copyDestinationMemory");
        auto srcMemory = compiler.new_gp32("copySourceMemory");
        emitter.mov(dstMemory, static_cast<uint32_t>(destination));
        emitter.mov(srcMemory, static_cast<uint32_t>(source));
        checkError(compiler.invoke(asmjit::Out(invoke),
                reinterpret_cast<uint64_t>(is64
                ? reinterpret_cast<void*>(&asmjitMemoryCopy64)
                : reinterpret_cast<void*>(&asmjitMemoryCopy)), signature),
                "AsmJit memory.copy helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, dstMemory);
        invoke->set_arg(2, srcMemory);
        invoke->set_arg(3, destinationAddress);
        invoke->set_arg(4, sourceAddress);
        invoke->set_arg(5, length);
        emitPendingTrapReturn();
    }


    void emitMemoryInit(uint32_t dataIndex, WASM::MemIdx memoryIndex)
    {
        const bool is64 = validateMemoryIndex(memoryIndex).indexType == WASM::MemoryIndexType::I64;
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.init operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto source = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto data = compiler.new_gp32("memoryInitDataIndex");
        auto memory = compiler.new_gp32("memoryInitMemoryIndex");
        emitter.mov(data, dataIndex);
        emitter.mov(memory, static_cast<uint32_t>(memoryIndex));
        checkError(compiler.invoke(asmjit::Out(invoke),
                reinterpret_cast<uint64_t>(is64
                ? reinterpret_cast<void*>(&asmjitMemoryInit64)
                : reinterpret_cast<void*>(&asmjitMemoryInit)), signature),
                "AsmJit memory.init helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, data);
        invoke->set_arg(2, memory);
        invoke->set_arg(3, destination);
        invoke->set_arg(4, source);
        invoke->set_arg(5, length);
        emitPendingTrapReturn();
    }

    void emitMemoryFill(WASM::MemIdx index)
    {
        const bool is64 = validateMemoryIndex(index).indexType == WASM::MemoryIndexType::I64;
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.fill operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto value = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUInt32);
        signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto memoryIndex = compiler.new_gp32("fillMemoryIndex");
        emitter.mov(memoryIndex, static_cast<uint32_t>(index));
        checkError(compiler.invoke(asmjit::Out(invoke),
                reinterpret_cast<uint64_t>(is64
                ? reinterpret_cast<void*>(&asmjitMemoryFill64)
                : reinterpret_cast<void*>(&asmjitMemoryFill)), signature),
                "AsmJit memory.fill helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, memoryIndex);
        invoke->set_arg(2, destination);
        invoke->set_arg(3, value);
        invoke->set_arg(4, length);
        emitPendingTrapReturn();
    }

	void pushControlFrame(ControlKind kind, const WASM::BlockType& type)
	{
		if (controls.size() >= WASM::kMaxControlNesting)
			throw std::runtime_error("AsmJit control nesting is too deep");
		ControlFrame frame;
		frame.kind = kind;
		frame.entryReachable = !armTerminated;
		frame.paramTypes = blockParamTypes(type);
		frame.paramCount = frame.paramTypes.size();
		if (values.size() < frame.paramCount) {
			// Polymorphic dead code may name block parameters that no live
			// producer supplied. The block can never execute, so give the frame
			// placeholder values instead of rejecting the body.
			if (!armTerminated)
				throw std::runtime_error("AsmJit block parameters underflow the operand stack");
			frame.stackHeight = 0;
			for (WASM::ValueTypeCode code : frame.paramTypes) {
				frame.params.push_back(kindWidth(code) == 8
						? compiler.new_gp64("deadBlockParam")
						: compiler.new_gp32("deadBlockParam"));
			}
		} else {
			frame.stackHeight = values.size() - frame.paramCount;
			for (std::size_t i = 0; i < frame.paramCount; ++i)
				frame.params.push_back(values[frame.stackHeight + i]);
		}
		frame.resultTypes = blockResultTypes(type);
		frame.resultCount = frame.resultTypes.size();
		for (WASM::ValueTypeCode code : frame.resultTypes) {
			if (code == WASM::ValueTypeCode::V128)
				frame.results.push_back(newVectorBuffer("blockVectorResult"));
			else if (kindWidth(code) == 0)
				throw std::runtime_error("AsmJit structured control flow does not model this block result");
			else
				frame.results.push_back(kindWidth(code) == 8
						? compiler.new_gp64("blockResult") : compiler.new_gp32("blockResult"));
		}
		if (kind == ControlKind::Loop) {
			frame.begin = compiler.new_label();
			frame.end = compiler.new_label();
		} else {
			frame.end = compiler.new_label();
		}
		controls.push_back(frame);
	}

	std::vector<WASM::ValueTypeCode> blockParamTypes(const WASM::BlockType& type) const
	{
		std::vector<WASM::ValueTypeCode> params;
		if (type.isVoid() || !type.isTypeIndex)
			return params;
		if (type.typeIndex < 0 || static_cast<uint32_t>(type.typeIndex) >= module.typeCount())
			throw std::runtime_error("AsmJit block type index is out of range");
		const WASM::FuncType blockType = module.funcTypeAt(static_cast<uint32_t>(type.typeIndex));
		for (const auto& param : blockType.params) {
			if (param.val.opcode != WASM::ValueTypeCode::V128)
				requireScalar(param.val, "block parameters");
			params.push_back(param.val.opcode);
		}
		return params;
	}

	std::vector<WASM::ValueTypeCode> blockResultTypes(const WASM::BlockType& type) const
	{
		std::vector<WASM::ValueTypeCode> results;
		if (type.isVoid())
			return results;
		if (type.isTypeIndex) {
			if (type.typeIndex < 0 || static_cast<uint32_t>(type.typeIndex) >= module.typeCount())
				throw std::runtime_error("AsmJit block type index is out of range");
			const WASM::FuncType blockType = module.funcTypeAt(static_cast<uint32_t>(type.typeIndex));
			for (const auto& result : blockType.results) {
				if (result.val.opcode != WASM::ValueTypeCode::V128)
					requireScalar(result.val, "block results");
				results.push_back(result.val.opcode);
			}
			return results;
		}
		if (type.valType != WASM::ValueTypeCode::V128 && kindWidth(type.valType) == 0)
			throw std::runtime_error("AsmJit structured control flow does not model this block result");
		results.push_back(type.valType);
		return results;
	}

	void storeBlockResult(const ControlFrame& frame, const char* description)
	{
		if (values.size() != frame.stackHeight + frame.resultCount)
			throw std::runtime_error(std::string("AsmJit ") + description
					+ " leaves an unexpected operand stack shape");
		const size_t first = values.size() - frame.resultCount;
		for (size_t i = 0; i < frame.resultCount; ++i)
			transferValue(frame.results[i], values[first + i], frame.resultTypes[i],
					"AsmJit structured result transfer failed");
	}
	void emitBinary(BinaryOp operation)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32 binary operation stack underflow");
		asmjit::x86::Gp rhs = values.back();
		values.pop_back();
		asmjit::x86::Gp lhs = values.back();
		values.pop_back();
		if (!lhs.is_valid() || !rhs.is_valid() || lhs.size() != rhs.size()) {
			throw std::runtime_error("AsmJit i32 binary operand shape mismatch: lhs="
					+ std::to_string(lhs.size()) + " rhs=" + std::to_string(rhs.size()));
		}
		switch (operation) {
		case BinaryOp::Add:
			emitter.add(lhs, rhs);
			break;
		case BinaryOp::Sub:
			emitter.sub(lhs, rhs);
			break;
		case BinaryOp::Mul:
			emitter.imul(lhs, rhs);
			break;
		case BinaryOp::And:
			emitter.and_(lhs, rhs);
			break;
		case BinaryOp::Or:
			emitter.or_(lhs, rhs);
			break;
		case BinaryOp::Xor:
			emitter.xor_(lhs, rhs);
			break;
		}
		values.push_back(lhs);
	}

	const WASM::GlobalType& validateGlobalIndex(WASM::GlobalIdx index) const
	{
		// The module's global index space is imports first, then definitions.
		// `importGlobals` and `globals` are separate vectors; neither alone is
		// the complete space.
		const WASM::GlobalType* type = nullptr;
		if (index < module.importGlobals.size()) {
			type = &module.importGlobals[index].global;
		} else {
			const std::size_t defined = static_cast<std::size_t>(index)
					- module.importGlobals.size();
			if (defined >= module.globals.size())
				throw std::runtime_error("AsmJit global instruction references an unknown global");
			type = &module.globals[defined].type;
		}
		if (type->contentType.opcode != WASM::ValueTypeCode::V128)
			requireScalar(type->contentType, "globals");
		return *type;
	}

	asmjit::x86::Gp emitGlobalCell(WASM::GlobalIdx index)
	{
		(void)validateGlobalIndex(index);
		auto globals = compiler.new_gp_ptr("globals");
		emitter.mov(globals, asmjit::x86::ptr(vmContext,
				static_cast<int32_t>(offsetof(WASM::VMContext, globals))));
		auto cell = compiler.new_gp_ptr("globalCell");
		emitter.mov(cell, asmjit::x86::ptr(globals,
				static_cast<int32_t>(index * sizeof(WASM::Value*))));
		return cell;
	}

	void emitGlobalGet(WASM::GlobalIdx index)
	{
		auto cell = emitGlobalCell(index);
		const auto& type = validateGlobalIndex(index);
		if (type.contentType.opcode == WASM::ValueTypeCode::V128) {
			auto buffer = newVectorBuffer("globalVector");
			for (int word = 0; word < 2; ++word) {
				auto bits = compiler.new_gp64("globalVectorWord");
				emitter.mov(bits, asmjit::x86::qword_ptr(cell,
						static_cast<int32_t>(offsetof(WASM::Value, v128) + word * 8)));
				emitter.mov(asmjit::x86::qword_ptr(buffer, word * 8), bits);
			}
			values.push_back(buffer);
			return;
		}
		auto value = newIntegerRegister(compiler, type.contentType, "globalValue");
		if (kindWidth(type.contentType.opcode) == 8) {
			emitter.mov(value, asmjit::x86::qword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i64))));
		} else {
			emitter.mov(value, asmjit::x86::dword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i32))));
		}
		if (isReferenceKind(type.contentType.opcode)) {
			// The global cell keeps its own count; the clone is the stack's.
			value = emitRefClone(value);
			markReference(value);
		}
		values.push_back(value);
	}

	void emitGlobalSet(WASM::GlobalIdx index)
	{
		if (inDeadCode())
			return;
		const WASM::GlobalType& type = validateGlobalIndex(index);
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.set references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.set operand stack underflow");
		auto value = values.back(); values.pop_back();
		if (isReferenceKind(type.contentType.opcode)) {
			asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
			signature.set_ret(asmjit::TypeId::kVoid);
			signature.add_arg(asmjit::TypeId::kUIntPtr);
			signature.add_arg(asmjit::TypeId::kUInt32);
			signature.add_arg(asmjit::TypeId::kUIntPtr);
			asmjit::InvokeNode* invoke = nullptr;
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&WASM::wasm_global_set_ref), signature),
					"AsmJit global.set reference barrier call failed");
			invoke->set_arg(0, vmContext);
			invoke->set_arg(1, asmjit::Imm(static_cast<uint32_t>(index)));
			invoke->set_arg(2, value);
			emitPendingTrapReturn();
			// The operand stack owned this reference; the store retained its own.
			emitRefDrop(value);
			return;
		}
		// An exported global is host-observable
		// (docs/proposals/PROFILES.md PR-3).
		const asmjit::x86::Gp stored = isFloatKind(type.contentType.opcode)
				? canonicalizeFloatBits(value, kindWidth(type.contentType.opcode)) : value;
		auto cell = emitGlobalCell(index);
		if (type.contentType.opcode == WASM::ValueTypeCode::V128) {
			for (int word = 0; word < 2; ++word) {
				auto bits = compiler.new_gp64("globalVectorWord");
				emitter.mov(bits,
						asmjit::x86::qword_ptr(value, word * 8));
				emitter.mov(asmjit::x86::qword_ptr(cell,
						static_cast<int32_t>(offsetof(WASM::Value, v128) + word * 8)), bits);
			}
			return;
		}
		if (kindWidth(type.contentType.opcode) == 8) {
			emitter.mov(asmjit::x86::qword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i64))), stored);
		} else {
			emitter.mov(asmjit::x86::dword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i32))), stored);
		}
	}

	const WASM::MemoryType& validateMemoryIndex(WASM::MemIdx index) const
	{
		// Module::memories holds only definitions; imported memories precede
		// them in the runtime's memory index space.
		if (index < module.importMemories.size())
			return module.importMemories[index].memory;
		const std::size_t defined = static_cast<std::size_t>(index)
				- module.importMemories.size();
		if (defined >= module.memories.size())
			throw std::runtime_error("AsmJit memory instruction references an unknown memory");
		return module.memories[defined];
	}

	void emitMemorySize(WASM::MemIdx index)
	{
		if (inDeadCode())
			return;
		const bool is64 = validateMemoryIndex(index).indexType
				== WASM::MemoryIndexType::I64;
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		auto memoryIndex = compiler.new_gp32("memorySizeIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(index));
		if (is64)
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemorySize64), signature),
					"AsmJit memory64.size helper call emission failed");
		else
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemorySize), signature),
					"AsmJit memory32.size helper call emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		auto result = is64
				? compiler.new_gp64("memorySize") : compiler.new_gp32("memorySize");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void emitMemoryGrow(WASM::MemIdx index)
	{
		if (inDeadCode())
			return;
		const bool is64 = validateMemoryIndex(index).indexType
				== WASM::MemoryIndexType::I64;
		if (values.empty())
			throw std::runtime_error("AsmJit memory.grow operand stack underflow");
		auto delta = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		auto memoryIndex = compiler.new_gp32("memoryGrowIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(index));
		if (is64)
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemoryGrow64), signature),
					"AsmJit memory64.grow helper call emission failed");
		else
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemoryGrow), signature),
					"AsmJit memory32.grow helper call emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, delta);
		auto result = is64
				? compiler.new_gp64("memoryGrowResult") : compiler.new_gp32("memoryGrowResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void validateMemoryArg(const WASM::MemArg& arg, uint32_t width) const
	{
		if (arg.isArray)
			throw std::runtime_error("AsmJit scalar loads/stores do not support GC-array memargs");
		const WASM::MemoryType& memory = validateMemoryIndex(arg.memidx);
		if (memory.indexType == WASM::MemoryIndexType::I32 && arg.offset > UINT32_MAX)
			throw std::runtime_error("AsmJit memory32 offset exceeds the encoded u32 range");
		(void)width;
	}

	//! Computes a GC-array byte address through the shared bounds-checking helper.
	asmjit::x86::Gp emitGcArrayAddress(const WASM::MemArg& arg, uint32_t width,
			const asmjit::x86::Gp& reference, const asmjit::x86::Gp& address)
	{
		const WASM::CoreTypeView view = module.typeView(arg.arrayTypeIdx);
		const uint32_t elementSize = static_cast<uint32_t>(
				WASM::gcStorageBytes(view.arrayElement().storage));
		const uint32_t dataOffset = static_cast<uint32_t>(
				WASM::gcArrayDataOffset(view.arrayElement().storage));
		const uint32_t packed = (width & 0xFFu) | (elementSize << 8)
				| (dataOffset << 16);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt32);
		auto offset = compiler.new_gp64("arrayOffset");
		emitter.mov(offset, arg.offset);
		auto packedValue = compiler.new_gp32("arrayPacked");
		emitter.mov(packedValue, packed);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&WASM::gcArrayByteAddress), signature),
				"AsmJit GC-array address helper call failed");
		invoke->set_arg(0, reference);
		invoke->set_arg(1, address);
		invoke->set_arg(2, offset);
		invoke->set_arg(3, packedValue);
		auto pointer = compiler.new_gp_ptr("arrayAddress");
		invoke->set_ret(0, pointer);
		emitPendingTrapReturn();
		return pointer;
	}

	asmjit::x86::Gp emitMemoryAddress(const WASM::MemArg& arg, uint32_t width,
			const asmjit::x86::Gp& wasmAddress)
	{
		validateMemoryArg(arg, width);
		const bool is64 = validateMemoryIndex(arg.memidx).indexType
				== WASM::MemoryIndexType::I64;
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		signature.add_arg(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		// Materialise every argument *before* appending the invoke node: an
		// operand must be defined at the call site, not after it, or the
		// register allocator hands the callee whatever the register held.
		auto memoryIndex = compiler.new_gp32("memoryIndex");
		emitter.mov(memoryIndex, static_cast<uint32_t>(arg.memidx));
		auto accessWidth = compiler.new_gp32("memoryWidth");
		emitter.mov(accessWidth, width);
		asmjit::x86::Gp offset;
		if (is64) {
			offset = compiler.new_gp64("memoryOffset");
			emitter.mov(offset, static_cast<uint64_t>(arg.offset));
		} else {
			offset = compiler.new_gp32("memoryOffset");
			emitter.mov(offset, static_cast<uint32_t>(arg.offset));
		}
		asmjit::InvokeNode* invoke = nullptr;
		if (is64)
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemoryAddress64), signature),
					"AsmJit memory64-address helper call emission failed");
		else
			checkError(compiler.invoke(asmjit::Out(invoke),
					reinterpret_cast<uint64_t>(&asmjitMemoryAddress), signature),
					"AsmJit memory32-address helper call emission failed");
		invoke->set_arg(0, vmContext);
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, wasmAddress);
		invoke->set_arg(3, offset);
		invoke->set_arg(4, accessWidth);
		auto pointer = compiler.new_gp_ptr("memoryAddress");
		invoke->set_ret(0, pointer);
		emitPendingTrapReturn();
		return pointer;
	}

	void emitF16Load(const WASM::MemArg& arg)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit f32.load_f16 operand stack underflow");
		auto wasmAddress = values.back();
		values.pop_back();
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array f16 load reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, 2, reference, wasmAddress);
		} else {
			pointer = emitMemoryAddress(arg, 2, wasmAddress);
		}
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kFloat32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_f32_load_f16), signature),
				"AsmJit f32.load_f16 helper call failed");
		invoke->set_arg(0, pointer);
		auto result = compiler.new_xmm("f16Load");
		invoke->set_ret(0, result);
		values.push_back(bitsOfFloat(result, 4));
	}

	void emitF16Store(const WASM::MemArg& arg)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit f32.store_f16 operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto wasmAddress = values.back();
		values.pop_back();
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array f16 store reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, 2, reference, wasmAddress);
		} else {
			pointer = emitMemoryAddress(arg, 2, wasmAddress);
		}
		const auto argument = materializeFloat(value, 4);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kFloat32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&wasm_f32_store_f16), signature),
				"AsmJit f32.store_f16 helper call failed");
		invoke->set_arg(0, pointer);
		invoke->set_arg(1, argument);
	}

	void emitMemoryLoad(const WASM::MemArg& arg, uint32_t width, LoadKind kind)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error("AsmJit memory.load operand stack underflow");
		auto wasmAddress = values.back();
		values.pop_back();
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array load reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, width, reference, wasmAddress);
		} else {
			pointer = emitMemoryAddress(arg, width, wasmAddress);
		}
		const bool wide = kind == LoadKind::I64 || kind == LoadKind::I64I8Signed
				|| kind == LoadKind::I64I8Unsigned || kind == LoadKind::I64I16Signed
				|| kind == LoadKind::I64I16Unsigned || kind == LoadKind::I64I32Signed
				|| kind == LoadKind::I64I32Unsigned || kind == LoadKind::F64;
		auto result = wide ? compiler.new_gp64("memoryLoad64") : compiler.new_gp32("memoryLoad32");
		switch (kind) {
		case LoadKind::I32:
			emitter.guestLoad32Z(result, asmjit::x86::dword_ptr(pointer)); break;
		case LoadKind::I8Signed:
			emitter.guestLoad8S(result, asmjit::x86::byte_ptr(pointer)); break;
		case LoadKind::I8Unsigned:
			emitter.guestLoad8Z(result, asmjit::x86::byte_ptr(pointer)); break;
		case LoadKind::I16Signed:
			emitter.guestLoad16S(result, asmjit::x86::word_ptr(pointer)); break;
		case LoadKind::I16Unsigned:
			emitter.guestLoad16Z(result, asmjit::x86::word_ptr(pointer)); break;
		case LoadKind::I64:
			emitter.guestLoad64(result, asmjit::x86::qword_ptr(pointer)); break;
		case LoadKind::I64I8Signed:
			emitter.guestLoad8S(result, asmjit::x86::byte_ptr(pointer)); break;
		case LoadKind::I64I8Unsigned:
			emitter.guestLoad8Z(result, asmjit::x86::byte_ptr(pointer)); break;
		case LoadKind::I64I16Signed:
			emitter.guestLoad16S(result, asmjit::x86::word_ptr(pointer)); break;
		case LoadKind::I64I16Unsigned:
			emitter.guestLoad16Z(result, asmjit::x86::word_ptr(pointer)); break;
		case LoadKind::I64I32Signed:
			emitter.guestLoad32S(result, asmjit::x86::dword_ptr(pointer)); break;
		case LoadKind::I64I32Unsigned:
			emitter.guestLoad32Z(result, asmjit::x86::dword_ptr(pointer)); break;
		case LoadKind::F32:
			emitter.guestLoad32Z(result, asmjit::x86::dword_ptr(pointer)); break;
		case LoadKind::F64:
			emitter.guestLoad64(result, asmjit::x86::qword_ptr(pointer)); break;
		}
		values.push_back(result);
	}

	void emitMemoryStore(const WASM::MemArg& arg, uint32_t width,
			bool canonicalizeFloat = false)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit memory.store operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto wasmAddress = values.back(); values.pop_back();
		// Memory is an observability point for a scalar float
		// (docs/proposals/PROFILES.md PR-3). Canonicalise before computing the
		// address so the helper call cannot disturb the address register.
		const asmjit::x86::Gp stored = canonicalizeFloat
				? canonicalizeFloatBits(value, static_cast<int>(width)) : value;
		asmjit::x86::Gp pointer;
		if (arg.isArray) {
			if (values.empty())
				throw std::runtime_error("AsmJit GC-array store reference underflow");
			auto reference = values.back();
			values.pop_back();
			pointer = emitGcArrayAddress(arg, width, reference, wasmAddress);
		} else {
			pointer = emitMemoryAddress(arg, width, wasmAddress);
		}
		switch (width) {
		case 1:
			emitter.guestStore8(asmjit::x86::byte_ptr(pointer), stored); break;
		case 2:
			emitter.guestStore16(asmjit::x86::word_ptr(pointer), stored); break;
		case 4:
			emitter.guestStore32(asmjit::x86::dword_ptr(pointer), stored); break;
		case 8:
			emitter.guestStore64(asmjit::x86::qword_ptr(pointer), stored); break;
		default:
			throw std::runtime_error("AsmJit internal error: unsupported memory-store width");
		}
	}

	void emitBitCount(BitCountOp operation)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i32 bit-count operand stack underflow");
		auto source = values.back(); values.pop_back();
		// The universal bit count is defined for zero as the register width, so
		// the host's zero branch and its scan-era `^ 31` adjustment are both gone.
		auto result = compiler.new_gp32("bitCount");
		if (operation == BitCountOp::LeadingZeros)
			emitter.clz(result, source);
		else
			emitter.ctz(result, source);
		values.push_back(result);
	}

	//! The 64-bit counterpart of `emitBitCount`, whose zero result is 64.
	void emitBitCount64(BitCountOp operation)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i64 bit-count operand stack underflow");
		auto source = values.back(); values.pop_back();
		auto result = compiler.new_gp64("bitCount64");
		if (operation == BitCountOp::LeadingZeros)
			emitter.clz(result, source);
		else
			emitter.ctz(result, source);
		values.push_back(result);
	}

	//! Calls a one-argument 64-bit integer helper over the top slot.
	void emitUnaryCall64(uint64_t (*function)(uint64_t), const char* operation)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + operation + " operand stack underflow");
		auto argument = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUInt64);
		signature.add_arg(asmjit::TypeId::kUInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(function), signature),
				"AsmJit 64-bit unary helper call emission failed");
		invoke->set_arg(0, argument);
		auto result = compiler.new_gp64("unaryArithmeticResult64");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

	void emitUnaryCall(uint32_t (*function)(uint32_t), const char* operation)
	{
		if (inDeadCode())
			return;
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + operation + " operand stack underflow");
		auto argument = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(function), signature),
				"AsmJit unary arithmetic helper call emission failed");
		invoke->set_arg(0, argument);
		auto result = compiler.new_gp32("unaryArithmeticResult");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

	template <typename Function>
	void emitCheckedBinaryCall(Function function, const char* operation)
	{
		if (values.size() < 2)
			throw std::runtime_error(std::string("AsmJit ") + operation + " operand stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kInt32);
		signature.add_arg(asmjit::TypeId::kInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(function), signature),
				"AsmJit checked arithmetic helper call emission failed");
		invoke->set_arg(0, lhs);
		invoke->set_arg(1, rhs);
		auto result = compiler.new_gp32("checkedArithmeticResult");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void emitSignExtend(uint32_t width)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit sign-extension operand stack underflow");
		auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("signExtended");
		if (width == 8)
			emitter.movsx8(result, source);
		else if (width == 16)
			emitter.movsx16(result, source);
		else
			throw std::runtime_error("AsmJit internal error: invalid sign-extension width");
		values.push_back(result);
	}

	void emitI32WrapI64()
	{
		if (inDeadCode())
			return;
		if (values.empty() || values.back().size() != 8)
			throw std::runtime_error("AsmJit i32.wrap_i64 requires an i64 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("wrappedI32");
		emitter.mov(result, source.r32());
		values.push_back(result);
	}

	void emitI64ExtendI32(bool isSigned)
	{
		if (inDeadCode())
			return;
		if (values.empty() || values.back().size() != 4)
			throw std::runtime_error("AsmJit i64.extend_i32 requires an i32 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp64("extendedI64");
		if (isSigned)
			emitter.movsx32(result, source);
		else
			emitter.mov(result.r32(), source);
		values.push_back(result);
	}

	void emitI64SignExtend(uint32_t width)
	{
		if (inDeadCode())
			return;
		if (values.empty() || values.back().size() != 8)
			throw std::runtime_error("AsmJit i64 sign-extension requires an i64 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp64("signExtendedI64");
		if (width == 8)
			emitter.movsx8(result, source);
		else if (width == 16)
			emitter.movsx16(result, source);
		else if (width == 32)
			emitter.movsx32(result, source.r32());
		else
			throw std::runtime_error("AsmJit internal error: invalid i64 sign-extension width");
		values.push_back(result);
	}

	//! Emits `lhs <condition> rhs` as an i32 0/1.
	void emitCompare(asmjit::x86::CondCode condition)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit integer comparison operand stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		auto result = compiler.new_gp32("comparison");
		// The condition carries its own comparison, so one universal `select`
		// replaces the zero/compare/setcc triple and defines the result on both
		// paths. The condition code is asmjit's target-independent enum. A
		// 64-bit comparison selects at its own width and is then narrowed,
		// because `select` requires a destination as wide as its operands.
		const asmjit::ujit::UniCondition test(asmjit::ujit::UniOpCond::kCompare,
				condition, lhs, rhs);
		emitter.selectFlag(result, lhs, test);
		values.push_back(result);
	}

	void emitIntegerBinary(BinaryOp operation)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i64 binary operation stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		switch (operation) {
		case BinaryOp::Add: emitter.add(lhs, rhs); break;
		case BinaryOp::Sub: emitter.sub(lhs, rhs); break;
		case BinaryOp::Mul: emitter.imul(lhs, rhs); break;
		case BinaryOp::And: emitter.and_(lhs, rhs); break;
		case BinaryOp::Or: emitter.or_(lhs, rhs); break;
		case BinaryOp::Xor: emitter.xor_(lhs, rhs); break;
		}
		values.push_back(lhs);
	}

	void emitIntegerShift(ShiftOp operation)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i64 shift operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto value = values.back(); values.pop_back();
		auto maskedCount = compiler.new_gp64("i64ShiftCount");
		emitter.mov(maskedCount, count);
		emitter.and_(maskedCount, asmjit::Imm(63));
		// The count is an ordinary operand in the universal vocabulary: the fixed
		// count register and the stack round-trip the host lowering needed are gone.
		auto shifted = compiler.new_gp64("i64ShiftValue");
		switch (operation) {
		case ShiftOp::Left: emitter.shl(shifted, value, maskedCount); break;
		case ShiftOp::ArithmeticRight: emitter.sar(shifted, value, maskedCount); break;
		case ShiftOp::LogicalRight: emitter.shr(shifted, value, maskedCount); break;
		case ShiftOp::RotateLeft: emitter.rol(shifted, value, maskedCount); break;
		case ShiftOp::RotateRight: emitter.ror(shifted, value, maskedCount); break;
		}
		values.push_back(shifted);
	}

	template <typename Function>
	void emitCheckedI64Call(Function function, const char* operation)
	{
		if (values.size() < 2)
			throw std::runtime_error(std::string("AsmJit ") + operation + " operand stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kInt64);
		signature.add_arg(asmjit::TypeId::kInt64);
		signature.add_arg(asmjit::TypeId::kInt64);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(function), signature),
				"AsmJit checked i64 arithmetic helper emission failed");
		invoke->set_arg(0, lhs);
		invoke->set_arg(1, rhs);
		auto result = compiler.new_gp64("checkedI64Result");
		invoke->set_ret(0, result);
		values.push_back(result);
		emitPendingTrapReturn();
	}

	void emitShift(ShiftOp operation)
	{
		if (inDeadCode())
			return;
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32 shift operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto value = values.back(); values.pop_back();
		// The low five bits are used by WebAssembly; masking explicitly also
		// documents the semantic rule for the generated sequence.
		auto maskedCount = compiler.new_gp32("shiftCount");
		emitter.mov(maskedCount, count);
		emitter.and_(maskedCount, asmjit::Imm(31));
		// The count is an ordinary operand in the universal vocabulary: the fixed
		// count register and the stack round-trip the host lowering needed are gone.
		auto shifted = compiler.new_gp32("shiftValue");
		switch (operation) {
		case ShiftOp::Left:
			emitter.shl(shifted, value, maskedCount); break;
		case ShiftOp::ArithmeticRight:
			emitter.sar(shifted, value, maskedCount); break;
		case ShiftOp::LogicalRight:
			emitter.shr(shifted, value, maskedCount); break;
		case ShiftOp::RotateLeft:
			emitter.rol(shifted, value, maskedCount); break;
		case ShiftOp::RotateRight:
			emitter.ror(shifted, value, maskedCount); break;
		}
		values.push_back(shifted);
	}

	asmjit::x86::Compiler& compiler;
	//! The emission seam: universal instructions through `asmjit::ujit`, the
	//! host compiler for the sites `BE-7` has not converted yet. Every raw site
	//! is listed in Asmjit.md "The host-specific set", a list that only shrinks.
	Emitter emitter;
	const WASM::Module& module;
	WASM::ModuleInstanceInternals& internals;
	const std::vector<asmjit::FuncNode*>& functions;
	const std::vector<std::unique_ptr<asmjit::FuncSignature>>& signatures;
	//! Module function index of this body (direct self tail calls, and the
	//! function a trap in this body is attributed to).
	uint32_t functionIndex = 0;
	//! Interned `$name` of this function, or null: a trap annotation embeds the
	//! pointer, so the string must outlive every call into the instance
	//! (docs/DIAGNOSTICS.md DG-2, docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	const char* functionName = nullptr;
	//! Number of Wasm parameters (the first parameterCount locals).
	uint32_t parameterCount = 0;
	//! Loop target of a direct self tail call.
	asmjit::Label bodyStart;
	asmjit::x86::Gp vmContext;
	//! The frame pointer in frame mode (docs/ABI.md).
	asmjit::x86::Gp framePointer;
	//! Whether *this* function is compiled in the staging-frame form.
	const bool frameMode = false;
	//! Call form per module-local type index: a call site emits the *callee
	//! type's* form, so one wide function no longer decides every call.
	std::span<const std::uint8_t> frameByType;
	//! True when the function type at `typeIndex` uses the staging frame.
	bool calleeUsesFrameForm(std::uint32_t typeIndex) const
	{
		return typeIndex < frameByType.size() && frameByType[typeIndex] != 0;
	}
	//! True when the active profile excludes the `N` marker: a scalar float that
	//! becomes observable is replaced by the canonical NaN
	//! (docs/proposals/PROFILES.md PR-3).
	const bool deterministicFloats = false;
	//! True while `dispatchResume` is driven by `resume_throw*`.
	bool resumeThrowActive_ = false;
	std::vector<asmjit::x86::Gp> locals;
	//! Declared type of every local, for reference barriers.
	std::vector<WASM::ValueTypeCode> localKinds;
	//! Slot width of every local (4/8, or 16 for a v128 buffer).
	std::vector<int> localWidths;
	std::vector<asmjit::x86::Gp> values;

	//! Virtual-register ids of operand-stack values that own one reference of
	//! any kind -- a GC object, a string or a view of one, a continuation handle,
	//! an unboxed i31 (uncounted but still a reference) or a host reference.
	//! The operand stack owns one count per reference it holds
	//! (docs/WRITING_A_BACKEND.md section 6), so a consumer must release it.
	//! AsmJIT ids are unique, so a consumed value's stale entry can never match
	//! a later, different value.
	std::unordered_set<uint32_t> referenceValues;
	//! Virtual-register ids of operand-stack values that own one string-family
	//! reference. AsmJIT ids are unique, so a consumed value's stale entry can
	//! never match a later, different value.
	std::unordered_set<uint32_t> stringValues;
	//! True while `emitDropReferenceLocals` is emitting its sequence, so a
	//! nested pending-trap return does not release the same locals twice.
	bool releasingTrapLocals = false;
	//! True while the current arm can fall through; a branch or return clears it.
	bool armTerminated = false;

	//! True when the linear walk is in polymorphic dead code and it is safe to
	//! skip value-consuming instructions.
	//!
	//! Exception modules opt out: `try_table` catch edges enter code whose
	//! linear predecessor terminated, but the edge itself is live. AsmJIT's
	//! `armTerminated` flag does not yet distinguish that case, so suppressing
	//! lowerings there would drop the catch arm. The price is the old
	//! dead-code-underflow failures in EH modules, not silent wrong code.
	bool inDeadCode() const noexcept
	{
		return armTerminated && module.tags.empty();
	}
	std::vector<ControlFrame> controls;
	uint32_t resultCount = 0;
	WASM::ValueTypeCode resultType = WASM::ValueTypeCode::Void;
	//! Declared result types, in order, for frame-mode returns.
	std::vector<WASM::ValueTypeCode> resultTypes;
	//! Byte offset of the result block in a staging frame.
	uint32_t frameResultOffset = 0;
	//! Stack slot holding the caller-supplied 16-byte buffer of a v128 result.
	asmjit::x86::Mem v128ResultSlot;
	//! Stack slot holding the exception currently being routed.
	asmjit::x86::Mem exceptionPtrSlot;
	//! True when the single result is a v128 returned through that buffer.
	bool bufferedVectorResult = false;
};

} // namespace

struct ModuleCompiler::FunctionRecord {
	const asmjit::FuncSignature* signature = nullptr;
	asmjit::FuncNode* node = nullptr;
	void* entry = nullptr;
};

ModuleCompiler::ModuleCompiler(Context& context)
	: context(context)
{
}

ModuleCompiler::~ModuleCompiler() = default;

void ModuleCompiler::translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module,
		WASM::ModuleInstanceInternals& internals)
{
	// No byte-order refusal any more (`BE-7`, closing `BED4`'s AsmJIT half): every
	// guest access this backend emits goes through `Emitter`'s guest helpers,
	// which convert when the instance's decision says so, and every GC header
	// word is read through the little-endian-on-every-host rule. The decision was
	// reached before this call (`BE-2`), so `compileFunctions` can read it.
	signatures.clear();
	// Staging-frame ABI (docs/ABI.md): each type's form is decided before
	// translation, because the signature itself changes shape. A module may mix
	// the two forms (`GAP-1`, `GR-8`).
	const std::vector<std::uint8_t> forms = TypeTranslator::frameForms(
			module, WASM::frameCallAbiOptIn());
	instance.setFrameCallForms(forms);
	instance.setFrameCallAbi(TypeTranslator::uniformlyFrameForm(module, forms));
	signatures.resize(module.typeIds.size());
	internals.translatedTypes.assign(module.typeIds.size(), nullptr);

	for (uint32_t i = 0; i < module.typeIds.size(); ++i) {
		if (module.typeView(i).kind() != WASM::CanonTypeKind::Func)
			continue;
		const WASM::FuncType type = module.funcTypeAt(i);
		const bool frameForm = i < forms.size() && forms[i] != 0;
		signatures[i] = std::make_unique<asmjit::FuncSignature>(
				TypeTranslator::translateFunctionType(type, frameForm));
		internals.translatedTypes[i] = signatures[i].get();
	}
}

void ModuleCompiler::declareFunctions(WASM::ModuleInstance& instance,
		const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	internals.compiledFunctions.resize(module.internalFunctionTypeIndices.size());
	internals.internalCallables.resize(module.internalFunctionTypeIndices.size());
	functionRecords.clear();
	functionRecords.reserve(module.internalFunctionTypeIndices.size());

	if (!internals.internalCallables.empty())
		instance.store().registerCallableRange(
				internals.internalCallables.data(), internals.internalCallables.size());

	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i) {
		const uint32_t typeIndex = module.internalFunctionTypeIndices[i];
		if (typeIndex >= signatures.size() || !signatures[typeIndex])
			throw std::runtime_error("AsmJit function references a non-function or unavailable type");
		auto record = std::make_unique<FunctionRecord>();
		record->signature = signatures[typeIndex].get();
		internals.compiledFunctions[i] = nullptr;
		internals.internalCallables[i] = WASM::Callable {
			.fnPtr = nullptr,
			.context = instance.context(),
			.localTypeIdx = typeIndex,
			.typeId = module.typeId(WASM::LocalTypeIdx{typeIndex})
		};
		functionRecords.push_back(std::move(record));
	}
}

void ModuleCompiler::compileFunctions(WASM::ModuleInstance& instance,
		const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	// A v128 result travels through the caller-supplied 16-byte buffer: the
	// signature spells it as a trailing pointer and a void return, so the call
	// cache must describe that shape rather than a returned aggregate
	// (docs/ABI.md).
	instance.setV128ResultIsBuffered(true);

	if (module.functionBodies.size() != functionRecords.size())
		throw std::runtime_error("AsmJit function declaration/body count mismatch");
	if (module.functionBodies.empty()) {
		// An empty module (memory/table/global declarations only) generates no
		// code. `JitRuntime::add` rejects a zero-sized code holder with
		// NoCodeGenerated, so do not create or install one.
		functionRecords.clear();
		return;
	}

	// The per-type call form, read back from the instance so the emitter and the
	// tail-call installer use exactly the table `translateTypes` recorded.
	std::vector<std::uint8_t> forms(module.typeCount(), 0);
	for (uint32_t i = 0; i < module.typeCount(); ++i)
		forms[i] = instance.usesFrameForm(WASM::LocalTypeIdx{i}) ? 1 : 0;

	// The byte-order decision, reached once per module before any body was
	// compiled (docs/proposals/BIG_ENDIAN.md `BE-2`): every guest access this
	// module's code emits converts when it says so, and the code is otherwise
	// byte-identical to what a little-endian build always emitted (`BED5`).
	const bool swapsGuestAccesses = instance.byteOrderDecision().swapsAccesses;

	const std::size_t allocationCheckpoint = context.allocationCheckpoint();
	try {
		asmjit::CodeHolder code;
		checkError(code.init(context.environment(), context.cpuFeatures()),
				"AsmJit CodeHolder initialisation failed");
		asmjit::x86::Compiler compiler(&code);
		// One Universal JIT layer for the whole code holder: `add_func`/`end_func`
		// hook and unhook it per body, which is what makes `ujit` emission legal
		// inside that body (docs/proposals/BIG_ENDIAN.md `BE-7`).
		asmjit::ujit::UniCompiler universal(&compiler, context.cpuFeatures(),
				asmjit::CpuHints{});

		// Create every function node before emitting any body. Labels therefore
		// exist for forward calls and mutual recursion as well as backward calls.
		std::vector<asmjit::FuncNode*> functions;
		functions.reserve(functionRecords.size());
		for (auto& record : functionRecords) {
			record->node = compiler.new_func(*record->signature);
			if (record->node == nullptr)
				throw std::runtime_error("AsmJit failed to declare an internal function node");
			functions.push_back(record->node);
		}

		for (uint32_t i = 0; i < module.functionBodies.size(); ++i) {
			auto* function = universal.add_func(functions[i]);
			if (function == nullptr)
				throw std::runtime_error("AsmJit failed to begin an internal function body");
			const uint32_t typeIndex = module.internalFunctionTypeIndices[i];
			const WASM::FuncType type = module.funcTypeAt(typeIndex);
			const bool ownFrameForm = instance.usesFrameForm(WASM::LocalTypeIdx{typeIndex});
			IntegerFunctionEmitter emitter(compiler, universal, *function, type,
					module.functionBodies[i],
					module, internals, functions, signatures,
					static_cast<uint32_t>(module.importFunctions.size() + i),
					ownFrameForm, forms, profile().deterministic(),
					swapsGuestAccesses);
			try {
				emitter.compile(module.functionBodies[i], type);
			} catch (const std::exception& error) {
				throw std::runtime_error("AsmJit function " + std::to_string(i) + ": " + error.what());
			}
			// Through the universal layer: it unhooks the body before ending it.
			checkError(universal.end_func(), "AsmJit function finalisation failed");
		}

		checkError(compiler.finalize(), "AsmJit compiler finalisation failed");
		const std::size_t codeBytes = code.code_size();
		void* codeBase = context.install(code);
		moduleBases.emplace_back(&instance, codeBase);
		std::vector<uint64_t> offsets(functionRecords.size(), 0);
		for (uint32_t i = 0; i < functionRecords.size(); ++i) {
			const uint64_t offset = code.label_offset_from_base(functions[i]->label());
			offsets[i] = offset;
			functionRecords[i]->entry = static_cast<uint8_t*>(codeBase) + offset;
			internals.internalCallables[i].rawFnPtr = functionRecords[i]->entry;
			internals.internalCallables[i].fnPtr = functionRecords[i]->entry;
			internals.compiledFunctions[i] = functionRecords[i]->entry;
		}
		// `BED5` diagnostics: the first body's entry and its own size — measured
		// from its label to the next function's, because `code_size()` describes
		// the whole holder and does not follow the layout changes a converted
		// access makes. A test can therefore tell a converting compilation from a
		// native one without reading the emitter.
		if (!functionRecords.empty()) {
			const uint64_t begin = offsets.front();
			const uint64_t end = offsets.size() > 1 ? offsets[1] : codeBytes;
			firstCodeEntry_ = functionRecords.front()->entry;
			firstCodeBytes_ = static_cast<std::size_t>(end > begin ? end - begin : 0);
		}
		if (WASM::tailCallTrampolinesEligible(module, forms)
				&& !WASM::installTailCallTrampolines(module, internals, forms))
			throw std::runtime_error("AsmJit could not install tail-call trampolines");
		// FuncNodes belong to the temporary CodeHolder. Runtime handles are the
		// installed entry pointers above, so discard all compiler-side records.
		functionRecords.clear();
	} catch (...) {
		// Instantiation is transactional with respect to executable memory:
		// a failed module must not pin partially generated code in the session.
		context.releaseAllocationsSince(allocationCheckpoint);
		for (uint32_t i = 0; i < functionRecords.size(); ++i) {
			functionRecords[i]->entry = nullptr;
			internals.internalCallables[i].rawFnPtr = nullptr;
			internals.internalCallables[i].fnPtr = nullptr;
			internals.compiledFunctions[i] = nullptr;
		}
		functionRecords.clear();
		throw;
	}
}

void ModuleCompiler::releaseInstance(WASM::ModuleInstance& instance) noexcept
{
	WASM::ModuleInstanceInternals& internals = instanceInternals(instance);
	WASM::releaseTailCallTrampolines(internals);
	for (WASM::Callable& callable : internals.internalCallables) {
		callable.fnPtr = nullptr;
		callable.rawFnPtr = nullptr;
		callable.tailEntry = nullptr;
	}
	for (auto it = moduleBases.begin(); it != moduleBases.end(); ++it) {
		if (it->first == &instance) {
			context.release(it->second);
			moduleBases.erase(it);
			break;
		}
	}
}

void ModuleCompiler::callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx,
		WASM::ModuleInstanceInternals& internals)
{
	WASM::Callable* callable = nullptr;
	const uint32_t importedCount = instance.context()->importedFunctionCount;
	if (funcIdx < importedCount) {
		if (funcIdx >= internals.importStorage.size())
			throw std::runtime_error("AsmJit start function index is outside the function index space");
		callable = &internals.importStorage[funcIdx];
	} else {
		const uint32_t localIndex = funcIdx - importedCount;
		if (localIndex >= internals.internalCallables.size())
			throw std::runtime_error("AsmJit start function index is outside the function index space");
		callable = &internals.internalCallables[localIndex];
	}
	if (callable->fnPtr == nullptr)
		throw std::runtime_error("AsmJit start function has no compiled entry point");

	using StartFunction = void (*)(WASM::VMContext*);
	{
		WASM::CallDepthScope depth;
		reinterpret_cast<StartFunction>(callable->fnPtr)(callable->context);
	}
	WASM::checkTrap();
}

} // namespace AsmJitBackend
