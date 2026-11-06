#include "AsmjitModuleCompiler.hpp"

#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmSafety.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"
#include "../WasmBase/WasmType.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
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
extern "C" float asmjitF32Ceil(float x) noexcept { return std::ceil(x); }
extern "C" float asmjitF32Floor(float x) noexcept { return std::floor(x); }

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
extern "C" double asmjitF64Ceil(double x) noexcept { return std::ceil(x); }
extern "C" double asmjitF64Floor(double x) noexcept { return std::floor(x); }

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
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
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

void* asmjitArrayNew(WASM::VMContext* vm, uint32_t typeIndex, uint32_t length) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateArrayObject(typeIndex, length);
}

void* asmjitStructNew(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex) noexcept
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
}

void asmjitReportNullReference() noexcept
{
	WASM::reportTrap(WASM::TrapKind::NullReference);
}

void asmjitReportUnreachable() noexcept
{
	WASM::reportTrap(WASM::TrapKind::Unreachable);
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

uint32_t asmjitTableSize(WASM::VMContext* vm, uint32_t tableIndex) noexcept
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
    return static_cast<uint32_t>(table->size);
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
public:
	IntegerFunctionEmitter(asmjit::x86::Compiler& compiler, asmjit::FuncNode& function,
			const WASM::FuncType& type, const WASM::FunctionBody& body,
			const WASM::Module& module, WASM::ModuleInstanceInternals& internals,
			const std::vector<asmjit::FuncNode*>& functions,
			const std::vector<std::unique_ptr<asmjit::FuncSignature>>& signatures)
		: compiler(compiler), module(module), internals(internals),
		  functions(functions), signatures(signatures)
	{
		WASM::PreparedFunctionStack stack;
		stack.prepare(type, body);

		if (type.results.size() > 1)
			throw std::runtime_error("AsmJit initial compiler does not support multi-value results");
		for (const auto& parameter : type.params)
			requireScalar(parameter.val, "parameters");
		for (const auto& result : type.results)
			requireScalar(result.val, "results");
		for (const auto& local : stack.allLocals) {
			if (local.opcode != WASM::ValueTypeCode::V128)
				requireScalar(local, "locals");
		}
		resultType = type.results.empty()
				? WASM::ValueTypeCode::Void : type.results.front().val.opcode;

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
		checkError(compiler.mov(pending, asmjit::x86::dword_ptr(trapSlot,
				static_cast<int32_t>(offsetof(WASM::TrapSlot, pending)))),
				"AsmJit failed to load pending-trap state");
		asmjit::Label bodyLabel = compiler.new_label();
		checkError(compiler.test(pending, pending), "AsmJit pending-trap test emission failed");
		checkError(compiler.jz(bodyLabel), "AsmJit pending-trap branch emission failed");
		if (type.results.empty()) {
			checkError(compiler.ret(), "AsmJit trap-path void return emission failed");
		} else if (isFloatKind(resultType)) {
			auto zero = compiler.new_xmm("trapFloatResult");
			checkError(compiler.xorps(zero, zero), "AsmJit trap float result zeroing failed");
			checkError(compiler.ret(zero), "AsmJit trap-path float return emission failed");
		} else {
			asmjit::x86::Gp zero = kindWidth(resultType) == 8
					? compiler.new_gp64("trapResult") : compiler.new_gp32("trapResult");
			checkError(compiler.xor_(zero, zero), "AsmJit trap-result zeroing failed");
			checkError(compiler.ret(zero), "AsmJit trap-path result return emission failed");
		}
		checkError(compiler.bind(bodyLabel), "AsmJit body label binding failed");

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
					checkError(compiler.movd(bits, parameter),
							"AsmJit f32 parameter copy failed");
				else
					checkError(compiler.movq(bits, parameter),
							"AsmJit f64 parameter copy failed");
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
				checkError(compiler.xor_(local, local), "AsmJit local zero-initialisation failed");
				locals.push_back(local);
			}
		}
		localWidths.reserve(stack.allLocals.size());
		for (const auto& local : stack.allLocals)
			localWidths.push_back(slotWidthOf(local.opcode));
		resultCount = static_cast<uint32_t>(type.results.size());
	}

	void compile(const WASM::FunctionBody& body, const WASM::FuncType& type)
	{
		const std::span<const std::byte> code(
				reinterpret_cast<const std::byte*>(body.code.data()), body.code.size());
		Euph::Io::ConstBufferDevice device(code);
		WASM::WasmStream<Euph::Io::ConstBufferDevice> stream(device);
		readCode(stream);

		if (resultCount == 0) {
			if (!values.empty())
				throw std::runtime_error("AsmJit function ended with an unexpected operand stack value");
			checkError(compiler.ret(), "AsmJit void function return emission failed");
		} else {
			if (values.size() != 1)
				throw std::runtime_error("AsmJit function must end with exactly one result");
			if (isFloatKind(resultType))
				checkError(compiler.ret(
						materializeFloat(values.back(), kindWidth(resultType))),
						"AsmJit float function return emission failed");
			else
				checkError(compiler.ret(values.back()),
						"AsmJit function return emission failed");
		}
		(void)type;
	}

