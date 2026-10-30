#include "SljitOpcodeDispatcher.hpp"

#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmTrap.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <span>
#include <utility>

namespace Sljit {

namespace {

//! Thrown internally to abandon one function; never leaves this translation unit.
struct UnsupportedBody {
};

//! Highest number of Wasm parameters this emitter can carry (SLJIT allows four
//! arguments in total, and `VMContext*` is always the first one).
constexpr std::uint32_t kMaxParameters = 3;

//! Every frame slot is one SLJIT machine word.
constexpr std::size_t kSlotBytes = sizeof(sljit_sw);

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

//! C entry point for the pending-trap probe. Returns a machine word so the
//! comparison in generated code does not depend on how a C `int` return is
//! extended by the host compiler.
extern "C" sljit_sw sljitTrapPending()
{
	return WASM::trapPending() ? 1 : 0;
}

//! A function pointer as the integer operand SLJIT takes for a direct call.
template <typename Fn>
sljit_sw codeAddress(Fn fn)
{
	return reinterpret_cast<sljit_sw>(fn);
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

	if (parameterCount_ > kMaxParameters) {
		unsupportedReason_ = "the provisional SLJIT backend supports at most three parameters";
		return;
	}
	for (int width : localWidths_) {
		if (width == 0) {
			unsupportedReason_ = "the provisional SLJIT backend only models i32 and i64 locals";
			return;
		}
	}
	if (prepared.allReturns.size() > 1) {
		unsupportedReason_ = "the provisional SLJIT backend does not support multiple results";
		return;
	}
	if (prepared.allReturns.size() == 1) {
		resultWidth_ = widthOf(prepared.allReturns[0]);
		if (resultWidth_ == 0) {
			unsupportedReason_ = "the provisional SLJIT backend only returns i32 and i64";
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
	stackDepth_ = 0;
	returned_ = false;
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

	const std::size_t frameSlots = localSlots_ + maxStackDepth_;
	if (frameSlots > SLJIT_MAX_LOCAL_SIZE / kSlotBytes) {
		unsupportedReason_ = "the function frame exceeds SLJIT's local-area limit";
		return false;
	}

	// The argument list is `VMContext*` followed by the Wasm parameters. Without
	// the `_R` suffix the arguments arrive in S0, S1, ... and survive nested
	// helper calls, which is exactly what the prologue needs while it copies
	// them into the frame.
	sljit_s32 argTypes = resultWidth_ == 0
			? SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
			: SLJIT_ARG_RETURN(resultWidth_ == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W);
	argTypes |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
	for (std::uint32_t i = 0; i < parameterCount_; ++i) {
		argTypes |= SLJIT_ARG_VALUE(
				paramWidths_[i] == 4 ? SLJIT_ARG_TYPE_32 : SLJIT_ARG_TYPE_W, i + 2);
	}

	const sljit_s32 savedCount = static_cast<sljit_s32>(1 + parameterCount_);
	// Three scratch registers: R0/R1 carry the integer operations and R2 carries
	// the third argument of the trap-site annotation call in the epilogue.
	emit(sljit_emit_enter(compiler_, 0, argTypes, 3, savedCount,
			static_cast<sljit_s32>(frameSlots * kSlotBytes)));
	if (!ok_) {
		// A failed prologue leaves SLJIT in its error state; emitting more would
		// only trip its own argument checks.
		unsupportedReason_ = "SLJIT rejected the function prologue";
		return false;
	}

	// The guest stack guard (WasmStack.hpp): every compiled function calls it
	// before its body runs.
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0));
	emit(sljit_emit_icall(compiler_, SLJIT_CALL, SLJIT_ARGS1V(P),
			SLJIT_IMM, codeAddress(&WASM::wasm_stack_check)));

	// ... and returns a benign value once the guard has reported a trap, rather
	// than running a body whose stack is already exhausted.
	emit(sljit_emit_icall(compiler_, SLJIT_CALL, SLJIT_ARGS0(W),
			SLJIT_IMM, codeAddress(&sljitTrapPending)));
	trapJump_ = sljit_emit_cmp(compiler_, SLJIT_NOT_EQUAL, SLJIT_RETURN_REG, 0, SLJIT_IMM, 0);
	if (trapJump_ == nullptr)
		ok_ = false;

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

	// The trap path: it joins here from the prologue's pending check.
	sljit_label* trapLabel = sljit_emit_label(compiler_);
	if (trapLabel == nullptr || trapJump_ == nullptr) {
		unsupportedReason_ = "SLJIT could not place the trap epilogue";
		return false;
	}
	sljit_set_label(trapJump_, trapLabel);

	// Name the function in the diagnostic, then return a benign zero result:
	// a trap is a pending status, never a non-local jump (WasmTrap.hpp).
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
			static_cast<sljit_sw>(functionIndex_)));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0));
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
			codeAddress(functionName_)));
	emit(sljit_emit_icall(compiler_, SLJIT_CALL, SLJIT_ARGS3V(32, 32, P),
			SLJIT_IMM, codeAddress(&WASM::wasm_trap_annotate)));
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

