#include "SljitOpcodeDispatcher.hpp"

#include "SljitModuleCompiler.hpp"

#include "../WasmBase/WasmMemory.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace Sljit {

namespace {

//! Thrown internally to abandon one function; never leaves this translation unit.
struct UnsupportedBody {
};

//! Every frame slot is one SLJIT machine word.
constexpr std::size_t kSlotBytes = sizeof(sljit_sw);

//! Reserved frame slots that follow the locals and precede the operand stack.
constexpr std::size_t kReservedSlots = 3;

//! Width of a local in bytes, or 0 when the type is outside the supported subset.
int widthOf(const WASM::ValueType& type)
{
	switch (type.opcode) {
	case WASM::ValueTypeCode::I32:
		return 4;
	case WASM::ValueTypeCode::I64:
		return 8;
	default:
		return 0;
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

//! Reports the spec's `unreachable` trap without returning through a Wasm frame.
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

//! Checked address of a memory32 access. The immediate offset and the access
//! width travel packed in one word, because an SLJIT call carries at most four
//! integer arguments and `VMContext*` is one of them.
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
//! the declared value widths.
sljit_s32 callArgTypes(const WASM::FuncType& type, int resultWidth)
{
	sljit_s32 types = resultWidth == 0
			? SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
			: SLJIT_ARG_RETURN(resultWidth == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W);
	types |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
	for (std::size_t i = 0; i < type.params.size(); ++i) {
		types |= SLJIT_ARG_VALUE(
				widthOf(type.params[i].val) == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W,
				static_cast<int>(i) + 2);
	}
	return types;
}

} // namespace

OpcodeDispatcher::OpcodeDispatcher(const WASM::Module& module, const WASM::FuncType& funcType,
		const WASM::FunctionBody& body, std::uint32_t functionIndex)
	: module_(module)
	, funcType_(funcType)
	, body_(body)
	, functionIndex_(functionIndex)
{
	const std::string* name = module.findName(WASM::NameKind::Function, functionIndex);
	if (name != nullptr)
		functionName_ = name->c_str();

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
	for (std::uint32_t i = 0; i < parameterCount_; ++i)
		paramWidths_.push_back(widthOf(prepared.allLocals[i]));

	localWidths_.reserve(localSlots_);
	for (const WASM::ValueType& local : prepared.allLocals)
		localWidths_.push_back(widthOf(local));

	if (parameterCount_ > kMaxWasmParameters) {
		unsupportedReason_ = "the SLJIT backend supports at most three parameters";
		return;
	}
	for (int width : localWidths_) {
		if (width == 0) {
			unsupportedReason_ = "the SLJIT backend only models i32 and i64 locals";
			return;
		}
	}
	if (prepared.allReturns.size() > 1) {
		unsupportedReason_ = "the SLJIT backend does not support multiple results";
		return;
	}
	if (prepared.allReturns.size() == 1) {
		resultWidth_ = widthOf(prepared.allReturns[0]);
		if (resultWidth_ == 0) {
			unsupportedReason_ = "the SLJIT backend only returns i32 and i64";
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
	return ok_;
}

bool OpcodeDispatcher::beginFunction()
{
	compiler_ = sljit_create_compiler(nullptr);
	if (compiler_ == nullptr) {
		unsupportedReason_ = "SLJIT could not create a compiler";
		return false;
	}

	const std::size_t frameSlots = localSlots_ + kReservedSlots + maxStackDepth_;
	if (frameSlots > SLJIT_MAX_LOCAL_SIZE / kSlotBytes) {
		unsupportedReason_ = "the function frame exceeds SLJIT's local-area limit";
		return false;
	}

	// The argument list is `VMContext*` followed by the Wasm parameters. Without
	// the `_R` suffix the arguments arrive in S0, S1, ... and survive nested
	// helper calls.
	sljit_s32 argTypes = resultWidth_ == 0
			? SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
			: SLJIT_ARG_RETURN(resultWidth_ == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W);
	argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
	for (std::uint32_t i = 0; i < parameterCount_; ++i) {
		argTypes |= SLJIT_ARG_VALUE(
				paramWidths_[i] == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W, i + 2);
	}

	// Four scratch registers: R0-R3 carry a helper call with up to four integer
	// arguments, which is the widest call this backend emits.
	const sljit_s32 savedCount = static_cast<sljit_s32>(1 + parameterCount_);
	emit(sljit_emit_enter(compiler_, 0, argTypes, 4, savedCount,
			static_cast<sljit_s32>(frameSlots * kSlotBytes)));
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

	// The guest stack guard (WasmStack.hpp): every compiled function calls it
	// before its body runs.
	loadContext(SLJIT_R0);
	emitHelper(SLJIT_ARGS1V(P), &WASM::wasm_stack_check);
	emitPendingTrapCheck();

	for (std::uint32_t i = 0; i < parameterCount_; ++i) {
		emit(sljit_emit_op1(compiler_, paramWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_S(1 + i), 0));
	}
	// Declared locals start at zero, as the spec requires.
	for (std::size_t i = parameterCount_; i < localSlots_; ++i) {
		emit(sljit_emit_op1(compiler_, localWidths_[i] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
				SLJIT_MEM1(SLJIT_SP), localSlotOffset(i), SLJIT_IMM, 0));
	}
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
	if (resultWidth_ == 0) {
		emit(sljit_emit_return_void(compiler_));
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
	if (resultWidth_ == 0) {
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	requireStack(1);
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
	emitHelper(SLJIT_ARGS0(W), &sljitTrapPending);
	sljit_jump* jump = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL,
			SLJIT_R0, 0, SLJIT_IMM, 0);
	if (jump == nullptr) {
		ok_ = false;
		return;
	}
	trapJumps_.push_back(jump);
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
		require(type.valType == WASM::ValueTypeCode::I32
						|| type.valType == WASM::ValueTypeCode::I64,
				"the SLJIT backend only models i32 and i64 block types");
		signature.results.push_back(type.valType == WASM::ValueTypeCode::I32 ? 4 : 8);
		return signature;
	}
	require(type.typeIndex >= 0
					&& static_cast<std::uint32_t>(type.typeIndex) < module_.typeCount(),
			"the block type index is out of range");
	const WASM::FuncType blockType = module_.funcTypeAt(static_cast<std::uint32_t>(type.typeIndex));
	for (const WASM::StorageType& parameter : blockType.params) {
		const int width = widthOf(parameter.val);
		require(width != 0, "the SLJIT backend only models i32 and i64 block parameters");
		signature.params.push_back(width);
	}
	for (const WASM::StorageType& result : blockType.results) {
		const int width = widthOf(result.val);
		require(width != 0, "the SLJIT backend only models i32 and i64 block results");
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
			else if (resultWidth_ != 0) {
				emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0));
				emit(sljit_emit_return(compiler_,
						resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV, SLJIT_R0, 0));
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
		endReachable = (frame.entryReachable && reachable_) || frame.endTargeted;
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

	const std::size_t base = stackWidths_.size() - type.params.size();
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R2, 0));
	for (std::size_t i = 0; i < type.params.size(); ++i)
		loadSlot(SLJIT_R(static_cast<int>(i) + 1), base + i,
				widthOf(type.params[i].val));

	const int resultWidth = type.results.empty() ? 0 : widthOf(type.results.front().val);
	emit(sljit_emit_icall(compiler_, SLJIT_CALL, callArgTypes(type, resultWidth),
			SLJIT_MEM1(SLJIT_SP), targetSlotOffset()));
	if (resultWidth != 0)
		storeSlot(SLJIT_R0, base, resultWidth);
	emitPendingTrapCheck();
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
	emitCall(arg, true);
	if (!measureOnly_)
		emitReturnValue();
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
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
		loadSlot(SLJIT_R1, stackWidths_.size() - 3, valueWidth);
		loadSlot(SLJIT_R2, stackWidths_.size() - 2, valueWidth);
		loadSlot(SLJIT_R0, stackWidths_.size() - 1, 4);
		emit(sljit_emit_op2u(compiler_, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0,
				SLJIT_IMM, 0));
		emit(sljit_emit_select(compiler_,
				SLJIT_NOT_EQUAL | (valueWidth == 4 ? SLJIT_32 : 0),
				SLJIT_R1, SLJIT_R1, 0, SLJIT_R2));
		storeSlot(SLJIT_R1, stackWidths_.size() - 3, valueWidth);
	}
	stackWidths_.resize(stackWidths_.size() - 2);
}

void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& types)
{
	require(types.size() == 1, "select_t requires exactly one value type");
	require(widthOf(types.front()) != 0, "select_t only supports i32 and i64 values");
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
			"the SLJIT backend only models i32 and i64 globals");
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
	const WASM::MemoryType& type = module_.memories[index];
	require(type.indexType == WASM::MemoryIndexType::I32,
			"the SLJIT backend only lowers memory32");
	return type;
}