protected:
	void dispatchI32Const(int32_t value) override
	{
		asmjit::x86::Gp reg = compiler.new_gp32("constant");
		checkError(compiler.mov(reg, value), "AsmJit i32.const emission failed");
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
		checkError(compiler.mov(copy, locals[i]), "AsmJit local.get emission failed");
		values.push_back(copy);
	}

	void dispatchLocalSet(WASM::LocalIdx index) override
	{
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size() || values.empty())
			throw std::runtime_error("AsmJit local.set stack/index underflow");
		const asmjit::x86::Gp value = values.back();
		values.pop_back();
		if (localWidths[i] == 16) {
			copyVector(locals[i], value);
			return;
		}
		checkError(compiler.mov(locals[i], value), "AsmJit local.set emission failed");
	}

	void dispatchLocalTee(WASM::LocalIdx index) override
	{
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size() || values.empty())
			throw std::runtime_error("AsmJit local.tee stack/index underflow");
		if (localWidths[i] == 16) {
			copyVector(locals[i], values.back());
			return;
		}
		asmjit::x86::Gp copy = locals[i].size() == 8
				? compiler.new_gp64("teeCopy") : compiler.new_gp32("teeCopy");
		checkError(compiler.mov(copy, values.back()), "AsmJit local.tee emission failed");
		checkError(compiler.mov(locals[i], copy), "AsmJit local.tee store emission failed");
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

	void dispatchDrop() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit drop operand stack underflow");
		values.pop_back();
	}

	void dispatchBlock(const WASM::BlockType& type) override
	{
		pushControlFrame(ControlKind::Block, type);
	}

	void dispatchLoop(const WASM::BlockType& type) override
	{
		pushControlFrame(ControlKind::Loop, type);
		checkError(compiler.bind(controls.back().begin), "AsmJit loop label binding failed");
	}

	void dispatchIf(const WASM::BlockType& type) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit if condition stack underflow");
		auto condition = values.back();
		values.pop_back();
		pushControlFrame(ControlKind::If, type);
		auto& frame = controls.back();
		frame.alternate = compiler.new_label();
		checkError(compiler.test(condition, condition), "AsmJit if condition test failed");
		checkError(compiler.jz(frame.alternate), "AsmJit if condition branch failed");
	}

	void dispatchElse() override
	{
		if (controls.empty() || controls.back().kind != ControlKind::If || controls.back().hasElse)
			throw std::runtime_error("AsmJit encountered an unmatched or duplicate else");
		ControlFrame& frame = controls.back();
		storeBlockResult(frame, "if then");
		checkError(compiler.jmp(frame.end), "AsmJit if/else end jump failed");
		checkError(compiler.bind(frame.alternate), "AsmJit else label binding failed");
		values.resize(frame.stackHeight);
		frame.hasElse = true;
	}

	void dispatchEnd() override
	{
		if (controls.empty())
			return;
		ControlFrame frame = controls.back();
		controls.pop_back();
		if (frame.kind == ControlKind::If && !frame.hasElse) {
			if (frame.resultCount != 0)
				throw std::runtime_error("AsmJit if-without-else cannot produce a result");
			if (values.size() != frame.stackHeight)
				throw std::runtime_error("AsmJit if arm leaves an unexpected operand stack shape");
			checkError(compiler.bind(frame.alternate), "AsmJit implicit else label binding failed");
		} else {
			storeBlockResult(frame, "structured block");
		}
		checkError(compiler.bind(frame.end), "AsmJit structured block end binding failed");
		values.resize(frame.stackHeight);
		if (frame.resultCount != 0)
			values.push_back(frame.result);
	}

	void dispatchBrIf(WASM::LabelIdx depth) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit br_if condition stack underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_if label depth is out of range");
		const auto condition = values.back();
		values.pop_back();
		if (depth == controls.size()) {
			if (values.size() != resultCount)
				throw std::runtime_error("AsmJit conditional function branch has the wrong result arity");
			auto skip = compiler.new_label();
			checkError(compiler.test(condition, condition), "AsmJit conditional function branch test failed");
			checkError(compiler.jz(skip), "AsmJit conditional function branch skip failed");
			if (resultCount == 0)
				checkError(compiler.ret(), "AsmJit conditional function branch return failed");
			else
				emitFunctionReturn(values.back());
			checkError(compiler.bind(skip), "AsmJit conditional function branch label binding failed");
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const size_t branchValues = target.kind == ControlKind::Loop ? 0 : target.resultCount;
		if (values.size() != target.stackHeight + branchValues)
			throw std::runtime_error("AsmJit br_if operand stack does not match target label signature");
		const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
		if (branchValues != 0) {
			auto skip = compiler.new_label();
			checkError(compiler.test(condition, condition), "AsmJit br_if condition test failed");
			checkError(compiler.jz(skip), "AsmJit br_if conditional skip emission failed");
			checkError(compiler.mov(target.result, values.back()), "AsmJit br_if result transfer failed");
			checkError(compiler.jmp(label), "AsmJit br_if branch emission failed");
			checkError(compiler.bind(skip), "AsmJit br_if skip label binding failed");
		} else {
			checkError(compiler.test(condition, condition), "AsmJit br_if condition test failed");
			checkError(compiler.jnz(label), "AsmJit br_if branch emission failed");
		}
	}

	void dispatchBr(WASM::LabelIdx depth) override
	{
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br label depth is out of range");
		if (depth == controls.size()) {
			if (values.size() != resultCount)
				throw std::runtime_error("AsmJit function branch has the wrong result arity");
			if (resultCount == 0)
				checkError(compiler.ret(), "AsmJit function branch return failed");
			else
				emitFunctionReturn(values.back());
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const size_t branchValues = target.kind == ControlKind::Loop ? 0 : target.resultCount;
		if (values.size() != target.stackHeight + branchValues)
			throw std::runtime_error("AsmJit br operand stack does not match target label signature");
		if (branchValues != 0)
			checkError(compiler.mov(target.result, values.back()), "AsmJit br result transfer failed");
		const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
		checkError(compiler.jmp(label), "AsmJit br emission failed");
	}
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& depths, WASM::LabelIdx defaultDepth) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit br_table selector stack underflow");
		const auto selector = values.back();
		values.pop_back();
		size_t expectedStackHeight = 0;
		size_t expectedBranchValues = 0;
		WASM::ValueTypeCode expectedBranchType = WASM::ValueTypeCode::Void;
		bool first = true;
		auto validateTarget = [&](WASM::LabelIdx depth) {
			if (depth > controls.size())
				throw std::runtime_error("AsmJit br_table label depth is out of range");
			size_t stackHeight = 0;
			size_t branchValues = resultCount;
			WASM::ValueTypeCode branchType = resultType;
			if (depth < controls.size()) {
				const ControlFrame& target = controls[controls.size() - 1 - depth];
				stackHeight = target.stackHeight;
				branchValues = target.kind == ControlKind::Loop ? 0 : target.resultCount;
				branchType = target.kind == ControlKind::Loop
						? WASM::ValueTypeCode::Void : target.resultType;
			}
			if (first) {
				expectedStackHeight = stackHeight;
				expectedBranchValues = branchValues;
				expectedBranchType = branchType;
				first = false;
			} else if (stackHeight != expectedStackHeight || branchValues != expectedBranchValues
					|| branchType != expectedBranchType) {
				throw std::runtime_error("AsmJit br_table targets require identical stack heights and label signatures");
			}
		};
		for (WASM::LabelIdx depth : depths)
			validateTarget(depth);
		validateTarget(defaultDepth);
		if (values.size() != expectedStackHeight + expectedBranchValues)
			throw std::runtime_error("AsmJit br_table operand stack does not match its target labels");
		auto emitTarget = [&](WASM::LabelIdx depth) {
			if (depth == controls.size()) {
				if (resultCount == 0)
					checkError(compiler.ret(), "AsmJit br_table function return failed");
				else
					emitFunctionReturn(values.back());
				return;
			}
			ControlFrame& target = controls[controls.size() - 1 - depth];
			if (target.kind != ControlKind::Loop && target.resultCount != 0)
				checkError(compiler.mov(target.result, values.back()), "AsmJit br_table result transfer failed");
			const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
			checkError(compiler.jmp(label), "AsmJit br_table branch emission failed");
		};
		for (size_t i = 0; i < depths.size(); ++i) {
			auto matched = compiler.new_label();
			auto next = compiler.new_label();
			checkError(compiler.cmp(selector, static_cast<uint32_t>(i)), "AsmJit br_table selector comparison failed");
			checkError(compiler.jz(matched), "AsmJit br_table match branch failed");
			checkError(compiler.jmp(next), "AsmJit br_table next-case branch failed");
			checkError(compiler.bind(matched), "AsmJit br_table match label binding failed");
			emitTarget(depths[i]);
			checkError(compiler.bind(next), "AsmJit br_table next-case label binding failed");
		}
		emitTarget(defaultDepth);
	}

	void dispatchReturnCall(WASM::FuncIdx index) override
	{
		const size_t importedCount = module.importFunctions.size();
		const size_t totalCount = importedCount + module.internalFunctionTypeIndices.size();
		if (index >= totalCount)
			throw std::runtime_error("AsmJit return_call function index is out of range");
		const uint32_t typeIndex = index < importedCount
				? module.importFunctions[index].typeIdx
				: module.internalFunctionTypeIndices[index - importedCount];
		validateTailCallType(typeIndex);
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (values.size() != calledType.params.size())
			throw std::runtime_error("AsmJit return_call has unexpected values below its arguments");
		dispatchCall(index);
		dispatchReturn();
	}

	void dispatchReturnCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex) override
	{
		validateTailCallType(typeIndex);
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (values.size() != calledType.params.size() + 1)
			throw std::runtime_error("AsmJit return_call_indirect has unexpected values below its arguments");
		dispatchCallIndirect(typeIndex, tableIndex);
		dispatchReturn();
	}

	void validateTailCallType(uint32_t typeIndex) const
	{
		if (typeIndex >= signatures.size() || !signatures[typeIndex])
			throw std::runtime_error("AsmJit tail call references an unavailable function signature");
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (calledType.results.size() != resultCount)
			throw std::runtime_error("AsmJit tail-call result arity differs from the caller");
		for (const auto& parameter : calledType.params)
			requireI32(parameter.val, "tail-call parameters");
		for (const auto& result : calledType.results)
			requireI32(result.val, "tail-call results");
	}

	void dispatchCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex) override
	{
		if (tableIndex >= module.importTables.size() + module.tables.size())
			throw std::runtime_error("AsmJit call_indirect references an unknown table");
		if (typeIndex >= signatures.size() || !signatures[typeIndex])
			throw std::runtime_error("AsmJit call_indirect references an unavailable function type");
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (calledType.results.size() > 1)
			throw std::runtime_error("AsmJit call_indirect currently supports at most one result");
		for (const auto& parameter : calledType.params)
			requireScalar(parameter.val, "indirect-call parameters");
		for (const auto& result : calledType.results)
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
		checkError(compiler.mov(table, static_cast<uint32_t>(tableIndex)), "AsmJit indirect table index materialisation failed");
		auto expected = compiler.new_gp32("indirectExpectedType");
		checkError(compiler.mov(expected, module.typeId(WASM::LocalTypeIdx{typeIndex}).value),
				"AsmJit indirect-call type id materialisation failed");
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

		auto target = compiler.new_gp_ptr("indirectFunction");
		checkError(compiler.mov(target, asmjit::x86::ptr(callable,
				static_cast<int32_t>(offsetof(WASM::Callable, fnPtr)))),
				"AsmJit indirect function pointer load failed");
		auto calleeContext = compiler.new_gp_ptr("indirectContext");
		checkError(compiler.mov(calleeContext, asmjit::x86::ptr(callable,
				static_cast<int32_t>(offsetof(WASM::Callable, context)))),
				"AsmJit indirect VMContext load failed");
		auto callerContext = compiler.new_label();
		checkError(compiler.test(calleeContext, calleeContext), "AsmJit indirect context test failed");
		checkError(compiler.jnz(callerContext), "AsmJit indirect context branch failed");
		checkError(compiler.mov(calleeContext, vmContext), "AsmJit native indirect context selection failed");
		checkError(compiler.bind(callerContext), "AsmJit indirect context label binding failed");

		auto preparedArguments = prepareCallArguments(calledType, arguments);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]),
				"AsmJit indirect function invocation emission failed");
		invoke->set_arg(0, calleeContext);
		setCallArguments(invoke, preparedArguments);
		emitCallResult(invoke, calledType, "indirectCallResult");
		emitPendingTrapReturn();
	}

	void dispatchCall(WASM::FuncIdx index) override
	{
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
		const WASM::FuncType calledType = module.funcTypeAt(typeIndex);
		if (calledType.results.size() > 1)
			throw std::runtime_error("AsmJit calls currently support at most one result");
		for (const auto& parameter : calledType.params)
			requireScalar(parameter.val, "call parameters");
		for (const auto& result : calledType.results)
			requireScalar(result.val, "call results");
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
			checkError(compiler.mov(importedContext,
					reinterpret_cast<uint64_t>(importedCallee)),
					"AsmJit imported VMContext materialisation failed");
		}

		auto preparedArguments = prepareCallArguments(calledType, arguments);
		asmjit::InvokeNode* invoke = nullptr;
		const asmjit::Error error = isInternal
				? compiler.invoke(asmjit::Out(invoke), targetLabel, *signatures[typeIndex])
				: compiler.invoke(asmjit::Out(invoke), target, *signatures[typeIndex]);
		checkError(error, "AsmJit function-call emission failed");
		if (importedCallee == nullptr)
			invoke->set_arg(0, vmContext);
		else
			invoke->set_arg(0, importedContext);
		setCallArguments(invoke, preparedArguments);
		emitCallResult(invoke, calledType, "callResult");
		emitPendingTrapReturn();
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
		checkError(compiler.mov(pending, asmjit::x86::dword_ptr(slot,
				static_cast<int32_t>(offsetof(WASM::TrapSlot, pending)))),
				"AsmJit failed to load post-call pending-trap state");
		auto noTrap = compiler.new_label();
		checkError(compiler.test(pending, pending), "AsmJit post-call trap test failed");
		checkError(compiler.jz(noTrap), "AsmJit post-call trap branch failed");
		if (resultCount == 0) {
			checkError(compiler.ret(), "AsmJit post-call void trap return failed");
		} else if (isFloatKind(resultType)) {
			auto zero = compiler.new_xmm("postCallTrapFloatResult");
			checkError(compiler.xorps(zero, zero),
					"AsmJit post-call trap float result initialisation failed");
			checkError(compiler.ret(zero),
					"AsmJit post-call trap float result return failed");
		} else {
			auto zero = kindWidth(resultType) == 8
					? compiler.new_gp64("postCallTrapResult")
					: compiler.new_gp32("postCallTrapResult");
			checkError(compiler.xor_(zero, zero), "AsmJit post-call trap result initialisation failed");
			checkError(compiler.ret(zero), "AsmJit post-call trap result return failed");
		}
		checkError(compiler.bind(noTrap), "AsmJit post-call continuation binding failed");
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
        checkError(compiler.mov(index, dataIndex), "AsmJit data.drop index materialisation failed");
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
        checkError(compiler.mov(element, elementIndex), "AsmJit table.init element index failed");
        checkError(compiler.mov(table, static_cast<uint32_t>(tableIndex)), "AsmJit table.init table index failed");
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
        checkError(compiler.mov(element, elementIndex), "AsmJit elem.drop index failed");
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitElemDrop), signature),
                "AsmJit elem.drop helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, element);
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
        checkError(compiler.mov(dstTable, static_cast<uint32_t>(destinationTable)), "AsmJit table.copy destination table index failed");
        checkError(compiler.mov(srcTable, static_cast<uint32_t>(sourceTable)), "AsmJit table.copy source table index failed");
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

    void dispatchTableSize(WASM::TableIdx tableIndex) override
    {
        if (static_cast<size_t>(tableIndex) >= module.importTables.size() + module.tables.size())
            throw std::runtime_error("AsmJit table.size table index is out of range");
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kUInt32);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto index = compiler.new_gp32("tableSizeIndex");
        checkError(compiler.mov(index, static_cast<uint32_t>(tableIndex)), "AsmJit table.size index materialisation failed");
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitTableSize), signature),
                "AsmJit table.size helper call emission failed");
        invoke->set_arg(0, vmContext);
        invoke->set_arg(1, index);
        auto size = compiler.new_gp32("tableSizeResult");
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
		checkError(compiler.mov(low, lo), "AsmJit v128.const low materialisation failed");
		checkError(compiler.mov(asmjit::x86::qword_ptr(buffer, 0), low),
				"AsmJit v128.const low word failed");
		auto high = compiler.new_gp64("v128ConstHigh");
		checkError(compiler.mov(high, hi), "AsmJit v128.const high materialisation failed");
		checkError(compiler.mov(asmjit::x86::qword_ptr(buffer, 8), high),
				"AsmJit v128.const high word failed");
		values.push_back(buffer);
	}

	void dispatchV128Load(WASM::MemArg arg) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit v128.load address underflow");
		auto address = values.back();
		values.pop_back();
		auto pointer = emitMemoryAddress(arg, 16, address);
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
		auto pointer = emitMemoryAddress(arg, 16, address);
		copyVector(pointer, value);
	}

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
	void dispatchV128And() override { emitVectorHelper2(&wasm_v128_and); }
	void dispatchV128AndNot() override { emitVectorHelper2(&wasm_v128_andnot); }
	void dispatchV128Or() override { emitVectorHelper2(&wasm_v128_or); }
	void dispatchV128Xor() override { emitVectorHelper2(&wasm_v128_xor); }

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
		checkError(compiler.mov(low, asmjit::x86::qword_ptr(vector, 0)),
				"AsmJit v128.any_true low word failed");
		checkError(compiler.mov(high, asmjit::x86::qword_ptr(vector, 8)),
				"AsmJit v128.any_true high word failed");
		checkError(compiler.or_(low, high), "AsmJit v128.any_true merge failed");
		auto result = compiler.new_gp32("v128AnyResult");
		checkError(compiler.xor_(result, result),
				"AsmJit v128.any_true result zeroing failed");
		checkError(compiler.test(low, low), "AsmJit v128.any_true test failed");
		checkError(compiler.set(asmjit::x86::CondCode::kNE, result.r8()),
				"AsmJit v128.any_true set failed");
		values.push_back(result);
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

	void dispatchI8x16Shl() override { emitVectorShift(&wasm_i8x16_shl); }
	void dispatchI8x16ShrS() override { emitVectorShift(&wasm_i8x16_shr_s); }
	void dispatchI8x16ShrU() override { emitVectorShift(&wasm_i8x16_shr_u); }
	void dispatchI16x8Shl() override { emitVectorShift(&wasm_i16x8_shl); }
	void dispatchI16x8ShrS() override { emitVectorShift(&wasm_i16x8_shr_s); }
	void dispatchI16x8ShrU() override { emitVectorShift(&wasm_i16x8_shr_u); }
	void dispatchI32x4Shl() override { emitVectorShift(&wasm_i32x4_shl); }
	void dispatchI32x4ShrS() override { emitVectorShift(&wasm_i32x4_shr_s); }
	void dispatchI32x4ShrU() override { emitVectorShift(&wasm_i32x4_shr_u); }
	void dispatchI64x2Shl() override { emitVectorShift(&wasm_i64x2_shl); }
	void dispatchI64x2ShrS() override { emitVectorShift(&wasm_i64x2_shr_s); }
	void dispatchI64x2ShrU() override { emitVectorShift(&wasm_i64x2_shr_u); }

	void dispatchI8x16MinS() override { emitVectorHelper2(&wasm_i8x16_min_s); }
	void dispatchI8x16MinU() override { emitVectorHelper2(&wasm_i8x16_min_u); }
	void dispatchI8x16MaxS() override { emitVectorHelper2(&wasm_i8x16_max_s); }
	void dispatchI8x16MaxU() override { emitVectorHelper2(&wasm_i8x16_max_u); }
	void dispatchI16x8MinS() override { emitVectorHelper2(&wasm_i16x8_min_s); }
	void dispatchI16x8MinU() override { emitVectorHelper2(&wasm_i16x8_min_u); }
	void dispatchI16x8MaxS() override { emitVectorHelper2(&wasm_i16x8_max_s); }
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
	void dispatchI8x16AvgrU() override { emitVectorHelper2(&wasm_i8x16_avgr_u); }
	void dispatchI16x8AvgrU() override { emitVectorHelper2(&wasm_i16x8_avgr_u); }

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

	void dispatchRefNull(const WASM::HeapType&) override
	{
		// A reference is one null word: the backend does not model typed nulls.
		auto reference = compiler.new_gp64("nullRef");
		checkError(compiler.xor_(reference, reference), "AsmJit ref.null zeroing failed");
		values.push_back(reference);
	}

	void dispatchRefIsNull() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit ref.is_null operand underflow");
		auto reference = values.back();
		values.pop_back();
		// The result is zeroed before the flags are set (the P3 lesson).
		auto result = compiler.new_gp32("refIsNull");
		checkError(compiler.xor_(result, result), "AsmJit ref.is_null zeroing failed");
		checkError(compiler.test(reference, reference), "AsmJit ref.is_null test failed");
		checkError(compiler.set(asmjit::x86::CondCode::kE, result.r8()),
				"AsmJit ref.is_null set failed");
		values.push_back(result);
	}

	void dispatchBrOnNull(WASM::LabelIdx depth) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit br_on_null operand underflow");
		if (depth > controls.size())
			throw std::runtime_error("AsmJit br_on_null label depth is out of range");
		auto reference = values.back();
		if (depth == controls.size()) {
			if (resultCount != 0)
				throw std::runtime_error("AsmJit br_on_null to the function label requires a void result");
			auto skip = compiler.new_label();
			checkError(compiler.test(reference, reference), "AsmJit br_on_null test failed");
			checkError(compiler.jnz(skip), "AsmJit br_on_null skip failed");
			checkError(compiler.ret(), "AsmJit br_on_null return failed");
			checkError(compiler.bind(skip), "AsmJit br_on_null label binding failed");
			return;
		}
		ControlFrame& target = controls[controls.size() - 1 - depth];
		const size_t branchValues = target.kind == ControlKind::Loop ? 0 : target.resultCount;
		if (branchValues != 0)
			throw std::runtime_error("AsmJit br_on_null requires a label without results");
		if (values.size() != target.stackHeight + 1)
			throw std::runtime_error("AsmJit br_on_null operand stack does not match the target label");
		const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
		checkError(compiler.test(reference, reference), "AsmJit br_on_null test failed");
		checkError(compiler.jz(label), "AsmJit br_on_null branch failed");
	}

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
			if (fields[i].storage.isPacked)
				throw std::runtime_error("AsmJit struct.new_default does not lower packed fields yet");
			const int width = kindWidth(fields[i].storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new_default does not lower this field type yet");
			const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, i));
			if (width == 4)
				checkError(compiler.mov(asmjit::x86::dword_ptr(object, offset),
						static_cast<uint32_t>(0)), "AsmJit struct.new_default zeroing failed");
			else
				checkError(compiler.mov(asmjit::x86::qword_ptr(object, offset),
						static_cast<uint64_t>(0)), "AsmJit struct.new_default zeroing failed");
		}
		values.push_back(object);
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
					checkError(compiler.mov(asmjit::x86::byte_ptr(object,
							static_cast<int32_t>(offset)), operands[i].r8()),
							"AsmJit struct.new packed field store failed");
				else
					checkError(compiler.mov(asmjit::x86::word_ptr(object,
							static_cast<int32_t>(offset)), operands[i].r16()),
							"AsmJit struct.new packed field store failed");
				continue;
			}
			const int width = kindWidth(field.storage.value.opcode);
			if (width != 4 && width != 8)
				throw std::runtime_error("AsmJit struct.new does not lower this field type yet");
			if (width == 4)
				checkError(compiler.mov(asmjit::x86::dword_ptr(object,
						static_cast<int32_t>(offset)), operands[i]),
						"AsmJit struct.new field store failed");
			else
				checkError(compiler.mov(asmjit::x86::qword_ptr(object,
						static_cast<int32_t>(offset)), operands[i]),
						"AsmJit struct.new field store failed");
		}
		values.push_back(object);
	}

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
		checkError(compiler.test(object, object), "AsmJit array.get_s/u null test failed");
		checkError(compiler.jnz(notNull), "AsmJit array.get_s/u null branch failed");
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.get_s/u null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.get_s/u null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		checkError(compiler.mov(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset))),
				"AsmJit array.get_s/u length load failed");
		asmjit::Label inBounds = compiler.new_label();
		checkError(compiler.cmp(index, length), "AsmJit array.get_s/u bounds compare failed");
		checkError(compiler.jb(inBounds), "AsmJit array.get_s/u bounds branch failed");
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.get_s/u bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.get_s/u bounds label binding failed");
		auto byteOffset = compiler.new_gp64("arrayByteOffset");
		checkError(compiler.mov(byteOffset.r32(), index), "AsmJit array.get_s/u index widening failed");
		checkError(compiler.shl(byteOffset, isI8 ? 0 : 1),
				"AsmJit array.get_s/u index scaling failed");
		checkError(compiler.add(byteOffset,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage))),
				"AsmJit array.get_s/u data offset failed");
		auto result = compiler.new_gp32("arrayPackedElement");
		if (isI8) {
			if (signExtend)
				checkError(compiler.movsx(result, asmjit::x86::byte_ptr(object, byteOffset)),
						"AsmJit array.get_s byte load failed");
			else
				checkError(compiler.movzx(result, asmjit::x86::byte_ptr(object, byteOffset)),
						"AsmJit array.get_u byte load failed");
		} else {
			if (signExtend)
				checkError(compiler.movsx(result, asmjit::x86::word_ptr(object, byteOffset)),
						"AsmJit array.get_s word load failed");
			else
				checkError(compiler.movzx(result, asmjit::x86::word_ptr(object, byteOffset)),
						"AsmJit array.get_u word load failed");
		}
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
		checkError(compiler.test(object, object), "AsmJit array.set null test failed");
		checkError(compiler.jnz(notNull), "AsmJit array.set null branch failed");
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.set null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.set null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		checkError(compiler.mov(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset))),
				"AsmJit array.set length load failed");
		asmjit::Label inBounds = compiler.new_label();
		checkError(compiler.cmp(index, length), "AsmJit array.set bounds compare failed");
		checkError(compiler.jb(inBounds), "AsmJit array.set bounds branch failed");
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.set bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.set bounds label binding failed");
		// Fold the offset into one register, exactly as the read path does.
		auto address = compiler.new_gp_ptr("arraySetAddress");
		checkError(compiler.mov(address, object), "AsmJit array.set base copy failed");
		auto byteOffset = compiler.new_gp64("arraySetOffset");
		checkError(compiler.mov(byteOffset.r32(), index), "AsmJit array.set index widening failed");
		checkError(compiler.shl(byteOffset, width == 4 ? 2 : 3),
				"AsmJit array.set index scaling failed");
		checkError(compiler.add(address, byteOffset), "AsmJit array.set address add failed");
		checkError(compiler.add(address,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage))),
				"AsmJit array.set data offset failed");
		if (packed) {
			if (packedI8)
				checkError(compiler.mov(asmjit::x86::byte_ptr(address), value.r8()),
						"AsmJit array.set packed store failed");
			else
				checkError(compiler.mov(asmjit::x86::word_ptr(address), value.r16()),
						"AsmJit array.set packed store failed");
		} else if (width == 4)
			checkError(compiler.mov(asmjit::x86::dword_ptr(address), value),
					"AsmJit array.set element store failed");
		else
			checkError(compiler.mov(asmjit::x86::qword_ptr(address), value),
					"AsmJit array.set element store failed");
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
		checkError(compiler.test(object, object), "AsmJit array.get null test failed");
		checkError(compiler.jnz(notNull), "AsmJit array.get null branch failed");
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), voidSignature),
				"AsmJit array.get null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.get null label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		checkError(compiler.mov(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset))),
				"AsmJit array.get length load failed");
		asmjit::Label inBounds = compiler.new_label();
		checkError(compiler.cmp(index, length), "AsmJit array.get bounds compare failed");
		checkError(compiler.jb(inBounds), "AsmJit array.get bounds branch failed");
		asmjit::InvokeNode* oobCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(oobCall),
				reinterpret_cast<uint64_t>(&asmjitReportArrayOob), voidSignature),
				"AsmJit array.get bounds trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(inBounds), "AsmJit array.get bounds label binding failed");
		// Element address = object + dataOffset + index * elementSize, computed in a
		// register so the memory operand stays a simple base+index form.
		auto byteOffset = compiler.new_gp64("arrayByteOffset");
		checkError(compiler.mov(byteOffset.r32(), index), "AsmJit array.get index widening failed");
		checkError(compiler.shl(byteOffset, width == 4 ? 2 : 3),
				"AsmJit array.get index scaling failed");
		checkError(compiler.add(byteOffset,
				static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage))),
				"AsmJit array.get data offset failed");
		auto result = width == 4 ? compiler.new_gp32("arrayElementValue")
				: compiler.new_gp64("arrayElementValue");
		if (width == 4)
			checkError(compiler.mov(result, asmjit::x86::dword_ptr(object, byteOffset)),
					"AsmJit array.get element load failed");
		else
			checkError(compiler.mov(result, asmjit::x86::qword_ptr(object, byteOffset)),
					"AsmJit array.get element load failed");
		values.push_back(result);
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
		checkError(compiler.mov(counter.r32(), length.r32()),
				"AsmJit array.new_default counter load failed");
		checkError(compiler.lea(cursor, asmjit::x86::ptr(object, dataOffset)),
				"AsmJit array.new_default cursor setup failed");
		asmjit::Label loopStart = compiler.new_label();
		asmjit::Label loopDone = compiler.new_label();
		checkError(compiler.bind(loopStart), "AsmJit array.new_default label bind failed");
		checkError(compiler.test(counter, counter), "AsmJit array.new_default loop test failed");
		checkError(compiler.jz(loopDone), "AsmJit array.new_default loop exit failed");
		if (width == 1)
			checkError(compiler.mov(asmjit::x86::byte_ptr(cursor), asmjit::Imm(0)),
					"AsmJit array.new_default zero store failed");
		else if (width == 2)
			checkError(compiler.mov(asmjit::x86::word_ptr(cursor), asmjit::Imm(0)),
					"AsmJit array.new_default zero store failed");
		else if (width == 4)
			checkError(compiler.mov(asmjit::x86::dword_ptr(cursor), asmjit::Imm(0)),
					"AsmJit array.new_default zero store failed");
		else
			checkError(compiler.mov(asmjit::x86::qword_ptr(cursor), asmjit::Imm(0)),
					"AsmJit array.new_default zero store failed");
		checkError(compiler.add(cursor, asmjit::Imm(elementBytes)),
				"AsmJit array.new_default cursor advance failed");
		checkError(compiler.dec(counter), "AsmJit array.new_default counter step failed");
		checkError(compiler.jmp(loopStart), "AsmJit array.new_default back edge failed");
		checkError(compiler.bind(loopDone), "AsmJit array.new_default label bind failed");
		values.pop_back();
		values.push_back(object);
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
					checkError(compiler.mov(asmjit::x86::byte_ptr(object, offset),
							operands[i].r8()), "AsmJit array.new_fixed packed store failed");
				else
					checkError(compiler.mov(asmjit::x86::word_ptr(object, offset),
							operands[i].r16()), "AsmJit array.new_fixed packed store failed");
			} else if (width == 4)
				checkError(compiler.mov(asmjit::x86::dword_ptr(object, offset), operands[i]),
						"AsmJit array.new_fixed element store failed");
			else
				checkError(compiler.mov(asmjit::x86::qword_ptr(object, offset), operands[i]),
						"AsmJit array.new_fixed element store failed");
		}
		values.push_back(object);
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
		const auto offset = static_cast<int32_t>(WASM::gcArrayDataOffset(element.storage));
		if (packed) {
			if (packedI8)
				checkError(compiler.mov(asmjit::x86::byte_ptr(object, offset),
							elementValue.r8()), "AsmJit array.new packed store failed");
			else
				checkError(compiler.mov(asmjit::x86::word_ptr(object, offset),
							elementValue.r16()), "AsmJit array.new packed store failed");
		} else if (width == 4)
			checkError(compiler.mov(asmjit::x86::dword_ptr(object, offset), elementValue),
					"AsmJit array.new element store failed");
		else
			checkError(compiler.mov(asmjit::x86::qword_ptr(object, offset), elementValue),
					"AsmJit array.new element store failed");
		values.push_back(object);
	}

	void dispatchArrayLen() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit array.len operand stack underflow");
		auto object = values.back();
		values.pop_back();
		asmjit::Label notNull = compiler.new_label();
		checkError(compiler.test(object, object), "AsmJit array.len null test failed");
		checkError(compiler.jnz(notNull), "AsmJit array.len null branch failed");
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullArray), nullSignature),
				"AsmJit array.len null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit array.len label binding failed");
		auto length = compiler.new_gp32("arrayLength");
		checkError(compiler.mov(length, asmjit::x86::dword_ptr(object,
				static_cast<int32_t>(WASM::GcHeader::kArrayLengthOffset))),
				"AsmJit array.len load failed");
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
		checkError(compiler.test(object, object), "AsmJit struct.get_s/u null test failed");
		checkError(compiler.jnz(notNull), "AsmJit struct.get_s/u null branch failed");
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
				checkError(compiler.movsx(result, asmjit::x86::byte_ptr(object, offset)),
						"AsmJit struct.get_s byte load failed");
			else
				checkError(compiler.movzx(result, asmjit::x86::byte_ptr(object, offset)),
						"AsmJit struct.get_u byte load failed");
		} else {
			if (signExtend)
				checkError(compiler.movsx(result, asmjit::x86::word_ptr(object, offset)),
						"AsmJit struct.get_s word load failed");
			else
				checkError(compiler.movzx(result, asmjit::x86::word_ptr(object, offset)),
						"AsmJit struct.get_u word load failed");
		}
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
		if (fields[arg2].storage.isPacked)
			throw std::runtime_error("AsmJit struct.set does not lower packed fields yet");
		const int width = kindWidth(fields[arg2].storage.value.opcode);
		if (width != 4 && width != 8)
			throw std::runtime_error("AsmJit struct.set does not lower this field type yet");
		if (values.size() < 2)
			throw std::runtime_error("AsmJit struct.set operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto object = values.back();
		values.pop_back();
		const auto offset = static_cast<int32_t>(WASM::gcStructFieldOffset(fields, arg2));
		asmjit::Label notNull = compiler.new_label();
		checkError(compiler.test(object, object), "AsmJit struct.set null test failed");
		checkError(compiler.jnz(notNull), "AsmJit struct.set null branch failed");
		asmjit::FuncSignature nullSignature(asmjit::CallConvId::kCDecl);
		nullSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* nullCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(nullCall),
				reinterpret_cast<uint64_t>(&asmjitReportNullStructure), nullSignature),
				"AsmJit struct.set null trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit struct.set label binding failed");
		if (width == 4)
			checkError(compiler.mov(asmjit::x86::dword_ptr(object, offset), value),
					"AsmJit struct.set field store failed");
		else
			checkError(compiler.mov(asmjit::x86::qword_ptr(object, offset), value),
					"AsmJit struct.set field store failed");
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
		checkError(compiler.test(object, object), "AsmJit struct.get null test failed");
		checkError(compiler.jnz(notNull), "AsmJit struct.get null branch failed");
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
			checkError(compiler.mov(result, asmjit::x86::dword_ptr(object,
					static_cast<int32_t>(offset))), "AsmJit struct.get field load failed");
		else
			checkError(compiler.mov(result, asmjit::x86::qword_ptr(object,
					static_cast<int32_t>(offset))), "AsmJit struct.get field load failed");
		values.push_back(result);
	}

	void dispatchRefFunc(WASM::FuncIdx index) override
	{
		const size_t importedCount = module.importFunctions.size();
		const size_t totalCount = importedCount + module.internalFunctionTypeIndices.size();
		if (index >= totalCount)
			throw std::runtime_error("AsmJit ref.func index is out of range");
		// The reference is the function's own code address, so no call and no
		// memory load are needed. Equality is all Wasm can observe, and distinct
		// functions have distinct bodies.
		auto reference = compiler.new_gp_ptr("funcRef");
		if (index >= importedCount) {
			const size_t internalIndex = static_cast<size_t>(index) - importedCount;
			checkError(compiler.lea(reference,
					asmjit::x86::ptr(functions[internalIndex]->label())),
					"AsmJit ref.func address emission failed");
		} else {
			if (index >= internals.importStorage.size()
					|| internals.importStorage[index].fnPtr == nullptr)
				throw std::runtime_error("AsmJit ref.func references an unresolved function import");
			checkError(compiler.mov(reference,
					reinterpret_cast<uint64_t>(internals.importStorage[index].fnPtr)),
					"AsmJit ref.func import materialisation failed");
		}
		values.push_back(reference);
	}

	void emitRefCast(const WASM::HeapType& arg, bool nullable)
	{
		if (!arg.isTypeIndex)
			throw std::runtime_error("AsmJit ref.cast on an abstract heap type is not lowered yet");
		if (values.empty())
			throw std::runtime_error("AsmJit ref.cast operand stack underflow");
		// A cast returns the reference, so it stays on the value stack.
		auto reference = values.back();
		const uint32_t expected = module.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
		asmjit::FuncSignature voidSignature(asmjit::CallConvId::kCDecl);
		voidSignature.set_ret(asmjit::TypeId::kVoid);
		asmjit::Label afterCast = compiler.new_label();
		if (nullable) {
			checkError(compiler.test(reference, reference), "AsmJit ref.cast null test failed");
			checkError(compiler.jz(afterCast), "AsmJit ref.cast null pass failed");
		} else {
			asmjit::Label notNull = compiler.new_label();
			checkError(compiler.test(reference, reference), "AsmJit ref.cast null test failed");
			checkError(compiler.jnz(notNull), "AsmJit ref.cast null branch failed");
			asmjit::InvokeNode* nullCall = nullptr;
			checkError(compiler.invoke(asmjit::Out(nullCall),
					reinterpret_cast<uint64_t>(&asmjitReportNullStructure), voidSignature),
					"AsmJit ref.cast null trap emission failed");
			emitPendingTrapReturn();
			checkError(compiler.bind(notNull), "AsmJit ref.cast null label binding failed");
		}
		auto typeId = compiler.new_gp32("castTypeId");
		checkError(compiler.mov(typeId, asmjit::x86::dword_ptr(reference,
				static_cast<int32_t>(WASM::GcHeader::kTypeIdOffset))),
				"AsmJit ref.cast type-id load failed");
		checkError(compiler.cmp(typeId, expected), "AsmJit ref.cast compare failed");
		checkError(compiler.je(afterCast), "AsmJit ref.cast match branch failed");
		asmjit::InvokeNode* castCall = nullptr;
		checkError(compiler.invoke(asmjit::Out(castCall),
				reinterpret_cast<uint64_t>(&asmjitReportCastFailure), voidSignature),
				"AsmJit ref.cast failure trap emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(afterCast), "AsmJit ref.cast label binding failed");
	}

	void dispatchRefCast(const WASM::HeapType& arg) override { emitRefCast(arg, false); }
	void dispatchRefCastNull(const WASM::HeapType& arg) override { emitRefCast(arg, true); }

	void dispatchRefTest(const WASM::HeapType& arg) override
	{
		if (!arg.isTypeIndex)
			throw std::runtime_error("AsmJit ref.test on an abstract heap type is not lowered yet");
		if (values.empty())
			throw std::runtime_error("AsmJit ref.test operand stack underflow");
		auto reference = values.back();
		values.pop_back();
		const uint32_t expected = module.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
		auto result = compiler.new_gp32("refTest");
		checkError(compiler.mov(result, static_cast<uint32_t>(0)), "AsmJit ref.test zeroing failed");
		asmjit::Label done = compiler.new_label();
		checkError(compiler.test(reference, reference), "AsmJit ref.test null test failed");
		checkError(compiler.jz(done), "AsmJit ref.test null branch failed");
		auto typeId = compiler.new_gp32("refTestTypeId");
		checkError(compiler.mov(typeId, asmjit::x86::dword_ptr(reference,
				static_cast<int32_t>(WASM::GcHeader::kTypeIdOffset))),
				"AsmJit ref.test type-id load failed");
		checkError(compiler.cmp(typeId, expected), "AsmJit ref.test type-id compare failed");
		checkError(compiler.jne(done), "AsmJit ref.test mismatch branch failed");
		checkError(compiler.mov(result, static_cast<uint32_t>(1)), "AsmJit ref.test match failed");
		checkError(compiler.bind(done), "AsmJit ref.test label binding failed");
		values.push_back(result);
	}

	void dispatchRefTestNull(const WASM::HeapType& arg) override
	{
		if (!arg.isTypeIndex)
			throw std::runtime_error("AsmJit ref.test on an abstract heap type is not lowered yet");
		if (values.empty())
			throw std::runtime_error("AsmJit ref.test operand stack underflow");
		auto reference = values.back();
		values.pop_back();
		const uint32_t expected = module.typeId(WASM::LocalTypeIdx{arg.typeIndex}).value;
		// Null passes the nullable test; otherwise the type id decides.
		auto result = compiler.new_gp32("refTest");
		checkError(compiler.mov(result, static_cast<uint32_t>(1)), "AsmJit ref.test initialisation failed");
		asmjit::Label done = compiler.new_label();
		checkError(compiler.test(reference, reference), "AsmJit ref.test null test failed");
		checkError(compiler.jz(done), "AsmJit ref.test null branch failed");
		checkError(compiler.mov(result, static_cast<uint32_t>(0)), "AsmJit ref.test reset failed");
		auto typeId = compiler.new_gp32("refTestTypeId");
		checkError(compiler.mov(typeId, asmjit::x86::dword_ptr(reference,
				static_cast<int32_t>(WASM::GcHeader::kTypeIdOffset))),
				"AsmJit ref.test type-id load failed");
		checkError(compiler.cmp(typeId, expected), "AsmJit ref.test type-id compare failed");
		checkError(compiler.jne(done), "AsmJit ref.test mismatch branch failed");
		checkError(compiler.mov(result, static_cast<uint32_t>(1)), "AsmJit ref.test match failed");
		checkError(compiler.bind(done), "AsmJit ref.test label binding failed");
		values.push_back(result);
	}

	void dispatchRefAsNonNull() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit ref.as_non_null operand underflow");
		auto reference = values.back();
		asmjit::Label notNull = compiler.new_label();
		checkError(compiler.test(reference, reference), "AsmJit ref.as_non_null test failed");
		checkError(compiler.jnz(notNull), "AsmJit ref.as_non_null branch failed");
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke),
				reinterpret_cast<uint64_t>(&asmjitReportNullStructure), signature),
				"AsmJit ref.as_non_null trap helper emission failed");
		emitPendingTrapReturn();
		checkError(compiler.bind(notNull), "AsmJit ref.as_non_null label binding failed");
	}

	void dispatchRefEq() override
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit ref.eq operand underflow");
		auto rhs = values.back();
		values.pop_back();
		auto lhs = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("refEq");
		checkError(compiler.xor_(result, result), "AsmJit ref.eq zeroing failed");
		checkError(compiler.cmp(lhs, rhs), "AsmJit ref.eq compare failed");
		checkError(compiler.set(asmjit::x86::CondCode::kE, result.r8()),
				"AsmJit ref.eq set failed");
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

	void dispatchI8x16Add() override { emitVectorHelper2(&wasm_i8x16_add); }
	void dispatchI8x16Sub() override { emitVectorHelper2(&wasm_i8x16_sub); }
	void dispatchI16x8Add() override { emitVectorHelper2(&wasm_i16x8_add); }
	void dispatchI16x8Sub() override { emitVectorHelper2(&wasm_i16x8_sub); }
	void dispatchI16x8Mul() override { emitVectorHelper2(&wasm_i16x8_mul); }
	void dispatchI32x4Add() override { emitVectorHelper2(&wasm_i32x4_add); }
	void dispatchI32x4Sub() override { emitVectorHelper2(&wasm_i32x4_sub); }
	void dispatchI32x4Mul() override { emitVectorHelper2(&wasm_i32x4_mul); }
	void dispatchI64x2Add() override { emitVectorHelper2(&wasm_i64x2_add); }
	void dispatchI64x2Sub() override { emitVectorHelper2(&wasm_i64x2_sub); }
	void dispatchI64x2Mul() override { emitVectorHelper2(&wasm_i64x2_mul); }
	void dispatchI8x16Eq() override { emitVectorHelper2(&wasm_i8x16_eq); }
	void dispatchI8x16Ne() override { emitVectorHelper2(&wasm_i8x16_ne); }
	void dispatchI8x16LtS() override { emitVectorHelper2(&wasm_i8x16_lt_s); }
	void dispatchI8x16LtU() override { emitVectorHelper2(&wasm_i8x16_lt_u); }
	void dispatchI8x16GtS() override { emitVectorHelper2(&wasm_i8x16_gt_s); }
	void dispatchI8x16GtU() override { emitVectorHelper2(&wasm_i8x16_gt_u); }
	void dispatchI8x16LeS() override { emitVectorHelper2(&wasm_i8x16_le_s); }
	void dispatchI8x16LeU() override { emitVectorHelper2(&wasm_i8x16_le_u); }
	void dispatchI8x16GeS() override { emitVectorHelper2(&wasm_i8x16_ge_s); }
	void dispatchI8x16GeU() override { emitVectorHelper2(&wasm_i8x16_ge_u); }
	void dispatchI16x8Eq() override { emitVectorHelper2(&wasm_i16x8_eq); }
	void dispatchI16x8Ne() override { emitVectorHelper2(&wasm_i16x8_ne); }
	void dispatchI16x8LtS() override { emitVectorHelper2(&wasm_i16x8_lt_s); }
	void dispatchI16x8LtU() override { emitVectorHelper2(&wasm_i16x8_lt_u); }
	void dispatchI16x8GtS() override { emitVectorHelper2(&wasm_i16x8_gt_s); }
	void dispatchI16x8GtU() override { emitVectorHelper2(&wasm_i16x8_gt_u); }
	void dispatchI16x8LeS() override { emitVectorHelper2(&wasm_i16x8_le_s); }
	void dispatchI16x8LeU() override { emitVectorHelper2(&wasm_i16x8_le_u); }
	void dispatchI16x8GeS() override { emitVectorHelper2(&wasm_i16x8_ge_s); }
	void dispatchI16x8GeU() override { emitVectorHelper2(&wasm_i16x8_ge_u); }
	void dispatchI32x4Eq() override { emitVectorHelper2(&wasm_i32x4_eq); }
	void dispatchI32x4Ne() override { emitVectorHelper2(&wasm_i32x4_ne); }
	void dispatchI32x4LtS() override { emitVectorHelper2(&wasm_i32x4_lt_s); }
	void dispatchI32x4LtU() override { emitVectorHelper2(&wasm_i32x4_lt_u); }
	void dispatchI32x4GtS() override { emitVectorHelper2(&wasm_i32x4_gt_s); }
	void dispatchI32x4GtU() override { emitVectorHelper2(&wasm_i32x4_gt_u); }
	void dispatchI32x4LeS() override { emitVectorHelper2(&wasm_i32x4_le_s); }
	void dispatchI32x4LeU() override { emitVectorHelper2(&wasm_i32x4_le_u); }
	void dispatchI32x4GeS() override { emitVectorHelper2(&wasm_i32x4_ge_s); }
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
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kVoid);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitReportUnreachable), signature),
				"AsmJit unreachable trap helper emission failed");
		const size_t stackHeight = controls.empty() ? 0 : controls.back().stackHeight;
		const size_t placeholderCount = controls.empty() ? resultCount : controls.back().resultCount;
		values.resize(stackHeight);
		if (resultCount == 0) {
			checkError(compiler.ret(), "AsmJit unreachable void return emission failed");
		} else {
			auto zero = resultType == WASM::ValueTypeCode::I64
					? compiler.new_gp64("unreachableResult") : compiler.new_gp32("unreachableResult");
			checkError(compiler.xor_(zero, zero), "AsmJit unreachable result initialisation failed");
			checkError(compiler.ret(zero), "AsmJit unreachable result return emission failed");
		}
		if (placeholderCount != 0) {
			auto placeholder = compiler.new_gp32("unreachableStackPlaceholder");
			checkError(compiler.xor_(placeholder, placeholder), "AsmJit unreachable placeholder initialisation failed");
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

	void dispatchI32Clz() override { emitBitCount(BitCountOp::LeadingZeros); }
	void dispatchI32Ctz() override { emitBitCount(BitCountOp::TrailingZeros); }
	void dispatchI32Popcnt() override { emitUnaryCall(&asmjitI32Popcnt, "i32.popcnt"); }
	void dispatchI32DivS() override { emitCheckedBinaryCall(&asmjitI32DivS, "i32.div_s"); }
	void dispatchI32DivU() override { emitCheckedBinaryCall(&asmjitI32DivU, "i32.div_u"); }
	void dispatchI32RemS() override { emitCheckedBinaryCall(&asmjitI32RemS, "i32.rem_s"); }
	void dispatchI32RemU() override { emitCheckedBinaryCall(&asmjitI32RemU, "i32.rem_u"); }

	void dispatchI64Const(int64_t value) override
	{
		auto reg = compiler.new_gp64("i64Constant");
		checkError(compiler.mov(reg, value), "AsmJit i64.const emission failed");
		values.push_back(reg);
	}

	void dispatchI64Eqz() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i64.eqz operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("i64Eqz");
		checkError(compiler.xor_(result, result), "AsmJit i64.eqz result initialisation failed");
		checkError(compiler.test(value, value), "AsmJit i64.eqz test emission failed");
		checkError(compiler.set(asmjit::x86::CondCode::kE, result.r8()),
				"AsmJit i64.eqz condition emission failed");
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

	void dispatchI64Add() override { emitIntegerBinary(BinaryOp::Add); }
	void dispatchI64Sub() override { emitIntegerBinary(BinaryOp::Sub); }
	void dispatchI64Mul() override { emitIntegerBinary(BinaryOp::Mul); }
	void dispatchI64And() override { emitIntegerBinary(BinaryOp::And); }
	void dispatchI64Or() override { emitIntegerBinary(BinaryOp::Or); }
	void dispatchI64Xor() override { emitIntegerBinary(BinaryOp::Xor); }
	void dispatchI64Shl() override { emitIntegerShift(ShiftOp::Left); }
	void dispatchI64ShrS() override { emitIntegerShift(ShiftOp::ArithmeticRight); }
	void dispatchI64ShrU() override { emitIntegerShift(ShiftOp::LogicalRight); }
	void dispatchI64Rotl() override { emitIntegerShift(ShiftOp::RotateLeft); }
	void dispatchI64Rotr() override { emitIntegerShift(ShiftOp::RotateRight); }
	void dispatchI64DivS() override { emitCheckedI64Call(&asmjitI64DivS, "i64.div_s"); }
	void dispatchI64DivU() override { emitCheckedI64Call(&asmjitI64DivU, "i64.div_u"); }
	void dispatchI64RemS() override { emitCheckedI64Call(&asmjitI64RemS, "i64.rem_s"); }
	void dispatchI64RemU() override { emitCheckedI64Call(&asmjitI64RemU, "i64.rem_u"); }

	void dispatchI32Eqz() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i32.eqz operand stack underflow");
		auto value = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("eqz");
		checkError(compiler.xor_(result, result), "AsmJit i32.eqz result initialisation failed");
		checkError(compiler.test(value, value), "AsmJit i32.eqz test emission failed");
		checkError(compiler.set(asmjit::x86::CondCode::kE, result.r8()),
				"AsmJit i32.eqz condition emission failed");
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
		checkError(compiler.mov(reg, bits), "AsmJit f32.const emission failed");
		values.push_back(reg);
	}

	void dispatchF64Const(double value) override
	{
		uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		auto reg = compiler.new_gp64("f64Constant");
		checkError(compiler.mov(reg, bits), "AsmJit f64.const emission failed");
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
	{ emitFloatBinary(asmjit::x86::Inst::kIdAddss, asmjit::x86::Inst::kIdAddsd, 4); }
	void dispatchF32Sub() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdSubss, asmjit::x86::Inst::kIdSubsd, 4); }
	void dispatchF32Mul() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdMulss, asmjit::x86::Inst::kIdMulsd, 4); }
	void dispatchF32Div() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdDivss, asmjit::x86::Inst::kIdDivsd, 4); }
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
	{ emitFloatBinary(asmjit::x86::Inst::kIdAddss, asmjit::x86::Inst::kIdAddsd, 8); }
	void dispatchF64Sub() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdSubss, asmjit::x86::Inst::kIdSubsd, 8); }
	void dispatchF64Mul() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdMulss, asmjit::x86::Inst::kIdMulsd, 8); }
	void dispatchF64Div() override
	{ emitFloatBinary(asmjit::x86::Inst::kIdDivss, asmjit::x86::Inst::kIdDivsd, 8); }
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
	// already carries it, so there is nothing to emit.
	void dispatchI32ReinterpretF32() override {}
	void dispatchI64ReinterpretF64() override {}
	void dispatchF32ReinterpretI32() override {}
	void dispatchF64ReinterpretI64() override {}

	void dispatchF32Load(WASM::MemArg arg) override
	{ emitMemoryLoad(arg, 4, LoadKind::F32); }
	void dispatchF64Load(WASM::MemArg arg) override
	{ emitMemoryLoad(arg, 8, LoadKind::F64); }
	void dispatchF32Store(WASM::MemArg arg) override { emitMemoryStore(arg, 4); }
	void dispatchF64Store(WASM::MemArg arg) override { emitMemoryStore(arg, 8); }


	void dispatchReturn() override
	{
		if (resultCount == 0) {
			checkError(compiler.ret(), "AsmJit return emission failed");
			return;
		}
		if (values.empty())
			throw std::runtime_error("AsmJit return operand stack underflow");
		if (isFloatKind(resultType))
			checkError(compiler.ret(
					materializeFloat(values.back(), kindWidth(resultType))),
					"AsmJit float return emission failed");
		else
			checkError(compiler.ret(values.back()), "AsmJit return emission failed");
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
		WASM::ValueTypeCode resultType = WASM::ValueTypeCode::Void;
		asmjit::x86::Gp result;
		asmjit::Label begin;
		asmjit::Label alternate;
		asmjit::Label end;
		bool hasElse = false;
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
			checkError(compiler.movd(xmm, bits), "AsmJit f32 materialisation failed");
		else
			checkError(compiler.movq(xmm, bits), "AsmJit f64 materialisation failed");
		return xmm;
	}

	//! Copies a float value out of an Xmm into a fresh Gp slot.
	asmjit::x86::Gp bitsOfFloat(const asmjit::x86::Vec& xmm, int width)
	{
		asmjit::x86::Gp bits = width == 4
				? compiler.new_gp32("floatBits") : compiler.new_gp64("floatBits");
		if (width == 4)
			checkError(compiler.movd(bits, xmm), "AsmJit f32 result move failed");
		else
			checkError(compiler.movq(bits, xmm), "AsmJit f64 result move failed");
		return bits;
	}

	//! Returns one value from the function, materialising a float result.
	void emitFunctionReturn(const asmjit::x86::Gp& value)
	{
		if (isFloatKind(resultType))
			checkError(compiler.ret(materializeFloat(value, kindWidth(resultType))),
					"AsmJit float function-branch return failed");
		else
			checkError(compiler.ret(value), "AsmJit function-branch return failed");
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
		checkError(compiler.lea(address, storage), "AsmJit v128 buffer address failed");
		return address;
	}

	//! Copies 16 bytes between two buffer addresses.
	void copyVector(const asmjit::x86::Gp& destination, const asmjit::x86::Gp& source)
	{
		for (int word = 0; word < 2; ++word) {
			auto value = compiler.new_gp64("v128Word");
			checkError(compiler.mov(value,
					asmjit::x86::qword_ptr(source, word * 8)),
					"AsmJit v128 word load failed");
			checkError(compiler.mov(asmjit::x86::qword_ptr(destination, word * 8), value),
					"AsmJit v128 word store failed");
		}
	}

	//! Writes 16 zero bytes to a buffer.
	void zeroVector(const asmjit::x86::Gp& buffer)
	{
		for (int word = 0; word < 2; ++word) {
			checkError(compiler.mov(asmjit::x86::qword_ptr(buffer, word * 8),
					static_cast<uint64_t>(0)), "AsmJit v128 zeroing failed");
		}
	}

	//! Sets a call's value arguments from prepared operands.
	void setCallArguments(asmjit::InvokeNode* invoke,
			const std::vector<asmjit::Reg>& prepared)
	{
		for (size_t i = 0; i < prepared.size(); ++i)
			invoke->set_arg(i + 1, prepared[i]);
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
			values.push_back(result);
		}
	}
	void emitFloatBinary(asmjit::x86::Inst::Id op32, asmjit::x86::Inst::Id op64, int width)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit float binary operand stack underflow");
		const auto second = values.back(); values.pop_back();
		const auto first = values.back(); values.pop_back();
		const auto lhs = materializeFloat(first, width);
		const auto rhs = materializeFloat(second, width);
		checkError(compiler.emit(width == 4 ? op32 : op64, lhs, rhs),
				"AsmJit float binary emission failed");
		values.push_back(bitsOfFloat(lhs, width));
	}

	//! Calls a one-argument float helper over the top slot.
	void emitFloatHelper1(uint64_t function, int width)
	{
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
		if (values.size() < 2)
			throw std::runtime_error("AsmJit float compare operand stack underflow");
		const auto second = values.back(); values.pop_back();
		const auto first = values.back(); values.pop_back();
		const auto lhs = materializeFloat(first, width);
		const auto rhs = materializeFloat(second, width);

		// Zero the flag registers *before* the compare: `xor_` sets flags, and
		// `setcc` must read the comparison's flags, not the zeroing's.
		auto primary = compiler.new_gp32("floatFlag");
		auto secondary = compiler.new_gp32("floatFlag");
		checkError(compiler.xor_(primary, primary), "AsmJit float flag zeroing failed");
		checkError(compiler.xor_(secondary, secondary), "AsmJit float flag zeroing failed");
		checkError(width == 4 ? compiler.ucomiss(lhs, rhs) : compiler.ucomisd(lhs, rhs),
				"AsmJit float compare emission failed");
		const auto set = [&](asmjit::x86::CondCode condition, asmjit::x86::Gp& reg) {
			checkError(compiler.set(condition, reg.r8()),
					"AsmJit float condition emission failed");
		};
		switch (operation) {
		case FloatCompareOp::Eq:
			set(asmjit::x86::CondCode::kE, primary);
			set(asmjit::x86::CondCode::kNP, secondary);
			checkError(compiler.and_(primary, secondary),
					"AsmJit float eq ordered mask failed");
			values.push_back(primary);
			break;
		case FloatCompareOp::Ne:
			set(asmjit::x86::CondCode::kNE, primary);
			set(asmjit::x86::CondCode::kP, secondary);
			checkError(compiler.or_(primary, secondary),
					"AsmJit float ne unordered mask failed");
			values.push_back(primary);
			break;
		case FloatCompareOp::Lt:
			set(asmjit::x86::CondCode::kB, primary);
			set(asmjit::x86::CondCode::kNP, secondary);
			checkError(compiler.and_(primary, secondary),
					"AsmJit float lt ordered mask failed");
			values.push_back(primary);
			break;
		case FloatCompareOp::Le:
			set(asmjit::x86::CondCode::kBE, primary);
			set(asmjit::x86::CondCode::kNP, secondary);
			checkError(compiler.and_(primary, secondary),
					"AsmJit float le ordered mask failed");
			values.push_back(primary);
			break;
		case FloatCompareOp::Gt:
			set(asmjit::x86::CondCode::kA, primary);
			values.push_back(primary);
			break;
		case FloatCompareOp::Ge:
			set(asmjit::x86::CondCode::kAE, primary);
			values.push_back(primary);
			break;
		}
	}

	//! Absolute value as a bit operation (clear the sign bit).
	void emitFloatAbs()
	{
		if (values.empty())
			throw std::runtime_error("AsmJit f32.abs operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		checkError(compiler.shl(bits, 1), "AsmJit float abs shift failed");
		checkError(compiler.shr(bits, 1), "AsmJit float abs restore failed");
		values.push_back(bits);
	}

	//! Negation as a bit operation (toggle the sign bit).
	void emitFloatNeg(int width)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit f32.neg operand stack underflow");
		const auto bits = values.back(); values.pop_back();
		const auto sign = width == 4
				? compiler.new_gp32("floatSign") : compiler.new_gp64("floatSign");
		checkError(compiler.mov(sign, 1), "AsmJit float sign initialisation failed");
		checkError(compiler.shl(sign, width == 4 ? 31 : 63),
				"AsmJit float sign shift failed");
		checkError(compiler.xor_(bits, sign), "AsmJit float neg emission failed");
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
		checkError(compiler.mov(mask, 1), "AsmJit copysign mask initialisation failed");
		checkError(compiler.shl(mask, width == 4 ? 31 : 63),
				"AsmJit copysign mask shift failed");
		checkError(compiler.mov(sign, y), "AsmJit copysign sign copy failed");
		checkError(compiler.and_(sign, mask), "AsmJit copysign sign extraction failed");
		checkError(compiler.shl(x, 1), "AsmJit copysign magnitude shift failed");
		checkError(compiler.shr(x, 1), "AsmJit copysign magnitude restore failed");
		checkError(compiler.or_(x, sign), "AsmJit copysign combine failed");
		values.push_back(x);
	}

	//! Calls a checked float-to-integer helper over the top slot.
	void emitFloatToInt(uint64_t function, int floatWidth, int intWidth)
	{
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
        auto result = first.size() == 8
                ? compiler.new_gp64("selectResult") : compiler.new_gp32("selectResult");
        auto chooseSecond = compiler.new_label();
        auto done = compiler.new_label();
        checkError(compiler.mov(result, first), "AsmJit select first-value copy failed");
        checkError(compiler.test(condition, condition), "AsmJit select condition test failed");
        checkError(compiler.jz(chooseSecond), "AsmJit select condition branch failed");
        checkError(compiler.jmp(done), "AsmJit select completion jump failed");
        checkError(compiler.bind(chooseSecond), "AsmJit select second-value label failed");
        checkError(compiler.mov(result, second), "AsmJit select second-value copy failed");
        checkError(compiler.bind(done), "AsmJit select completion label failed");
        values.push_back(result);
    }

    void emitMemoryCopy(WASM::MemIdx destination, WASM::MemIdx source)
    {
        if (validateMemoryIndex(destination).indexType != WASM::MemoryIndexType::I32
                || validateMemoryIndex(source).indexType != WASM::MemoryIndexType::I32)
            throw std::runtime_error("AsmJit does not lower bulk memory on memory64 yet");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.copy operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto sourceAddress = values.back(); values.pop_back();
        const auto destinationAddress = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        for (int i = 0; i < 6; ++i)
            signature.add_arg(i == 0 ? asmjit::TypeId::kUIntPtr : asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto dstMemory = compiler.new_gp32("copyDestinationMemory");
        auto srcMemory = compiler.new_gp32("copySourceMemory");
        checkError(compiler.mov(dstMemory, static_cast<uint32_t>(destination)), "AsmJit memory.copy destination index failed");
        checkError(compiler.mov(srcMemory, static_cast<uint32_t>(source)), "AsmJit memory.copy source index failed");
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitMemoryCopy), signature),
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
        if (validateMemoryIndex(memoryIndex).indexType != WASM::MemoryIndexType::I32)
            throw std::runtime_error("AsmJit does not lower bulk memory on memory64 yet");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.init operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto source = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        for (int i = 0; i < 5; ++i)
            signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto data = compiler.new_gp32("memoryInitDataIndex");
        auto memory = compiler.new_gp32("memoryInitMemoryIndex");
        checkError(compiler.mov(data, dataIndex), "AsmJit memory.init data index materialisation failed");
        checkError(compiler.mov(memory, static_cast<uint32_t>(memoryIndex)), "AsmJit memory.init memory index materialisation failed");
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitMemoryInit), signature),
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
        if (validateMemoryIndex(index).indexType != WASM::MemoryIndexType::I32)
            throw std::runtime_error("AsmJit does not lower bulk memory on memory64 yet");
        if (values.size() < 3)
            throw std::runtime_error("AsmJit memory.fill operand stack underflow");
        const auto length = values.back(); values.pop_back();
        const auto value = values.back(); values.pop_back();
        const auto destination = values.back(); values.pop_back();
        asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
        signature.set_ret(asmjit::TypeId::kVoid);
        signature.add_arg(asmjit::TypeId::kUIntPtr);
        // vmContext + (memory index, destination, value, length) — the arity of
        // `asmjitMemoryFill` above, which every `set_arg` below must match.
        for (int i = 0; i < 4; ++i)
            signature.add_arg(asmjit::TypeId::kUInt32);
        asmjit::InvokeNode* invoke = nullptr;
        auto memoryIndex = compiler.new_gp32("fillMemoryIndex");
        checkError(compiler.mov(memoryIndex, static_cast<uint32_t>(index)), "AsmJit memory.fill index failed");
        checkError(compiler.invoke(asmjit::Out(invoke), reinterpret_cast<uint64_t>(&asmjitMemoryFill), signature),
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
		frame.stackHeight = values.size();
		frame.resultType = blockResultType(type);
		frame.resultCount = frame.resultType == WASM::ValueTypeCode::Void ? 0 : 1;
		if (frame.resultCount != 0) {
			frame.result = kindWidth(frame.resultType) == 8
					? compiler.new_gp64("blockResult") : compiler.new_gp32("blockResult");
		}
		if (kind == ControlKind::Loop) {
			frame.begin = compiler.new_label();
			frame.end = compiler.new_label();
		} else {
			frame.end = compiler.new_label();
		}
		controls.push_back(frame);
	}

	WASM::ValueTypeCode blockResultType(const WASM::BlockType& type) const
	{
		if (type.isVoid())
			return WASM::ValueTypeCode::Void;
		if (type.isTypeIndex) {
			if (type.typeIndex < 0 || static_cast<uint32_t>(type.typeIndex) >= module.typeCount())
				throw std::runtime_error("AsmJit block type index is out of range");
			const WASM::FuncType blockType = module.funcTypeAt(static_cast<uint32_t>(type.typeIndex));
			if (!blockType.params.empty())
				throw std::runtime_error("AsmJit block parameters are not supported yet");
			if (blockType.results.size() > 1)
				throw std::runtime_error("AsmJit block multi-value results are not supported yet");
			if (blockType.results.empty())
				return WASM::ValueTypeCode::Void;
			requireScalar(blockType.results.front().val, "block results");
			return blockType.results.front().val.opcode;
		}
		if (kindWidth(type.valType) == 0)
			throw std::runtime_error("AsmJit structured control flow supports only scalar block results");
		return type.valType;
	}

	void storeBlockResult(const ControlFrame& frame, const char* description)
	{
		if (values.size() != frame.stackHeight + frame.resultCount)
			throw std::runtime_error(std::string("AsmJit ") + description
					+ " leaves an unexpected operand stack shape");
		if (frame.resultCount != 0)
			checkError(compiler.mov(frame.result, values.back()), "AsmJit structured result transfer failed");
	}
	void emitBinary(BinaryOp operation)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32 binary operation stack underflow");
		asmjit::x86::Gp rhs = values.back();
		values.pop_back();
		asmjit::x86::Gp lhs = values.back();
		values.pop_back();
		switch (operation) {
		case BinaryOp::Add:
			checkError(compiler.add(lhs, rhs), "AsmJit i32.add emission failed");
			break;
		case BinaryOp::Sub:
			checkError(compiler.sub(lhs, rhs), "AsmJit i32.sub emission failed");
			break;
		case BinaryOp::Mul:
			checkError(compiler.imul(lhs, rhs), "AsmJit i32.mul emission failed");
			break;
		case BinaryOp::And:
			checkError(compiler.and_(lhs, rhs), "AsmJit i32.and emission failed");
			break;
		case BinaryOp::Or:
			checkError(compiler.or_(lhs, rhs), "AsmJit i32.or emission failed");
			break;
		case BinaryOp::Xor:
			checkError(compiler.xor_(lhs, rhs), "AsmJit i32.xor emission failed");
			break;
		}
		values.push_back(lhs);
	}

	const WASM::GlobalType& validateGlobalIndex(WASM::GlobalIdx index) const
	{
		// Module::globals is already the complete global index space:
		// imported globals precede defined globals. importGlobals is only the
		// import-section metadata and must not be counted a second time.
		if (index >= module.globals.size())
			throw std::runtime_error("AsmJit global instruction references an unknown global");
		const WASM::GlobalType& type = module.globals[index].type;
		requireScalar(type.contentType, "globals");
		return type;
	}

	asmjit::x86::Gp emitGlobalCell(WASM::GlobalIdx index)
	{
		(void)validateGlobalIndex(index);
		auto globals = compiler.new_gp_ptr("globals");
		checkError(compiler.mov(globals, asmjit::x86::ptr(vmContext,
				static_cast<int32_t>(offsetof(WASM::VMContext, globals)))),
				"AsmJit VMContext globals load failed");
		auto cell = compiler.new_gp_ptr("globalCell");
		checkError(compiler.mov(cell, asmjit::x86::ptr(globals,
				static_cast<int32_t>(index * sizeof(WASM::Value*)))),
				"AsmJit global cell lookup failed");
		return cell;
	}

	void emitGlobalGet(WASM::GlobalIdx index)
	{
		auto cell = emitGlobalCell(index);
		const auto& type = validateGlobalIndex(index);
		auto value = newIntegerRegister(compiler, type.contentType, "globalValue");
		if (kindWidth(type.contentType.opcode) == 8) {
			checkError(compiler.mov(value, asmjit::x86::qword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i64)))),
					"AsmJit 64-bit global.get load failed");
		} else {
			checkError(compiler.mov(value, asmjit::x86::dword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i32)))),
					"AsmJit 32-bit global.get load failed");
		}
		values.push_back(value);
	}

	void emitGlobalSet(WASM::GlobalIdx index)
	{
		const WASM::GlobalType& type = validateGlobalIndex(index);
		if (!type.isMutable)
			throw std::runtime_error("AsmJit global.set references an immutable global");
		if (values.empty())
			throw std::runtime_error("AsmJit global.set operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto cell = emitGlobalCell(index);
		if (kindWidth(type.contentType.opcode) == 8) {
			checkError(compiler.mov(asmjit::x86::qword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i64))), value),
					"AsmJit 64-bit global.set store failed");
		} else {
			checkError(compiler.mov(asmjit::x86::dword_ptr(cell,
					static_cast<int32_t>(offsetof(WASM::Value, i32))), value),
					"AsmJit 32-bit global.set store failed");
		}
	}

	const WASM::MemoryType& validateMemoryIndex(WASM::MemIdx index) const
	{
		// Module::memories is the complete memory index space, imports first.
		if (index >= module.memories.size())
			throw std::runtime_error("AsmJit memory instruction references an unknown memory");
		return module.memories[index];
	}

	void emitMemorySize(WASM::MemIdx index)
	{
		const bool is64 = validateMemoryIndex(index).indexType
				== WASM::MemoryIndexType::I64;
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(is64 ? asmjit::TypeId::kUInt64 : asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		auto memoryIndex = compiler.new_gp32("memorySizeIndex");
		checkError(compiler.mov(memoryIndex, static_cast<uint32_t>(index)),
				"AsmJit memory.size index materialisation failed");
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
		checkError(compiler.mov(memoryIndex, static_cast<uint32_t>(index)),
				"AsmJit memory.grow index materialisation failed");
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
		checkError(compiler.mov(memoryIndex, static_cast<uint32_t>(arg.memidx)),
				"AsmJit memory-index materialisation failed");
		auto accessWidth = compiler.new_gp32("memoryWidth");
		checkError(compiler.mov(accessWidth, width), "AsmJit memory-width materialisation failed");
		asmjit::x86::Gp offset;
		if (is64) {
			offset = compiler.new_gp64("memoryOffset");
			checkError(compiler.mov(offset, static_cast<uint64_t>(arg.offset)),
					"AsmJit memory64-offset materialisation failed");
		} else {
			offset = compiler.new_gp32("memoryOffset");
			checkError(compiler.mov(offset, static_cast<uint32_t>(arg.offset)),
					"AsmJit memory32-offset materialisation failed");
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

	void emitMemoryLoad(const WASM::MemArg& arg, uint32_t width, LoadKind kind)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit memory.load operand stack underflow");
		auto wasmAddress = values.back();
		values.pop_back();
		auto pointer = emitMemoryAddress(arg, width, wasmAddress);
		const bool wide = kind == LoadKind::I64 || kind == LoadKind::I64I8Signed
				|| kind == LoadKind::I64I8Unsigned || kind == LoadKind::I64I16Signed
				|| kind == LoadKind::I64I16Unsigned || kind == LoadKind::I64I32Signed
				|| kind == LoadKind::I64I32Unsigned || kind == LoadKind::F64;
		auto result = wide ? compiler.new_gp64("memoryLoad64") : compiler.new_gp32("memoryLoad32");
		switch (kind) {
		case LoadKind::I32:
			checkError(compiler.mov(result, asmjit::x86::dword_ptr(pointer)), "AsmJit i32.load emission failed"); break;
		case LoadKind::I8Signed:
			checkError(compiler.movsx(result, asmjit::x86::byte_ptr(pointer)), "AsmJit i32.load8_s emission failed"); break;
		case LoadKind::I8Unsigned:
			checkError(compiler.movzx(result, asmjit::x86::byte_ptr(pointer)), "AsmJit i32.load8_u emission failed"); break;
		case LoadKind::I16Signed:
			checkError(compiler.movsx(result, asmjit::x86::word_ptr(pointer)), "AsmJit i32.load16_s emission failed"); break;
		case LoadKind::I16Unsigned:
			checkError(compiler.movzx(result, asmjit::x86::word_ptr(pointer)), "AsmJit i32.load16_u emission failed"); break;
		case LoadKind::I64:
			checkError(compiler.mov(result, asmjit::x86::qword_ptr(pointer)), "AsmJit i64.load emission failed"); break;
		case LoadKind::I64I8Signed:
			checkError(compiler.movsx(result, asmjit::x86::byte_ptr(pointer)), "AsmJit i64.load8_s emission failed"); break;
		case LoadKind::I64I8Unsigned:
			checkError(compiler.movzx(result, asmjit::x86::byte_ptr(pointer)), "AsmJit i64.load8_u emission failed"); break;
		case LoadKind::I64I16Signed:
			checkError(compiler.movsx(result, asmjit::x86::word_ptr(pointer)), "AsmJit i64.load16_s emission failed"); break;
		case LoadKind::I64I16Unsigned:
			checkError(compiler.movzx(result, asmjit::x86::word_ptr(pointer)), "AsmJit i64.load16_u emission failed"); break;
		case LoadKind::I64I32Signed:
			checkError(compiler.movsxd(result, asmjit::x86::dword_ptr(pointer)), "AsmJit i64.load32_s emission failed"); break;
		case LoadKind::I64I32Unsigned:
			checkError(compiler.mov(result.r32(), asmjit::x86::dword_ptr(pointer)), "AsmJit i64.load32_u emission failed"); break;
		case LoadKind::F32:
			checkError(compiler.mov(result.r32(), asmjit::x86::dword_ptr(pointer)), "AsmJit f32.load emission failed"); break;
		case LoadKind::F64:
			checkError(compiler.mov(result, asmjit::x86::qword_ptr(pointer)), "AsmJit f64.load emission failed"); break;
		}
		values.push_back(result);
	}

	void emitMemoryStore(const WASM::MemArg& arg, uint32_t width)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit memory.store operand stack underflow");
		auto value = values.back(); values.pop_back();
		auto wasmAddress = values.back(); values.pop_back();
		auto pointer = emitMemoryAddress(arg, width, wasmAddress);
		switch (width) {
		case 1:
			checkError(compiler.mov(asmjit::x86::byte_ptr(pointer), value.r8()), "AsmJit store8 emission failed"); break;
		case 2:
			checkError(compiler.mov(asmjit::x86::word_ptr(pointer), value.r16()), "AsmJit store16 emission failed"); break;
		case 4:
			checkError(compiler.mov(asmjit::x86::dword_ptr(pointer), value.r32()), "AsmJit store32 emission failed"); break;
		case 8:
			checkError(compiler.mov(asmjit::x86::qword_ptr(pointer), value), "AsmJit i64.store emission failed"); break;
		default:
			throw std::runtime_error("AsmJit internal error: unsupported memory-store width");
		}
	}

	void emitBitCount(BitCountOp operation)
	{
		if (values.empty())
			throw std::runtime_error("AsmJit i32 bit-count operand stack underflow");
		auto source = values.back(); values.pop_back();
		auto result = compiler.new_gp32("bitCount");
		auto nonZero = compiler.new_label();
		auto done = compiler.new_label();
		checkError(compiler.test(source, source), "AsmJit bit-count zero test failed");
		checkError(compiler.jnz(nonZero), "AsmJit bit-count nonzero branch failed");
		checkError(compiler.mov(result, 32), "AsmJit bit-count zero result failed");
		checkError(compiler.jmp(done), "AsmJit bit-count completion jump failed");
		checkError(compiler.bind(nonZero), "AsmJit bit-count nonzero label failed");
		if (operation == BitCountOp::LeadingZeros) {
			checkError(compiler.bsr(result, source), "AsmJit i32.clz scan failed");
			checkError(compiler.xor_(result, 31), "AsmJit i32.clz adjustment failed");
		} else {
			checkError(compiler.bsf(result, source), "AsmJit i32.ctz scan failed");
		}
		checkError(compiler.bind(done), "AsmJit bit-count result label failed");
		values.push_back(result);
	}

	void emitUnaryCall(uint32_t (*function)(uint32_t), const char* operation)
	{
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
			checkError(compiler.movsx(result, source.r8()), "AsmJit i32.extend8_s emission failed");
		else if (width == 16)
			checkError(compiler.movsx(result, source.r16()), "AsmJit i32.extend16_s emission failed");
		else
			throw std::runtime_error("AsmJit internal error: invalid sign-extension width");
		values.push_back(result);
	}

	void emitI32WrapI64()
	{
		if (values.empty() || values.back().size() != 8)
			throw std::runtime_error("AsmJit i32.wrap_i64 requires an i64 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp32("wrappedI32");
		checkError(compiler.mov(result, source.r32()), "AsmJit i32.wrap_i64 emission failed");
		values.push_back(result);
	}

	void emitI64ExtendI32(bool isSigned)
	{
		if (values.empty() || values.back().size() != 4)
			throw std::runtime_error("AsmJit i64.extend_i32 requires an i32 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp64("extendedI64");
		if (isSigned)
			checkError(compiler.movsxd(result, source), "AsmJit i64.extend_i32_s emission failed");
		else
			checkError(compiler.mov(result.r32(), source), "AsmJit i64.extend_i32_u emission failed");
		values.push_back(result);
	}

	void emitI64SignExtend(uint32_t width)
	{
		if (values.empty() || values.back().size() != 8)
			throw std::runtime_error("AsmJit i64 sign-extension requires an i64 operand");
		const auto source = values.back();
		values.pop_back();
		auto result = compiler.new_gp64("signExtendedI64");
		if (width == 8)
			checkError(compiler.movsx(result, source.r8()), "AsmJit i64.extend8_s emission failed");
		else if (width == 16)
			checkError(compiler.movsx(result, source.r16()), "AsmJit i64.extend16_s emission failed");
		else if (width == 32)
			checkError(compiler.movsxd(result, source.r32()), "AsmJit i64.extend32_s emission failed");
		else
			throw std::runtime_error("AsmJit internal error: invalid i64 sign-extension width");
		values.push_back(result);
	}

	void emitCompare(asmjit::x86::CondCode condition)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit integer comparison operand stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		auto result = compiler.new_gp32("comparison");
		checkError(compiler.xor_(result, result), "AsmJit comparison result initialisation failed");
		checkError(compiler.cmp(lhs, rhs), "AsmJit comparison emission failed");
		checkError(compiler.set(condition, result.r8()), "AsmJit comparison condition emission failed");
		values.push_back(result);
	}

	void emitIntegerBinary(BinaryOp operation)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i64 binary operation stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		switch (operation) {
		case BinaryOp::Add: checkError(compiler.add(lhs, rhs), "AsmJit i64.add emission failed"); break;
		case BinaryOp::Sub: checkError(compiler.sub(lhs, rhs), "AsmJit i64.sub emission failed"); break;
		case BinaryOp::Mul: checkError(compiler.imul(lhs, rhs), "AsmJit i64.mul emission failed"); break;
		case BinaryOp::And: checkError(compiler.and_(lhs, rhs), "AsmJit i64.and emission failed"); break;
		case BinaryOp::Or: checkError(compiler.or_(lhs, rhs), "AsmJit i64.or emission failed"); break;
		case BinaryOp::Xor: checkError(compiler.xor_(lhs, rhs), "AsmJit i64.xor emission failed"); break;
		}
		values.push_back(lhs);
	}

	void emitIntegerShift(ShiftOp operation)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i64 shift operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto value = values.back(); values.pop_back();
		auto maskedCount = compiler.new_gp32("i64ShiftCount");
		checkError(compiler.mov(maskedCount, count.r32()), "AsmJit i64 shift-count copy failed");
		checkError(compiler.and_(maskedCount, 63), "AsmJit i64 shift-count mask failed");
		checkError(compiler.mov(asmjit::x86::ecx, maskedCount), "AsmJit i64 shift-count setup failed");
		switch (operation) {
		case ShiftOp::Left: checkError(compiler.shl(value, asmjit::x86::cl), "AsmJit i64.shl emission failed"); break;
		case ShiftOp::ArithmeticRight: checkError(compiler.sar(value, asmjit::x86::cl), "AsmJit i64.shr_s emission failed"); break;
		case ShiftOp::LogicalRight: checkError(compiler.shr(value, asmjit::x86::cl), "AsmJit i64.shr_u emission failed"); break;
		case ShiftOp::RotateLeft: checkError(compiler.rol(value, asmjit::x86::cl), "AsmJit i64.rotl emission failed"); break;
		case ShiftOp::RotateRight: checkError(compiler.ror(value, asmjit::x86::cl), "AsmJit i64.rotr emission failed"); break;
		}
		values.push_back(value);
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
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32 shift operand stack underflow");
		auto count = values.back(); values.pop_back();
		auto value = values.back(); values.pop_back();
		// x86 variable shifts/rotates take their count from CL. The low five
		// bits are used by WebAssembly; masking explicitly also documents the
		// semantic rule for the generated instruction sequence.
		auto maskedCount = compiler.new_gp32("shiftCount");
		checkError(compiler.mov(maskedCount, count), "AsmJit shift-count copy failed");
		checkError(compiler.and_(maskedCount, 31), "AsmJit shift-count mask failed");
		checkError(compiler.mov(asmjit::x86::ecx, maskedCount), "AsmJit shift-count setup failed");
		switch (operation) {
		case ShiftOp::Left:
			checkError(compiler.shl(value, asmjit::x86::cl), "AsmJit i32.shl emission failed"); break;
		case ShiftOp::ArithmeticRight:
			checkError(compiler.sar(value, asmjit::x86::cl), "AsmJit i32.shr_s emission failed"); break;
		case ShiftOp::LogicalRight:
			checkError(compiler.shr(value, asmjit::x86::cl), "AsmJit i32.shr_u emission failed"); break;
		case ShiftOp::RotateLeft:
			checkError(compiler.rol(value, asmjit::x86::cl), "AsmJit i32.rotl emission failed"); break;
		case ShiftOp::RotateRight:
			checkError(compiler.ror(value, asmjit::x86::cl), "AsmJit i32.rotr emission failed"); break;
		}
		values.push_back(value);
	}

	asmjit::x86::Compiler& compiler;
	const WASM::Module& module;
	WASM::ModuleInstanceInternals& internals;
	const std::vector<asmjit::FuncNode*>& functions;
	const std::vector<std::unique_ptr<asmjit::FuncSignature>>& signatures;
	asmjit::x86::Gp vmContext;
	std::vector<asmjit::x86::Gp> locals;
	//! Slot width of every local (4/8, or 16 for a v128 buffer).
	std::vector<int> localWidths;
	std::vector<asmjit::x86::Gp> values;
	std::vector<ControlFrame> controls;
	uint32_t resultCount = 0;
	WASM::ValueTypeCode resultType = WASM::ValueTypeCode::Void;
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