void OpcodeDispatcher::push()
{
	++stackDepth_;
	if (stackDepth_ > maxStackDepth_)
		maxStackDepth_ = stackDepth_;
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
	require(stackDepth_ >= count, "the body consumes more operands than it produces");
}

void OpcodeDispatcher::requireLocal(std::size_t index, const char* what)
{
	if (index < localSlots_)
		return;
	unsupportedReason_ = std::string(what) + " names a local outside the function frame";
	throw UnsupportedBody{};
}

void OpcodeDispatcher::guardNotReturned(const char* what)
{
	require(!returned_, what);
}

sljit_sw OpcodeDispatcher::localSlotOffset(std::size_t index) const
{
	return static_cast<sljit_sw>(index * kSlotBytes);
}

sljit_sw OpcodeDispatcher::stackSlotOffset(std::size_t depth) const
{
	return static_cast<sljit_sw>((localSlots_ + depth) * kSlotBytes);
}

void OpcodeDispatcher::onUnhandledOpcode(const char* name)
{
	unsupportedReason_ = std::string("the provisional SLJIT backend does not lower ") + name
			+ " (docs/WRITING_A_BACKEND.md §9)";
	throw UnsupportedBody{};
}

void OpcodeDispatcher::dispatchNop()
{
	guardNotReturned("nop");
}

void OpcodeDispatcher::dispatchEnd()
{
	if (returned_)
		return;
	if (resultWidth_ != 0)
		requireStack(1);
	if (!measureOnly_)
		emitReturnValue();
	returned_ = true;
}

void OpcodeDispatcher::dispatchReturn()
{
	guardNotReturned("return");
	if (resultWidth_ != 0)
		requireStack(1);
	if (!measureOnly_)
		emitReturnValue();
	returned_ = true;
}

void OpcodeDispatcher::dispatchDrop()
{
	guardNotReturned("drop");
	requireStack(1);
	--stackDepth_;
}

void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	guardNotReturned("local.get");
	requireLocal(arg, "local.get");
	push();
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg)));
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), stackSlotOffset(stackDepth_ - 1), SLJIT_R0, 0));
}

void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	guardNotReturned("local.set");
	requireLocal(arg, "local.set");
	requireStack(1);
	--stackDepth_;
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), stackSlotOffset(stackDepth_)));
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg), SLJIT_R0, 0));
}

void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	guardNotReturned("local.tee");
	requireLocal(arg, "local.tee");
	requireStack(1);
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), stackSlotOffset(stackDepth_ - 1)));
	emit(sljit_emit_op1(compiler_, localWidths_[arg] == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_MEM1(SLJIT_SP), localSlotOffset(arg), SLJIT_R0, 0));
}

void OpcodeDispatcher::dispatchI32Const(std::int32_t arg)
{
	guardNotReturned("i32.const");
	push();
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP),
			stackSlotOffset(stackDepth_ - 1), SLJIT_IMM, static_cast<sljit_sw>(arg)));
}

void OpcodeDispatcher::dispatchI64Const(std::int64_t arg)
{
	guardNotReturned("i64.const");
	push();
	if (measureOnly_)
		return;
	emit(sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
			stackSlotOffset(stackDepth_ - 1), SLJIT_IMM, static_cast<sljit_sw>(arg)));
}