void OpcodeDispatcher::emitMemoryLoad(const WASM::MemArg& arg, std::uint32_t width,
		sljit_s32 loadOp, int resultWidth)
{
	if (!reachable_)
		return;
	(void)requireMemoryIndex(arg.memidx);
	require(!arg.isArray, "the SLJIT backend does not lower GC-array access");
	require(arg.offset <= UINT32_MAX, "the memory32 offset exceeds the encoded u32 range");
	requireStack(1);
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg.memidx)));
		loadSlot(SLJIT_R2, stackWidths_.size() - 1, 4);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM,
				static_cast<sljit_sw>((static_cast<std::uint64_t>(width) << 32)
						| arg.offset)));
		emitHelper(SLJIT_ARGS4(P, P, 32, 32, W), &sljitMemoryAddress);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset(), SLJIT_R0, 0));
		emitPendingTrapCheck();
		emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
				scratchSlotOffset()));
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
	(void)requireMemoryIndex(arg.memidx);
	require(!arg.isArray, "the SLJIT backend does not lower GC-array access");
	require(arg.offset <= UINT32_MAX, "the memory32 offset exceeds the encoded u32 range");
	requireStack(2);
	pop();
	pop();
	if (measureOnly_)
		return;
	const std::size_t addressDepth = stackWidths_.size();
	const std::size_t valueDepth = stackWidths_.size() + 1;
	loadContext(SLJIT_R0);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
			static_cast<sljit_sw>(arg.memidx)));
	loadSlot(SLJIT_R2, addressDepth, 4);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM,
			static_cast<sljit_sw>((static_cast<std::uint64_t>(width) << 32)
					| arg.offset)));
	emitHelper(SLJIT_ARGS4(P, P, 32, 32, W), &sljitMemoryAddress);
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset(), SLJIT_R0, 0));
	emitPendingTrapCheck();
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP),
			scratchSlotOffset()));
	loadSlot(SLJIT_R1, valueDepth, static_cast<int>(width));
	emit(sljit_emit_op1(compiler_, storeOp, SLJIT_MEM1(SLJIT_R0), 0, SLJIT_R1, 0));
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
	(void)requireMemoryIndex(arg);
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		emitHelper(SLJIT_ARGS2(32, P, 32), &sljitMemorySize);
		storeSlot(SLJIT_R0, stackWidths_.size(), 4);
		emitPendingTrapCheck();
	}
	push(4);
}