void ModuleCompiler::translateTypes(WASM::ModuleInstance&, const WASM::Module& module,
		WASM::ModuleInstanceInternals& internals)
{
	signatures.clear();
	signatures.resize(module.typeIds.size());
	internals.translatedTypes.assign(module.typeIds.size(), nullptr);

	for (uint32_t i = 0; i < module.typeIds.size(); ++i) {
		if (module.typeView(i).kind() != WASM::CanonTypeKind::Func)
			continue;
		const WASM::FuncType type = module.funcTypeAt(i);
		signatures[i] = std::make_unique<asmjit::FuncSignature>(
				TypeTranslator::translateFunctionType(type));
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
	if (module.functionBodies.size() != functionRecords.size())
		throw std::runtime_error("AsmJit function declaration/body count mismatch");

	const std::size_t allocationCheckpoint = context.allocationCheckpoint();
	try {
		asmjit::CodeHolder code;
		checkError(code.init(context.environment(), context.cpuFeatures()),
				"AsmJit CodeHolder initialisation failed");
		asmjit::x86::Compiler compiler(&code);

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
			auto* function = compiler.add_func(functions[i]);
			if (function == nullptr)
				throw std::runtime_error("AsmJit failed to begin an internal function body");
			const uint32_t typeIndex = module.internalFunctionTypeIndices[i];
			const WASM::FuncType type = module.funcTypeAt(typeIndex);
			IntegerFunctionEmitter emitter(compiler, *function, type, module.functionBodies[i],
					module, internals, functions, signatures);
			emitter.compile(module.functionBodies[i], type);
			checkError(compiler.end_func(), "AsmJit function finalisation failed");
		}

		checkError(compiler.finalize(), "AsmJit compiler finalisation failed");
		void* codeBase = context.install(code);
		moduleBases.emplace_back(&instance, codeBase);
		for (uint32_t i = 0; i < functionRecords.size(); ++i) {
			const uint64_t offset = code.label_offset_from_base(functions[i]->label());
			functionRecords[i]->entry = static_cast<uint8_t*>(codeBase) + offset;
			internals.internalCallables[i].rawFnPtr = functionRecords[i]->entry;
			internals.internalCallables[i].fnPtr = functionRecords[i]->entry;
			internals.compiledFunctions[i] = functionRecords[i]->entry;
		}
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