void OpcodeDispatcher::dispatchI32Add() { guardNotReturned("i32.add"); emitIntegerBinary(SLJIT_ADD, 4); }
void OpcodeDispatcher::dispatchI32Sub() { guardNotReturned("i32.sub"); emitIntegerBinary(SLJIT_SUB, 4); }
void OpcodeDispatcher::dispatchI32Mul() { guardNotReturned("i32.mul"); emitIntegerBinary(SLJIT_MUL, 4); }
void OpcodeDispatcher::dispatchI32And() { guardNotReturned("i32.and"); emitIntegerBinary(SLJIT_AND, 4); }
void OpcodeDispatcher::dispatchI32Or() { guardNotReturned("i32.or"); emitIntegerBinary(SLJIT_OR, 4); }
void OpcodeDispatcher::dispatchI32Xor() { guardNotReturned("i32.xor"); emitIntegerBinary(SLJIT_XOR, 4); }
// Wasm masks a shift count by the operand width, which is SLJIT's `M` (masked)
// shift form; the plain form leaves an out-of-range count undefined.
void OpcodeDispatcher::dispatchI32Shl() { guardNotReturned("i32.shl"); emitIntegerBinary(SLJIT_MSHL, 4); }
void OpcodeDispatcher::dispatchI32ShrS() { guardNotReturned("i32.shr_s"); emitIntegerBinary(SLJIT_MASHR, 4); }
void OpcodeDispatcher::dispatchI32ShrU() { guardNotReturned("i32.shr_u"); emitIntegerBinary(SLJIT_MLSHR, 4); }
void OpcodeDispatcher::dispatchI64Add() { guardNotReturned("i64.add"); emitIntegerBinary(SLJIT_ADD, 8); }
void OpcodeDispatcher::dispatchI64Sub() { guardNotReturned("i64.sub"); emitIntegerBinary(SLJIT_SUB, 8); }
void OpcodeDispatcher::dispatchI64Mul() { guardNotReturned("i64.mul"); emitIntegerBinary(SLJIT_MUL, 8); }
void OpcodeDispatcher::dispatchI64And() { guardNotReturned("i64.and"); emitIntegerBinary(SLJIT_AND, 8); }
void OpcodeDispatcher::dispatchI64Or() { guardNotReturned("i64.or"); emitIntegerBinary(SLJIT_OR, 8); }
void OpcodeDispatcher::dispatchI64Xor() { guardNotReturned("i64.xor"); emitIntegerBinary(SLJIT_XOR, 8); }
void OpcodeDispatcher::dispatchI64Shl() { guardNotReturned("i64.shl"); emitIntegerBinary(SLJIT_MSHL, 8); }
void OpcodeDispatcher::dispatchI64ShrS() { guardNotReturned("i64.shr_s"); emitIntegerBinary(SLJIT_MASHR, 8); }
void OpcodeDispatcher::dispatchI64ShrU() { guardNotReturned("i64.shr_u"); emitIntegerBinary(SLJIT_MLSHR, 8); }

void OpcodeDispatcher::emitIntegerBinary(sljit_s32 wordOp, int width)
{
	requireStack(2);
	--stackDepth_;
	if (measureOnly_)
		return;

	const sljit_s32 load = width == 4 ? SLJIT_MOV_U32 : SLJIT_MOV;
	const sljit_s32 store = width == 4 ? SLJIT_MOV32 : SLJIT_MOV;
	const sljit_s32 op = width == 4 ? (wordOp | SLJIT_32) : wordOp;
	const sljit_sw left = stackSlotOffset(stackDepth_ - 1);
	const sljit_sw right = stackSlotOffset(stackDepth_);
	emit(sljit_emit_op1(compiler_, load, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), left));
	emit(sljit_emit_op1(compiler_, load, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), right));
	emit(sljit_emit_op2(compiler_, op, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0));
	emit(sljit_emit_op1(compiler_, store, SLJIT_MEM1(SLJIT_SP), left, SLJIT_R0, 0));
}

void OpcodeDispatcher::emitReturnValue()
{
	if (resultWidth_ == 0) {
		emit(sljit_emit_return_void(compiler_));
		return;
	}
	// An i32 comes back zero-extended from MOV_U32, and the C ABI of the caller
	// reads only the low 32 bits of the return register. The *operation* passed
	// to `sljit_emit_return` must match the declared return width, though: SLJIT
	// asserts that a 32-bit return uses MOV32.
	emit(sljit_emit_op1(compiler_, resultWidth_ == 4 ? SLJIT_MOV_U32 : SLJIT_MOV,
			SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), stackSlotOffset(stackDepth_ - 1)));
	emit(sljit_emit_return(compiler_, resultWidth_ == 4 ? SLJIT_MOV32 : SLJIT_MOV,
			SLJIT_R0, 0));
}

} // namespace Sljit