void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	if (!reachable_)
		return;
	(void)requireMemoryIndex(arg);
	requireStack(1);
	pop();
	if (!measureOnly_) {
		loadContext(SLJIT_R0);
		emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
				static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, stackWidths_.size(), 4);
		emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitMemoryGrow);
		storeSlot(SLJIT_R0, stackWidths_.size(), 4);
		emitPendingTrapCheck();
	}
	push(4);
}

void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2)
{
	if (!reachable_)
		return;
	(void)requireMemoryIndex(arg1);
	(void)requireMemoryIndex(arg2);
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
		emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitMemoryCopy);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 3);
}

void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg)
{
	if (!reachable_)
		return;
	(void)requireMemoryIndex(arg);
	requireStack(3);
	if (!measureOnly_) {
		const std::size_t base = stackWidths_.size() - 3;
		loadContext(SLJIT_R0);
		loadSlot(SLJIT_R1, base + 1, 4);
		emit(sljit_emit_op2(compiler_, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0,
				SLJIT_IMM, 32));
		emit(sljit_emit_op2(compiler_, SLJIT_OR, SLJIT_R1, 0, SLJIT_R1, 0,
				SLJIT_IMM, static_cast<sljit_sw>(arg)));
		loadSlot(SLJIT_R2, base, 4);
		loadSlot(SLJIT_R3, base + 2, 4);
		emitHelper(SLJIT_ARGS4V(P, W, 32, 32), &sljitMemoryFill);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 3);
}

void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2)
{
	if (!reachable_)
		return;
	(void)requireMemoryIndex(arg2);
	require(arg1 < module_.dataSegments.size(), "the data segment index is out of range");
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
		emitHelper(SLJIT_ARGS4V(P, W, W, 32), &sljitMemoryInit);
		emitPendingTrapCheck();
	}
	stackWidths_.resize(stackWidths_.size() - 3);
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

















